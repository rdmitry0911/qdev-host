/* test_lavc_util.h - shared test-time stream generation (libx264/libx265 ->
 * MP4 -> avcC/hvcC + length-prefixed samples) and the independent software
 * oracle (libavcodec display-order decode, MD5 per decode index over packed
 * cropped NV12). Tests only. */
#ifndef TEST_LAVC_UTIL_H
#define TEST_LAVC_UTIL_H
#include <stddef.h>
#include <stdint.h>
#include <libavcodec/avcodec.h>

#define MAXF 64

typedef struct stream {
    uint8_t  *extradata; size_t extradata_len;
    uint8_t **pkt; size_t *pkt_len; int n;   /* decode order                 */
    int       w, h;
    enum AVCodecID id;
} stream;

typedef struct oracle {
    int     have[MAXF];
    uint8_t md5[MAXF][16];
    int     w, h, n_out;
    int     bits;   /* 8: packed NV12 MD5s; 10: packed P010 ('x420') MD5s */
} oracle;

void stream_free(stream *s);
/* 0 ok, 77 encoder missing, 1 failure */
int  make_stream(const char *encoder, const char *params, enum AVPixelFormat pf,
                 int w, int h, int n, const char *tag, stream *out);
int  run_oracle(const stream *s, oracle *o);
#endif
