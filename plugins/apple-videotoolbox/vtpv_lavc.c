#define _GNU_SOURCE
/*
 * vtpv_lavc.c - decode-order stream decode core over libavcodec + VA-API /
 * CUDA (see vtpv_lavc.h). Written from the public libavcodec/libavutil API.
 */
#include "vtpv_lavc.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <va/va.h>

#define MAX_UNIT_ALLOCS 4

struct vtpv_lavc {
    vtpv_lavc_hw       hw;
    AVBufferRef       *device;   /* NULL for VTPV_LAVC_HW_NONE               */
    enum AVPixelFormat hw_fmt;   /* AV_PIX_FMT_VAAPI / AV_PIX_FMT_CUDA       */
    uint32_t           next_id;  /* backend-local surface ids                */
    vtpv_lavc_dec     *decs;     /* open decoders (surface id lookup)        */
};

struct vtpv_lavc_dec {
    vtpv_lavc       *v;
    vtpv_lavc_dec   *next;
    uint32_t         codec;
    AVCodecContext  *ctx;
    AVPacket        *pkt;
    AVFrame         *out_frame;  /* libavcodec's display-order output (dropped) */
    AVFrame         *sw_frame;   /* hardware -> CPU transfer target            */
    AVFrame         *alloc[MAX_UNIT_ALLOCS]; /* pictures allocated for this unit */
    int              n_alloc;
    int              in_unit;
    uint8_t         *conv;       /* software-path NV12/P010 conversion buffer  */
    size_t           conv_cap;
    uint8_t         *au;         /* padded copy of the access unit             */
    size_t           au_cap;
    int64_t          seq;        /* decode index used as pts                   */
    int              nal_len_size; /* 1..4, from avcC/hvcC                     */
    uint32_t         cur_id;     /* surface id of `cur` (0 = none)             */
    vtpv_lavc_frame  cur;
};

static void seterr(char *buf, size_t len, const char *fmt, ...)
{
    va_list ap;
    if (!buf || !len) return;
    va_start(ap, fmt);
    vsnprintf(buf, len, fmt, ap);
    va_end(ap);
}

int vtpv_lavc_open(vtpv_lavc_hw hw, const char *device, vtpv_lavc **out,
                   char *errbuf, size_t errlen)
{
    vtpv_lavc *v;
    int rc;
    enum AVHWDeviceType type;

    if (!out) return VTPV_ERR_MALFORMED;
    *out = NULL;
    v = calloc(1, sizeof(*v));
    if (!v) return VTPV_ERR_BACKEND;
    v->hw = hw;
    v->hw_fmt = AV_PIX_FMT_NONE;
    v->next_id = 1;
    if (hw == VTPV_LAVC_HW_NONE) { *out = v; return VTPV_OK; }

    if (hw == VTPV_LAVC_HW_VAAPI) { type = AV_HWDEVICE_TYPE_VAAPI; v->hw_fmt = AV_PIX_FMT_VAAPI; }
    else if (hw == VTPV_LAVC_HW_CUDA) { type = AV_HWDEVICE_TYPE_CUDA; v->hw_fmt = AV_PIX_FMT_CUDA; }
    else { free(v); seterr(errbuf, errlen, "unknown hw type %d", (int)hw); return VTPV_ERR_UNSUPPORTED; }

    rc = av_hwdevice_ctx_create(&v->device, type, device, NULL, 0);
    if (rc < 0) {
        char e[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(rc, e, sizeof(e));
        seterr(errbuf, errlen, "av_hwdevice_ctx_create(%s, %s): %s",
               av_hwdevice_get_type_name(type), device ? device : "(default)", e);
        free(v);
        return VTPV_ERR_UNSUPPORTED;
    }
    *out = v;
    return VTPV_OK;
}

void vtpv_lavc_close(vtpv_lavc *v)
{
    if (!v) return;
    while (v->decs) vtpv_lavc_close_decoder(v->decs);
    av_buffer_unref(&v->device);
    free(v);
}

vtpv_lavc_hw vtpv_lavc_hw_type(const vtpv_lavc *v) { return v ? v->hw : VTPV_LAVC_HW_NONE; }

/* ---- capabilities --------------------------------------------------- */

static int va_profile_vld(VADisplay dpy, VAProfile p, uint32_t *maxw, uint32_t *maxh)
{
    int n = vaMaxNumEntrypoints(dpy), i, got = 0;
    VAEntrypoint *eps;
    VAConfigAttrib at[2];
    if (n <= 0) return 0;
    eps = calloc((size_t)n, sizeof(*eps));
    if (!eps) return 0;
    if (vaQueryConfigEntrypoints(dpy, p, eps, &n) == VA_STATUS_SUCCESS)
        for (i = 0; i < n; i++) if (eps[i] == VAEntrypointVLD) got = 1;
    free(eps);
    if (!got) return 0;
    at[0].type = VAConfigAttribMaxPictureWidth;
    at[1].type = VAConfigAttribMaxPictureHeight;
    if (vaGetConfigAttributes(dpy, p, VAEntrypointVLD, at, 2) == VA_STATUS_SUCCESS) {
        if (at[0].value != VA_ATTRIB_NOT_SUPPORTED && at[0].value) *maxw = at[0].value;
        if (at[1].value != VA_ATTRIB_NOT_SUPPORTED && at[1].value) *maxh = at[1].value;
    }
    return 1;
}

int vtpv_lavc_query_caps(vtpv_lavc *v, vtpv_caps *out, size_t out_cap)
{
    vtpv_codec_cap *c;
    int has264 = 0, has265 = 0, n = 0;
    uint32_t w264 = 4096, h264 = 4096, w265 = 8192, h265 = 8192;

    if (!v || !out || out_cap < sizeof(*out) + 2 * sizeof(vtpv_codec_cap))
        return VTPV_ERR_MALFORMED;

    if (v->hw == VTPV_LAVC_HW_VAAPI) {
        AVHWDeviceContext *hd = (AVHWDeviceContext *)v->device->data;
        AVVAAPIDeviceContext *va = hd->hwctx;
        int np = vaMaxNumProfiles(va->display), i;
        VAProfile *ps = np > 0 ? calloc((size_t)np, sizeof(*ps)) : NULL;
        if (ps && vaQueryConfigProfiles(va->display, ps, &np) == VA_STATUS_SUCCESS) {
            for (i = 0; i < np; i++) {
                switch (ps[i]) {
                case VAProfileH264ConstrainedBaseline:
                case VAProfileH264Main:
                case VAProfileH264High:
                    if (va_profile_vld(va->display, ps[i], &w264, &h264)) has264 = 1;
                    break;
                case VAProfileHEVCMain:
                    if (va_profile_vld(va->display, ps[i], &w265, &h265)) has265 = 1;
                    break;
                default: break;
                }
            }
        }
        free(ps);
        /* radeonsi decodes a 4096x4096 H.264 picture to zeros while reporting
         * success; keep H.264 at 4096x4080 (driver quirk, plan R-3). */
        if (h264 > 4080 && w264 >= 4096) h264 = 4080;
    } else {
        /* CUDA/NVDEC (Turing and newer) and the software oracle. */
        has264 = has265 = 1;
    }

    out->magic = VTPV_CAPS_MAGIC;
    out->abi = VTPV_ABI;
    c = (vtpv_codec_cap *)(out + 1);
    if (has264) {
        memset(&c[n], 0, sizeof(c[n]));
        c[n].codec = VTPV_CODEC_H264; c[n].can_decode = 1;
        c[n].max_width = w264; c[n].max_height = h264; c[n].out_pixfmt = VTPV_PIXFMT_NV12;
        n++;
    }
    if (has265) {
        memset(&c[n], 0, sizeof(c[n]));
        c[n].codec = VTPV_CODEC_HEVC; c[n].can_decode = 1;
        c[n].max_width = w265; c[n].max_height = h265; c[n].out_pixfmt = VTPV_PIXFMT_NV12;
        n++;
    }
    out->n_codecs = (uint16_t)n;
    return n;
}

/* ---- codec record checks --------------------------------------------- *
 * The output contract today is an 8-bit 4:2:0 '420v' guest pool, so streams
 * that need another output format are refused at open (the guest keeps its
 * software decoder for them) instead of being converted lossily. */
static int record_is_8bit_420(uint32_t codec, const uint8_t *x, size_t n)
{
    if (codec == VTPV_CODEC_H264) {
        /* avcC: [0]=1 version, [1]=profile_idc. Profiles that may carry more
         * than 8-bit 4:2:0 (High 10/4:2:2/4:4:4, CAVLC 4:4:4) are refused; for
         * High (100) the optional avcC tail states chroma/bit depth. */
        int prof;
        if (n < 7 || x[0] != 1) return 0;
        prof = x[1];
        if (prof == 110 || prof == 122 || prof == 244 || prof == 44) return 0;
        return 1;
    }
    if (codec == VTPV_CODEC_HEVC) {
        /* hvcC: [1] profile_space(2)|tier(1)|profile_idc(5); [16] low 2 bits
         * chroma_format_idc; [17]/[18] low 3 bits bit depth minus 8. */
        if (n < 23 || x[0] != 1) return 0;
        if ((x[16] & 3) != 1) return 0;
        if ((x[17] & 7) != 0 || (x[18] & 7) != 0) return 0;
        return 1;
    }
    return 0;
}


/* NAL length size from the codec record: avcC [4] low 2 bits + 1, hvcC [21]
 * low 2 bits + 1. */
static int record_nal_len_size(uint32_t codec, const uint8_t *x, size_t n)
{
    if (codec == VTPV_CODEC_H264 && n >= 5) return (x[4] & 3) + 1;
    if (codec == VTPV_CODEC_HEVC && n >= 22) return (x[21] & 3) + 1;
    return 4;
}

/* Does the length-prefixed access unit carry a coded picture (a VCL NAL)?
 * H.264 VCL = nal_unit_type 1..5; HEVC VCL = nal_unit_type 0..31. */
static int unit_has_vcl(uint32_t codec, int lsz, const uint8_t *p, size_t n)
{
    size_t off = 0;
    while (off + (size_t)lsz < n) {
        size_t len = 0;
        int i;
        for (i = 0; i < lsz; i++) len = (len << 8) | p[off + i];
        off += (size_t)lsz;
        if (len == 0 || len > n - off) return 0;
        if (codec == VTPV_CODEC_H264) { int t = p[off] & 0x1f; if (t >= 1 && t <= 5) return 1; }
        else { int t = (p[off] >> 1) & 0x3f; if (t <= 31) return 1; }
        off += len;
    }
    return 0;
}

/* ---- decoder -------------------------------------------------------- */

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *fmts)
{
    vtpv_lavc_dec *d = ctx->opaque;
    const enum AVPixelFormat *p;
    for (p = fmts; *p != AV_PIX_FMT_NONE; p++)
        if (*p == d->v->hw_fmt) return *p;
    return AV_PIX_FMT_NONE; /* fail closed: no software fallback            */
}

/* Every picture buffer the decoder allocates while it decodes one access unit
 * belongs to that unit; keep a reference so the unit's picture can be read
 * back right after the unit, in decode order. */
static int capture_get_buffer2(AVCodecContext *ctx, AVFrame *f, int flags)
{
    vtpv_lavc_dec *d = ctx->opaque;
    int rc = avcodec_default_get_buffer2(ctx, f, flags);
    if (rc < 0 || !d->in_unit) return rc;
    if (d->n_alloc < MAX_UNIT_ALLOCS) {
        d->alloc[d->n_alloc] = av_frame_clone(f);
        if (d->alloc[d->n_alloc]) d->n_alloc++;
    }
    return rc;
}

static void drop_allocs(vtpv_lavc_dec *d)
{
    int i;
    for (i = 0; i < d->n_alloc; i++) av_frame_free(&d->alloc[i]);
    d->n_alloc = 0;
}

int vtpv_lavc_open_decoder(vtpv_lavc *v, uint32_t codec, uint32_t width,
                           uint32_t height, const uint8_t *extradata,
                           size_t extradata_len, vtpv_lavc_dec **out)
{
    const AVCodec *c;
    vtpv_lavc_dec *d;
    enum AVCodecID id;

    if (!v || !out) return VTPV_ERR_MALFORMED;
    *out = NULL;
    if (codec == VTPV_CODEC_H264) id = AV_CODEC_ID_H264;
    else if (codec == VTPV_CODEC_HEVC) id = AV_CODEC_ID_HEVC;
    else return VTPV_ERR_UNSUPPORTED;
    if (!extradata || !extradata_len || extradata_len > VTPV_MAX_EXTRADATA_BYTES)
        return VTPV_ERR_MALFORMED;
    if (width > VTPV_MAX_DIM || height > VTPV_MAX_DIM) return VTPV_ERR_TOO_LARGE;
    if (!record_is_8bit_420(codec, extradata, extradata_len)) return VTPV_ERR_UNSUPPORTED;

    c = avcodec_find_decoder(id);
    if (!c) return VTPV_ERR_UNSUPPORTED;
    d = calloc(1, sizeof(*d));
    if (!d) return VTPV_ERR_BACKEND;
    d->v = v;
    d->codec = codec;
    d->nal_len_size = record_nal_len_size(codec, extradata, extradata_len);
    d->ctx = avcodec_alloc_context3(c);
    d->pkt = av_packet_alloc();
    d->out_frame = av_frame_alloc();
    d->sw_frame = av_frame_alloc();
    if (!d->ctx || !d->pkt || !d->out_frame || !d->sw_frame) goto fail;

    d->ctx->opaque = d;
    d->ctx->width = (int)width;
    d->ctx->height = (int)height;
    d->ctx->thread_count = 1;      /* keep unit <-> picture capture exact   */
    d->ctx->get_buffer2 = capture_get_buffer2;
    d->ctx->extra_hw_frames = MAX_UNIT_ALLOCS;
    d->ctx->extradata = av_mallocz(extradata_len + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!d->ctx->extradata) goto fail;
    memcpy(d->ctx->extradata, extradata, extradata_len);
    d->ctx->extradata_size = (int)extradata_len;
    if (v->hw != VTPV_LAVC_HW_NONE) {
        d->ctx->hw_device_ctx = av_buffer_ref(v->device);
        if (!d->ctx->hw_device_ctx) goto fail;
        d->ctx->get_format = get_hw_format;
        /* H.264 Baseline (profile_idc 66) without constraint_set1: hardware
         * decoders offer Constrained Baseline/Main only, and libavcodec then
         * declines the hwaccel unless told to try. The features that set
         * Baseline apart (FMO/ASO/redundant slices) are refused by libavcodec's
         * H.264 decoder itself, so a stream that decodes here decodes right.
         * The guest's own 352x288 test stream is such a stream. */
        if (codec == VTPV_CODEC_H264 && extradata_len > 1 && extradata[1] == 66)
            d->ctx->hwaccel_flags |= AV_HWACCEL_FLAG_ALLOW_PROFILE_MISMATCH;
    }
    if (avcodec_open2(d->ctx, c, NULL) < 0) goto fail;
    d->next = v->decs;
    v->decs = d;
    *out = d;
    return VTPV_OK;
fail:
    avcodec_free_context(&d->ctx);
    av_packet_free(&d->pkt);
    av_frame_free(&d->out_frame);
    av_frame_free(&d->sw_frame);
    free(d);
    return VTPV_ERR_BACKEND;
}

/* Software path (tests/oracle): planar YUV 4:2:0 -> NV12 / P010 in d->conv,
 * cropped to w x h. */
static int sw_to_biplanar(vtpv_lavc_dec *d, const AVFrame *f, uint32_t w, uint32_t h,
                          vtpv_lavc_frame *out)
{
    int hi = f->format == AV_PIX_FMT_YUV420P10LE;
    size_t bpp = hi ? 2 : 1, cw = (w + 1) / 2, ch = (h + 1) / 2;
    size_t ystride = w * bpp, uvstride = cw * 2 * bpp;
    size_t need = ystride * h + uvstride * ch, x, y;
    uint8_t *yp, *uvp;

    if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P && !hi)
        return VTPV_ERR_UNSUPPORTED;
    if (d->conv_cap < need) {
        uint8_t *n = realloc(d->conv, need);
        if (!n) return VTPV_ERR_BACKEND;
        d->conv = n; d->conv_cap = need;
    }
    yp = d->conv; uvp = d->conv + ystride * h;
    for (y = 0; y < h; y++) {
        const uint8_t *s = f->data[0] + (size_t)f->linesize[0] * y;
        if (!hi) memcpy(yp + ystride * y, s, w);
        else for (x = 0; x < w; x++) {
            uint16_t v = (uint16_t)(((const uint16_t *)s)[x] << 6);
            memcpy(yp + ystride * y + 2 * x, &v, 2);
        }
    }
    for (y = 0; y < ch; y++) {
        const uint8_t *u = f->data[1] + (size_t)f->linesize[1] * y;
        const uint8_t *v = f->data[2] + (size_t)f->linesize[2] * y;
        uint8_t *dst = uvp + uvstride * y;
        for (x = 0; x < cw; x++) {
            if (!hi) { dst[2 * x] = u[x]; dst[2 * x + 1] = v[x]; }
            else {
                uint16_t a = (uint16_t)(((const uint16_t *)u)[x] << 6);
                uint16_t b = (uint16_t)(((const uint16_t *)v)[x] << 6);
                memcpy(dst + 4 * x, &a, 2); memcpy(dst + 4 * x + 2, &b, 2);
            }
        }
    }
    out->width = w; out->height = h;
    out->pixfmt = hi ? VTPV_PIXFMT_P010 : VTPV_PIXFMT_NV12;
    out->plane[0] = yp; out->stride[0] = ystride;
    out->plane[1] = uvp; out->stride[1] = uvstride;
    return VTPV_OK;
}

/* Consume libavcodec's display-order output; the unit's picture is read from
 * the capture instead. */
static int drain_output(vtpv_lavc_dec *d)
{
    for (;;) {
        int rc = avcodec_receive_frame(d->ctx, d->out_frame);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return 0;
        if (rc < 0) return rc;
        av_frame_unref(d->out_frame);
    }
}

int vtpv_lavc_decode(vtpv_lavc_dec *d, const uint8_t *sample, size_t sample_len,
                     vtpv_lavc_frame *out)
{
    int rc, err = 0;
    AVFrame *pic;
    uint32_t w, h;

    if (!d || !sample || !sample_len || !out) return VTPV_ERR_MALFORMED;
    memset(out, 0, sizeof(*out));
    d->cur_id = 0;
    if (sample_len > VTPV_MAX_SAMPLE_BYTES * 16u) return VTPV_ERR_TOO_LARGE;
    if (d->au_cap < sample_len + AV_INPUT_BUFFER_PADDING_SIZE) {
        uint8_t *n = realloc(d->au, sample_len + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!n) return VTPV_ERR_BACKEND;
        d->au = n;
        d->au_cap = sample_len + AV_INPUT_BUFFER_PADDING_SIZE;
    }
    memcpy(d->au, sample, sample_len);
    memset(d->au + sample_len, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    drop_allocs(d);
    d->in_unit = 1;
    av_packet_unref(d->pkt);
    d->pkt->data = d->au;
    d->pkt->size = (int)sample_len;
    d->pkt->pts = d->seq++;
    d->pkt->dts = AV_NOPTS_VALUE;
    rc = avcodec_send_packet(d->ctx, d->pkt);
    if (rc == AVERROR(EAGAIN)) {      /* not expected: output always drained */
        if (drain_output(d) < 0) err = 1;
        rc = avcodec_send_packet(d->ctx, d->pkt);
    }
    if (rc < 0) err = 1;
    if (drain_output(d) < 0) err = 1; /* decodes the unit, drops reordered output */
    d->in_unit = 0;
    d->pkt->data = NULL; d->pkt->size = 0;

    /* A unit without a coded picture (parameter sets, SEI, delimiters) is a
     * dropped frame, whatever the decoder said about it. */
    if (!unit_has_vcl(d->codec, d->nal_len_size, sample, sample_len)) {
        drop_allocs(d);
        return VTPV_LAVC_NOPIC;
    }
    if (d->n_alloc == 0) return err ? VTPV_ERR_BACKEND : VTPV_LAVC_NOPIC;
    if (err) { drop_allocs(d); return VTPV_ERR_BACKEND; }

    /* One allocation for a well-formed unit. With concealment frames the
     * decoders allocate the generated pictures before (H.264 frame_num gaps)
     * or after (HEVC missing references) the unit's own picture. */
    pic = d->codec == VTPV_CODEC_H264 ? d->alloc[d->n_alloc - 1] : d->alloc[0];
    w = (uint32_t)d->ctx->width;      /* cropped display size from the SPS    */
    h = (uint32_t)d->ctx->height;

    if (d->v->hw == VTPV_LAVC_HW_NONE) {
        rc = sw_to_biplanar(d, pic, w, h, out);
    } else {
        av_frame_unref(d->sw_frame);
        if (pic->format != d->v->hw_fmt || av_hwframe_transfer_data(d->sw_frame, pic, 0) < 0) {
            rc = VTPV_ERR_BACKEND;
        } else if (d->sw_frame->format == AV_PIX_FMT_NV12 || d->sw_frame->format == AV_PIX_FMT_P010LE) {
            out->width = w; out->height = h;
            out->pixfmt = d->sw_frame->format == AV_PIX_FMT_NV12 ? VTPV_PIXFMT_NV12 : VTPV_PIXFMT_P010;
            out->plane[0] = d->sw_frame->data[0]; out->stride[0] = (size_t)d->sw_frame->linesize[0];
            out->plane[1] = d->sw_frame->data[1]; out->stride[1] = (size_t)d->sw_frame->linesize[1];
            rc = (w <= (uint32_t)d->sw_frame->width && h <= (uint32_t)d->sw_frame->height)
                 ? VTPV_OK : VTPV_ERR_BACKEND;
        } else {
            rc = VTPV_ERR_UNSUPPORTED;
        }
    }
    drop_allocs(d);
    if (rc != VTPV_OK) return rc;
    d->cur = *out;
    d->cur_id = d->v->next_id++;
    if (d->v->next_id == 0) d->v->next_id = 1;
    return VTPV_OK;
}

void vtpv_lavc_flush(vtpv_lavc_dec *d)
{
    if (!d) return;
    avcodec_flush_buffers(d->ctx);
    drop_allocs(d);
    d->cur_id = 0;
}

void vtpv_lavc_close_decoder(vtpv_lavc_dec *d)
{
    vtpv_lavc_dec **pp;
    if (!d) return;
    for (pp = &d->v->decs; *pp; pp = &(*pp)->next)
        if (*pp == d) { *pp = d->next; break; }
    drop_allocs(d);
    avcodec_free_context(&d->ctx);
    av_packet_free(&d->pkt);
    av_frame_free(&d->out_frame);
    av_frame_free(&d->sw_frame);
    free(d->conv);
    free(d->au);
    free(d);
}

int vtpv_lavc_frame_copy(const vtpv_lavc_frame *f, uint8_t *y, size_t y_stride,
                         uint8_t *uv, size_t uv_stride)
{
    size_t bpp, row, r, ch;
    if (!f || !y || !f->plane[0]) return VTPV_ERR_MALFORMED;
    bpp = f->pixfmt == VTPV_PIXFMT_P010 ? 2 : 1;
    row = (size_t)f->width * bpp;
    if (y_stride < row) return VTPV_ERR_MALFORMED;
    for (r = 0; r < f->height; r++)
        memcpy(y + y_stride * r, f->plane[0] + f->stride[0] * r, row);
    if (uv) {
        row = (((size_t)f->width + 1) / 2) * 2 * bpp;
        if (uv_stride < row || !f->plane[1]) return VTPV_ERR_MALFORMED;
        ch = ((size_t)f->height + 1) / 2;
        for (r = 0; r < ch; r++)
            memcpy(uv + uv_stride * r, f->plane[1] + f->stride[1] * r, row);
    }
    return VTPV_OK;
}

/* ---- vtpv_decoder_backend adapter ------------------------------------ */

static int be_query_caps(void *ctx, vtpv_caps *out, size_t cap)
{
    return vtpv_lavc_query_caps(ctx, out, cap);
}

static int be_open_decoder(void *ctx, uint32_t codec, uint32_t w, uint32_t h,
                           const uint8_t *extradata, size_t extradata_len, void **dec)
{
    vtpv_lavc_dec *d = NULL;
    int rc = vtpv_lavc_open_decoder(ctx, codec, w, h, extradata, extradata_len, &d);
    *dec = d;
    return rc;
}

static int be_decode(void *dec, const uint8_t *sample, size_t sample_len,
                     vtpv_decoded_frame *out)
{
    vtpv_lavc_dec *d = dec;
    vtpv_lavc_frame f;
    int rc = vtpv_lavc_decode(d, sample, sample_len, &f);
    memset(out, 0, sizeof(*out));
    if (rc == VTPV_LAVC_NOPIC) return VTPV_DROPPED;     /* engine emits a dropped frame  */
    if (rc != VTPV_OK) return rc;
    out->surface_id = d->cur_id;
    out->width = f.width;
    out->height = f.height;
    out->pixfmt = f.pixfmt;
    return VTPV_OK;
}

static void be_close_decoder(void *dec) { vtpv_lavc_close_decoder(dec); }

static const vtpv_decoder_backend g_lavc_backend = {
    be_query_caps, be_open_decoder, be_decode, be_close_decoder
};

const vtpv_decoder_backend *vtpv_lavc_backend(void) { return &g_lavc_backend; }

static const vtpv_lavc_frame *find_surface(vtpv_lavc *v, uint32_t id)
{
    vtpv_lavc_dec *d;
    if (!v || !id) return NULL;
    for (d = v->decs; d; d = d->next)
        if (d->cur_id == id) return &d->cur;
    return NULL;
}

int vtpv_lavc_read_luma(vtpv_lavc *v, uint32_t surface_id, uint8_t *dst,
                        size_t dst_stride, uint32_t w, uint32_t h)
{
    const vtpv_lavc_frame *f = find_surface(v, surface_id);
    size_t r, row;
    if (!f || !dst || f->pixfmt != VTPV_PIXFMT_NV12) return VTPV_ERR_BACKEND;
    if (w > f->width || h > f->height || dst_stride < w) return VTPV_ERR_MALFORMED;
    row = w;
    for (r = 0; r < h; r++) memcpy(dst + dst_stride * r, f->plane[0] + f->stride[0] * r, row);
    return VTPV_OK;
}

int vtpv_lavc_read_chroma(vtpv_lavc *v, uint32_t surface_id, uint8_t *dst,
                          size_t dst_stride, uint32_t w, uint32_t h)
{
    const vtpv_lavc_frame *f = find_surface(v, surface_id);
    size_t r, row = ((size_t)w + 1) / 2 * 2, rows = ((size_t)h + 1) / 2;
    if (!f || !dst || f->pixfmt != VTPV_PIXFMT_NV12) return VTPV_ERR_BACKEND;
    if (w > f->width || h > f->height || dst_stride < row) return VTPV_ERR_MALFORMED;
    for (r = 0; r < rows; r++) memcpy(dst + dst_stride * r, f->plane[1] + f->stride[1] * r, row);
    return VTPV_OK;
}
