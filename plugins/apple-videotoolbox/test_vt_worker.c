#define _GNU_SOURCE
/*
 * test_vt_worker.c - VT-DECODE-WORKER (D-AGVA-TIMEOUT-UNDER-LOAD; apple-virgl
 * analysis/20261128-FIX-CANDIDATE-VT-DECODE-WORKER.md): the queue-0 drain (vt_submit) on a synthetic split ring,
 * with the REAL session engine (aw_session) (the fix adds the REAL decode worker, vt_worker, and its backend lock),
 * and boundary doubles for the decoder backend (slow decode, unit ids recorded), the landing (copy_frame, with a
 * latch that holds it), the lent-buffer provider, the queue-1 sink and the device lock (a mutex standing for d->m).
 * No libva, no GPU, no guest.
 *
 * Contract VT-Q0-STATUS (contract B section 1, AppleVideoToolboxParavirtualization/analysis/CONTRACT-B-APPLE-VT-WIRE-
 * 20261116.md): a queue-0 chain is one message + a 4-byte status, the status is the delivery result; decode results
 * travel on queue 1. Engine source: a Dbuf reply's status is AW_NOERR on every branch. So no queue-0 status may wait
 * for a decode or a landing (the guest gives up after 5 s: `sendMessageToHost driver rc=0xe00002d6`).
 * Build (red step, before the Makefile target exists): cc -O2 -g -Wall -Wextra -std=c11 -pthread -o test-vt-worker \
 *   aw_wire.c aw_sbuf.c aw_bplist.c aw_session.c vt_scatter.c vt_land.c vt_submit.c vtpv_virtq.c vtpv_proto.c test_vt_worker.c
 * RED STEP (before the fix): the device's only configuration is SYNC (no worker yet); W-01 checks the contract
 * against it and fails. The fix commit adds the worker configuration and W-02..W-08.
 * Each scenario later runs in two configurations: SYNC = the engine without the worker (the behaviour before the fix,
 * `worker=0`), WORKER = the device default after the fix. W-01/W-03/W-06 hold the contract; W-02/W-08 require the
 * WORKER per-session queue-1 byte stream and decode order to equal SYNC's; W-04/W-05/W-07 the lock rules.
 * CLEAN-ROOM.
 */
#include "vt_submit.h"
#include "aw_session.h"
#include "aw_sbuf.h"
#include "aw_wire.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } \
                           else { printf("  ok: " __VA_ARGS__); printf("\n"); } } while (0)

/* ---- the device lock, guest memory lock, worker ---------------------------------------------- */
static pthread_mutex_t g_dm = PTHREAD_MUTEX_INITIALIZER;   /* stands for d->m */
static int g_worker;                                       /* configuration of the current run */
static aw_engine *g_e;

/* ---- synthetic guest memory + queue 0 (32 = 16 two-descriptor chains) ------------------------ */
static uint8_t g_mem[1u << 17];
static void *map_mem(void *ctx, uint64_t iova, uint64_t len)
{ (void)ctx; return (iova + len > sizeof g_mem) ? NULL : g_mem + iova; }
#define QSZ   32
#define DESC  0x1000
#define AVAIL 0x1200
#define USED  0x1400
#define MSG   0x2000   /* chain i's message at MSG + i * 0x400 (16 chains max per run) */
#define STAT  0x8000
static void put16(uint64_t at, uint16_t v) { memcpy(g_mem + at, &v, 2); }
static uint16_t get16(uint64_t at) { uint16_t v; memcpy(&v, g_mem + at, 2); return v; }
static unsigned g_chains;
static vtpv_virtq g_q0;
/* the guest posts one chain (the device lock is held: the drain may run on another thread) */
static unsigned post_chain(const uint8_t *m, size_t n)
{
    pthread_mutex_lock(&g_dm);
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
    pthread_mutex_unlock(&g_dm);
    return c;
}
static uint16_t used_idx_locked(void) { return get16(USED + 2); }
static uint16_t used_idx(void) { pthread_mutex_lock(&g_dm); uint16_t v = get16(USED + 2); pthread_mutex_unlock(&g_dm); return v; }
static int32_t stat_of(unsigned c)
{ pthread_mutex_lock(&g_dm); int32_t v; memcpy(&v, g_mem + STAT + 4u * c, 4); pthread_mutex_unlock(&g_dm); return v; }

/* ---- Apple-wire messages (as in test_aw_session.c) ------------------------------------------- */
static size_t msg0(uint8_t *buf, uint32_t type, uint32_t flags, const uint8_t *uuid)
{ aw_builder b; aw_builder_init(&b, buf, 256); aw_build_header(&b, type, flags, uuid, 7); return aw_build_finalize(&b); }
static size_t build_dsta(uint8_t *m, const uint8_t *uuid)
{
    uint8_t fds[160];
    static const uint8_t avcc[7] = { 0x01,0x42,0x00,0x1e,0xff,0xe1,0x00 };
    size_t fl = aw_imgdesc_build(fds, sizeof fds, AW_4CC('a','v','c','1'), 352, 288, AW_4CC('a','v','c','C'), avcc, sizeof avcc);
    aw_builder b; aw_builder_init(&b, m, 512);
    aw_build_header(&b, AW_MSG_DSTA, AW_FLAG_GUEST_REQ, uuid, 7);
    aw_put_bytes(&b, AW_KEY_FDS, AW_VT_FDS, fds, fl);
    return aw_build_finalize(&b);
}
/* unit id in au[5]; dfl 1 = async (early dfrm reply), 0 = sync */
static size_t build_dfrm(uint8_t *m, const uint8_t *uuid, int64_t frm, uint64_t rid, uint8_t unit, int async)
{
    uint8_t au[8] = { 0,0,0,4, 0x65, unit, 2, 3 };
    uint8_t sbf[256];
    size_t sl = aw_sbuf_write(sbf, sizeof sbf, NULL, au, sizeof au);
    aw_builder b; aw_builder_init(&b, m, 512);
    aw_build_header(&b, AW_MSG_DFRM, AW_FLAG_GUEST_REQ, uuid, rid);
    aw_put_s64(&b, AW_KEY_FRM, frm);
    aw_put_bytes(&b, AW_KEY_SBF, AW_VT_SBF, sbf, sl);
    aw_put_u32(&b, AW_KEY_DFL, async ? 1u : 0u);
    return aw_build_finalize(&b);
}
static size_t build_dbuf_reply(uint8_t *m, const uint8_t *uuid, uint64_t mapid)
{
    uint64_t ids[AW_SUBS_IDS] = { mapid, 0, 0, 0, 0 };
    size_t sl = aw_subs_write(m, 256, ids);
    uint8_t bu[16]; memset(bu, 0xB0, 16);
    aw_builder b; aw_builder_init(&b, m + sl, 256 - sl);
    aw_build_header(&b, AW_MSG_DBUF, AW_FLAG_GUEST_REPLY, uuid, 100);
    aw_put_s32(&b, AW_KEY_ERR, 0);
    aw_put_uui(&b, AW_KEY_UUI, bu);
    return sl + aw_build_finalize(&b);
}
__attribute__((unused)) static size_t build_demt_reply(uint8_t *m, const uint8_t *uuid)
{
    aw_builder b; aw_builder_init(&b, m, 256);
    aw_build_header(&b, AW_MSG_DEMT, AW_FLAG_GUEST_REPLY, uuid, 101);
    aw_put_s32(&b, AW_KEY_ERR, 0);
    return aw_build_finalize(&b);
}

/* ---- boundary doubles ------------------------------------------------------------------------- */
static int g_in_be, g_overlap;                 /* backend single entry (atomic)               */
static void be_enter(void) { if (__atomic_add_fetch(&g_in_be, 1, __ATOMIC_SEQ_CST) > 1) __atomic_add_fetch(&g_overlap, 1, __ATOMIC_SEQ_CST); }
static void be_leave(void) { __atomic_sub_fetch(&g_in_be, 1, __ATOMIC_SEQ_CST); }
static int g_decodes, g_decode_us, g_opens, g_closes, g_drop_at = -1, g_fail_at = -1;
static uint8_t g_order[64]; static int g_norder;  /* unit ids in decode order */
static int be_query(void *c, vtpv_caps *o, size_t cap)
{
    (void)c;
    vtpv_codec_cap *cc = (vtpv_codec_cap *)((uint8_t *)o + sizeof(*o));
    if (cap < sizeof(*o) + sizeof(*cc)) return -1;
    o->magic = VTPV_CAPS_MAGIC; o->abi = VTPV_ABI; o->n_codecs = 1;
    cc[0].codec = VTPV_CODEC_H264; cc[0].can_decode = 1; cc[0].can_encode = 0; cc[0].reserved = 0;
    cc[0].max_width = 4096; cc[0].max_height = 4096; cc[0].out_pixfmt = VTPV_PIXFMT_NV12;
    return 1;
}
static int be_open(void *c, uint32_t codec, uint32_t w, uint32_t h, const uint8_t *ex, size_t exl, void **dec)
{
    (void)c; (void)codec; (void)w; (void)h; (void)ex; (void)exl;
    be_enter(); if (g_decode_us) usleep(g_decode_us / 4); __atomic_add_fetch(&g_opens, 1, __ATOMIC_SEQ_CST); be_leave();
    *dec = (void *)0xD0D0; return 0;
}
static int be_decode(void *dec, const uint8_t *s, size_t sl, vtpv_decoded_frame *o)
{
    (void)dec;
    be_enter();
    if (g_decode_us) usleep(g_decode_us);
    int k = __atomic_fetch_add(&g_decodes, 1, __ATOMIC_SEQ_CST);
    if (g_norder < 64) g_order[g_norder++] = sl > 5 ? s[5] : 0xff;
    be_leave();
    if (k == g_drop_at) return VTPV_DROPPED;
    if (k == g_fail_at) return VTPV_ERR_BACKEND;
    o->surface_id = 42; o->width = 352; o->height = 288; o->pixfmt = VTPV_PIXFMT_NV12;
    return 0;
}
static void be_close(void *dec) { (void)dec; be_enter(); __atomic_add_fetch(&g_closes, 1, __ATOMIC_SEQ_CST); be_leave(); }
static const vtpv_decoder_backend SLOW = { be_query, be_open, be_decode, be_close, NULL, NULL };

/* the landing: holds while the latch is closed (3 s ceiling), records the used idx at its end */
static pthread_mutex_t g_lm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_lcv = PTHREAD_COND_INITIALIZER;
static int g_latch_closed, g_landing, g_landings, g_puts, g_resolve_fail;
static uint16_t g_used_at_land_end;
static int cpy(void *c, const vtpv_decoded_frame *f, const aw_out_buffer *b)
{
    (void)c; (void)f; (void)b;
    pthread_mutex_lock(&g_lm);
    g_landing = 1;
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 3;
    while (g_latch_closed) if (pthread_cond_timedwait(&g_lcv, &g_lm, &ts) == ETIMEDOUT) break;
    g_landing = 0; g_landings++;
    pthread_mutex_unlock(&g_lm);
    /* SYNC: inside the drain, which holds the device lock */
    if (!g_worker) g_used_at_land_end = used_idx_locked();
    return 0;
}
static int landing_now(void) { pthread_mutex_lock(&g_lm); int v = g_landing; pthread_mutex_unlock(&g_lm); return v; }
static void latch(int closed) { pthread_mutex_lock(&g_lm); g_latch_closed = closed; pthread_cond_broadcast(&g_lcv); pthread_mutex_unlock(&g_lm); }
static int res(void *c, const uint8_t u[16], uint64_t id, uint32_t w, uint32_t h, aw_out_buffer *o)
{ (void)c; (void)u; memset(o, 0, sizeof *o); memset(o->uuid, 0xB0, 16); o->mapping_id = id; o->width = w; o->height = h; return g_resolve_fail ? -1 : 0; }
static void put(void *c, const aw_out_buffer *b) { (void)c; (void)b; g_puts++; }

/* queue-1 sink: the byte stream (emit runs under the device lock) */
static uint8_t g_wire[96][512]; static size_t g_wirel[96]; static int g_nwire;
static void emit(void *c, const uint8_t *m, size_t n)
{ (void)c; if (g_nwire < 96 && n <= 512) { memcpy(g_wire[g_nwire], m, n); g_wirel[g_nwire] = n; } g_nwire++; }
static int g_posts;
__attribute__((unused)) static void post(void *c) { (void)c; g_posts++; }
static void raise_q0(void *c) { (void)c; }
static void flush_q1(void *c) { (void)c; }

static void reset_all(void)
{
    memset(g_mem, 0, sizeof g_mem); g_chains = 0;
    g_decodes = g_opens = g_closes = g_norder = g_landings = g_puts = g_posts = g_nwire = 0;
    g_decode_us = 0; g_drop_at = g_fail_at = -1; g_resolve_fail = 0; g_overlap = 0;
    latch(0);
    memset(&g_q0, 0, sizeof g_q0);
    g_q0.size = QSZ; g_q0.ready = 1; g_q0.desc = DESC; g_q0.avail = AVAIL; g_q0.used = USED;
}
static void start(int worker)
{
    reset_all();
    g_worker = worker;
    aw_session_cfg cfg; memset(&cfg, 0, sizeof cfg);
    cfg.emit = emit; cfg.resolve_buffer = res; cfg.put_buffer = put;
    cfg.be = &SLOW; cfg.copy_frame = cpy;
    g_e = aw_engine_create(&cfg);
}
static void finish(void)
{
    aw_engine_destroy(g_e);
    g_e = NULL;
}
/* the queue-0 notify: the device thread drains under its lock */
static void drain(void)
{
    static uint8_t buf[1u << 16];
    const vt_submit_io io = { .sq = &g_q0, .map = map_mem, .map_ctx = NULL, .engine = g_e,
                              .buf = buf, .buf_cap = sizeof buf, .ctx = NULL,
                              .raise = raise_q0, .flush = flush_q1, .rx = NULL, .served = NULL };
    pthread_mutex_lock(&g_dm);
    (void)vt_submit_process(&io, 2 * 256, 4);
    pthread_mutex_unlock(&g_dm);
}
/* the guest waits for the host's answers before its next message (causality of the replies) */
static void idle(void) { }
static int wait_landing(int ms) { for (int i = 0; i < ms; i++) { if (landing_now()) return 1; usleep(1000); } return 0; }
static int count_type(uint32_t t)
{
    int n = 0;
    for (int i = 0; i < g_nwire && i < 96; i++) { aw_msg_view v; if (aw_parse(g_wire[i], g_wirel[i], &v) == AW_PARSE_OK && v.type == t) n++; }
    return n;
}

static const uint8_t UA[16] = { 0xA1,0xA1,0xA1,0xA1,0xA1,0xA1,0xA1,0xA1,0xA1,0xA1,0xA1,0xA1,0xA1,0xA1,0xA1,0xA1 };
__attribute__((unused)) static const uint8_t UB[16] = { 0xB2,0xB2,0xB2,0xB2,0xB2,0xB2,0xB2,0xB2,0xB2,0xB2,0xB2,0xB2,0xB2,0xB2,0xB2,0xB2 };
static const uint8_t UC[16] = { 0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3 };
static uint8_t g_m[512];
#define POST(expr) post_chain(g_m, (expr))
static void open_session(const uint8_t *u, uint8_t unit)
{
    POST(msg0(g_m, AW_MSG_PDEC, AW_FLAG_GUEST_REQ, u));
    POST(build_dsta(g_m, u));
    POST(build_dfrm(g_m, u, 40, 801, unit, 1));
    drain(); idle();
}

/* W-01: a chain behind a Dbuf reply is retired while that frame still lands */
static int w01(int worker)
{
    start(worker);
    open_session(UA, 1);                                   /* chains 0-2, unit 1 decoded ahead */
    latch(1);
    POST(build_dbuf_reply(g_m, UA, 0xA100));               /* 3: the lend: decode + landing   */
    POST(msg0(g_m, AW_MSG_PDEC, AW_FLAG_GUEST_REQ, UC));   /* 4: an unrelated request          */
    drain();
    if (worker) (void)wait_landing(1000);
    int during = landing_now() && used_idx() == 5 && stat_of(4) == AW_NOERR;
    latch(0); idle();
    int ok = g_used_at_land_end == 5 && count_type(AW_MSG_DEMT) == 1;
    printf("  [%s] retired during the landing: %d; used idx at the landing's end %u (want 5)\n",
           worker ? "WORKER" : "SYNC", during, (unsigned)g_used_at_land_end);
    finish();
    (void)during;
    return ok;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("-- W-01 (VT-Q0-STATUS): a chain behind a Dbuf reply is retired while that frame lands --\n");
    CHECK(w01(0), "W-01: chain 4 (status 0) is retired while chain 3's frame still lands");
    printf("%s: VT-DECODE-WORKER (red step)\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
