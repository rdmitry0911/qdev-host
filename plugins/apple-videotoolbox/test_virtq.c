/*
 * test_virtq.c - unit test for the virtio split-virtqueue mechanics, driving a
 * full submission -> protocol-handler -> completion round-trip over a SYNTHETIC
 * ring in a flat buffer (no guest VM). CLEAN-ROOM. Increment 2, lane L-M2V03.
 */
#include "vtpv_virtq.h"
#include "vtpv_proto.h"

#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* ---- synthetic guest memory ---------------------------------------- */

static uint8_t g_mem[1u << 16];
static void *map_mem(void *ctx, uint64_t iova, uint64_t len)
{
    (void)ctx;
    if (iova + len > sizeof(g_mem)) return NULL;
    return g_mem + iova;
}

/* compact ring layout (queue size 8) */
#define QSZ        8
#define SUB_DESC   0x1000
#define SUB_AVAIL  0x1200
#define SUB_USED   0x1400
#define SUB_MSG    0x1600
#define CMP_DESC   0x2000
#define CMP_AVAIL  0x2200
#define CMP_USED   0x2400
#define CMP_BUF    0x2600

static void put16(uint64_t at, uint16_t v) { memcpy(g_mem + at, &v, 2); }
static uint16_t get16(uint64_t at) { uint16_t v; memcpy(&v, g_mem + at, 2); return v; }
static void put_desc(uint64_t table, uint16_t i, uint64_t addr, uint32_t len, uint16_t flags, uint16_t next)
{
    vtpv_vq_desc d = { addr, len, flags, next };
    memcpy(g_mem + table + (uint64_t)16 * i, &d, sizeof(d));
}

/* ---- a mock decoder backend (caps only) ---------------------------- */
static int mock_caps(void *ctx, vtpv_caps *out, size_t cap)
{
    vtpv_codec_cap *c = (vtpv_codec_cap *)((uint8_t *)out + sizeof(*out));
    (void)ctx;
    if (cap < sizeof(*out) + sizeof(*c)) return -1;
    out->magic = VTPV_CAPS_MAGIC; out->abi = VTPV_ABI; out->n_codecs = 1;
    c[0].codec = VTPV_CODEC_H264; c[0].can_decode = 1; c[0].can_encode = 0;
    c[0].reserved = 0; c[0].max_width = 4096; c[0].max_height = 4096; c[0].out_pixfmt = VTPV_PIXFMT_NV12;
    return 1;
}
static const vtpv_decoder_backend mock_be = { mock_caps, NULL, NULL, NULL };

/* ---- the emit sink: place a completion on the completion vq --------- */
typedef struct { vtpv_virtq *cmp; int emits; uint16_t last_head; uint32_t last_written; } emit_ctx;
static void sink(void *c, const uint8_t *cmpl, size_t len)
{
    emit_ctx *e = c;
    uint16_t head = 0xffff; uint32_t written = 0;
    int rc = vtpv_virtq_put_writable(e->cmp, map_mem, NULL, cmpl, len, &head, &written);
    if (rc == 1) {
        vtpv_virtq_retire(e->cmp, map_mem, NULL, head, written);
        e->emits++; e->last_head = head; e->last_written = (uint32_t)len;
    }
}

int main(void)
{
    vtpv_virtq sub = {0}, cmp = {0};
    emit_ctx ec;
    vtpv_proto *p;
    uint8_t msg[128];
    size_t mlen;

    printf("apple-videotoolbox: virtio virtqueue unit test\n");
    memset(g_mem, 0, sizeof(g_mem));

    /* submission queue: one readable chain holding a QUERY_CAPS message. */
    sub.size = QSZ; sub.ready = 1; sub.desc = SUB_DESC; sub.avail = SUB_AVAIL; sub.used = SUB_USED;
    {
        vtpv_submit_hdr *h = (vtpv_submit_hdr *)(g_mem + SUB_MSG);
        memset(h, 0, sizeof(*h));
        h->magic = VTPV_SUBMIT_MAGIC; h->abi = VTPV_ABI; h->type = VTPV_MSG_QUERY_CAPS;
        h->tag = 0xABCD; h->length = sizeof(*h);
        put_desc(SUB_DESC, 0, SUB_MSG, sizeof(*h), 0 /* readable */, 0);
        put16(SUB_AVAIL + 2, 1);   /* avail.idx = 1 */
        put16(SUB_AVAIL + 4, 0);   /* avail.ring[0] = desc 0 */
    }

    /* completion queue: one pre-posted writable buffer. */
    cmp.size = QSZ; cmp.ready = 1; cmp.desc = CMP_DESC; cmp.avail = CMP_AVAIL; cmp.used = CMP_USED;
    put_desc(CMP_DESC, 0, CMP_BUF, 512, VTPV_VQ_DESC_F_WRITE, 0);
    put16(CMP_AVAIL + 2, 1);       /* avail.idx = 1 */
    put16(CMP_AVAIL + 4, 0);       /* avail.ring[0] = desc 0 */

    ec.cmp = &cmp; ec.emits = 0; ec.last_head = 0; ec.last_written = 0;
    p = vtpv_proto_create(&mock_be, NULL, sink, &ec);
    CHECK(p != NULL, "proto_create");

    /* 1. has_avail reports the posted submission. */
    CHECK(vtpv_virtq_has_avail(&sub, map_mem, NULL) == 1, "submission has an available chain");

    /* 2. pop the readable chain -> the message bytes. */
    {
        uint16_t head = 0xffff;
        int rc = vtpv_virtq_get_readable(&sub, map_mem, NULL, msg, sizeof(msg), &mlen, &head);
        CHECK(rc == 1 && head == 0, "get_readable popped head 0 (rc=%d head=%u)", rc, head);
        CHECK(mlen == sizeof(vtpv_submit_hdr), "gathered the submission header (%zu)", mlen);
    }

    /* 3. hand to the protocol handler -> it emits a completion onto the cmp vq. */
    {
        int rc = vtpv_proto_submit(p, msg, mlen);
        CHECK(rc == 0 && ec.emits == 1, "handler emitted one completion (rc=%d emits=%d)", rc, ec.emits);
    }

    /* 4. retire the submission chain on its used ring. */
    CHECK(vtpv_virtq_retire(&sub, map_mem, NULL, 0, 0) == 0, "retire submission chain");
    CHECK(get16(SUB_USED + 2) == 1, "submission used.idx advanced to 1 (%u)", get16(SUB_USED + 2));
    { vtpv_vq_used_elem e; memcpy(&e, g_mem + SUB_USED + 4, 8);
      CHECK(e.id == 0 && e.len == 0, "submission used[0] = {0, 0}"); }

    /* 5. the completion landed in the guest buffer, with the right shape. */
    CHECK(get16(CMP_USED + 2) == 1, "completion used.idx advanced to 1 (%u)", get16(CMP_USED + 2));
    {
        vtpv_vq_used_elem e; memcpy(&e, g_mem + CMP_USED + 4, 8);
        const vtpv_cmpl_hdr *h = (const vtpv_cmpl_hdr *)(g_mem + CMP_BUF);
        const vtpv_caps *caps = (const vtpv_caps *)(g_mem + CMP_BUF + sizeof(*h));
        CHECK(e.id == 0, "completion used[0].id = 0");
        CHECK(e.len == ec.last_written && e.len >= sizeof(vtpv_cmpl_hdr), "completion used[0].len = %u", e.len);
        CHECK(h->magic == VTPV_CMPL_MAGIC && h->type == VTPV_MSG_QUERY_CAPS && h->status == VTPV_OK &&
              h->tag == 0xABCD, "completion header: magic/type/status/tag");
        CHECK(caps->magic == VTPV_CAPS_MAGIC && caps->n_codecs == 1, "completion caps: 1 codec");
    }

    /* 6. no more available on either queue (all consumed). */
    CHECK(vtpv_virtq_has_avail(&sub, map_mem, NULL) == 0, "submission drained");
    CHECK(vtpv_virtq_has_avail(&cmp, map_mem, NULL) == 0, "completion buffers consumed");

    /* 7. a malformed chain (cyclic next) is refused, not followed. */
    {
        vtpv_virtq bad = {0};
        bad.size = QSZ; bad.ready = 1; bad.desc = SUB_DESC; bad.avail = SUB_AVAIL; bad.used = SUB_USED;
        bad.last_avail = 0;
        put_desc(SUB_DESC, 0, SUB_MSG, 8, VTPV_VQ_DESC_F_NEXT, 1);
        put_desc(SUB_DESC, 1, SUB_MSG, 8, VTPV_VQ_DESC_F_NEXT, 0); /* 1 -> 0 -> 1 ... cycle */
        put16(SUB_AVAIL + 2, 1);
        uint16_t head; size_t l;
        int rc = vtpv_virtq_get_readable(&bad, map_mem, NULL, msg, sizeof(msg), &l, &head);
        CHECK(rc < 0, "cyclic descriptor chain refused (rc=%d)", rc);
    }

    /* 8. H7: queue-0 asymmetric pop_inout — one chain = a readable message +
     * a writable 4-byte status tail. Gather the message, write the i32 status
     * into the tail, retire. (Fresh ring at 0x3000 to avoid the above state.) */
    {
#define Q0_DESC  0x3000
#define Q0_AVAIL 0x3200
#define Q0_USED  0x3400
#define Q0_MSG   0x3600
#define Q0_STAT  0x3700
        vtpv_virtq q0 = {0};
        q0.size = QSZ; q0.ready = 1; q0.desc = Q0_DESC; q0.avail = Q0_AVAIL; q0.used = Q0_USED;
        memcpy(g_mem + Q0_MSG, "APPLEMSG", 8);              /* a stand-in message   */
        put_desc(Q0_DESC, 0, Q0_MSG, 8, VTPV_VQ_DESC_F_NEXT, 1);        /* readable */
        put_desc(Q0_DESC, 1, Q0_STAT, 4, VTPV_VQ_DESC_F_WRITE, 0);      /* status tail */
        put16(Q0_AVAIL + 2, 1); put16(Q0_AVAIL + 4, 0);

        uint8_t rbuf[64]; size_t rlen = 0; uint16_t head = 0xffff;
        uint8_t *wptr = NULL; uint32_t wcap = 0;
        int rc = vtpv_virtq_pop_inout(&q0, map_mem, NULL, rbuf, sizeof rbuf,
                                      &rlen, &wptr, &wcap, &head);
        CHECK(rc == 1 && head == 0, "pop_inout popped head 0 (rc=%d)", rc);
        CHECK(rlen == 8 && memcmp(rbuf, "APPLEMSG", 8) == 0, "gathered the readable message");
        CHECK(wptr == g_mem + Q0_STAT && wcap == 4, "exposed the 4-byte writable status tail");

        int32_t status = -19094;                 /* a representative device status  */
        memcpy(wptr, &status, 4);                 /* native i32, as the device writes */
        vtpv_virtq_retire(&q0, map_mem, NULL, head, 4);
        int32_t back; memcpy(&back, g_mem + Q0_STAT, 4);
        CHECK(back == -19094, "status i32 written to the tail (got %d)", back);
        vtpv_vq_used_elem ue; memcpy(&ue, g_mem + Q0_USED + 4, 8);
        CHECK(get16(Q0_USED + 2) == 1 && ue.id == 0 && ue.len == 4, "retired {head 0, len 4}");
    }

    vtpv_proto_destroy(p);
    printf("%s: virtqueue mechanics (submission->handler->completion round-trip)\n",
           g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
