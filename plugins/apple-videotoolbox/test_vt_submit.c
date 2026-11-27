#define _GNU_SOURCE
/*
 * test_vt_submit.c - the queue-0 drain (vt_submit_process) of the apple-videotoolbox device on a
 * synthetic split ring, with the REAL session engine (aw_session) and a slow-decode backend double.
 * No libva, no GPU, no guest.
 *
 * Contract VT-Q0-RETIRE (D-AGVA-TIMEOUT-UNDER-LOAD; apple-virgl analysis/20261128-D-AGVA-TIMEOUT-UNDER-LOAD.md):
 *   Apple's queue 0 is asymmetric. Each chain is one message plus a 4-byte status tail, and the status is
 *   the result of delivering the message. Decode results travel on queue 1 (contract B section 1:
 *   AppleVideoToolboxParavirtualization/analysis/CONTRACT-B-APPLE-VT-WIRE-20261116.md). So the
 *   retire of a queue-0 chain must not wait for the deferred (decode-ahead) units of OTHER messages.
 *   Its latency is bounded by at most the one unit in progress when the chain arrives.
 *   The guest driver gives up on a queue-0 status after AGVA_SUBMIT_TIMEOUT_SECONDS (5 s) and logs
 *   `sendMessageToHost driver rc=0xe00002d6`.
 *
 * Boundary doubles (scope: this test only): the decoder backend (slow-decode double that also plays the
 * guest posting a new chain while the host decodes), the queue-0 interrupt and the queue-1 flush.
 * The ring code (vtpv_virtq), the drain (vt_submit) and the engine (aw_session) are production code.
 * CLEAN-ROOM.
 */
#include "vt_submit.h"
#include "aw_session.h"
#include "aw_sbuf.h"
#include "aw_wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } \
                           else { printf("  ok: " __VA_ARGS__); printf("\n"); } } while (0)

/* ---- synthetic guest memory + queue 0 (size 32 = 16 two-descriptor chains) ---- */
static uint8_t g_mem[1u << 16];
static void *map_mem(void *ctx, uint64_t iova, uint64_t len)
{
    (void)ctx;
    if (iova + len > sizeof(g_mem)) return NULL;
    return g_mem + iova;
}
#define QSZ    32
#define DESC   0x1000
#define AVAIL  0x1200
#define USED   0x1400
#define MSG    0x2000   /* chain i's message at MSG + i * 0x400 */
#define STAT   0x8000   /* chain i's status tail at STAT + i * 4 */
static void put16(uint64_t at, uint16_t v) { memcpy(g_mem + at, &v, 2); }
static uint16_t get16(uint64_t at) { uint16_t v; memcpy(&v, g_mem + at, 2); return v; }
static int32_t stat_of(unsigned chain) { int32_t v; memcpy(&v, g_mem + STAT + 4u * chain, 4); return v; }
static unsigned g_chains;
/* the guest posts one queue-0 chain: readable message + writable 4-byte status (poisoned) */
static unsigned post_chain(const uint8_t *m, size_t n)
{
    unsigned c = g_chains++;
    memcpy(g_mem + MSG + 0x400u * c, m, n);
    int32_t poison = 0x5a5a5a5a; memcpy(g_mem + STAT + 4u * c, &poison, 4);
    vtpv_vq_desc r = { MSG + 0x400u * c, (uint32_t)n, VTPV_VQ_DESC_F_NEXT, (uint16_t)(2 * c + 1) };
    vtpv_vq_desc w = { STAT + 4u * c, 4, VTPV_VQ_DESC_F_WRITE, 0 };
    memcpy(g_mem + DESC + 16u * (2 * c), &r, 16);
    memcpy(g_mem + DESC + 16u * (2 * c + 1), &w, 16);
    uint16_t idx = get16(AVAIL + 2);
    put16(AVAIL + 4 + 2u * (idx % QSZ), (uint16_t)(2 * c));
    put16(AVAIL + 2, (uint16_t)(idx + 1));
    return c;
}
static uint16_t used_idx(void) { return get16(USED + 2); }

/* ---- Apple-wire messages (as in test_aw_session.c) ---- */
static size_t msg0(uint8_t *buf, uint32_t type, uint32_t flags, const uint8_t *uuid) {
    aw_builder b; aw_builder_init(&b, buf, 256);
    aw_build_header(&b, type, flags, uuid, 7);
    return aw_build_finalize(&b);
}
static size_t build_dsta(uint8_t *m, const uint8_t *uuid) {
    uint8_t fds[160];
    static const uint8_t avcc[7] = { 0x01,0x42,0x00,0x1e,0xff,0xe1,0x00 };
    size_t fl = aw_imgdesc_build(fds, sizeof fds, AW_4CC('a','v','c','1'),
                                 352, 288, AW_4CC('a','v','c','C'), avcc, sizeof avcc);
    aw_builder b; aw_builder_init(&b, m, 512);
    aw_build_header(&b, AW_MSG_DSTA, AW_FLAG_GUEST_REQ, uuid, 7);
    aw_put_bytes(&b, AW_KEY_FDS, AW_VT_FDS, fds, fl);
    return aw_build_finalize(&b);
}
static size_t build_dfrm(uint8_t *m, const uint8_t *uuid, int64_t frm, uint64_t rid) {
    static const uint8_t au[8] = { 0,0,0,4, 0x65, 1,2,3 };
    uint8_t sbf[256];
    size_t sl = aw_sbuf_write(sbf, sizeof sbf, NULL, au, sizeof au);
    aw_builder b; aw_builder_init(&b, m, 512);
    aw_build_header(&b, AW_MSG_DFRM, AW_FLAG_GUEST_REQ, uuid, rid);
    aw_put_s64(&b, AW_KEY_FRM, frm);
    aw_put_bytes(&b, AW_KEY_SBF, AW_VT_SBF, sbf, sl);
    aw_put_u32(&b, AW_KEY_DFL, 1);
    return aw_build_finalize(&b);
}

/* ---- boundary doubles ---- */
static int g_decodes, g_raises, g_flushes, g_emits;
static uint16_t g_used_at_decode[8];      /* used-ring index seen at the START of decode k */
static int g_inject_at = -1;              /* decode index during which the guest posts a chain */
static int g_inject_session;              /* ...and then dsta + dfrm for that new session */
static unsigned g_injected = ~0u;
static uint16_t g_used_at_last_raise;
static void emit(void *c, const uint8_t *m, size_t n) { (void)c; (void)m; (void)n; g_emits++; }
static int be_query(void *c, vtpv_caps *o, size_t cap) {
    (void)c;
    vtpv_codec_cap *cc = (vtpv_codec_cap *)((uint8_t *)o + sizeof(*o));
    if (cap < sizeof(*o) + sizeof(*cc)) return -1;
    o->magic = VTPV_CAPS_MAGIC; o->abi = VTPV_ABI; o->n_codecs = 1;
    cc[0].codec = VTPV_CODEC_H264; cc[0].can_decode = 1; cc[0].can_encode = 0; cc[0].reserved = 0;
    cc[0].max_width = 4096; cc[0].max_height = 4096; cc[0].out_pixfmt = VTPV_PIXFMT_NV12;
    return 1;
}
static int be_open(void *c, uint32_t codec, uint32_t w, uint32_t h, const uint8_t *ex, size_t exl, void **dec) {
    (void)c; (void)codec; (void)w; (void)h; (void)ex; (void)exl; *dec = (void *)0xD0D0; return 0;
}
/* The slow decode: a real host decode takes long under GPU load; here "slow" means the guest gets to
 * post a new queue-0 chain while it runs (decode g_inject_at), which is the hazard being tested. */
static int be_decode(void *dec, const uint8_t *s, size_t sl, vtpv_decoded_frame *o) {
    (void)dec; (void)s; (void)sl;
    int k = g_decodes++;
    if (k < 8) g_used_at_decode[k] = used_idx();
    if (k == g_inject_at) {
        uint8_t m[512], u[16]; memset(u, 0xCC, 16);
        g_injected = post_chain(m, msg0(m, AW_MSG_PDEC, AW_FLAG_GUEST_REQ, u));
        if (g_inject_session) {                /* a whole new session with one unit */
            post_chain(m, build_dsta(m, u));
            post_chain(m, build_dfrm(m, u, 40, 1001));
        }
    }
    o->surface_id = 42; o->width = 352; o->height = 288; o->pixfmt = VTPV_PIXFMT_NV12;
    return 0;
}
static void be_close(void *dec) { (void)dec; }
static const vtpv_decoder_backend SLOW = { be_query, be_open, be_decode, be_close, NULL, NULL };
static int res(void *c, const uint8_t u[16], uint64_t id, uint32_t w, uint32_t h, aw_out_buffer *o) {
    (void)c; (void)u; memset(o, 0, sizeof *o); o->mapping_id = id; o->width = w; o->height = h; return 0;
}
static int cpy(void *c, const vtpv_decoded_frame *f, const aw_out_buffer *b) { (void)c; (void)f; (void)b; return 0; }
static void put(void *c, const aw_out_buffer *b) { (void)c; (void)b; }
static void raise_q0(void *ctx) { (void)ctx; g_raises++; g_used_at_last_raise = used_idx(); }
static void flush_q1(void *ctx) { (void)ctx; g_flushes++; }

static void reset(void)
{
    memset(g_mem, 0, sizeof g_mem); g_chains = 0;
    g_decodes = g_raises = g_flushes = g_emits = 0; g_inject_at = -1; g_injected = ~0u;
    memset(g_used_at_decode, 0xff, sizeof g_used_at_decode); g_used_at_last_raise = 0;
}

/* two decode sessions, each with one deferred unit after the initial drain (PDEC, dsta, dfrm each) */
static int run(int inject_at, vtpv_virtq *q0)
{
    static uint8_t buf[1u << 16];
    aw_session_cfg cfg; memset(&cfg, 0, sizeof cfg);
    cfg.be = &SLOW; cfg.emit = emit; cfg.resolve_buffer = res; cfg.copy_frame = cpy; cfg.put_buffer = put;
    aw_engine *e = aw_engine_create(&cfg);
    if (!e) return -2;
    reset(); g_inject_at = inject_at;
    memset(q0, 0, sizeof *q0);
    q0->size = QSZ; q0->ready = 1; q0->desc = DESC; q0->avail = AVAIL; q0->used = USED;
    uint8_t m[512], a[16], b[16]; memset(a, 0xA1, 16); memset(b, 0xB2, 16);
    post_chain(m, msg0(m, AW_MSG_PDEC, AW_FLAG_GUEST_REQ, a));
    post_chain(m, build_dsta(m, a));
    post_chain(m, build_dfrm(m, a, 40, 801));
    post_chain(m, msg0(m, AW_MSG_PDEC, AW_FLAG_GUEST_REQ, b));
    post_chain(m, build_dsta(m, b));
    post_chain(m, build_dfrm(m, b, 40, 901));
    const vt_submit_io io = { .sq = q0, .map = map_mem, .map_ctx = NULL, .engine = e,
                              .buf = buf, .buf_cap = sizeof buf, .ctx = NULL,
                              .raise = raise_q0, .flush = flush_q1, .rx = NULL, .served = NULL };
    int rc = vt_submit_process(&io, 2 * 256, 4);   /* the device's bounds */
    aw_engine_destroy(e);
    return rc;
}

int main(void)
{
    vtpv_virtq q0;

    printf("-- VT-Q0-RETIRE control: no chain arrives during the decodes --\n");
    {
        int rc = run(-1, &q0);
        CHECK(rc == 0, "VT-Q0-00: drain rc 0 (got %d)", rc);
        CHECK(g_decodes == 2, "VT-Q0-00: both deferred units decoded ahead (%d)", g_decodes);
        CHECK(g_used_at_decode[0] == 6, "VT-Q0-00: the 6 initial chains are retired before the first decode (%u)",
              (unsigned)g_used_at_decode[0]);
        CHECK(used_idx() == 6 && g_used_at_last_raise == 6, "VT-Q0-00: 6 retired, the last raise covers them");
        int st_ok = 1; for (unsigned c = 0; c < 6; c++) st_ok &= stat_of(c) == AW_NOERR;
        CHECK(st_ok, "VT-Q0-00: every status tail holds the delivery status 0");
    }

    printf("-- VT-Q0-RETIRE: a chain posted while the first deferred unit decodes --\n");
    {
        int rc = run(0, &q0);
        CHECK(rc == 0, "VT-Q0-01: drain rc 0 (got %d)", rc);
        CHECK(g_decodes == 2, "VT-Q0-02: both deferred units decoded (%d)", g_decodes);
        CHECK(g_injected == 6, "VT-Q0-02: the guest posted chain 6 during decode 0");
        /* the contract: chain 6 is retired, with its status, BEFORE the next deferred unit decodes */
        CHECK(g_used_at_decode[1] == 7,
              "VT-Q0-03: chain 6 retired before deferred decode 1 starts (used idx at decode 1 = %u, want 7)",
              (unsigned)g_used_at_decode[1]);
        CHECK(stat_of(6) == AW_NOERR, "VT-Q0-04: chain 6's status tail holds its delivery status 0 (got %#x)",
              (unsigned)stat_of(6));
        CHECK(used_idx() == 7 && g_used_at_last_raise == 7,
              "VT-Q0-05: all 7 chains retired and the guest was interrupted after chain 6 (raise saw %u)",
              (unsigned)g_used_at_last_raise);
    }

    printf("-- VT-Q0-RETIRE: P1 decode-ahead keeps every unit when the retired chains open a new one --\n");
    {
        g_inject_session = 1;
        int rc = run(0, &q0);
        g_inject_session = 0;
        CHECK(rc == 0, "VT-Q0-06: drain rc 0 (got %d)", rc);
        CHECK(g_decodes == 3, "VT-Q0-06: the new session's unit is decoded ahead too (%d decodes, want 3)", g_decodes);
        CHECK(g_used_at_decode[1] == 9,
              "VT-Q0-06: the 3 chains posted during decode 0 are retired before decode 1 (used idx %u, want 9)",
              (unsigned)g_used_at_decode[1]);
        int st_ok = 1; for (unsigned c = 0; c < 9; c++) st_ok &= stat_of(c) == AW_NOERR;
        CHECK(used_idx() == 9 && st_ok, "VT-Q0-06: all 9 chains retired with delivery status 0");
    }

    printf("%s: queue-0 drain (VT-Q0-RETIRE)\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
