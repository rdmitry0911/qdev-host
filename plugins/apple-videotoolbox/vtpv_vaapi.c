#define _GNU_SOURCE
/*
 * vtpv_vaapi.c - VA-API decode backend for the AppleGVA host device.
 * CLEAN-ROOM (see vtpv_vaapi.h). Increment 1, lane L-M2V03, 2026-11-16.
 *
 * State (increment 1):
 *   - display open, capability enumeration (-> D-GVA-2), decode-pipeline
 *     setup (config/context/surface) and surface readback are REAL libva
 *     against the host render node.
 *   - H.264 decode is REAL: the in-band SPS/PPS/slice are parsed (vtpv_h264.c)
 *     into the VA parameter buffers and run through vaBeginPicture /
 *     vaRenderPicture / vaEndPicture into a render-target surface. Verified on
 *     radeonsi with a minimal I_PCM I-frame: the decoded luma matches the
 *     encoded samples exactly (test_h264.c).
 *   - HEVC: the session lifecycle + surface handoff are wired; the HEVC
 *     bitstream -> VA-buffer parse is the next codec increment.
 */
#include "vtpv_vaapi.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <va/va.h>
#include <va/va_drm.h>

#include "vtpv_h264.h"

#define MAX_SURFACES 64

typedef struct surf_entry {
    uint32_t     id;        /* our surface_id (0 = free) */
    VASurfaceID  va;
    uint32_t     width, height;
} surf_entry;

struct vtpv_vaapi {
    int          drm_fd;
    VADisplay    dpy;
    int          va_major, va_minor;
    uint32_t     next_surface_id;
    surf_entry   surfaces[MAX_SURFACES];
    /* caps cache */
    int          has_h264, has_hevc;
};

/* One open decoder session. Decode render targets are a small pool created
 * at open and handed to vaCreateContext (radeonsi requires the targets at
 * context creation); decode returns one of them, round-robin. */
#define DEC_POOL 4
typedef struct vaapi_dec {
    vtpv_vaapi *v;
    uint32_t    codec;
    uint32_t    width, height;
    VAProfile   profile;
    VAConfigID  config;
    VAContextID context;
    VASurfaceID pool[DEC_POOL];
    int         n_pool;
    int         next;
} vaapi_dec;

static int g_debug; /* VTPV_DEBUG=1 logs VA errors to stderr */
#define VDBG(...) do { if (g_debug) { fprintf(stderr, "vtpv_vaapi: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

static void seterr(char *buf, size_t len, const char *fmt, ...)
{
    va_list ap;
    if (!buf || !len) return;
    va_start(ap, fmt);
    vsnprintf(buf, len, fmt, ap);
    va_end(ap);
}

/* ---- surface registry ---------------------------------------------- */

static uint32_t surf_register(vtpv_vaapi *v, VASurfaceID va, uint32_t w, uint32_t h)
{
    int i;
    for (i = 0; i < MAX_SURFACES; i++) {
        if (v->surfaces[i].id == 0) {
            uint32_t id = ++v->next_surface_id;
            if (id == 0) id = ++v->next_surface_id;
            v->surfaces[i].id = id;
            v->surfaces[i].va = va;
            v->surfaces[i].width = w;
            v->surfaces[i].height = h;
            return id;
        }
    }
    return 0;
}
static surf_entry *surf_find(vtpv_vaapi *v, uint32_t id)
{
    int i;
    for (i = 0; i < MAX_SURFACES; i++)
        if (v->surfaces[i].id == id)
            return &v->surfaces[i];
    return NULL;
}

/* ---- capability enumeration (-> D-GVA-2) --------------------------- */

static int profile_has_vld(VADisplay dpy, VAProfile profile)
{
    VAEntrypoint eps[16];
    int n = 0, i;
    if (vaQueryConfigEntrypoints(dpy, profile, eps, &n) != VA_STATUS_SUCCESS)
        return 0;
    for (i = 0; i < n; i++)
        if (eps[i] == VAEntrypointVLD)
            return 1;
    return 0;
}

static void probe_caps(vtpv_vaapi *v)
{
    VAProfile profiles[64];
    int n = 0, i;
    v->has_h264 = v->has_hevc = 0;
    if (vaQueryConfigProfiles(v->dpy, profiles, &n) != VA_STATUS_SUCCESS)
        return;
    for (i = 0; i < n; i++) {
        switch (profiles[i]) {
        case VAProfileH264Main:
        case VAProfileH264High:
        case VAProfileH264ConstrainedBaseline:
            if (profile_has_vld(v->dpy, profiles[i])) v->has_h264 = 1;
            break;
        case VAProfileHEVCMain:
        case VAProfileHEVCMain10:
            if (profile_has_vld(v->dpy, profiles[i])) v->has_hevc = 1;
            break;
        default: break;
        }
    }
}

static int va_query_caps(void *ctx, vtpv_caps *out, size_t cap)
{
    vtpv_vaapi *v = ctx;
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

/* ---- decoder session lifecycle ------------------------------------- */

static VAProfile pick_profile(uint32_t codec)
{
    if (codec == VTPV_CODEC_H264) return VAProfileH264High;
    if (codec == VTPV_CODEC_HEVC) return VAProfileHEVCMain;
    return VAProfileNone;
}

static int va_open_decoder(void *ctx, uint32_t codec, uint32_t w, uint32_t h,
                           const uint8_t *extradata, size_t ed_len, void **out)
{
    vtpv_vaapi *v = ctx;
    vaapi_dec *d;
    VAConfigAttrib attrib;
    VAStatus st;
    (void)extradata; (void)ed_len;

    if ((codec == VTPV_CODEC_H264 && !v->has_h264) ||
        (codec == VTPV_CODEC_HEVC && !v->has_hevc))
        return VTPV_ERR_UNSUPPORTED;

    d = calloc(1, sizeof(*d));
    if (!d) return VTPV_ERR_BACKEND;
    d->v = v; d->codec = codec; d->width = w; d->height = h;
    d->profile = pick_profile(codec);
    d->config = VA_INVALID_ID; d->context = VA_INVALID_ID;

    attrib.type = VAConfigAttribRTFormat;
    attrib.value = 0;
    vaGetConfigAttributes(v->dpy, d->profile, VAEntrypointVLD, &attrib, 1);
    if (!(attrib.value & VA_RT_FORMAT_YUV420)) {
        VDBG("no YUV420 RT format for profile %d", d->profile);
        free(d); return VTPV_ERR_UNSUPPORTED;
    }
    attrib.value = VA_RT_FORMAT_YUV420;
    st = vaCreateConfig(v->dpy, d->profile, VAEntrypointVLD, &attrib, 1, &d->config);
    if (st != VA_STATUS_SUCCESS) {
        VDBG("vaCreateConfig: %s", vaErrorStr(st));
        free(d); return VTPV_ERR_BACKEND;
    }

    /* decode render-target pool, handed to vaCreateContext */
    st = vaCreateSurfaces(v->dpy, VA_RT_FORMAT_YUV420, w, h, d->pool, DEC_POOL, NULL, 0);
    if (st != VA_STATUS_SUCCESS) {
        VDBG("vaCreateSurfaces(%ux%u): %s", w, h, vaErrorStr(st));
        vaDestroyConfig(v->dpy, d->config);
        free(d); return VTPV_ERR_BACKEND;
    }
    d->n_pool = DEC_POOL;

    st = vaCreateContext(v->dpy, d->config, (int)w, (int)h, VA_PROGRESSIVE,
                         d->pool, d->n_pool, &d->context);
    if (st != VA_STATUS_SUCCESS) {
        VDBG("vaCreateContext(%ux%u): %s", w, h, vaErrorStr(st));
        vaDestroySurfaces(v->dpy, d->pool, d->n_pool);
        vaDestroyConfig(v->dpy, d->config);
        free(d); return VTPV_ERR_BACKEND;
    }
    *out = d;
    return VTPV_OK;
}

static void surf_forget_va(vtpv_vaapi *v, VASurfaceID va)
{
    int i;
    for (i = 0; i < MAX_SURFACES; i++)
        if (v->surfaces[i].id != 0 && v->surfaces[i].va == va)
            v->surfaces[i].id = 0;
}

/* Release a list of VA buffers. */
static void destroy_bufs(VADisplay dpy, VABufferID *b, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (b[i] != VA_INVALID_ID) vaDestroyBuffer(dpy, b[i]);
}

/*
 * Real H.264 intra decode: parse the in-band SPS/PPS/slice, build the VA
 * parameter buffers, and run vaBeginPicture/vaRenderPicture/vaEndPicture into
 * `surf`. Returns VTPV_OK (surf holds the decoded frame) or VTPV_ERR_*.
 */
static int h264_decode(vaapi_dec *d, const uint8_t *sample, size_t len,
                       VASurfaceID surf)
{
    vtpv_vaapi *v = d->v;
    h264_nal nals[H264_MAX_NALS];
    h264_sps sps; h264_pps pps; h264_slice sh;
    int nn, i, have_sps = 0, have_pps = 0, slice_idx = -1;
    VAPictureParameterBufferH264 pp;
    VASliceParameterBufferH264 spar;
    VAIQMatrixBufferH264 iq;
    VABufferID buf[4] = { VA_INVALID_ID, VA_INVALID_ID, VA_INVALID_ID, VA_INVALID_ID };
    VAStatus st;

    nn = h264_split_nals(sample, len, nals, H264_MAX_NALS);
    for (i = 0; i < nn; i++) {
        int t = nals[i].size ? (nals[i].data[0] & 0x1f) : 0;
        if (t == 7 && h264_parse_sps(&nals[i], &sps) == 0) have_sps = 1;
        else if (t == 8 && have_sps && h264_parse_pps(&nals[i], &sps, &pps) == 0) have_pps = 1;
        else if ((t == 5 || t == 1) && slice_idx < 0) slice_idx = i;
    }
    if (!have_sps || !have_pps || slice_idx < 0) {
        VDBG("h264: missing sps/pps/slice (sps=%d pps=%d slice=%d)",
             have_sps, have_pps, slice_idx);
        return VTPV_ERR_MALFORMED;
    }
    if (h264_parse_slice_header(&nals[slice_idx], &sps, &pps, &sh) != 0) {
        VDBG("h264: slice header parse failed");
        return VTPV_ERR_MALFORMED;
    }

    /* picture parameter buffer */
    memset(&pp, 0, sizeof(pp));
    pp.CurrPic.picture_id = surf;
    pp.CurrPic.frame_idx = (uint32_t)sh.frame_num;
    pp.CurrPic.flags = 0;
    pp.CurrPic.TopFieldOrderCnt = sh.pic_order_cnt_lsb;
    pp.CurrPic.BottomFieldOrderCnt = sh.pic_order_cnt_lsb;
    for (i = 0; i < 16; i++) {
        pp.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        pp.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    }
    pp.picture_width_in_mbs_minus1 = (uint16_t)(sps.pic_width_in_mbs - 1);
    pp.picture_height_in_mbs_minus1 = (uint16_t)(sps.pic_height_in_map_units - 1);
    pp.bit_depth_luma_minus8 = (uint8_t)(sps.bit_depth_luma - 8);
    pp.bit_depth_chroma_minus8 = (uint8_t)(sps.bit_depth_chroma - 8);
    pp.num_ref_frames = (uint8_t)sps.max_num_ref_frames;
    pp.seq_fields.bits.chroma_format_idc = sps.chroma_format_idc;
    pp.seq_fields.bits.residual_colour_transform_flag = 0;
    pp.seq_fields.bits.gaps_in_frame_num_value_allowed_flag =
        sps.gaps_in_frame_num_value_allowed_flag;
    pp.seq_fields.bits.frame_mbs_only_flag = sps.frame_mbs_only_flag;
    pp.seq_fields.bits.mb_adaptive_frame_field_flag = sps.mb_adaptive_frame_field_flag;
    pp.seq_fields.bits.direct_8x8_inference_flag = sps.direct_8x8_inference_flag;
    pp.seq_fields.bits.MinLumaBiPredSize8x8 = 0;
    pp.seq_fields.bits.log2_max_frame_num_minus4 = sps.log2_max_frame_num - 4;
    pp.seq_fields.bits.pic_order_cnt_type = sps.pic_order_cnt_type;
    pp.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 =
        sps.pic_order_cnt_type == 0 ? sps.log2_max_pic_order_cnt_lsb - 4 : 0;
    pp.seq_fields.bits.delta_pic_order_always_zero_flag = 0;
    pp.pic_init_qp_minus26 = (int8_t)(pps.pic_init_qp - 26);
    pp.chroma_qp_index_offset = (int8_t)pps.chroma_qp_index_offset;
    pp.second_chroma_qp_index_offset = (int8_t)pps.second_chroma_qp_index_offset;
    pp.pic_fields.bits.entropy_coding_mode_flag = pps.entropy_coding_mode_flag;
    pp.pic_fields.bits.weighted_pred_flag = pps.weighted_pred_flag;
    pp.pic_fields.bits.weighted_bipred_idc = pps.weighted_bipred_idc;
    pp.pic_fields.bits.transform_8x8_mode_flag = pps.transform_8x8_mode_flag;
    pp.pic_fields.bits.field_pic_flag = 0;
    pp.pic_fields.bits.constrained_intra_pred_flag = pps.constrained_intra_pred_flag;
    pp.pic_fields.bits.pic_order_present_flag =
        pps.bottom_field_pic_order_in_frame_present_flag;
    pp.pic_fields.bits.deblocking_filter_control_present_flag =
        pps.deblocking_filter_control_present_flag;
    pp.pic_fields.bits.redundant_pic_cnt_present_flag = pps.redundant_pic_cnt_present_flag;
    pp.pic_fields.bits.reference_pic_flag = sh.nal_ref_idc != 0;
    pp.frame_num = (uint16_t)sh.frame_num;

    /* flat (unscaled) quant matrices */
    memset(&iq, 16, sizeof(iq));

    /* slice parameter + data: the slice data buffer is the slice NAL bytes
     * (start code excluded), slice_data_bit_offset measured from its first
     * byte (the NAL header), which is what the parser reports. */
    memset(&spar, 0, sizeof(spar));
    spar.slice_data_size = (uint32_t)nals[slice_idx].size;
    spar.slice_data_offset = 0;
    spar.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    spar.slice_data_bit_offset = (uint16_t)sh.data_bit_offset;
    spar.first_mb_in_slice = (uint16_t)sh.first_mb_in_slice;
    spar.slice_type = (uint8_t)sh.slice_type;
    spar.slice_qp_delta = (int8_t)(sh.slice_qp - pps.pic_init_qp);
    spar.num_ref_idx_l0_active_minus1 = 0;
    spar.num_ref_idx_l1_active_minus1 = 0;
    for (i = 0; i < 32; i++) {
        spar.RefPicList0[i].picture_id = VA_INVALID_SURFACE;
        spar.RefPicList0[i].flags = VA_PICTURE_H264_INVALID;
        spar.RefPicList1[i].picture_id = VA_INVALID_SURFACE;
        spar.RefPicList1[i].flags = VA_PICTURE_H264_INVALID;
    }

    st = vaCreateBuffer(v->dpy, d->context, VAPictureParameterBufferType,
                        sizeof(pp), 1, &pp, &buf[0]);
    if (st == VA_STATUS_SUCCESS)
        st = vaCreateBuffer(v->dpy, d->context, VAIQMatrixBufferType,
                            sizeof(iq), 1, &iq, &buf[1]);
    if (st == VA_STATUS_SUCCESS)
        st = vaCreateBuffer(v->dpy, d->context, VASliceParameterBufferType,
                            sizeof(spar), 1, &spar, &buf[2]);
    if (st == VA_STATUS_SUCCESS)
        st = vaCreateBuffer(v->dpy, d->context, VASliceDataBufferType,
                            (unsigned)nals[slice_idx].size, 1,
                            (void *)nals[slice_idx].data, &buf[3]);
    if (st != VA_STATUS_SUCCESS) {
        VDBG("h264: vaCreateBuffer: %s", vaErrorStr(st));
        destroy_bufs(v->dpy, buf, 4);
        return VTPV_ERR_BACKEND;
    }

    st = vaBeginPicture(v->dpy, d->context, surf);
    if (st == VA_STATUS_SUCCESS)
        st = vaRenderPicture(v->dpy, d->context, buf, 4);
    if (st == VA_STATUS_SUCCESS)
        st = vaEndPicture(v->dpy, d->context);
    destroy_bufs(v->dpy, buf, 4);
    if (st != VA_STATUS_SUCCESS) {
        VDBG("h264: begin/render/end: %s", vaErrorStr(st));
        return VTPV_ERR_BACKEND;
    }
    st = vaSyncSurface(v->dpy, surf);
    if (st != VA_STATUS_SUCCESS) {
        VDBG("h264: vaSyncSurface: %s", vaErrorStr(st));
        return VTPV_ERR_BACKEND;
    }
    return VTPV_OK;
}

static int va_decode(void *dec, const uint8_t *sample, size_t sample_len,
                     vtpv_decoded_frame *out)
{
    vaapi_dec *d = dec;
    vtpv_vaapi *v = d->v;
    VASurfaceID surf;
    uint32_t sid;
    int rc;

    if (d->n_pool <= 0) return VTPV_ERR_BACKEND;
    surf = d->pool[d->next % d->n_pool];
    d->next++;
    surf_forget_va(v, surf); /* reuse: drop a prior registration of it */

    if (d->codec == VTPV_CODEC_H264) {
        rc = h264_decode(d, sample, sample_len, surf);
        if (rc != VTPV_OK) return rc;
    } else {
        /*
         * HEVC decode (parse + VA-buffer mapping) is the next codec increment;
         * the session lifecycle + surface handoff are exercised here. Return
         * the render-target surface (content defined once HEVC parse lands).
         */
        (void)sample; (void)sample_len;
    }

    sid = surf_register(v, surf, d->width, d->height);
    if (sid == 0)
        return VTPV_ERR_BACKEND;
    out->surface_id = sid;
    out->width = d->width;
    out->height = d->height;
    out->pixfmt = VTPV_PIXFMT_NV12;
    return VTPV_OK;
}

static void va_close_decoder(void *dec)
{
    vaapi_dec *d = dec;
    int i;
    if (!d) return;
    if (d->context != VA_INVALID_ID) vaDestroyContext(d->v->dpy, d->context);
    if (d->n_pool > 0) {
        for (i = 0; i < d->n_pool; i++) surf_forget_va(d->v, d->pool[i]);
        vaDestroySurfaces(d->v->dpy, d->pool, d->n_pool);
    }
    if (d->config != VA_INVALID_ID) vaDestroyConfig(d->v->dpy, d->config);
    free(d);
}

static const vtpv_decoder_backend g_va_backend = {
    va_query_caps, va_open_decoder, va_decode, va_close_decoder
};

/* ---- open / close / readback --------------------------------------- */

int vtpv_vaapi_open(const char *render_node, vtpv_vaapi **out,
                    const vtpv_decoder_backend **out_be, void *unused,
                    char *errbuf, size_t errbuf_len)
{
    vtpv_vaapi *v;
    VAStatus st;
    (void)unused;

    if (!render_node || !out || !out_be) return -EINVAL;
    { const char *dbg = getenv("VTPV_DEBUG"); g_debug = dbg && atoi(dbg); }
    v = calloc(1, sizeof(*v));
    if (!v) return -ENOMEM;
    v->drm_fd = open(render_node, O_RDWR | O_CLOEXEC);
    if (v->drm_fd < 0) {
        seterr(errbuf, errbuf_len, "open %s: %s", render_node, strerror(errno));
        free(v); return -errno;
    }
    v->dpy = vaGetDisplayDRM(v->drm_fd);
    if (!v->dpy) {
        seterr(errbuf, errbuf_len, "vaGetDisplayDRM failed");
        close(v->drm_fd); free(v); return -ENODEV;
    }
    st = vaInitialize(v->dpy, &v->va_major, &v->va_minor);
    if (st != VA_STATUS_SUCCESS) {
        seterr(errbuf, errbuf_len, "vaInitialize: %s", vaErrorStr(st));
        close(v->drm_fd); free(v); return -ENODEV;
    }
    probe_caps(v);
    if (!v->has_h264 && !v->has_hevc) {
        seterr(errbuf, errbuf_len, "no H.264/HEVC VLD decode on this driver");
        vaTerminate(v->dpy); close(v->drm_fd); free(v); return -ENOTSUP;
    }
    *out = v;
    *out_be = &g_va_backend;
    return 0;
}

void *vtpv_vaapi_ctx(vtpv_vaapi *v) { return v; }

int vtpv_vaapi_read_luma(vtpv_vaapi *v, uint32_t surface_id,
                         uint8_t *dst, size_t dst_stride,
                         uint32_t width, uint32_t height)
{
    surf_entry *e = surf_find(v, surface_id);
    VAImage image;
    void *base = NULL;
    VAStatus st;
    uint32_t row;

    if (!e) return -ENOENT;
    vaSyncSurface(v->dpy, e->va);
    st = vaDeriveImage(v->dpy, e->va, &image);
    if (st != VA_STATUS_SUCCESS) return -EIO;
    st = vaMapBuffer(v->dpy, image.buf, &base);
    if (st != VA_STATUS_SUCCESS) { vaDestroyImage(v->dpy, image.image_id); return -EIO; }
    for (row = 0; row < height; row++)
        memcpy(dst + row * dst_stride,
               (uint8_t *)base + image.offsets[0] + row * image.pitches[0],
               width);
    vaUnmapBuffer(v->dpy, image.buf);
    vaDestroyImage(v->dpy, image.image_id);
    return 0;
}

void vtpv_vaapi_close(vtpv_vaapi *v)
{
    if (!v) return;
    /* Decode surfaces are owned by their decoder session (freed on
     * SESSION_END / close_decoder); the registry here is non-owning. Any
     * decoder left open leaks its pool until vaTerminate tears the driver
     * down, which is the process-exit path. */
    if (v->dpy) vaTerminate(v->dpy);
    if (v->drm_fd >= 0) close(v->drm_fd);
    free(v);
}
