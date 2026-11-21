#define _GNU_SOURCE
/*
 * vtpv_nvdec.c - NVDEC (NVIDIA CUVID) decode backend for the AppleGVA host
 * device. CLEAN-ROOM (see vtpv_nvdec.h): written from NVIDIA's own public Video
 * Codec SDK API (cuviddec.h / nvcuvid.h / cuda.h) and the public NvDecoder /
 * AppDecode sample flow. reims was NOT consulted. Mirrors the structure of our
 * own clean-room VA-API backend (vtpv_vaapi.c) and reuses our H.264 parser
 * (vtpv_h264.c).
 *
 * Flow (NVIDIA's documented decode path):
 *   open  : dlopen libcuda + libnvcuvid, cuInit, create a CUDA context on the
 *           chosen device + a CUVID context lock, probe H.264 decode caps
 *           (cuvidGetDecoderCaps).
 *   open_decoder : create a CUVID video parser (cuvidCreateVideoParser) with
 *           our sequence/decode/display callbacks.
 *   decode: feed one access unit to the parser (cuvidParseVideoData with
 *           CUVID_PKT_ENDOFPICTURE, max display delay 0). The parser calls back
 *           synchronously: the sequence callback creates the decoder
 *           (cuvidCreateDecoder), the decode callback runs cuvidDecodePicture,
 *           and the display callback maps the NV12 surface
 *           (cuvidMapVideoFrame64) and copies luma+chroma out with cuMemcpy2D.
 *   readback : vtpv_nvdec_read_luma/read_chroma hand the stored NV12 planes to
 *           the caller.
 *
 * FFI: libcuda.so.1 / libnvcuvid.so.1 are loaded at run time with dlopen, so
 * the .so links only -ldl and still loads on a host with no NVIDIA driver (open
 * then fails cleanly). The CUDA/CUVID struct layouts below are reconstructed
 * from NVIDIA's headers and size-checked with _Static_assert against NVIDIA's
 * documented x86-64 layouts.
 */
#include "vtpv_nvdec.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vtpv_h264.h"

/* ====================================================================== *
 *  NVIDIA CUDA Driver API + CUVID/NVDEC FFI (clean-room from NVIDIA SDK)  *
 *  Reconstructed from cuda.h, cuviddec.h, nvcuvid.h. On Linux x86-64 the  *
 *  SDK's `unsigned long` fields are 8 bytes; CUDAAPI has no calling-conv  *
 *  decoration. Only the subset this backend uses is declared.            *
 * ====================================================================== */

#define CUDAAPI
#define CU_NV_UL unsigned long   /* the SDK's `unsigned long` (tcu_ulong) */

typedef int                       CUdevice;
typedef unsigned long long        CUdeviceptr;      /* 64-bit build        */
typedef long long                 CUvideotimestamp;
typedef void                     *CUcontext;
typedef void                     *CUstream;
typedef void                     *CUarray;
typedef void                     *CUvideodecoder;
typedef void                     *CUvideoparser;
typedef void                     *CUvideoctxlock;

typedef enum { CUDA_SUCCESS = 0 } CUresult; /* only the success code is named */

typedef enum {
    cudaVideoCodec_H264 = 4,
    cudaVideoCodec_HEVC = 8,
} cudaVideoCodec;

typedef enum {
    cudaVideoChromaFormat_Monochrome = 0,
    cudaVideoChromaFormat_420        = 1,
    cudaVideoChromaFormat_422        = 2,
    cudaVideoChromaFormat_444        = 3,
} cudaVideoChromaFormat;

typedef enum {
    cudaVideoSurfaceFormat_NV12 = 0,
    cudaVideoSurfaceFormat_P016 = 1,
} cudaVideoSurfaceFormat;

typedef enum {
    cudaVideoDeinterlaceMode_Weave = 0,
} cudaVideoDeinterlaceMode;

enum { cudaVideoCreate_PreferCUVID = 0x04 };

typedef enum {
    CU_MEMORYTYPE_HOST   = 1,
    CU_MEMORYTYPE_DEVICE = 2,
    CU_MEMORYTYPE_ARRAY  = 3,
} CUmemorytype;

/* CUVID source-data-packet flags */
enum {
    CUVID_PKT_ENDOFSTREAM  = 0x01,
    CUVID_PKT_TIMESTAMP    = 0x02,
    CUVID_PKT_ENDOFPICTURE = 0x08,
};

typedef struct CUDA_MEMCPY2D_st {
    size_t        srcXInBytes, srcY;
    CUmemorytype  srcMemoryType;
    const void   *srcHost;
    CUdeviceptr   srcDevice;
    CUarray       srcArray;
    size_t        srcPitch;
    size_t        dstXInBytes, dstY;
    CUmemorytype  dstMemoryType;
    void         *dstHost;
    CUdeviceptr   dstDevice;
    CUarray       dstArray;
    size_t        dstPitch;
    size_t        WidthInBytes, Height;
} CUDA_MEMCPY2D;

typedef struct {
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
    unsigned int          reserved3[10];
} CUVIDDECODECAPS;

typedef struct {
    CU_NV_UL                 ulWidth, ulHeight, ulNumDecodeSurfaces;
    cudaVideoCodec           CodecType;
    cudaVideoChromaFormat    ChromaFormat;
    CU_NV_UL                 ulCreationFlags, bitDepthMinus8, ulIntraDecodeOnly;
    CU_NV_UL                 ulMaxWidth, ulMaxHeight, Reserved1;
    struct { short left, top, right, bottom; } display_area;
    cudaVideoSurfaceFormat   OutputFormat;
    cudaVideoDeinterlaceMode DeinterlaceMode;
    CU_NV_UL                 ulTargetWidth, ulTargetHeight, ulNumOutputSurfaces;
    CUvideoctxlock           vidLock;
    struct { short left, top, right, bottom; } target_rect;
    CU_NV_UL                 enableHistogram, Reserved2[4];
} CUVIDDECODECREATEINFO;

typedef struct {
    int                 progressive_frame, second_field, top_field_first, unpaired_field;
    unsigned int        reserved_flags, reserved_zero;
    unsigned long long  raw_input_dptr;
    unsigned int        raw_input_pitch, raw_input_format;
    unsigned long long  raw_output_dptr;
    unsigned int        raw_output_pitch, Reserved1;
    CUstream            output_stream;
    unsigned int        Reserved[46];
    unsigned long long *histogram_dptr;
    void               *Reserved2[1];
} CUVIDPROCPARAMS;

typedef struct {
    cudaVideoCodec codec;
    struct { unsigned int numerator, denominator; } frame_rate;
    unsigned char progressive_sequence;
    unsigned char bit_depth_luma_minus8;
    unsigned char bit_depth_chroma_minus8;
    unsigned char min_num_decode_surfaces;
    unsigned int  coded_width, coded_height;
    struct { int left, top, right, bottom; } display_area;
    cudaVideoChromaFormat chroma_format;
    unsigned int  bitrate;
    struct { int x, y; } display_aspect_ratio;
    struct {
        unsigned char video_format          : 3;
        unsigned char video_full_range_flag : 1;
        unsigned char reserved_zero_bits    : 4;
        unsigned char color_primaries;
        unsigned char transfer_characteristics;
        unsigned char matrix_coefficients;
    } video_signal_description;
    unsigned int seqhdr_data_length;
} CUVIDEOFORMAT;

typedef struct {
    CU_NV_UL             flags;
    CU_NV_UL             payload_size;
    const unsigned char *payload;
    CUvideotimestamp     timestamp;
} CUVIDSOURCEDATAPACKET;

typedef struct {
    int              picture_index;
    int              progressive_frame;
    int              top_field_first;
    int              repeat_first_field;
    CUvideotimestamp timestamp;
} CUVIDPARSERDISPINFO;

/* The parser owns and fills CUVIDPICPARAMS; we only pass the pointer it gives
 * us straight to cuvidDecodePicture, so this backend treats it as opaque. */
typedef struct CUVIDPICPARAMS CUVIDPICPARAMS;

typedef int      (CUDAAPI *PFNVIDSEQUENCECALLBACK)(void *, CUVIDEOFORMAT *);
typedef int      (CUDAAPI *PFNVIDDECODECALLBACK)(void *, CUVIDPICPARAMS *);
typedef int      (CUDAAPI *PFNVIDDISPLAYCALLBACK)(void *, CUVIDPARSERDISPINFO *);

typedef struct {
    cudaVideoCodec         CodecType;
    unsigned int           ulMaxNumDecodeSurfaces;
    unsigned int           ulClockRate;
    unsigned int           ulErrorThreshold;
    unsigned int           ulMaxDisplayDelay;
    unsigned int           bAnnexb : 1;
    unsigned int           uReserved : 31;
    unsigned int           uReserved1[4];
    void                  *pUserData;
    PFNVIDSEQUENCECALLBACK pfnSequenceCallback;
    PFNVIDDECODECALLBACK   pfnDecodePicture;
    PFNVIDDISPLAYCALLBACK  pfnDisplayPicture;
    void                  *pfnGetOperatingPoint; /* unused (AV1) */
    void                  *pfnGetSEIMsg;         /* unused        */
    void                  *pvReserved2[5];
    void                  *pExtVideoInfo;
} CUVIDPARSERPARAMS;

/* Size checks against NVIDIA's documented x86-64 layouts (verified by compiling
 * NVIDIA's own headers: sizeof printed 128/88/176/264/64/32/24/136). */
_Static_assert(sizeof(CUDA_MEMCPY2D)         == 128, "CUDA_MEMCPY2D layout");
_Static_assert(sizeof(CUVIDDECODECAPS)       == 88,  "CUVIDDECODECAPS layout");
_Static_assert(sizeof(CUVIDDECODECREATEINFO) == 176, "CUVIDDECODECREATEINFO layout");
_Static_assert(sizeof(CUVIDPROCPARAMS)       == 264, "CUVIDPROCPARAMS layout");
_Static_assert(sizeof(CUVIDEOFORMAT)         == 64,  "CUVIDEOFORMAT layout");
_Static_assert(sizeof(CUVIDSOURCEDATAPACKET) == 32,  "CUVIDSOURCEDATAPACKET layout");
_Static_assert(sizeof(CUVIDPARSERDISPINFO)   == 24,  "CUVIDPARSERDISPINFO layout");
_Static_assert(sizeof(CUVIDPARSERPARAMS)     == 136, "CUVIDPARSERPARAMS layout");
_Static_assert(sizeof(CUdeviceptr)           == 8,   "64-bit CUdeviceptr");

/* Resolved entry points. */
typedef CUresult (CUDAAPI *t_cuInit)(unsigned int);
typedef CUresult (CUDAAPI *t_cuDeviceGetCount)(int *);
typedef CUresult (CUDAAPI *t_cuDeviceGet)(CUdevice *, int);
typedef CUresult (CUDAAPI *t_cuDeviceGetName)(char *, int, CUdevice);
typedef CUresult (CUDAAPI *t_cuCtxCreate)(CUcontext *, unsigned int, CUdevice);
typedef CUresult (CUDAAPI *t_cuCtxDestroy)(CUcontext);
typedef CUresult (CUDAAPI *t_cuCtxPushCurrent)(CUcontext);
typedef CUresult (CUDAAPI *t_cuCtxPopCurrent)(CUcontext *);
typedef CUresult (CUDAAPI *t_cuMemcpy2D)(const CUDA_MEMCPY2D *);
typedef CUresult (CUDAAPI *t_cuGetErrorString)(CUresult, const char **);

typedef CUresult (CUDAAPI *t_cuvidCtxLockCreate)(CUvideoctxlock *, CUcontext);
typedef CUresult (CUDAAPI *t_cuvidCtxLockDestroy)(CUvideoctxlock);
typedef CUresult (CUDAAPI *t_cuvidGetDecoderCaps)(CUVIDDECODECAPS *);
typedef CUresult (CUDAAPI *t_cuvidCreateDecoder)(CUvideodecoder *, CUVIDDECODECREATEINFO *);
typedef CUresult (CUDAAPI *t_cuvidDestroyDecoder)(CUvideodecoder);
typedef CUresult (CUDAAPI *t_cuvidDecodePicture)(CUvideodecoder, CUVIDPICPARAMS *);
typedef CUresult (CUDAAPI *t_cuvidMapVideoFrame64)(CUvideodecoder, int, unsigned long long *,
                                                   unsigned int *, CUVIDPROCPARAMS *);
typedef CUresult (CUDAAPI *t_cuvidUnmapVideoFrame64)(CUvideodecoder, unsigned long long);
typedef CUresult (CUDAAPI *t_cuvidCreateVideoParser)(CUvideoparser *, CUVIDPARSERPARAMS *);
typedef CUresult (CUDAAPI *t_cuvidParseVideoData)(CUvideoparser, CUVIDSOURCEDATAPACKET *);
typedef CUresult (CUDAAPI *t_cuvidDestroyVideoParser)(CUvideoparser);

typedef struct nv_api {
    void *libcuda, *libnvcuvid;
    t_cuInit                  cuInit;
    t_cuDeviceGetCount        cuDeviceGetCount;
    t_cuDeviceGet             cuDeviceGet;
    t_cuDeviceGetName         cuDeviceGetName;
    t_cuCtxCreate             cuCtxCreate;
    t_cuCtxDestroy            cuCtxDestroy;
    t_cuCtxPushCurrent        cuCtxPushCurrent;
    t_cuCtxPopCurrent         cuCtxPopCurrent;
    t_cuMemcpy2D              cuMemcpy2D;
    t_cuGetErrorString        cuGetErrorString;
    t_cuvidCtxLockCreate      cuvidCtxLockCreate;
    t_cuvidCtxLockDestroy     cuvidCtxLockDestroy;
    t_cuvidGetDecoderCaps     cuvidGetDecoderCaps;
    t_cuvidCreateDecoder      cuvidCreateDecoder;
    t_cuvidDestroyDecoder     cuvidDestroyDecoder;
    t_cuvidDecodePicture      cuvidDecodePicture;
    t_cuvidMapVideoFrame64    cuvidMapVideoFrame64;
    t_cuvidUnmapVideoFrame64  cuvidUnmapVideoFrame64;
    t_cuvidCreateVideoParser  cuvidCreateVideoParser;
    t_cuvidParseVideoData     cuvidParseVideoData;
    t_cuvidDestroyVideoParser cuvidDestroyVideoParser;
} nv_api;

/* ====================================================================== *
 *  Backend state                                                         *
 * ====================================================================== */

#define MAX_SURFACES 16

typedef struct surf_entry {
    uint32_t id;               /* our surface_id (0 = free)                */
    uint32_t width, height;    /* luma dimensions of the stored planes     */
    uint8_t *luma;             /* width*height, tightly packed             */
    uint8_t *chroma;           /* width*ceil(height/2), interleaved UV     */
    uint32_t pixfmt;           /* VTPV_PIXFMT_NV12                         */
} surf_entry;

struct vtpv_nvdec {
    nv_api         api;
    CUdevice       dev;
    CUcontext      ctx;
    CUvideoctxlock lock;
    int            has_h264;
    uint32_t       next_surface_id;
    int            next_evict;
    surf_entry     surfaces[MAX_SURFACES];
};

/* One open decoder session. */
typedef struct nvdec_dec {
    vtpv_nvdec    *v;
    uint32_t       codec;             /* VTPV_CODEC_*                        */
    cudaVideoCodec cu_codec;
    uint32_t       req_width, req_height;
    CUvideoparser  parser;
    CUvideodecoder decoder;
    /* geometry captured in the sequence callback */
    unsigned int   out_width, out_height; /* displayed luma dimensions       */
    unsigned int   surface_height;        /* NVDEC output surface height      */
    /* per-decode handoff from the display callback */
    int            have_surface;
    uint32_t       pending_surface_id;
    int            cb_error;
    /* avcC-carried parameter sets (live wire), converted to Annex-B and
     * prepended to the first access unit. Empty for the inline-Annex-B gate. */
    uint8_t       *annexb_hdr;
    size_t         annexb_hdr_len;
    int            hdr_sent;
    int            drained;           /* parser flushed (EOS) -> needs reset  */
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

static const char *cu_errstr(nv_api *a, CUresult r)
{
    const char *s = NULL;
    if (a->cuGetErrorString && a->cuGetErrorString(r, &s) == CUDA_SUCCESS && s) return s;
    return "CUDA error";
}

/* ---- dynamic loader -------------------------------------------------- */

#define LOADSYM(handle, field, name) do {                                   \
        *(void **)(&a->field) = dlsym(handle, name);                        \
        if (!a->field) { miss = name; goto missing; }                       \
    } while (0)

static int nv_load(nv_api *a, char *errbuf, size_t errlen)
{
    const char *miss = NULL;
    memset(a, 0, sizeof(*a));
    a->libcuda = dlopen("libcuda.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!a->libcuda) a->libcuda = dlopen("libcuda.so", RTLD_NOW | RTLD_GLOBAL);
    if (!a->libcuda) { seterr(errbuf, errlen, "libcuda not found (%s)", dlerror()); return -1; }
    a->libnvcuvid = dlopen("libnvcuvid.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!a->libnvcuvid) a->libnvcuvid = dlopen("libnvcuvid.so", RTLD_NOW | RTLD_GLOBAL);
    if (!a->libnvcuvid) { seterr(errbuf, errlen, "libnvcuvid not found (%s)", dlerror());
                          dlclose(a->libcuda); a->libcuda = NULL; return -1; }

    /* The _v2 ABI is what the installed driver exports for these entries. */
    LOADSYM(a->libcuda, cuInit,            "cuInit");
    LOADSYM(a->libcuda, cuDeviceGetCount,  "cuDeviceGetCount");
    LOADSYM(a->libcuda, cuDeviceGet,       "cuDeviceGet");
    LOADSYM(a->libcuda, cuDeviceGetName,   "cuDeviceGetName");
    LOADSYM(a->libcuda, cuCtxCreate,       "cuCtxCreate_v2");
    LOADSYM(a->libcuda, cuCtxDestroy,      "cuCtxDestroy_v2");
    LOADSYM(a->libcuda, cuCtxPushCurrent,  "cuCtxPushCurrent_v2");
    LOADSYM(a->libcuda, cuCtxPopCurrent,   "cuCtxPopCurrent_v2");
    LOADSYM(a->libcuda, cuMemcpy2D,        "cuMemcpy2D_v2");
    LOADSYM(a->libcuda, cuGetErrorString,  "cuGetErrorString");

    LOADSYM(a->libnvcuvid, cuvidCtxLockCreate,      "cuvidCtxLockCreate");
    LOADSYM(a->libnvcuvid, cuvidCtxLockDestroy,     "cuvidCtxLockDestroy");
    LOADSYM(a->libnvcuvid, cuvidGetDecoderCaps,     "cuvidGetDecoderCaps");
    LOADSYM(a->libnvcuvid, cuvidCreateDecoder,      "cuvidCreateDecoder");
    LOADSYM(a->libnvcuvid, cuvidDestroyDecoder,     "cuvidDestroyDecoder");
    LOADSYM(a->libnvcuvid, cuvidDecodePicture,      "cuvidDecodePicture");
    LOADSYM(a->libnvcuvid, cuvidMapVideoFrame64,    "cuvidMapVideoFrame64");
    LOADSYM(a->libnvcuvid, cuvidUnmapVideoFrame64,  "cuvidUnmapVideoFrame64");
    LOADSYM(a->libnvcuvid, cuvidCreateVideoParser,  "cuvidCreateVideoParser");
    LOADSYM(a->libnvcuvid, cuvidParseVideoData,     "cuvidParseVideoData");
    LOADSYM(a->libnvcuvid, cuvidDestroyVideoParser, "cuvidDestroyVideoParser");
    return 0;

missing:
    seterr(errbuf, errlen, "missing NVIDIA entry point: %s", miss);
    dlclose(a->libnvcuvid); dlclose(a->libcuda);
    a->libnvcuvid = a->libcuda = NULL;
    return -1;
}

static void nv_unload(nv_api *a)
{
    if (a->libnvcuvid) dlclose(a->libnvcuvid);
    if (a->libcuda)    dlclose(a->libcuda);
    a->libnvcuvid = a->libcuda = NULL;
}

/* ---- surface registry ------------------------------------------------ */

static surf_entry *surf_find(vtpv_nvdec *v, uint32_t id)
{
    int i;
    for (i = 0; i < MAX_SURFACES; i++)
        if (v->surfaces[i].id == id) return &v->surfaces[i];
    return NULL;
}

static void surf_release(surf_entry *e)
{
    free(e->luma); free(e->chroma);
    memset(e, 0, sizeof(*e));
}

/* Allocate (or recycle) a surface slot holding width*height luma + chroma. */
static surf_entry *surf_alloc(vtpv_nvdec *v, uint32_t w, uint32_t h)
{
    uint32_t id;
    size_t   crows = (h + 1) / 2;
    surf_entry *e = NULL;
    int i;
    for (i = 0; i < MAX_SURFACES; i++)
        if (v->surfaces[i].id == 0) { e = &v->surfaces[i]; break; }
    if (!e) { /* evict round-robin */
        e = &v->surfaces[v->next_evict];
        v->next_evict = (v->next_evict + 1) % MAX_SURFACES;
        surf_release(e);
    }
    e->luma   = malloc((size_t)w * h);
    e->chroma = malloc((size_t)w * crows);
    if (!e->luma || !e->chroma) { surf_release(e); return NULL; }
    id = ++v->next_surface_id;
    if (id == 0) id = ++v->next_surface_id;
    e->id = id; e->width = w; e->height = h; e->pixfmt = VTPV_PIXFMT_NV12;
    return e;
}

/* ====================================================================== *
 *  CUVID parser callbacks                                                *
 * ====================================================================== */

static int CUDAAPI cb_sequence(void *user, CUVIDEOFORMAT *fmt)
{
    nvdec_dec *d = user;
    vtpv_nvdec *v = d->v;
    CUVIDDECODECREATEINFO ci;
    CUresult r;
    unsigned int ndec, disp_w, disp_h, tgt_h;

    NDBG("cb_sequence: codec=%d coded=%ux%u bitdepth+8=%u chroma=%d min_surf=%u",
         fmt->codec, fmt->coded_width, fmt->coded_height,
         fmt->bit_depth_luma_minus8 + 8, fmt->chroma_format, fmt->min_num_decode_surfaces);
    disp_w = (unsigned)(fmt->display_area.right - fmt->display_area.left);
    disp_h = (unsigned)(fmt->display_area.bottom - fmt->display_area.top);
    if (disp_w == 0) disp_w = fmt->coded_width;
    if (disp_h == 0) disp_h = fmt->coded_height;
    tgt_h = (disp_h + 1) & ~1u;   /* NVDEC output height, aligned to 2 */

    ndec = fmt->min_num_decode_surfaces;
    if (ndec < 1)  ndec = 1;
    if (ndec > 20) ndec = 20;

    d->out_width = disp_w;
    d->out_height = disp_h;
    d->surface_height = tgt_h;

    if (d->decoder) return (int)ndec; /* already created; keep it */

    /* 8-bit 4:2:0 only in this increment; anything else is a later codec/depth
     * increment (reported as unsupported rather than mis-decoded). */
    if (fmt->bit_depth_luma_minus8 != 0 ||
        fmt->chroma_format != cudaVideoChromaFormat_420) {
        NDBG("unsupported format: bitdepth+8=%u chroma=%d",
             fmt->bit_depth_luma_minus8 + 8, fmt->chroma_format);
        d->cb_error = 1;
        return 0;
    }

    memset(&ci, 0, sizeof(ci));
    ci.ulWidth             = fmt->coded_width;
    ci.ulHeight            = fmt->coded_height;
    ci.ulNumDecodeSurfaces = ndec;
    ci.CodecType           = fmt->codec;
    ci.ChromaFormat        = fmt->chroma_format;
    ci.ulCreationFlags     = cudaVideoCreate_PreferCUVID;
    ci.bitDepthMinus8      = fmt->bit_depth_luma_minus8;
    ci.ulIntraDecodeOnly   = 0;
    ci.ulMaxWidth          = fmt->coded_width;
    ci.ulMaxHeight         = fmt->coded_height;
    ci.display_area.left   = (short)fmt->display_area.left;
    ci.display_area.top    = (short)fmt->display_area.top;
    ci.display_area.right  = (short)fmt->display_area.right;
    ci.display_area.bottom = (short)fmt->display_area.bottom;
    ci.OutputFormat        = cudaVideoSurfaceFormat_NV12;
    ci.DeinterlaceMode     = cudaVideoDeinterlaceMode_Weave;
    ci.ulTargetWidth       = disp_w;
    ci.ulTargetHeight      = tgt_h;
    ci.ulNumOutputSurfaces = 2;
    ci.vidLock             = v->lock;

    r = v->api.cuvidCreateDecoder(&d->decoder, &ci);
    if (r != CUDA_SUCCESS) {
        NDBG("cuvidCreateDecoder: %s", cu_errstr(&v->api, r));
        d->decoder = NULL; d->cb_error = 1;
        return 0;
    }
    return (int)ndec;
}

static int CUDAAPI cb_decode(void *user, CUVIDPICPARAMS *pic)
{
    nvdec_dec *d = user;
    CUresult r;
    NDBG("cb_decode: fired");
    if (!d->decoder) { d->cb_error = 1; return 0; }
    r = d->v->api.cuvidDecodePicture(d->decoder, pic);
    if (r != CUDA_SUCCESS) {
        NDBG("cuvidDecodePicture: %s", cu_errstr(&d->v->api, r));
        d->cb_error = 1;
        return 0;
    }
    return 1;
}

static int CUDAAPI cb_display(void *user, CUVIDPARSERDISPINFO *disp)
{
    nvdec_dec *d = user;
    vtpv_nvdec *v = d->v;
    CUVIDPROCPARAMS pp;
    unsigned long long devptr = 0;
    unsigned int pitch = 0;
    CUDA_MEMCPY2D m;
    surf_entry *e;
    CUresult r;
    uint32_t w, h, crows;

    NDBG("cb_display: fired picidx=%d", disp ? disp->picture_index : -1);
    if (!disp || !d->decoder) return 1; /* flush/EOS dummy display */

    memset(&pp, 0, sizeof(pp));
    pp.progressive_frame = disp->progressive_frame;
    pp.top_field_first   = disp->top_field_first;
    pp.second_field      = 0;
    pp.unpaired_field    = (disp->repeat_first_field < 0);
    pp.output_stream     = NULL; /* default stream */

    r = v->api.cuvidMapVideoFrame64(d->decoder, disp->picture_index, &devptr, &pitch, &pp);
    if (r != CUDA_SUCCESS) {
        NDBG("cuvidMapVideoFrame64: %s", cu_errstr(&v->api, r));
        d->cb_error = 1;
        return 0;
    }

    w = d->out_width; h = d->out_height; crows = (h + 1) / 2;
    e = surf_alloc(v, w, h);
    if (!e) { v->api.cuvidUnmapVideoFrame64(d->decoder, devptr); d->cb_error = 1; return 0; }

    /* luma plane: device (pitched) -> host (tight, stride = width) */
    memset(&m, 0, sizeof(m));
    m.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    m.srcDevice     = devptr;
    m.srcPitch      = pitch;
    m.dstMemoryType = CU_MEMORYTYPE_HOST;
    m.dstHost       = e->luma;
    m.dstPitch      = w;
    m.WidthInBytes  = w;
    m.Height        = h;
    r = v->api.cuMemcpy2D(&m);
    if (r == CUDA_SUCCESS) {
        /* interleaved NV12 chroma: begins at luma + pitch * surface_height */
        memset(&m, 0, sizeof(m));
        m.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        m.srcDevice     = devptr + (unsigned long long)pitch * d->surface_height;
        m.srcPitch      = pitch;
        m.dstMemoryType = CU_MEMORYTYPE_HOST;
        m.dstHost       = e->chroma;
        m.dstPitch      = w;
        m.WidthInBytes  = w;       /* w/2 UV pairs = w bytes */
        m.Height        = crows;
        r = v->api.cuMemcpy2D(&m);
    }
    v->api.cuvidUnmapVideoFrame64(d->decoder, devptr);
    if (r != CUDA_SUCCESS) {
        NDBG("cuMemcpy2D: %s", cu_errstr(&v->api, r));
        surf_release(e); d->cb_error = 1;
        return 0;
    }

    d->pending_surface_id = e->id;
    d->have_surface = 1;
    return 1;
}

/* ====================================================================== *
 *  vtpv_decoder_backend vtable                                           *
 * ====================================================================== */

static int nvdec_query_caps(void *ctx, vtpv_caps *out, size_t cap)
{
    vtpv_nvdec *v = ctx;
    vtpv_codec_cap *c = (vtpv_codec_cap *)((uint8_t *)out + sizeof(*out));
    int n = 0;
    size_t need = sizeof(*out) + (size_t)(!!v->has_h264) * sizeof(*c);
    if (cap < need) return -1;
    out->magic = VTPV_CAPS_MAGIC;
    out->abi = VTPV_ABI;
    if (v->has_h264) {
        c[n].codec = VTPV_CODEC_H264; c[n].can_decode = 1; c[n].can_encode = 0;
        c[n].reserved = 0; c[n].max_width = 4096; c[n].max_height = 4096;
        c[n].out_pixfmt = VTPV_PIXFMT_NV12; n++;
    }
    out->n_codecs = (uint16_t)n;
    return n;
}

static cudaVideoCodec map_codec(uint32_t codec)
{
    if (codec == VTPV_CODEC_HEVC) return cudaVideoCodec_HEVC;
    return cudaVideoCodec_H264;
}

/* Create (or recreate) the CUVID video parser for this session. The CUDA
 * context must be current. */
static int nvdec_make_parser(nvdec_dec *d)
{
    CUVIDPARSERPARAMS pp;
    CUresult r;
    memset(&pp, 0, sizeof(pp));
    pp.CodecType              = d->cu_codec;
    pp.ulMaxNumDecodeSurfaces = 1;     /* the sequence callback overrides */
    pp.ulClockRate            = 0;
    pp.ulErrorThreshold       = 100;   /* decode even a partially-corrupt AU */
    pp.ulMaxDisplayDelay      = 0;      /* lowest latency                    */
    pp.pUserData              = d;
    pp.pfnSequenceCallback    = cb_sequence;
    pp.pfnDecodePicture       = cb_decode;
    pp.pfnDisplayPicture      = cb_display;
    r = d->v->api.cuvidCreateVideoParser(&d->parser, &pp);
    if (r != CUDA_SUCCESS) {
        NDBG("cuvidCreateVideoParser: %s", cu_errstr(&d->v->api, r));
        d->parser = NULL;
        return -1;
    }
    return 0;
}

static int nvdec_open_decoder(void *ctx, uint32_t codec, uint32_t w, uint32_t h,
                              const uint8_t *extradata, size_t ed_len, void **out)
{
    vtpv_nvdec *v = ctx;
    nvdec_dec *d;
    CUresult r;
    CUcontext prev = NULL;

    if (codec != VTPV_CODEC_H264) /* HEVC decode is a later increment */
        return VTPV_ERR_UNSUPPORTED;
    if (codec == VTPV_CODEC_H264 && !v->has_h264)
        return VTPV_ERR_UNSUPPORTED;

    d = calloc(1, sizeof(*d));
    if (!d) return VTPV_ERR_BACKEND;
    d->v = v; d->codec = codec; d->cu_codec = map_codec(codec);
    d->req_width = w; d->req_height = h;

    /* If the ImageDescription carried the parameter sets in an avcC (live wire;
     * the AUs are then slice-only), convert SPS+PPS to Annex-B (start codes) and
     * prepend them to the first access unit so the CUVID parser sees a sequence.
     * Uses OUR H.264 parser (vtpv_h264.c). The inline-Annex-B gate has no
     * extradata and skips this. */
    if (codec == VTPV_CODEC_H264 && extradata && ed_len > 0) {
        h264_nal sps, pps; int nls = 4;
        if (h264_parse_avcc(extradata, ed_len, &sps, &pps, &nls) == 0 &&
            sps.size > 0 && pps.size > 0) {
            static const uint8_t sc[4] = { 0, 0, 0, 1 };
            size_t len = 4 + sps.size + 4 + pps.size;
            uint8_t *buf = malloc(len), *p;
            if (buf) {
                p = buf;
                memcpy(p, sc, 4); p += 4; memcpy(p, sps.data, sps.size); p += sps.size;
                memcpy(p, sc, 4); p += 4; memcpy(p, pps.data, pps.size); p += pps.size;
                d->annexb_hdr = buf; d->annexb_hdr_len = len;
            }
        }
    }

    if (v->api.cuCtxPushCurrent(v->ctx) != CUDA_SUCCESS) {
        free(d->annexb_hdr); free(d); return VTPV_ERR_BACKEND;
    }
    r = nvdec_make_parser(d) == 0 ? CUDA_SUCCESS : (CUresult)-1;
    v->api.cuCtxPopCurrent(&prev);
    if (r != CUDA_SUCCESS) {
        free(d->annexb_hdr); free(d);
        return VTPV_ERR_BACKEND;
    }
    *out = d;
    return VTPV_OK;
}

static int feed_packet(nvdec_dec *d, const uint8_t *data, size_t len, unsigned flags)
{
    CUVIDSOURCEDATAPACKET pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.flags = flags;
    pkt.payload = data;
    pkt.payload_size = len;
    return d->v->api.cuvidParseVideoData(d->parser, &pkt) == CUDA_SUCCESS ? 0 : -1;
}

static int nvdec_decode(void *dec, const uint8_t *sample, size_t sample_len,
                        vtpv_decoded_frame *out)
{
    nvdec_dec *d = dec;
    vtpv_nvdec *v = d->v;
    CUcontext prev = NULL;
    int rc = VTPV_OK;

    d->have_surface = 0;
    d->cb_error = 0;

    if (v->api.cuCtxPushCurrent(v->ctx) != CUDA_SUCCESS)
        return VTPV_ERR_BACKEND;

    /* A prior decode flushed this parser with EOS to drain its display queue
     * (see below); rebuild a fresh parser (and decoder) before reusing it.
     * Each access unit in the current scope is an independent intra frame, so a
     * clean parser per frame is correct; pipelined multi-frame / DPB reuse is a
     * later increment. */
    if (d->drained) {
        if (d->parser)  { v->api.cuvidDestroyVideoParser(d->parser); d->parser = NULL; }
        if (d->decoder) { v->api.cuvidDestroyDecoder(d->decoder);   d->decoder = NULL; }
        d->drained = 0; d->hdr_sent = 0;
        if (nvdec_make_parser(d) != 0) { v->api.cuCtxPopCurrent(&prev); return VTPV_ERR_BACKEND; }
    }

    /* Prepend the avcC-derived SPS/PPS exactly once, ahead of the first AU. */
    if (d->annexb_hdr && !d->hdr_sent) {
        if (feed_packet(d, d->annexb_hdr, d->annexb_hdr_len, 0) != 0) rc = VTPV_ERR_BACKEND;
        d->hdr_sent = 1;
    }
    if (rc == VTPV_OK && feed_packet(d, sample, sample_len, CUVID_PKT_ENDOFPICTURE) != 0)
        rc = VTPV_ERR_BACKEND;

    /* The CUVID parser queues the decoded picture for display and emits the
     * display callback on a subsequent parse or at end-of-stream. Drain it now
     * with an EOS packet so the just-decoded frame is handed back synchronously
     * (this is NVIDIA's documented flush path). */
    if (rc == VTPV_OK && !d->cb_error && !d->have_surface) {
        feed_packet(d, NULL, 0, CUVID_PKT_ENDOFSTREAM);
        d->drained = 1;
    }

    v->api.cuCtxPopCurrent(&prev);

    if (rc != VTPV_OK || d->cb_error) return VTPV_ERR_BACKEND;
    if (!d->have_surface) {
        NDBG("decode produced no display surface");
        return VTPV_ERR_BACKEND;
    }
    out->surface_id = d->pending_surface_id;
    out->width = d->out_width;
    out->height = d->out_height;
    out->pixfmt = VTPV_PIXFMT_NV12;
    return VTPV_OK;
}

static void nvdec_close_decoder(void *dec)
{
    nvdec_dec *d = dec;
    CUcontext prev = NULL;
    if (!d) return;
    if (d->v->api.cuCtxPushCurrent(d->v->ctx) == CUDA_SUCCESS) {
        if (d->parser)  d->v->api.cuvidDestroyVideoParser(d->parser);
        if (d->decoder) d->v->api.cuvidDestroyDecoder(d->decoder);
        d->v->api.cuCtxPopCurrent(&prev);
    }
    free(d->annexb_hdr);
    free(d);
}

static const vtpv_decoder_backend g_nvdec_backend = {
    nvdec_query_caps, nvdec_open_decoder, nvdec_decode, nvdec_close_decoder, NULL /* NV12 only: '420v' */
};

/* ====================================================================== *
 *  open / close / readback                                               *
 * ====================================================================== */

static int probe_h264(vtpv_nvdec *v)
{
    CUVIDDECODECAPS caps;
    memset(&caps, 0, sizeof(caps));
    caps.eCodecType = cudaVideoCodec_H264;
    caps.eChromaFormat = cudaVideoChromaFormat_420;
    caps.nBitDepthMinus8 = 0;
    if (v->api.cuvidGetDecoderCaps(&caps) != CUDA_SUCCESS)
        return 0;
    return caps.bIsSupported ? 1 : 0;
}

int vtpv_nvdec_open(const char *gpu, vtpv_nvdec **out,
                    const vtpv_decoder_backend **out_be, void *unused,
                    char *errbuf, size_t errbuf_len)
{
    vtpv_nvdec *v;
    CUresult r;
    CUcontext prev = NULL;
    int ordinal = 0, ndev = 0;
    char name[128] = {0};
    (void)unused;

    if (!out || !out_be) return -EINVAL;
    { const char *dbg = getenv("VTPV_DEBUG"); g_debug = dbg && atoi(dbg); }
    if (gpu && *gpu) ordinal = atoi(gpu);
    if (ordinal < 0) ordinal = 0;

    v = calloc(1, sizeof(*v));
    if (!v) return -ENOMEM;

    if (nv_load(&v->api, errbuf, errbuf_len) != 0) { free(v); return -ENODEV; }

    r = v->api.cuInit(0);
    if (r != CUDA_SUCCESS) {
        seterr(errbuf, errbuf_len, "cuInit: %s", cu_errstr(&v->api, r));
        nv_unload(&v->api); free(v); return -ENODEV;
    }
    if (v->api.cuDeviceGetCount(&ndev) != CUDA_SUCCESS || ndev <= 0) {
        seterr(errbuf, errbuf_len, "no CUDA device");
        nv_unload(&v->api); free(v); return -ENODEV;
    }
    if (ordinal >= ndev) ordinal = 0;
    r = v->api.cuDeviceGet(&v->dev, ordinal);
    if (r != CUDA_SUCCESS) {
        seterr(errbuf, errbuf_len, "cuDeviceGet(%d): %s", ordinal, cu_errstr(&v->api, r));
        nv_unload(&v->api); free(v); return -ENODEV;
    }
    v->api.cuDeviceGetName(name, sizeof(name) - 1, v->dev);

    r = v->api.cuCtxCreate(&v->ctx, 0, v->dev);
    if (r != CUDA_SUCCESS) {
        seterr(errbuf, errbuf_len, "cuCtxCreate: %s", cu_errstr(&v->api, r));
        nv_unload(&v->api); free(v); return -ENODEV;
    }
    r = v->api.cuvidCtxLockCreate(&v->lock, v->ctx);
    if (r != CUDA_SUCCESS) {
        seterr(errbuf, errbuf_len, "cuvidCtxLockCreate: %s", cu_errstr(&v->api, r));
        v->api.cuCtxDestroy(v->ctx); nv_unload(&v->api); free(v); return -ENODEV;
    }

    /* cuCtxCreate leaves the context current on this thread; probe then restore. */
    v->has_h264 = probe_h264(v);
    v->api.cuCtxPopCurrent(&prev);

    if (!v->has_h264) {
        seterr(errbuf, errbuf_len, "NVDEC on %s advertises no H.264 decode", name[0] ? name : "GPU");
        v->api.cuvidCtxLockDestroy(v->lock);
        v->api.cuCtxDestroy(v->ctx);
        nv_unload(&v->api); free(v);
        return -ENOTSUP;
    }

    NDBG("NVDEC up on device %d (%s), H.264 decode", ordinal, name);
    *out = v;
    *out_be = &g_nvdec_backend;
    return 0;
}

void *vtpv_nvdec_ctx(vtpv_nvdec *v) { return v; }

int vtpv_nvdec_read_luma(vtpv_nvdec *v, uint32_t surface_id,
                         uint8_t *dst, size_t dst_stride,
                         uint32_t width, uint32_t height)
{
    surf_entry *e = surf_find(v, surface_id);
    uint32_t row, w;
    if (!e || !e->luma) return -ENOENT;
    w = width < e->width ? width : e->width;
    if (height > e->height) height = e->height;
    for (row = 0; row < height; row++)
        memcpy(dst + (size_t)row * dst_stride, e->luma + (size_t)row * e->width, w);
    return 0;
}

int vtpv_nvdec_read_chroma(vtpv_nvdec *v, uint32_t surface_id,
                           uint8_t *dst, size_t dst_stride,
                           uint32_t width, uint32_t height)
{
    surf_entry *e = surf_find(v, surface_id);
    uint32_t row, w, crows;
    if (!e || !e->chroma) return -ENOENT;
    if (e->pixfmt != VTPV_PIXFMT_NV12) return -ENOTSUP;
    crows = (height + 1) / 2;
    if (crows > (e->height + 1) / 2) crows = (e->height + 1) / 2;
    w = width < e->width ? width : e->width;
    for (row = 0; row < crows; row++)
        memcpy(dst + (size_t)row * dst_stride, e->chroma + (size_t)row * e->width, w);
    return 0;
}

void vtpv_nvdec_close(vtpv_nvdec *v)
{
    int i;
    if (!v) return;
    for (i = 0; i < MAX_SURFACES; i++)
        if (v->surfaces[i].id) surf_release(&v->surfaces[i]);
    if (v->api.libcuda) {
        if (v->lock) v->api.cuvidCtxLockDestroy(v->lock);
        if (v->ctx)  v->api.cuCtxDestroy(v->ctx);
    }
    nv_unload(&v->api);
    free(v);
}
