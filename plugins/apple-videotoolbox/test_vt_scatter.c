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

int main(void)
{
    test_write_lands_each_byte_at_its_scattered_address();
    test_gather_is_the_inverse();
    test_roundtrip_preserves_untouched_bytes();
    test_failures_are_all_or_nothing();
    if (g_fails) { printf("FAILED: %d of %d check(s)\n", g_fails, g_checks); return 1; }
    printf("OK: all vt_scatter checks passed (%d)\n", g_checks);
    return 0;
}
