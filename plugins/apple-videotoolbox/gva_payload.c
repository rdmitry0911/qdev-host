/*
 * gva_payload.c - write a multi-frame guest decode payload for the AppleGVA
 * E2E (plan U1/U3): a stream generated with libx264/libx265 (MP4 round trip ->
 * avcC/hvcC + length-prefixed samples, as VideoToolbox gets them from a movie)
 * and the software oracle's MD5 per decode index (packed cropped NV12).
 *
 *   gva-payload <dir> h264|hevc|hevc10 <w> <h> <n>
 *
 * hevc10 = HEVC Main 10 (libx265 yuv420p10le): the oracle MD5s are of packed cropped
 * P010 ('x420': each 10-bit sample in the high bits of a little-endian 16-bit word)
 * and codec.txt gets a fifth field, the bit depth (10).
 *
 * Files: codec.txt ("avc1|hvc1 W H N [10]"), record.bin (avcC/hvcC), samples.bin
 * (per sample: u32 big-endian length, bytes), md5.txt (one hex MD5 per decode
 * index). Test tooling only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_lavc_util.h"

int main(int argc, char **argv)
{
    stream s;
    static oracle o;
    char p[1024];
    FILE *f;
    int i, k, w, h, n, hevc, ten;
    if (argc < 6) { fprintf(stderr, "usage: %s <dir> h264|hevc|hevc10 <w> <h> <n>\n", argv[0]); return 2; }
    ten = !strcmp(argv[2], "hevc10");
    hevc = ten || !strcmp(argv[2], "hevc");
    w = atoi(argv[3]); h = atoi(argv[4]); n = atoi(argv[5]);
    if (n > MAXF) n = MAXF;
    if (make_stream(hevc ? "libx265" : "libx264",
                    hevc ? "bframes=3:keyint=8:scenecut=0:log-level=error"
                         : "bframes=2:ref=3:keyint=12:min-keyint=12:scenecut=0",
                    ten ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P, w, h, n,
                    ten ? "pl265m10" : hevc ? "pl265" : "pl264", &s) != 0) return 1;
    if (run_oracle(&s, &o) != 0 || o.n_out != n) { fprintf(stderr, "oracle %d/%d\n", o.n_out, n); return 1; }
    if (o.bits != (ten ? 10 : 8)) { fprintf(stderr, "oracle bit depth %d\n", o.bits); return 1; }
    snprintf(p, sizeof p, "%s/codec.txt", argv[1]);
    f = fopen(p, "w"); if (!f) return 1;
    if (ten) fprintf(f, "hvc1 %d %d %d 10\n", s.w, s.h, s.n);
    else fprintf(f, "%s %d %d %d\n", hevc ? "hvc1" : "avc1", s.w, s.h, s.n);
    fclose(f);
    snprintf(p, sizeof p, "%s/record.bin", argv[1]);
    f = fopen(p, "wb"); if (!f) return 1;
    fwrite(s.extradata, 1, s.extradata_len, f); fclose(f);
    snprintf(p, sizeof p, "%s/samples.bin", argv[1]);
    f = fopen(p, "wb"); if (!f) return 1;
    for (i = 0; i < s.n; i++) {
        uint8_t be[4] = { (uint8_t)(s.pkt_len[i] >> 24), (uint8_t)(s.pkt_len[i] >> 16),
                          (uint8_t)(s.pkt_len[i] >> 8), (uint8_t)s.pkt_len[i] };
        fwrite(be, 1, 4, f); fwrite(s.pkt[i], 1, s.pkt_len[i], f);
    }
    fclose(f);
    snprintf(p, sizeof p, "%s/md5.txt", argv[1]);
    f = fopen(p, "w"); if (!f) return 1;
    for (i = 0; i < s.n; i++) {
        for (k = 0; k < 16; k++) fprintf(f, "%02x", o.md5[i][k]);
        fprintf(f, "\n");
    }
    fclose(f);
    printf("payload %s: %s %dx%d %d samples, %d-bit\n", argv[1], hevc ? "hvc1" : "avc1", s.w, s.h, s.n, ten ? 10 : 8);
    stream_free(&s);
    return 0;
}
