/*
 * test_aw_decode.c - THE HOST MILESTONE: "Apple-wire bytes in -> NV12 out".
 *
 * Feeds a REAL Apple contract-B wire stream (+dec / dsta with a =fds carrying an
 * avcC / dfrm with a ,sbf sample buffer whose sdat is a real H.264 I-frame)
 * through the aw_session engine driving the live VA-API H.264 backend, and
 * verifies the decoded luma is byte-exact to the pixels that were encoded - plus
 * that the host emitted the correct Dpba/Dbuf/Demt/rels wire sequence.
 *
 * exit 0 PASS, 1 FAIL, 77 skipped (no render node / backend). CLEAN-ROOM.
 * Lane L-VIDEO-IMPL, 2026-11-16.
 */
#include "aw_session.h"
#include "aw_sbuf.h"
#include "aw_wire.h"
#include "vtpv_vaapi.h"
#include "vtpv_h264.h"
#include "h264_gen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* ---- captured queue-1 emissions ------------------------------------- */
#define MAXE 16
static uint8_t g_e[MAXE][1024];
static size_t  g_el[MAXE];
static int     g_ne;
static void cap_emit(void *ctx, const uint8_t *m, size_t n) {
    (void)ctx;
    if (g_ne < MAXE && n <= sizeof g_e[0]) { memcpy(g_e[g_ne], m, n); g_el[g_ne] = n; }
    g_ne++;
}

/* ---- the guest buffer seam, backed by a host NV12 luma buffer ------- */
typedef struct { vtpv_vaapi *v; uint8_t *luma; uint32_t w, h; int copied; } bufctx;
static int resolvebuf(void *c, const uint8_t uuid[16], uint64_t mapid,
                      uint32_t w, uint32_t h, aw_out_buffer *o) {
    bufctx *b = c; (void)uuid;
    memset(o, 0, sizeof *o);
    memset(o->uuid, 0xC0, 16);
    o->mapping_id = mapid;
    o->y = b->luma; o->y_stride = w; o->width = w; o->height = h;
    return 0;
}
static int copyframe(void *c, const vtpv_decoded_frame *f, const aw_out_buffer *o) {
    bufctx *b = c;
    int rc = vtpv_vaapi_read_luma(b->v, f->surface_id, o->y, o->y_stride, f->width, f->height);
    if (rc == 0) b->copied = 1;
    return rc;
}
static void putbuf(void *c, const aw_out_buffer *o) { (void)c; (void)o; }

/* synthetic guest: a Dbuf reply (subs prefix with the mapping id + ,uui) */
static size_t guest_dbuf_reply(uint8_t *m, size_t cap, const uint8_t *uuid, uint64_t mapid) {
    uint64_t ids[AW_SUBS_IDS] = { mapid, 0, 0, 0, 0 };
    size_t sl = aw_subs_write(m, cap, ids);
    uint8_t bu[16]; memset(bu, 0xC0, 16);
    aw_builder b; aw_builder_init(&b, m + sl, cap - sl);
    aw_build_header(&b, AW_MSG_DBUF, AW_FLAG_GUEST_REPLY, uuid, 100);
    aw_put_s32(&b, AW_KEY_ERR, 0);
    aw_put_uui(&b, AW_KEY_UUI, bu);
    return sl + aw_build_finalize(&b);
}
static size_t guest_demt_reply(uint8_t *m, size_t cap, const uint8_t *uuid) {
    aw_builder b; aw_builder_init(&b, m, cap);
    aw_build_header(&b, AW_MSG_DEMT, AW_FLAG_GUEST_REPLY, uuid, 101);
    aw_put_s32(&b, AW_KEY_ERR, 0);
    return aw_build_finalize(&b);
}

/* build an avcC record from the SPS and PPS NALs in an Annex-B stream */
static size_t build_avcc(const uint8_t *bs, size_t bslen, uint8_t *out, size_t cap) {
    h264_nal nals[H264_MAX_NALS];
    int n = h264_split_nals(bs, bslen, nals, H264_MAX_NALS);
    const h264_nal *sps = NULL, *pps = NULL;
    for (int i = 0; i < n; i++) {
        int t = nals[i].data[0] & 0x1f;
        if (t == 7 && !sps) sps = &nals[i];
        if (t == 8 && !pps) pps = &nals[i];
    }
    if (!sps || !pps) return 0;
    size_t need = 6 + 2 + sps->size + 1 + 2 + pps->size;
    if (cap < need) return 0;
    uint8_t *p = out;
    *p++ = 1;                                   /* configurationVersion          */
    *p++ = sps->data[1];                        /* AVCProfileIndication          */
    *p++ = sps->data[2];                        /* profile_compatibility         */
    *p++ = sps->data[3];                        /* AVCLevelIndication            */
    *p++ = 0xff;                                /* reserved | lengthSizeMinusOne=3*/
    *p++ = 0xe1;                                /* reserved | numOfSPS = 1        */
    aw_put_be16(p, (uint16_t)sps->size); p += 2;
    memcpy(p, sps->data, sps->size); p += sps->size;
    *p++ = 1;                                   /* numOfPPS = 1                   */
    aw_put_be16(p, (uint16_t)pps->size); p += 2;
    memcpy(p, pps->data, pps->size); p += pps->size;
    return (size_t)(p - out);
}

int main(int argc, char **argv) {
    const char *node = (argc > 1) ? argv[1] : "/dev/dri/renderD128";
    const int W = 352, H = 288;
    vtpv_vaapi *v = NULL;
    const vtpv_decoder_backend *be = NULL;
    char err[256] = {0};

    printf("apple-videotoolbox: Apple-wire bytes -> NV12 milestone on %s (%dx%d)\n", node, W, H);

    if (vtpv_vaapi_open(node, &v, &be, NULL, err, sizeof err) != 0) {
        printf("SKIP: VA-API unavailable: %s\n", err); return 77;
    }

    uint8_t *bs = malloc(1u << 20), *luma_ref = malloc((size_t)W * H);
    uint8_t *luma_out = malloc((size_t)W * H), *msg = malloc((1u << 20) + 512);
    uint8_t avcc[512], fds[768];
    if (!bs || !luma_ref || !luma_out || !msg) { printf("OOM\n"); return 1; }

    size_t bslen = h264_gen_ipcm_frame(W, H, bs, 1u << 20, luma_ref);
    CHECK(bslen > 0, "generated I_PCM bitstream (%zu bytes)", bslen);

    size_t avcc_len = build_avcc(bs, bslen, avcc, sizeof avcc);
    CHECK(avcc_len > 0, "built avcC from SPS/PPS (%zu bytes)", avcc_len);
    size_t fds_len = aw_imgdesc_build(fds, sizeof fds, AW_4CC('a','v','c','1'),
                                      (uint16_t)W, (uint16_t)H,
                                      AW_4CC('a','v','c','C'), avcc, avcc_len);
    CHECK(fds_len > 0, "built =fds ImageDescription (%zu bytes)", fds_len);

    memset(luma_out, 0, (size_t)W * H);
    bufctx bc = { v, luma_out, (uint32_t)W, (uint32_t)H, 0 };
    aw_session_cfg cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.be = be; cfg.be_ctx = vtpv_vaapi_ctx(v);
    cfg.emit = cap_emit;
    cfg.resolve_buffer = resolvebuf; cfg.copy_frame = copyframe; cfg.put_buffer = putbuf;
    cfg.buf_ctx = &bc;
    aw_engine *e = aw_engine_create(&cfg);

    uint8_t uuid[16]; memset(uuid, 0x5a, 16);
    aw_builder b;

    /* +dec (open the session for this UUID) */
    aw_builder_init(&b, msg, (1u << 20) + 512);
    aw_build_header(&b, AW_MSG_PDEC, AW_FLAG_GUEST_REQ, uuid, 10);
    aw_put_s32(&b, AW_KEY_COD, (int32_t)AW_4CC('a','v','c','1'));
    aw_put_u32(&b, AW_KEY_VER, 6);
    size_t n = aw_build_finalize(&b);
    CHECK(aw_engine_deliver(e, msg, n) == AW_NOERR, "+dec accepted");

    /* dsta (=fds carrying the avcC) -> Dpba then the reply */
    g_ne = 0;
    aw_builder_init(&b, msg, (1u << 20) + 512);
    aw_build_header(&b, AW_MSG_DSTA, AW_FLAG_GUEST_REQ, uuid, 11);
    aw_put_bytes(&b, AW_KEY_FDS, AW_VT_FDS, fds, fds_len);
    n = aw_build_finalize(&b);
    int32_t st = aw_engine_deliver(e, msg, n);
    CHECK(st == AW_NOERR, "dsta accepted (status=%d)", st);
    {
        aw_msg_view mv;
        CHECK(g_ne == 2, "dsta emitted Dpba + reply (%d)", g_ne);
        CHECK(aw_parse(g_e[0], g_el[0], &mv) == AW_PARSE_OK && mv.type == AW_MSG_DPBA
              && mv.flags == AW_FLAG_HOST_REQ, "Dpba (0x2) first");
    }

    /* dfrm (async) with ,sbf whose sdat is the whole Annex-B I-frame */
    uint8_t *sbf = malloc((1u << 20) + 64);
    if (!sbf) { printf("OOM\n"); return 1; }
    size_t sbf_len = aw_sbuf_write(sbf, (1u << 20) + 64, NULL, bs, bslen);
    CHECK(sbf_len > 0, "built sbuf{sdat} (%zu bytes)", sbf_len);
    g_ne = 0;
    aw_builder_init(&b, msg, (1u << 20) + 512);
    aw_build_header(&b, AW_MSG_DFRM, AW_FLAG_GUEST_REQ, uuid, 12);
    aw_put_s64(&b, AW_KEY_FRM, 0);
    aw_put_bytes(&b, AW_KEY_SBF, AW_VT_SBF, sbf, sbf_len);
    aw_put_u32(&b, AW_KEY_DFL, 1); /* async-only */
    n = aw_build_finalize(&b);
    st = aw_engine_deliver(e, msg, n);
    CHECK(st == AW_NOERR, "dfrm accepted (status=%d)", st);
    /* event-driven: dfrm emits Dbuf and parks (no decode yet) */
    {
        aw_msg_view mv;
        CHECK(g_ne == 1 && aw_parse(g_e[0], g_el[0], &mv) == AW_PARSE_OK
              && mv.type == AW_MSG_DBUF && mv.flags == AW_FLAG_HOST_REQ, "dfrm emits Dbuf, parks");
        CHECK(bc.copied == 0, "no decode before the Dbuf reply");
    }

    /* the (synthetic) guest replies Dbuf, lending a buffer (mapping id 0x4200) */
    g_ne = 0;
    n = guest_dbuf_reply(msg, (1u << 20) + 512, uuid, 0x4200);
    st = aw_engine_deliver(e, msg, n);
    CHECK(st == AW_NOERR, "Dbuf reply accepted (status=%d)", st);
    CHECK(bc.copied == 1, "decoded frame copied into the lent buffer on the Dbuf reply");
    {
        aw_msg_view mv;
        CHECK(g_ne == 2, "Dbuf reply emitted dfrm-reply + Demt (%d)", g_ne);
        CHECK(aw_parse(g_e[0], g_el[0], &mv) == AW_PARSE_OK && mv.type == AW_MSG_DFRM
              && mv.flags == AW_FLAG_HOST_REPLY, "dfrm reply early");
        CHECK(aw_parse(g_e[1], g_el[1], &mv) == AW_PARSE_OK && mv.type == AW_MSG_DEMT, "Demt");
    }

    /* the guest replies Demt -> rels names the lent surface */
    g_ne = 0;
    n = guest_demt_reply(msg, (1u << 20) + 512, uuid);
    aw_engine_deliver(e, msg, n);
    CHECK(g_ne == 1 && aw_get_be32(g_e[0] + 4) == AW_ATOM_RELS, "Demt reply -> rels");

    /* THE PROOF: the NV12 luma out equals the encoded samples, byte-exact. */
    if (g_fail == 0) {
        size_t bad = 0, first = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (luma_out[y * W + x] != luma_ref[y * W + x]) { if (!bad) first = (size_t)y * W + x; bad++; }
        printf("  Apple-wire -> NV12 luma: %zu/%d mismatches; pixel0 got=%u want=%u\n",
               bad, W * H, luma_out[0], luma_ref[0]);
        if (bad) printf("  first mismatch at (%d,%d) got=%u want=%u\n",
                        (int)(first % W), (int)(first / W), luma_out[first], luma_ref[first]);
        CHECK(bad == 0, "decoded NV12 luma matches the encoded I_PCM samples");
    }

    /* -dec (close) */
    aw_builder_init(&b, msg, (1u << 20) + 512);
    aw_build_header(&b, AW_MSG_MDEC, AW_FLAG_GUEST_REQ, uuid, 13);
    n = aw_build_finalize(&b);
    aw_engine_deliver(e, msg, n);
    CHECK(aw_engine_open_sessions(e) == 0, "-dec closed the session");

    aw_engine_destroy(e);
    vtpv_vaapi_close(v);
    free(bs); free(luma_ref); free(luma_out); free(msg); free(sbf);
    printf("%s: Apple-wire bytes in -> NV12 out (real contract-B framing + live VA-API)\n",
           g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
