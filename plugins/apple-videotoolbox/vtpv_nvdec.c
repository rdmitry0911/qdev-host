#define _GNU_SOURCE
/*
 * vtpv_nvdec.c - NVDEC (NVIDIA) decode backend for the AppleGVA host device.
 *
 * Direct reference-port of reims's NVDEC path (see vtpv_nvdec.h). The CUVID and
 * CUDA driver-API declarations below are NVIDIA's (libnvcuvid cuviddec.h,
 * libcuda cuda.h), transcribed from reims's checked bindings:
 *   - CUVID structs / constants / function table:
 *       reims .../reims-vgpu-video-nvidia/src/nvdec/ffi.rs
 *   - CUDA structs / constants / function table:
 *       reims .../reims-vgpu-video-nvidia/src/cuda/ffi.rs
 *   - run-time library loading (dlopen/dlsym):          src/dl.rs
 *   - decoder create / decode / map / copy / unmap flow: src/nvdec/decoder.rs
 *   - CUVIDH264PICPARAMS field mapping from SPS/PPS/slice: src/nvdec/h264.rs
 *   - CUDA context retain + push-per-call:               src/cuda.rs
 *
 * Structure mirrors vtpv_vaapi.c: open/probe caps, open_decoder (create a
 * CUvideodecoder), decode (parse the AU with vtpv_h264.c, fill CUVIDPICPARAMS,
 * cuvidDecodePicture, then map+copy the NV12 frame to host staging), close.
 * The staging-into-host-then-CPU-copy-out is reims Picture::read_back's model.
 *
 * H.264 intra decode is real; the decoded NV12 luma is verified pixel-exact
 * against a known I_PCM frame on real NVDEC (test_nvdec.c). HEVC lifecycle is
 * wired, its CUVID parse is a later codec increment (as in vtpv_vaapi.c).
 */
#include "vtpv_nvdec.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vtpv_h264.h"

/* ================================================================== *
 *  CUDA driver API (libcuda) - ported from reims cuda/ffi.rs          *
 * ================================================================== */

typedef int          CUresult;
typedef int          CUdevice;
typedef unsigned long long CUdeviceptr;
typedef void        *CUcontext;
typedef void        *CUstream;
typedef void        *CUarray;
typedef int          CUmemorytype;

#define CUDA_SUCCESS            0
#define CU_MEMORYTYPE_HOST      1
#define CU_MEMORYTYPE_DEVICE    2
#define CU_STREAM_NON_BLOCKING  0x1u

/* CUDA_MEMCPY2D (reims cuda/ffi.rs, size 128 on x86-64). */
typedef struct CUDA_MEMCPY2D {
    size_t        srcXInBytes;
    size_t        srcY;
    CUmemorytype  srcMemoryType;
    const void   *srcHost;
    CUdeviceptr   srcDevice;
    CUarray       srcArray;
    size_t        srcPitch;
    size_t        dstXInBytes;
    size_t        dstY;
    CUmemorytype  dstMemoryType;
    void         *dstHost;
    CUdeviceptr   dstDevice;
    CUarray       dstArray;
    size_t        dstPitch;
    size_t        WidthInBytes;
    size_t        Height;
} CUDA_MEMCPY2D;
_Static_assert(sizeof(CUDA_MEMCPY2D) == 128, "CUDA_MEMCPY2D layout");

/* ================================================================== *
 *  NVDEC API (libnvcuvid, cuviddec.h) - ported from reims nvdec/ffi.rs *
 * ================================================================== */

typedef void *CUvideodecoder;
typedef void *CUvideoctxlock;
typedef long long CUvideotimestamp;
typedef unsigned int cudaVideoCodec;
typedef unsigned int cudaVideoChromaFormat;
typedef unsigned int cudaVideoSurfaceFormat;
typedef unsigned int cudaVideoDeinterlaceMode;
typedef unsigned int cuvidDecodeStatus;

#define cudaVideoCodec_H264   4u
#define cudaVideoCodec_HEVC   8u

#define cudaVideoChromaFormat_420 1u

#define cudaVideoSurfaceFormat_NV12 0u

#define cudaVideoDeinterlaceMode_Weave 0u

#define cudaVideoCreate_PreferCUVID 0x04ul

#define cuvidDecodeStatus_InProgress     1u
#define cuvidDecodeStatus_Error          8u
#define cuvidDecodeStatus_Error_Concealed 9u

/* CUVIDDECODECAPS (reims, size 88). First three fields in, rest out. */
typedef struct CUVIDDECODECAPS {
    cudaVideoCodec        eCodecType;
    cudaVideoChromaFormat eChromaFormat;
    unsigned int          nBitDepthMinus8;
    unsigned int          reserved1[3];
    unsigned char         bIsSupported;
    unsigned char         nNumNVDECs;
    unsigned short        nOutputFormatMask;
    unsigned int          nMaxWidth;
    unsigned int          nMaxHeight;
    unsigned int          nMaxMBCount;
    unsigned short        nMinWidth;
    unsigned short        nMinHeight;
    unsigned char         bIsHistogramSupported;
    unsigned char         nCounterBitDepth;
    unsigned short        nMaxHistogramBins;
    unsigned char         bIsDecodeStatsSupported;
    unsigned char         reserved4[3];
    unsigned int          reserved3[9];
} CUVIDDECODECAPS;
_Static_assert(sizeof(CUVIDDECODECAPS) == 88, "CUVIDDECODECAPS layout");

typedef struct CuvidRect {
    short left, top, right, bottom;
} CuvidRect;

/* CUVIDDECODECREATEINFO (reims, size 176). */
typedef struct CUVIDDECODECREATEINFO {
    unsigned long          ulWidth;
    unsigned long          ulHeight;
    unsigned long          ulNumDecodeSurfaces;
    cudaVideoCodec         CodecType;
    cudaVideoChromaFormat  ChromaFormat;
    unsigned long          ulCreationFlags;
    unsigned long          bitDepthMinus8;
    unsigned long          ulIntraDecodeOnly;
    unsigned long          ulMaxWidth;
    unsigned long          ulMaxHeight;
    unsigned long          Reserved1;
    CuvidRect              display_area;
    cudaVideoSurfaceFormat OutputFormat;
    cudaVideoDeinterlaceMode DeinterlaceMode;
    unsigned long          ulTargetWidth;
    unsigned long          ulTargetHeight;
    unsigned long          ulNumOutputSurfaces;
    CUvideoctxlock         vidLock;
    CuvidRect              target_rect;
    unsigned long          enableHistogram;
    unsigned long          enableDecodeFeatures;
    unsigned long          Reserved2[3];
} CUVIDDECODECREATEINFO;
_Static_assert(sizeof(CUVIDDECODECREATEINFO) == 176, "CUVIDDECODECREATEINFO layout");

/* CUVIDH264DPBENTRY (reims). */
typedef struct CUVIDH264DPBENTRY {
    int PicIdx;
    int FrameIdx;
    int is_long_term;
    int not_existing;
    int used_for_reference;
    int FieldOrderCnt[2];
} CUVIDH264DPBENTRY;

/* CUVIDH264PICPARAMS (reims, size 984). The trailing mvc/svc union is held as
 * bytes (18 * u64), neither used. */
typedef struct CUVIDH264PICPARAMS {
    int log2_max_frame_num_minus4;
    int pic_order_cnt_type;
    int log2_max_pic_order_cnt_lsb_minus4;
    int delta_pic_order_always_zero_flag;
    int frame_mbs_only_flag;
    int direct_8x8_inference_flag;
    int num_ref_frames;
    unsigned char residual_colour_transform_flag;
    unsigned char bit_depth_luma_minus8;
    unsigned char bit_depth_chroma_minus8;
    unsigned char qpprime_y_zero_transform_bypass_flag;
    int entropy_coding_mode_flag;
    int pic_order_present_flag;
    int num_ref_idx_l0_active_minus1;
    int num_ref_idx_l1_active_minus1;
    int weighted_pred_flag;
    int weighted_bipred_idc;
    int pic_init_qp_minus26;
    int deblocking_filter_control_present_flag;
    int redundant_pic_cnt_present_flag;
    int transform_8x8_mode_flag;
    int MbaffFrameFlag;
    int constrained_intra_pred_flag;
    int chroma_qp_index_offset;
    int second_chroma_qp_index_offset;
    int ref_pic_flag;
    int frame_num;
    int CurrFieldOrderCnt[2];
    CUVIDH264DPBENTRY dpb[16];
    unsigned char WeightScale4x4[6][16];
    unsigned char WeightScale8x8[2][64];
    unsigned char fmo_aso_enable;
    unsigned char num_slice_groups_minus1;
    unsigned char slice_group_map_type;
    signed char   pic_init_qs_minus26;
    unsigned int  slice_group_change_rate_minus1;
    unsigned long long fmo;          /* union slice_group_map_addr / pMb2SliceGroupMap */
    unsigned int  mbaff_word;        /* mb_adaptive_frame_field_flag:2 + Reserved1:30 */
    unsigned int  Reserved[11];
    unsigned long long mvcext[18];   /* anonymous union of mvcext/svcext */
} CUVIDH264PICPARAMS;
_Static_assert(sizeof(CUVIDH264PICPARAMS) == 984, "CUVIDH264PICPARAMS layout");

/* CUVIDPICPARAMS::CodecSpecific union (reims). Sized by CodecReserved[1024]. */
typedef union CuvidCodecSpecific {
    CUVIDH264PICPARAMS h264;
    unsigned int       CodecReserved[1024];
} CuvidCodecSpecific;

/* CUVIDPICPARAMS (reims, size 4280). */
typedef struct CUVIDPICPARAMS {
    int                 PicWidthInMbs;
    int                 FrameHeightInMbs;
    int                 CurrPicIdx;
    int                 field_pic_flag;
    int                 bottom_field_flag;
    int                 second_field;
    unsigned int        nBitstreamDataLen;
    const unsigned char *pBitstreamData;
    unsigned int        nNumSlices;
    const unsigned int  *pSliceDataOffsets;
    int                 ref_pic_flag;
    int                 intra_pic_flag;
    CUvideotimestamp    timestamp;
    unsigned int        Reserved[28];
    CuvidCodecSpecific  CodecSpecific;
} CUVIDPICPARAMS;
_Static_assert(sizeof(CUVIDPICPARAMS) == 4280, "CUVIDPICPARAMS layout");

/* CUVIDPROCPARAMS (reims, size 264). */
typedef struct CUVIDPROCPARAMS {
    int           progressive_frame;
    int           second_field;
    int           top_field_first;
    int           unpaired_field;
    unsigned int  reserved_flags;
    unsigned int  reserved_zero;
    unsigned long long raw_input_dptr;
    unsigned int  raw_input_pitch;
    unsigned int  raw_input_format;
    unsigned long long raw_output_dptr;
    unsigned int  raw_output_pitch;
    unsigned int  Reserved1;
    CUstream      output_stream;
    unsigned int  Reserved[46];
    unsigned long long *histogram_dptr;
    void         *pCuvidProcExt;
} CUVIDPROCPARAMS;
_Static_assert(sizeof(CUVIDPROCPARAMS) == 264, "CUVIDPROCPARAMS layout");

/* CUVIDGETDECODESTATUS (reims, size 192). */
typedef struct CUVIDGETDECODESTATUS {
    cuvidDecodeStatus decodeStatus;
    unsigned int      reserved[31];
    void             *pReserved[8];
} CUVIDGETDECODESTATUS;
_Static_assert(sizeof(CUVIDGETDECODESTATUS) == 192, "CUVIDGETDECODESTATUS layout");

/* ---- function-pointer typedefs (reims functions! tables) ------------ */

typedef CUresult (*t_cuInit)(unsigned int);
typedef CUresult (*t_cuDeviceGetCount)(int *);
typedef CUresult (*t_cuDeviceGet)(CUdevice *, int);
typedef CUresult (*t_cuDeviceGetName)(char *, int, CUdevice);
typedef CUresult (*t_cuDevicePrimaryCtxRetain)(CUcontext *, CUdevice);
typedef CUresult (*t_cuDevicePrimaryCtxRelease)(CUdevice);
typedef CUresult (*t_cuCtxPushCurrent)(CUcontext);
typedef CUresult (*t_cuCtxPopCurrent)(CUcontext *);
typedef CUresult (*t_cuStreamCreate)(CUstream *, unsigned int);
typedef CUresult (*t_cuStreamSynchronize)(CUstream);
typedef CUresult (*t_cuStreamDestroy)(CUstream);
typedef CUresult (*t_cuMemcpy2D)(const CUDA_MEMCPY2D *);
typedef CUresult (*t_cuGetErrorString)(CUresult, const char **);

typedef CUresult (*t_cuvidGetDecoderCaps)(CUVIDDECODECAPS *);
typedef CUresult (*t_cuvidCreateDecoder)(CUvideodecoder *, CUVIDDECODECREATEINFO *);
typedef CUresult (*t_cuvidDestroyDecoder)(CUvideodecoder);
typedef CUresult (*t_cuvidDecodePicture)(CUvideodecoder, CUVIDPICPARAMS *);
typedef CUresult (*t_cuvidGetDecodeStatus)(CUvideodecoder, int, CUVIDGETDECODESTATUS *);
typedef CUresult (*t_cuvidMapVideoFrame64)(CUvideodecoder, int, unsigned long long *,
                                           unsigned int *, CUVIDPROCPARAMS *);
typedef CUresult (*t_cuvidUnmapVideoFrame64)(CUvideodecoder, unsigned long long);

/* ================================================================== *
 *  Backend state                                                      *
 * ================================================================== */

#define MAX_SURFACES  64
#define NUM_DECODE_SURFACES 8   /* ulNumDecodeSurfaces; intra -> a small pool  */

typedef struct surf_entry {
    uint32_t id;            /* our surface_id (0 = free)                       */
    uint8_t *y;             /* host luma staging, width bytes/row, height rows */
    uint8_t *uv;            /* host chroma staging, width bytes/row, ceil(h/2) */
    uint32_t width, height;
} surf_entry;

struct vtpv_nvdec {
    void      *libcuda;
    void      *libnvcuvid;
    CUdevice   dev;
    CUcontext  ctx;         /* primary context (reims cuDevicePrimaryCtxRetain) */
    CUstream   stream;
    uint32_t   next_surface_id;
    surf_entry surfaces[MAX_SURFACES];
    int        has_h264, has_hevc;

    /* libcuda */
    t_cuInit                   cuInit;
    t_cuDeviceGetCount         cuDeviceGetCount;
    t_cuDeviceGet              cuDeviceGet;
    t_cuDeviceGetName          cuDeviceGetName;
    t_cuDevicePrimaryCtxRetain cuDevicePrimaryCtxRetain;
    t_cuDevicePrimaryCtxRelease cuDevicePrimaryCtxRelease;
    t_cuCtxPushCurrent         cuCtxPushCurrent;
    t_cuCtxPopCurrent          cuCtxPopCurrent;
    t_cuStreamCreate           cuStreamCreate;
    t_cuStreamSynchronize      cuStreamSynchronize;
    t_cuStreamDestroy          cuStreamDestroy;
    t_cuMemcpy2D               cuMemcpy2D;
    t_cuGetErrorString         cuGetErrorString;
    /* libnvcuvid */
    t_cuvidGetDecoderCaps      cuvidGetDecoderCaps;
    t_cuvidCreateDecoder       cuvidCreateDecoder;
    t_cuvidDestroyDecoder      cuvidDestroyDecoder;
    t_cuvidDecodePicture       cuvidDecodePicture;
    t_cuvidGetDecodeStatus     cuvidGetDecodeStatus;
    t_cuvidMapVideoFrame64     cuvidMapVideoFrame64;
    t_cuvidUnmapVideoFrame64   cuvidUnmapVideoFrame64;
};

typedef struct nvdec_dec {
    vtpv_nvdec    *v;
    uint32_t       codec;
    uint32_t       width, height;
    CUvideodecoder decoder;
    /* config SPS/PPS from the avcC the ImageDescription carried (as vtpv_vaapi.c). */
    h264_sps       cfg_sps;
    h264_pps       cfg_pps;
    int            have_cfg_sps, have_cfg_pps;
    int            nal_length_size;
    int            next_pic;    /* round-robin CurrPicIdx into the surface pool */
} nvdec_dec;

static int g_debug; /* VTPV_DEBUG=1 logs to stderr */
#define NDBG(...) do { if (g_debug) { fprintf(stderr, "vtpv_nvdec: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

static void seterr(char *buf, size_t len, const char *fmt, ...)
{
    va_list ap;
    if (!buf || !len) return;
    va_start(ap, fmt);
    vsnprintf(buf, len, fmt, ap);
    va_end(ap);
}

/* CUDA result check (reims Cuda::check): log the driver's text on failure. */
static int cu_ok(vtpv_nvdec *v, const char *call, CUresult r)
{
    if (r == CUDA_SUCCESS) return 1;
    const char *msg = NULL;
    if (v->cuGetErrorString) v->cuGetErrorString(r, &msg);
    NDBG("%s failed: %d (%s)", call, r, msg ? msg : "?");
    return 0;
}

/* Push/pop the primary context around a call (reims cuda.rs Gpu::push). The
 * plugin's proto handler is single-threaded; push per op and pop at the end. */
static int ctx_push(vtpv_nvdec *v) { return cu_ok(v, "cuCtxPushCurrent", v->cuCtxPushCurrent(v->ctx)); }
static void ctx_pop(vtpv_nvdec *v) { CUcontext c = NULL; v->cuCtxPopCurrent(&c); }

/* ---- surface registry (as vtpv_vaapi.c) ----------------------------- */

static surf_entry *surf_alloc(vtpv_nvdec *v, uint32_t w, uint32_t h, uint32_t *id_out)
{
    int i;
    for (i = 0; i < MAX_SURFACES; i++) {
        if (v->surfaces[i].id == 0) {
            uint32_t id = ++v->next_surface_id;
            if (id == 0) id = ++v->next_surface_id;
            v->surfaces[i].id = id;
            v->surfaces[i].width = w;
            v->surfaces[i].height = h;
            free(v->surfaces[i].y); free(v->surfaces[i].uv);
            v->surfaces[i].y = malloc((size_t)w * h);
            v->surfaces[i].uv = malloc((size_t)w * ((h + 1) / 2));
            if (!v->surfaces[i].y || !v->surfaces[i].uv) {
                free(v->surfaces[i].y); free(v->surfaces[i].uv);
                v->surfaces[i].y = v->surfaces[i].uv = NULL;
                v->surfaces[i].id = 0;
                return NULL;
            }
            *id_out = id;
            return &v->surfaces[i];
        }
    }
    return NULL;
}
static surf_entry *surf_find(vtpv_nvdec *v, uint32_t id)
{
    int i;
    for (i = 0; i < MAX_SURFACES; i++)
        if (v->surfaces[i].id == id)
            return &v->surfaces[i];
    return NULL;
}

/* ================================================================== *
 *  Capability enumeration (-> D-GVA-2)                                *
 * ================================================================== */

/* reims decode_capabilities: cuvidGetDecoderCaps for a codec at 4:2:0, 8-bit. */
static int nvdec_supports(vtpv_nvdec *v, cudaVideoCodec codec)
{
    CUVIDDECODECAPS caps;
    memset(&caps, 0, sizeof caps);
    caps.eCodecType = codec;
    caps.eChromaFormat = cudaVideoChromaFormat_420;
    caps.nBitDepthMinus8 = 0;
    if (!cu_ok(v, "cuvidGetDecoderCaps", v->cuvidGetDecoderCaps(&caps)))
        return 0;
    return caps.bIsSupported ? 1 : 0;
}

static void probe_caps(vtpv_nvdec *v)
{
    if (!ctx_push(v)) return;
    v->has_h264 = nvdec_supports(v, cudaVideoCodec_H264);
    v->has_hevc = nvdec_supports(v, cudaVideoCodec_HEVC);
    ctx_pop(v);
}

static int nv_query_caps(void *ctx, vtpv_caps *out, size_t cap)
{
    vtpv_nvdec *v = ctx;
    vtpv_codec_cap *c = (vtpv_codec_cap *)((uint8_t *)out + sizeof(*out));
    int n = 0;
    size_t need = sizeof(*out) + (size_t)(!!v->has_h264 + !!v->has_hevc) * sizeof(*c);
    if (cap < need) return -1;
    out->magic = VTPV_CAPS_MAGIC;
    out->abi = VTPV_ABI;
    if (v->has_h264) {
        c[n].codec = VTPV_CODEC_H264; c[n].can_decode = 1; c[n].can_encode = 0;
        c[n].reserved = 0; c[n].max_width = 4096; c[n].max_height = 4096;
        c[n].out_pixfmt = VTPV_PIXFMT_NV12; n++;
    }
    if (v->has_hevc) {
        c[n].codec = VTPV_CODEC_HEVC; c[n].can_decode = 1; c[n].can_encode = 0;
        c[n].reserved = 0; c[n].max_width = 8192; c[n].max_height = 8192;
        c[n].out_pixfmt = VTPV_PIXFMT_NV12; n++;
    }
    out->n_codecs = (uint16_t)n;
    return n;
}

/* ================================================================== *
 *  Decoder session lifecycle (reims nvdec/decoder.rs Decoder::new)    *
 * ================================================================== */

static int nv_open_decoder(void *ctx, uint32_t codec, uint32_t w, uint32_t h,
                           const uint8_t *extradata, size_t ed_len, void **out)
{
    vtpv_nvdec *v = ctx;
    nvdec_dec *d;
    CUVIDDECODECREATEINFO info;

    if ((codec == VTPV_CODEC_H264 && !v->has_h264) ||
        (codec == VTPV_CODEC_HEVC && !v->has_hevc))
        return VTPV_ERR_UNSUPPORTED;
    if (codec != VTPV_CODEC_H264 && codec != VTPV_CODEC_HEVC)
        return VTPV_ERR_UNSUPPORTED;

    d = calloc(1, sizeof(*d));
    if (!d) return VTPV_ERR_BACKEND;
    d->v = v; d->codec = codec; d->width = w; d->height = h;
    d->nal_length_size = 4; /* avcC default (lengthSizeMinusOne=3) */

    /* Capture the avcC parameter sets (as vtpv_vaapi.c): on the live wire the
     * guest delivers slice-only length-prefixed AUs and the SPS/PPS only here. */
    if (codec == VTPV_CODEC_H264 && extradata && ed_len > 0) {
        h264_nal sps_nal, pps_nal;
        int nls = 4;
        if (h264_parse_avcc(extradata, ed_len, &sps_nal, &pps_nal, &nls) == 0) {
            d->nal_length_size = nls;
            if (h264_parse_sps(&sps_nal, &d->cfg_sps) == 0) {
                d->have_cfg_sps = 1;
                if (h264_parse_pps(&pps_nal, &d->cfg_sps, &d->cfg_pps) == 0)
                    d->have_cfg_pps = 1;
            }
        }
    }

    if (!ctx_push(v)) { free(d); return VTPV_ERR_BACKEND; }
    memset(&info, 0, sizeof info);
    info.ulWidth = w;
    info.ulHeight = h;
    info.ulNumDecodeSurfaces = NUM_DECODE_SURFACES;
    info.CodecType = (codec == VTPV_CODEC_H264) ? cudaVideoCodec_H264 : cudaVideoCodec_HEVC;
    info.ChromaFormat = cudaVideoChromaFormat_420;
    info.ulCreationFlags = cudaVideoCreate_PreferCUVID;
    info.bitDepthMinus8 = 0;
    info.ulMaxWidth = w;
    info.ulMaxHeight = h;
    info.display_area.right = (short)w;
    info.display_area.bottom = (short)h;
    info.OutputFormat = cudaVideoSurfaceFormat_NV12;
    info.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;
    info.ulTargetWidth = w;
    info.ulTargetHeight = h;
    info.ulNumOutputSurfaces = 1;
    if (!cu_ok(v, "cuvidCreateDecoder", v->cuvidCreateDecoder(&d->decoder, &info))) {
        ctx_pop(v); free(d); return VTPV_ERR_BACKEND;
    }
    ctx_pop(v);
    *out = d;
    return VTPV_OK;
}

/* Fill CUVIDH264PICPARAMS from the parsed SPS/PPS/slice, mirroring reims
 * nvdec/h264.rs picture_parameters(); intra frame -> no DPB references. */
static void fill_h264_params(CUVIDPICPARAMS *pic, const h264_sps *sps,
                             const h264_pps *pps, const h264_slice *sh,
                             int all_intra)
{
    CUVIDH264PICPARAMS *h = &pic->CodecSpecific.h264;
    int reference = sh->nal_ref_idc != 0;
    int i, j;

    pic->PicWidthInMbs = sps->pic_width_in_mbs;
    pic->FrameHeightInMbs = sps->pic_height_in_map_units; /* frame_mbs_only */
    pic->field_pic_flag = 0;
    pic->bottom_field_flag = 0;
    pic->second_field = 0;
    pic->ref_pic_flag = reference;
    pic->intra_pic_flag = all_intra;

    h->log2_max_frame_num_minus4 = sps->log2_max_frame_num - 4;
    h->pic_order_cnt_type = sps->pic_order_cnt_type;
    h->log2_max_pic_order_cnt_lsb_minus4 =
        sps->pic_order_cnt_type == 0 ? sps->log2_max_pic_order_cnt_lsb - 4 : 0;
    h->delta_pic_order_always_zero_flag = sps->delta_pic_order_always_zero_flag;
    h->frame_mbs_only_flag = sps->frame_mbs_only_flag;
    h->direct_8x8_inference_flag = sps->direct_8x8_inference_flag;
    h->num_ref_frames = sps->max_num_ref_frames;
    h->residual_colour_transform_flag = (sps->chroma_format_idc == 3) ? 1 : 0;
    h->bit_depth_luma_minus8 = (unsigned char)(sps->bit_depth_luma - 8);
    h->bit_depth_chroma_minus8 = (unsigned char)(sps->bit_depth_chroma - 8);
    h->qpprime_y_zero_transform_bypass_flag = 0;
    h->entropy_coding_mode_flag = pps->entropy_coding_mode_flag;
    h->pic_order_present_flag = pps->bottom_field_pic_order_in_frame_present_flag;
    h->num_ref_idx_l0_active_minus1 = pps->num_ref_idx_l0_default_active - 1;
    h->num_ref_idx_l1_active_minus1 = pps->num_ref_idx_l1_default_active - 1;
    h->weighted_pred_flag = pps->weighted_pred_flag;
    h->weighted_bipred_idc = pps->weighted_bipred_idc;
    h->pic_init_qp_minus26 = pps->pic_init_qp - 26;
    h->deblocking_filter_control_present_flag = pps->deblocking_filter_control_present_flag;
    h->redundant_pic_cnt_present_flag = pps->redundant_pic_cnt_present_flag;
    h->transform_8x8_mode_flag = pps->transform_8x8_mode_flag;
    h->MbaffFrameFlag = 0;
    h->constrained_intra_pred_flag = pps->constrained_intra_pred_flag;
    h->chroma_qp_index_offset = pps->chroma_qp_index_offset;
    h->second_chroma_qp_index_offset = pps->second_chroma_qp_index_offset;
    h->ref_pic_flag = reference;
    h->frame_num = sh->frame_num;
    h->CurrFieldOrderCnt[0] = sh->pic_order_cnt_lsb;
    h->CurrFieldOrderCnt[1] = sh->pic_order_cnt_lsb;

    /* No references for an intra frame (reims dpb_entries over an empty set). */
    for (i = 0; i < 16; i++) {
        h->dpb[i].PicIdx = -1;
        h->dpb[i].FrameIdx = 0;
        h->dpb[i].is_long_term = 0;
        h->dpb[i].not_existing = 0;
        h->dpb[i].used_for_reference = 0;
        h->dpb[i].FieldOrderCnt[0] = h->dpb[i].FieldOrderCnt[1] = 0;
    }
    /* Flat (unscaled) weight/scaling lists: value 16, as vtpv_vaapi.c's IQ
     * matrix and H.264's default when no scaling matrix is present. */
    for (i = 0; i < 6; i++)
        for (j = 0; j < 16; j++) h->WeightScale4x4[i][j] = 16;
    for (i = 0; i < 2; i++)
        for (j = 0; j < 64; j++) h->WeightScale8x8[i][j] = 16;

    h->num_slice_groups_minus1 = (unsigned char)(pps->num_slice_groups > 0
                                                  ? pps->num_slice_groups - 1 : 0);
    h->mbaff_word = 0;
}

/* Real H.264 intra decode on NVDEC: parse the AU, fill CUVIDPICPARAMS, send the
 * slices as an Annex-B stream (3-byte start codes + pSliceDataOffsets, reims
 * nvdec/decoder.rs decode_nals/annex_b), cuvidDecodePicture, then map+copy the
 * NV12 frame into `s`'s host staging. Returns VTPV_OK or a VTPV_ERR_* code. */
static int h264_decode_nvdec(nvdec_dec *d, const uint8_t *sample, size_t len,
                             int pic_idx, surf_entry *s)
{
    vtpv_nvdec *v = d->v;
    h264_nal nals[H264_MAX_NALS];
    h264_sps sps; h264_pps pps; h264_slice sh;
    int nn, i, have_sps = 0, have_pps = 0;
    int slice_nals[H264_MAX_NALS], n_slices = 0, first_slice = -1;
    CUVIDPICPARAMS pic;
    uint8_t *bitstream = NULL;
    unsigned int *offsets = NULL;
    size_t bs_cap, bs_len = 0;
    CUVIDGETDECODESTATUS st;
    unsigned long long dptr = 0;
    unsigned int pitch = 0;
    CUVIDPROCPARAMS proc;
    CUDA_MEMCPY2D cp;
    int rc = VTPV_ERR_BACKEND, spins;

    if (h264_is_annexb(sample, len))
        nn = h264_split_nals(sample, len, nals, H264_MAX_NALS);
    else
        nn = h264_split_avcc_nals(sample, len,
                                  d->nal_length_size ? d->nal_length_size : 4,
                                  nals, H264_MAX_NALS);
    for (i = 0; i < nn; i++) {
        int t = nals[i].size ? (nals[i].data[0] & 0x1f) : 0;
        if (t == 7 && h264_parse_sps(&nals[i], &sps) == 0) have_sps = 1;
        else if (t == 8 && have_sps && h264_parse_pps(&nals[i], &sps, &pps) == 0) have_pps = 1;
        else if (t == 5 || t == 1) {
            if (first_slice < 0) first_slice = i;
            if (n_slices < H264_MAX_NALS) slice_nals[n_slices++] = i;
        }
    }
    if (!have_sps && d->have_cfg_sps) { sps = d->cfg_sps; have_sps = 1; }
    if (!have_pps && d->have_cfg_pps) { pps = d->cfg_pps; have_pps = 1; }
    if (!have_sps || !have_pps || first_slice < 0) {
        NDBG("h264: missing sps/pps/slice (sps=%d pps=%d slices=%d nals=%d)",
             have_sps, have_pps, n_slices, nn);
        return VTPV_ERR_MALFORMED;
    }
    if (h264_parse_slice_header(&nals[first_slice], &sps, &pps, &sh) != 0) {
        NDBG("h264: slice header parse failed");
        return VTPV_ERR_MALFORMED;
    }

    /* Build the Annex-B bitstream NVDEC decodes: each slice NAL after a 3-byte
     * start code; pSliceDataOffsets points at each start code (reims annex_b). */
    bs_cap = 0;
    for (i = 0; i < n_slices; i++) bs_cap += 3 + nals[slice_nals[i]].size;
    bitstream = malloc(bs_cap ? bs_cap : 1);
    offsets = malloc((size_t)(n_slices ? n_slices : 1) * sizeof(unsigned int));
    if (!bitstream || !offsets) { free(bitstream); free(offsets); return VTPV_ERR_BACKEND; }
    for (i = 0; i < n_slices; i++) {
        h264_nal *nl = &nals[slice_nals[i]];
        offsets[i] = (unsigned int)bs_len;
        bitstream[bs_len++] = 0; bitstream[bs_len++] = 0; bitstream[bs_len++] = 1;
        memcpy(bitstream + bs_len, nl->data, nl->size);
        bs_len += nl->size;
    }

    memset(&pic, 0, sizeof pic);
    fill_h264_params(&pic, &sps, &pps, &sh, /*all_intra*/ 1);
    pic.CurrPicIdx = pic_idx;
    pic.nBitstreamDataLen = (unsigned int)bs_len;
    pic.pBitstreamData = bitstream;
    pic.nNumSlices = (unsigned int)n_slices;
    pic.pSliceDataOffsets = offsets;
    pic.timestamp = 0;

    if (!ctx_push(v)) { free(bitstream); free(offsets); return VTPV_ERR_BACKEND; }

    if (!cu_ok(v, "cuvidDecodePicture", v->cuvidDecodePicture(d->decoder, &pic))) {
        rc = VTPV_ERR_BACKEND; goto out;
    }
    /* NVDEC keeps H.264 decode status; poll until it is no longer in progress
     * (reims with_mapped: wait::poll over decode_status), bounded. */
    for (spins = 0; spins < 100000; spins++) {
        memset(&st, 0, sizeof st);
        if (v->cuvidGetDecodeStatus(d->decoder, pic_idx, &st) != CUDA_SUCCESS)
            break; /* no status for this codec -> taken as done (reims) */
        if (st.decodeStatus != cuvidDecodeStatus_InProgress)
            break;
        { struct timespec ts = { 0, 100000 }; nanosleep(&ts, NULL); }
    }
    if (st.decodeStatus == cuvidDecodeStatus_Error ||
        st.decodeStatus == cuvidDecodeStatus_Error_Concealed) {
        NDBG("h264: NVDEC reports decode failed (status %u)", st.decodeStatus);
        rc = VTPV_ERR_BACKEND; goto out;
    }

    /* Map the decoded surface and copy its NV12 planes to host staging. */
    memset(&proc, 0, sizeof proc);
    proc.progressive_frame = 1;
    proc.output_stream = v->stream;
    if (!cu_ok(v, "cuvidMapVideoFrame64",
               v->cuvidMapVideoFrame64(d->decoder, pic_idx, &dptr, &pitch, &proc))) {
        rc = VTPV_ERR_BACKEND; goto out;
    }

    /* Luma: width bytes/row, height rows, from the surface base.
     * Chroma: interleaved UV, width bytes/row, ceil(h/2) rows, starting at
     * base + pitch * round_up(height,2) (reims decoder.rs Mapped::of). */
    memset(&cp, 0, sizeof cp);
    cp.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    cp.srcDevice = dptr;
    cp.srcPitch = pitch;
    cp.dstMemoryType = CU_MEMORYTYPE_HOST;
    cp.dstHost = s->y;
    cp.dstPitch = s->width;
    cp.WidthInBytes = s->width;
    cp.Height = s->height;
    if (!cu_ok(v, "cuMemcpy2D(luma)", v->cuMemcpy2D(&cp))) {
        v->cuvidUnmapVideoFrame64(d->decoder, dptr);
        rc = VTPV_ERR_BACKEND; goto out;
    }
    memset(&cp, 0, sizeof cp);
    cp.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    cp.srcDevice = dptr + (unsigned long long)pitch * ((s->height + 1) & ~1u);
    cp.srcPitch = pitch;
    cp.dstMemoryType = CU_MEMORYTYPE_HOST;
    cp.dstHost = s->uv;
    cp.dstPitch = s->width;
    cp.WidthInBytes = s->width;
    cp.Height = (s->height + 1) / 2;
    if (!cu_ok(v, "cuMemcpy2D(chroma)", v->cuMemcpy2D(&cp))) {
        v->cuvidUnmapVideoFrame64(d->decoder, dptr);
        rc = VTPV_ERR_BACKEND; goto out;
    }
    v->cuvidUnmapVideoFrame64(d->decoder, dptr);
    rc = VTPV_OK;
out:
    ctx_pop(v);
    free(bitstream);
    free(offsets);
    return rc;
}

static int nv_decode(void *dec, const uint8_t *sample, size_t sample_len,
                     vtpv_decoded_frame *out)
{
    nvdec_dec *d = dec;
    vtpv_nvdec *v = d->v;
    surf_entry *s;
    uint32_t sid = 0;
    int pic_idx, rc;

    pic_idx = d->next_pic % NUM_DECODE_SURFACES;
    d->next_pic++;

    s = surf_alloc(v, d->width, d->height, &sid);
    if (!s) return VTPV_ERR_BACKEND;

    if (d->codec == VTPV_CODEC_H264) {
        rc = h264_decode_nvdec(d, sample, sample_len, pic_idx, s);
        if (rc != VTPV_OK) { s->id = 0; return rc; }
    } else {
        /* HEVC parse->CUVID mapping is the next codec increment (as vtpv_vaapi.c);
         * the session lifecycle + surface handoff are exercised here. */
        (void)sample; (void)sample_len;
    }

    out->surface_id = sid;
    out->width = d->width;
    out->height = d->height;
    out->pixfmt = VTPV_PIXFMT_NV12;
    return VTPV_OK;
}

static void nv_close_decoder(void *dec)
{
    nvdec_dec *d = dec;
    if (!d) return;
    if (d->decoder) {
        if (ctx_push(d->v)) {
            d->v->cuvidDestroyDecoder(d->decoder);
            ctx_pop(d->v);
        }
    }
    free(d);
}

static const vtpv_decoder_backend g_nvdec_backend = {
    nv_query_caps, nv_open_decoder, nv_decode, nv_close_decoder
};

/* ================================================================== *
 *  dlopen loading (reims dl.rs Library::open / symbol)                *
 * ================================================================== */

static void *dl_sym(void *lib, const char *name, int *ok)
{
    void *p = dlsym(lib, name);
    if (!p) *ok = 0;
    return p;
}

static int load_libs(vtpv_nvdec *v, char *errbuf, size_t errlen)
{
    int ok = 1;
    v->libcuda = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!v->libcuda) { seterr(errbuf, errlen, "dlopen libcuda.so.1: %s", dlerror()); return -ENODEV; }
    v->libnvcuvid = dlopen("libnvcuvid.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!v->libnvcuvid) { seterr(errbuf, errlen, "dlopen libnvcuvid.so.1: %s", dlerror()); return -ENODEV; }

    v->cuInit = dl_sym(v->libcuda, "cuInit", &ok);
    v->cuDeviceGetCount = dl_sym(v->libcuda, "cuDeviceGetCount", &ok);
    v->cuDeviceGet = dl_sym(v->libcuda, "cuDeviceGet", &ok);
    v->cuDeviceGetName = dl_sym(v->libcuda, "cuDeviceGetName", &ok);
    v->cuDevicePrimaryCtxRetain = dl_sym(v->libcuda, "cuDevicePrimaryCtxRetain", &ok);
    v->cuDevicePrimaryCtxRelease = dl_sym(v->libcuda, "cuDevicePrimaryCtxRelease", &ok);
    v->cuCtxPushCurrent = dl_sym(v->libcuda, "cuCtxPushCurrent_v2", &ok);
    v->cuCtxPopCurrent = dl_sym(v->libcuda, "cuCtxPopCurrent_v2", &ok);
    v->cuStreamCreate = dl_sym(v->libcuda, "cuStreamCreate", &ok);
    v->cuStreamSynchronize = dl_sym(v->libcuda, "cuStreamSynchronize", &ok);
    v->cuStreamDestroy = dl_sym(v->libcuda, "cuStreamDestroy_v2", &ok);
    v->cuMemcpy2D = dl_sym(v->libcuda, "cuMemcpy2D_v2", &ok);
    v->cuGetErrorString = dl_sym(v->libcuda, "cuGetErrorString", &ok);

    v->cuvidGetDecoderCaps = dl_sym(v->libnvcuvid, "cuvidGetDecoderCaps", &ok);
    v->cuvidCreateDecoder = dl_sym(v->libnvcuvid, "cuvidCreateDecoder", &ok);
    v->cuvidDestroyDecoder = dl_sym(v->libnvcuvid, "cuvidDestroyDecoder", &ok);
    v->cuvidDecodePicture = dl_sym(v->libnvcuvid, "cuvidDecodePicture", &ok);
    v->cuvidGetDecodeStatus = dl_sym(v->libnvcuvid, "cuvidGetDecodeStatus", &ok);
    v->cuvidMapVideoFrame64 = dl_sym(v->libnvcuvid, "cuvidMapVideoFrame64", &ok);
    v->cuvidUnmapVideoFrame64 = dl_sym(v->libnvcuvid, "cuvidUnmapVideoFrame64", &ok);

    if (!ok) { seterr(errbuf, errlen, "a required CUDA/NVCUVID symbol is missing"); return -ENOSYS; }
    return 0;
}

int vtpv_nvdec_open(const char *gpu, vtpv_nvdec **out,
                    const vtpv_decoder_backend **out_be, void *unused,
                    char *errbuf, size_t errbuf_len)
{
    vtpv_nvdec *v;
    int rc, ordinal = 0, count = 0;
    (void)unused;

    if (!out || !out_be) return -EINVAL;
    { const char *dbg = getenv("VTPV_DEBUG"); g_debug = dbg && atoi(dbg); }
    if (gpu && *gpu) { char *end = NULL; long o = strtol(gpu, &end, 10); if (end != gpu) ordinal = (int)o; }

    v = calloc(1, sizeof(*v));
    if (!v) return -ENOMEM;

    rc = load_libs(v, errbuf, errbuf_len);
    if (rc != 0) { vtpv_nvdec_close(v); return rc; }

    if (!cu_ok(v, "cuInit", v->cuInit(0))) {
        seterr(errbuf, errbuf_len, "cuInit failed (no usable NVIDIA GPU)");
        vtpv_nvdec_close(v); return -ENODEV;
    }
    if (!cu_ok(v, "cuDeviceGetCount", v->cuDeviceGetCount(&count)) || count <= 0) {
        seterr(errbuf, errbuf_len, "no CUDA device");
        vtpv_nvdec_close(v); return -ENODEV;
    }
    if (ordinal < 0 || ordinal >= count) ordinal = 0;
    if (!cu_ok(v, "cuDeviceGet", v->cuDeviceGet(&v->dev, ordinal))) {
        seterr(errbuf, errbuf_len, "cuDeviceGet(%d) failed", ordinal);
        vtpv_nvdec_close(v); return -ENODEV;
    }
    if (!cu_ok(v, "cuDevicePrimaryCtxRetain", v->cuDevicePrimaryCtxRetain(&v->ctx, v->dev))) {
        seterr(errbuf, errbuf_len, "cuDevicePrimaryCtxRetain failed");
        vtpv_nvdec_close(v); return -ENODEV;
    }
    if (!ctx_push(v)) {
        seterr(errbuf, errbuf_len, "cuCtxPushCurrent failed");
        vtpv_nvdec_close(v); return -ENODEV;
    }
    if (!cu_ok(v, "cuStreamCreate", v->cuStreamCreate(&v->stream, CU_STREAM_NON_BLOCKING))) {
        ctx_pop(v);
        seterr(errbuf, errbuf_len, "cuStreamCreate failed");
        vtpv_nvdec_close(v); return -ENODEV;
    }
    ctx_pop(v);

    probe_caps(v);
    if (!v->has_h264 && !v->has_hevc) {
        seterr(errbuf, errbuf_len, "NVDEC decodes no H.264/HEVC on this GPU");
        vtpv_nvdec_close(v); return -ENOTSUP;
    }

    *out = v;
    *out_be = &g_nvdec_backend;
    return 0;
}

void *vtpv_nvdec_ctx(vtpv_nvdec *v) { return v; }

int vtpv_nvdec_read_luma(vtpv_nvdec *v, uint32_t surface_id,
                         uint8_t *dst, size_t dst_stride,
                         uint32_t width, uint32_t height)
{
    surf_entry *s = surf_find(v, surface_id);
    uint32_t row, w;
    if (!s || !s->y) return -ENOENT;
    w = width < s->width ? width : s->width;
    for (row = 0; row < height && row < s->height; row++)
        memcpy(dst + (size_t)row * dst_stride, s->y + (size_t)row * s->width, w);
    return 0;
}

int vtpv_nvdec_read_chroma(vtpv_nvdec *v, uint32_t surface_id,
                           uint8_t *dst, size_t dst_stride,
                           uint32_t width, uint32_t height)
{
    surf_entry *s = surf_find(v, surface_id);
    uint32_t row, w, crows = (height + 1) / 2, srows = (s ? (s->height + 1) / 2 : 0);
    if (!s || !s->uv) return -ENOENT;
    w = width < s->width ? width : s->width;
    for (row = 0; row < crows && row < srows; row++)
        memcpy(dst + (size_t)row * dst_stride, s->uv + (size_t)row * s->width, w);
    return 0;
}

void vtpv_nvdec_close(vtpv_nvdec *v)
{
    int i;
    if (!v) return;
    if (v->stream && v->cuStreamDestroy && v->ctx && v->cuCtxPushCurrent) {
        if (ctx_push(v)) { v->cuStreamDestroy(v->stream); ctx_pop(v); }
    }
    if (v->ctx && v->cuDevicePrimaryCtxRelease)
        v->cuDevicePrimaryCtxRelease(v->dev);
    for (i = 0; i < MAX_SURFACES; i++) { free(v->surfaces[i].y); free(v->surfaces[i].uv); }
    if (v->libnvcuvid) dlclose(v->libnvcuvid);
    if (v->libcuda) dlclose(v->libcuda);
    free(v);
}
