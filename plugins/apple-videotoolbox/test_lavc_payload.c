/*
 * test_lavc_payload.c - the stream decode core on a recorded payload, per backend:
 * every access unit decoded through vtpv_lavc (the production core: open,
 * decode, the unit's CPU-readable picture) and landed with the device's direct
 * landing (vt_scatter_resolve + vt_scatter_write_rows) into a scattered guest
 * surface; the landed planes' packed cropped NV12 MD5 must equal the software
 * oracle's MD5 for that DECODE index (md5.txt), each unit exactly one picture.
 *
 *   test-lavc-payload vaapi|cuda DEVICE PAYLOAD_DIR...
 *     DEVICE: render node (vaapi) or CUDA ordinal (cuda), e.g. /dev/dri/renderD128 or 0
 *     PAYLOAD_DIR: codec.txt ("avc1|hvc1 W H N"), record.bin (avcC/hvcC),
 *                  samples.bin (4-byte big-endian length + length-prefixed NAL
 *                  units per access unit), md5.txt (oracle MD5 per decode index)
 *                  = the gva-payload tool's output (gva_payload.c) as used by the
 *                  guest E2E (apple-virgl evidence gva-u1-e2e / gva-deploy).
 * Needs no encoder, so it runs against the package's bundled libavcodec (decoders
 * + hwaccels only) on the target host. Exit 0 all equal, 1 a mismatch, 77 the
 * backend is not available here.
 *
 * Written for the CUDA/NVDEC decode-order picture (vtpv_lavc.c): with nvdec the
 * decoder's picture receives the pixels only in its FrameDecodeData.post_process
 * at output time, so a read right after the unit's decode saw zeros
 * (warm-4080 2026-11-21) - RED there before the fix, GREEN after; VA-API must
 * stay GREEN unchanged.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/log.h>
#include <libavutil/md5.h>
#include <libavutil/mem.h>

#include "vtpv_lavc.h"
#include "vt_scatter.h"

#define PS 4096u
#define MAXU 256

static uint8_t *g_ram;
static size_t g_ram_len;
static void *map_ram(void *ctx, uint64_t gpa, uint64_t len)
{
    (void)ctx;
    if (gpa + len > g_ram_len || gpa + len < gpa) return NULL;
    return g_ram + gpa;
}

static uint8_t *slurp(const char *dir, const char *name, size_t *n)
{
    char p[1024];
    FILE *f;
    uint8_t *b;
    long l;
    snprintf(p, sizeof p, "%s/%s", dir, name);
    if (!(f = fopen(p, "rb"))) return NULL;
    fseek(f, 0, SEEK_END); l = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc((size_t)l + 1);
    if (b && fread(b, 1, (size_t)l, f) != (size_t)l) { free(b); b = NULL; }
    fclose(f);
    *n = (size_t)l;
    return b;
}

/* returns 0 = all equal, 1 = mismatch/failure */
static int run_payload(vtpv_lavc *hw, const char *bk, const char *dir)
{
    char p[1024], codec[8] = "", exp[MAXU][33];
    int W, H, N, i, ok = 0, emitted = 0, zero = 0, rc;
    size_t rl = 0, sl = 0, off = 0;
    uint8_t *rec, *smp;
    FILE *f;
    vtpv_lavc_dec *d = NULL;

    snprintf(p, sizeof p, "%s/codec.txt", dir);
    if (!(f = fopen(p, "r")) || fscanf(f, "%4s %d %d %d", codec, &W, &H, &N) != 4) { printf("  %s: bad codec.txt\n", dir); if (f) fclose(f); return 1; }
    fclose(f);
    if (N > MAXU) N = MAXU;
    snprintf(p, sizeof p, "%s/md5.txt", dir);
    if (!(f = fopen(p, "r"))) { printf("  %s: no md5.txt\n", dir); return 1; }
    for (i = 0; i < N; i++) if (fscanf(f, "%32s", exp[i]) != 1) { fclose(f); printf("  %s: short md5.txt\n", dir); return 1; }
    fclose(f);
    rec = slurp(dir, "record.bin", &rl); smp = slurp(dir, "samples.bin", &sl);
    if (!rec || !smp) { printf("  %s: no record/samples\n", dir); free(rec); free(smp); return 1; }

    /* the guest's '420v' IOSurface layout seen live (y_off 256, rows = width, CbCr after
     * the luma, allocation rounded to pages), guest pages in shuffled 4-page chunks */
    size_t ystride = (size_t)W, yoff = 256, uvoff = yoff + ystride * (size_t)H, uvstride = ystride;
    size_t total = (uvoff + uvstride * (((size_t)H + 1) / 2) + PS - 1) / PS * PS;
    uint32_t np = (uint32_t)(total / PS), nchunk = (np + 3) / 4, c;
    uint64_t *pg = malloc(sizeof(uint64_t) * np);
    uint32_t *perm = malloc(sizeof(uint32_t) * nchunk);
    for (c = 0; c < nchunk; c++) perm[c] = c;
    srand(11);
    for (c = nchunk; c > 1; c--) { uint32_t j = (uint32_t)rand() % c, t = perm[c - 1]; perm[c - 1] = perm[j]; perm[j] = t; }
    for (uint32_t q = 0; q < np; q++) pg[q] = ((uint64_t)perm[q / 4] * 4 + q % 4 + 8) * PS;
    g_ram_len = ((size_t)nchunk * 4 + 16) * PS;
    g_ram = malloc(g_ram_len);
    void **ptrs = malloc(sizeof(void *) * np);
    size_t *runv = malloc(sizeof(size_t) * np);
    uint8_t *row = malloc(ystride + 2);

    rc = vtpv_lavc_open_decoder(hw, !strcmp(codec, "hvc1") ? VTPV_CODEC_HEVC : VTPV_CODEC_H264,
                                (uint32_t)W, (uint32_t)H, rec, rl, &d);
    if (rc != VTPV_OK) { printf("  %s %s: open_decoder rc %d\n", bk, dir, rc); N = 0; }
    for (i = 0; d && i < N && off + 4 <= sl; i++) {
        size_t len = ((size_t)smp[off] << 24) | ((size_t)smp[off + 1] << 16) | ((size_t)smp[off + 2] << 8) | smp[off + 3];
        vtpv_lavc_frame fr;
        off += 4;
        if (len > sl - off) break;
        rc = vtpv_lavc_decode(d, smp + off, len, &fr);
        off += len;
        if (rc != VTPV_OK) { printf("  unit %d: decode rc %d\n", i, rc); continue; }
        emitted++;
        size_t cw = ((size_t)fr.width + 1) / 2 * 2, ch = ((size_t)fr.height + 1) / 2, r, nz = 0;
        for (size_t k = 0; k < g_ram_len; k++) g_ram[k] = (uint8_t)(k * 29u + 3u);
        if (vt_scatter_resolve(ptrs, runv, total, pg, np, PS, map_ram, NULL) != 0 ||
            vt_scatter_write_rows(ptrs, runv, total, PS, yoff, ystride, fr.plane[0], fr.stride[0], fr.width, fr.height) != 0 ||
            vt_scatter_write_rows(ptrs, runv, total, PS, uvoff, uvstride, fr.plane[1], fr.stride[1], cw, ch) != 0) {
            printf("  unit %d: landing failed\n", i);
            continue;
        }
        struct AVMD5 *m = av_md5_alloc();
        uint8_t md[16];
        char hex[33];
        av_md5_init(m);
        for (r = 0; r < fr.height; r++) {
            size_t o = yoff + ystride * r;
            for (size_t b = 0; b < fr.width; b++) { row[b] = g_ram[pg[(o + b) / PS] + (o + b) % PS]; nz += row[b] != 0; }
            av_md5_update(m, row, fr.width);
        }
        for (r = 0; r < ch; r++) {
            size_t o = uvoff + uvstride * r;
            for (size_t b = 0; b < cw; b++) row[b] = g_ram[pg[(o + b) / PS] + (o + b) % PS];
            av_md5_update(m, row, cw);
        }
        av_md5_final(m, md); av_free(m);
        for (int k = 0; k < 16; k++) sprintf(hex + 2 * k, "%02x", md[k]);
        if (!nz) zero++;
        if (!strcmp(hex, exp[i])) ok++;
        else if (i < 4 || getenv("PAYLOAD_VERBOSE")) printf("  unit %d: md5 %s != oracle %s%s\n", i, hex, exp[i], nz ? "" : " (all-zero luma)");
    }
    vtpv_lavc_close_decoder(d);
    printf("RESULT: backend=%s payload=%s codec=%s %dx%d frames_ok=%d/%d emitted=%d/%d all_zero_luma=%d\n",
           bk, dir, codec, W, H, ok, N, emitted, N, zero);
    free(pg); free(perm); free(g_ram); free(ptrs); free(runv); free(row); free(rec); free(smp);
    g_ram = NULL;
    return (N > 0 && ok == N && emitted == N) ? 0 : 1;
}

int main(int argc, char **argv)
{
    vtpv_lavc *hw = NULL;
    char err[256] = "";
    int fails = 0, i, cuda;
    if (argc < 4) { fprintf(stderr, "usage: %s vaapi|cuda DEVICE PAYLOAD_DIR...\n", argv[0]); return 2; }
    cuda = !strcmp(argv[1], "cuda");
    av_log_set_level(getenv("PAYLOAD_VERBOSE") ? AV_LOG_INFO : AV_LOG_QUIET);
    if (vtpv_lavc_open(cuda ? VTPV_LAVC_HW_CUDA : VTPV_LAVC_HW_VAAPI, argv[2], &hw, err, sizeof(err)) != VTPV_OK) {
        printf("SKIP: %s backend unavailable: %s\n", argv[1], err);
        return 77;
    }
    for (i = 3; i < argc; i++) fails += run_payload(hw, argv[1], argv[i]);
    vtpv_lavc_close(hw);
    printf("%s: %d payload(s), %d failed\n", fails ? "FAIL" : "PASS", argc - 3, fails);
    return fails ? 1 : 0;
}
