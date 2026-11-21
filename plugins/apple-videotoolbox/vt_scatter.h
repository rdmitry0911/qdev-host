/* vt_scatter.h - page-aware gather/scatter between a host image and a guest
 * surface whose backing pages are NOT physically contiguous.
 *
 * Contract (pvg_mapper_bridge.h, the guest surface descriptor): the surface's
 * allocation is the byte sequence page0|page1|...|page(n-1), page i living at
 * guest-physical gpa_pages[i] (page_size bytes each); byte X of the allocation is
 * at gpa_pages[X / page_size] + (X % page_size). The decoded NV12 planes are laid
 * out inside that allocation at the surface descriptor's offsets/strides, so the
 * host composes the frame in a contiguous shadow of the allocation and moves it
 * to/from the scattered pages here. All-or-nothing: every needed page is resolved
 * before any byte moves, so a partial/unmapped page never half-lands a frame. */
#ifndef VT_SCATTER_H
#define VT_SCATTER_H
#include <stddef.h>
#include <stdint.h>

enum {
    VT_SCATTER_OK           = 0,
    VT_SCATTER_ERR_INVAL    = -1, /* null arg / zero page size / zero total          */
    VT_SCATTER_ERR_RANGE    = -2, /* total exceeds npages * page_size                */
    VT_SCATTER_ERR_UNMAPPED = -3, /* a needed page is not in the guest DMA regions   */
};

/* Resolve `len` bytes at guest-physical `gpa` to a host pointer, or NULL. */
typedef void *(*vt_scatter_map_fn)(void *ctx, uint64_t gpa, uint64_t len);

/* shadow <- guest pages (the first `total` bytes of the allocation). */
int vt_scatter_gather(uint8_t *dst, size_t total, const uint64_t *gpa_pages,
                      uint32_t npages, size_t page_size,
                      vt_scatter_map_fn map, void *ctx);
/* guest pages <- shadow (the first `total` bytes of the allocation). */
int vt_scatter_write(const uint8_t *src, size_t total, const uint64_t *gpa_pages,
                     uint32_t npages, size_t page_size,
                     vt_scatter_map_fn map, void *ctx);

/* Direct landing (no shadow): resolve the host pointers of the pages covering the
 * first `total` bytes of the allocation once (all-or-nothing, like the calls above:
 * nothing is written when a needed page is unmapped), then write image rows
 * straight into the guest pages. Bytes the rows do not cover keep the guest's
 * content, exactly as the shadow path (gather + compose + write back) leaves them.
 * `ptrs` / `run` hold `need` = ceil(total / page_size) entries; run[i] = bytes
 * that are contiguous in host memory from the start of page i (adjacent pages
 * merge into one memcpy). */
int vt_scatter_resolve(void **ptrs, size_t *run, size_t total, const uint64_t *gpa_pages,
                       uint32_t npages, size_t page_size, vt_scatter_map_fn map, void *ctx);
/* `rows` rows of `row_bytes` from src (src_stride apart) to allocation offset
 * dst_off + r * dst_stride. The caller keeps every row inside `total` (checked:
 * VT_SCATTER_ERR_RANGE, nothing written). */
int vt_scatter_write_rows(void *const *ptrs, const size_t *run, size_t total, size_t page_size,
                          size_t dst_off, size_t dst_stride, const uint8_t *src, size_t src_stride,
                          size_t row_bytes, size_t rows);
#endif
