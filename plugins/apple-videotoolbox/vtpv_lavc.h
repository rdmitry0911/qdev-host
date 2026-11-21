/*
 * vtpv_lavc.h - stream decode core for the AppleGVA host device over FFmpeg
 * libavcodec + a hardware device (VA-API render node or CUDA/NVDEC).
 *
 * Contract (Apple's host decoder, as measured on Apple's own host and recorded
 * in our wire contract): one Demt per dfrm, frames come out in DECODE order
 * ("frame reordering in the stream notwithstanding"), a sample with no picture
 * is emitted as dropped without a buffer, a frame that does not decode is
 * emitted with the decoder's status and no buffer. The earlier backends
 * (vtpv_vaapi, vtpv_nvdec) decode each access unit as an independent intra
 * frame (no DPB). Here libavcodec owns the bitstream parse, POC/DPB, reference
 * lists and the per-vendor slice parameters, and keeps the DPB across dfrm
 * calls; this module captures the picture each access unit produced (through
 * the decoder's buffer allocation for that unit) and hands it back right after
 * that unit was decoded, so emission is in decode order while references stay
 * correct. libavcodec's own display-order output is consumed and dropped.
 * CUDA/NVDEC: the decoder's picture gets its pixels only in libavcodec's output
 * post-processing, so the unit's picture is its own output when it comes out
 * during the unit, else the post-processed reference of the decoder's picture
 * (vtpv_lavc.c; bundled libavcodec only, vtpv_lavc_cuda_lib_ok).
 *
 * Hardware is mandatory in production: get_format refuses software formats,
 * so a stream the GPU cannot decode fails instead of silently using host CPU.
 * VTPV_LAVC_HW_NONE (software) exists for tests/oracles only.
 *
 * Plan: apple-virgl analysis/20261121-APPLEGVA-RESUME-PLAN.md, unit U1.
 * Written from the public libavcodec/libavutil API; reims code not used.
 */
#ifndef VTPV_LAVC_H
#define VTPV_LAVC_H

#include <stddef.h>
#include <stdint.h>

#include "vtpv_proto.h" /* VTPV_CODEC_*, VTPV_PIXFMT_*, VTPV_ERR_*, vtpv_caps, backend vtable */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum vtpv_lavc_hw {
    VTPV_LAVC_HW_NONE  = 0, /* software decode: tests/oracle only            */
    VTPV_LAVC_HW_VAAPI = 1, /* device = DRM render node, e.g. /dev/dri/renderD128 */
    VTPV_LAVC_HW_CUDA  = 2, /* device = CUDA ordinal string, e.g. "0" (NVDEC)  */
} vtpv_lavc_hw;

typedef struct vtpv_lavc     vtpv_lavc;     /* one hardware device            */
typedef struct vtpv_lavc_dec vtpv_lavc_dec; /* one decoder session            */

/* One decoded picture, CPU-visible after the hardware transfer. Plane pointers
 * stay valid until the next decode / close on the same session. */
typedef struct vtpv_lavc_frame {
    uint32_t       width;      /* cropped display size                        */
    uint32_t       height;
    uint32_t       pixfmt;     /* VTPV_PIXFMT_NV12 (8-bit) or VTPV_PIXFMT_P010 */
    const uint8_t *plane[2];   /* [0] luma, [1] interleaved CbCr              */
    size_t         stride[2];  /* bytes per row of each plane                 */
} vtpv_lavc_frame;

/* vtpv_lavc_decode() result besides VTPV_OK / VTPV_ERR_*: the access unit
 * carried no picture (emit it as dropped, no buffer). */
#define VTPV_LAVC_NOPIC 1

int  vtpv_lavc_open(vtpv_lavc_hw hw, const char *device, vtpv_lavc **out,
                    char *errbuf, size_t errlen);
void vtpv_lavc_close(vtpv_lavc *v);
/* 0 when the loaded libavcodec is the one the CUDA path is written against (the
 * package's bundled FFmpeg 7.1.3, see vtpv_lavc.c); vtpv_lavc_open(CUDA) refuses
 * otherwise. */
int  vtpv_lavc_cuda_lib_ok(char *errbuf, size_t errlen);
vtpv_lavc_hw vtpv_lavc_hw_type(const vtpv_lavc *v);

/* Codecs this device decodes in hardware (H.264, HEVC Main), with max sizes;
 * vtpv_caps layout. Returns the codec count or <0. */
int  vtpv_lavc_query_caps(vtpv_lavc *v, vtpv_caps *out, size_t out_cap);

/* Open a decoder: codec = VTPV_CODEC_H264 (extradata avcC) or VTPV_CODEC_HEVC
 * (extradata hvcC); samples are length-prefixed NAL units as the guest sends
 * them. Accepts 4:2:0 at 8 bits (H.264, HEVC Main) and HEVC Main 10 where the
 * device decodes it; refuses (VTPV_ERR_UNSUPPORTED) anything else (no lossy
 * conversion); interlaced is left to the decoder. */
int  vtpv_lavc_open_decoder(vtpv_lavc *v, uint32_t codec, uint32_t width,
                            uint32_t height, const uint8_t *extradata,
                            size_t extradata_len, vtpv_lavc_dec **out);
/* Decode one access unit; on VTPV_OK *out holds THIS unit's picture (decode
 * order). VTPV_LAVC_NOPIC: no picture in the unit. <0: the unit did not decode
 * (the decoder state is kept; the next IRAP recovers). */
int  vtpv_lavc_decode(vtpv_lavc_dec *d, const uint8_t *sample, size_t sample_len,
                      vtpv_lavc_frame *out);
/* Drop the DPB (invalidate/seek); the next unit must start at a random access
 * point. */
void vtpv_lavc_flush(vtpv_lavc_dec *d);
/* The guest pool's CoreVideo format for this decoder's pictures (VTPV_CV_*):
 * '420v' / '420f' for 8 bits, 'x420' / 'xf20' (P010 layout) for 10, by the
 * first SPS's range (HEVC) - what the host asks for in Dpba. */
uint32_t vtpv_lavc_output_format(const vtpv_lavc_dec *d);
void vtpv_lavc_close_decoder(vtpv_lavc_dec *d);

/* Copy a frame's cropped planes into destination planes (row by row): luma
 * width*bpp bytes per row, chroma the same row size for ceil(h/2) rows; bpp 1
 * (NV12) or 2 (P010). */
int  vtpv_lavc_frame_copy(const vtpv_lavc_frame *f, uint8_t *y, size_t y_stride,
                          uint8_t *uv, size_t uv_stride);

/* ---- vtpv_decoder_backend adapter (the Apple-wire engine's backend) ------
 * decode() keeps the picture under a backend-local surface id; the device's
 * copy step reads it with read_luma/read_chroma until the next decode on that
 * session. */
const vtpv_decoder_backend *vtpv_lavc_backend(void);
/* The current picture of a surface id (planes valid until that decoder's next
 * unit, flush or close), for a landing that writes the planes itself. */
int  vtpv_lavc_get_frame(vtpv_lavc *v, uint32_t surface_id, vtpv_lavc_frame *out);
int  vtpv_lavc_read_luma(vtpv_lavc *v, uint32_t surface_id, uint8_t *dst,
                         size_t dst_stride, uint32_t w, uint32_t h);
int  vtpv_lavc_read_chroma(vtpv_lavc *v, uint32_t surface_id, uint8_t *dst,
                           size_t dst_stride, uint32_t w, uint32_t h);

#ifdef __cplusplus
}
#endif

#endif /* VTPV_LAVC_H */
