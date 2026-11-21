/*
 * test_lavc_land.c - the per-frame host path of a decoded unit into a scattered
 * guest surface (GVA 4K landing), checked and timed.
 *
 *   test-lavc-land [render_node] [frames]
 *
 * Path under test = what the device does for one dfrm/Dbuf exchange with the
 * stream core: vtpv_lavc_decode() (decode + CPU-readable picture) then the landing
 * of its NV12 planes into the lent IOSurface's guest pages.
 *  - shadow landing (the device's legacy path, kept for the intra backends):
 *    malloc a shadow of the allocation, vt_scatter_gather, copy the rows,
 *    vt_scatter_write, free;
 *  - direct landing (the stream core path): vt_scatter_resolve +
 *    vt_scatter_write_rows for the luma and CbCr rows.
 * Checks: every unit's direct landing leaves guest RAM byte-identical to the
 * shadow landing on the same initial RAM, and the landed planes' packed cropped
 * NV12 MD5 equals the independent software oracle (test_lavc_util.c) for that
 * decode index. Surface layout = the guest's 4K '420v' IOSurface seen live on
 * warm-hzmain (y_off 256, rows = width, uv after the luma, alloc rounded to pages);
 * guest pages = a BOUNDARY DOUBLE (host memory, 4-page chunks in shuffled order).
 * Prints per-frame ms: decode (incl. the CPU-readable picture), shadow landing,
 * direct landing. LAND_COLD=1 evicts the CPU caches (a 192 MB sweep) before the
 * decode and before each landing, as in the device process where QEMU and the
 * guest share the caches. Exit 77 = no render node / no encoder.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavutil/md5.h>

#include "vtpv_lavc.h"
#include "vt_scatter.h"
#include "test_lavc_util.h"

static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define PS 4096u
static uint8_t *g_ram;
static size_t g_ram_len;
static void *map_ram(void *ctx, uint64_t gpa, uint64_t len)
{
    (void)ctx;
    if (gpa + len > g_ram_len || gpa + len < gpa) return NULL;
    return g_ram + gpa;
}

static uint8_t *g_evict;
static size_t g_evict_len;
static void evict(void)
{
    if (!g_evict) return;
    for (size_t k = 0; k < g_evict_len; k += 64) g_evict[k]++;
}

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void run(vtpv_lavc *hw, const char *name, const char *params, int w, int h, int n)
{
    stream s;
    static oracle o;
    vtpv_lavc_dec *d = NULL;
    int rc = make_stream("libx265", params, AV_PIX_FMT_YUV420P, w, h, n, name, &s), i;
    if (rc == 77) { printf("  %s: SKIP (no libx265)\n", name); return; }
    CHECK(rc == 0 && run_oracle(&s, &o) == 0 && o.n_out == n, "%s: stream/oracle", name);
    if (rc) return;
    /* the guest surface: 420v, rows = width, luma at 256, CbCr right after it */
    size_t ystride = (size_t)w, yoff = 256, uvoff = yoff + ystride * (size_t)h, uvstride = ystride;
    size_t total = (uvoff + uvstride * (((size_t)h + 1) / 2) + PS - 1) / PS * PS;
    uint32_t np = (uint32_t)(total / PS), i4, nchunk = (np + 3) / 4;
    uint64_t *pg = malloc(sizeof(uint64_t) * np);
    uint32_t *perm = malloc(sizeof(uint32_t) * nchunk);
    for (i4 = 0; i4 < nchunk; i4++) perm[i4] = i4;
    srand(7);
    for (i4 = nchunk; i4 > 1; i4--) { uint32_t j = (uint32_t)rand() % i4, t = perm[i4 - 1]; perm[i4 - 1] = perm[j]; perm[j] = t; }
    for (uint32_t p = 0; p < np; p++) pg[p] = ((uint64_t)perm[p / 4] * 4 + p % 4 + 8) * PS; /* page 0..7 unused */
    g_ram_len = ((size_t)nchunk * 4 + 16) * PS;
    uint8_t *ram_a = malloc(g_ram_len), *ram_b = malloc(g_ram_len), *init = malloc(g_ram_len);
    for (size_t k = 0; k < g_ram_len; k++) init[k] = (uint8_t)(k * 29u + 3u);
    void **ptrs = malloc(sizeof(void *) * np);
    size_t *runv = malloc(sizeof(size_t) * np);
    double t_dec = 0, t_old = 0, t_new = 0;
    int counted = 0, same = 0, oracle_ok = 0;

    CHECK(vtpv_lavc_open_decoder(hw, VTPV_CODEC_HEVC, (uint32_t)w, (uint32_t)h, s.extradata, s.extradata_len, &d) == VTPV_OK,
          "%s: open", name);
    for (i = 0; d && i < s.n; i++) {
        vtpv_lavc_frame f;
        evict();
        double t0 = now_ms();
        rc = vtpv_lavc_decode(d, s.pkt[i], s.pkt_len[i], &f);
        double t1 = now_ms();
        if (rc != VTPV_OK) { CHECK(0, "%s: unit %d rc %d", name, i, rc); continue; }
        size_t cw = ((size_t)f.width + 1) / 2 * 2, ch = ((size_t)f.height + 1) / 2, r;
        /* shadow landing into ram_a */
        memcpy(ram_a, init, g_ram_len); g_ram = ram_a;
        evict();
        double t2 = now_ms();
        uint8_t *sh = malloc(total);
        CHECK(vt_scatter_gather(sh, total, pg, np, PS, map_ram, NULL) == 0, "gather");
        for (r = 0; r < f.height; r++) memcpy(sh + yoff + ystride * r, f.plane[0] + f.stride[0] * r, f.width);
        for (r = 0; r < ch; r++) memcpy(sh + uvoff + uvstride * r, f.plane[1] + f.stride[1] * r, cw);
        CHECK(vt_scatter_write(sh, total, pg, np, PS, map_ram, NULL) == 0, "write");
        free(sh);
        double t3 = now_ms();
        /* direct landing into ram_b */
        memcpy(ram_b, init, g_ram_len); g_ram = ram_b;
        evict();
        double t4 = now_ms();
        int ok = vt_scatter_resolve(ptrs, runv, total, pg, np, PS, map_ram, NULL) == 0 &&
                 vt_scatter_write_rows(ptrs, runv, total, PS, yoff, ystride, f.plane[0], f.stride[0], f.width, f.height) == 0 &&
                 vt_scatter_write_rows(ptrs, runv, total, PS, uvoff, uvstride, f.plane[1], f.stride[1], cw, ch) == 0;
        double t5 = now_ms();
        CHECK(ok, "%s: unit %d direct landing", name, i);
        if (!memcmp(ram_a, ram_b, g_ram_len)) same++;
        /* landed planes (read back through the page list) vs the software oracle */
        {
            struct AVMD5 *m = av_md5_alloc();
            uint8_t md5[16], *row = malloc(ystride);
            av_md5_init(m);
            for (r = 0; r < f.height; r++) {
                size_t off = yoff + ystride * r;
                for (size_t b = 0; b < f.width; b++) row[b] = ram_b[pg[(off + b) / PS] + (off + b) % PS];
                av_md5_update(m, row, f.width);
            }
            for (r = 0; r < ch; r++) {
                size_t off = uvoff + uvstride * r;
                for (size_t b = 0; b < cw; b++) row[b] = ram_b[pg[(off + b) / PS] + (off + b) % PS];
                av_md5_update(m, row, cw);
            }
            av_md5_final(m, md5); av_free(m); free(row);
            if (o.have[i] && !memcmp(md5, o.md5[i], 16)) oracle_ok++;
        }
        if (i >= 2) { t_dec += t1 - t0; t_old += t3 - t2; t_new += t5 - t4; counted++; }
    }
    CHECK(same == s.n, "%s: %d/%d units: direct landing == shadow landing (all guest RAM bytes)", name, same, s.n);
    CHECK(oracle_ok == s.n, "%s: %d/%d landed units == software oracle", name, oracle_ok, s.n);
    if (counted)
        printf("  %s: %d units, per frame (units 2..): decode+cpu-picture %.2f ms, shadow landing %.2f ms, direct landing %.2f ms;"
               " direct==shadow %d/%d, oracle %d/%d\n", name, s.n, t_dec / counted, t_old / counted, t_new / counted,
               same, s.n, oracle_ok, s.n);
    vtpv_lavc_close_decoder(d);
    free(pg); free(perm); free(ram_a); free(ram_b); free(init); free(ptrs); free(runv);
    stream_free(&s);
}

int main(int argc, char **argv)
{
    const char *node = argc > 1 ? argv[1] : "/dev/dri/renderD128";
    int n = argc > 2 ? atoi(argv[2]) : 8;
    vtpv_lavc *hw = NULL;
    char err[256] = "";
    av_log_set_level(AV_LOG_QUIET);
    if (getenv("LAND_COLD") && atoi(getenv("LAND_COLD"))) {
        g_evict_len = 192u << 20; g_evict = calloc(1, g_evict_len);
        printf("  (LAND_COLD: caches evicted before decode and before each landing)\n");
    }
    if (vtpv_lavc_open(VTPV_LAVC_HW_VAAPI, node, &hw, err, sizeof(err)) != VTPV_OK) { printf("SKIP: %s\n", err); return 77; }
    run(hw, "hevc-3840x2160", "bframes=2:keyint=24:scenecut=0:log-level=error", 3840, 2160, n);
    run(hw, "hevc-1920x1080", "bframes=2:keyint=24:scenecut=0:log-level=error", 1920, 1080, n);
    vtpv_lavc_close(hw);
    printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
