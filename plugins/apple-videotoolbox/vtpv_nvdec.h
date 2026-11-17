/*
 * vtpv_nvdec.h - an NVDEC (NVIDIA) decode backend for the AppleGVA host device.
 *
 * AppleGVA host backend, NVDEC increment (lane L-VIDEO-NVDEC, 2026-11-17).
 *
 * Direct reference-port of the reims NVDEC decode path
 * (research/reims/reims-vgpu/crates/reims-vgpu-video-nvidia/src/: nvdec/ffi.rs,
 * nvdec/decoder.rs, nvdec/h264.rs, cuda.rs, cuda/ffi.rs, dl.rs), adapted to this
 * plugin's C types, memory ownership and build. The CUVID/CUDA struct layouts,
 * field mapping and call order are the NVIDIA driver API's (libnvcuvid cuviddec.h,
 * libcuda cuda.h), the same contract reims drives; ported here per AGENTS.md
 * "Use recovered reference code directly when it shortens wall time".
 *
 * Implements the vtpv_decoder_backend vtable (vtpv_proto.h) over libnvcuvid +
 * libcuda, loaded at run time with dlopen (reims dl.rs), so the plugin still
 * loads on a host with no NVIDIA driver. H.264 decode is real; HEVC session
 * lifecycle is wired, its bitstream->CUVID parse is a later codec increment,
 * mirroring vtpv_vaapi.c.
 */
#ifndef VTPV_NVDEC_H
#define VTPV_NVDEC_H

#include "vtpv_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vtpv_nvdec vtpv_nvdec;

/*
 * Open an NVDEC backend on an NVIDIA GPU. `gpu` selects the GPU: NULL or ""
 * means CUDA device ordinal 0; a decimal string is a CUDA ordinal. On success
 * *out_be points at a vtable whose ctx is the returned handle; pass both to
 * vtpv_proto_create. Returns 0, or a negative errno; *errbuf (if non-NULL) gets
 * a human-readable reason (e.g. libcuda/libnvcuvid absent, no NVDEC H.264).
 */
int vtpv_nvdec_open(const char *gpu, vtpv_nvdec **out,
                    const vtpv_decoder_backend **out_be, void *out_ctx_unused,
                    char *errbuf, size_t errbuf_len);

/* The backend context pointer to hand vtpv_proto_create as be_ctx. */
void *vtpv_nvdec_ctx(vtpv_nvdec *v);

void vtpv_nvdec_close(vtpv_nvdec *v);

/*
 * Test/helper: copy the luma (Y) plane of a decoded surface into dst (row by
 * row, dst_stride bytes per row, 'height' rows). Returns 0 or <0. Same contract
 * as vtpv_vaapi_read_luma; surface_id is the id a vtpv_decoded_frame reported.
 */
int vtpv_nvdec_read_luma(vtpv_nvdec *v, uint32_t surface_id,
                         uint8_t *dst, size_t dst_stride,
                         uint32_t width, uint32_t height);

/*
 * Copy the interleaved NV12 chroma (UV) plane of a decoded surface into dst:
 * ceil(height/2) rows of `width` bytes (w/2 UV pairs), dst_stride apart. Returns
 * 0, or <0. Same contract as vtpv_vaapi_read_chroma.
 */
int vtpv_nvdec_read_chroma(vtpv_nvdec *v, uint32_t surface_id,
                           uint8_t *dst, size_t dst_stride,
                           uint32_t width, uint32_t height);

#ifdef __cplusplus
}
#endif

#endif /* VTPV_NVDEC_H */
