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

/* ---- H2 real-guest-buffer seam: resolve the lent surface into (simulated)
 * guest RAM, exactly as the device's vt_resolve_buffer maps the mapping id
 * through the qdp DMA-region table. The mapping id is an offset into guest RAM;
 * the NV12 Y stride is the Dpba alignment (align(w,64)). This stands in for the
 * host-side QMetal page resolution. */
typedef struct { vtpv_vaapi *v; uint8_t *guest_ram; size_t ram_len;
                 uint32_t ystride; int copied; } bufctx;
static int resolvebuf(void *c, const uint8_t uuid[16], uint64_t mapid,
                      uint32_t w, uint32_t h, aw_out_buffer *o) {
    bufctx *b = c; (void)uuid;
    /* L-GVA-SURFACE: mapping_id 0 = a surface-less Dbuf reply (no subs box); it is
     * NEVER a valid landing (it would resolve to guest-physical 0). Mirror the
     * device's vt_resolve_buffer: refuse it so the engine drops the frame. */
    if (mapid == 0) return -1;
    uint32_t ystride = (w + 63u) & ~63u;
    size_t ysize = (size_t)ystride * h;
    size_t csize = (size_t)ystride * ((h + 1u) / 2u);
    if (mapid + ysize + csize > b->ram_len) return -1;   /* unmapped / too small */
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
    if (rc == 0 && o->uv)   /* land the interleaved NV12 chroma too (full frame) */
        rc = vtpv_vaapi_read_chroma(b->v, f->surface_id, o->uv, o->uv_stride, f->width, f->height);
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

    /* simulated guest RAM; the guest lends its output surface at GUEST_OFF */
    const uint64_t GUEST_OFF = 0x3000;
    const size_t RAM = 1u << 20;
    uint8_t *bs = malloc(1u << 20), *luma_ref = malloc((size_t)W * H);
    uint8_t *guest_ram = malloc(RAM), *msg = malloc((1u << 20) + 512);
    uint8_t avcc[512], fds[768];
    if (!bs || !luma_ref || !guest_ram || !msg) { printf("OOM\n"); return 1; }

    size_t bslen = h264_gen_ipcm_frame(W, H, bs, 1u << 20, luma_ref);
    CHECK(bslen > 0, "generated I_PCM bitstream (%zu bytes)", bslen);

    size_t avcc_len = build_avcc(bs, bslen, avcc, sizeof avcc);
    CHECK(avcc_len > 0, "built avcC from SPS/PPS (%zu bytes)", avcc_len);
    size_t fds_len = aw_imgdesc_build(fds, sizeof fds, AW_4CC('a','v','c','1'),
                                      (uint16_t)W, (uint16_t)H,
                                      AW_4CC('a','v','c','C'), avcc, avcc_len);
    CHECK(fds_len > 0, "built =fds ImageDescription (%zu bytes)", fds_len);

    memset(guest_ram, 0, RAM);
    bufctx bc = { v, guest_ram, RAM, 0, 0 };
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

    /* the (synthetic) guest replies Dbuf, lending its output surface at GUEST_OFF
     * in guest RAM (the mapping id = the surface's guest offset; H2 resolves it
     * through the DMA-region path, here the guest_ram model) */
    g_ne = 0;
    n = guest_dbuf_reply(msg, (1u << 20) + 512, uuid, GUEST_OFF);
    st = aw_engine_deliver(e, msg, n);
    CHECK(st == AW_NOERR, "Dbuf reply accepted (status=%d)", st);
    CHECK(bc.copied == 1, "decoded frame landed in the guest buffer on the Dbuf reply");
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

    /* THE PROOF: the decoded luma landed in GUEST RAM at the lent surface's
     * offset, byte-exact, at the Dpba-aligned stride. */
    if (g_fail == 0) {
        const uint8_t *gy = guest_ram + GUEST_OFF;   /* the lent Y plane          */
        uint32_t ys = bc.ystride;                    /* align(W,64)               */
        size_t bad = 0, first = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (gy[(size_t)y * ys + x] != luma_ref[y * W + x]) {
                    if (!bad) first = (size_t)y * W + x;
                    bad++;
                }
        printf("  Apple-wire -> guest NV12 luma @off %#llx stride %u: %zu/%d mismatches; pixel0 got=%u want=%u\n",
               (unsigned long long)GUEST_OFF, ys, bad, W * H, gy[0], luma_ref[0]);
        if (bad) printf("  first mismatch at (%d,%d) got=%u want=%u\n",
                        (int)(first % W), (int)(first / W),
                        gy[(first / W) * ys + (first % W)], luma_ref[first]);
        CHECK(bad == 0, "decoded luma landed in the guest buffer, byte-exact");
    }

    /* ---- NEGATIVE GATE (L-GVA-SURFACE): a surface-less Dbuf reply (mapping_id 0)
     * MUST be refused, never silently landed at guest-physical 0. This is the
     * exact live failure: the host "resolved" gpa 0, DMA'd the decode nowhere,
     * and the guest delivered its own never-written output IOSurface = stale RAM
     * aliasing the input bitstream (the misleading "0.5% luma residual"). Gate it
     * so that class of disconnected delivery can never again pass as success:
     * resolve must fail, no frame is landed (copied==0), and NO Demt (decode-
     * complete) is emitted - the engine drops the frame after only the early
     * dfrm reply. */
    {
        uint8_t uuid0[16]; memset(uuid0, 0x3c, 16);

        aw_builder_init(&b, msg, (1u << 20) + 512);
        aw_build_header(&b, AW_MSG_PDEC, AW_FLAG_GUEST_REQ, uuid0, 30);
        aw_put_s32(&b, AW_KEY_COD, (int32_t)AW_4CC('a','v','c','1'));
        aw_put_u32(&b, AW_KEY_VER, 6);
        n = aw_build_finalize(&b);
        CHECK(aw_engine_deliver(e, msg, n) == AW_NOERR, "mapid0: +dec accepted");

        aw_builder_init(&b, msg, (1u << 20) + 512);
        aw_build_header(&b, AW_MSG_DSTA, AW_FLAG_GUEST_REQ, uuid0, 31);
        aw_put_bytes(&b, AW_KEY_FDS, AW_VT_FDS, fds, fds_len);
        n = aw_build_finalize(&b);
        CHECK(aw_engine_deliver(e, msg, n) == AW_NOERR, "mapid0: dsta accepted");

        aw_builder_init(&b, msg, (1u << 20) + 512);
        aw_build_header(&b, AW_MSG_DFRM, AW_FLAG_GUEST_REQ, uuid0, 32);
        aw_put_s64(&b, AW_KEY_FRM, 0);
        aw_put_bytes(&b, AW_KEY_SBF, AW_VT_SBF, sbf, sbf_len);
        aw_put_u32(&b, AW_KEY_DFL, 1);
        n = aw_build_finalize(&b);
        CHECK(aw_engine_deliver(e, msg, n) == AW_NOERR, "mapid0: dfrm accepted");

        /* Dbuf reply that lends NO surface (mapping_id 0) */
        bc.copied = 0;
        g_ne = 0;
        n = guest_dbuf_reply(msg, (1u << 20) + 512, uuid0, 0 /* no surface */);
        st = aw_engine_deliver(e, msg, n);
        CHECK(st == AW_NOERR, "mapid0: Dbuf(no-surface) reply accepted (status=%d)", st);
        CHECK(bc.copied == 0, "mapid0: NO decode landed (gpa 0 refused, not delivered)");
        {
            int saw_demt = 0;
            for (int i = 0; i < g_ne && i < MAXE; i++) {
                aw_msg_view mv;
                if (aw_parse(g_e[i], g_el[i], &mv) == AW_PARSE_OK && mv.type == AW_MSG_DEMT)
                    saw_demt = 1;
            }
            CHECK(!saw_demt, "mapid0: NO Demt emitted (frame dropped honestly, not a false success)");
        }

        /* close the session */
        aw_builder_init(&b, msg, (1u << 20) + 512);
        aw_build_header(&b, AW_MSG_MDEC, AW_FLAG_GUEST_REQ, uuid0, 33);
        n = aw_build_finalize(&b);
        aw_engine_deliver(e, msg, n);
    }

    /* ---- LIVE-WIRE FRAMING: avcC param sets + length-prefixed slice-only AU.
     * The in-guest VideoToolbox does NOT inline SPS/PPS in the access unit; it
     * sends them once in the dsta avcC and then delivers slice-only, 4-byte
     * length-prefixed (avcC/ISO) access units in dfrm. Exercise exactly that so
     * the live wire->VA-API assembly stays covered (regression for
     * L-GVA-DECODECONTENT). */
    {
        /* extract the IDR slice NAL from the Annex-B stream and reframe it as a
         * single 4-byte-length-prefixed avcC AU (no SPS/PPS, no start codes) */
        h264_nal nals[H264_MAX_NALS];
        int nn = h264_split_nals(bs, bslen, nals, H264_MAX_NALS);
        const h264_nal *slice = NULL;
        for (int i = 0; i < nn; i++) {
            int t = nals[i].data[0] & 0x1f;
            if (t == 5 || t == 1) { slice = &nals[i]; break; }
        }
        CHECK(slice != NULL, "found the IDR slice NAL to reframe as avcC");

        uint8_t uuid2[16]; memset(uuid2, 0x7b, 16);

        /* +dec */
        aw_builder_init(&b, msg, (1u << 20) + 512);
        aw_build_header(&b, AW_MSG_PDEC, AW_FLAG_GUEST_REQ, uuid2, 20);
        aw_put_s32(&b, AW_KEY_COD, (int32_t)AW_4CC('a','v','c','1'));
        aw_put_u32(&b, AW_KEY_VER, 6);
        n = aw_build_finalize(&b);
        CHECK(aw_engine_deliver(e, msg, n) == AW_NOERR, "avcC: +dec accepted");

        /* dsta: the SAME fds/avcC (carries SPS/PPS) */
        aw_builder_init(&b, msg, (1u << 20) + 512);
        aw_build_header(&b, AW_MSG_DSTA, AW_FLAG_GUEST_REQ, uuid2, 21);
        aw_put_bytes(&b, AW_KEY_FDS, AW_VT_FDS, fds, fds_len);
        n = aw_build_finalize(&b);
        CHECK(aw_engine_deliver(e, msg, n) == AW_NOERR, "avcC: dsta accepted");

        /* dfrm: sdat = [4-byte BE length][slice NAL], slice only */
        uint8_t *au = malloc(slice->size + 4);
        if (!au) { printf("OOM\n"); return 1; }
        aw_put_be32(au, (uint32_t)slice->size);
        memcpy(au + 4, slice->data, slice->size);
        size_t sbf2_len = aw_sbuf_write(sbf, (1u << 20) + 64, NULL, au, slice->size + 4);
        CHECK(sbf2_len > 0, "avcC: built sbuf{sdat=length-prefixed slice} (%zu B)", sbf2_len);

        aw_builder_init(&b, msg, (1u << 20) + 512);
        aw_build_header(&b, AW_MSG_DFRM, AW_FLAG_GUEST_REQ, uuid2, 22);
        aw_put_s64(&b, AW_KEY_FRM, 0);
        aw_put_bytes(&b, AW_KEY_SBF, AW_VT_SBF, sbf, sbf2_len);
        aw_put_u32(&b, AW_KEY_DFL, 1);
        n = aw_build_finalize(&b);
        CHECK(aw_engine_deliver(e, msg, n) == AW_NOERR, "avcC: dfrm accepted");

        /* Dbuf reply -> decode into a fresh guest offset */
        const uint64_t OFF2 = 0x40000;
        bc.copied = 0;
        n = guest_dbuf_reply(msg, (1u << 20) + 512, uuid2, OFF2);
        aw_engine_deliver(e, msg, n);
        CHECK(bc.copied == 1, "avcC: live-framed AU decoded into the guest buffer");
        if (bc.copied) {
            const uint8_t *gy = guest_ram + OFF2;
            uint32_t ys = bc.ystride;
            size_t bad = 0;
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++)
                    if (gy[(size_t)y * ys + x] != luma_ref[y * W + x]) bad++;
            printf("  avcC live-framing -> NV12 luma: %zu/%d mismatches\n", bad, W * H);
            CHECK(bad == 0, "avcC live-framed decode byte-exact to reference");
        }
        /* Demt + close */
        n = guest_demt_reply(msg, (1u << 20) + 512, uuid2);
        aw_engine_deliver(e, msg, n);
        aw_builder_init(&b, msg, (1u << 20) + 512);
        aw_build_header(&b, AW_MSG_MDEC, AW_FLAG_GUEST_REQ, uuid2, 23);
        n = aw_build_finalize(&b);
        aw_engine_deliver(e, msg, n);
        free(au);
    }

    /* -dec (close) */
    aw_builder_init(&b, msg, (1u << 20) + 512);
    aw_build_header(&b, AW_MSG_MDEC, AW_FLAG_GUEST_REQ, uuid, 13);
    n = aw_build_finalize(&b);
    aw_engine_deliver(e, msg, n);
    CHECK(aw_engine_open_sessions(e) == 0, "-dec closed the session");

    aw_engine_destroy(e);
    vtpv_vaapi_close(v);
    free(bs); free(luma_ref); free(guest_ram); free(msg); free(sbf);
    printf("%s: Apple-wire bytes in -> NV12 out (real contract-B framing + live VA-API)\n",
           g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
