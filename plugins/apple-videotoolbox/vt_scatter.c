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

int vt_scatter_resolve(void **ptrs, size_t *run, size_t total, const uint64_t *gpa_pages,
                       uint32_t npages, size_t page_size, vt_scatter_map_fn map, void *ctx)
{
    if (!ptrs || !run || !gpa_pages || !map || page_size == 0 || total == 0) return VT_SCATTER_ERR_INVAL;
    size_t need = (total + page_size - 1) / page_size;
    if (need > npages) return VT_SCATTER_ERR_RANGE;
    for (size_t i = 0; i < need; ++i) {
        size_t len = (i + 1 == need) ? (total - i * page_size) : page_size;
        if (!(ptrs[i] = map(ctx, gpa_pages[i], len))) return VT_SCATTER_ERR_UNMAPPED;
    }
    for (size_t i = need; i-- > 0;) {
        size_t len = (i + 1 == need) ? (total - i * page_size) : page_size;
        run[i] = len;
        if (i + 1 < need && (uint8_t *)ptrs[i] + page_size == (uint8_t *)ptrs[i + 1]) run[i] += run[i + 1];
    }
    return VT_SCATTER_OK;
}

int vt_scatter_write_rows(void *const *ptrs, const size_t *run, size_t total, size_t page_size,
                          size_t dst_off, size_t dst_stride, const uint8_t *src, size_t src_stride,
                          size_t row_bytes, size_t rows)
{
    if (!ptrs || !run || !src || page_size == 0) return VT_SCATTER_ERR_INVAL;
    if (rows == 0 || row_bytes == 0) return VT_SCATTER_OK;
    /* every row inside the allocation: the last one ends at dst_off + (rows-1)*stride + row_bytes */
    size_t last, end;
    if (__builtin_mul_overflow(rows - 1, dst_stride, &last) || __builtin_add_overflow(last, dst_off, &end) ||
        __builtin_add_overflow(end, row_bytes, &end) || end > total) return VT_SCATTER_ERR_RANGE;
    for (size_t r = 0; r < rows; ++r) {
        size_t off = dst_off + r * dst_stride, left = row_bytes;
        const uint8_t *s = src + r * src_stride;
        while (left) {
            size_t pg = off / page_size, in = off % page_size;
            size_t n = run[pg] - in;
            if (n > left) n = left;
            memcpy((uint8_t *)ptrs[pg] + in, s, n);
            s += n; off += n; left -= n;
        }
    }
    return VT_SCATTER_OK;
}
