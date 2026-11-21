/* test_vt_land_lavc.c - GVA-LAND-BOUNDS on a real decoded picture (production
 * vtpv_lavc core, software decode; exit 77 when no encoder).
 *
 * The incident class (warm-4080 guest panic 2026-11-21): a stream whose picture is
 * TALLER than the guest's description (Dpba size) on a physically contiguous lent
 * surface. LAND-OLD-01 records what the pre-fix contiguous path did - its exact
 * calls, vtpv_lavc_read_luma/read_chroma(frame width, frame height) at the
 * surface's plane offsets - into a boundary-double 'guest RAM' (one surface page +
 * guard pages): bytes past the surface change. LAND-NEW-01: the fixed landing
 * (vt_land_planes, what the device now runs for every bridge surface) on the same
 * picture: top left of the picture in the planes, every guard byte kept. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vtpv_lavc.h"
#include "vt_land.h"
#include "vt_scatter.h"
#include "test_lavc_util.h"

#define PS 4096u
static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t g_ram[16 * PS];
static void *map_ram(void *ctx, uint64_t gpa, uint64_t len)
{ (void)ctx; return (gpa + len <= sizeof g_ram) ? g_ram + gpa : NULL; }

static size_t stray_bytes(size_t lo, size_t hi) /* bytes != 0xEE outside [lo,hi) */
{
    size_t n = 0;
    for (size_t a = 0; a < sizeof g_ram; a++) if ((a < lo || a >= hi) && g_ram[a] != 0xEE) n++;
    return n;
}

int main(void)
{
    stream s;
    vtpv_lavc *v = NULL;
    vtpv_lavc_dec *d = NULL;
    vtpv_lavc_frame f;
    /* the guest's '420v' surface for a 64x16 description: luma bpr 64 x 16 at 256,
     * CbCr bpr 64 x 8 at 1280, one page; placed at page 4 of the RAM double */
    const size_t base = 4 * PS;
    vt_land_geom g = { PS, 256, 64, 64, 16, 1280, 64, 32, 8 };
    int rc;

    rc = make_stream("libx264", "keyint=1:bframes=0", AV_PIX_FMT_YUV420P, 64, 256, 1, "land-tall", &s);
    if (rc == 77) { printf("SKIP: no libx264\n"); return 77; }
    CHECK(rc == 0, "stream");
    if (rc) return 1;
    vtpv_lavc_open(VTPV_LAVC_HW_NONE, NULL, &v, NULL, 0);
    CHECK(vtpv_lavc_open_decoder(v, VTPV_CODEC_H264, 64, 16 /* the description, not the SPS */,
                                 s.extradata, s.extradata_len, &d) == VTPV_OK, "open with a 64x16 description");
    rc = vtpv_lavc_decode(d, s.pkt[0], s.pkt_len[0], &f);
    CHECK(rc == VTPV_OK && f.width == 64 && f.height == 256, "picture is the SPS's 64x256 (rc %d %ux%u)", rc, f.width, f.height);
    if (rc != VTPV_OK) return 1;
    uint32_t sid = 0;
    { vtpv_decoded_frame df; /* the backend's surface id of the current picture */
      (void)df; }
    /* the current picture's backend surface id: the adapter names it; take it via get_frame by probing ids */
    for (uint32_t id = 1; id < 16 && !sid; id++) { vtpv_lavc_frame t; if (vtpv_lavc_get_frame(v, id, &t) == VTPV_OK) sid = id; }
    CHECK(sid != 0, "picture id");

    /* LAND-OLD-01: the pre-fix contiguous path (p = vt_map(gpa_base); read_luma/read_chroma(f.width, f.height)) */
    memset(g_ram, 0xEE, sizeof g_ram);
    int lr = vtpv_lavc_read_luma(v, sid, g_ram + base + 256, 64, f.width, f.height);
    int cr = vtpv_lavc_read_chroma(v, sid, g_ram + base + 1280, 64, f.width, f.height);
    size_t old_stray = stray_bytes(base, base + PS);
    printf("  old contiguous path: read_luma rc %d read_chroma rc %d, %zu bytes written OUTSIDE the one-page surface\n",
           lr, cr, old_stray);
    CHECK(lr == 0 && old_stray > 0, "LAND-OLD-01 (records the hole): the pre-fix path writes past the surface");

    /* LAND-NEW-01: the fixed landing on the same picture */
    memset(g_ram, 0xEE, sizeof g_ram);
    {
        uint64_t pages[1] = { base };
        void *ptrs[1]; size_t run[1];
        vt_land_rows r;
        CHECK(vt_scatter_resolve(ptrs, run, PS, pages, 1, PS, map_ram, NULL) == VT_SCATTER_OK, "resolve");
        rc = vt_land_planes(ptrs, run, PS, &g, f.plane[0], f.stride[0], f.plane[1], f.stride[1],
                            f.width, f.height, 1, &r);
        CHECK(rc == 0 && r.cropped && r.y_rows == 16 && r.uv_rows == 8, "land: rc %d rows %zu/%zu", rc, r.y_rows, r.uv_rows);
        size_t bad = 0;
        for (uint32_t row = 0; row < 16; row++)
            if (memcmp(g_ram + base + 256 + row * 64, f.plane[0] + f.stride[0] * row, 64)) bad++;
        for (uint32_t row = 0; row < 8; row++)
            if (memcmp(g_ram + base + 1280 + row * 64, f.plane[1] + f.stride[1] * row, 64)) bad++;
        size_t new_stray = stray_bytes(base, base + PS);
        CHECK(bad == 0, "LAND-NEW-01: %zu rows differ from the picture's top left", bad);
        CHECK(new_stray == 0, "LAND-NEW-01: %zu bytes written outside the surface", new_stray);
        printf("  fixed landing: 64x16 top left landed, %zu bytes outside the surface\n", new_stray);
    }
    vtpv_lavc_close_decoder(d);
    vtpv_lavc_close(v);
    stream_free(&s);
    if (g_fail) { printf("FAILED: %d of %d checks\n", g_fail, g_checks); return 1; }
    printf("OK: %d land-bounds checks passed\n", g_checks);
    return 0;
}
