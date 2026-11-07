/*
 * qdev-host: out-of-process PCI device host for stock QEMU (vfio-user-pci)
 *
 *   plugin (.so, qdp.h ABI) --> libvfio-user --> unix socket --> QEMU vfio-user-pci
 *   plugin scanout (memfd)  --> damage detection --> sinks:
 *        * org.qemu.Display1 (p2p server socket and/or message bus, bus name
 *          org.qemu): what QSM Direct / qemu-display-probe connect to, exactly
 *          like a QEMU started with -display dbus
 *        * PPM dump on SIGUSR1
 *        * (planned) DRM/KMS: a real monitor as the output of the guest device
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
#include <glib.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include "qdp.h"
#include "dbus/dbus-display1.h"

#define DISPLAY1_ROOT "/org/qemu/Display1"
#define DBUS_TIMEOUT  5000

/* ------------------------------------------------------------------ options */
static struct {
    const char *plugin, *socket, *plugin_args, *display_p2p, *display_bus,
               *display_name, *vm_name, *dump_dir;
    int fps;
    bool verbose;
} opt = { .display_name = "org.qemu", .vm_name = "qdev-host", .dump_dir = ".", .fps = 30 };

#define LOG(...) do { fprintf(stderr, "qdev-host: " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define DBG(...) do { if (opt.verbose) LOG(__VA_ARGS__); } while (0)

/* ------------------------------------------------------- plugin + vfio-user */
static vfu_ctx_t *vfu;
static const qdp_device_ops *ops;
static void *priv;
static bool attached;
static guint vfu_watch;

static int reset_cb(vfu_ctx_t *c, vfu_reset_type_t t)
{
    DBG("device reset (type %d)", t);
    return ops->reset ? ops->reset(priv, t) : 0;
}
static void dma_register(vfu_ctx_t *c, vfu_dma_info_t *i) {}
static void dma_unregister(vfu_ctx_t *c, vfu_dma_info_t *i) {}

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
        if (vfu_run_ctx(vfu) < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return G_SOURCE_CONTINUE;
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
    void *h = dlopen(opt.plugin, RTLD_NOW | RTLD_LOCAL);
    if (!h) { LOG("dlopen %s: %s", opt.plugin, dlerror()); exit(1); }
    qdp_plugin_v1_fn fn = (qdp_plugin_v1_fn)dlsym(h, QDP_PLUGIN_SYMBOL);
    if (!fn) { LOG("%s: no %s symbol", opt.plugin, QDP_PLUGIN_SYMBOL); exit(1); }
    ops = fn();
    if (!ops || ops->abi_version != QDP_ABI_VERSION) {
        LOG("%s: ABI %u, host wants %u", opt.plugin, ops ? ops->abi_version : 0, QDP_ABI_VERSION); exit(1);
    }
    LOG("plugin %s %s (ABI %u)", ops->name, ops->version, ops->abi_version);
}

static void setup_vfu(void)
{
    unlink(opt.socket);
    vfu = vfu_create_ctx(VFU_TRANS_SOCK, opt.socket, LIBVFIO_USER_FLAG_ATTACH_NB, NULL, VFU_DEV_TYPE_PCI);
    if (!vfu) { LOG("vfu_create_ctx %s: %s", opt.socket, strerror(errno)); exit(1); }
    int r = ops->setup(vfu, opt.plugin_args, &priv);
    if (r < 0) { LOG("plugin setup failed: %s", strerror(-r)); exit(1); }
    if (vfu_setup_device_reset_cb(vfu, reset_cb) < 0) { LOG("reset cb: %s", strerror(errno)); exit(1); }
    if (vfu_setup_device_dma(vfu, LIBVFIO_USER_MAX_DMA_REGIONS, dma_register, dma_unregister) < 0) {
        LOG("dma setup: %s", strerror(errno)); exit(1);
    }
    if (vfu_realize_ctx(vfu) < 0) { LOG("vfu_realize_ctx: %s", strerror(errno)); exit(1); }
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

static struct {
    qdp_scanout cur;        /* current mode (enabled == false: blank) */
    uint8_t *map;           /* mapping of the framebuffer (page aligned base) */
    size_t   map_len;
    uint8_t *pix;           /* first pixel of the scanout */
    uint8_t *shadow;        /* last pixels sent (width*4*height, packed) */
    GPtrArray *sinks;
} disp;

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
    disp.shadow = g_malloc0((size_t)s->width * 4 * s->height);
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

static gboolean disp_tick(gpointer data)
{
    if (ops->tick) ops->tick(priv);
    if (!ops->get_scanout) return G_SOURCE_CONTINUE;
    qdp_scanout now;
    if (!ops->get_scanout(priv, &now)) return G_SOURCE_CONTINUE;
    if (mode_changed(&now, &disp.cur)) {
        disp_unmap();
        disp.cur = now;
        if (now.enabled && !disp_map(&now)) disp.cur.enabled = false;
        LOG("scanout %s %ux%u stride %u fmt 0x%x", disp.cur.enabled ? "on" : "off",
            now.width, now.height, now.stride, now.pixman_format);
        disp_notify_scanout();
        if (!disp.cur.enabled) return G_SOURCE_CONTINUE;
        /* first frame: everything is damage */
        memset(disp.shadow, 0xff, (size_t)now.width * 4 * now.height);
    }
    if (!disp.cur.enabled) return G_SOURCE_CONTINUE;
    int x, y, w, h;
    if (disp_damage(&x, &y, &w, &h)) {
        for (guint i = 0; i < disp.sinks->len; i++) {
            sink_t *s = g_ptr_array_index(disp.sinks, i);
            if (s->update) s->update(s, x, y, w, h);
        }
    }
    return G_SOURCE_CONTINUE;
}

/* --------------------------------------------------------------- PPM sink */
static unsigned dump_count;
static void dump_ppm(void)
{
    const qdp_scanout *s = &disp.cur;
    if (!s->enabled) { LOG("dump: scanout disabled"); return; }
    g_autofree char *name = g_strdup_printf("%s/qdev-fb-%u.ppm", opt.dump_dir, dump_count++);
    FILE *f = fopen(name, "wb");
    if (!f) { LOG("dump: %s: %s", name, strerror(errno)); return; }
    fprintf(f, "P6\n%u %u\n255\n", s->width, s->height);
    for (uint32_t y = 0; y < s->height; y++) {
        const uint8_t *row = disp.pix + (size_t)y * s->stride;
        for (uint32_t x = 0; x < s->width; x++) { fputc(row[x*4+2], f); fputc(row[x*4+1], f); fputc(row[x*4], f); }
    }
    fclose(f);
    LOG("dump: %s (%ux%u)", name, s->width, s->height);
}
static gboolean on_sigusr1(gpointer data) { dump_ppm(); return G_SOURCE_CONTINUE; }

/* ------------------------------------------------------ Display1 listener */
typedef struct {
    GDBusConnection *conn;
    QemuDBusDisplay1Listener *proxy;
    QemuDBusDisplay1ListenerUnixMap *map_proxy;
    bool can_map;     /* listener implements org.qemu.Display1.Listener.Unix.Map */
    bool mapped;      /* ScanoutMap of the current scanout succeeded */
    bool scanout_sent;
} listener_t;

static GPtrArray *listeners;   /* listener_t* */

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
    return TRUE;
}

static gboolean handle_set_uiinfo(QemuDBusDisplay1Console *o, GDBusMethodInvocation *inv, guint16 wmm, guint16 hmm,
                                  gint xoff, gint yoff, guint w, guint h, gpointer data)
{
    DBG("SetUIInfo %ux%u (ignored: the guest chooses the mode)", w, h);
    qemu_dbus_display1_console_complete_set_uiinfo(o, inv);
    return TRUE;
}
/* Input: accepted and dropped for now. Next step: forward to the VM's own
 * QEMU Display1 (Keyboard/Mouse of its console) so QSM input keeps working. */
static gboolean kbd_press(QemuDBusDisplay1Keyboard *o, GDBusMethodInvocation *inv, guint k, gpointer d)
{ qemu_dbus_display1_keyboard_complete_press(o, inv); return TRUE; }
static gboolean kbd_release(QemuDBusDisplay1Keyboard *o, GDBusMethodInvocation *inv, guint k, gpointer d)
{ qemu_dbus_display1_keyboard_complete_release(o, inv); return TRUE; }
static gboolean mouse_press(QemuDBusDisplay1Mouse *o, GDBusMethodInvocation *inv, guint b, gpointer d)
{ qemu_dbus_display1_mouse_complete_press(o, inv); return TRUE; }
static gboolean mouse_release(QemuDBusDisplay1Mouse *o, GDBusMethodInvocation *inv, guint b, gpointer d)
{ qemu_dbus_display1_mouse_complete_release(o, inv); return TRUE; }
static gboolean mouse_abs(QemuDBusDisplay1Mouse *o, GDBusMethodInvocation *inv, guint x, guint y, gpointer d)
{ qemu_dbus_display1_mouse_complete_set_abs_position(o, inv); return TRUE; }
static gboolean mouse_rel(QemuDBusDisplay1Mouse *o, GDBusMethodInvocation *inv, gint dx, gint dy, gpointer d)
{ qemu_dbus_display1_mouse_complete_rel_motion(o, inv); return TRUE; }

static void export_closed(GDBusConnection *c, gboolean remote, GError *err, gpointer data)
{
    export_t *e = data;
    DBG("display client connection closed");
    g_ptr_array_remove(exports, e);
    g_clear_object(&e->mgr);
    g_clear_object(&e->console);
    g_clear_object(&e->conn);
    g_free(e);
}

static void export_display1(GDBusConnection *conn)
{
    static const char *vm_ifaces[] = { NULL };
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
    qemu_dbus_display1_mouse_set_is_absolute(mouse, TRUE);
    g_signal_connect(mouse, "handle-press", G_CALLBACK(mouse_press), NULL);
    g_signal_connect(mouse, "handle-release", G_CALLBACK(mouse_release), NULL);
    g_signal_connect(mouse, "handle-set-abs-position", G_CALLBACK(mouse_abs), NULL);
    g_signal_connect(mouse, "handle-rel-motion", G_CALLBACK(mouse_rel), NULL);
    GDBusObjectSkeleton *co = g_dbus_object_skeleton_new(DISPLAY1_ROOT "/Console_0");
    g_dbus_object_skeleton_add_interface(co, G_DBUS_INTERFACE_SKELETON(e->console));
    g_dbus_object_skeleton_add_interface(co, G_DBUS_INTERFACE_SKELETON(kbd));
    g_dbus_object_skeleton_add_interface(co, G_DBUS_INTERFACE_SKELETON(mouse));
    g_dbus_object_manager_server_export(e->mgr, co);
    g_object_unref(kbd); g_object_unref(mouse); g_object_unref(co);

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
        "  --fps N                  damage poll rate (default 30)\n"
        "  -v                       verbose\n");
    exit(2);
}

int main(int argc, char **argv)
{
    static const struct option lopts[] = {
        { "plugin", 1, 0, 'p' }, { "socket", 1, 0, 's' }, { "plugin-args", 1, 0, 'a' },
        { "display-p2p", 1, 0, 'P' }, { "display-bus", 1, 0, 'B' }, { "display-name", 1, 0, 'N' },
        { "vm-name", 1, 0, 'V' }, { "dump-dir", 1, 0, 'D' }, { "fps", 1, 0, 'f' }, { "verbose", 0, 0, 'v' },
        { "help", 0, 0, 'h' }, { 0 }
    };
    int c;
    while ((c = getopt_long(argc, argv, "p:s:a:P:B:N:V:D:f:vh", lopts, NULL)) != -1) {
        switch (c) {
        case 'p': opt.plugin = optarg; break;      case 's': opt.socket = optarg; break;
        case 'a': opt.plugin_args = optarg; break; case 'P': opt.display_p2p = optarg; break;
        case 'B': opt.display_bus = optarg; break; case 'N': opt.display_name = optarg; break;
        case 'V': opt.vm_name = optarg; break;     case 'D': opt.dump_dir = optarg; break;
        case 'f': opt.fps = atoi(optarg); break;   case 'v': opt.verbose = true; break;
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

    load_plugin();
    setup_vfu();
    setup_display1();
    g_timeout_add(1000 / opt.fps, disp_tick, NULL);
    g_unix_signal_add(SIGUSR1, on_sigusr1, NULL);

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    return 0;
}
