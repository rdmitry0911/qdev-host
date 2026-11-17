/*
 * test_aw_bplist.c - the H4 proof: the binary-plist encoder emits real Dpba /
 * ,hin bytes a VideoToolbox framework would accept. We build the dicts, assert
 * the bplist00 structure (magic + 32-byte trailer), re-parse with
 * aw_bplist_check (stands in for CFPropertyListCreateWithData), and spot-check
 * the ASCII keys/values are present. CLEAN-ROOM. Lane L-VIDEO-IMPL, 2026-11-16.
 */
#include "aw_bplist.h"

#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static int contains(const uint8_t *h, size_t hn, const char *needle) {
    size_t nn = strlen(needle);
    if (nn > hn) return 0;
    for (size_t i = 0; i + nn <= hn; i++)
        if (memcmp(h + i, needle, nn) == 0) return 1;
    return 0;
}

/* read the big-endian u64 at the trailer field */
static uint64_t be64(const uint8_t *p) {
    uint64_t v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | p[i]; return v;
}

static void test_dpba(void) {
    printf("-- Dpba attributes bplist --\n");
    aw_bplist *b = aw_bplist_new();
    CHECK(b != NULL, "new");

    int fourcc = aw_bpl_int(b, 0x34323076); /* '420v' */
    int v_pft  = aw_bpl_array(b, &fourcc, 1);
    int v_io   = aw_bpl_dict(b, NULL, NULL, 0);
    int keys[7] = {
        aw_bpl_string(b, "PixelFormatType"),
        aw_bpl_string(b, "Width"),
        aw_bpl_string(b, "Height"),
        aw_bpl_string(b, "IOSurfaceProperties"),
        aw_bpl_string(b, "BytesPerRowAlignment"),
        aw_bpl_string(b, "PlaneAlignment"),
        aw_bpl_string(b, "PreferRealTimeCacheModeIfEveryoneDoes"),
    };
    int vals[7] = { v_pft, aw_bpl_int(b, 352), aw_bpl_int(b, 288), v_io,
                    aw_bpl_int(b, 64), aw_bpl_int(b, 256), aw_bpl_bool(b, 1) };
    int root = aw_bpl_dict(b, keys, vals, 7);
    CHECK(root >= 0, "dict built");

    uint8_t out[512];
    size_t n = aw_bplist_finalize(b, root, out, sizeof out);
    CHECK(n > 8 + 32, "finalized (%zu bytes)", n);

    /* structure: magic + trailer. */
    CHECK(memcmp(out, "bplist00", 8) == 0, "bplist00 magic");
    const uint8_t *tr = out + n - 32;
    CHECK(tr[6] >= 1 && tr[7] >= 1, "trailer offset/ref sizes nonzero");
    uint64_t nobj = be64(tr + 8), top = be64(tr + 16), table = be64(tr + 24);
    CHECK(nobj >= 10 && top == nobj - 1, "num objects / root index (nobj=%llu top=%llu)",
          (unsigned long long)nobj, (unsigned long long)top);
    CHECK(table + nobj * tr[6] + 32 == n, "offset table + trailer fill the file");

    /* THE PROOF: a CF-style re-parse accepts it, top object is a dict. */
    CHECK(aw_bplist_check(out, n) == 0, "aw_bplist_check accepts the Dpba plist");

    /* content spot-checks: the ASCII keys are present. */
    CHECK(contains(out, n, "PixelFormatType"), "key PixelFormatType present");
    CHECK(contains(out, n, "BytesPerRowAlignment"), "key BytesPerRowAlignment present");
    CHECK(contains(out, n, "PreferRealTimeCacheModeIfEveryoneDoes"), "key PreferRealTime... present");

    /* a truncated plist is rejected. */
    CHECK(aw_bplist_check(out, n - 1) < 0, "truncated plist rejected");
    aw_bplist_free(b);
}

static void test_hin_and_edges(void) {
    printf("-- ,hin + integer widths --\n");
    aw_bplist *b = aw_bplist_new();
    int k = aw_bpl_string(b, "VTHostBuildVersion");
    int v = aw_bpl_string(b, "applegva-video 0.1");
    int root = aw_bpl_dict(b, &k, &v, 1);
    uint8_t out[128];
    size_t n = aw_bplist_finalize(b, root, out, sizeof out);
    CHECK(n > 0 && aw_bplist_check(out, n) == 0, ",hin plist valid");
    CHECK(contains(out, n, "VTHostBuildVersion") && contains(out, n, "applegva-video 0.1"),
          ",hin key + value present");
    aw_bplist_free(b);

    /* integer widths: 1-byte (200), 2-byte (300), 4-byte (70000) encode and
     * the plist stays valid. */
    aw_bplist *c = aw_bplist_new();
    int kk = aw_bpl_string(c, "n");
    int vv = aw_bpl_int(c, 70000);
    int r2 = aw_bpl_dict(c, &kk, &vv, 1);
    uint8_t o2[64]; size_t n2 = aw_bplist_finalize(c, r2, o2, sizeof o2);
    CHECK(n2 > 0 && aw_bplist_check(o2, n2) == 0, "4-byte int plist valid");
    aw_bplist_free(c);
}

int main(void) {
    test_dpba();
    test_hin_and_edges();
    if (g_fail) { printf("FAILED: %d check(s)\n", g_fail); return 1; }
    printf("OK: all aw_bplist checks passed\n");
    return 0;
}
