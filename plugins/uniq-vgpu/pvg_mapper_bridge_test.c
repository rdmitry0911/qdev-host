/* pvg_mapper_bridge_test.c — off-VM unit test for the uniq-vgpu<->apple-videotoolbox bridge.
 *
 * Operates on a plain heap buffer standing in for the shared-memory region: publishes a surface as
 * uniq-vgpu's mapper ring would, then resolves it as apple-videotoolbox's resolve_buffer would,
 * checking the authoritative layout and the gpa pages survive the round trip (contiguous +
 * scattered), plus retract and miss.
 *
 * Build + run:
 *   gcc -std=c11 -O1 -Wall -Wextra -o /tmp/bridge_test pvg_mapper_bridge_test.c  (run /tmp/bridge_test)
 */
#include "pvg_mapper_bridge.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
    return 1; } } while (0)

static int test_contiguous_round_trip(pvg_mapper_bridge *b) {
    /* a 2-page 420v surface: Y plane then interleaved UV, contiguous gpa */
    uint64_t pages[2] = { 0x200000, 0x201000 };
    pvg_bridge_publish_info info;
    memset(&info, 0, sizeof info);
    info.pixel_format = 0x34323076; /* '420v' */
    info.width = 64; info.height = 64;
    info.plane_count = 2;
    info.alloc_size = 2 * 4096;
    info.planes[0].offset = 0;    info.planes[0].bytes_per_row = 64; info.planes[0].size = 64 * 64;
    info.planes[1].offset = 4096; info.planes[1].bytes_per_row = 64; info.planes[1].size = 64 * 32;
    info.page_count = 2;
    info.gpa_pages = pages;
    info.page_size = 4096;
    CHECK(pvg_mapper_bridge_publish(b, 7, &info) == 0);

    pvg_bridge_resolved r;
    CHECK(pvg_mapper_bridge_lookup(b, 7, &r) == 0);
    CHECK(r.mapping_id == 7);
    CHECK(r.pixel_format == 0x34323076);
    CHECK(r.width == 64 && r.height == 64);
    CHECK(r.plane_count == 2);
    CHECK(r.planes[0].bytes_per_row == 64);     /* authoritative stride (not a guess) */
    CHECK(r.planes[1].offset == 4096);
    CHECK(r.page_count == 2);
    CHECK(r.contiguous == 1);                   /* fast path: vt_map(gpa_base, size)   */
    CHECK(r.gpa_base == 0x200000);
    CHECK(r.gpa_pages[1] == 0x201000);
    printf("ok  contiguous publish -> resolve (authoritative layout)\n");
    return 0;
}

static int test_scattered_and_retract(pvg_mapper_bridge *b) {
    uint64_t pages[3] = { 0x400000, 0x409000, 0x402000 }; /* non-contiguous */
    pvg_bridge_publish_info info;
    memset(&info, 0, sizeof info);
    info.pixel_format = 0x34323076;
    info.width = 96; info.height = 32; info.plane_count = 2;
    info.alloc_size = 3 * 4096;
    info.planes[0].bytes_per_row = 96;
    info.page_count = 3; info.gpa_pages = pages; info.page_size = 4096;
    CHECK(pvg_mapper_bridge_publish(b, 42, &info) == 0);

    pvg_bridge_resolved r;
    CHECK(pvg_mapper_bridge_lookup(b, 42, &r) == 0);
    CHECK(r.contiguous == 0);                   /* scatter path: walk gpa_pages[]       */
    CHECK(r.page_count == 3);
    CHECK(r.gpa_pages[0] == 0x400000 && r.gpa_pages[1] == 0x409000 && r.gpa_pages[2] == 0x402000);

    /* the first surface is still resolvable alongside */
    CHECK(pvg_mapper_bridge_lookup(b, 7, &r) == 0);

    /* retract 42 -> miss; 7 still there */
    pvg_mapper_bridge_retract(b, 42);
    CHECK(pvg_mapper_bridge_lookup(b, 42, &r) == -1);
    CHECK(pvg_mapper_bridge_lookup(b, 7, &r) == 0);
    printf("ok  scattered publish, coexistence, retract -> miss\n");
    return 0;
}

static int test_misses_and_limits(pvg_mapper_bridge *b) {
    pvg_bridge_resolved r;
    CHECK(pvg_mapper_bridge_lookup(b, 0, &r) == -1);       /* id 0 never resolves (honest drop) */
    CHECK(pvg_mapper_bridge_lookup(b, 9999, &r) == -1);    /* unmapped id */

    pvg_bridge_publish_info info; memset(&info, 0, sizeof info);
    uint64_t one = 0x500000; info.gpa_pages = &one; info.page_count = 1; info.page_size = 4096;
    info.plane_count = 1; info.planes[0].bytes_per_row = 4;
    CHECK(pvg_mapper_bridge_publish(b, 0, &info) == -1);   /* publishing id 0 refused */

    info.page_count = PVG_BRIDGE_MAX_PAGES + 1;            /* too many pages for a slot */
    CHECK(pvg_mapper_bridge_publish(b, 5, &info) == -1);
    printf("ok  id-0 and unmapped miss, publish limits\n");
    return 0;
}

int main(void) {
    pvg_mapper_bridge *b = malloc(pvg_mapper_bridge_bytes());
    if (!b) return 2;
    pvg_mapper_bridge_init(b, 4096);
    int rc = 0;
    rc |= test_contiguous_round_trip(b);
    rc |= test_scattered_and_retract(b);
    rc |= test_misses_and_limits(b);
    free(b);
    if (rc == 0) printf("ALL BRIDGE TESTS PASSED\n");
    return rc;
}
