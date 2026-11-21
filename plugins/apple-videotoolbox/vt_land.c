/* vt_land.c - see vt_land.h. libc only. */
#include "vt_land.h"
#include "vt_scatter.h"

/* rows of row_bytes at off + r*stride that end inside total: clip `rows`. */
static size_t rows_inside(size_t total, size_t off, size_t stride, size_t row_bytes, size_t rows)
{
    if (rows == 0 || row_bytes == 0) return 0;
    if (off > total || row_bytes > total - off) return 0;   /* not even the first row */
    if (stride == 0) return 1;
    size_t fit = (total - off - row_bytes) / stride + 1;
    return rows < fit ? rows : fit;
}

int vt_land_fit(const vt_land_geom *g, uint32_t fw, uint32_t fh, int bpp, vt_land_rows *out)
{
    if (!g || !out || bpp < 1 || fw == 0 || fh == 0) return -1;
    size_t ycols = fw, yrows = fh, pairs = ((size_t)fw + 1) / 2, uvrows = ((size_t)fh + 1) / 2;
    if (g->y_w && ycols > g->y_w) ycols = g->y_w;
    if (g->y_h && yrows > g->y_h) yrows = g->y_h;
    if (g->uv_w && pairs > g->uv_w) pairs = g->uv_w;
    if (g->uv_h && uvrows > g->uv_h) uvrows = g->uv_h;
    size_t ybytes = ycols * (size_t)bpp, uvbytes = pairs * 2 * (size_t)bpp;
    if (g->y_stride && ybytes > g->y_stride) ybytes = g->y_stride / (size_t)bpp * (size_t)bpp;
    if (g->uv_stride && uvbytes > g->uv_stride) uvbytes = g->uv_stride / (2 * (size_t)bpp) * 2 * (size_t)bpp;
    out->y_bytes = ybytes;
    out->y_rows = rows_inside(g->total, g->y_off, g->y_stride, ybytes, yrows);
    out->uv_bytes = uvbytes;
    out->uv_rows = rows_inside(g->total, g->uv_off, g->uv_stride, uvbytes, uvrows);
    out->cropped = out->y_rows < (size_t)fh || out->y_bytes < (size_t)fw * (size_t)bpp ||
                   out->uv_rows < ((size_t)fh + 1) / 2 || out->uv_bytes < ((size_t)fw + 1) / 2 * 2 * (size_t)bpp;
    return out->y_rows ? 0 : -1;
}

int vt_land_planes(void *const *ptrs, const size_t *run, size_t page_size, const vt_land_geom *g,
                   const uint8_t *y, size_t ys, const uint8_t *uv, size_t uvs,
                   uint32_t fw, uint32_t fh, int bpp, vt_land_rows *out)
{
    vt_land_rows r;
    if (!y) return -1;
    if (vt_land_fit(g, fw, fh, bpp, &r) != 0) return -1;
    if (!uv) r.uv_rows = 0;
    int rc = vt_scatter_write_rows(ptrs, run, g->total, page_size, g->y_off, g->y_stride, y, ys,
                                   r.y_bytes, r.y_rows);
    if (rc == VT_SCATTER_OK && r.uv_rows)
        rc = vt_scatter_write_rows(ptrs, run, g->total, page_size, g->uv_off, g->uv_stride, uv, uvs,
                                   r.uv_bytes, r.uv_rows);
    if (out) *out = r;
    return rc;
}
