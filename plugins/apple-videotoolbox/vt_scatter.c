/* vt_scatter.c - see vt_scatter.h. Kernel-free, libc only. */
#include "vt_scatter.h"
#include <string.h>

static int vt_scatter_check(size_t total, const uint64_t *gpa_pages, uint32_t npages,
                            size_t page_size, vt_scatter_map_fn map, void *ctx,
                            size_t *pages_needed)
{
    if (!gpa_pages || !map || page_size == 0 || total == 0) return VT_SCATTER_ERR_INVAL;
    size_t need = (total + page_size - 1) / page_size;
    if (need > npages) return VT_SCATTER_ERR_RANGE;
    for (size_t i = 0; i < need; ++i) {
        size_t len = (i + 1 == need) ? (total - i * page_size) : page_size;
        if (!map(ctx, gpa_pages[i], len)) return VT_SCATTER_ERR_UNMAPPED;
    }
    *pages_needed = need;
    return VT_SCATTER_OK;
}

int vt_scatter_gather(uint8_t *dst, size_t total, const uint64_t *gpa_pages,
                      uint32_t npages, size_t page_size, vt_scatter_map_fn map, void *ctx)
{
    size_t need = 0;
    if (!dst) return VT_SCATTER_ERR_INVAL;
    int rc = vt_scatter_check(total, gpa_pages, npages, page_size, map, ctx, &need);
    if (rc != VT_SCATTER_OK) return rc;
    for (size_t i = 0; i < need; ++i) {
        size_t len = (i + 1 == need) ? (total - i * page_size) : page_size;
        memcpy(dst + i * page_size, map(ctx, gpa_pages[i], len), len);
    }
    return VT_SCATTER_OK;
}

int vt_scatter_write(const uint8_t *src, size_t total, const uint64_t *gpa_pages,
                     uint32_t npages, size_t page_size, vt_scatter_map_fn map, void *ctx)
{
    size_t need = 0;
    if (!src) return VT_SCATTER_ERR_INVAL;
    int rc = vt_scatter_check(total, gpa_pages, npages, page_size, map, ctx, &need);
    if (rc != VT_SCATTER_OK) return rc;
    for (size_t i = 0; i < need; ++i) {
        size_t len = (i + 1 == need) ? (total - i * page_size) : page_size;
        memcpy(map(ctx, gpa_pages[i], len), src + i * page_size, len);
    }
    return VT_SCATTER_OK;
}
