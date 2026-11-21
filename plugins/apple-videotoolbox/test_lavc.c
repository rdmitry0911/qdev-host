/*
 * test_lavc.c - conformance tests for the decode-order stream decode core
 * (vtpv_lavc). Plan U1 (apple-virgl analysis/20261121-APPLEGVA-RESUME-PLAN.md).
 *
 * Streams are generated at test time (libx264 / libx265 through libavcodec,
 * muxed to MP4 so the decoder gets avcC/hvcC + length-prefixed samples exactly
 * as the guest's VideoToolbox sends them) and never committed.
 *
 * Oracle (independent of vtpv_lavc): plain libavcodec SOFTWARE decode through
 * its normal display-order output, each frame tagged by the decode index of
 * its access unit (pts), converted to packed cropped NV12 by this file's own
 * code; MD5 per decode index. Under test: vtpv_lavc_decode() on the GPU (and
 * on the software path), one unit at a time, which must return THAT unit's
 * picture (decode order, Apple host contract) with the oracle's MD5.
 *
 * Exit 77 = skipped (no render node).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/md5.h>
#include <libavutil/opt.h>

#include "vtpv_lavc.h"

static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#include "test_lavc_util.h"

static void frame_md5(const vtpv_lavc_frame *f, uint8_t md5[16])
{
    struct AVMD5 *m = av_md5_alloc();
    size_t bpp = f->pixfmt == VTPV_PIXFMT_P010 ? 2 : 1, r;
    av_md5_init(m);
    for (r = 0; r < f->height; r++) av_md5_update(m, f->plane[0] + f->stride[0] * r, f->width * bpp);
    for (r = 0; r < (f->height + 1) / 2; r++)
        av_md5_update(m, f->plane[1] + f->stride[1] * r, ((f->width + 1) / 2) * 2 * bpp);
    av_md5_final(m, md5);
    av_free(m);
}

static uint32_t codec_of(const stream *s) { return s->id == AV_CODEC_ID_HEVC ? VTPV_CODEC_HEVC : VTPV_CODEC_H264; }

/* decode-order check of one decoder (hw or sw) against the oracle */
static void check_decode_order(vtpv_lavc *v, const char *name, const char *arm, const stream *s,
                               const oracle *o)
{
    vtpv_lavc_dec *d = NULL;
    vtpv_lavc_frame f;
    uint8_t md5[16];
    int i, rc, ok = 0, first_bad = -1;
    rc = vtpv_lavc_open_decoder(v, codec_of(s), (uint32_t)s->w, (uint32_t)s->h,
                                s->extradata, s->extradata_len, &d);
    CHECK(rc == VTPV_OK, "%s/%s: open rc %d", name, arm, rc);
    if (rc != VTPV_OK) return;
    for (i = 0; i < s->n; i++) {
        rc = vtpv_lavc_decode(d, s->pkt[i], s->pkt_len[i], &f);
        if (rc != VTPV_OK) { if (first_bad < 0) first_bad = i; printf("  %s/%s: unit %d rc %d\n", name, arm, i, rc); continue; }
        frame_md5(&f, md5);
        if (f.width == (uint32_t)o->w && f.height == (uint32_t)o->h && f.pixfmt == VTPV_PIXFMT_NV12 &&
            o->have[i] && !memcmp(md5, o->md5[i], 16)) ok++;
        else if (first_bad < 0) first_bad = i;
    }
    CHECK(ok == s->n, "%s/%s: %d/%d units match the oracle in decode order (first bad %d)", name, arm, ok, s->n, first_bad);
    printf("  %s/%s: %d/%d units == oracle (decode order, %dx%d)\n", name, arm, ok, s->n, o->w, o->h);
    vtpv_lavc_close_decoder(d);
}

static void run_case(vtpv_lavc *hw, vtpv_lavc *sw, const char *name, const char *enc,
                     const char *params, enum AVPixelFormat pf, int w, int h, int n)
{
    stream s;
    static oracle o;
    int rc = make_stream(enc, params, pf, w, h, n, name, &s);
    if (rc == 77) { printf("  %s: SKIP (no %s)\n", name, enc); return; }
    CHECK(rc == 0, "%s: stream generation failed", name);
    if (rc) return;
    CHECK(run_oracle(&s, &o) == 0 && o.n_out == n, "%s: oracle returned %d frames", name, o.n_out);
    check_decode_order(hw, name, "gpu", &s, &o);
    check_decode_order(sw, name, "sw", &s, &o);
    stream_free(&s);
}

int main(int argc, char **argv)
{
    const char *node = argc > 1 ? argv[1] : "/dev/dri/renderD128";
    vtpv_lavc *hw = NULL, *sw = NULL;
    char err[256] = "";
    int rc, i;
    uint8_t capbuf[sizeof(vtpv_caps) + 4 * sizeof(vtpv_codec_cap)];
    vtpv_caps *caps = (vtpv_caps *)capbuf;

    av_log_set_level(AV_LOG_QUIET);
    rc = vtpv_lavc_open(VTPV_LAVC_HW_VAAPI, node, &hw, err, sizeof(err));
    if (rc != VTPV_OK) { printf("SKIP: %s\n", err); return 77; }
    vtpv_lavc_open(VTPV_LAVC_HW_NONE, NULL, &sw, NULL, 0);

    /* LAVC-CAPS-01: hardware caps list only what VA-API decodes (VLD); H.264
     * clamped to 4080 rows (radeonsi 4096x4096 hazard). */
    rc = vtpv_lavc_query_caps(hw, caps, sizeof(capbuf));
    CHECK(rc >= 1, "caps: %d codecs", rc);
    for (i = 0; i < rc; i++) {
        vtpv_codec_cap *c = &((vtpv_codec_cap *)(caps + 1))[i];
        printf("  caps: %c%c%c%c decode=%u max %ux%u\n", (char)(c->codec >> 24), (char)(c->codec >> 16),
               (char)(c->codec >> 8), (char)c->codec, c->can_decode, c->max_width, c->max_height);
        if (c->codec == VTPV_CODEC_H264) CHECK(c->max_height <= 4080 || c->max_width < 4096, "caps: H.264 clamp");
    }

    /* LAVC-H264-IPB-01..02: B-frames, multiple refs, B-pyramid; each unit's own
     * picture in decode order, references kept across units. */
    run_case(hw, sw, "h264-ipb-640x360", "libx264", "bframes=2:ref=3:keyint=12:min-keyint=12:scenecut=0",
             AV_PIX_FMT_YUV420P, 640, 360, 30);
    run_case(hw, sw, "h264-ipb-1920x1080", "libx264", "bframes=3:ref=4:keyint=24:scenecut=0:b-pyramid=normal",
             AV_PIX_FMT_YUV420P, 1920, 1080, 12);
    /* LAVC-HEVC-MAIN-01..02: HEVC Main with B-frames; 3840x2160 (aerial size). */
    run_case(hw, sw, "hevc-main-1280x720", "libx265", "bframes=3:keyint=16:min-keyint=16:scenecut=0:log-level=error",
             AV_PIX_FMT_YUV420P, 1280, 720, 24);
    run_case(hw, sw, "hevc-main-3840x2160", "libx265", "bframes=2:keyint=8:scenecut=0:log-level=error",
             AV_PIX_FMT_YUV420P, 3840, 2160, 6);

    /* LAVC-REFUSE-01: what the '420v' output contract cannot carry is refused
     * at open (guest keeps software): HEVC Main10, H.264 High 4:4:4. */
    {
        stream s;
        vtpv_lavc_dec *d = NULL;
        if (make_stream("libx265", "keyint=4:log-level=error", AV_PIX_FMT_YUV420P10LE, 320, 240, 2, "m10", &s) == 0) {
            rc = vtpv_lavc_open_decoder(hw, VTPV_CODEC_HEVC, 320, 240, s.extradata, s.extradata_len, &d);
            CHECK(rc == VTPV_ERR_UNSUPPORTED && !d, "refuse: HEVC Main10 open rc %d", rc);
            stream_free(&s);
        }
        if (make_stream("libx264", "keyint=4", AV_PIX_FMT_YUV444P, 320, 240, 2, "444", &s) == 0) {
            rc = vtpv_lavc_open_decoder(hw, VTPV_CODEC_H264, 320, 240, s.extradata, s.extradata_len, &d);
            CHECK(rc == VTPV_ERR_UNSUPPORTED && !d, "refuse: H.264 4:4:4 open rc %d", rc);
            stream_free(&s);
        }
    }

    /* LAVC-FAILCLOSED-01: a stream the GPU cannot decode (8192 wide H.264 is
     * beyond VA-API's max) fails; it is never decoded in software here. */
    {
        stream s;
        vtpv_lavc_dec *d = NULL;
        vtpv_lavc_frame f;
        if (make_stream("libx264", "keyint=2:bframes=0", AV_PIX_FMT_YUV420P, 8192, 256, 2, "wide", &s) == 0) {
            rc = vtpv_lavc_open_decoder(hw, VTPV_CODEC_H264, 8192, 256, s.extradata, s.extradata_len, &d);
            if (rc == VTPV_OK) {
                rc = vtpv_lavc_decode(d, s.pkt[0], s.pkt_len[0], &f);
                CHECK(rc < 0, "fail-closed: 8192x256 decode rc %d (want an error)", rc);
                printf("  fail-closed 8192x256: rc %d\n", rc);
                vtpv_lavc_close_decoder(d);
            } else CHECK(rc < 0, "fail-closed: open rc %d", rc);
            stream_free(&s);
        }
    }

    /* LAVC-NOPIC-01: a unit without a picture (access unit delimiter only)
     * returns NOPIC (emitted as dropped, no buffer). */
    {
        stream s;
        vtpv_lavc_dec *d = NULL;
        vtpv_lavc_frame f;
        static const uint8_t aud[] = { 0, 0, 0, 2, 0x09, 0xf0 };
        if (make_stream("libx264", "keyint=4:bframes=0", AV_PIX_FMT_YUV420P, 320, 240, 2, "nopic", &s) == 0 &&
            vtpv_lavc_open_decoder(hw, VTPV_CODEC_H264, 320, 240, s.extradata, s.extradata_len, &d) == VTPV_OK) {
            CHECK(vtpv_lavc_decode(d, s.pkt[0], s.pkt_len[0], &f) == VTPV_OK, "nopic: IDR decodes");
            rc = vtpv_lavc_decode(d, aud, sizeof(aud), &f);
            CHECK(rc == VTPV_LAVC_NOPIC, "nopic: AUD-only unit rc %d", rc);
            CHECK(vtpv_lavc_decode(d, s.pkt[1], s.pkt_len[1], &f) == VTPV_OK, "nopic: next unit decodes");
            vtpv_lavc_close_decoder(d);
        }
        if (s.n) stream_free(&s);
    }

    /* LAVC-FLUSH-01 + LAVC-ADAPTER-01: flush (invalidate) then the same
     * session decodes the stream again with identical pictures; the backend
     * adapter hands the unit's picture to read_luma/read_chroma by surface id. */
    {
        stream s;
        static oracle o;
        const vtpv_decoder_backend *be = vtpv_lavc_backend();
        void *dec = NULL;
        vtpv_lavc_dec *d;
        vtpv_decoded_frame df;
        int pass, k, ok = 0;
        if (make_stream("libx264", "bframes=2:ref=3:keyint=8:scenecut=0", AV_PIX_FMT_YUV420P, 320, 240, 16, "flush", &s) == 0 &&
            run_oracle(&s, &o) == 0 &&
            be->open_decoder(hw, VTPV_CODEC_H264, 320, 240, s.extradata, s.extradata_len, &dec) == VTPV_OK) {
            uint8_t *y = malloc(384 * 240), *uv = malloc(384 * 120);
            d = dec;
            for (pass = 0; pass < 2; pass++) {
                for (k = 0; k < s.n; k++) {
                    struct AVMD5 *m;
                    uint8_t md5[16];
                    int r;
                    if (be->decode(dec, s.pkt[k], s.pkt_len[k], &df) != VTPV_OK) continue;
                    if (vtpv_lavc_read_luma(hw, df.surface_id, y, 384, df.width, df.height) != VTPV_OK) continue;
                    if (vtpv_lavc_read_chroma(hw, df.surface_id, uv, 384, df.width, df.height) != VTPV_OK) continue;
                    m = av_md5_alloc(); av_md5_init(m);
                    for (r = 0; r < 240; r++) av_md5_update(m, y + 384 * r, 320);
                    for (r = 0; r < 120; r++) av_md5_update(m, uv + 384 * r, 320);
                    av_md5_final(m, md5); av_free(m);
                    if (!memcmp(md5, o.md5[k], 16)) ok++;
                }
                vtpv_lavc_flush(d);
            }
            CHECK(ok == 2 * s.n, "flush/adapter: %d/%d units match after flush + via read_luma/chroma", ok, 2 * s.n);
            CHECK(vtpv_lavc_read_luma(hw, 0xdeadbeefu, y, 384, 320, 240) == VTPV_ERR_BACKEND, "adapter: unknown surface id refused");
            printf("  flush/adapter: %d/%d units == oracle\n", ok, 2 * s.n);
            free(y); free(uv);
            be->close_decoder(dec);
        }
        if (s.n) stream_free(&s);
    }

    vtpv_lavc_close(hw);
    vtpv_lavc_close(sw);
    printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
