/* test_vt_land.c - GVA-LAND-BOUNDS reference-contract tests (vt_land.h).
 * Contract (reference: Apple's host writes the picture at the buffer's top left,
 * unscaled, buffers of the description's size - reims docs/video/devices.md
 * @3b3a5a7f^ "A format description of another size"; the guest's surface
 * descriptor gives each plane's offset, bytes per row and extent): a picture
 * lands top-left, only what fits each plane, never a byte outside the surface.
 * Oracle: computed here from the fixture geometry. The 'guest RAM' is a BOUNDARY
 * DOUBLE (host memory with guard bytes), not real DMA. */
#include "vt_land.h"
#include "vt_scatter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PS 4096u
static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t g_ram[8 * PS];
static void *map_ram(void *ctx, uint64_t gpa, uint64_t len)
{ (void)ctx; return (gpa + len <= sizeof g_ram) ? g_ram + gpa : NULL; }

/* the guest's '420v' pool surface for a 16x16 description (Dpba BytesPerRowAlignment
 * 64, PlaneAlignment 256; luma at 256 as seen live): luma bpr 64 x 16 rows at 256,
 * CbCr bpr 64 x 8 rows at 1280, one 4 KiB page */
static vt_land_geom geom16(void)
{
    vt_land_geom g = { PS, 256, 64, 16, 16, 1280, 64, 8, 8 };
    return g;
}

int main(void)
{
    vt_land_rows r;
    vt_land_geom g = geom16();

    /* LAND-FIT-01: a picture of the description's size lands whole */
    CHECK(vt_land_fit(&g, 16, 16, 1, &r) == 0 && r.y_rows == 16 && r.y_bytes == 16 && r.uv_rows == 8 &&
          r.uv_bytes == 16 && !r.cropped, "16x16 into 16x16: whole");
    /* LAND-FIT-02: a smaller picture lands whole at the top left (Apple: 1280x720 under 1920x1080) */
    CHECK(vt_land_fit(&g, 10, 6, 1, &r) == 0 && r.y_rows == 6 && r.y_bytes == 10 && r.uv_rows == 3 &&
          r.uv_bytes == 10 && !r.cropped, "10x6 into 16x16: whole, top left");
    /* LAND-FIT-03: the incident class - a picture larger than its description: only what fits */
    CHECK(vt_land_fit(&g, 1920, 1080, 1, &r) == 0 && r.y_rows == 16 && r.y_bytes == 16 && r.uv_rows == 8 &&
          r.uv_bytes == 16 && r.cropped, "1920x1080 into 16x16: 16x16 top left, cropped");
    /* LAND-FIT-04: unknown extents: rows bounded by total (and stride) alone */
    {
        vt_land_geom u = { PS, 256, 64, 0, 0, 1280, 64, 0, 0 };
        CHECK(vt_land_fit(&u, 1920, 1080, 1, &r) == 0 && r.y_bytes == 64 && r.uv_bytes == 64 &&
              256 + (r.y_rows - 1) * 64 + r.y_bytes <= PS && 1280 + (r.uv_rows - 1) * 64 + r.uv_bytes <= PS,
              "unknown extents: inside total (y %zu rows, uv %zu rows)", r.y_rows, r.uv_rows);
    }
    /* LAND-FIT-05: broken geometry (plane past the surface): nothing lands */
    {
        vt_land_geom b = { PS, PS + 1, 64, 16, 16, 1280, 64, 8, 8 };
        CHECK(vt_land_fit(&b, 16, 16, 1, &r) == -1, "luma offset past the surface: refused");
    }
    /* LAND-FIT-06: odd width rounds CbCr pairs up, two bytes a sample doubles the rows */
    {
        vt_land_geom w = { 8 * PS, 0, 128, 33, 9, 2048, 128, 17, 5 };
        CHECK(vt_land_fit(&w, 33, 9, 1, &r) == 0 && r.y_bytes == 33 && r.uv_bytes == 34 && r.uv_rows == 5 && !r.cropped,
              "33x9 NV12: luma 33 B, CbCr 17 pairs = 34 B, 5 rows");
        CHECK(vt_land_fit(&w, 33, 9, 2, &r) == 0 && r.y_bytes == 66 && r.uv_bytes == 68, "33x9 at 2 B a sample");
    }

    /* LAND-WRITE-01: the production writer lands a 1920x1080 picture into the one-page
     * 16x16 surface at page 2: the 16x16 top left is the picture's, every byte of the
     * page outside the planes' rows and every other page (guards) keeps its value. */
    {
        const uint32_t fw = 1920, fh = 1080;
        uint8_t *y = malloc((size_t)fw * fh), *uv = malloc((size_t)fw * (fh / 2));
        uint64_t pages[1] = { 2 * PS };
        void *ptrs[1]; size_t run[1];
        for (size_t i = 0; i < (size_t)fw * fh; i++) y[i] = (uint8_t)(i * 7 + 1);
        for (size_t i = 0; i < (size_t)fw * (fh / 2); i++) uv[i] = (uint8_t)(i * 13 + 5);
        memset(g_ram, 0xEE, sizeof g_ram);
        CHECK(vt_scatter_resolve(ptrs, run, PS, pages, 1, PS, map_ram, NULL) == VT_SCATTER_OK, "resolve one page");
        int rc = vt_land_planes(ptrs, run, PS, &g, y, fw, uv, fw, fw, fh, 1, &r);
        CHECK(rc == 0 && r.cropped, "land 1920x1080 into 16x16: rc %d cropped %d", rc, r.cropped);
        int bad = 0, stray = 0;
        for (uint32_t row = 0; row < 16; row++)
            for (uint32_t c = 0; c < 16; c++)
                if (g_ram[2 * PS + 256 + row * 64 + c] != y[(size_t)row * fw + c]) bad++;
        for (uint32_t row = 0; row < 8; row++)
            for (uint32_t c = 0; c < 16; c++)
                if (g_ram[2 * PS + 1280 + row * 64 + c] != uv[(size_t)row * fw + c]) bad++;
        for (size_t a = 0; a < sizeof g_ram; a++) {
            int in_y = a >= 2 * PS + 256 && a < 2 * PS + 256 + 16 * 64 && (a - (2 * PS + 256)) % 64 < 16;
            int in_uv = a >= 2 * PS + 1280 && a < 2 * PS + 1280 + 8 * 64 && (a - (2 * PS + 1280)) % 64 < 16;
            if (!in_y && !in_uv && g_ram[a] != 0xEE) stray++;
        }
        CHECK(bad == 0, "top-left 16x16 differs in %d bytes", bad);
        CHECK(stray == 0, "%d bytes written outside the planes' rows (the panic class)", stray);
        free(y); free(uv);
    }

    /* LAND-FMT-01: NV12 into the 8-bit pools (and the legacy unknown format) */
    CHECK(vt_land_bpp(VTPV_PIXFMT_NV12, VTPV_CV_420V) == 1, "NV12 -> 420v");
    CHECK(vt_land_bpp(VTPV_PIXFMT_NV12, VTPV_CV_420F) == 1, "NV12 -> 420f");
    CHECK(vt_land_bpp(VTPV_PIXFMT_NV12, 0) == 1, "NV12 -> legacy unknown");
    /* LAND-FMT-02: P010 into the 10-bit pools, two bytes a sample */
    CHECK(vt_land_bpp(VTPV_PIXFMT_P010, VTPV_CV_X420) == 2, "P010 -> x420");
    CHECK(vt_land_bpp(VTPV_PIXFMT_P010, VTPV_CV_XF20) == 2, "P010 -> xf20");
    /* LAND-FMT-03: mismatches fail (no lossy or misread landing) */
    CHECK(vt_land_bpp(VTPV_PIXFMT_P010, VTPV_CV_420V) == -1, "P010 -> 420v refused");
    CHECK(vt_land_bpp(VTPV_PIXFMT_P010, VTPV_CV_420F) == -1, "P010 -> 420f refused");
    CHECK(vt_land_bpp(VTPV_PIXFMT_P010, 0) == -1, "P010 -> unknown refused");
    CHECK(vt_land_bpp(VTPV_PIXFMT_NV12, VTPV_CV_X420) == -1, "NV12 -> x420 refused");
    CHECK(vt_land_bpp(VTPV_PIXFMT_NV12, VTPV_CV_XF20) == -1, "NV12 -> xf20 refused");
    CHECK(vt_land_bpp(VTPV_PIXFMT_NV12, 0x42475241u /* 'BGRA' */) == -1, "NV12 -> BGRA refused");
    CHECK(vt_land_bpp(0, VTPV_CV_420V) == -1, "unknown frame format refused");
    /* LAND-FMT-04: a 3840x2160 'x420' surface as the guest describes it (IOSurface getters:
     * plane 0 3840x2160 rowBytes 7680, plane 1 1920x1080 pairs rowBytes 7680, 6075 pages):
     * a 4K P010 picture fits whole at two bytes a sample. */
    {
        vt_land_geom x = { 6075 * PS, 0, 7680, 3840, 2160, 7680u * 2160u, 7680, 1920, 1080 };
        CHECK(vt_land_fit(&x, 3840, 2160, 2, &r) == 0 && r.y_bytes == 7680 && r.y_rows == 2160 &&
              r.uv_bytes == 7680 && r.uv_rows == 1080 && !r.cropped, "4K P010 into 4K x420: whole");
        CHECK(vt_land_fit(&x, 3840, 2160, 1, &r) == 0 && r.y_bytes == 3840, "an NV12-sized fit is half the row");
    }

    /* LAND-ROUTE-01..04 */
    CHECK(vt_resolve_route_for(0, 1, 0) == VT_ROUTE_DROP, "id 0 drops");
    CHECK(vt_resolve_route_for(7, 1, 1) == VT_ROUTE_BRIDGE, "bridge hit");
    CHECK(vt_resolve_route_for(7, 1, 0) == VT_ROUTE_DROP, "configured bridge + miss -> drop, never a GPA");
    CHECK(vt_resolve_route_for(0x80000000ull, 0, 0) == VT_ROUTE_GPA, "no bridge configured -> legacy GPA");

    if (g_fail) { printf("FAILED: %d of %d checks\n", g_fail, g_checks); return 1; }
    printf("OK: %d vt_land checks passed\n", g_checks);
    return 0;
}
