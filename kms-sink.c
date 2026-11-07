/*
 * DRM/KMS sink for qdev-host: a real monitor as the output of the guest device.
 *
 * Two dumb buffers of the connector's mode size; the guest scanout is copied
 * (damage rectangles only) into the back buffer and page-flipped at vblank.
 * The connector's EDID is exported so the plugin can hand it to the guest, which
 * then picks a mode the monitor really has.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <glib.h>
#include <glib-unix.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "kms-sink.h"

#define LOG(...) do { fprintf(stderr, "kms: " __VA_ARGS__); fputc('\n', stderr); } while (0)

typedef struct { int x0, y0, x1, y1; bool any; } rect_t;
typedef struct { uint32_t handle, pitch, fb; uint64_t size; uint8_t *map; rect_t dirty; } dumb_t;

struct kms {
    int fd;
    bool verbose;
    char card[32], conn_name[32];
    uint32_t conn_id, crtc_id;
    drmModeModeInfo mode;
    drmModeCrtc *saved;
    uint8_t *edid; uint32_t edid_len;
    dumb_t buf[2];
    int front;
    bool mode_set, flip_pending, enabled;
    /* current guest scanout geometry and placement in the mode */
    uint32_t sw, sh, sstride; int ox, oy;
    const uint8_t *spix;
    guint ev_watch;
    /* stats */
    struct { uint64_t flips, bytes; double lat_sum, lat_max, copy_sum, copy_max; double t_first_damage; double t_last_print; } st;
};

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void rect_add(rect_t *r, int x, int y, int w, int h)
{
    if (!r->any) { r->x0 = x; r->y0 = y; r->x1 = x + w; r->y1 = y + h; r->any = true; return; }
    if (x < r->x0) r->x0 = x;
    if (y < r->y0) r->y0 = y;
    if (x + w > r->x1) r->x1 = x + w;
    if (y + h > r->y1) r->y1 = y + h;
}

static bool dumb_create(kms_t *k, dumb_t *b, uint32_t w, uint32_t h)
{
    uint64_t off;
    if (drmModeCreateDumbBuffer(k->fd, w, h, 32, 0, &b->handle, &b->pitch, &b->size) < 0) { LOG("create dumb: %s", strerror(errno)); return false; }
    if (drmModeAddFB(k->fd, w, h, 24, 32, b->pitch, b->handle, &b->fb) < 0) { LOG("addfb: %s", strerror(errno)); return false; }
    if (drmModeMapDumbBuffer(k->fd, b->handle, &off) < 0) { LOG("map dumb: %s", strerror(errno)); return false; }
    b->map = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, k->fd, off);
    if (b->map == MAP_FAILED) { LOG("mmap dumb: %s", strerror(errno)); b->map = NULL; return false; }
    memset(b->map, 0, b->size);
    b->dirty.any = false;
    return true;
}

static void dumb_destroy(kms_t *k, dumb_t *b)
{
    if (b->map) munmap(b->map, b->size);
    if (b->fb) drmModeRmFB(k->fd, b->fb);
    if (b->handle) drmModeDestroyDumbBuffer(k->fd, b->handle);
    memset(b, 0, sizeof(*b));
}

static void find_edid(kms_t *k, drmModeConnector *c)
{
    drmModeObjectProperties *props = drmModeObjectGetProperties(k->fd, c->connector_id, DRM_MODE_OBJECT_CONNECTOR);
    if (!props) return;
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyRes *p = drmModeGetProperty(k->fd, props->props[i]);
        if (!p) continue;
        if (!strcmp(p->name, "EDID") && (p->flags & DRM_MODE_PROP_BLOB)) {
            drmModePropertyBlobRes *b = drmModeGetPropertyBlob(k->fd, props->prop_values[i]);
            if (b && b->length >= 128) { k->edid = g_memdup2(b->data, b->length); k->edid_len = b->length; }
            if (b) drmModeFreePropertyBlob(b);
        }
        if (!strcmp(p->name, "DPMS")) drmModeConnectorSetProperty(k->fd, c->connector_id, p->prop_id, DRM_MODE_DPMS_ON);
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
}

kms_t *kms_open(const char *spec, bool verbose)
{
    g_autofree char *path = g_strdup(spec);
    char *want = strchr(path, ':');
    if (want) *want++ = 0;
    kms_t *k = g_new0(kms_t, 1);
    k->verbose = verbose;
    k->fd = open(path, O_RDWR | O_CLOEXEC);
    if (k->fd < 0) { LOG("open %s: %s", path, strerror(errno)); goto fail; }
    snprintf(k->card, sizeof(k->card), "%s", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
    if (drmSetMaster(k->fd) < 0) { LOG("drmSetMaster %s: %s (another compositor/X server holds the device?)", path, strerror(errno)); goto fail; }
    drmModeRes *res = drmModeGetResources(k->fd);
    if (!res) { LOG("%s: no mode resources (not a KMS device?)", path); goto fail; }
    drmModeConnector *conn = NULL;
    for (int i = 0; i < res->count_connectors && !conn; i++) {
        drmModeConnector *c = drmModeGetConnector(k->fd, res->connectors[i]);
        if (!c) continue;
        char name[32];
        snprintf(name, sizeof(name), "%s-%u", drmModeGetConnectorTypeName(c->connector_type) ?: "Unknown", c->connector_type_id);
        bool match = want ? !strcmp(name, want) : (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0);
        if (match) { conn = c; snprintf(k->conn_name, sizeof(k->conn_name), "%s", name); }
        else drmModeFreeConnector(c);
    }
    if (!conn) { LOG("%s: connector %s not found", path, want ? want : "(any connected)"); drmModeFreeResources(res); goto fail; }
    if (conn->connection != DRM_MODE_CONNECTED || conn->count_modes == 0) LOG("%s: %s is not connected (yet)", path, k->conn_name);
    k->conn_id = conn->connector_id;
    /* crtc: the connector's current encoder, else the first possible one */
    drmModeEncoder *enc = conn->encoder_id ? drmModeGetEncoder(k->fd, conn->encoder_id) : NULL;
    if (enc && enc->crtc_id) k->crtc_id = enc->crtc_id;
    if (!k->crtc_id) {
        for (int i = 0; i < conn->count_encoders && !k->crtc_id; i++) {
            drmModeEncoder *e = drmModeGetEncoder(k->fd, conn->encoders[i]);
            if (!e) continue;
            for (int c = 0; c < res->count_crtcs; c++) if (e->possible_crtcs & (1u << c)) { k->crtc_id = res->crtcs[c]; break; }
            drmModeFreeEncoder(e);
        }
    }
    if (enc) drmModeFreeEncoder(enc);
    if (!k->crtc_id) { LOG("no CRTC for %s", k->conn_name); drmModeFreeConnector(conn); drmModeFreeResources(res); goto fail; }
    k->saved = drmModeGetCrtc(k->fd, k->crtc_id);
    find_edid(k, conn);
    if (conn->count_modes) k->mode = conn->modes[0];   /* preferred first */
    for (int i = 0; i < conn->count_modes; i++) if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) { k->mode = conn->modes[i]; break; }
    LOG("%s %s: crtc %u, %d modes, preferred %ux%u@%u, EDID %u bytes", k->card, k->conn_name, k->crtc_id,
        conn->count_modes, k->mode.hdisplay, k->mode.vdisplay, k->mode.vrefresh, k->edid_len);
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    return k;
fail:
    if (k->fd >= 0) close(k->fd);
    g_free(k);
    return NULL;
}

const uint8_t *kms_edid(kms_t *k, uint32_t *len) { *len = k->edid_len; return k->edid; }
const char *kms_connector_name(kms_t *k) { return k->conn_name; }

const char *kms_connector_status(kms_t *k)
{
    static char buf[32];
    g_autofree char *p = g_strdup_printf("/sys/class/drm/%s-%s/status", k->card, k->conn_name);
    FILE *f = fopen(p, "r");
    if (!f) return "unknown";
    if (!fgets(buf, sizeof(buf), f)) buf[0] = 0;
    fclose(f);
    g_strstrip(buf);
    return buf[0] ? buf : "unknown";
}

/* copy the dirty part of the scanout into buffer b (with the centering offset) */
static uint64_t copy_dirty(kms_t *k, dumb_t *b)
{
    if (!b->dirty.any || !k->spix) return 0;
    int x0 = MAX(b->dirty.x0, 0), y0 = MAX(b->dirty.y0, 0);
    int x1 = MIN(b->dirty.x1, (int)k->sw), y1 = MIN(b->dirty.y1, (int)k->sh);
    uint64_t bytes = 0;
    for (int y = y0; y < y1; y++) {
        int dy = y + k->oy; if (dy < 0 || dy >= k->mode.vdisplay) continue;
        int dx0 = x0 + k->ox, dx1 = x1 + k->ox;
        if (dx0 < 0) { x0 -= dx0; dx0 = 0; }
        if (dx1 > k->mode.hdisplay) dx1 = k->mode.hdisplay;
        if (dx1 <= dx0) continue;
        size_t n = (size_t)(dx1 - dx0) * 4;
        memcpy(b->map + (size_t)dy * b->pitch + (size_t)dx0 * 4, k->spix + (size_t)y * k->sstride + (size_t)x0 * 4, n);
        bytes += n;
    }
    b->dirty.any = false;
    return bytes;
}

static void do_flip(kms_t *k)
{
    int back = 1 - k->front;
    double t0 = now_s();
    uint64_t bytes = copy_dirty(k, &k->buf[back]);
    double tc = now_s() - t0;
    k->st.copy_sum += tc; if (tc > k->st.copy_max) k->st.copy_max = tc; k->st.bytes += bytes;
    if (drmModePageFlip(k->fd, k->crtc_id, k->buf[back].fb, DRM_MODE_PAGE_FLIP_EVENT, k) < 0) {
        LOG("page flip: %s", strerror(errno));
        return;
    }
    k->flip_pending = true;
}

static void page_flip_handler(int fd, unsigned seq, unsigned sec, unsigned usec, void *data)
{
    kms_t *k = data;
    k->flip_pending = false;
    k->front = 1 - k->front;
    k->st.flips++;
    if (k->st.t_first_damage > 0) {
        double lat = (sec + usec / 1e6) - k->st.t_first_damage;   /* DRM timestamps are CLOCK_MONOTONIC */
        k->st.lat_sum += lat; if (lat > k->st.lat_max) k->st.lat_max = lat;
        k->st.t_first_damage = 0;
    }
    if (k->buf[1 - k->front].dirty.any) {
        k->st.t_first_damage = now_s();
        do_flip(k);
    }
}

static gboolean drm_event_cb(gint fd, GIOCondition cond, gpointer data)
{
    drmEventContext ev = { .version = DRM_EVENT_CONTEXT_VERSION, .page_flip_handler = page_flip_handler };
    drmHandleEvent(fd, &ev);
    return G_SOURCE_CONTINUE;
}

void kms_scanout(kms_t *k, const uint8_t *pix, uint32_t w, uint32_t h, uint32_t stride, bool enabled)
{
    k->spix = pix; k->sw = w; k->sh = h; k->sstride = stride; k->enabled = enabled;
    if (!enabled) {
        if (k->mode_set) { memset(k->buf[k->front].map, 0, k->buf[k->front].size); k->buf[1 - k->front].dirty.any = false; }
        LOG("scanout off, screen blanked");
        return;
    }
    /* pick the connector mode: exact size match if available, else keep the preferred one and center */
    drmModeConnector *conn = drmModeGetConnector(k->fd, k->conn_id);
    drmModeModeInfo best = k->mode; bool exact = false;
    if (conn) {
        for (int i = 0; i < conn->count_modes; i++) {
            drmModeModeInfo *m = &conn->modes[i];
            if (m->hdisplay == w && m->vdisplay == h && (!exact || m->vrefresh > best.vrefresh || (m->type & DRM_MODE_TYPE_PREFERRED))) { best = *m; exact = true; }
        }
        drmModeFreeConnector(conn);
    }
    bool remode = !k->mode_set || memcmp(&best, &k->mode, sizeof(best)) != 0;
    k->mode = best;
    k->ox = ((int)k->mode.hdisplay - (int)w) / 2; k->oy = ((int)k->mode.vdisplay - (int)h) / 2;
    if (remode) {
        if (k->ev_watch) { g_source_remove(k->ev_watch); k->ev_watch = 0; }
        for (int i = 0; i < 2; i++) dumb_destroy(k, &k->buf[i]);
        for (int i = 0; i < 2; i++) if (!dumb_create(k, &k->buf[i], k->mode.hdisplay, k->mode.vdisplay)) return;
        k->front = 0; k->flip_pending = false;
        rect_add(&k->buf[0].dirty, 0, 0, w, h);
        copy_dirty(k, &k->buf[0]);
        if (drmModeSetCrtc(k->fd, k->crtc_id, k->buf[0].fb, 0, 0, &k->conn_id, 1, &k->mode) < 0) { LOG("setcrtc: %s", strerror(errno)); return; }
        k->mode_set = true;
        k->ev_watch = g_unix_fd_add(k->fd, G_IO_IN, drm_event_cb, k);
        LOG("mode %ux%u@%u on %s (%s), guest %ux%u at +%d+%d", k->mode.hdisplay, k->mode.vdisplay, k->mode.vrefresh,
            k->conn_name, exact ? "exact" : "centered in preferred", w, h, k->ox, k->oy);
    } else {
        memset(k->buf[0].map, 0, k->buf[0].size); memset(k->buf[1].map, 0, k->buf[1].size);
    }
    rect_add(&k->buf[1 - k->front].dirty, 0, 0, w, h);
    if (!k->flip_pending) { k->st.t_first_damage = now_s(); do_flip(k); }
}

void kms_update(kms_t *k, const uint8_t *pix, uint32_t stride, int x, int y, int w, int h)
{
    if (!k->mode_set || !k->enabled) return;
    k->spix = pix; k->sstride = stride;
    rect_add(&k->buf[0].dirty, x, y, w, h);
    rect_add(&k->buf[1].dirty, x, y, w, h);
    if (k->st.t_first_damage == 0) k->st.t_first_damage = now_s();
    if (!k->flip_pending) do_flip(k);
}

void kms_stats(kms_t *k, FILE *out, bool reset)
{
    double now = now_s(), dt = k->st.t_last_print ? now - k->st.t_last_print : 0;
    fprintf(out, "kms: %s %ux%u@%u: flips %llu (%.1f/s), damage->vblank avg %.2f ms max %.2f ms, copy avg %.3f ms max %.3f ms, %.1f MB copied\n",
            k->conn_name, k->mode.hdisplay, k->mode.vdisplay, k->mode.vrefresh, (unsigned long long)k->st.flips,
            dt > 0 ? k->st.flips / dt : 0.0, k->st.flips ? k->st.lat_sum / k->st.flips * 1e3 : 0.0, k->st.lat_max * 1e3,
            k->st.flips ? k->st.copy_sum / k->st.flips * 1e3 : 0.0, k->st.copy_max * 1e3, k->st.bytes / 1e6);
    if (reset) { memset(&k->st, 0, sizeof(k->st)); }
    k->st.t_last_print = now;
}

void kms_close(kms_t *k)
{
    if (!k) return;
    if (k->ev_watch) g_source_remove(k->ev_watch);
    if (k->saved && k->saved->mode_valid)
        drmModeSetCrtc(k->fd, k->saved->crtc_id, k->saved->buffer_id, k->saved->x, k->saved->y, &k->conn_id, 1, &k->saved->mode);
    else if (k->mode_set)
        drmModeSetCrtc(k->fd, k->crtc_id, 0, 0, 0, NULL, 0, NULL);
    if (k->saved) drmModeFreeCrtc(k->saved);
    for (int i = 0; i < 2; i++) dumb_destroy(k, &k->buf[i]);
    drmDropMaster(k->fd);
    close(k->fd);
    g_free(k->edid);
    g_free(k);
}
