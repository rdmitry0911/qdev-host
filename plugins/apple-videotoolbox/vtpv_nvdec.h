/*
 * vtpv_nvdec.h - an NVDEC (NVIDIA CUVID) decode backend for the AppleGVA host
 * device.
 *
 * AppleGVA host backend, NVDEC increment (lane L-M2V03).
 *
 * CLEAN-ROOM provenance: this backend is written from NVIDIA's own public
 * Video Codec SDK API only - the CUVID/NVDEC headers (cuviddec.h, nvcuvid.h)
 * and the CUDA Driver API (cuda.h), and the public NvDecoder/AppDecode sample
 * flow (NVIDIA SDK sample license). The FFI struct layouts below are
 * reconstructed from those NVIDIA headers and size-checked with _Static_assert
 * against NVIDIA's documented layouts. It does NOT copy, port, or transcribe
 * any code from the reims reference (GPL/LGPL): reims was not consulted for
 * this file. It mirrors the structure of our own clean-room VA-API backend
 * (vtpv_vaapi.c) and reuses our own H.264 parser (vtpv_h264.c).
 *
 * Implements the vtpv_decoder_backend vtable (vtpv_proto.h) over libnvcuvid +
 * libcuda, loaded at run time with dlopen so the plugin .so still loads on a
 * host with no NVIDIA driver (open then fails cleanly and the caller falls back
 * to VA-API). Decode: H.264 -> NV12. HEVC decode and encode are later
 * increments.
 */
#ifndef VTPV_NVDEC_H
#define VTPV_NVDEC_H

#include "vtpv_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vtpv_nvdec vtpv_nvdec;

/*
 * Open an NVDEC backend on the given CUDA device ordinal (a decimal string,
 * e.g. "0"; NULL means "0"). On success *out points at the handle and *out_be
 * at a vtable whose ctx is that handle; pass both to vtpv_proto_create. Returns
 * 0, or a negative errno; *errbuf (if non-NULL) gets a human-readable reason.
 * Returns non-zero (and the decode test SKIPs) where libcuda/libnvcuvid or an
 * NVIDIA GPU is not present. `out_ctx_unused` mirrors the VA-API signature.
 */
int vtpv_nvdec_open(const char *gpu, vtpv_nvdec **out,
                    const vtpv_decoder_backend **out_be, void *out_ctx_unused,
                    char *errbuf, size_t errbuf_len);

/* The backend context pointer to hand vtpv_proto_create as be_ctx. */
void *vtpv_nvdec_ctx(vtpv_nvdec *v);

void vtpv_nvdec_close(vtpv_nvdec *v);

/*
 * Test/helper: copy the luma (Y) plane of a decoded surface into dst (row by
 * row, dst_stride bytes per row, 'height' rows). Returns 0 or <0. surface_id is
 * the id a vtpv_decoded_frame reported.
 */
int vtpv_nvdec_read_luma(vtpv_nvdec *v, uint32_t surface_id,
                         uint8_t *dst, size_t dst_stride,
                         uint32_t width, uint32_t height);

/*
 * Copy the interleaved NV12 chroma (UV) plane of a decoded surface into dst:
 * ceil(height/2) rows of `width` bytes (w/2 UV pairs), dst_stride apart.
 * Returns 0, -ENOTSUP if the surface is not NV12, or <0.
 */
int vtpv_nvdec_read_chroma(vtpv_nvdec *v, uint32_t surface_id,
                           uint8_t *dst, size_t dst_stride,
                           uint32_t width, uint32_t height);

#ifdef __cplusplus
}
#endif

#endif /* VTPV_NVDEC_H */
