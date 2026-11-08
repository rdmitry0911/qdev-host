/*
 * qdev-host: out-of-process PCI device host for stock QEMU (vfio-user-pci)
 *
 *   plugin (.so, qdp.h ABI) --> libvfio-user --> unix socket --> QEMU vfio-user-pci
 *   plugin scanout (memfd)  --> damage detection --> sinks:
 *        * org.qemu.Display1 (p2p server socket and/or message bus, bus name
 *          org.qemu): what QSM Direct / qemu-display-probe connect to, exactly
 *          like a QEMU started with -display dbus
 *        * PPM dump on SIGUSR1 and on request over the control socket (--control)
 *        * DRM/KMS: a real monitor as the output of the guest device
 *   ABI v2 plugins (GPUs) also get guest RAM regions with fd/offset, a quiesce
 *   before map changes, and host services callable from their own threads
 *   (IRQ trigger, frame-ready with damage, cursor, log).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dlfcn.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/mman.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/eventfd.h>
#include <inttypes.h>
#include <stdarg.h>
#include <pthread.h>
#include <glib.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include "qdp.h"
#include "dbus/dbus-display1.h"
#include "kms-sink.h"

#define DISPLAY1_ROOT "/org/qemu/Display1"
#define DBUS_TIMEOUT  5000

/* ------------------------------------------------------------------ options */
static struct {
    const char *plugin, *socket, *plugin_args, *display_p2p, *display_bus,
               *display_name, *vm_name, *dump_dir, *kms, *on_connect, *on_disconnect,
               *qemu_display, *qemu_display_dest, *head_origin, *desktop, *control, *qemu_qmp;
    int fps, idle_fps, idle_after, qemu_console, stats_interval;
    bool verbose, kms_copy;
} opt = { .display_name = "org.qemu", .vm_name = "qdev-host", .dump_dir = ".", .fps = 30,
          .idle_fps = 10, .idle_after = 2, .qemu_display_dest = "org.qemu" };
static bool poll_idle; static double last_damage_t;

static kms_t *kms;
static struct { double cmp_sum, cmp_max; uint64_t ticks, damaged; } tstat;   /* damage detection cost */
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

#define LOG(...) do { fprintf(stderr, "qdev-host: " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define DBG(...) do { if (opt.verbose) LOG(__VA_ARGS__); } while (0)

/* ------------------------------------------------------- plugin + vfio-user */
#if !defined(VFU_DMA_INFO_HAS_FD) || !defined(VFU_IRQ_TRIGGER_THREAD_SAFE)
#error "qdev-host needs the patched libvfio-user of third_party/libvfio-user (make vfu)"
#endif
static vfu_ctx_t *vfu;
static const qdp_device_ops *ops;
static void *priv;
static bool attached;
static guint vfu_watch;
static int vfu_realized;          /* atomic: vfu_irq_trigger() is usable from plugin threads */

/* v2 fields of the plugin, copied at load time; all zero for a v1 plugin, whose
 * qdp_device_ops ends at get_scanout and must not be read beyond it */
static uint32_t abi;
static struct {
    uint32_t flags;
    int  (*quiesce)(void *);
    void (*resume)(void *);
    void (*dma_register)(void *, const qdp_dma_region *);
    void (*dma_unregister)(void *, const qdp_dma_region *);
    bool (*get_cursor)(void *, qdp_cursor *);
} v2;
#define PLUGIN_DAMAGE (v2.flags & QDP_F_PLUGIN_DAMAGE)

/* counters shown by SIGUSR2/--stats-interval and the control socket "stats";
 * the irq ones are bumped from plugin threads (atomics) */
static struct { uint64_t irq_ok, irq_drop, frame_calls, frames, cursor_calls, cursor_sent, quiesce, dma_maps, dma_unmaps; } hstat;

static bool plugin_exit;           /* v2 reset(LOST_CONN) returned QDP_RESET_EXIT */
static void host_exit(int status);

static int reset_cb(vfu_ctx_t *c, vfu_reset_type_t t)
{
    DBG("device reset (type %d)", t);
    int r = ops->reset ? ops->reset(priv, t) : 0;
    if (abi >= 2 && r == QDP_RESET_EXIT) {
        if (t == VFU_RESET_LOST_CONN) plugin_exit = true;
        else LOG("plugin returned QDP_RESET_EXIT for reset type %d (only meaningful for a lost connection)", t);
        return 0;
    }
    return r;
}

/* Guest RAM regions (v2): libvfio-user calls these on the main loop for every
 * DMA_MAP/DMA_UNMAP and for each region still mapped when the client goes away.
 * The identical re-map of a region already mapped is not forwarded again, so the
 * plugin sees one register and one unregister per region. */
static GHashTable *dma_map;       /* &region->iova -> qdp_dma_region*, regions known to the plugin */
static uint64_t dma_bytes;
static void dma_register(vfu_ctx_t *c, vfu_dma_info_t *i)
{
    if (abi < 2) return;
    uint64_t iova = (uintptr_t)i->iova.iov_base;
    if (g_hash_table_lookup(dma_map, &iova)) { DBG("dma: identical re-map of 0x%" PRIx64 " not forwarded", iova); return; }
    qdp_dma_region *r = g_new0(qdp_dma_region, 1);
    *r = (qdp_dma_region){ .iova = iova, .len = i->iova.iov_len, .vaddr = i->vaddr, .fd = i->fd,
                           .fd_offset = i->fd_offset, .prot = i->prot };
    g_hash_table_insert(dma_map, &r->iova, r);
    dma_bytes += r->len; hstat.dma_maps++;
    DBG("dma map iova 0x%" PRIx64 " len 0x%" PRIx64 " fd %d offset 0x%" PRIx64 " vaddr %p prot %x",
        r->iova, r->len, r->fd, r->fd_offset, r->vaddr, r->prot);
    if (v2.dma_register) v2.dma_register(priv, r);
}
static void dma_unregister(vfu_ctx_t *c, vfu_dma_info_t *i)
{
    if (abi < 2) return;
    uint64_t iova = (uintptr_t)i->iova.iov_base;
    qdp_dma_region *r = g_hash_table_lookup(dma_map, &iova);
    if (!r) return;
    DBG("dma unmap iova 0x%" PRIx64 " len 0x%" PRIx64, r->iova, r->len);
    if (v2.dma_unregister) v2.dma_unregister(priv, r);    /* fd and vaddr stay valid until this returns */
    dma_bytes -= r->len; hstat.dma_unmaps++;
    g_hash_table_remove(dma_map, &iova);
}

/* Quiesce (v2 plugins with a quiesce op). libvfio-user asks before every
 * DMA_MAP, DMA_UNMAP, device reset (also FLR) and before dropping a lost
 * client. The plugin's quiesce is synchronous; resume() follows once
 * vfu_run_ctx() has handled the request. A refusal fails the client request
 * (it must not be EBUSY, which would mean "asynchronous" to libvfio-user). */
static bool plugin_quiesced;
static int quiesce_cb(vfu_ctx_t *c)
{
    if (plugin_quiesced) return 0;           /* a reset after a failed reply, same request */
    int r = v2.quiesce(priv);
    if (r < 0) { LOG("plugin refused to quiesce: %s", strerror(-r)); errno = r == -EBUSY ? EAGAIN : -r; return -1; }
    plugin_quiesced = true; hstat.quiesce++;
    return 0;
}
static void plugin_resume(void)
{
    if (!plugin_quiesced) return;
    plugin_quiesced = false;
    if (v2.resume) v2.resume(priv);
}

/* ----------------------------------------------- host services (ABI v2)
 * Called from any plugin thread. irq_trigger goes straight to the eventfd
 * (vfu_irq_trigger is thread-safe in the vendored libvfio-user and never waits
 * for the main loop). frame_ready/cursor_changed record what changed under
 * hs_lock and wake the main loop through an eventfd, once per batch. */
static int wake_fd = -1;
static pthread_mutex_t hs_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {                     /* under hs_lock */
    bool frame, cursor, full;
    uint64_t seq;
    uint32_t x0, y0, x1, y1;        /* union of the damage since the last batch */
} hs;

static void wake_main(void) { uint64_t one = 1; if (write(wake_fd, &one, sizeof(one)) < 0 && errno != EAGAIN) LOG("wake: %s", strerror(errno)); }

static int host_irq_trigger(void *h, uint32_t subindex)
{
    if (!__atomic_load_n(&vfu_realized, __ATOMIC_ACQUIRE)) return -EINVAL;
    if (vfu_irq_trigger(vfu, subindex) < 0) {
        int e = errno;
        __atomic_add_fetch(&hstat.irq_drop, 1, __ATOMIC_RELAXED);
        return e == ENOENT ? -EINVAL : -e;  /* ENOENT: the client set no eventfd, i.e. not enabled */
    }
    __atomic_add_fetch(&hstat.irq_ok, 1, __ATOMIC_RELAXED);
    return 0;
}

static void host_frame_ready(void *h, const qdp_frame_info *f)
{
    pthread_mutex_lock(&hs_lock);
    bool kick = !hs.frame && !hs.cursor;
    if (f->w == 0 || f->h == 0) hs.full = true;
    else if (!hs.frame) { hs.x0 = f->x; hs.y0 = f->y; hs.x1 = f->x + f->w; hs.y1 = f->y + f->h; }
    else { hs.x0 = MIN(hs.x0, f->x); hs.y0 = MIN(hs.y0, f->y); hs.x1 = MAX(hs.x1, f->x + f->w); hs.y1 = MAX(hs.y1, f->y + f->h); }
    hs.seq = f->seq; hs.frame = true; __atomic_add_fetch(&hstat.frame_calls, 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&hs_lock);
    if (kick) wake_main();
}

static void host_cursor_changed(void *h)
{
    pthread_mutex_lock(&hs_lock);
    bool kick = !hs.frame && !hs.cursor;
    hs.cursor = true; __atomic_add_fetch(&hstat.cursor_calls, 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&hs_lock);
    if (kick) wake_main();
}

static void host_log(void *h, int level, const char *fmt, ...)
{
    static const char *pfx[] = { "error: ", "warning: ", "", "debug: " };
    if (level >= 3 && !opt.verbose) return;
    char buf[1024]; va_list ap;
    va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    size_t n = strlen(buf); while (n && buf[n - 1] == '\n') buf[--n] = 0;
    fprintf(stderr, "qdev-host: %s: %s%s\n", ops->name, pfx[level < 0 ? 0 : level > 3 ? 3 : level], buf);   /* one call: one line */
}

static int host_token;
static const qdp_host_ops host_ops = {
    .size = sizeof(qdp_host_ops), .host = &host_token, .irq_trigger = host_irq_trigger,
    .frame_ready = host_frame_ready, .cursor_changed = host_cursor_changed, .log = host_log,
};

static gboolean vfu_io_cb(gint fd, GIOCondition cond, gpointer data)
{
    if (!attached) {
        if (vfu_attach_ctx(vfu) < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return G_SOURCE_CONTINUE;
            LOG("vfu_attach_ctx: %s", strerror(errno)); exit(1);
        }
        attached = true;
        LOG("client attached");
    } else {
        int r = vfu_run_ctx(vfu), e = errno;
        if (plugin_exit) {                /* lost client, and the plugin cannot serve another one */
            LOG("client detached; the plugin cannot serve another client (QDP_RESET_EXIT): exiting");
            host_exit(0);
        }
        plugin_resume();                  /* the map change (or reset) of this request is done */
        errno = e;
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return G_SOURCE_CONTINUE;
            /* a v1 plugin's own asynchronous quiesce is pending: it calls vfu_device_quiesced() */
            if (errno == EBUSY) return G_SOURCE_CONTINUE;
            if (errno != ENOTCONN) { LOG("vfu_run_ctx: %s", strerror(errno)); exit(1); }
            attached = false;
            LOG("client detached, device state kept, waiting for the next client");
        } else {
            return G_SOURCE_CONTINUE;
        }
    }
    /* the poll fd changes on attach/detach: re-arm on the new one */
    vfu_watch = g_unix_fd_add(vfu_get_poll_fd(vfu), G_IO_IN, vfu_io_cb, NULL);
    return G_SOURCE_REMOVE;
}

static void load_plugin(void)
{
    void *h = dlopen(opt.plugin, RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);   /* never unloaded */
    if (!h) { LOG("dlopen %s: %s", opt.plugin, dlerror()); exit(1); }
    qdp_plugin_v1_fn fn = (qdp_plugin_v1_fn)dlsym(h, QDP_PLUGIN_SYMBOL);
    if (!fn) { LOG("%s: no %s symbol", opt.plugin, QDP_PLUGIN_SYMBOL); exit(1); }
    ops = fn();
    if (!ops || ops->abi_version < QDP_ABI_VERSION_MIN || ops->abi_version > QDP_ABI_VERSION) {
        LOG("%s: ABI %u, host supports %u..%u", opt.plugin, ops ? ops->abi_version : 0, QDP_ABI_VERSION_MIN, QDP_ABI_VERSION);
        exit(1);
    }
    abi = ops->abi_version;
    if (abi >= 2) {
        v2.flags = ops->flags; v2.quiesce = ops->quiesce; v2.resume = ops->resume;
        v2.dma_register = ops->dma_register; v2.dma_unregister = ops->dma_unregister; v2.get_cursor = ops->get_cursor;
    }
    LOG("plugin %s %s (ABI %u%s)", ops->name, ops->version, abi, PLUGIN_DAMAGE ? ", plugin damage" : "");
}

static void setup_vfu(void)
{
    unlink(opt.socket);
    vfu = vfu_create_ctx(VFU_TRANS_SOCK, opt.socket, LIBVFIO_USER_FLAG_ATTACH_NB, NULL, VFU_DEV_TYPE_PCI);
    if (!vfu) { LOG("vfu_create_ctx %s: %s", opt.socket, strerror(errno)); exit(1); }
    qdp_host_env env = { .size = sizeof(env) };
    if (kms) env.edid = kms_edid(kms, &env.edid_len);
    if (abi >= 2) env.host_ops = &host_ops;
    int r = ops->setup(vfu, opt.plugin_args, &env, &priv);
    if (r < 0) { LOG("plugin setup failed: %s", strerror(-r)); exit(1); }
    if (vfu_setup_device_reset_cb(vfu, reset_cb) < 0) { LOG("reset cb: %s", strerror(errno)); exit(1); }
    if (vfu_setup_device_dma(vfu, LIBVFIO_USER_MAX_DMA_REGIONS, dma_register, dma_unregister) < 0) {
        LOG("dma setup: %s", strerror(errno)); exit(1);
    }
    if (v2.quiesce) vfu_setup_device_quiesce_cb(vfu, quiesce_cb);
    if (vfu_realize_ctx(vfu) < 0) { LOG("vfu_realize_ctx: %s", strerror(errno)); exit(1); }
    __atomic_store_n(&vfu_realized, 1, __ATOMIC_RELEASE);
    vfu_watch = g_unix_fd_add(vfu_get_poll_fd(vfu), G_IO_IN, vfu_io_cb, NULL);
    LOG("vfio-user socket %s", opt.socket);
}

/* ---------------------------------------------------------- display core */
typedef struct sink sink_t;
struct sink {
    const char *name;
    void (*scanout)(sink_t *s);                                       /* mode changed or disabled */
    void (*update)(sink_t *s, int x, int y, int w, int h);            /* damage */
    void *priv;
};

static GPtrArray *listeners;   /* listener_t*, Display1 listeners (defined below) */
static struct {
    qdp_scanout cur;        /* current mode (enabled == false: blank) */
    uint8_t *map;           /* mapping of the framebuffer (page aligned base) */
    size_t   map_len;
    uint8_t *pix;           /* first pixel of the scanout */
    uint8_t *shadow;        /* last pixels sent (width*4*height, packed); none with plugin damage */
    bool shadow_stale;
    GPtrArray *sinks;
    uint64_t seq;           /* last presented frame: the plugin's seq, or a count of damaged ticks */
} disp;
static void disp_presented(uint64_t seq);
static bool ctl_shots_pending(void);

static void disp_unmap(void)
{
    if (disp.map) munmap(disp.map, disp.map_len);
    disp.map = NULL; disp.pix = NULL; disp.map_len = 0;
    g_free(disp.shadow); disp.shadow = NULL;
}

static bool disp_map(const qdp_scanout *s)
{
    long page = sysconf(_SC_PAGESIZE);
    size_t aligned = s->offset & ~(page - 1);
    size_t len = (s->offset - aligned) + (size_t)s->stride * s->height;
    uint8_t *m = mmap(NULL, len, PROT_READ, MAP_SHARED, s->fd, aligned);
    if (m == MAP_FAILED) { LOG("mmap scanout: %s", strerror(errno)); return false; }
    disp.map = m; disp.map_len = len; disp.pix = m + (s->offset - aligned);
    if (!PLUGIN_DAMAGE) disp.shadow = g_malloc0((size_t)s->width * 4 * s->height);
    return true;
}

static bool mode_changed(const qdp_scanout *a, const qdp_scanout *b)
{
    return a->enabled != b->enabled || a->fd != b->fd || a->offset != b->offset ||
           a->width != b->width || a->height != b->height || a->stride != b->stride ||
           a->pixman_format != b->pixman_format;
}

/* Compare the live framebuffer with the shadow; return the damage rectangle. */
static bool disp_damage(int *rx, int *ry, int *rw, int *rh)
{
    const qdp_scanout *s = &disp.cur;
    int w = s->width, h = s->height;
    int y0 = -1, y1 = -1, x0 = w, x1 = -1;
    for (int y = 0; y < h; y++) {
        const uint32_t *live = (const uint32_t *)(disp.pix + (size_t)y * s->stride);
        uint32_t *sh = (uint32_t *)(disp.shadow + (size_t)y * w * 4);
        if (memcmp(live, sh, (size_t)w * 4) == 0) continue;
        int a = 0, b = w - 1;
        while (a < w && live[a] == sh[a]) a++;
        while (b > a && live[b] == sh[b]) b--;
        if (y0 < 0) y0 = y;
        y1 = y;
        if (a < x0) x0 = a;
        if (b > x1) x1 = b;
        memcpy(sh, live, (size_t)w * 4);
    }
    if (y0 < 0) return false;
    *rx = x0; *ry = y0; *rw = x1 - x0 + 1; *rh = y1 - y0 + 1;
    return true;
}

static void disp_notify_scanout(void)
{
    for (guint i = 0; i < disp.sinks->len; i++) {
        sink_t *s = g_ptr_array_index(disp.sinks, i);
        if (s->scanout) s->scanout(s);
    }
}

/* Take a new scanout from the plugin; on a mode change tell the sinks. Returns
 * true when the mode changed (the next update must then cover everything). */
static bool disp_set_mode(const qdp_scanout *now)
{
    if (!mode_changed(now, &disp.cur)) return false;
    disp_unmap();
    disp.cur = *now;
    if (now->enabled && !disp_map(now)) disp.cur.enabled = false;
    LOG("scanout %s %ux%u stride %u fmt 0x%x", disp.cur.enabled ? "on" : "off",
        now->width, now->height, now->stride, now->pixman_format);
    disp_notify_scanout();
    return true;
}

static void disp_feed(int x, int y, int w, int h)
{
    for (guint i = 0; i < disp.sinks->len; i++) {
        sink_t *s = g_ptr_array_index(disp.sinks, i);
        if (s->update) s->update(s, x, y, w, h);
    }
}

/* polling path (v1, and v2 without QDP_F_PLUGIN_DAMAGE): damage by comparison */
static gboolean disp_tick(gpointer data)
{
    if (ops->tick && abi < 2) ops->tick(priv);    /* v1 hosts always ticked at the poll rate; v2 honours tick_ms */
    if (!ops->get_scanout) return G_SOURCE_CONTINUE;
    qdp_scanout now;
    if (!ops->get_scanout(priv, &now)) return G_SOURCE_CONTINUE;
    if (disp_set_mode(&now)) {
        if (!disp.cur.enabled) return G_SOURCE_CONTINUE;
        /* first frame: everything is damage */
        memset(disp.shadow, 0xff, (size_t)now.width * 4 * now.height);
    }
    if (!disp.cur.enabled) return G_SOURCE_CONTINUE;
    /* nobody needs damage rectangles: zero-copy KMS reads the memfd itself, no Display1 listener
     * is attached and no screendump waits for a frame */
    if (listeners->len == 0 && !ctl_shots_pending() && (!kms || kms_zero_copy(kms))) { disp.shadow_stale = true; return G_SOURCE_CONTINUE; }
    bool baseline = disp.shadow_stale;   /* the shadow is old: this compare only refreshes it, it is no new frame */
    if (disp.shadow_stale) { memset(disp.shadow, 0xff, (size_t)disp.cur.width * 4 * disp.cur.height); disp.shadow_stale = false; }
    int x, y, w, h;
    double t0 = now_s();
    bool dmg = disp_damage(&x, &y, &w, &h);
    double tc = now_s() - t0;
    tstat.ticks++; tstat.cmp_sum += tc; if (tc > tstat.cmp_max) tstat.cmp_max = tc;
    if (dmg) {
        tstat.damaged++; last_damage_t = t0;
        disp_feed(x, y, w, h);
        if (!baseline) disp_presented(disp.seq + 1);
    }
    /* adaptive polling: drop to --idle-fps after --idle-after seconds without damage */
    if (opt.idle_fps > 0 && opt.idle_fps < opt.fps) {
        bool want_idle = !dmg && (t0 - last_damage_t) > opt.idle_after;
        if (want_idle != poll_idle) {
            poll_idle = want_idle;
            DBG("poll rate -> %d Hz", poll_idle ? opt.idle_fps : opt.fps);
            g_timeout_add(1000 / (poll_idle ? opt.idle_fps : opt.fps), disp_tick, NULL);
            return G_SOURCE_REMOVE;
        }
    }
    return G_SOURCE_CONTINUE;
}

/* plugin damage path (QDP_F_PLUGIN_DAMAGE): one batch of frame_ready() calls */
static void disp_frame(uint64_t seq, bool full, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    qdp_scanout now;
    hstat.frames++;
    if (!ops->get_scanout || !ops->get_scanout(priv, &now)) return;
    if (disp_set_mode(&now)) full = true;
    if (!disp.cur.enabled) return;
    uint32_t W = disp.cur.width, H = disp.cur.height;
    if (full) { x0 = 0; y0 = 0; x1 = W; y1 = H; }
    x1 = MIN(x1, W); y1 = MIN(y1, H);
    if (x0 < x1 && y0 < y1) disp_feed(x0, y0, x1 - x0, y1 - y0);
    disp_presented(seq);
}

/* v2: the initial scanout, once right after setup() (a QDP_F_PLUGIN_DAMAGE
 * plugin is otherwise asked only after frame_ready()) */
static void disp_initial(void)
{
    qdp_scanout now;
    if (abi < 2 || !ops->get_scanout || !ops->get_scanout(priv, &now)) return;
    if (!disp_set_mode(&now) || !disp.cur.enabled) return;
    if (disp.shadow) memset(disp.shadow, 0xff, (size_t)disp.cur.width * 4 * disp.cur.height);   /* polling: first tick sends all */
    else disp_feed(0, 0, disp.cur.width, disp.cur.height);
}

/* v2: plugin tick at its own tick_ms */
static gboolean plugin_tick(gpointer data) { ops->tick(priv); return G_SOURCE_CONTINUE; }

static void print_stats(bool reset)
{
    struct rusage ru; getrusage(RUSAGE_SELF, &ru);
    LOG("stats: ticks %llu (damaged %llu, poll %d Hz), compare avg %.3f ms max %.3f ms, process cpu %.2fs user %.2fs sys",
        (unsigned long long)tstat.ticks, (unsigned long long)tstat.damaged, poll_idle ? opt.idle_fps : opt.fps,
        tstat.ticks ? tstat.cmp_sum / tstat.ticks * 1e3 : 0.0, tstat.cmp_max * 1e3,
        ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6, ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6);
    if (abi >= 2)
        LOG("stats: v2: frames %" PRIu64 " (frame_ready %" PRIu64 "), seq %" PRIu64 ", irqs %" PRIu64 " (dropped %" PRIu64 "), dma %u regions %" PRIu64 " MiB, quiesce %" PRIu64 ", cursor %" PRIu64 "/%" PRIu64,
            hstat.frames, __atomic_load_n(&hstat.frame_calls, __ATOMIC_RELAXED), disp.seq,
            __atomic_load_n(&hstat.irq_ok, __ATOMIC_RELAXED), __atomic_load_n(&hstat.irq_drop, __ATOMIC_RELAXED),
            g_hash_table_size(dma_map), dma_bytes >> 20, hstat.quiesce, hstat.cursor_sent, __atomic_load_n(&hstat.cursor_calls, __ATOMIC_RELAXED));
    if (kms) kms_stats(kms, stderr, reset);
    if (reset) memset(&tstat, 0, sizeof(tstat));
}
static gboolean on_sigusr2(gpointer data) { print_stats(true); return G_SOURCE_CONTINUE; }
static gboolean on_stats_timer(gpointer data) { print_stats(true); return G_SOURCE_CONTINUE; }

/* ---------------------------------------------------------------- KMS sink */
static void kms_sink_scanout(sink_t *s)
{
    kms_scanout(kms, disp.pix, disp.cur.width, disp.cur.height, disp.cur.stride, disp.cur.enabled,
                disp.cur.fd, disp.cur.offset);
}
static void kms_sink_update(sink_t *s, int x, int y, int w, int h)
{
    kms_update(kms, disp.pix, disp.cur.stride, x, y, w, h);
}
/* physical connector hot-plug -> run the hooks (e.g. tbctl.py plug/unplug) */
static gboolean kms_hotplug_poll(gpointer data)
{
    static char last[32];
    const char *st = kms_connector_status(kms);
    if (last[0] && strcmp(last, st) != 0) {
        LOG("%s: %s -> %s", kms_connector_name(kms), last, st);
        const char *cmd = !strcmp(st, "connected") ? opt.on_connect : (!strcmp(st, "disconnected") ? opt.on_disconnect : NULL);
        if (cmd) { g_autoptr(GError) err = NULL; if (!g_spawn_command_line_async(cmd, &err)) LOG("hook: %s", err->message); }
    }
    g_strlcpy(last, st, sizeof(last));
    return G_SOURCE_CONTINUE;
}
/* Teardown is the last call into the plugin; the plugin is never dlclose()d.
 * After a v2 teardown the process leaves with _exit(): threads the plugin
 * could not join (GPU driver, QMetal) must not race library destructors. */
static void host_exit(int status)
{
    if (kms) kms_close(kms);
    kms = NULL;
    if (ops && ops->teardown) ops->teardown(priv);
    if (abi >= 2) { fflush(NULL); _exit(status); }
    exit(status);
}
static gboolean on_term(gpointer data) { LOG("terminating"); host_exit(0); return G_SOURCE_REMOVE; }

/* --------------------------------------------------------------- PPM sink */
static unsigned dump_count;
/* Write the current scanout as binary PPM (P6), through FILE.tmp + rename so a
 * reader never sees a partial file. 32 bpp pixman formats of type ARGB, ABGR,
 * BGRA and RGBA (alpha/padding ignored). */
static bool write_ppm(const char *path)
{
    const qdp_scanout *s = &disp.cur;
    if (!s->enabled || !disp.pix) { errno = ENODATA; return false; }
    uint32_t type = (s->pixman_format >> 16) & 0xff;
    int rs, gs, bs;
    switch (type) {
    case 2: rs = 16; gs = 8; bs = 0; break;      /* ARGB: x8r8g8b8, a8r8g8b8 */
    case 3: rs = 0; gs = 8; bs = 16; break;      /* ABGR */
    case 8: rs = 8; gs = 16; bs = 24; break;     /* BGRA */
    case 9: rs = 24; gs = 16; bs = 8; break;     /* RGBA */
    default: errno = ENOTSUP; return false;
    }
    if ((s->pixman_format >> 24) != 32) { errno = ENOTSUP; return false; }
    g_autofree char *tmp = g_strdup_printf("%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    fprintf(f, "P6\n%u %u\n255\n", s->width, s->height);
    g_autofree uint8_t *row = g_malloc((size_t)s->width * 3);
    for (uint32_t y = 0; y < s->height; y++) {
        const uint32_t *px = (const uint32_t *)(disp.pix + (size_t)y * s->stride);
        for (uint32_t x = 0; x < s->width; x++) {
            uint32_t v = px[x];
            row[x * 3] = v >> rs; row[x * 3 + 1] = v >> gs; row[x * 3 + 2] = v >> bs;
        }
        fwrite(row, 3, s->width, f);
    }
    if (fclose(f) != 0 || rename(tmp, path) != 0) { int e = errno; unlink(tmp); errno = e; return false; }
    return true;
}
static void dump_ppm(void)
{
    const qdp_scanout *s = &disp.cur;
    if (!s->enabled) { LOG("dump: scanout disabled"); return; }
    g_autofree char *name = g_strdup_printf("%s/qdev-fb-%u.ppm", opt.dump_dir, dump_count++);
    if (!write_ppm(name)) { LOG("dump: %s: %s", name, strerror(errno)); return; }
    LOG("dump: %s (%ux%u)", name, s->width, s->height);
}
static gboolean on_sigusr1(gpointer data)
{
    dump_ppm();
    if (kms) {
        g_autofree char *name = g_strdup_printf("%s/qdev-kms-%u.ppm", opt.dump_dir, dump_count);
        if (kms_dump_front(kms, name)) LOG("dump: %s (scanned-out buffer)", name);
    }
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------- control socket (--control)
 * Line protocol on a unix stream socket, one reply line per command, for test
 * harnesses (QMP screendump cannot see the display of an external device):
 *   screendump FILE  wait for the next presented frame (at most 1 s), write the
 *                    scanout as PPM P6 -> "ok <seq> <w>x<h>"
 *   frame            -> "ok <seq> <w>x<h> stride <n> format 0x<pixman>" or "ok <seq> off"
 *   stats            -> "ok key=value ..."
 * Errors: "err <reason>". A client's commands are answered in order: while its
 * screendump waits, its later lines stay queued. */
typedef struct { int fd; guint watch; GString *in; char *shot; guint shot_timer; } ctl_t;
static GPtrArray *ctls;
static double last_present_t;

static void ctl_reply(ctl_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void ctl_reply(ctl_t *c, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    g_autofree char *body = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    g_autofree char *line = g_strconcat(body, "\n", NULL);
    if (send(c->fd, line, strlen(line), MSG_NOSIGNAL) < 0) DBG("control: send: %s", strerror(errno));
}
static bool ctl_shots_pending(void)
{
    for (guint i = 0; ctls && i < ctls->len; i++) if (((ctl_t *)g_ptr_array_index(ctls, i))->shot) return true;
    return false;
}
static void ctl_process(ctl_t *c);
static void ctl_shot_done(ctl_t *c)
{
    const qdp_scanout *s = &disp.cur;
    if (!s->enabled) ctl_reply(c, "err scanout off");
    else if (!write_ppm(c->shot)) ctl_reply(c, "err %s: %s", c->shot, strerror(errno));
    else ctl_reply(c, "ok %" PRIu64 " %ux%u", disp.seq, s->width, s->height);
    g_clear_pointer(&c->shot, g_free);
    if (c->shot_timer) { g_source_remove(c->shot_timer); c->shot_timer = 0; }
    ctl_process(c);
}
static gboolean ctl_shot_timeout(gpointer data)
{
    ctl_t *c = data;
    c->shot_timer = 0;               /* idle: take what is there */
    ctl_shot_done(c);
    return G_SOURCE_REMOVE;
}
/* a frame went to the sinks: complete the screendumps that waited for it */
static void disp_presented(uint64_t seq)
{
    disp.seq = seq; last_present_t = now_s();
    if (!ctls) return;
    g_autoptr(GPtrArray) done = g_ptr_array_new();
    for (guint i = 0; i < ctls->len; i++) { ctl_t *c = g_ptr_array_index(ctls, i); if (c->shot) g_ptr_array_add(done, c); }
    for (guint i = 0; i < done->len; i++) ctl_shot_done(g_ptr_array_index(done, i));
}
static void ctl_command(ctl_t *c, char *line)
{
    const qdp_scanout *s = &disp.cur;
    if (!*line) return;
    if (!strncmp(line, "screendump", 10) && (line[10] == ' ' || line[10] == 0)) {
        char *path = g_strstrip(line + 10);
        if (!*path) { ctl_reply(c, "err usage: screendump FILE"); return; }
        c->shot = g_strdup(path);
        c->shot_timer = g_timeout_add(1000, ctl_shot_timeout, c);
    } else if (!strcmp(line, "frame")) {
        if (s->enabled) ctl_reply(c, "ok %" PRIu64 " %ux%u stride %u format 0x%08x", disp.seq, s->width, s->height, s->stride, s->pixman_format);
        else ctl_reply(c, "ok %" PRIu64 " off", disp.seq);
    } else if (!strcmp(line, "stats")) {
        struct rusage ru; getrusage(RUSAGE_SELF, &ru);
        ctl_reply(c, "ok seq=%" PRIu64 " mode=%ux%u frames=%" PRIu64 " frame_calls=%" PRIu64 " ticks=%" PRIu64 " damaged=%" PRIu64
                  " idle_s=%.1f irqs=%" PRIu64 " irqs_dropped=%" PRIu64 " dma_regions=%u dma_mib=%" PRIu64 " dma_maps=%" PRIu64
                  " dma_unmaps=%" PRIu64 " quiesce=%" PRIu64 " cursor_calls=%" PRIu64 " cursor_sent=%" PRIu64 " listeners=%u"
                  " attached=%d cpu_s=%.2f",
                  disp.seq, s->enabled ? s->width : 0, s->enabled ? s->height : 0, hstat.frames,
                  __atomic_load_n(&hstat.frame_calls, __ATOMIC_RELAXED), tstat.ticks, tstat.damaged,
                  last_present_t ? now_s() - last_present_t : -1.0,
                  __atomic_load_n(&hstat.irq_ok, __ATOMIC_RELAXED), __atomic_load_n(&hstat.irq_drop, __ATOMIC_RELAXED),
                  dma_map ? g_hash_table_size(dma_map) : 0, dma_bytes >> 20, hstat.dma_maps, hstat.dma_unmaps, hstat.quiesce,
                  __atomic_load_n(&hstat.cursor_calls, __ATOMIC_RELAXED), hstat.cursor_sent, listeners->len, attached,
                  ru.ru_utime.tv_sec + ru.ru_stime.tv_sec + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6);
    } else {
        ctl_reply(c, "err unknown command (screendump FILE | frame | stats)");
    }
}
static void ctl_process(ctl_t *c)
{
    char *nl;
    while (!c->shot && (nl = memchr(c->in->str, '\n', c->in->len))) {
        size_t len = nl - c->in->str;
        g_autofree char *line = g_strndup(c->in->str, len);
        g_string_erase(c->in, 0, len + 1);
        ctl_command(c, g_strstrip(line));
    }
}
static void ctl_free(ctl_t *c)
{
    if (c->watch) g_source_remove(c->watch);
    if (c->shot_timer) g_source_remove(c->shot_timer);
    close(c->fd); g_string_free(c->in, TRUE); g_free(c->shot);
    g_ptr_array_remove(ctls, c); g_free(c);
}
static gboolean ctl_read(gint fd, GIOCondition cond, gpointer data)
{
    ctl_t *c = data; char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) return G_SOURCE_CONTINUE;
    if (n <= 0 || c->in->len + n > 65536) { DBG("control client gone"); c->watch = 0; ctl_free(c); return G_SOURCE_REMOVE; }
    g_string_append_len(c->in, buf, n);
    ctl_process(c);
    return G_SOURCE_CONTINUE;
}
static gboolean ctl_accept(gint fd, GIOCondition cond, gpointer data)
{
    int cfd = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
    if (cfd < 0) { if (errno != EAGAIN && errno != EINTR) LOG("control accept: %s", strerror(errno)); return G_SOURCE_CONTINUE; }
    ctl_t *c = g_new0(ctl_t, 1);
    c->fd = cfd; c->in = g_string_new(NULL);
    c->watch = g_unix_fd_add(cfd, G_IO_IN | G_IO_HUP | G_IO_ERR, ctl_read, c);
    g_ptr_array_add(ctls, c);
    DBG("control client connected");
    return G_SOURCE_CONTINUE;
}
static void setup_control(void)
{
    if (!opt.control) return;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (strlen(opt.control) >= sizeof(a.sun_path)) { LOG("--control path too long"); exit(1); }
    strcpy(a.sun_path, opt.control);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    unlink(opt.control);
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 8) < 0) {
        LOG("control socket %s: %s", opt.control, strerror(errno)); exit(1);
    }
    ctls = g_ptr_array_new();
    g_unix_fd_add(fd, G_IO_IN, ctl_accept, NULL);
    LOG("control socket %s", opt.control);
}

/* ------------------------------------------------------ Display1 listener */
typedef struct {
    GDBusConnection *conn;
    QemuDBusDisplay1Listener *proxy;
    QemuDBusDisplay1ListenerUnixMap *map_proxy;
    bool can_map;     /* listener implements org.qemu.Display1.Listener.Unix.Map */
    bool mapped;      /* ScanoutMap of the current scanout succeeded */
    bool scanout_sent;
} listener_t;

static void listener_free(listener_t *l)
{
    g_clear_object(&l->map_proxy);
    g_clear_object(&l->proxy);
    g_clear_object(&l->conn);
    g_free(l);
}

static void listener_closed(GDBusConnection *c, gboolean remote, GError *err, gpointer data)
{
    listener_t *l = data;
    LOG("listener gone");
    g_ptr_array_remove(listeners, l);
    listener_free(l);
}

static GVariant *packed_rect(int x, int y, int w, int h)
{
    const qdp_scanout *s = &disp.cur;
    size_t len = (size_t)w * 4 * h;
    uint8_t *buf = g_malloc(len);
    for (int r = 0; r < h; r++)
        memcpy(buf + (size_t)r * w * 4, disp.pix + (size_t)(y + r) * s->stride + (size_t)x * 4, (size_t)w * 4);
    return g_variant_new_from_data(G_VARIANT_TYPE("ay"), buf, len, TRUE, g_free, buf);
}

static void listener_send_scanout(listener_t *l)
{
    const qdp_scanout *s = &disp.cur;
    g_autoptr(GError) err = NULL;
    l->mapped = false;
    if (!s->enabled) {
        qemu_dbus_display1_listener_call_disable(l->proxy, G_DBUS_CALL_FLAGS_NONE, DBUS_TIMEOUT, NULL, NULL, NULL);
        l->scanout_sent = true;
        return;
    }
    if (l->can_map) {
        g_autoptr(GUnixFDList) fds = g_unix_fd_list_new();
        if (g_unix_fd_list_append(fds, s->fd, &err) < 0) {
            LOG("fd list: %s", err->message); g_clear_error(&err);
        } else if (qemu_dbus_display1_listener_unix_map_call_scanout_map_sync(
                       l->map_proxy, g_variant_new_handle(0), s->offset, s->width, s->height,
                       s->stride, s->pixman_format, G_DBUS_CALL_FLAGS_NONE, DBUS_TIMEOUT,
                       fds, NULL, NULL, &err)) {
            l->mapped = true; l->scanout_sent = true;
            DBG("ScanoutMap sent (%ux%u)", s->width, s->height);
            /* the shared mapping already holds the pixels; tell the listener they are valid */
            qemu_dbus_display1_listener_unix_map_call_update_map(l->map_proxy, 0, 0, s->width, s->height,
                G_DBUS_CALL_FLAGS_NONE, DBUS_TIMEOUT, NULL, NULL, NULL);
            return;
        } else {
            LOG("ScanoutMap failed (%s), falling back to inline Scanout", err->message);
            g_clear_error(&err); l->can_map = false;
        }
    }
    if (!qemu_dbus_display1_listener_call_scanout_sync(l->proxy, s->width, s->height, s->width * 4,
            s->pixman_format, packed_rect(0, 0, s->width, s->height),
            G_DBUS_CALL_FLAGS_NONE, DBUS_TIMEOUT, NULL, &err)) {
        LOG("Scanout failed: %s", err->message);
        return;
    }
    l->scanout_sent = true;
    DBG("Scanout sent inline (%ux%u)", s->width, s->height);
}

static void listener_send_update(listener_t *l, int x, int y, int w, int h)
{
    if (!l->scanout_sent) { listener_send_scanout(l); return; }
    if (!disp.cur.enabled) return;
    if (l->mapped) {
        qemu_dbus_display1_listener_unix_map_call_update_map(l->map_proxy, x, y, w, h,
            G_DBUS_CALL_FLAGS_NONE, DBUS_TIMEOUT, NULL, NULL, NULL);
    } else {
        qemu_dbus_display1_listener_call_update(l->proxy, x, y, w, h, w * 4, disp.cur.pixman_format,
            packed_rect(x, y, w, h), G_DBUS_CALL_FLAGS_NONE, DBUS_TIMEOUT, NULL, NULL, NULL);
    }
}

/* Cursor plane (v2 get_cursor): CursorDefine when the image changes (serial),
 * MouseSet with the pointer position, i.e. the image's top-left plus hotspot,
 * when anything changes. The last state is kept for listeners that come later. */
static struct {
    bool have, visible;
    int32_t x, y;
    uint32_t hot_x, hot_y, w, h;
    uint64_t serial;
    GBytes *img;            /* w*h*4 bytes, ARGB8888 host order */
} cur;

static void listener_send_cursor(listener_t *l, bool define)
{
    if (define && cur.img)
        qemu_dbus_display1_listener_call_cursor_define(l->proxy, cur.w, cur.h, cur.hot_x, cur.hot_y,
            g_variant_new_from_bytes(G_VARIANT_TYPE_BYTESTRING, cur.img, TRUE),
            G_DBUS_CALL_FLAGS_NONE, DBUS_TIMEOUT, NULL, NULL, NULL);
    qemu_dbus_display1_listener_call_mouse_set(l->proxy, cur.x + (int32_t)cur.hot_x, cur.y + (int32_t)cur.hot_y,
        cur.visible, G_DBUS_CALL_FLAGS_NONE, DBUS_TIMEOUT, NULL, NULL, NULL);
}

static void cursor_update(void)
{
    qdp_cursor c;
    if (!v2.get_cursor || !v2.get_cursor(priv, &c)) return;
    bool define = !cur.have || c.serial != cur.serial;
    if (define) {
        g_clear_pointer(&cur.img, g_bytes_unref);
        if (c.pixels && c.width && c.height && c.width <= 1024 && c.height <= 1024)
            cur.img = g_bytes_new(c.pixels, (size_t)c.width * c.height * 4);
        cur.w = c.width; cur.h = c.height; cur.hot_x = c.hot_x; cur.hot_y = c.hot_y; cur.serial = c.serial;
    }
    bool changed = define || c.visible != cur.visible || c.x != cur.x || c.y != cur.y;
    cur.visible = c.visible; cur.x = c.x; cur.y = c.y; cur.have = true;
    if (!changed) return;
    for (guint i = 0; i < listeners->len; i++) listener_send_cursor(g_ptr_array_index(listeners, i), define);
    hstat.cursor_sent++;
}

/* main loop side of frame_ready()/cursor_changed() */
static gboolean wake_cb(gint fd, GIOCondition cond, gpointer data)
{
    uint64_t v;
    if (read(wake_fd, &v, sizeof(v)) < 0 && errno != EAGAIN) LOG("wake read: %s", strerror(errno));
    pthread_mutex_lock(&hs_lock);
    bool frame = hs.frame, cursor = hs.cursor, full = hs.full;
    uint64_t seq = hs.seq; uint32_t x0 = hs.x0, y0 = hs.y0, x1 = hs.x1, y1 = hs.y1;
    hs.frame = hs.cursor = hs.full = false;
    pthread_mutex_unlock(&hs_lock);
    if (frame) {
        if (PLUGIN_DAMAGE) disp_frame(seq, full, x0, y0, x1, y1);
        else DBG("frame_ready ignored: the plugin did not set QDP_F_PLUGIN_DAMAGE");
    }
    if (cursor) cursor_update();
    return G_SOURCE_CONTINUE;
}

static void display1_sink_scanout(sink_t *s)
{
    for (guint i = 0; i < listeners->len; i++) listener_send_scanout(g_ptr_array_index(listeners, i));
}
static void display1_sink_update(sink_t *s, int x, int y, int w, int h)
{
    for (guint i = 0; i < listeners->len; i++) listener_send_update(g_ptr_array_index(listeners, i), x, y, w, h);
}

/* ------------------------------------------------ Display1 server objects */
typedef struct {
    GDBusConnection *conn;
    GDBusObjectManagerServer *mgr;
    QemuDBusDisplay1Console *console;
    QemuDBusDisplay1Mouse *mouse;
} export_t;
static GPtrArray *exports;   /* export_t*, one per connection serving the objects */

static gboolean handle_register_listener(QemuDBusDisplay1Console *object, GDBusMethodInvocation *inv,
                                         GUnixFDList *fd_list, GVariant *arg_listener, gpointer data)
{
    g_autoptr(GError) err = NULL;
    int fd = g_unix_fd_list_get(fd_list, g_variant_get_handle(arg_listener), &err);
    if (err) {
        g_dbus_method_invocation_return_dbus_error(inv, "org.qemu.Display1.Error.Failed", err->message);
        return TRUE;
    }
    g_autoptr(GSocket) sock = g_socket_new_from_fd(fd, &err);
    if (!sock) {
        close(fd);
        g_dbus_method_invocation_return_dbus_error(inv, "org.qemu.Display1.Error.Failed", err->message);
        return TRUE;
    }
    g_autoptr(GSocketConnection) sconn = g_socket_connection_factory_create_connection(sock);
    qemu_dbus_display1_console_complete_register_listener(object, inv, NULL);

    g_autofree char *guid = g_dbus_generate_guid();
    GDBusConnection *conn = g_dbus_connection_new_sync(G_IO_STREAM(sconn), guid,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_SERVER, NULL, NULL, &err);
    if (!conn) { LOG("listener connection: %s", err->message); return TRUE; }

    listener_t *l = g_new0(listener_t, 1);
    l->conn = conn;
    l->proxy = qemu_dbus_display1_listener_proxy_new_sync(conn, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
        NULL, DISPLAY1_ROOT "/Listener", NULL, &err);
    if (!l->proxy) { LOG("listener proxy: %s", err->message); listener_free(l); return TRUE; }
    const gchar *const *ifaces = qemu_dbus_display1_listener_get_interfaces(l->proxy);
    if (ifaces && g_strv_contains(ifaces, "org.qemu.Display1.Listener.Unix.Map")) {
        l->map_proxy = qemu_dbus_display1_listener_unix_map_proxy_new_sync(conn,
            G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, NULL, DISPLAY1_ROOT "/Listener", NULL, &err);
        l->can_map = l->map_proxy != NULL;
        if (!l->map_proxy) { LOG("map proxy: %s", err->message); g_clear_error(&err); }
    }
    g_signal_connect(conn, "closed", G_CALLBACK(listener_closed), l);
    g_ptr_array_add(listeners, l);
    LOG("listener registered (Unix.Map: %s)", l->can_map ? "yes" : "no");
    listener_send_scanout(l);
    if (cur.have) listener_send_cursor(l, true);
    return TRUE;
}

static gboolean handle_set_uiinfo(QemuDBusDisplay1Console *o, GDBusMethodInvocation *inv, guint16 wmm, guint16 hmm,
                                  gint xoff, gint yoff, guint w, guint h, gpointer data)
{
    DBG("SetUIInfo %ux%u (ignored: the guest chooses the mode)", w, h);
    qemu_dbus_display1_console_complete_set_uiinfo(o, inv);
    return TRUE;
}
/* Input forwarding: Keyboard/Mouse of the exported console are relayed to the
 * VM's own QEMU Display1 console (--qemu-display), where the real input
 * devices live. Absolute pointer positions are rescaled from this display's
 * size to that console's size. Without --qemu-display events are dropped. */
static struct {
    GDBusConnection *conn;
    QemuDBusDisplay1VM *vm;
    QemuDBusDisplay1Console *console;
    QemuDBusDisplay1Keyboard *kbd;
    QemuDBusDisplay1Mouse *mouse;
    QemuDBusDisplay1Audio *audio;             /* VM's Audio (NULL if absent) */
    QemuDBusDisplay1Clipboard *clip_to_qemu;  /* VM's Clipboard (NULL if absent) */
    QemuDBusDisplay1Clipboard *clip_from_qemu;/* our Clipboard object on the VM connection: QEMU calls us here */
    uint64_t forwarded;
} fwd;
/* the one Display1 client (QSM worker) that registered for clipboard events */
static struct { GDBusConnection *conn; char *name; QemuDBusDisplay1Clipboard *proxy; bool registered; } clip_client;

/* QEMU does not list Audio/Clipboard in VM.Interfaces; ask the object itself */
static bool vm_has(const char *iface)
{
    const char *dest = (opt.qemu_display_dest && *opt.qemu_display_dest) ? opt.qemu_display_dest : NULL;
    g_autofree char *path = g_strdup_printf(DISPLAY1_ROOT "/%s", strrchr(iface, '.') + 1);
    g_autoptr(GVariant) r = g_dbus_connection_call_sync(fwd.conn, dest, path, "org.freedesktop.DBus.Introspectable",
        "Introspect", NULL, G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, DBUS_TIMEOUT, NULL, NULL);
    if (!r) return false;
    const char *xml; g_variant_get(r, "(&s)", &xml);
    return strstr(xml, iface) != NULL;
}

/* All relays are asynchronous: QEMU answers a Register with a GetAll on our
 * object, and a Request may bounce back to us, so our main loop must never
 * block on a call to QEMU (or to the client). */
typedef struct { GDBusMethodInvocation *inv; gpointer obj; } relay_t;
static relay_t *relay_new(gpointer obj, GDBusMethodInvocation *inv) { relay_t *r = g_new0(relay_t, 1); r->obj = obj; r->inv = inv; return r; }

static void relay_done_void(GObject *src, GAsyncResult *res, gpointer data)
{
    relay_t *r = data; g_autoptr(GError) err = NULL;
    g_autoptr(GVariant) v = g_dbus_proxy_call_finish(G_DBUS_PROXY(src), res, &err);
    if (!v) { LOG("relay %s: %s", g_dbus_method_invocation_get_method_name(r->inv), err->message);
              g_dbus_method_invocation_return_dbus_error(r->inv, "org.qemu.Display1.Error.Failed", err->message); }
    else g_dbus_method_invocation_return_value(r->inv, NULL);
    g_free(r);
}
static void relay_done_request(GObject *src, GAsyncResult *res, gpointer data)
{
    relay_t *r = data; g_autoptr(GError) err = NULL; g_autofree char *mime = NULL; GVariant *d = NULL;
    if (!qemu_dbus_display1_clipboard_call_request_finish(QEMU_DBUS_DISPLAY1_CLIPBOARD(src), &mime, &d, res, &err)) {
        LOG("relay Request: %s", err->message);
        g_dbus_method_invocation_return_dbus_error(r->inv, "org.qemu.Display1.Error.Failed", err->message);
    } else qemu_dbus_display1_clipboard_complete_request(r->obj, r->inv, mime, d);
    g_free(r);
}
#define RELAY_FAIL(inv, msg) do { g_dbus_method_invocation_return_dbus_error(inv, "org.qemu.Display1.Error.Failed", msg); return TRUE; } while (0)

/* ---- clipboard: QEMU -> client (callbacks QEMU makes on our object on the VM bus) */
static gboolean qclip_grab(QemuDBusDisplay1Clipboard *o, GDBusMethodInvocation *inv, guint sel, guint serial, const gchar *const *mimes, gpointer d)
{
    if (!clip_client.proxy) RELAY_FAIL(inv, "no clipboard client");
    qemu_dbus_display1_clipboard_call_grab(clip_client.proxy, sel, serial, mimes, G_DBUS_CALL_FLAGS_NONE, DBUS_TIMEOUT, NULL, relay_done_void, relay_new(o, inv));
    return TRUE;
}
static gboolean qclip_release(QemuDBusDisplay1Clipboard *o, GDBusMethodInvocation *inv, guint sel, gpointer d)
{
    if (!clip_client.proxy) RELAY_FAIL(inv, "no clipboard client");
    qemu_dbus_display1_clipboard_call_release(clip_client.proxy, sel, G_DBUS_CALL_FLAGS_NONE, DBUS_TIMEOUT, NULL, relay_done_void, relay_new(o, inv));
    return TRUE;
}
static gboolean qclip_request(QemuDBusDisplay1Clipboard *o, GDBusMethodInvocation *inv, guint sel, const gchar *const *mimes, gpointer d)
{
    if (!clip_client.proxy) RELAY_FAIL(inv, "no clipboard client");
    qemu_dbus_display1_clipboard_call_request(clip_client.proxy, sel, mimes, G_DBUS_CALL_FLAGS_NONE, DBUS_TIMEOUT, NULL, relay_done_request, relay_new(o, inv));
    return TRUE;
}
/* QEMU also calls Register/Unregister on its peer's object: acknowledge */
static gboolean qclip_noop(QemuDBusDisplay1Clipboard *o, GDBusMethodInvocation *inv, gpointer d)
{ g_dbus_method_invocation_return_value(inv, NULL); return TRUE; }

/* ---- clipboard: client -> QEMU (our exported Clipboard object) */
static void clip_client_set(GDBusMethodInvocation *inv)
{
    GDBusConnection *c = g_dbus_method_invocation_get_connection(inv);
    const char *sender = g_dbus_method_invocation_get_sender(inv);
    if (clip_client.conn == c && !g_strcmp0(clip_client.name, sender)) return;
    g_clear_object(&clip_client.proxy); g_clear_object(&clip_client.conn); g_clear_pointer(&clip_client.name, g_free);
    clip_client.conn = g_object_ref(c); clip_client.name = g_strdup(sender);
    clip_client.proxy = qemu_dbus_display1_clipboard_proxy_new_sync(c, G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES, sender, DISPLAY1_ROOT "/Clipboard", NULL, NULL);
}
static void relay_done_register(GObject *src, GAsyncResult *res, gpointer data)
{
    relay_t *r = data; g_autoptr(GError) err = NULL;
    g_autoptr(GVariant) v = g_dbus_proxy_call_finish(G_DBUS_PROXY(src), res, &err);
    if (!v) { LOG("clipboard Register at the VM: %s", err->message); g_dbus_method_invocation_return_dbus_error(r->inv, "org.qemu.Display1.Error.Failed", err->message); }
    else { clip_client.registered = true; LOG("clipboard client registered, relayed to the VM"); g_dbus_method_invocation_return_value(r->inv, NULL); }
    g_free(r);
}
static gboolean cclip_register(QemuDBusDisplay1Clipboard *o, GDBusMethodInvocation *inv, gpointer d)
{
    clip_client_set(inv);
    if (clip_client.registered) { qemu_dbus_display1_clipboard_complete_register(o, inv); return TRUE; }
    qemu_dbus_display1_clipboard_call_register(fwd.clip_to_qemu, G_DBUS_CALL_FLAGS_NONE, DBUS_TIMEOUT, NULL, relay_done_register, relay_new(o, inv));
    return TRUE;
}
static gboolean cclip_unregister(QemuDBusDisplay1Clipboard *o, GDBusMethodInvocation *inv, gpointer d)
{
    if (clip_client.registered) { qemu_dbus_display1_clipboard_call_unregister(fwd.clip_to_qemu, G_DBUS_CALL_FLAGS_NONE, DBUS_TIMEOUT, NULL, NULL, NULL); clip_client.registered = false; }
    g_clear_object(&clip_client.proxy); g_clear_object(&clip_client.conn); g_clear_pointer(&clip_client.name, g_free);
    qemu_dbus_display1_clipboard_complete_unregister(o, inv); return TRUE;
}
static gboolean cclip_grab(QemuDBusDisplay1Clipboard *o, GDBusMethodInvocation *inv, guint sel, guint serial, const gchar *const *mimes, gpointer d)
{
    qemu_dbus_display1_clipboard_call_grab(fwd.clip_to_qemu, sel, serial, mimes, G_DBUS_CALL_FLAGS_NONE, DBUS_TIMEOUT, NULL, relay_done_void, relay_new(o, inv));
    return TRUE;
}
static gboolean cclip_release(QemuDBusDisplay1Clipboard *o, GDBusMethodInvocation *inv, guint sel, gpointer d)
{
    qemu_dbus_display1_clipboard_call_release(fwd.clip_to_qemu, sel, G_DBUS_CALL_FLAGS_NONE, DBUS_TIMEOUT, NULL, relay_done_void, relay_new(o, inv));
    return TRUE;
}
static gboolean cclip_request(QemuDBusDisplay1Clipboard *o, GDBusMethodInvocation *inv, guint sel, const gchar *const *mimes, gpointer d)
{
    qemu_dbus_display1_clipboard_call_request(fwd.clip_to_qemu, sel, mimes, G_DBUS_CALL_FLAGS_NONE, DBUS_TIMEOUT, NULL, relay_done_request, relay_new(o, inv));
    return TRUE;
}

/* ---- audio: the listener fd is handed straight to the VM's QEMU, which then talks to the client directly */
static void relay_done_audio(GObject *src, GAsyncResult *res, gpointer data)
{
    relay_t *r = data; g_autoptr(GError) err = NULL;
    g_autoptr(GVariant) v = g_dbus_proxy_call_with_unix_fd_list_finish(G_DBUS_PROXY(src), NULL, res, &err);
    if (!v) { LOG("audio listener relay: %s", err->message); g_dbus_method_invocation_return_dbus_error(r->inv, "org.qemu.Display1.Error.Failed", err->message); }
    else { LOG("audio listener relayed to the VM"); g_dbus_method_invocation_return_value_with_unix_fd_list(r->inv, NULL, NULL); }
    g_free(r);
}
static gboolean audio_reg(QemuDBusDisplay1Audio *o, GDBusMethodInvocation *inv, GUnixFDList *fds, GVariant *h, bool out)
{
    if (out) qemu_dbus_display1_audio_call_register_out_listener(fwd.audio, h, G_DBUS_CALL_FLAGS_NONE, DBUS_TIMEOUT, fds, NULL, relay_done_audio, relay_new(o, inv));
    else qemu_dbus_display1_audio_call_register_in_listener(fwd.audio, h, G_DBUS_CALL_FLAGS_NONE, DBUS_TIMEOUT, fds, NULL, relay_done_audio, relay_new(o, inv));
    return TRUE;
}
static gboolean audio_reg_out(QemuDBusDisplay1Audio *o, GDBusMethodInvocation *inv, GUnixFDList *fds, GVariant *h, gpointer d) { return audio_reg(o, inv, fds, h, true); }
static gboolean audio_reg_in(QemuDBusDisplay1Audio *o, GDBusMethodInvocation *inv, GUnixFDList *fds, GVariant *h, gpointer d) { return audio_reg(o, inv, fds, h, false); }

static void setup_input_forwarding(void)
{
    if (!opt.qemu_display) return;
    g_autoptr(GError) err = NULL;
    const char *dest = (opt.qemu_display_dest && *opt.qemu_display_dest) ? opt.qemu_display_dest : NULL;
    GDBusConnectionFlags fl = G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | (dest ? G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION : 0);
    fwd.conn = g_dbus_connection_new_for_address_sync(opt.qemu_display, fl, NULL, NULL, &err);
    if (!fwd.conn) { LOG("qemu display %s: %s", opt.qemu_display, err->message); exit(1); }
    g_autofree char *path = g_strdup_printf(DISPLAY1_ROOT "/Console_%d", opt.qemu_console);
    fwd.console = qemu_dbus_display1_console_proxy_new_sync(fwd.conn, G_DBUS_PROXY_FLAGS_NONE, dest, path, NULL, &err);
    if (!fwd.console) { LOG("qemu console proxy: %s", err->message); exit(1); }
    fwd.kbd = qemu_dbus_display1_keyboard_proxy_new_sync(fwd.conn, G_DBUS_PROXY_FLAGS_NONE, dest, path, NULL, &err);
    if (!fwd.kbd) { LOG("qemu keyboard proxy: %s", err->message); exit(1); }
    fwd.mouse = qemu_dbus_display1_mouse_proxy_new_sync(fwd.conn, G_DBUS_PROXY_FLAGS_NONE, dest, path, NULL, &err);
    if (!fwd.mouse) { LOG("qemu mouse proxy: %s", err->message); exit(1); }
    fwd.vm = qemu_dbus_display1_vm_proxy_new_sync(fwd.conn, G_DBUS_PROXY_FLAGS_NONE, dest, DISPLAY1_ROOT "/VM", NULL, &err);
    if (!fwd.vm) { LOG("qemu VM proxy: %s", err->message); g_clear_error(&err); }
    if (vm_has("org.qemu.Display1.Audio")) {
        fwd.audio = qemu_dbus_display1_audio_proxy_new_sync(fwd.conn, G_DBUS_PROXY_FLAGS_NONE, dest, DISPLAY1_ROOT "/Audio", NULL, &err);
        if (!fwd.audio) { LOG("qemu audio proxy: %s", err->message); g_clear_error(&err); }
    }
    if (vm_has("org.qemu.Display1.Clipboard")) {
        fwd.clip_to_qemu = qemu_dbus_display1_clipboard_proxy_new_sync(fwd.conn, G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES, dest, DISPLAY1_ROOT "/Clipboard", NULL, &err);
        if (!fwd.clip_to_qemu) { LOG("qemu clipboard proxy: %s", err->message); g_clear_error(&err); }
        /* QEMU calls back the sender's /org/qemu/Display1/Clipboard: export ours on this connection */
        fwd.clip_from_qemu = qemu_dbus_display1_clipboard_skeleton_new();
        g_signal_connect(fwd.clip_from_qemu, "handle-grab", G_CALLBACK(qclip_grab), NULL);
        g_signal_connect(fwd.clip_from_qemu, "handle-release", G_CALLBACK(qclip_release), NULL);
        g_signal_connect(fwd.clip_from_qemu, "handle-request", G_CALLBACK(qclip_request), NULL);
        g_signal_connect(fwd.clip_from_qemu, "handle-register", G_CALLBACK(qclip_noop), NULL);
        g_signal_connect(fwd.clip_from_qemu, "handle-unregister", G_CALLBACK(qclip_noop), NULL);
        if (!g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(fwd.clip_from_qemu), fwd.conn, DISPLAY1_ROOT "/Clipboard", &err)) {
            LOG("export clipboard on the VM bus: %s", err->message); g_clear_error(&err);
        }
    }
    LOG("VM interfaces: audio %s, clipboard %s", fwd.audio ? "proxied" : "none", fwd.clip_to_qemu ? "proxied" : "none");
    LOG("input forwarded to %s %s%s (console %ux%u, %s pointer)", opt.qemu_display, dest ? dest : "p2p", path,
        qemu_dbus_display1_console_get_width(fwd.console), qemu_dbus_display1_console_get_height(fwd.console),
        qemu_dbus_display1_mouse_get_is_absolute(fwd.mouse) ? "absolute" : "relative");
}

/* Input relay through QMP (--qemu-qmp, used when there is no --qemu-display):
 * for VMs whose only display is this device, so QEMU has no graphic console to
 * relay to. Events go to QEMU's input layer with input-send-event (no console:
 * the default keyboard/pointer handlers). Display1 key codes are QEMU key
 * numbers, which is also what a QMP KeyValue of type "number" carries; buttons
 * map to InputButton names; absolute positions are scaled to 0..0x7fff over the
 * guest desktop (this display unless --head-origin/--desktop say otherwise).
 * Mouse.IsAbsolute is true when QEMU has any absolute mouse (query-mice).
 * The connection is asynchronous and retried every second: a cold-plugged
 * device needs qdev-host listening before QEMU starts, and QEMU answers QMP
 * only once its main loop runs, after the device has been realized. Events
 * before the handshake is done are dropped. Replies are read and dropped,
 * errors logged. */
enum { QMP_DOWN, QMP_CAPS, QMP_MICE, QMP_READY };
static struct { int fd, state; GString *in; bool abs, warned; uint64_t sent, errors; } qmp = { .fd = -1, .abs = true };
static void exports_set_absolute(bool abs);

static void qmp_send(const char *json)
{
    if (qmp.fd < 0) return;
    size_t len = strlen(json);
    if (send(qmp.fd, json, len, MSG_NOSIGNAL) != (ssize_t)len) { LOG("qmp send: %s", strerror(errno)); return; }
    qmp.sent++;
}
static void qmp_events(const char *events)
{
    if (qmp.state != QMP_READY) return;
    g_autofree char *j = g_strdup_printf("{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[%s]}}\n", events);
    qmp_send(j);
}
static void qmp_key(guint k, bool down)
{
    g_autofree char *e = g_strdup_printf("{\"type\":\"key\",\"data\":{\"down\":%s,\"key\":{\"type\":\"number\",\"data\":%u}}}",
                                         down ? "true" : "false", k);
    qmp_events(e);
}
static void qmp_btn(guint b, bool down)
{
    static const char *names[] = { "left", "middle", "right", "wheel-up", "wheel-down", "side", "extra", "wheel-left", "wheel-right" };
    if (b >= G_N_ELEMENTS(names)) { DBG("qmp: button %u not mapped", b); return; }
    g_autofree char *e = g_strdup_printf("{\"type\":\"btn\",\"data\":{\"down\":%s,\"button\":\"%s\"}}", down ? "true" : "false", names[b]);
    qmp_events(e);
}
/* one JSON line from QEMU (greeting, reply or event) */
static void qmp_line(const char *line)
{
    bool ret = strstr(line, "\"return\"") != NULL, err = strstr(line, "\"error\"") != NULL;
    if (err) { qmp.errors++; if (qmp.errors <= 10 || opt.verbose) LOG("qmp: %s", line); }
    if (qmp.state == QMP_CAPS && (ret || err)) {
        qmp.state = QMP_MICE;
        qmp_send("{\"execute\":\"query-mice\"}\n");
    } else if (qmp.state == QMP_MICE && (ret || err)) {
        /* absolute if any mouse is (usb-tablet etc.): input-send-event hands abs events to the first
         * handler that takes them, whatever query-mice calls "current" (before the guest activates the
         * tablet that is the PS/2 mouse) */
        DBG("qmp: %s", line);
        if (ret && strstr(line, "\"name\"")) qmp.abs = strstr(line, "\"absolute\": true") != NULL;
        qmp.state = QMP_READY;
        exports_set_absolute(qmp.abs);
        LOG("input forwarded to QEMU QMP %s with input-send-event (%s pointer)", opt.qemu_qmp, qmp.abs ? "absolute" : "relative");
    }
}
static gboolean qmp_read(gint fd, GIOCondition cond, gpointer data)
{
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) return G_SOURCE_CONTINUE;
    if (n <= 0) {
        LOG("qmp: QEMU closed the connection; input is dropped until it is back");
        close(qmp.fd); qmp.fd = -1; qmp.state = QMP_DOWN; qmp.warned = false;
        return G_SOURCE_REMOVE;
    }
    g_string_append_len(qmp.in, buf, n);
    char *nl;
    while ((nl = memchr(qmp.in->str, '\n', qmp.in->len))) {
        g_autofree char *line = g_strndup(qmp.in->str, nl - qmp.in->str);
        g_string_erase(qmp.in, 0, nl - qmp.in->str + 1);
        qmp_line(g_strstrip(line));
    }
    return G_SOURCE_CONTINUE;
}
static gboolean qmp_connect(gpointer data)
{
    if (qmp.fd >= 0) return G_SOURCE_CONTINUE;
    const char *path = g_str_has_prefix(opt.qemu_qmp, "unix:") ? opt.qemu_qmp + 5 : opt.qemu_qmp;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    g_strlcpy(a.sun_path, path, sizeof(a.sun_path));
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        if (!qmp.warned) { LOG("qemu qmp %s: %s (retrying every second)", path, strerror(errno)); qmp.warned = true; }
        if (fd >= 0) close(fd);
        return G_SOURCE_CONTINUE;
    }
    struct timeval tv = { .tv_sec = 1 };   /* a stuck QEMU must not hold the main loop for long */
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    qmp.fd = fd; qmp.state = QMP_CAPS;
    if (!qmp.in) qmp.in = g_string_new(NULL);
    g_string_truncate(qmp.in, 0);
    g_unix_fd_add(fd, G_IO_IN | G_IO_HUP | G_IO_ERR, qmp_read, NULL);
    qmp_send("{\"execute\":\"qmp_capabilities\"}\n");   /* queued until QEMU's monitor runs */
    DBG("qmp: connected to %s", path);
    return G_SOURCE_CONTINUE;
}
static void setup_qmp(void)
{
    if (!opt.qemu_qmp) return;
    if (opt.qemu_display) { LOG("--qemu-display given: input goes to its Display1 console, --qemu-qmp is not used"); return; }
    const char *path = g_str_has_prefix(opt.qemu_qmp, "unix:") ? opt.qemu_qmp + 5 : opt.qemu_qmp;
    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) { LOG("--qemu-qmp path too long"); exit(1); }
    qmp_connect(NULL);
    g_timeout_add_seconds(1, qmp_connect, NULL);
}

static gboolean kbd_press(QemuDBusDisplay1Keyboard *o, GDBusMethodInvocation *inv, guint k, gpointer d)
{
    if (fwd.kbd) { qemu_dbus_display1_keyboard_call_press(fwd.kbd, k, G_DBUS_CALL_FLAGS_NONE, DBUS_TIMEOUT, NULL, NULL, NULL); fwd.forwarded++; }
    else if (qmp.state == QMP_READY) { qmp_key(k, true); fwd.forwarded++; }
    qemu_dbus_display1_keyboard_complete_press(o, inv); return TRUE;
}
static gboolean kbd_release(QemuDBusDisplay1Keyboard *o, GDBusMethodInvocation *inv, guint k, gpointer d)
{
    if (fwd.kbd) { qemu_dbus_display1_keyboard_call_release(fwd.kbd, k, G_DBUS_CALL_FLAGS_NONE, DBUS_TIMEOUT, NULL, NULL, NULL); fwd.forwarded++; }
    else if (qmp.state == QMP_READY) { qmp_key(k, false); fwd.forwarded++; }
    qemu_dbus_display1_keyboard_complete_release(o, inv); return TRUE;
}
static gboolean mouse_press(QemuDBusDisplay1Mouse *o, GDBusMethodInvocation *inv, guint b, gpointer d)
{
    if (fwd.mouse) { qemu_dbus_display1_mouse_call_press(fwd.mouse, b, G_DBUS_CALL_FLAGS_NONE, DBUS_TIMEOUT, NULL, NULL, NULL); fwd.forwarded++; }
    else if (qmp.state == QMP_READY) { qmp_btn(b, true); fwd.forwarded++; }
    qemu_dbus_display1_mouse_complete_press(o, inv); return TRUE;
}
static gboolean mouse_release(QemuDBusDisplay1Mouse *o, GDBusMethodInvocation *inv, guint b, gpointer d)
{
    if (fwd.mouse) { qemu_dbus_display1_mouse_call_release(fwd.mouse, b, G_DBUS_CALL_FLAGS_NONE, DBUS_TIMEOUT, NULL, NULL, NULL); fwd.forwarded++; }
    else if (qmp.state == QMP_READY) { qmp_btn(b, false); fwd.forwarded++; }
    qemu_dbus_display1_mouse_complete_release(o, inv); return TRUE;
}
/* Absolute pointer on a second head. The VM's tablet spans the whole guest
 * desktop, and QEMU scales Console_N coordinates by that console's size. So a
 * point (x, y) on this display maps to desktop (ox + x, oy + y), which is sent as
 * (ox + x) * vmW / desktopW. Layout from --head-origin/--desktop, default: this
 * display to the right of the VM console. */
static void head_layout(guint vw, guint vh, int *ox, int *oy, guint *dw, guint *dh)
{
    *ox = vw; *oy = 0; *dw = vw + disp.cur.width; *dh = MAX(vh, disp.cur.height);
    if (opt.head_origin && strcmp(opt.head_origin, "auto")) sscanf(opt.head_origin, "%d,%d", ox, oy);
    if (opt.desktop && strcmp(opt.desktop, "auto")) sscanf(opt.desktop, "%ux%u", dw, dh);
}
static gboolean mouse_abs(QemuDBusDisplay1Mouse *o, GDBusMethodInvocation *inv, guint x, guint y, gpointer d)
{
    if (fwd.mouse) {
        guint vw = qemu_dbus_display1_console_get_width(fwd.console), vh = qemu_dbus_display1_console_get_height(fwd.console);
        if (disp.cur.enabled && disp.cur.width && disp.cur.height && vw && vh) {
            int ox, oy; guint dw, dh;
            head_layout(vw, vh, &ox, &oy, &dw, &dh);
            int64_t X = (int64_t)ox + x, Y = (int64_t)oy + y;
            X = X < 0 ? 0 : X; Y = Y < 0 ? 0 : Y;
            x = (guint)MIN((uint64_t)X * vw / (dw ? dw : 1), vw - 1); y = (guint)MIN((uint64_t)Y * vh / (dh ? dh : 1), vh - 1);
        }
        qemu_dbus_display1_mouse_call_set_abs_position(fwd.mouse, x, y, G_DBUS_CALL_FLAGS_NONE, DBUS_TIMEOUT, NULL, NULL, NULL);
        fwd.forwarded++;
    } else if (qmp.state == QMP_READY && disp.cur.enabled && disp.cur.width && disp.cur.height) {
        /* no VM console: the desktop is this display, or what --head-origin/--desktop describe */
        int ox, oy; guint dw, dh;
        head_layout(0, 0, &ox, &oy, &dw, &dh);
        int64_t X = MAX((int64_t)ox + x, 0), Y = MAX((int64_t)oy + y, 0);
        uint64_t ax = MIN((uint64_t)X * 0x7fff / (dw ? dw : 1), 0x7fff), ay = MIN((uint64_t)Y * 0x7fff / (dh ? dh : 1), 0x7fff);
        g_autofree char *e = g_strdup_printf("{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":%" PRIu64 "}},"
                                             "{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":%" PRIu64 "}}", ax, ay);
        qmp_events(e);
        fwd.forwarded++;
    }
    qemu_dbus_display1_mouse_complete_set_abs_position(o, inv); return TRUE;
}
static gboolean mouse_rel(QemuDBusDisplay1Mouse *o, GDBusMethodInvocation *inv, gint dx, gint dy, gpointer d)
{
    if (fwd.mouse) { qemu_dbus_display1_mouse_call_rel_motion(fwd.mouse, dx, dy, G_DBUS_CALL_FLAGS_NONE, DBUS_TIMEOUT, NULL, NULL, NULL); fwd.forwarded++; }
    else if (qmp.state == QMP_READY) {
        g_autofree char *e = g_strdup_printf("{\"type\":\"rel\",\"data\":{\"axis\":\"x\",\"value\":%d}},"
                                             "{\"type\":\"rel\",\"data\":{\"axis\":\"y\",\"value\":%d}}", dx, dy);
        qmp_events(e);
        fwd.forwarded++;
    }
    qemu_dbus_display1_mouse_complete_rel_motion(o, inv); return TRUE;
}

static void export_closed(GDBusConnection *c, gboolean remote, GError *err, gpointer data)
{
    export_t *e = data;
    DBG("display client connection closed");
    g_ptr_array_remove(exports, e);
    g_clear_object(&e->mgr);
    g_clear_object(&e->console);
    g_clear_object(&e->mouse);
    g_clear_object(&e->conn);
    g_free(e);
}

static void export_display1(GDBusConnection *conn)
{
    const char *vm_ifaces[3] = { NULL }; int nvi = 0;
    if (fwd.clip_to_qemu) vm_ifaces[nvi++] = "org.qemu.Display1.Clipboard";
    if (fwd.audio) vm_ifaces[nvi++] = "org.qemu.Display1.Audio";
    static const char *con_ifaces[] = { "org.qemu.Display1.Keyboard", "org.qemu.Display1.Mouse", NULL };
    export_t *e = g_new0(export_t, 1);
    e->conn = g_object_ref(conn);
    e->mgr = g_dbus_object_manager_server_new(DISPLAY1_ROOT);

    QemuDBusDisplay1VM *vm = qemu_dbus_display1_vm_skeleton_new();
    guint32 ids[] = { 0 };
    g_autofree char *uuid = g_uuid_string_random();
    g_object_set(vm, "name", opt.vm_name, "uuid", uuid,
                 "console-ids", g_variant_new_fixed_array(G_VARIANT_TYPE("u"), ids, 1, sizeof(guint32)),
                 "interfaces", vm_ifaces, NULL);
    GDBusObjectSkeleton *vmo = g_dbus_object_skeleton_new(DISPLAY1_ROOT "/VM");
    g_dbus_object_skeleton_add_interface(vmo, G_DBUS_INTERFACE_SKELETON(vm));
    g_dbus_object_manager_server_export(e->mgr, vmo);
    g_object_unref(vm); g_object_unref(vmo);

    e->console = qemu_dbus_display1_console_skeleton_new();
    g_object_set(e->console, "label", ops->name, "type", "Graphic", "head", 0u,
                 "width", disp.cur.enabled ? disp.cur.width : 0u,
                 "height", disp.cur.enabled ? disp.cur.height : 0u,
                 "device-address", "", "interfaces", con_ifaces, NULL);
    g_signal_connect(e->console, "handle-register-listener", G_CALLBACK(handle_register_listener), NULL);
    g_signal_connect(e->console, "handle-set-uiinfo", G_CALLBACK(handle_set_uiinfo), NULL);
    QemuDBusDisplay1Keyboard *kbd = qemu_dbus_display1_keyboard_skeleton_new();
    g_signal_connect(kbd, "handle-press", G_CALLBACK(kbd_press), NULL);
    g_signal_connect(kbd, "handle-release", G_CALLBACK(kbd_release), NULL);
    QemuDBusDisplay1Mouse *mouse = qemu_dbus_display1_mouse_skeleton_new();
    qemu_dbus_display1_mouse_set_is_absolute(mouse, fwd.mouse ? qemu_dbus_display1_mouse_get_is_absolute(fwd.mouse) :
                                                    opt.qemu_qmp ? qmp.abs : TRUE);
    g_signal_connect(mouse, "handle-press", G_CALLBACK(mouse_press), NULL);
    g_signal_connect(mouse, "handle-release", G_CALLBACK(mouse_release), NULL);
    g_signal_connect(mouse, "handle-set-abs-position", G_CALLBACK(mouse_abs), NULL);
    g_signal_connect(mouse, "handle-rel-motion", G_CALLBACK(mouse_rel), NULL);
    GDBusObjectSkeleton *co = g_dbus_object_skeleton_new(DISPLAY1_ROOT "/Console_0");
    g_dbus_object_skeleton_add_interface(co, G_DBUS_INTERFACE_SKELETON(e->console));
    g_dbus_object_skeleton_add_interface(co, G_DBUS_INTERFACE_SKELETON(kbd));
    g_dbus_object_skeleton_add_interface(co, G_DBUS_INTERFACE_SKELETON(mouse));
    g_dbus_object_manager_server_export(e->mgr, co);
    e->mouse = mouse;
    g_object_unref(kbd); g_object_unref(co);

    if (fwd.clip_to_qemu) {
        QemuDBusDisplay1Clipboard *cl = qemu_dbus_display1_clipboard_skeleton_new();
        static const char *none[] = { NULL };
        qemu_dbus_display1_clipboard_set_interfaces(cl, none);
        g_signal_connect(cl, "handle-register", G_CALLBACK(cclip_register), NULL);
        g_signal_connect(cl, "handle-unregister", G_CALLBACK(cclip_unregister), NULL);
        g_signal_connect(cl, "handle-grab", G_CALLBACK(cclip_grab), NULL);
        g_signal_connect(cl, "handle-release", G_CALLBACK(cclip_release), NULL);
        g_signal_connect(cl, "handle-request", G_CALLBACK(cclip_request), NULL);
        GDBusObjectSkeleton *clo = g_dbus_object_skeleton_new(DISPLAY1_ROOT "/Clipboard");
        g_dbus_object_skeleton_add_interface(clo, G_DBUS_INTERFACE_SKELETON(cl));
        g_dbus_object_manager_server_export(e->mgr, clo);
        g_object_unref(cl); g_object_unref(clo);
    }
    if (fwd.audio) {
        QemuDBusDisplay1Audio *au = qemu_dbus_display1_audio_skeleton_new();
        static const char *none[] = { NULL };
        qemu_dbus_display1_audio_set_interfaces(au, none);
        qemu_dbus_display1_audio_set_nsamples(au, qemu_dbus_display1_audio_get_nsamples(fwd.audio));
        g_signal_connect(au, "handle-register-out-listener", G_CALLBACK(audio_reg_out), NULL);
        g_signal_connect(au, "handle-register-in-listener", G_CALLBACK(audio_reg_in), NULL);
        GDBusObjectSkeleton *auo = g_dbus_object_skeleton_new(DISPLAY1_ROOT "/Audio");
        g_dbus_object_skeleton_add_interface(auo, G_DBUS_INTERFACE_SKELETON(au));
        g_dbus_object_manager_server_export(e->mgr, auo);
        g_object_unref(au); g_object_unref(auo);
    }

    g_dbus_object_manager_server_set_connection(e->mgr, conn);
    g_signal_connect(conn, "closed", G_CALLBACK(export_closed), e);
    g_ptr_array_add(exports, e);
}

/* keep Console.Width/Height in sync with the mode */
static void exports_sink_scanout(sink_t *s)
{
    for (guint i = 0; i < exports->len; i++) {
        export_t *e = g_ptr_array_index(exports, i);
        qemu_dbus_display1_console_set_width(e->console, disp.cur.enabled ? disp.cur.width : 0);
        qemu_dbus_display1_console_set_height(e->console, disp.cur.enabled ? disp.cur.height : 0);
    }
}

static void exports_set_absolute(bool abs)
{
    for (guint i = 0; exports && i < exports->len; i++)
        qemu_dbus_display1_mouse_set_is_absolute(((export_t *)g_ptr_array_index(exports, i))->mouse, abs);
}

static gboolean on_new_connection(GDBusServer *srv, GDBusConnection *conn, gpointer data)
{
    LOG("display client connected (p2p)");
    export_display1(conn);
    return TRUE;
}

static void setup_display1(void)
{
    g_autoptr(GError) err = NULL;
    if (opt.display_p2p) {
        unlink(opt.display_p2p);
        g_autofree char *addr = g_strdup_printf("unix:path=%s", opt.display_p2p);
        g_autofree char *guid = g_dbus_generate_guid();
        GDBusServer *srv = g_dbus_server_new_sync(addr, G_DBUS_SERVER_FLAGS_NONE, guid, NULL, NULL, &err);
        if (!srv) { LOG("display p2p server: %s", err->message); exit(1); }
        g_signal_connect(srv, "new-connection", G_CALLBACK(on_new_connection), NULL);
        g_dbus_server_start(srv);
        LOG("Display1 p2p: %s", addr);
    }
    if (opt.display_bus) {
        GDBusConnection *bus = g_dbus_connection_new_for_address_sync(opt.display_bus,
            G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
            NULL, NULL, &err);
        if (!bus) { LOG("display bus %s: %s", opt.display_bus, err->message); exit(1); }
        export_display1(bus);
        g_bus_own_name_on_connection(bus, opt.display_name, G_BUS_NAME_OWNER_FLAGS_NONE, NULL, NULL, NULL, NULL);
        LOG("Display1 bus: %s as %s", opt.display_bus, opt.display_name);
    }
}

/* ------------------------------------------------------------------ main */
static void usage(void)
{
    fprintf(stderr,
        "usage: qdev-host --plugin FILE.so --socket PATH [options]\n"
        "  --plugin-args STR        plugin options (e.g. edid=file,vram=16)\n"
        "  --display-p2p PATH       org.qemu.Display1 peer-to-peer server socket\n"
        "  --display-bus ADDR       join a message bus (unix:path=...) and own --display-name\n"
        "  --display-name NAME      bus name (default org.qemu, what QSM Direct expects)\n"
        "  --vm-name NAME           Display1.VM.Name\n"
        "  --dump-dir DIR           where SIGUSR1 writes qdev-fb-N.ppm (default .)\n"
        "  --fps N                  damage poll rate while the picture changes (default 30)\n"
        "  --idle-fps N             poll rate after --idle-after seconds without changes (default 10; 0 = off)\n"
        "  --idle-after S           seconds without changes before dropping to --idle-fps (default 2)\n"
        "  --kms /dev/dri/cardN[:CONNECTOR]  scan the device out on a real monitor (DRM master needed)\n"
        "  --on-connect CMD / --on-disconnect CMD  run when the KMS connector's status changes\n"
        "  --qemu-display ADDR      the VM's own Display1 (unix:path=...) to forward keyboard/mouse to\n"
        "  --qemu-display-dest NAME bus name there (default org.qemu; empty for p2p)\n"
        "  --qemu-console N         console index there (default 0)\n"
        "  --head-origin X,Y|auto   where this display sits on the guest desktop (default: right of the VM console)\n"
        "  --desktop WxH|auto       guest desktop size for pointer mapping (default: VM console + this display)\n"
        "  --qemu-qmp PATH          the VM's QMP socket: forward keyboard/mouse with input-send-event\n"
        "                           (for VMs without a graphic console; --qemu-display wins if both are given)\n"
        "  --control PATH           control socket for automation: screendump FILE | frame | stats\n"
        "  --kms-copy               force the copy path (no udmabuf zero-copy)\n"
        "  --stats-interval S       print stats every S seconds (SIGUSR2 prints them once)\n"
        "  -v                       verbose\n");
    exit(2);
}

/* The device process takes over QEMU's role for the device: raise the soft
 * NOFILE limit to the hard one as QEMU does at startup (os-posix.c
 * os_setup_limits). A GPU plugin keeps an fd per guest-RAM alias and per
 * imported buffer; with a 1024 soft limit it runs out where the same device
 * inside QEMU did not. */
static void setup_limits(void)
{
    struct rlimit nofile;

    if (getrlimit(RLIMIT_NOFILE, &nofile) < 0) {
        LOG("unable to query NOFILE limit: %s", strerror(errno));
        return;
    }
    if (nofile.rlim_cur == nofile.rlim_max) {
        return;
    }
    nofile.rlim_cur = nofile.rlim_max;
    if (setrlimit(RLIMIT_NOFILE, &nofile) < 0) {
        LOG("unable to set NOFILE limit: %s", strerror(errno));
    }
}

int main(int argc, char **argv)
{
    static const struct option lopts[] = {
        { "plugin", 1, 0, 'p' }, { "socket", 1, 0, 's' }, { "plugin-args", 1, 0, 'a' },
        { "display-p2p", 1, 0, 'P' }, { "display-bus", 1, 0, 'B' }, { "display-name", 1, 0, 'N' },
        { "vm-name", 1, 0, 'V' }, { "dump-dir", 1, 0, 'D' }, { "fps", 1, 0, 'f' }, { "verbose", 0, 0, 'v' },
        { "kms", 1, 0, 'K' }, { "on-connect", 1, 0, 1 }, { "on-disconnect", 1, 0, 2 },
        { "qemu-display", 1, 0, 'Q' }, { "qemu-display-dest", 1, 0, 3 }, { "qemu-console", 1, 0, 4 },
        { "stats-interval", 1, 0, 'S' }, { "idle-fps", 1, 0, 5 }, { "idle-after", 1, 0, 6 },
        { "head-origin", 1, 0, 7 }, { "desktop", 1, 0, 8 }, { "kms-copy", 0, 0, 9 }, { "control", 1, 0, 10 },
        { "qemu-qmp", 1, 0, 11 }, { "help", 0, 0, 'h' }, { 0 }
    };
    setup_limits();
    int c;
    while ((c = getopt_long(argc, argv, "p:s:a:P:B:N:V:D:f:K:Q:S:vh", lopts, NULL)) != -1) {
        switch (c) {
        case 'p': opt.plugin = optarg; break;      case 's': opt.socket = optarg; break;
        case 'a': opt.plugin_args = optarg; break; case 'P': opt.display_p2p = optarg; break;
        case 'B': opt.display_bus = optarg; break; case 'N': opt.display_name = optarg; break;
        case 'V': opt.vm_name = optarg; break;     case 'D': opt.dump_dir = optarg; break;
        case 'f': opt.fps = atoi(optarg); break;   case 'v': opt.verbose = true; break;
        case 'K': opt.kms = optarg; break;         case 1: opt.on_connect = optarg; break;
        case 2: opt.on_disconnect = optarg; break; case 'Q': opt.qemu_display = optarg; break;
        case 3: opt.qemu_display_dest = optarg; break; case 4: opt.qemu_console = atoi(optarg); break;
        case 'S': opt.stats_interval = atoi(optarg); break;
        case 5: opt.idle_fps = atoi(optarg); break; case 6: opt.idle_after = atoi(optarg); break;
        case 7: opt.head_origin = optarg; break;   case 8: opt.desktop = optarg; break;
        case 9: opt.kms_copy = true; break;
        case 10: opt.control = optarg; break;      case 11: opt.qemu_qmp = optarg; break;
        default: usage();
        }
    }
    if (!opt.plugin || !opt.socket || opt.fps < 1 || opt.fps > 240) usage();

    listeners = g_ptr_array_new();
    exports = g_ptr_array_new();
    disp.sinks = g_ptr_array_new();
    static sink_t display1_sink = { "display1", display1_sink_scanout, display1_sink_update };
    static sink_t console_props_sink = { "console-props", exports_sink_scanout, NULL };
    g_ptr_array_add(disp.sinks, &console_props_sink);
    g_ptr_array_add(disp.sinks, &display1_sink);

    static sink_t kms_sink = { "kms", kms_sink_scanout, kms_sink_update };
    if (opt.kms) {
        kms = kms_open(opt.kms, opt.verbose);
        if (!kms) exit(1);
        kms_set_allow_zero_copy(kms, !opt.kms_copy);
        g_ptr_array_add(disp.sinks, &kms_sink);
        g_timeout_add(1000, kms_hotplug_poll, NULL);
    }
    dma_map = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL, g_free);
    wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake_fd < 0) { LOG("eventfd: %s", strerror(errno)); exit(1); }
    g_unix_fd_add(wake_fd, G_IO_IN, wake_cb, NULL);
    load_plugin();
    setup_vfu();
    disp_initial();
    setup_input_forwarding();
    setup_qmp();
    setup_display1();
    setup_control();
    if (!PLUGIN_DAMAGE) g_timeout_add(1000 / opt.fps, disp_tick, NULL);   /* else frame_ready() drives the sinks */
    if (abi >= 2 && ops->tick && ops->tick_ms) g_timeout_add(ops->tick_ms, plugin_tick, NULL);
    g_unix_signal_add(SIGUSR1, on_sigusr1, NULL);
    g_unix_signal_add(SIGUSR2, on_sigusr2, NULL);
    g_unix_signal_add(SIGTERM, on_term, NULL);
    g_unix_signal_add(SIGINT, on_term, NULL);
    if (opt.stats_interval > 0) g_timeout_add_seconds(opt.stats_interval, on_stats_timer, NULL);

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    return 0;
}
