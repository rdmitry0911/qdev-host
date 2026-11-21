/* vt_land.h - where a decoded picture may land in a guest-lent decoder-output
 * surface (apple-videotoolbox device), natively tested in test_vt_land.c.
 *
 * GVA-LAND-BOUNDS (warm-4080 guest kernel panic 2026-11-21 ~16:55Z, "element
 * modified after free" over pixel bytes): the picture's size comes from the
 * stream's SPS, the lent surface's from the guest's format description (the
 * Dpba Width/Height of dsta). A guest description may give another size than its
 * stream's; a picture LARGER than its surface was written whole, past the surface
 * into the next guest pages (kernel memory) on the contiguous-surface path.
 * Reference: Apple's host makes the buffers the description's size and writes the
 * picture at their top left without scaling (measured, reims docs/video/devices.md
 * @3b3a5a7f^ "A format description of another size"); Apple never runs a
 * description smaller than its stream (its own host panicked on that write). Here
 * the picture lands at the top left and only what fits in each plane is written:
 * never a byte outside the lent surface's planes or its pages.
 */
#ifndef VT_LAND_H
#define VT_LAND_H

#include <stddef.h>
#include <stdint.h>
#include "vtpv_proto.h"

/* The lent surface as the guest described it (mapper bridge = the guest's own
 * IOSurface descriptor). Extents 0 = unknown (then only `total` bounds a plane). */
typedef struct vt_land_geom {
    size_t   total;              /* bytes of the surface the host may touch           */
    size_t   y_off, y_stride;    /* luma plane: byte offset, bytes per row            */
    uint32_t y_w, y_h;           /* luma plane: width (samples), height (rows)        */
    size_t   uv_off, uv_stride;  /* CbCr plane: byte offset, bytes per row            */
    uint32_t uv_w, uv_h;         /* CbCr plane: width (Cb/Cr pairs), height (rows)    */
} vt_land_geom;

/* What lands: rows x bytes of each plane, from the picture's top left. */
typedef struct vt_land_rows {
    size_t y_rows, y_bytes, uv_rows, uv_bytes;
    int    cropped;              /* the picture did not fit whole                     */
} vt_land_rows;

/* Fit a fw x fh 4:2:0 biplanar picture (bpp bytes a sample: 1 NV12, 2 P010) into g.
 * 0 and *out = the rows to write (every row inside its plane's extents and inside
 * total), or -1 when not even one luma row fits (broken geometry: nothing lands). */
int vt_land_fit(const vt_land_geom *g, uint32_t fw, uint32_t fh, int bpp, vt_land_rows *out);

/* Fit, then write the picture's planes (y / uv, src strides ys / uvs) into the
 * surface pages resolved by vt_scatter_resolve (ptrs / run / page_size). Returns
 * 0, -1 nothing fits, or a VT_SCATTER_ERR_* code (nothing written then). *out
 * (may be NULL) gets what landed. */
int vt_land_planes(void *const *ptrs, const size_t *run, size_t page_size, const vt_land_geom *g,
                   const uint8_t *y, size_t ys, const uint8_t *uv, size_t uvs,
                   uint32_t fw, uint32_t fh, int bpp, vt_land_rows *out);

/* Bytes per sample for a picture of `frame_pixfmt` (VTPV_PIXFMT_*) landing in a lent
 * surface of CoreVideo format `surface_fmt` (VTPV_CV_*, the guest descriptor's pixel
 * format; 0 = unknown, the legacy no-bridge path, taken as '420v'): 1 for NV12 into
 * '420v'/'420f', 2 for P010 into 'x420'/'xf20' (each 10-bit sample in the high bits
 * of a little-endian 16-bit word), -1 for any other pair (8-bit into a 10-bit pool or
 * the reverse, or another format): the frame then fails instead of landing in a
 * layout the guest reads differently. Reference: Apple's host asks for the pool by
 * the stream's bit depth and range ('420v'/'420f' 8-bit, 'x420'/'xf20' 10-bit;
 * reims docs/video/devices.md @3b3a5a7f^ "HEVC"); the guest descriptor stores the
 * surface's getPixelFormat() verbatim (populate_descriptor @0x8610, OMAP-X420-01). */
static inline int vt_land_bpp(uint32_t frame_pixfmt, uint32_t surface_fmt)
{
    if (frame_pixfmt == VTPV_PIXFMT_NV12)
        return (surface_fmt == 0 || surface_fmt == VTPV_CV_420V || surface_fmt == VTPV_CV_420F) ? 1 : -1;
    if (frame_pixfmt == VTPV_PIXFMT_P010)
        return (surface_fmt == VTPV_CV_X420 || surface_fmt == VTPV_CV_XF20) ? 2 : -1;
    return -1;
}

/* How a lent buffer's mapping id resolves. */
enum vt_resolve_route {
    VT_ROUTE_DROP   = 0, /* no usable surface: the frame is not landed             */
    VT_ROUTE_BRIDGE = 1, /* the uniq-vgpu mapper bridge resolved the id            */
    VT_ROUTE_GPA    = 2, /* legacy bring-up: the id is a guest-physical address    */
};
/* bridge_configured = the launcher named a bridge (PVG_MAPPER_BRIDGE_SHM): then a
 * mapping id is an opaque mapper token and a MISS drops the frame - it must never
 * be taken as a guest-physical address (that wrote frames at guest-physical ==
 * mapping id). The GPA shortcut exists only for runs without a bridge. */
static inline enum vt_resolve_route vt_resolve_route_for(uint64_t mapping_id, int bridge_configured,
                                                         int bridge_hit)
{
    if (mapping_id == 0) return VT_ROUTE_DROP;
    if (bridge_hit) return VT_ROUTE_BRIDGE;
    return bridge_configured ? VT_ROUTE_DROP : VT_ROUTE_GPA;
}

#endif /* VT_LAND_H */
