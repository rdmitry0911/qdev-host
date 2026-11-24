/* scanout-flip-test - unit cases of scanout-flip.h (IDLE-PATH-1a; first failing on the stub = today's behaviour).
 *   F-01 identical scanout -> SAME
 *   F-02 only the offset differs (same fd, geometry, format, enabled) -> FLIP            [RED on the stub: MODE]
 *   F-03 fd / width / height / stride / format / enable differ -> MODE
 *   F-04 slots A B A B A through the map cache: 2 maps, 0 unmaps, stable pointers        [RED on the stub: 5 maps, 4 unmaps]
 *   F-05 after a MODE clear the cache is empty: the next get maps again, every slot was unmapped once
 * Reference: QEMU apple-gfx.m (the PVG host the uniq-vgpu plugin forks) sets the console surface once per mode; new frames update it.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scanout-flip.h"

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t arena[3][4096];
static int maps, unmaps;
static void *fake_map(void *ud, int fd, size_t off, size_t len)
{
    (void)ud; (void)fd; (void)len;
    maps++;
    return arena[(off / 8294400u) % 3]; /* one distinct buffer per 1080p slot */
}
static void fake_unmap(void *ud, void *base, size_t len) { (void)ud; (void)base; (void)len; unmaps++; }

int main(void)
{
    const qdp_scanout a = { true, 7, 0, 1920, 1080, 7680, 0x20020888 };
    qdp_scanout b = a; b.offset = 8294400; /* the next slot: 1080 x 7680 */
    CHECK(qdp_scanout_classify(&a, &a) == QDP_SC_SAME, "F-01 identical -> SAME");
    CHECK(qdp_scanout_classify(&a, &b) == QDP_SC_FLIP, "F-02 offset only -> FLIP (got %d)", qdp_scanout_classify(&a, &b));
    qdp_scanout c;
    c = a; c.fd = 8;            CHECK(qdp_scanout_classify(&a, &c) == QDP_SC_MODE, "F-03 fd -> MODE");
    c = a; c.width = 1280;      CHECK(qdp_scanout_classify(&a, &c) == QDP_SC_MODE, "F-03 width -> MODE");
    c = a; c.height = 720;      CHECK(qdp_scanout_classify(&a, &c) == QDP_SC_MODE, "F-03 height -> MODE");
    c = a; c.stride = 8192;     CHECK(qdp_scanout_classify(&a, &c) == QDP_SC_MODE, "F-03 stride -> MODE");
    c = a; c.pixman_format = 1; CHECK(qdp_scanout_classify(&a, &c) == QDP_SC_MODE, "F-03 format -> MODE");
    c = a; c.enabled = false;   CHECK(qdp_scanout_classify(&a, &c) == QDP_SC_MODE, "F-03 disable -> MODE");
    c = b; c.width = 1280;      CHECK(qdp_scanout_classify(&a, &c) == QDP_SC_MODE, "F-03 offset + width -> MODE");

    qdp_scanout_maps m;
    memset(&m, 0, sizeof m);
    m.map = fake_map; m.unmap = fake_unmap; m.page = 4096;
    maps = unmaps = 0;
    uint8_t *pa = qdp_scanout_maps_get(&m, &a), *pb = qdp_scanout_maps_get(&m, &b);
    uint8_t *pa2 = qdp_scanout_maps_get(&m, &a), *pb2 = qdp_scanout_maps_get(&m, &b), *pa3 = qdp_scanout_maps_get(&m, &a);
    CHECK(pa && pb && pa != pb, "F-04 two distinct slot pointers");
    CHECK(maps == 2 && unmaps == 0, "F-04 A B A B A maps each slot once and unmaps nothing (maps %d unmaps %d)", maps, unmaps);
    CHECK(pa2 == pa && pa3 == pa && pb2 == pb, "F-04 stable pointers per slot");
    qdp_scanout_maps_clear(&m);
    CHECK(unmaps == 2, "F-05 clear unmaps both cached slots once (unmaps %d)", unmaps);
    const int before = maps;
    CHECK(qdp_scanout_maps_get(&m, &a) != NULL && maps == before + 1, "F-05 the next get maps again");
    qdp_scanout_maps_clear(&m);
    /* F-06 (DISPLAY lane): more slots than entries -> round-robin replacement, the replaced slot is unmapped once and mapped
     * again when it comes back; nothing leaks at the clear */
    maps = unmaps = 0;
    qdp_scanout s5[5];
    for (int i = 0; i < 5; i++) { s5[i] = a; s5[i].offset = (uint32_t)i * 8294400u; }
    for (int i = 0; i < 5; i++) CHECK(qdp_scanout_maps_get(&m, &s5[i]) != NULL, "F-06 slot %d mapped", i);
    CHECK(maps == 5 && unmaps == 1, "F-06 five slots through four entries: 5 maps, 1 unmap (maps %d unmaps %d)", maps, unmaps);
    CHECK(qdp_scanout_maps_get(&m, &s5[0]) != NULL && maps == 6 && unmaps == 2, "F-06 the replaced slot maps again (maps %d unmaps %d)",
          maps, unmaps);
    qdp_scanout_maps_clear(&m);
    CHECK(unmaps == 6 && maps == 6, "F-06 clear: every mapping unmapped exactly once (maps %d unmaps %d)", maps, unmaps);
    printf("scanout-flip-test: %d checks, %d failures\nRESULT %s\n", checks, failures, failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
