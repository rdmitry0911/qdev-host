/*
 * test_aw_replay.c - replay LIVE-captured Apple-wire decode bytes through the
 * aw_session engine + live VA-API backend, exactly as the in-guest VideoToolbox
 * delivered them in the L-GVA-E2E lane:
 *
 *   dsta ,fds  -> an ImageDescription whose avcC is the REAL 23-byte record
 *                 (SPS/PPS only, length-prefixed; NO inline param sets in the AU)
 *   dfrm ,sbf  -> an sdat that is a REAL avcC-framed access unit: a 4-byte
 *                 big-endian NAL length prefix then the slice NAL (NO start codes,
 *                 NO SPS/PPS).
 *
 * This is the byte stream the known-good synthetic test (test_aw_decode.c) does
 * NOT exercise: that one puts a whole Annex-B stream (SPS+PPS+slice, start codes)
 * in the sdat. The divergence here localizes the live wire->VA-API assembly bug.
 *
 *   ./test-aw-replay <render_node> <avcc.bin> <sample.bin> <luma_ref.bin> W H
 *
 * exit 0 PASS, 1 FAIL, 77 skipped (no render node / backend). CLEAN-ROOM.
 * Lane L-GVA-DECODECONTENT, 2026-11-16.
 */
#include "aw_session.h"
#include "aw_sbuf.h"
#include "aw_wire.h"
#include "vtpv_vaapi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

#define MAXE 16
static uint8_t g_e[MAXE][1024];
static size_t  g_el[MAXE];
static int     g_ne;
static void cap_emit(void *ctx, const uint8_t *m, size_t n) {
    (void)ctx;
    if (g_ne < MAXE && n <= sizeof g_e[0]) { memcpy(g_e[g_ne], m, n); g_el[g_ne] = n; }
    g_ne++;
}

typedef struct { vtpv_vaapi *v; uint8_t *guest_ram; size_t ram_len;
                 uint32_t ystride; int copied; } bufctx;
static int resolvebuf(void *c, const uint8_t uuid[16], uint64_t mapid,
                      uint32_t w, uint32_t h, aw_out_buffer *o) {
    bufctx *b = c; (void)uuid;
    uint32_t ystride = (w + 63u) & ~63u;
    size_t ysize = (size_t)ystride * h;
    size_t csize = (size_t)ystride * ((h + 1u) / 2u);
    if (mapid + ysize + csize > b->ram_len) return -1;
    b->ystride = ystride;
    memset(o, 0, sizeof *o);
    memset(o->uuid, 0xC0, 16);
    o->mapping_id = mapid;
    o->y = b->guest_ram + mapid;  o->y_stride = ystride;
    o->uv = o->y + ysize;         o->uv_stride = ystride;
    o->width = w; o->height = h;
    return 0;
}
static int copyframe(void *c, const vtpv_decoded_frame *f, const aw_out_buffer *o) {
    bufctx *b = c;
    int rc = vtpv_vaapi_read_luma(b->v, f->surface_id, o->y, o->y_stride, f->width, f->height);
    if (rc == 0 && o->uv)
        rc = vtpv_vaapi_read_chroma(b->v, f->surface_id, o->uv, o->uv_stride, f->width, f->height);
    if (rc == 0) b->copied = 1;
    return rc;
}
static void putbuf(void *c, const aw_out_buffer *o) { (void)c; (void)o; }

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

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    uint8_t *p = malloc((size_t)n + 1);
    if (!p) { fclose(f); return NULL; }
    if (fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return p;
}

int main(int argc, char **argv) {
    if (argc < 7) {
        printf("usage: %s <render_node> <avcc.bin> <sample.bin> <luma_ref.bin> W H\n", argv[0]);
        return 1;
    }
    const char *node = argv[1];
    const int W = atoi(argv[5]), H = atoi(argv[6]);
    vtpv_vaapi *v = NULL;
    const vtpv_decoder_backend *be = NULL;
    char err[256] = {0};

    size_t avcc_len = 0, sample_len = 0, luma_len = 0;
    uint8_t *avcc = slurp(argv[2], &avcc_len);
    uint8_t *sample = slurp(argv[3], &sample_len);    /* avcC-framed AU (len prefix + slice) */
    uint8_t *luma_ref = slurp(argv[4], &luma_len);
    if (!avcc || !sample || !luma_ref) { printf("FAIL: cannot read inputs\n"); return 1; }

    printf("replay: avcc=%zuB sample=%zuB luma_ref=%zuB (%dx%d) on %s\n",
           avcc_len, sample_len, luma_len, W, H, node);
    printf("  sample[0..7] = %02x %02x %02x %02x %02x %02x %02x %02x (BE NAL length prefix + slice NAL header)\n",
           sample[0], sample[1], sample[2], sample[3], sample[4], sample[5], sample[6], sample[7]);

    if (vtpv_vaapi_open(node, &v, &be, NULL, err, sizeof err) != 0) {
        printf("SKIP: VA-API unavailable: %s\n", err); return 77;
    }

    const uint64_t GUEST_OFF = 0x3000;
    const size_t RAM = 1u << 20;
    uint8_t *guest_ram = malloc(RAM), *msg = malloc((1u << 20) + 512);
    uint8_t fds[768];
    if (!guest_ram || !msg) { printf("OOM\n"); return 1; }

    size_t fds_len = aw_imgdesc_build(fds, sizeof fds, AW_4CC('a','v','c','1'),
                                      (uint16_t)W, (uint16_t)H,
                                      AW_4CC('a','v','c','C'), avcc, avcc_len);
    CHECK(fds_len > 0, "built =fds ImageDescription carrying the live avcC (%zu bytes)", fds_len);

    memset(guest_ram, 0, RAM);
    bufctx bc = { v, guest_ram, RAM, 0, 0 };
    aw_session_cfg cfg; memset(&cfg, 0, sizeof cfg);
    cfg.be = be; cfg.be_ctx = vtpv_vaapi_ctx(v);
    cfg.emit = cap_emit;
    cfg.resolve_buffer = resolvebuf; cfg.copy_frame = copyframe; cfg.put_buffer = putbuf;
    cfg.buf_ctx = &bc;
    aw_engine *e = aw_engine_create(&cfg);

    uint8_t uuid[16]; memset(uuid, 0x5a, 16);
    aw_builder b;

    /* +dec */
    aw_builder_init(&b, msg, (1u << 20) + 512);
    aw_build_header(&b, AW_MSG_PDEC, AW_FLAG_GUEST_REQ, uuid, 10);
    aw_put_s32(&b, AW_KEY_COD, (int32_t)AW_4CC('a','v','c','1'));
    aw_put_u32(&b, AW_KEY_VER, 6);
    size_t n = aw_build_finalize(&b);
    CHECK(aw_engine_deliver(e, msg, n) == AW_NOERR, "+dec accepted");

    /* dsta (=fds carrying the live avcC) */
    g_ne = 0;
    aw_builder_init(&b, msg, (1u << 20) + 512);
    aw_build_header(&b, AW_MSG_DSTA, AW_FLAG_GUEST_REQ, uuid, 11);
    aw_put_bytes(&b, AW_KEY_FDS, AW_VT_FDS, fds, fds_len);
    n = aw_build_finalize(&b);
    int32_t st = aw_engine_deliver(e, msg, n);
    CHECK(st == AW_NOERR, "dsta accepted (status=%d)", st);

    /* dfrm with ,sbf whose sdat is the live avcC-framed AU (slice only) */
    uint8_t *sbf = malloc(sample_len + 64);
    if (!sbf) { printf("OOM\n"); return 1; }
    size_t sbf_len = aw_sbuf_write(sbf, sample_len + 64, NULL, sample, sample_len);
    CHECK(sbf_len > 0, "built sbuf{sdat=live AU} (%zu bytes)", sbf_len);
    g_ne = 0;
    aw_builder_init(&b, msg, (1u << 20) + 512);
    aw_build_header(&b, AW_MSG_DFRM, AW_FLAG_GUEST_REQ, uuid, 12);
    aw_put_s64(&b, AW_KEY_FRM, 0);
    aw_put_bytes(&b, AW_KEY_SBF, AW_VT_SBF, sbf, sbf_len);
    aw_put_u32(&b, AW_KEY_DFL, 1);
    n = aw_build_finalize(&b);
    st = aw_engine_deliver(e, msg, n);
    CHECK(st == AW_NOERR, "dfrm accepted (status=%d)", st);

    /* guest Dbuf reply -> the decode happens here */
    g_ne = 0;
    n = guest_dbuf_reply(msg, (1u << 20) + 512, uuid, GUEST_OFF);
    st = aw_engine_deliver(e, msg, n);
    CHECK(st == AW_NOERR, "Dbuf reply accepted (status=%d)", st);
    CHECK(bc.copied == 1, "decoded frame landed in the guest buffer (live AU decoded)");

    /* inspect the Demt err the engine reported for the frame */
    {
        aw_msg_view mv;
        for (int i = 0; i < g_ne && i < MAXE; i++) {
            if (aw_parse(g_e[i], g_el[i], &mv) == AW_PARSE_OK && mv.type == AW_MSG_DEMT) {
                int32_t derr = 0; aw_get_s32(&mv, AW_KEY_ERR, &derr);
                printf("  Demt err = %d\n", derr);
            }
        }
    }

    if (bc.copied) {
        const uint8_t *gy = guest_ram + GUEST_OFF;
        uint32_t ys = bc.ystride;
        size_t bad = 0, first = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (gy[(size_t)y * ys + x] != luma_ref[y * W + x]) {
                    if (!bad) first = (size_t)y * W + x;
                    bad++;
                }
        printf("  live-replay NV12 luma @stride %u: %zu/%d mismatches; pixel0 got=%u want=%u\n",
               ys, bad, W * H, gy[0], luma_ref[0]);
        if (bad) printf("  first mismatch at (%d,%d) got=%u want=%u\n",
                        (int)(first % W), (int)(first / W),
                        gy[(first / W) * ys + (first % W)], luma_ref[first]);
        CHECK(bad == 0, "live-replay decoded luma byte-exact to reference");
    }

    n = guest_demt_reply(msg, (1u << 20) + 512, uuid);
    aw_engine_deliver(e, msg, n);

    aw_engine_destroy(e);
    vtpv_vaapi_close(v);
    free(avcc); free(sample); free(luma_ref); free(guest_ram); free(msg); free(sbf);
    printf("%s: live-captured Apple-wire bytes -> NV12 (avcC param sets + avcC-framed AU)\n",
           g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
