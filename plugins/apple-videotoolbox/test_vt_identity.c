/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * test_vt_identity: the apple-videotoolbox plugin's PCI identity and interrupt resources per plugin args, as the
 * vfio-user client sees them (the real plugin .so, set up on a real libvfio-user context; an in-process client reads
 * config space and sends SET_IRQS).  No VM; no decode backend (render_node points nowhere, backend=vaapi), so no GPU.
 *
 * Reference contract (arm64, analysis/20261128-ARM64-PORT-SCOPE.md §9.6 (c)): the stock arm64e
 * AppleVideoToolboxParavirtualization 15.0.0 (25G83) binds AppleVirtIOPCITransport on IOPCIDevice with
 * IOPCIPrimaryMatch 0x1a03106b (vendor 0x106b, device 0x1a03).  The x86 eee1 guest kext matches 0xEEE21B36, the
 * plugin's default.  INTx is the line the guest uses when it does not enable MSI-X; its assert/deassert behaviour is
 * the shared pvg-intx state, tested in plugins/uniq-vgpu/pvg_intx_test.c.
 *
 * Usage: test_vt_identity PLUGIN.so [SCRATCH_DIR]
 */
#define _GNU_SOURCE
#include "qdp.h"
#include "../uniq-vgpu/vfu_mini_client.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/eventfd.h>

static int failures, checks;
static void check(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void check(bool ok, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    printf("%s: ", ok ? "PASS" : "FAIL");
    vprintf(fmt, ap);
    putchar('\n');
    va_end(ap);
    checks++;
    failures += !ok;
}

static vfu_ctx_t *g_ctx;
static int host_trigger(void *host, uint32_t sub)
{
    (void)host;
    return vfu_irq_trigger(g_ctx, sub) < 0 ? -EINVAL : 0;
}
static void host_frame(void *host, const qdp_frame_info *f) { (void)host; (void)f; }
static void host_cursor(void *host) { (void)host; }
static void host_log(void *host, int level, const char *fmt, ...)
{
    va_list ap;
    (void)host;
    if (!getenv("VT_TEST_LOG") || level > 2) return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
static const qdp_host_ops host_ops = { .size = sizeof(qdp_host_ops), .host = NULL, .irq_trigger = host_trigger,
                                       .frame_ready = host_frame, .cursor_changed = host_cursor, .log = host_log };

typedef struct { const qdp_device_ops *ops; const char *path; const char *args; void *priv; int ready, rc;
                 pthread_mutex_t m; pthread_cond_t c; pthread_t thread; } Srv;

static void *srv_main(void *arg)
{
    Srv *s = arg;
    qdp_host_env env = { .size = sizeof(env), .host_ops = &host_ops };
    int r;

    g_ctx = vfu_create_ctx(VFU_TRANS_SOCK, s->path, 0, NULL, VFU_DEV_TYPE_PCI);
    s->rc = g_ctx ? s->ops->setup(g_ctx, s->args, &env, &s->priv) : -errno;
    if (s->rc == 0 && vfu_realize_ctx(g_ctx) < 0) s->rc = -errno;
    pthread_mutex_lock(&s->m);
    s->ready = 1;
    pthread_cond_signal(&s->c);
    pthread_mutex_unlock(&s->m);
    if (s->rc < 0) return NULL;
    if (vfu_attach_ctx(g_ctx) < 0) return NULL;
    do {
        r = vfu_run_ctx(g_ctx);
    } while (r >= 0 || errno == EINTR || errno == EAGAIN || errno == EBUSY);
    if (s->ops->teardown) s->ops->teardown(s->priv);
    vfu_destroy_ctx(g_ctx);
    g_ctx = NULL;
    return NULL;
}

static int run_case(const qdp_device_ops *ops, const char *dir, const char *args, uint8_t cfg[256], Srv *s)
{
    static int n;
    static char path[108];

    snprintf(path, sizeof(path), "%s/vt-id-%d.sock", dir, n++);
    unlink(path);
    *s = (Srv){ .ops = ops, .path = path, .args = args };
    pthread_mutex_init(&s->m, NULL);
    pthread_cond_init(&s->c, NULL);
    pthread_create(&s->thread, NULL, srv_main, s);
    pthread_mutex_lock(&s->m);
    while (!s->ready) pthread_cond_wait(&s->c, &s->m);
    pthread_mutex_unlock(&s->m);
    if (s->rc < 0) {
        pthread_join(s->thread, NULL);
        unlink(path);
        return s->rc;
    }
    vfu_mini_connect(path);
    rd_cfg(cfg);
    return 0;
}

static void end_case(Srv *s)
{
    close(sock);
    sock = -1;
    pthread_join(s->thread, NULL);   /* the server's run loop ends with the connection; teardown runs there */
    unlink(s->path);
}

int main(int argc, char **argv)
{
    const char *so = argc > 1 ? argv[1] : "build/apple-videotoolbox.so";
    const char *dir = argc > 2 ? argv[2] : "/tmp";
    void *h = dlopen(so, RTLD_NOW | RTLD_LOCAL);
    qdp_plugin_v1_fn entry;
    const qdp_device_ops *ops;
    uint8_t cfg[256];
    Srv s;
    int efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

    if (!h || !(entry = (qdp_plugin_v1_fn)dlsym(h, "qdp_plugin_v1")) || !(ops = entry())) {
        fprintf(stderr, "dlopen %s: %s\n", so, dlerror());
        return 2;
    }
    const char *nogpu = "render_node=/nonexistent/renderD999,backend=vaapi,worker=0";
    char args[256];

    /* default: the x86 eee1 identity, MSI-X, INTx pin as before */
    snprintf(args, sizeof(args), "%s", nogpu);
    if (run_case(ops, dir, args, cfg, &s) < 0) { check(false, "setup with default args (rc %d)", s.rc); return 1; }
    check((cfg[0] | cfg[1] << 8) == 0x1b36 && (cfg[2] | cfg[3] << 8) == 0xeee2 && vfu_mini_has_cap(cfg, 0x11) &&
          cfg[0x3d] == 1, "default: 1b36:eee2 (x86 guest kext IOPCIPrimaryMatch 0xEEE21B36), MSI-X capability, INTA#");
    check(set_irqs(VFIO_PCI_MSIX_IRQ_INDEX, VFIO_IRQ_SET_ACTION_TRIGGER, VFIO_IRQ_SET_DATA_EVENTFD, efd) == 0 &&
          vfu_irq_trigger_index(g_ctx) == VFU_DEV_MSIX_IRQ, "default: MSI-X eventfd accepted");
    check(rd(0, 0x1000, 1) == 0, "default: ISR status reads 0 (MSI-X in use)");
    end_case(&s);

    /* arm64: Apple's identity, irq=auto */
    snprintf(args, sizeof(args), "%s,vendor-id=0x106b,device-id=0x1a03,irq=auto", nogpu);
    if (run_case(ops, dir, args, cfg, &s) < 0) { check(false, "setup with vendor-id/device-id/irq=auto"); return 1; }
    check((cfg[0] | cfg[1] << 8) == 0x106b && (cfg[2] | cfg[3] << 8) == 0x1a03 &&
          (cfg[0x2c] | cfg[0x2d] << 8) == 0x106b && (cfg[0x2e] | cfg[0x2f] << 8) == 0x1a03,
          "vendor-id=0x106b,device-id=0x1a03: config 106b:1a03 (stock arm64e IOPCIPrimaryMatch 0x1a03106b), subsystem the same");
    check(vfu_mini_has_cap(cfg, 0x11) && cfg[0x3d] == 1, "irq=auto: MSI-X capability and INTA#");
    check(set_irqs(VFIO_PCI_INTX_IRQ_INDEX, VFIO_IRQ_SET_ACTION_TRIGGER, VFIO_IRQ_SET_DATA_EVENTFD, efd) == 0 &&
          vfu_irq_trigger_index(g_ctx) == VFU_DEV_INTX_IRQ, "irq=auto: INTx eventfd accepted (guest without MSI-X)");
    check(set_irqs(VFIO_PCI_INTX_IRQ_INDEX, VFIO_IRQ_SET_ACTION_UNMASK, VFIO_IRQ_SET_DATA_NONE, -1) == 0,
          "irq=auto: INTx UNMASK (DATA_NONE) accepted; the plugin registered the IRQ-state callback");
    check(rd(0, 0x1000, 1) == 0, "irq=auto: ISR status 0 with nothing raised");
    end_case(&s);

    /* irq=intx: no MSI-X */
    snprintf(args, sizeof(args), "%s,vendor-id=0x106b,device-id=0x1a03,irq=intx", nogpu);
    if (run_case(ops, dir, args, cfg, &s) < 0) { check(false, "setup with irq=intx"); return 1; }
    check(!vfu_mini_has_cap(cfg, 0x11) && vfu_mini_has_cap(cfg, 0x09) && cfg[0x3d] == 1,
          "irq=intx: no MSI-X capability, the virtio vendor capabilities, INTA#");
    check(set_irqs(VFIO_PCI_MSIX_IRQ_INDEX, VFIO_IRQ_SET_ACTION_TRIGGER, VFIO_IRQ_SET_DATA_EVENTFD, efd) < 0,
          "irq=intx: the client cannot set MSI-X eventfds");
    end_case(&s);

    /* bad args refuse setup */
    snprintf(args, sizeof(args), "%s,irq=msi-x", nogpu);
    check(run_case(ops, dir, args, cfg, &s) < 0, "irq=msi-x (unknown): setup refused");
    snprintf(args, sizeof(args), "%s,vendor-id=0xffff", nogpu);
    check(run_case(ops, dir, args, cfg, &s) < 0, "vendor-id=0xffff: setup refused");

    printf("test_vt_identity: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
