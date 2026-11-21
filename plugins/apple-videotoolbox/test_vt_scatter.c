/* test_vt_scatter.c - reference-contract tests for the scattered-page landing
 * (vt_scatter.h). Oracle: byte X of the surface allocation is at
 * gpa_pages[X/page_size] + X%page_size (pvg_mapper_bridge.h descriptor contract);
 * computed independently here from the fixture, not by calling the code under test.
 * The 'guest RAM' below is a BOUNDARY DOUBLE (plain host memory), not real DMA. */
#include "vt_scatter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PS 4096u
#define RAM_PAGES 64u
static uint8_t g_ram[RAM_PAGES * PS];
static int g_fails, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fails++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static int g_unmapped_gpa_valid; static uint64_t g_unmapped_gpa;
static void *map_ram(void *ctx, uint64_t gpa, uint64_t len)
{
    (void)ctx;
    if (g_unmapped_gpa_valid && gpa == g_unmapped_gpa) return NULL;
    if (gpa + len > sizeof g_ram || gpa + len < gpa) return NULL;
    return g_ram + gpa;
}

/* a deliberately non-monotonic, non-adjacent page list (each page index used once) */
static void make_pages(uint64_t *pg, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i)
        pg[i] = (uint64_t)((i * 7u + 3u) % RAM_PAGES) * PS;
}

static void test_write_lands_each_byte_at_its_scattered_address(void)
{
    printf("-- write: byte X at gpa_pages[X/ps]+X%%ps --\n");
    uint64_t pg[41]; make_pages(pg, 41);
    size_t total = 41 * PS - 100; /* last page partial */
    uint8_t *src = malloc(total);
    for (size_t i = 0; i < total; ++i) src[i] = (uint8_t)(i * 31u + 7u);
    memset(g_ram, 0xEE, sizeof g_ram);
    CHECK(vt_scatter_write(src, total, pg, 41, PS, map_ram, NULL) == VT_SCATTER_OK, "write ok");
    int bad = 0;
    for (size_t x = 0; x < total; ++x)
        if (g_ram[pg[x / PS] + x % PS] != src[x]) { bad++; }
    CHECK(bad == 0, "%d bytes landed at the wrong scattered address", bad);
    /* bytes past `total` in the last page, and pages not in the list, are untouched */
    CHECK(g_ram[pg[40] + (total % PS)] == 0xEE, "byte past total in last page was modified");
    int stray = 0;
    for (uint32_t p = 0; p < RAM_PAGES; ++p) {
        int listed = 0;
        for (int i = 0; i < 41; ++i) if (pg[i] == (uint64_t)p * PS) listed = 1;
        if (!listed) for (uint32_t b = 0; b < PS; ++b) if (g_ram[(size_t)p * PS + b] != 0xEE) stray++;
    }
    CHECK(stray == 0, "%d bytes written to pages outside the surface list", stray);
    free(src);
}

static void test_gather_is_the_inverse(void)
{
    printf("-- gather reads the same image --\n");
    uint64_t pg[20]; make_pages(pg, 20);
    for (size_t i = 0; i < sizeof g_ram; ++i) g_ram[i] = (uint8_t)(i * 13u + 5u);
    size_t total = 20 * PS - 17;
    uint8_t *dst = calloc(1, total);
    CHECK(vt_scatter_gather(dst, total, pg, 20, PS, map_ram, NULL) == VT_SCATTER_OK, "gather ok");
    int bad = 0;
    for (size_t x = 0; x < total; ++x)
        if (dst[x] != g_ram[pg[x / PS] + x % PS]) bad++;
    CHECK(bad == 0, "%d gathered bytes differ", bad);
    free(dst);
}

static void test_roundtrip_preserves_untouched_bytes(void)
{
    printf("-- shadow compose: gather, overlay decoded bytes, write back --\n");
    uint64_t pg[8]; make_pages(pg, 8);
    for (size_t i = 0; i < sizeof g_ram; ++i) g_ram[i] = (uint8_t)(i ^ 0x5Au);
    size_t total = 8 * PS;
    uint8_t *shadow = malloc(total);
    uint8_t *orig = malloc(total);
    CHECK(vt_scatter_gather(shadow, total, pg, 8, PS, map_ram, NULL) == 0, "gather");
    memcpy(orig, shadow, total);
    memset(shadow + 1000, 0xAB, 5000); /* 'decoded plane' region spanning a page boundary */
    CHECK(vt_scatter_write(shadow, total, pg, 8, PS, map_ram, NULL) == 0, "write back");
    int bad = 0;
    for (size_t x = 0; x < total; ++x) {
        uint8_t want = (x >= 1000 && x < 6000) ? 0xAB : orig[x];
        if (g_ram[pg[x / PS] + x % PS] != want) bad++;
    }
    CHECK(bad == 0, "%d bytes wrong after compose/write-back", bad);
    free(shadow); free(orig);
}

static void test_failures_are_all_or_nothing(void)
{
    printf("-- errors: range / unmapped page / invalid args, nothing half-lands --\n");
    uint64_t pg[4]; make_pages(pg, 4);
    uint8_t src[4 * PS]; memset(src, 0x77, sizeof src);
    memset(g_ram, 0x11, sizeof g_ram);

    CHECK(vt_scatter_write(src, 4 * PS + 1, pg, 4, PS, map_ram, NULL) == VT_SCATTER_ERR_RANGE,
          "total beyond the page list must be RANGE");
    g_unmapped_gpa_valid = 1; g_unmapped_gpa = pg[2];
    CHECK(vt_scatter_write(src, 4 * PS, pg, 4, PS, map_ram, NULL) == VT_SCATTER_ERR_UNMAPPED,
          "an unmapped page must be UNMAPPED");
    g_unmapped_gpa_valid = 0;
    int touched = 0;
    for (size_t i = 0; i < sizeof g_ram; ++i) if (g_ram[i] != 0x11) touched++;
    CHECK(touched == 0, "%d bytes modified by failed writes (must be all-or-nothing)", touched);

    CHECK(vt_scatter_write(NULL, 10, pg, 4, PS, map_ram, NULL) == VT_SCATTER_ERR_INVAL, "null src");
    CHECK(vt_scatter_write(src, 0, pg, 4, PS, map_ram, NULL) == VT_SCATTER_ERR_INVAL, "zero total");
    CHECK(vt_scatter_write(src, 10, pg, 4, 0, map_ram, NULL) == VT_SCATTER_ERR_INVAL, "zero page size");
    CHECK(vt_scatter_gather(NULL, 10, pg, 4, PS, map_ram, NULL) == VT_SCATTER_ERR_INVAL, "null dst");
}

/* Direct landing (vt_scatter_resolve + vt_scatter_write_rows) == the shadow path
 * (gather, overlay the image rows, write back) for every byte of guest RAM, with
 * scattered pages, a run of host-adjacent pages, rows crossing page boundaries,
 * strides wider than the rows, and a partial last page. */
static void overlay_rows(uint8_t *shadow, size_t off, size_t dstride, const uint8_t *src,
                         size_t sstride, size_t rowb, size_t rows)
{
    for (size_t r = 0; r < rows; ++r) memcpy(shadow + off + r * dstride, src + r * sstride, rowb);
}

static void test_rows_equal_shadow_path(void)
{
    printf("-- direct rows == shadow compose (scattered + adjacent pages) --\n");
    uint64_t pg[24];
    for (uint32_t i = 0; i < 24; ++i) pg[i] = (uint64_t)((i * 7u + 3u) % RAM_PAGES) * PS;
    pg[10] = 40u * PS; pg[11] = 41u * PS; pg[12] = 42u * PS; /* a host-adjacent run (merged memcpy) */
    size_t total = 24 * PS - 333;
    const size_t ystride = 3072 + 64, w = 3000, h = 20, uvoff = 64000, uvstride = 3200, uvrows = 10;
    uint8_t *src = malloc(ystride * h), *uvsrc = malloc(uvstride * uvrows);
    for (size_t i = 0; i < ystride * h; ++i) src[i] = (uint8_t)(i * 37u + 1u);
    for (size_t i = 0; i < uvstride * uvrows; ++i) uvsrc[i] = (uint8_t)(i * 11u + 9u);
    /* reference: shadow path */
    for (size_t i = 0; i < sizeof g_ram; ++i) g_ram[i] = (uint8_t)(i * 3u + 17u);
    uint8_t *shadow = malloc(total);
    CHECK(vt_scatter_gather(shadow, total, pg, 24, PS, map_ram, NULL) == 0, "gather");
    overlay_rows(shadow, 256, ystride, src, ystride, w, h);
    overlay_rows(shadow, uvoff, uvstride, uvsrc, uvstride, w, uvrows);
    CHECK(vt_scatter_write(shadow, total, pg, 24, PS, map_ram, NULL) == 0, "write back");
    uint8_t *want = malloc(sizeof g_ram); memcpy(want, g_ram, sizeof g_ram);
    /* direct path on the same initial RAM */
    for (size_t i = 0; i < sizeof g_ram; ++i) g_ram[i] = (uint8_t)(i * 3u + 17u);
    void *ptrs[24]; size_t run[24];
    CHECK(vt_scatter_resolve(ptrs, run, total, pg, 24, PS, map_ram, NULL) == 0, "resolve");
    CHECK(run[10] == 3 * PS && run[11] == 2 * PS && run[12] == PS && run[9] == PS, "adjacent run merged (%zu %zu)", run[10], run[9]);
    CHECK(vt_scatter_write_rows(ptrs, run, total, PS, 256, ystride, src, ystride, w, h) == 0, "luma rows");
    CHECK(vt_scatter_write_rows(ptrs, run, total, PS, uvoff, uvstride, uvsrc, uvstride, w, uvrows) == 0, "chroma rows");
    int bad = 0;
    for (size_t i = 0; i < sizeof g_ram; ++i) if (g_ram[i] != want[i]) bad++;
    CHECK(bad == 0, "%d guest bytes differ between direct rows and the shadow path", bad);
    /* a row past the allocation is refused and writes nothing */
    memcpy(want, g_ram, sizeof g_ram);
    CHECK(vt_scatter_write_rows(ptrs, run, total, PS, total - 100, 0, src, 0, 101, 1) == VT_SCATTER_ERR_RANGE, "row past total");
    CHECK(vt_scatter_write_rows(ptrs, run, total, PS, 0, (size_t)-1, src, 0, 10, 3) == VT_SCATTER_ERR_RANGE, "stride overflow");
    bad = 0; for (size_t i = 0; i < sizeof g_ram; ++i) if (g_ram[i] != want[i]) bad++;
    CHECK(bad == 0, "%d bytes written by refused rows", bad);
    /* an unmapped page fails resolve (nothing written by the caller, which writes only after resolve) */
    g_unmapped_gpa_valid = 1; g_unmapped_gpa = pg[17];
    CHECK(vt_scatter_resolve(ptrs, run, total, pg, 24, PS, map_ram, NULL) == VT_SCATTER_ERR_UNMAPPED, "unmapped page");
    g_unmapped_gpa_valid = 0;
    CHECK(vt_scatter_resolve(ptrs, run, 25 * PS, pg, 24, PS, map_ram, NULL) == VT_SCATTER_ERR_RANGE, "total beyond pages");
    free(src); free(uvsrc); free(shadow); free(want);
}

int main(void)
{
    test_write_lands_each_byte_at_its_scattered_address();
    test_gather_is_the_inverse();
    test_roundtrip_preserves_untouched_bytes();
    test_failures_are_all_or_nothing();
    test_rows_equal_shadow_path();
    if (g_fails) { printf("FAILED: %d of %d check(s)\n", g_fails, g_checks); return 1; }
    printf("OK: all vt_scatter checks passed (%d)\n", g_checks);
    return 0;
}
