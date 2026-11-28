/* test_lavc_util.c - see test_lavc_util.h (tests only). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#ifndef TEST_LAVC_NO_ENCODE
#include <libavformat/avformat.h>
#endif
#include <libavutil/md5.h>
#include <libavutil/opt.h>

#include "test_lavc_util.h"



void stream_free(stream *s)
{
    int i;
    for (i = 0; i < s->n; i++) free(s->pkt[i]);
    free(s->pkt); free(s->pkt_len); free(s->extradata);
    memset(s, 0, sizeof(*s));
}

#ifndef TEST_LAVC_NO_ENCODE /* VT-DECODE-WORKER CUDA smoke: a build without encoders / libavformat */
static void fill_frame(AVFrame *f, int idx)
{
    int x, y, cw = (f->width + 1) / 2, ch = (f->height + 1) / 2;
    int cdiv = f->format == AV_PIX_FMT_YUV444P ? 1 : 2;
    int hi = f->format == AV_PIX_FMT_YUV420P10LE;
    (void)cw; (void)ch;
    for (y = 0; y < f->height; y++)
        for (x = 0; x < f->width; x++) {
            int v = (x + 2 * idx) ^ (y + idx);
            int blk = (x / 32 + y / 32 + idx / 3) & 1;
            int s = (v & 0xff) / 2 + (blk ? 90 : 20) + ((x * 7 + y * 13 + idx) % 11);
            if (s > 255) s = 255;
            if (hi) ((uint16_t *)(f->data[0] + f->linesize[0] * y))[x] = (uint16_t)(s * 4 + (x & 3));
            else f->data[0][f->linesize[0] * y + x] = (uint8_t)s;
        }
    for (y = 0; y < f->height / cdiv; y++)
        for (x = 0; x < f->width / cdiv; x++) {
            int u = 128 + ((x + idx) % 40) - 20, v = 128 + ((y * 2 + idx) % 50) - 25;
            if (hi) {
                ((uint16_t *)(f->data[1] + f->linesize[1] * y))[x] = (uint16_t)(u * 4);
                ((uint16_t *)(f->data[2] + f->linesize[2] * y))[x] = (uint16_t)(v * 4);
            } else {
                f->data[1][f->linesize[1] * y + x] = (uint8_t)u;
                f->data[2][f->linesize[2] * y + x] = (uint8_t)v;
            }
        }
}

static int make_stream_once(const char *encoder, const char *params, enum AVPixelFormat pf,
                       int w, int h, int n, const char *tag, stream *out)
{
    const AVCodec *c = avcodec_find_encoder_by_name(encoder);
    AVCodecContext *ec = NULL;
    AVFormatContext *oc = NULL, *ic = NULL;
    AVStream *st;
    AVFrame *f = NULL;
    AVPacket *p = NULL;
    char path[256];
    int i, rc = 1, cap = 0;

    memset(out, 0, sizeof(*out));
    if (!c) return 77;
    snprintf(path, sizeof(path), "build/test-lavc-%d-%s.mp4", (int)getpid(), tag);
    ec = avcodec_alloc_context3(c);
    ec->width = w; ec->height = h; ec->pix_fmt = pf;
    ec->time_base = (AVRational){1, 30}; ec->framerate = (AVRational){30, 1};
    ec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    ec->thread_count = 4;
    av_opt_set(ec->priv_data, "preset", "ultrafast", 0);
    av_opt_set(ec->priv_data, !strcmp(encoder, "libx264") ? "x264-params" : "x265-params", params, 0);
    if (avcodec_open2(ec, c, NULL) < 0) { printf("open %s failed\n", encoder); goto done; }

    if (avformat_alloc_output_context2(&oc, NULL, "mp4", path) < 0) goto done;
    st = avformat_new_stream(oc, NULL);
    avcodec_parameters_from_context(st->codecpar, ec);
    if (!strcmp(encoder, "libx265")) st->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
    st->time_base = ec->time_base;
    if (avio_open(&oc->pb, path, AVIO_FLAG_WRITE) < 0) goto done;
    if (avformat_write_header(oc, NULL) < 0) goto done;

    f = av_frame_alloc(); p = av_packet_alloc();
    f->format = pf; f->width = w; f->height = h;
    av_frame_get_buffer(f, 0);
    for (i = 0; i <= n; i++) {
        if (i < n) {
            av_frame_make_writable(f);
            fill_frame(f, i);
            f->pts = i;
            avcodec_send_frame(ec, f);
        } else avcodec_send_frame(ec, NULL);
        while (avcodec_receive_packet(ec, p) == 0) {
            av_packet_rescale_ts(p, ec->time_base, st->time_base);
            p->stream_index = 0;
            av_interleaved_write_frame(oc, p);
        }
    }
    av_write_trailer(oc);
    avio_closep(&oc->pb);

    if (avformat_open_input(&ic, path, NULL, NULL) < 0) goto done;
    if (avformat_find_stream_info(ic, NULL) < 0) goto done;
    /* x265 with very short 10-bit streams occasionally leaves an MP4 without a
     * track (seen 1 in ~15 runs); report it as a generation failure. */
    if (ic->nb_streams < 1 || !ic->streams[0]->codecpar->extradata_size) {
        printf("stream %s: muxed file has no usable track\n", encoder);
        goto done;
    }
    st = ic->streams[0];
    out->id = st->codecpar->codec_id;
    out->extradata_len = (size_t)st->codecpar->extradata_size;
    out->extradata = malloc(out->extradata_len);
    memcpy(out->extradata, st->codecpar->extradata, out->extradata_len);
    out->w = st->codecpar->width; out->h = st->codecpar->height;
    while (av_read_frame(ic, p) == 0) {
        if (out->n == cap) {
            cap = cap ? cap * 2 : 64;
            out->pkt = realloc(out->pkt, sizeof(*out->pkt) * (size_t)cap);
            out->pkt_len = realloc(out->pkt_len, sizeof(*out->pkt_len) * (size_t)cap);
        }
        out->pkt[out->n] = malloc((size_t)p->size);
        memcpy(out->pkt[out->n], p->data, (size_t)p->size);
        out->pkt_len[out->n] = (size_t)p->size;
        out->n++;
        av_packet_unref(p);
    }
    rc = out->n == n ? 0 : 1;
    if (rc) printf("stream %s: %d samples, want %d\n", encoder, out->n, n);
done:
    av_frame_free(&f); av_packet_free(&p);
    avcodec_free_context(&ec);
    if (oc) avformat_free_context(oc);
    if (ic) avformat_close_input(&ic);
    unlink(path);
    return rc;
}

#endif

/* ---- the oracle: libavcodec software decode, display order, by pts ---- */


static void md5_nv12_from_planar(const AVFrame *f, int w, int h, uint8_t out[16])
{
    struct AVMD5 *m = av_md5_alloc();
    uint8_t row[8192];
    int y, x, cw = (w + 1) / 2;
    av_md5_init(m);
    for (y = 0; y < h; y++) av_md5_update(m, f->data[0] + (size_t)f->linesize[0] * y, (size_t)w);
    for (y = 0; y < (h + 1) / 2; y++) {
        for (x = 0; x < cw; x++) {
            row[2 * x] = f->data[1][(size_t)f->linesize[1] * y + x];
            row[2 * x + 1] = f->data[2][(size_t)f->linesize[2] * y + x];
        }
        av_md5_update(m, row, (size_t)cw * 2);
    }
    av_md5_final(m, out);
    av_free(m);
}

/* x420 / P010: each 10-bit sample in the high bits of a little-endian 16-bit word
 * (CoreVideo kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange), CbCr interleaved. */
static void put_p010(uint8_t *d, uint16_t v10) { uint16_t v = (uint16_t)(v10 << 6); d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8); }
static void md5_p010_from_planar(const AVFrame *f, int w, int h, uint8_t out[16])
{
    struct AVMD5 *m = av_md5_alloc();
    static uint8_t row[16384];
    int y, x, cw = (w + 1) / 2;
    av_md5_init(m);
    for (y = 0; y < h; y++) {
        const uint16_t *s = (const uint16_t *)(f->data[0] + (size_t)f->linesize[0] * y);
        for (x = 0; x < w; x++) put_p010(row + 2 * x, s[x]);
        av_md5_update(m, row, (size_t)w * 2);
    }
    for (y = 0; y < (h + 1) / 2; y++) {
        const uint16_t *u = (const uint16_t *)(f->data[1] + (size_t)f->linesize[1] * y);
        const uint16_t *v = (const uint16_t *)(f->data[2] + (size_t)f->linesize[2] * y);
        for (x = 0; x < cw; x++) { put_p010(row + 4 * x, u[x]); put_p010(row + 4 * x + 2, v[x]); }
        av_md5_update(m, row, (size_t)cw * 4);
    }
    av_md5_final(m, out);
    av_free(m);
}

int run_oracle(const stream *s, oracle *o)
{
    const AVCodec *c = avcodec_find_decoder(s->id);
    AVCodecContext *ctx = avcodec_alloc_context3(c);
    AVPacket *p = av_packet_alloc();
    AVFrame *f = av_frame_alloc();
    int i, rc;
    memset(o, 0, sizeof(*o));
    ctx->extradata = av_mallocz(s->extradata_len + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(ctx->extradata, s->extradata, s->extradata_len);
    ctx->extradata_size = (int)s->extradata_len;
    ctx->thread_count = 1;
    if (avcodec_open2(ctx, c, NULL) < 0) return -1;
    for (i = 0; i <= s->n; i++) {
        if (i < s->n) {
            p->data = s->pkt[i]; p->size = (int)s->pkt_len[i]; p->pts = i;
            rc = avcodec_send_packet(ctx, p);
        } else rc = avcodec_send_packet(ctx, NULL);
        (void)rc;
        while (avcodec_receive_frame(ctx, f) == 0) {
            if (f->pts >= 0 && f->pts < MAXF &&
                (f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P /* full range 8-bit */ ||
                 f->format == AV_PIX_FMT_YUV420P10LE)) {
                if (f->format == AV_PIX_FMT_YUV420P10LE) {
                    md5_p010_from_planar(f, f->width, f->height, o->md5[f->pts]);
                    o->bits = 10;
                } else {
                    md5_nv12_from_planar(f, f->width, f->height, o->md5[f->pts]);
                    o->bits = 8;
                }
                o->have[f->pts] = 1;
                o->w = f->width; o->h = f->height;
                o->n_out++;
            }
            av_frame_unref(f);
        }
    }
    p->data = NULL; p->size = 0;
    av_packet_free(&p); av_frame_free(&f); avcodec_free_context(&ctx);
    return 0;
}


int make_stream(const char *encoder, const char *params, enum AVPixelFormat pf,
                int w, int h, int n, const char *tag, stream *out)
{
#ifdef TEST_LAVC_NO_ENCODE
    (void)encoder; (void)params; (void)pf; (void)w; (void)h; (void)n; (void)tag; (void)out;
    return -1;                       /* streams come from VT_TEST_LOAD files */
#else
    int rc = 1, attempt;
    for (attempt = 0; attempt < 3; attempt++) {
        rc = make_stream_once(encoder, params, pf, w, h, n, tag, out);
        if (rc != 1) break;
        stream_free(out);
    }
    return rc;
#endif
}
