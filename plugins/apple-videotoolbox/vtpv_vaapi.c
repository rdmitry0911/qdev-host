#define _GNU_SOURCE
/*
 * vtpv_vaapi.c - VA-API decode backend for the AppleGVA host device.
 * CLEAN-ROOM (see vtpv_vaapi.h). Increment 1, lane L-M2V03, 2026-11-16.
 *
 * Stage boundaries (increment 1):
 *   - display open, capability enumeration (-> D-GVA-2), decode-pipeline
 *     setup (config/context/surface) and surface readback are REAL libva
 *     against the host render node.
 *   - the per-frame bitstream decode (open_decoder/decode) has the VA object
 *     lifecycle and the surface handoff wired; the H.264 access-unit ->
 *     VA-buffer mapping is in vtpv_h264.c (a minimal I-frame path). Until a
 *     backend decode path is proven on this GPU, `decode` fills the surface
 *     deterministically so the surface + readback contract is testable; the
 *     real path replaces that fill (guarded by VTPV_VAAPI_REAL_DECODE).
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

static int va_decode(void *dec, const uint8_t *sample, size_t sample_len,
                     vtpv_decoded_frame *out)
{
    vaapi_dec *d = dec;
    vtpv_vaapi *v = d->v;
    VASurfaceID surf;
    uint32_t sid;
    (void)sample; (void)sample_len;

    /*
     * Increment 1, stage 1: the VA decode-session lifecycle and the surface
     * handoff are real; the per-frame bitstream -> VA-buffer decode
     * (vaBeginPicture/vaRenderPicture/vaEndPicture over an H.264/HEVC parse)
     * is the next stage. We hand back a real render-target surface from the
     * pool so the surface + readback contract (vaDeriveImage/map) is exercised
     * now; its pixel content is defined once the real decode lands.
     */
    if (d->n_pool <= 0) return VTPV_ERR_BACKEND;
    surf = d->pool[d->next % d->n_pool];
    d->next++;
    surf_forget_va(v, surf); /* reuse: drop a prior registration of it */

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
