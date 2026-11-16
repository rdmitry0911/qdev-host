/*
 * vtpv_vaapi.h - a VA-API decode backend for the AppleGVA host device.
 *
 * AppleGVA host backend, increment 1 (lane L-M2V03, 2026-11-16).
 *
 * CLEAN-ROOM: written from the libva headers (<va/va.h>, <va/va_drm.h>,
 * <va/va_dec_hevc.h>) and the H.264/HEVC specifications. It does NOT copy
 * code from the reims reference under research/reims (GPL/LGPL).
 *
 * Implements the vtpv_decoder_backend vtable (vtpv_proto.h) over libva on a
 * Linux DRM render node (the AMD Raphael iGPU /dev/dri/renderD128 on this
 * host). Decode only in increment 1; encode (VAEntrypointEncSlice is present
 * on this GPU) and NVDEC/VideoToolbox backends are later increments.
 */
#ifndef VTPV_VAAPI_H
#define VTPV_VAAPI_H

#include "vtpv_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vtpv_vaapi vtpv_vaapi;

/*
 * Open a VA-API backend on the given DRM render node (e.g.
 * "/dev/dri/renderD128"). On success *out_be points at a vtable whose ctx is
 * the returned handle; pass both to vtpv_proto_create. Returns 0, or a
 * negative errno; *errbuf (if non-NULL) gets a human-readable reason.
 */
int vtpv_vaapi_open(const char *render_node, vtpv_vaapi **out,
                    const vtpv_decoder_backend **out_be, void *out_ctx_unused,
                    char *errbuf, size_t errbuf_len);

/* The backend context pointer to hand vtpv_proto_create as be_ctx. */
void *vtpv_vaapi_ctx(vtpv_vaapi *v);

void vtpv_vaapi_close(vtpv_vaapi *v);

/*
 * Test/helper: copy the luma (Y) plane of a decoded surface into dst (row by
 * row, dst_stride bytes per row, 'height' rows). Returns 0 or <0. Used by the
 * unit test to verify a decoded frame's pixels. surface_id is the id a
 * vtpv_decoded_frame reported.
 */
int vtpv_vaapi_read_luma(vtpv_vaapi *v, uint32_t surface_id,
                         uint8_t *dst, size_t dst_stride,
                         uint32_t width, uint32_t height);

#ifdef __cplusplus
}
#endif

#endif /* VTPV_VAAPI_H */
