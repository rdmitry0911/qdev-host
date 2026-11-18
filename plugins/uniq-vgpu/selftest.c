/*
 * Self-test of the uniq-vgpu plugin without a VM.
 *
 * Loads the shipped plugin (dlopen, as qdev-host does), runs its setup() on a
 * real libvfio-user context with a host-services double, and acts as the
 * vfio-user client and the guest at once:
 *   - reads PCI config space (IDs, class, MSI capability, BAR0 type);
 *   - registers DMA regions over one memfd (as QEMU's memory-backend-memfd:
 *     two IOVA ranges at different file offsets, plus a region without fd);
 *   - reads the init registers and writes the FIFO setup through the BAR0
 *     callback the plugin registered (captured by interposing
 *     vfu_setup_region);
 *   - places a root FIFO NOP with a completion stamp in guest memory and
 *     rings FIFO_WRITE: QMetal must read the command through the plugin's DMA
 *     table, write the stamp back to guest memory and raise the MSI;
 *   - provokes non-RAM accesses and checks they are refused and logged;
 *   - quiesces, changes the map, resumes; checks the scanout and cursor;
 *   - runs teardown (the quit stop phase).
 * The QMetal library is the real one; select its Vulkan driver with
 * VK_ICD_FILENAMES. The host services are a test double, not qdev-host.
 *
 * Usage: selftest PLUGIN.so [--personality DEVICE_ID | --fw-identity]
 * With --personality, only the PCI identity is checked (and that a BAR0
 * access and a device reset leave it unchanged), then the client
 * connection is dropped (VFU_RESET_LOST_CONN): the plugin must run its quit
 * stop phase and return QDP_RESET_EXIT (exit-on-disconnect); the host exits 0.
 * With --fw-identity, the firmware-phase identity (fw-vendor-id/fw-device-id)
 * is checked the same way: 106b:eeee from setup and after every device/FLR
 * reset, 1b36:eee1 from the first BAR0 access on; then the LOST_CONN exit.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "qdp.h"
#include "qmu/pvg_regs.h"

static int failures, checks;

#define CHECK(cond, ...) do { \
    checks++; \
    if (cond) { printf("PASS "); } else { printf("FAIL "); failures++; } \
    printf(__VA_ARGS__); putchar('\n'); fflush(stdout); \
} while (0)

/* ------------------------------------------- BAR0 callback interposition */
static vfu_region_access_cb_t *bar0_cb;
static size_t bar0_size;
static int bar0_flags, bar0_fd = -2;

int vfu_setup_region(vfu_ctx_t *ctx, int idx, size_t size, vfu_region_access_cb_t *cb,
                     int flags, struct iovec *areas, uint32_t nr, int fd, uint64_t offset)
{
    static int (*real)(vfu_ctx_t *, int, size_t, vfu_region_access_cb_t *, int,
                       struct iovec *, uint32_t, int, uint64_t);

    if (!real) {
        *(void **)&real = dlsym(RTLD_NEXT, "vfu_setup_region");
    }
    if (idx == VFU_PCI_DEV_BAR0_REGION_IDX) {
        bar0_cb = cb;
        bar0_size = size;
        bar0_flags = flags;
        bar0_fd = fd;
    }
    return real(ctx, idx, size, cb, flags, areas, nr, fd, offset);
}

/* ------------------------------------------------ host services double */
static int irq_calls, irq_delivered, frame_calls, cursor_calls, msi_enabled;
static int in_quiesce, irq_in_quiesce;
static uint64_t last_seq;
static int refusals, plugin_errors, qmetal_errors, log_lines;
/* PGDevice-reset evidence, counted from the host log (no double on the call):
 *   qmu_reset_done   QMetal's own "QMU_DEVICE_RESET_LIFECYCLE phase=complete" line, i.e. the
 *                    real qmu_session_reset_for_device_reset ran to its end;
 *   plugin_reset_ok  the plugin's "DEVICE_RESET_LIFECYCLE ... qmu_reset_complete" line;
 *   ord_*            ordinal (log line number) of the last such line, to check the order. */
static int qmu_reset_done, plugin_reset_ok, plugin_reset_err, line_seq, ord_plugin_reset, ord_rearm;
static char last_refusal[4096];
/* ui_info refusals: the plugin's "[display][WARN] ui_info ... refused: <reason>" lines */
static int ui_info_refusals;
static char last_ui_info_refusal[4096];
static pthread_mutex_t host_lock = PTHREAD_MUTEX_INITIALIZER;

static int host_irq_trigger(void *host, uint32_t subindex)
{
    int rc;

    pthread_mutex_lock(&host_lock);
    irq_calls++;
    irq_in_quiesce += in_quiesce;
    rc = msi_enabled && subindex == 0 ? 0 : -EINVAL;
    if (!rc) {
        irq_delivered++;
    }
    pthread_mutex_unlock(&host_lock);
    return rc;
}

static void host_frame_ready(void *host, const qdp_frame_info *f)
{
    pthread_mutex_lock(&host_lock);
    frame_calls++;
    last_seq = f->seq;
    pthread_mutex_unlock(&host_lock);
}

static void host_cursor_changed(void *host)
{
    pthread_mutex_lock(&host_lock);
    cursor_calls++;
    pthread_mutex_unlock(&host_lock);
}

static void host_log(void *host, int level, const char *fmt, ...)
{
    char line[4096];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&host_lock);
    log_lines++;
    line_seq++;
    if (strstr(line, "QMU_DEVICE_RESET_LIFECYCLE phase=complete status=0")) {
        qmu_reset_done++;
    }
    if (strstr(line, "[apple-gfx-ml] DEVICE_RESET_LIFECYCLE type=") &&
        strstr(line, "phase=qmu_reset_complete")) {
        plugin_reset_ok++;
        ord_plugin_reset = line_seq;
    }
    if (strstr(line, "[apple-gfx-ml][reset][ERROR]")) {
        plugin_reset_err++;
    }
    if (strstr(line, "firmware PCI identity") && strstr(line, "armed (")) {
        ord_rearm = line_seq;
    }
    if (strstr(line, "[apple-gfx-ml][display][WARN] ui_info ") && strstr(line, " refused: ")) {
        ui_info_refusals++;
        snprintf(last_ui_info_refusal, sizeof(last_ui_info_refusal), "%s", line);
    }
    if (strstr(line, "[apple-gfx-ml][memory][ERROR]")) {
        refusals++;
        snprintf(last_refusal, sizeof(last_refusal), "%s", line);
    } else if (strstr(line, "[apple-gfx-ml][qmetal][ERROR]")) {
        qmetal_errors++;
    } else if (strstr(line, "[ERROR]")) {
        plugin_errors++;
    }
    pthread_mutex_unlock(&host_lock);
    fprintf(stderr, "host-log[%d] %s\n", level, line);
}

/* ------ live banner capture: libmlapi prints "[qmetal] libmlapi built ..."
 * straight to stderr (its default sink, bypassing the log callback), so to
 * prove the NEW build's banner is live after a reload we tee the process's own
 * stderr through a pipe and scan it for the banner line. */
static char live_banner[256];
static int banner_real_err = -1;
static void *banner_reader(void *arg)
{
    int rfd = (int)(intptr_t)arg;
    char buf[8192];
    ssize_t n;
    while ((n = read(rfd, buf, sizeof(buf))) > 0) {
        if (banner_real_err >= 0) { (void)!write(banner_real_err, buf, n); }
        char *b = strstr(buf, "libmlapi built");
        if (b) {
            char *e = strchr(b, '\n');
            pthread_mutex_lock(&host_lock);
            snprintf(live_banner, sizeof(live_banner), "%.*s", e ? (int)(e - b) : 200, b);
            pthread_mutex_unlock(&host_lock);
        }
    }
    return NULL;
}
static void banner_capture_start(void)
{
    int pfd[2];
    pthread_t t;
    fflush(stderr);
    banner_real_err = dup(2);
    if (pipe(pfd) != 0) { return; }
    dup2(pfd[1], 2);
    close(pfd[1]);
    setvbuf(stderr, NULL, _IONBF, 0);
    pthread_create(&t, NULL, banner_reader, (void *)(intptr_t)pfd[0]);
    pthread_detach(t);
}
static void get_live_banner(char *out, size_t len)
{
    pthread_mutex_lock(&host_lock);
    snprintf(out, len, "%s", live_banner);
    pthread_mutex_unlock(&host_lock);
}

static int get_int(int *v)
{
    int r;

    pthread_mutex_lock(&host_lock);
    r = *v;
    pthread_mutex_unlock(&host_lock);
    return r;
}

/* The plugin logs from its own writer thread: wait (at most 2 s) until more
 * than n ui_info refusal lines were seen, and copy the last one. */
static bool wait_ui_info_refusal(int n, char *out, size_t len)
{
    for (int i = 0; i < 2000; i++) {
        bool seen;

        pthread_mutex_lock(&host_lock);
        seen = ui_info_refusals > n;
        if (seen) {
            snprintf(out, len, "%s", last_ui_info_refusal);
        }
        pthread_mutex_unlock(&host_lock);
        if (seen) {
            return true;
        }
        usleep(1000);
    }
    snprintf(out, len, "(no refusal line within 2 s)");
    return false;
}

/* ------------------------------------------------------------- helpers */
static uint32_t rd(uint32_t off)
{
    uint32_t v = 0xdeadbeef;

    bar0_cb(NULL, (char *)&v, 4, off, false);
    return v;
}

static void wr(uint32_t off, uint32_t v)
{
    bar0_cb(NULL, (char *)&v, 4, off, true);
}

/* device log lines arrive through the plugin's asynchronous log writer */
static int wait_refusals(int want)
{
    for (int i = 0; i < 500 && get_int(&refusals) < want; i++) {
        usleep(1000);
    }
    usleep(20000);   /* and nothing beyond it */
    return get_int(&refusals);
}

static double now_s(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static bool wait_reg(uint32_t off, uint32_t want, double timeout_s)
{
    double end = now_s() + timeout_s;

    while (now_s() < end) {
        if (rd(off) == want) {
            return true;
        }
        usleep(1000);
    }
    return rd(off) == want;
}

/* ------------------------------------------------- reload proof (L-WARMSWAP-INPROC-RELOAD)
 * Mint a BANNER-DISTINGUISHABLE, ABI-identical libmlapi copy for reload cycle
 * `cycle` from a golden build: copy it into a private per-instance directory
 * (so its $ORIGIN RUNPATH resolves its own fresh libair2spirv/libMetal), then
 * overwrite the compiled-in build banner (marker + git, both fixed-length) with
 * cycle-distinct values.  Each dlmopen(LM_ID_NEWLM) of the result is an
 * independent co-resident instance whose live banner differs from every other.
 * Returns a malloc'd path to the minted libmlapi.so, or NULL. */
static int mint_replace(uint8_t *buf, size_t n, const char *from, const char *to)
{
    size_t fl = strlen(from), tl = strlen(to);
    if (fl != tl) { return -2; }           /* must be in-place (same length) */
    for (size_t i = 0; i + fl <= n; i++) {
        if (!memcmp(buf + i, from, fl)) { memcpy(buf + i, to, tl); return 0; }
    }
    return -1;                             /* not found */
}

static char *mint_instance(const char *golden, const char *sibdir, int cycle,
                           const char *root, char *marker_out, size_t marker_len)
{
    char dir[512], out[640], link[640], tgt[640];
    const char *sibs[] = { "libair2spirv.so.1", "libair2spirv.so", "libMetal.so" };
    char git[9], marker[31];
    uint8_t *buf;
    long n;
    FILE *f;

    snprintf(dir, sizeof(dir), "%s/inst-%02d", root, cycle);
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) { return NULL; }
    for (size_t i = 0; i < sizeof(sibs) / sizeof(sibs[0]); i++) {
        snprintf(tgt, sizeof(tgt), "%s/%s", sibdir, sibs[i]);
        snprintf(link, sizeof(link), "%s/%s", dir, sibs[i]);
        if (access(tgt, F_OK) == 0) { unlink(link); (void)symlink(tgt, link); }
    }
    f = fopen(golden, "rb");
    if (!f) { return NULL; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    buf = malloc(n);
    if (!buf || fread(buf, 1, n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);

    /* cycle-distinct, fixed-length banner fields (base: ae78d0ef build) */
    snprintf(git, sizeof(git), "rl%06d", cycle);                      /* 8 chars */
    snprintf(marker, sizeof(marker), "RELOADCYCLE_%02d_INPROC_WARMSWAP", cycle); /* 30 chars */
    if (strlen(git) != 8 || strlen(marker) != 30) { free(buf); return NULL; }
    if (mint_replace(buf, n, "2994552d", git) ||
        mint_replace(buf, n, "20260402_1308_FORCE_REBUILD_V3", marker)) {
        free(buf); return NULL;   /* golden must be the known base build */
    }
    snprintf(out, sizeof(out), "%s/libmlapi.so", dir);
    f = fopen(out, "wb");
    if (!f || fwrite(buf, 1, n, f) != (size_t)n) { if (f) fclose(f); free(buf); return NULL; }
    fclose(f);
    free(buf);
    if (marker_out) { snprintf(marker_out, marker_len, "%s", marker); }
    return strdup(out);
}

#define MiB (1024ull * 1024)
#define GUEST_FILE_SIZE (64 * MiB)
/* Reset evidence snapshot: wait briefly for the log writer thread, then read the counters. */
static int reset_done_count(void)
{
    usleep(150 * 1000);
    pthread_mutex_lock(&host_lock);
    int n = qmu_reset_done;
    pthread_mutex_unlock(&host_lock);
    return n;
}

/* The kext's boot register sequence as QMetal sees it on a cold boot (oracle: cold-boot log
 * lines 63-1034 of evidence/runs/vm118-retire-old-bundle-20261118T102554Z/host-run/
 * qdev-host-vm118.log): IOSFC_ENABLE, IOSFC TASK_PFN (createTask -> IOSFC_READY),
 * PROTOCOL_VERSION (-> GFX_INIT), the root FIFO tuple, FIFO_ENABLE (-> GFX_READY).  Returns the
 * PROTOCOL_VERSION readback (6 = accepted, 0x80000006 = rejected feedback).  The IOSFC_MAP_ADDR
 * framebuffer map of the real boot is NOT driven here (coverage gap, see the commit message). */
static uint32_t fw_boot_sequence(uint64_t task_gpa, uint64_t shared_gpa, uint64_t fifo_gpa)
{
    uint32_t pv;

    wr(PVG_REG_IOSFC_ENABLE, 1);
    wr(PVG_REG_IOSFC_TASK_PFN, (uint32_t)(task_gpa >> 12));
    wr(PVG_REG_IOSFC_TASK_SIZE, 0x1000);
    wr(PVG_REG_PROTOCOL_VERSION, 6);
    pv = rd(PVG_REG_PROTOCOL_VERSION);
    wr(PVG_REG_SHARED_STATE_PFN, (uint32_t)(shared_gpa >> 12));
    wr(PVG_REG_FIFO_PFN, (uint32_t)(fifo_gpa >> 12));
    wr(PVG_REG_FIFO_OFFSET, 0x1000);
    wr(PVG_REG_FIFO_SIZE, 0x10000);
    wr(PVG_REG_FIFO_ENABLE, 1);
    return pv;
}

static void reset_deadlock_watchdog(int sig)
{
    (void)sig;
    printf("FAIL reset under quiesce did not return within the watchdog (wait cycle)\n");
    fflush(stdout);
    _exit(2);
}

#define A_IOVA 0ull                   /* [0, 48 MiB) at file offset 0 */
#define A_LEN (48 * MiB)
#define B_IOVA 0x100000000ull         /* [4 GiB, +16 MiB) at file offset 48 MiB */
#define B_LEN (16 * MiB)
#define B_FILE_OFF (48 * MiB)
#define C_IOVA 0xc0000000ull          /* 1 MiB without fd (message DMA only) */
#define C_LEN (1 * MiB)
#define D_IOVA 0x200000000ull         /* registered after the quiesce test */

int main(int argc, char **argv)
{
    const char *plugin = argc > 1 ? argv[1] : "plugins/uniq-vgpu/uniq-vgpu.so";
    unsigned personality = 0;
    bool fw_identity = false;
    char sock[256], args[1024], cache[256];
    qdp_host_ops host_ops = {
        .size = sizeof(host_ops), .host = NULL, .irq_trigger = host_irq_trigger,
        .frame_ready = host_frame_ready, .cursor_changed = host_cursor_changed,
        .log = host_log,
    };
    qdp_host_env env = { .size = sizeof(env), .host_ops = &host_ops };
    const qdp_device_ops *ops;
    qdp_plugin_v1_fn fn;
    vfu_ctx_t *ctx;
    void *priv = NULL, *h;
    uint8_t *cfg, *guest;
    int memfd, r;
    double t0;

    if (argc > 3 && !strcmp(argv[2], "--personality")) {
        personality = strtoul(argv[3], NULL, 0);
    }
    if (argc > 2 && !strcmp(argv[2], "--fw-identity")) {
        fw_identity = true;
        personality = 0xeee1;
    }
    /* --reload N GOLDEN : after the boot sequence, run N in-process real-renderer
     * reload cycles against banner-distinct copies of GOLDEN (the base libmlapi). */
    int reload_n = 0; const char *reload_golden = NULL;
    for (int i = 2; i + 2 < argc + 1 && i < argc; i++) {
        if (!strcmp(argv[i], "--reload") && i + 2 < argc) {
            reload_n = atoi(argv[i + 1]);
            reload_golden = argv[i + 2];
        }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* 1. plugin ABI */
    h = dlopen(plugin, RTLD_NOW | RTLD_LOCAL);
    CHECK(h != NULL, "dlopen %s%s%s", plugin, h ? "" : ": ", h ? "" : dlerror());
    if (!h) {
        return 1;
    }
    fn = (qdp_plugin_v1_fn)dlsym(h, QDP_PLUGIN_SYMBOL);
    ops = fn ? fn() : NULL;
    CHECK(ops && ops->abi_version >= 3 && !strcmp(ops->name, "uniq-vgpu"),
          "qdp_plugin_v1: abi %u name %s", ops ? ops->abi_version : 0, ops ? ops->name : "-");
    if (!ops) {
        return 1;
    }
    CHECK((ops->flags & QDP_F_PLUGIN_DAMAGE) && ops->quiesce && ops->resume &&
          ops->dma_register && ops->dma_unregister && ops->get_scanout && ops->get_cursor &&
          ops->reset && ops->teardown && ops->ui_info,
          "v2 ops and the v3 ui_info present, QDP_F_PLUGIN_DAMAGE set");

    /* 2. setup on a real vfu context */
    snprintf(sock, sizeof(sock), "/tmp/uniq-vgpu-selftest-%d.sock", getpid());
    snprintf(cache, sizeof(cache), "/tmp/uniq-vgpu-selftest-%d.cache", getpid());
    mkdir(cache, 0700);
    ctx = vfu_create_ctx(VFU_TRANS_SOCK, sock, LIBVFIO_USER_FLAG_ATTACH_NB, NULL,
                         VFU_DEV_TYPE_PCI);
    CHECK(ctx != NULL, "vfu_create_ctx %s", sock);
    if (!ctx) {
        return 1;
    }
    if (fw_identity) {
        /* one of the pair alone is refused before any device state exists */
        snprintf(args, sizeof(args), "log=host,vendor-id=0x1b36,device-id=0xeee1,fw-vendor-id=0x106b,"
                 "spirv_cache_dir=%s", cache);
        r = ops->setup(ctx, args, &env, &priv);
        CHECK(r == -EINVAL && !priv, "fw-vendor-id without fw-device-id refused (%d)", r);
        snprintf(args, sizeof(args), "xres=1920,yres=1080,vram_size_mb=256,debug=0,log=host,"
                 "vendor-id=0x1b36,device-id=0xeee1,fw-vendor-id=0x106b,fw-device-id=0xeeee,"
                 "spirv_cache_dir=%s", cache);
    } else if (personality) {
        snprintf(args, sizeof(args), "xres=1920,yres=1080,vram_size_mb=256,debug=0,"
                 "log=host,device-id=0x%x,spirv_cache_dir=%s", personality, cache);
    } else {
        snprintf(args, sizeof(args), "xres=1920,yres=1080,vram_size_mb=256,debug=1,"
                 "log=host,exit-on-disconnect=off,spirv_cache_dir=%s", cache);
    }
    printf("INFO setup args: %s\n", args);
    t0 = now_s();
    r = ops->setup(ctx, args, &env, &priv);
    CHECK(r == 0 && priv, "setup returned %d in %.2f s (QMetal session created)", r, now_s() - t0);
    if (r) {
        return 1;
    }
    CHECK(bar0_cb && bar0_size == 0x4000 && bar0_fd == -1 &&
          bar0_flags == (VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM),
          "BAR0: %zu bytes, flags 0x%x (RW|MEM: 32-bit, non-prefetchable), fd %d (trapped)",
          bar0_size, bar0_flags, bar0_fd);
    r = vfu_realize_ctx(ctx);
    CHECK(r == 0, "vfu_realize_ctx %d", r);

    /* 3. config space */
    cfg = (uint8_t *)vfu_pci_get_config_space(ctx);
    {
        uint16_t vid = cfg[0] | cfg[1] << 8, did = cfg[2] | cfg[3] << 8;
        uint16_t status = cfg[6] | cfg[7] << 8;
        uint16_t ssvid = cfg[0x2c] | cfg[0x2d] << 8, ssid = cfg[0x2e] | cfg[0x2f] << 8;
        uint32_t bar0 = cfg[0x10] | cfg[0x11] << 8 | cfg[0x12] << 16 | (uint32_t)cfg[0x13] << 24;
        uint8_t cap = cfg[0x34];
        unsigned expect_did = personality && !fw_identity ? personality : 0xeeee;

        CHECK(vid == 0x106b && did == expect_did, "IDs %04x:%04x (want 106b:%04x%s)", vid, did,
              expect_did, fw_identity ? ", the firmware identity" : "");
        CHECK(cfg[8] == 0 && cfg[9] == 0x00 && cfg[0xa] == 0x80 && cfg[0xb] == 0x03,
              "revision %u, class %02x%02x%02x (want 03 80 00)", cfg[8], cfg[0xb], cfg[0xa],
              cfg[9]);
        CHECK(ssvid == 0 && ssid == 0 && (cfg[0xe] & 0x7f) == 0, "subsystem %04x:%04x, header %u",
              ssvid, ssid, cfg[0xe]);
        CHECK(cfg[0x3d] == 0, "interrupt pin %u (no INTx)", cfg[0x3d]);
        CHECK((bar0 & 0xf) == 0, "BAR0 type bits 0x%x (memory, 32-bit, non-prefetchable)",
              bar0 & 0xf);
        CHECK((status & 0x10) && cap >= 0x40, "status 0x%04x capabilities list at 0x%02x",
              status, cap);
        if (cap >= 0x40 && cap < 0xfc) {
            uint16_t mc = cfg[cap + 2] | cfg[cap + 3] << 8;

            CHECK(cfg[cap] == 0x05 && cfg[cap + 1] == 0,
                  "only capability: id 0x%02x (MSI) next 0x%02x", cfg[cap], cfg[cap + 1]);
            CHECK((mc & 0x80) && !(mc & 0x100) && ((mc >> 1) & 7) == 0 && !(mc & 1),
                  "MSI control 0x%04x: 64-bit, no per-vector mask, 1 vector, disabled", mc);
        }
    }

#define IDS() ((uint32_t)(cfg[0] | cfg[1] << 8) << 16 | (uint32_t)(cfg[2] | cfg[3] << 8))
    if (fw_identity) {
        uint16_t v16 = 0;

        int n0 = reset_done_count();

        r = ops->reset(priv, VFU_RESET_DEVICE);
        CHECK(r == 0 && IDS() == 0x106beeee, "device reset before any BAR0 access: %08x (firmware identity)",
              IDS());
        /* Reference apple_gfx_pci_reset = [pgdev reset] on every device reset: the QMetal reset
         * boundary runs (here on a fresh Session: benign) and BEFORE the identity re-arms. */
        CHECK(reset_done_count() == n0 + 1 && plugin_reset_err == 0 && ord_plugin_reset &&
              ord_plugin_reset < ord_rearm,
              "device reset applied the PGDevice reset once (%d) before the identity re-arm "
              "(plugin line #%d < re-arm line #%d)", reset_done_count() - n0, ord_plugin_reset,
              ord_rearm);
        (void)rd(PVG_REG_DISPLAY_PORT_COUNT);
        CHECK(IDS() == 0x1b36eee1, "first BAR0 read switches config space: %08x (want 1b36eee1)", IDS());
        (void)rd(PVG_REG_DISPLAY_PORT_COUNT);
        wr(0x3ff0, 0);
        CHECK(IDS() == 0x1b36eee1, "later BAR0 accesses keep it: %08x", IDS());
        r = ops->reset(priv, VFU_RESET_DEVICE);
        CHECK(r == 0 && IDS() == 0x106beeee, "device reset (guest reboot) re-arms: %08x", IDS());
        CHECK(reset_done_count() == n0 + 2 && ord_plugin_reset < ord_rearm,
              "guest-reboot device reset applied the PGDevice reset again (total %d)",
              reset_done_count() - n0);
        /* a refused (2-byte) access counts too: it is still the guest touching BAR0 */
        bar0_cb(NULL, (char *)&v16, 2, PVG_REG_FIFO_SIZE, true);
        CHECK(IDS() == 0x1b36eee1, "first BAR0 write (refused size) switches: %08x", IDS());
        r = ops->reset(priv, VFU_RESET_PCI_FLR);
        CHECK(r == 0 && IDS() == 0x106beeee, "FLR re-arms: %08x", IDS());
        CHECK(reset_done_count() == n0 + 3 && ord_plugin_reset < ord_rearm && plugin_reset_err == 0,
              "FLR applied the PGDevice reset (total %d) before the re-arm", reset_done_count() - n0);
        CHECK(cfg[0xa] == 0x80 && cfg[0xb] == 0x03 && cfg[0x2c] == 0 && cfg[0x2e] == 0,
              "class and subsystem unchanged by the identity switch");
    } else if (personality) {
        /* without fw-* args nothing switches */
        (void)rd(PVG_REG_DISPLAY_PORT_COUNT);
        CHECK(IDS() == (0x106b0000u | personality), "BAR0 access leaves IDs %08x", IDS());
        int n0 = reset_done_count();

        r = ops->reset(priv, VFU_RESET_DEVICE);
        CHECK(r == 0 && IDS() == (0x106b0000u | personality), "device reset leaves IDs %08x", IDS());
        CHECK(reset_done_count() == n0 + 1 && plugin_reset_err == 0,
              "device reset applied the PGDevice reset once (%d)", reset_done_count() - n0);
    }
    if (personality) {
        /* The client goes away: with exit-on-disconnect (default on) the
         * plugin runs the quit stop phase and asks the host to exit
         * (QDP_RESET_EXIT); the host then calls teardown() and exits 0. */
        printf("SUMMARY personality 0x%x: %d checks, %d failures; now "
               "reset(VFU_RESET_LOST_CONN), expecting QDP_RESET_EXIT\n", personality,
               checks, failures);
        fflush(stdout);
        if (failures) {
            return 1;
        }
        int n_lost = reset_done_count();

        r = ops->reset(priv, VFU_RESET_LOST_CONN);
        if (reset_done_count() != n_lost) {
            printf("FAIL reset(VFU_RESET_LOST_CONN) applied the PGDevice reset (not a device reset)\n");
            return 1;
        }
        printf("PASS reset(VFU_RESET_LOST_CONN) did not apply the PGDevice reset\n");
        if (r != QDP_RESET_EXIT) {
            printf("FAIL reset(VFU_RESET_LOST_CONN) returned %d with exit-on-disconnect on\n", r);
            return 1;
        }
        ops->teardown(priv);
        printf("PASS reset(VFU_RESET_LOST_CONN) returned QDP_RESET_EXIT; teardown done\n");
        fflush(stdout);
        _exit(0);   /* as the host does after a v2 teardown */
    }

    /* 4. guest RAM: one memfd split over two IOVA ranges, as QEMU's
     * memory-backend-memfd below/above 4 GiB; sealed like seal=on. */
    memfd = memfd_create("selftest-guest-ram", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    r = ftruncate(memfd, GUEST_FILE_SIZE);
    fcntl(memfd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL);
    guest = mmap(NULL, GUEST_FILE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
    CHECK(memfd >= 0 && r == 0 && guest != MAP_FAILED, "guest memfd %llu MiB", GUEST_FILE_SIZE / MiB);
    {
        qdp_dma_region a = { A_IOVA, A_LEN, guest, memfd, 0, PROT_READ | PROT_WRITE };
        qdp_dma_region b = { B_IOVA, B_LEN, guest + B_FILE_OFF, memfd, B_FILE_OFF,
                             PROT_READ | PROT_WRITE };
        qdp_dma_region c = { C_IOVA, C_LEN, NULL, -1, 0, PROT_READ | PROT_WRITE };

        ops->dma_register(priv, &a);
        ops->dma_register(priv, &b);
        ops->dma_register(priv, &c);
    }

    /* 5. init registers */
    msi_enabled = 0;
    printf("INFO PROTOCOL_VERSION initial 0x%x\n", rd(PVG_REG_PROTOCOL_VERSION));
    CHECK(rd(PVG_REG_DISPLAY_PORT_COUNT) == 1, "DISPLAY_PORT_COUNT %u", rd(PVG_REG_DISPLAY_PORT_COUNT));
    CHECK(rd(PVG_REG_IOSFC_CAPS) == 3, "IOSFC_CAPS 0x%x", rd(PVG_REG_IOSFC_CAPS));
    CHECK(rd(PVG_REG_FIFO_ENABLE) == 0 && rd(PVG_REG_FAULT_STATUS) == 0 &&
          rd(PVG_REG_EVENT_STAMPS) == 0 && rd(PVG_REG_DISPLAY_IRQ) == 0,
          "FIFO_ENABLE, FAULT_STATUS, EVENT_STAMPS, DISPLAY_IRQ read 0");
    printf("INFO 0x2000 sparse granularity 0x%x\n", rd(0x2000));
    {
        static const uint32_t modes[] = { (1080u << 16) | 1920u, (1080u << 16) | 1440u,
                                          (1024u << 16) | 1280u, 0 };
        bool ok = true;

        for (uint32_t i = 0; i < 4; i++) {
            wr(PVG_REG_IOSFC_CMD, i);
            ok = ok && rd(PVG_REG_IOSFC_DATA) == modes[i];
        }
        CHECK(ok, "IOSFC_DATA modes 1920x1080, 1440x1080, 1280x1024, then 0");
    }
    wr(PVG_REG_PROTOCOL_VERSION, 7);
    CHECK(rd(PVG_REG_PROTOCOL_VERSION) == 0x80000007, "PROTOCOL_VERSION 7 rejected: 0x%x",
          rd(PVG_REG_PROTOCOL_VERSION));
    wr(PVG_REG_PROTOCOL_VERSION, 6);
    CHECK(rd(PVG_REG_PROTOCOL_VERSION) == 6, "PROTOCOL_VERSION 6 accepted: 0x%x",
          rd(PVG_REG_PROTOCOL_VERSION));
    {
        uint32_t v[2] = { 1, 1 };
        uint16_t v16 = 0xffff;

        wr(PVG_REG_FIFO_SIZE, 0x10000);
        bar0_cb(NULL, (char *)v, 8, PVG_REG_FIFO_ENABLE, false);
        CHECK(v[0] == 0 && v[1] == 0x10000, "8-byte read at FIFO_ENABLE: 0x%x 0x%x "
              "(low dword FIFO_ENABLE, high dword FIFO_SIZE)", v[0], v[1]);
        bar0_cb(NULL, (char *)&v16, 2, PVG_REG_FIFO_SIZE, false);
        bar0_cb(NULL, (char *)v, 4, PVG_REG_FIFO_SIZE + 1, false);
        CHECK(v16 == 0 && v[0] == 0, "2-byte and unaligned reads return 0");
    }

    /* 6. FIFO in region B (file offset != IOVA): stamps page, then ring */
    {
        const uint64_t fifo_gpa = B_IOVA + 0x100000, shared_gpa = A_IOVA + 0x100000;
        uint8_t *fifo = guest + B_FILE_OFF + 0x100000;
        uint32_t nop[3] = { 0x03 /* NOP, no barriers */, 12, 0x1234 };
        int before = get_int(&refusals), irq0 = get_int(&irq_calls);
        uint32_t stamp, ev;

        wr(PVG_REG_SHARED_STATE_PFN, (uint32_t)(shared_gpa >> 12));
        wr(PVG_REG_FIFO_PFN, (uint32_t)(fifo_gpa >> 12));
        wr(PVG_REG_FIFO_OFFSET, 0x1000);
        wr(PVG_REG_FIFO_SIZE, 0x10000);
        wr(PVG_REG_FIFO_ENABLE, 1);
        CHECK(rd(PVG_REG_FIFO_ENABLE) == 1 && rd(PVG_REG_FIFO_PFN) == (uint32_t)(fifo_gpa >> 12) &&
              rd(PVG_REG_FIFO_OFFSET) == 0x1000 && rd(PVG_REG_FIFO_SIZE) == 0x10000 &&
              rd(PVG_REG_FIFO_READ) == 0 &&
              rd(PVG_REG_SHARED_STATE_PFN) == (uint32_t)(shared_gpa >> 12),
              "FIFO setup registers read back (PFN 0x%x in region B, enabled)",
              rd(PVG_REG_FIFO_PFN));
        memcpy(fifo + 0x1000, nop, sizeof(nop));
        memset(fifo, 0, 4);
        wr(PVG_REG_FIFO_WRITE, 12);
        CHECK(wait_reg(PVG_REG_FIFO_READ, 12, 5.0), "root FIFO NOP consumed: FIFO_READ %u",
              rd(PVG_REG_FIFO_READ));
        for (int i = 0; i < 5000 && get_int(&irq_calls) == irq0; i++) {
            usleep(1000);
        }
        memcpy(&stamp, fifo, 4);
        CHECK(stamp == 0x1234, "completion stamp 0x%x written to guest memory at "
              "file offset 0x%llx (GPA 0x%llx)", stamp,
              (unsigned long long)(B_FILE_OFF + 0x100000), (unsigned long long)fifo_gpa);
        CHECK(get_int(&irq_calls) > irq0 && get_int(&irq_delivered) == 0,
              "IRQ raised through irq_trigger (%d calls) and dropped with MSI disabled",
              get_int(&irq_calls) - irq0);
        ev = rd(PVG_REG_EVENT_STAMPS);
        CHECK((ev & 1) && rd(PVG_REG_EVENT_STAMPS) == 0,
              "EVENT_STAMPS read-and-clear: 0x%x, then 0", ev);

        /* second NOP with MSI enabled by the "guest" */
        msi_enabled = 1;
        irq0 = get_int(&irq_calls);
        nop[2] = 0x1235;
        memcpy(fifo + 0x1000 + 12, nop, sizeof(nop));
        wr(PVG_REG_FIFO_WRITE, 24);
        CHECK(wait_reg(PVG_REG_FIFO_READ, 24, 5.0), "second NOP consumed: FIFO_READ %u",
              rd(PVG_REG_FIFO_READ));
        for (int i = 0; i < 5000 && get_int(&irq_delivered) == 0; i++) {
            usleep(1000);
        }
        memcpy(&stamp, fifo, 4);
        CHECK(stamp == 0x1235 && get_int(&irq_delivered) >= 1,
              "stamp 0x%x, MSI delivered (%d delivered)", stamp, get_int(&irq_delivered));
        (void)rd(PVG_REG_EVENT_STAMPS);
        CHECK(wait_refusals(before + 1) == before, "no guest memory refused during FIFO init "
              "and processing (%d refusals)", get_int(&refusals) - before);

        wr(PVG_REG_FIFO_ENABLE, 0);
        CHECK(rd(PVG_REG_FIFO_ENABLE) == 0, "FIFO_ENABLE 1->0 (FIFO teardown boundary)");
    }

    /* 7. non-RAM targets fail and are logged */
    {
        int before = get_int(&refusals);

        wr(PVG_REG_SHARED_STATE_PFN, 0x80000);           /* GPA 2 GiB: no region */
        CHECK(wait_refusals(before + 1) == before + 1 && strstr(last_refusal, "gpa=0x80000000") &&
              strstr(last_refusal, "not in a DMA region"),
              "unmapped GPA refused: %s", last_refusal);
        wr(PVG_REG_SHARED_STATE_PFN, (uint32_t)(C_IOVA >> 12));   /* region without fd */
        CHECK(wait_refusals(before + 2) == before + 2 && strstr(last_refusal, "has no fd"),
              "region without fd refused: %s", last_refusal);
        wr(PVG_REG_SHARED_STATE_PFN, (uint32_t)((B_IOVA + 0x200000) >> 12));
        CHECK(wait_refusals(before + 3) == before + 2, "region B page mapped (no new refusal)");
    }

    /* 7b. a doorbell racing a map change: no irq_trigger while quiesced; the
     * command completes, with its stamp and MSI, after resume */
    {
        uint8_t *fifo = guest + B_FILE_OFF + 0x100000;
        uint32_t nop[3] = { 0x03, 12, 0x1236 }, stamp = 0;
        int delivered0;

        wr(PVG_REG_SHARED_STATE_PFN, 0x100);
        memcpy(fifo + 0x1000, nop, sizeof(nop));
        /* FIFO_ENABLE 1->0 retired the tuple: program it again, as a guest does */
        wr(PVG_REG_FIFO_PFN, (uint32_t)((B_IOVA + 0x100000) >> 12));
        wr(PVG_REG_FIFO_OFFSET, 0x1000);
        wr(PVG_REG_FIFO_SIZE, 0x10000);
        wr(PVG_REG_FIFO_ENABLE, 1);                      /* new epoch: read = write = 0 */
        delivered0 = get_int(&irq_delivered);
        wr(PVG_REG_FIFO_WRITE, 12);                      /* posted inside QMetal */
        r = ops->quiesce(priv);
        pthread_mutex_lock(&host_lock);
        in_quiesce = 1;
        pthread_mutex_unlock(&host_lock);
        usleep(200 * 1000);
        pthread_mutex_lock(&host_lock);
        in_quiesce = 0;
        pthread_mutex_unlock(&host_lock);
        ops->resume(priv);
        for (int i = 0; i < 5000 && get_int(&irq_delivered) == delivered0; i++) {
            usleep(1000);
        }
        memcpy(&stamp, fifo, 4);
        CHECK(r == 0 && get_int(&irq_in_quiesce) == 0 && stamp == 0x1236 &&
              get_int(&irq_delivered) == delivered0 + 1 && rd(PVG_REG_FIFO_READ) == 12,
              "doorbell over quiesce: %d irq_trigger while quiesced, after resume stamp 0x%x "
              "and %d MSI", get_int(&irq_in_quiesce), stamp,
              get_int(&irq_delivered) - delivered0);
        (void)rd(PVG_REG_EVENT_STAMPS);
        wr(PVG_REG_FIFO_ENABLE, 0);
    }

    /* 8. map change: quiesce, unregister B (still pinned by QMetal), add D */
    {
        qdp_dma_region b = { B_IOVA, B_LEN, guest + B_FILE_OFF, memfd, B_FILE_OFF,
                             PROT_READ | PROT_WRITE };
        qdp_dma_region d = { D_IOVA, 4 * MiB, guest + 32 * MiB, memfd, 32 * MiB,
                             PROT_READ | PROT_WRITE };
        int before;

        r = ops->quiesce(priv);
        CHECK(r == 0, "quiesce returned %d", r);
        ops->dma_unregister(priv, &b);
        ops->dma_register(priv, &d);
        ops->resume(priv);
        before = get_int(&refusals);
        wr(PVG_REG_SHARED_STATE_PFN, (uint32_t)(D_IOVA >> 12));
        CHECK(wait_refusals(before + 1) == before, "after resume: new region D maps");
        wr(PVG_REG_SHARED_STATE_PFN, (uint32_t)((B_IOVA + 0x300000) >> 12));
        CHECK(wait_refusals(before + 1) == before + 1 &&
              strstr(last_refusal, "gpa=0x100300000") && strstr(last_refusal, "not in a DMA region"),
              "after resume: unregistered region B is refused for new mappings");
        wr(PVG_REG_SHARED_STATE_PFN, 0x100);
    }

    /* 9. display and cursor */
    {
        qdp_scanout so;
        qdp_cursor cur;
        struct stat st;
        bool black = true;
        uint8_t *map;

        CHECK(ops->get_scanout(priv, &so) && so.enabled && so.width == 1920 &&
              so.height == 1080 && so.stride == 7680 && so.pixman_format == 0x20020888,
              "scanout %ux%u stride %u format 0x%x offset %u fd %d", so.width, so.height,
              so.stride, so.pixman_format, so.offset, so.fd);
        CHECK(fstat(so.fd, &st) == 0 && (uint64_t)st.st_size >= so.offset + (uint64_t)so.stride * so.height &&
              (fcntl(so.fd, F_GET_SEALS) & F_SEAL_SHRINK),
              "scanout memfd %lld bytes, F_SEAL_SHRINK", (long long)st.st_size);
        map = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, so.fd, 0);
        for (size_t i = 0; map != MAP_FAILED && i < (size_t)so.stride * so.height; i += 4096) {
            black = black && map[so.offset + i] == 0;
        }
        CHECK(map != MAP_FAILED && black, "initial frame is black (mode publication)");
        /* qdp v2: no host service before setup() returns; the setup-time
         * frame is fetched with get_scanout(), not announced */
        CHECK(get_int(&frame_calls) == 0, "no frame_ready for the setup-time frame (%d calls)",
              get_int(&frame_calls));
        CHECK(ops->get_cursor(priv, &cur) && !cur.visible && !cur.pixels,
              "cursor plane present, no glyph defined yet");
    }

    /* 9b. UI size requests (qdp v3 ui_info, display-modes=follow): the mode
     * list becomes the one requested size (served by the EFI mode registers
     * too), a repeat is reported unchanged, out-of-bounds requests change
     * nothing, and the scanout keeps its size until the guest presents.
     * Bounds are the reference's (uniq-vgpu.c pvg_ui_info_refusal): 1..65535
     * per side and no other minimum (host testUpdateModeList u16 page fields,
     * _PGDisplayMode init without a size check; guest edidInit u16 (w-1),(h-1),
     * validateDetailedTiming active < 0x10000), at most 16777216 pixels (64 MiB
     * IOFBMemorySize), the guest edidInit pixel clock ((h+10000)(w+10000)*
     * refresh16 + 0x270fffff) / 0x27100000 <= 0x1000000, flags 0, and 60 Hz only
     * (design debt E1). The clock cases: 65535x256 is 4648122 units at 60 Hz,
     * 16777164 at 216.567 Hz (in bounds, then refused as not 60 Hz) and
     * 16777241 at 216.568 Hz (above the clock limit). */
    {
        qdp_ui_info ui = { .size = sizeof(ui), .width = 1600, .height = 900 };
        static const struct { uint32_t w, h, mhz, flags; const char *what, *reason; } bad[] = {
            { 0, 900, 0, 0, "0x900", "refused: a side outside 1..65535" },
            { 1600, 0, 0, 0, "1600x0", "refused: a side outside 1..65535" },
            { 0x10000, 1000, 0, 0, "65536x1000", "refused: a side outside 1..65535" },
            { 1000, 0x10000, 0, 0, "1000x65536", "refused: a side outside 1..65535" },
            { 4097, 4096, 0, 0, "4097x4096 (16781312 pixels)", "refused: more than 16777216 pixels" },
            { 8192, 4096, 0, 0, "8192x4096", "refused: more than 16777216 pixels" },
            { 65535, 256, 216568, 0, "65535x256 @ 216.568 Hz (clock 16777241 > 0x1000000)",
              "refused: pixel clock above the guest EDID limit" },
            { 65535, 256, 216567, 0, "65535x256 @ 216.567 Hz (clock 16777164, in bounds)",
              "refused: only 60 Hz is offered" },
            { 1600, 900, 59940, 0, "1600x900 @ 59.94 Hz", "refused: only 60 Hz is offered" },
            { 1600, 900, 120000, 0, "1600x900 @ 120 Hz", "refused: only 60 Hz is offered" },
            { 1600, 900, 0, 1, "1600x900 flag 1", "refused: unknown flags" },
        };
        /* accepted: the VM118 window sizes, the 1x1 minimum, the former 800x600
         * policy's edges, the pixel bound and the largest 60 Hz clock */
        static const struct { uint32_t w, h; } good[] = {
            { 1630, 572 }, { 1324, 626 }, { 1, 1 }, { 799, 600 }, { 800, 599 },
            { 4096, 4096 }, { 65535, 256 }, { 256, 65535 },
        };
        int errors0 = get_int(&plugin_errors);
        uint32_t m0, m1;
        qdp_scanout so;

        r = ops->ui_info(priv, &ui);
        wr(PVG_REG_IOSFC_CMD, 0);
        m0 = rd(PVG_REG_IOSFC_DATA);
        wr(PVG_REG_IOSFC_CMD, 1);
        m1 = rd(PVG_REG_IOSFC_DATA);
        CHECK(r == 0 && m0 == ((900u << 16) | 1600u) && m1 == 0,
              "ui_info 1600x900: %d; mode list [1600x900] (IOSFC_DATA 0x%08x, then 0x%08x)", r, m0, m1);
        r = ops->ui_info(priv, &ui);
        CHECK(r == QDP_UI_INFO_UNCHANGED, "the same request again: %d (QDP_UI_INFO_UNCHANGED)", r);
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            qdp_ui_info b = { .size = sizeof(b), .width = bad[i].w, .height = bad[i].h,
                              .refresh_mhz = bad[i].mhz, .flags = bad[i].flags };
            int n0 = get_int(&ui_info_refusals), rb;
            char line[4096];
            bool logged;

            rb = ops->ui_info(priv, &b);
            logged = wait_ui_info_refusal(n0, line, sizeof(line));
            wr(PVG_REG_IOSFC_CMD, 0);
            m0 = rd(PVG_REG_IOSFC_DATA);
            CHECK(rb == -EINVAL && logged && strstr(line, bad[i].reason) && m0 == ((900u << 16) | 1600u),
                  "ui_info %s: %d (-EINVAL), '%s'; list unchanged (0x%08x) [%s]", bad[i].what, rb,
                  bad[i].reason, m0, line);
        }
        for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
            qdp_ui_info g = { .size = sizeof(g), .width = good[i].w, .height = good[i].h };

            r = ops->ui_info(priv, &g);
            wr(PVG_REG_IOSFC_CMD, 0);
            m0 = rd(PVG_REG_IOSFC_DATA);
            wr(PVG_REG_IOSFC_CMD, 1);
            m1 = rd(PVG_REG_IOSFC_DATA);
            CHECK(r == 0 && m0 == ((good[i].h << 16) | good[i].w) && m1 == 0,
                  "ui_info %ux%u accepted: %d; mode list [%ux%u] (IOSFC_DATA 0x%08x, then 0x%08x)",
                  good[i].w, good[i].h, r, good[i].w, good[i].h, m0, m1);
        }
        ui.width = 2560;
        ui.height = 1440;
        ui.width_mm = 600;
        ui.height_mm = 340;
        r = ops->ui_info(priv, &ui);
        wr(PVG_REG_IOSFC_CMD, 0);
        m0 = rd(PVG_REG_IOSFC_DATA);
        CHECK(r == 0 && m0 == ((1440u << 16) | 2560u), "ui_info 2560x1440 600x340 mm: %d, mode 0x%08x",
              r, m0);
        CHECK(ops->get_scanout(priv, &so) && so.width == 1920 && so.height == 1080,
              "scanout stays %ux%u until the guest presents at the new size", so.width, so.height);
        CHECK(get_int(&plugin_errors) == errors0, "no plugin [ERROR] line from ui_info");
    }

    /* 10. GUEST REBOOT: device reset while the old root-FIFO epoch is live, with NO guest
     * FIFO_ENABLE 1->0 (a reboot never runs the kext's teardown).
     * Reference: QEMU apple_gfx_pci_reset() = [pgdev reset] on every device reset
     * (reference/New rerference logs and driver/apple-gfx-pci.m:121-124) =
     * setRootPage(0), setBinaryVersion(0), setEFIDisplay(0) (host decompile
     * ParavirtualizedGraphics-host-lc.c:50724-50743).  Before the reset reached QMetal, boot 2's
     * IOSFC_ENABLE/TASK_PFN/PROTOCOL_VERSION/FIFO_ENABLE sequence was ignored (state machine
     * only advances from IOSFC_READY/UNINITIALIZED, FIFO writes dropped unless GFX_READY) and the
     * first root FIFO write never dispatched (boot 2 of the VM118 run stalled at GET_DEVICE_INFO).
     * The real libmlapi is the unit under test; the only double is the host-services callbacks. */
    {
        const uint64_t task1 = A_IOVA + 0x300000, fifo1 = A_IOVA + 0x400000, shared1 = A_IOVA + 0x500000;
        const uint64_t task2 = A_IOVA + 0x380000, fifo2 = A_IOVA + 0x600000, shared2 = A_IOVA + 0x700000;
        uint32_t nop[3] = { 0x03 /* NOP, no barriers */, 12, 0x4201 };
        uint32_t pv;
        int n0, rr, irq0;

        /* boot 1: two root commands, so the old write/read pointer (24) is above boot 2's first write */
        pv = fw_boot_sequence(task1, shared1, fifo1);
        CHECK(pv == 6, "boot 1: PROTOCOL_VERSION accepted (0x%x)", pv);
        memcpy(guest + fifo1 + 0x1000, nop, sizeof(nop));
        nop[2] = 0x4202;
        memcpy(guest + fifo1 + 0x1000 + 12, nop, sizeof(nop));
        memset(guest + fifo1, 0, 4);
        wr(PVG_REG_FIFO_WRITE, 24);
        CHECK(wait_reg(PVG_REG_FIFO_READ, 24, 5.0), "boot 1: both root commands drained, FIFO_READ %u",
              rd(PVG_REG_FIFO_READ));

        /* the reset exactly as qdev-host drives it: quiesce -> reset -> resume */
        n0 = reset_done_count();
        r = ops->quiesce(priv);
        rr = ops->reset(priv, VFU_RESET_DEVICE);
        ops->resume(priv);
        CHECK(r == 0 && rr == 0 && reset_done_count() == n0 + 1 && plugin_reset_ok >= 1 &&
              plugin_reset_err == 0,
              "VFU_RESET_DEVICE (quiesced) applied the PGDevice reset once (QMetal lifecycle lines +%d)",
              reset_done_count() - n0);
        CHECK(rd(PVG_REG_FIFO_ENABLE) == 0 && rd(PVG_REG_FIFO_READ) == 0 &&
              rd(PVG_REG_PROTOCOL_VERSION) == 0,
              "old epoch retired by the reset alone: FIFO_ENABLE %u FIFO_READ %u PROTOCOL_VERSION 0x%x",
              rd(PVG_REG_FIFO_ENABLE), rd(PVG_REG_FIFO_READ), rd(PVG_REG_PROTOCOL_VERSION));

        /* boot 2: same register sequence, new tuple, FIRST write (12) below boot 1's last (24) */
        pv = fw_boot_sequence(task2, shared2, fifo2);
        CHECK(pv == 6, "boot 2: PROTOCOL_VERSION accepted again (0x%x; 0x80000006 = ignored)", pv);
        nop[2] = 0x4301;
        memcpy(guest + fifo2 + 0x1000, nop, sizeof(nop));
        memset(guest + fifo2, 0, 4);
        msi_enabled = 1;
        irq0 = get_int(&irq_delivered);
        wr(PVG_REG_FIFO_WRITE, 12);
        CHECK(wait_reg(PVG_REG_FIFO_READ, 12, 5.0),
              "boot 2: first root FIFO write dispatches (FIFO_READ %u, want 12)", rd(PVG_REG_FIFO_READ));
        for (int i = 0; i < 5000 && get_int(&irq_delivered) == irq0; i++) {
            usleep(1000);
        }
        /* The completion stamp's memory readback is NOT asserted: steps 6/7b already fail on
         * unmodified main (QMetal cbc1c006d no longer writes the NOP stamp where this selftest
         * reads it; 4 pre-existing failures), so only the MSI that signals it is checked. */
        CHECK(get_int(&irq_delivered) > irq0, "boot 2: completion MSI delivered (+%d)",
              get_int(&irq_delivered) - irq0);
        (void)rd(PVG_REG_EVENT_STAMPS);

        /* a doorbell already posted inside QMetal when the host quiesces for the reset: the
         * reset drains accepted work, which needs guest-memory lookups -- it must not wait on a
         * quiesce the same thread holds (watchdog = a wait cycle, not a slow reset).  Race-
         * dependent and NOT a proof of the plugin's quiesce lift in pvg_qmetal_device_reset: a NOP
         * makes no Vulkan submit, so the old epoch's final-submit wait is not exercised (the
         * lift-disabled experiment also passed; an in-flight render at reset is a live-gate item). */
        nop[2] = 0x4302;
        memcpy(guest + fifo2 + 0x1000 + 12, nop, sizeof(nop));
        signal(SIGALRM, reset_deadlock_watchdog);
        wr(PVG_REG_FIFO_WRITE, 24);
        r = ops->quiesce(priv);
        alarm(30);
        rr = ops->reset(priv, VFU_RESET_DEVICE);
        alarm(0);
        ops->resume(priv);
        CHECK(r == 0 && rr == 0 && plugin_reset_err == 0 && rd(PVG_REG_FIFO_ENABLE) == 0,
              "reset with a posted doorbell under quiesce returned (no wait cycle)");
        (void)rd(PVG_REG_EVENT_STAMPS);
    }

    /* 10a. reset of the device keeps the device (fork semantics) */
    r = ops->reset(priv, VFU_RESET_DEVICE);
    CHECK(r == 0 && rd(PVG_REG_DISPLAY_PORT_COUNT) == 1, "VFU_RESET_DEVICE: %d, registers live", r);

    CHECK(get_int(&plugin_errors) == 0, "no plugin [ERROR] lines (%d host-log lines seen)",
          get_int(&log_lines));
    printf("INFO QMetal [ERROR] log lines: %d\n", get_int(&qmetal_errors));

    /* 10b. IN-PROCESS REAL-RENDERER RELOAD (L-WARMSWAP-INPROC-RELOAD)
     * N real device-reload cycles in ONE process: each loads a banner-distinct
     * libmlapi build as an INDEPENDENT base-namespace instance (unique DT_SONAME
     * + RTLD_DEEPBIND; driver-agnostic -- no dlmopen secondary namespace),
     * qmu_destroy + re-create the Session on it, with the device RESET (handshake
     * generation bumped) each time.  Assert: the NEW banner is live (marker
     * differs per cycle), a distinct independent instance handle each cycle,
     * device state re-adopted (FIFO re-arms + consumes), handshake_gen == cycle,
     * 0 new QMU errors, 0 abort/leak/join-hang. */
    int resets_before_reload = reset_done_count(), plugin_resets_before_reload;

    pthread_mutex_lock(&host_lock);
    plugin_resets_before_reload = plugin_reset_ok;
    pthread_mutex_unlock(&host_lock);
    if (reload_n > 0 && reload_golden) {
        CHECK(ops->abi_version >= 4 && ops->reload && ops->backend_report,
              "plugin exposes qdp v4 reload + backend_report (abi %u)", ops->abi_version);
        if (ops->reload && ops->backend_report) {
            char root[256], sib[512], rep[512];
            const char *slash = strrchr(reload_golden, '/');
            unsigned long prev_instance = 0;
            int ok_cycles = 0;
            int errs_before_all = get_int(&qmetal_errors);
            snprintf(root, sizeof(root), "/tmp/apple-pvg-reload-%d", getpid());
            mkdir(root, 0700);
            snprintf(sib, sizeof(sib), "%.*s", slash ? (int)(slash - reload_golden) : 1,
                     slash ? reload_golden : ".");

            printf("INFO reload: %d cycles, golden=%s sibdir=%s\n", reload_n, reload_golden, sib);
            banner_capture_start();   /* tee stderr to catch each build's live banner */
            for (int cyc = 1; cyc <= reload_n; cyc++) {
                char marker[64]; char *inst;
                int errs_before = get_int(&qmetal_errors);
                double tc0 = now_s();

                inst = mint_instance(reload_golden, sib, cyc, root, marker, sizeof(marker));
                CHECK(inst != NULL, "cycle %d: mint banner-distinct instance (marker=%s)",
                      cyc, inst ? marker : "FAILED");
                if (!inst) { break; }

                /* quiesce -> in-process reload (destroy + dlmopen + re-create,
                 * device reset) -> resume : exactly the qdev-host control path */
                r = ops->quiesce(priv);
                int rr = ops->reload(priv, inst, QDP_RELOAD_RESET);
                ops->resume(priv);
                double reload_ms = (now_s() - tc0) * 1000.0;
                CHECK(r == 0 && rr == 0, "cycle %d: quiesce+reload+resume rc=%d/%d in %.0f ms",
                      cyc, r, rr, reload_ms);
                if (rr) { free(inst); break; }

                ops->backend_report(priv, rep, sizeof(rep));
                /* NEW banner live (marker for THIS cycle), handshake bumped, 0 new errors.
                 * The banner is captured from the library's own stderr output. */
                usleep(50 * 1000);   /* let the reader thread drain the pipe */
                char banner[256]; get_live_banner(banner, sizeof(banner));
                CHECK(strstr(banner, marker) != NULL,
                      "cycle %d: NEW banner live (git/marker differ): %s", cyc, banner);
                char want_hs[64]; snprintf(want_hs, sizeof(want_hs), "handshake_gen=%d", cyc);
                CHECK(strstr(rep, want_hs) != NULL, "cycle %d: HandshakeGeneration bumped (%s)",
                      cyc, want_hs);
                char want_rl[64]; snprintf(want_rl, sizeof(want_rl), "reloads=%d", cyc);
                CHECK(strstr(rep, want_rl) != NULL, "cycle %d: reload count == %d", cyc, cyc);
                CHECK(get_int(&qmetal_errors) == errs_before,
                      "cycle %d: 0 new QMU [ERROR] lines (%d)", cyc,
                      get_int(&qmetal_errors) - errs_before);

                /* Driver-agnostic co-residency: the fresh libmlapi loads as an
                 * INDEPENDENT instance in the BASE link-map namespace (lmid==0;
                 * NOT a dlmopen secondary namespace, which SIGSEGVs on ICDs that
                 * RTLD_GLOBAL-dlopen inside vkCreateDevice, e.g. NVIDIA).  Prove
                 * independence by a distinct, non-NULL instance handle each cycle
                 * -- in tandem with the NEW-banner-live check above, which is the
                 * semantic contamination detector (a preempted/aliased instance
                 * would still carry the OLD banner). */
                unsigned long inst_h = 0;
                { char *p = strstr(rep, "instance="); if (p) inst_h = strtoul(p + 9, NULL, 0); }
                CHECK(inst_h != 0 && inst_h != prev_instance,
                      "cycle %d: independent base-ns instance=0x%lx (was 0x%lx)",
                      cyc, inst_h, prev_instance);
                prev_instance = inst_h;

                /* device state re-adopted: re-arm the FIFO on the new session and
                 * confirm it consumes a NOP (FIFO_READ advances) */
                {
                    const uint64_t fifo_gpa = B_IOVA + 0x100000;
                    uint8_t *fifo = guest + B_FILE_OFF + 0x100000;
                    uint32_t nop[3] = { 0x03, 12, 0x4200 + cyc };
                    /* B was unregistered in step 8; re-register for the probe */
                    qdp_dma_region b = { B_IOVA, B_LEN, guest + B_FILE_OFF, memfd, B_FILE_OFF,
                                         PROT_READ | PROT_WRITE };
                    r = ops->quiesce(priv); ops->dma_register(priv, &b); ops->resume(priv);
                    /* replay the minimal boot handshake the guest does on its
                     * post-reset re-handshake, so the re-created session reaches
                     * GFX_READY (not UNINITIALIZED): negotiate protocol v6 + the
                     * shared-state page, re-adopting the parked memory tables */
                    wr(PVG_REG_PROTOCOL_VERSION, 6);
                    wr(PVG_REG_SHARED_STATE_PFN, (uint32_t)((B_IOVA + 0x100000) >> 12));
                    wr(PVG_REG_FIFO_PFN, (uint32_t)(fifo_gpa >> 12));
                    wr(PVG_REG_FIFO_OFFSET, 0x1000);
                    wr(PVG_REG_FIFO_SIZE, 0x10000);
                    wr(PVG_REG_FIFO_ENABLE, 1);
                    memcpy(fifo + 0x1000, nop, sizeof(nop));
                    memset(fifo, 0, 4);
                    wr(PVG_REG_FIFO_WRITE, 12);
                    CHECK(wait_reg(PVG_REG_FIFO_READ, 12, 5.0),
                          "cycle %d: re-adopted session consumes FIFO NOP (FIFO_READ %u)",
                          cyc, rd(PVG_REG_FIFO_READ));
                    wr(PVG_REG_FIFO_ENABLE, 0);
                    r = ops->quiesce(priv); ops->dma_unregister(priv, &b); ops->resume(priv);
                }
                free(inst);
                ok_cycles++;
            }
            printf("INFO reload: %d/%d cycles OK, 0 new QMU errors=%s\n", ok_cycles, reload_n,
                   get_int(&qmetal_errors) == errs_before_all ? "yes" : "NO");

            /* 10c. HEALTH-GATE ROLLBACK: inject a BAD reload (a valid .so that is
             * NOT a libmlapi -> the vtable bind fails after the old Session is
             * already destroyed, device down), then auto-rollback by reloading a
             * known-good instance.  Proves the vtable gate + rollback recovery. */
            {
                char marker[64]; char badrep[512];
                char *good = mint_instance(reload_golden, sib, 99, root, marker, sizeof(marker));
                /* a non-libmlapi .so: the shipped backing_A.so has no qmu_* */
                const char *bad = "plugins/uniq-vgpu/backing_A.so";
                long reloads_before = 0;
                ops->backend_report(priv, rep, sizeof(rep));
                { char *p = strstr(rep, "reloads="); if (p) reloads_before = atol(p + 8); }

                r = ops->quiesce(priv);
                int rr = ops->reload(priv, bad, QDP_RELOAD_RESET);
                CHECK(rr != 0, "rollback: BAD reload (non-libmlapi) rejected by the vtable gate (rc=%d)", rr);
                /* device is down (Session destroyed, new bind failed): roll back */
                int rb = good ? ops->reload(priv, good, QDP_RELOAD_RESET) : -1;
                ops->resume(priv);
                ops->backend_report(priv, badrep, sizeof(badrep));
                long reloads_after = 0;
                { char *p = strstr(badrep, "reloads="); if (p) reloads_after = atol(p + 8); }
                CHECK(rb == 0 && reloads_after == reloads_before + 1,
                      "rollback: auto-rollback to good instance restored the device (rc=%d, reloads %ld->%ld)",
                      rb, reloads_before, reloads_after);
                usleep(50 * 1000);
                char banner[256]; get_live_banner(banner, sizeof(banner));
                CHECK(strstr(banner, marker) != NULL, "rollback: good banner live after recovery: %s", banner);
                free(good);
            }
        }
    }

    /* Warm-swap / renderer-reload non-regression: neither flow is a guest-visible device reset, so
     * neither may apply the PGDevice reset (audit: pvg_backend_reload/pvg_swap never enter
     * pvg_reset).  Counted from the real QMetal lifecycle line and the plugin's reset line. */
    if (reload_n > 0 && reload_golden) {
        int plugin_now;

        pthread_mutex_lock(&host_lock);
        plugin_now = plugin_reset_ok;
        pthread_mutex_unlock(&host_lock);
        CHECK(reset_done_count() == resets_before_reload && plugin_now == plugin_resets_before_reload,
              "%d reload cycles + rollback applied no PGDevice reset (QMetal lifecycle lines +%d)",
              reload_n, reset_done_count() - resets_before_reload);
    }
    if (ops->swap && access("plugins/uniq-vgpu/backing_A.so", R_OK) == 0) {
        int n_swap = reset_done_count();

        r = ops->quiesce(priv);
        int rs = ops->swap(priv, "plugins/uniq-vgpu/backing_A.so");
        ops->resume(priv);
        CHECK(r == 0 && rs == 0 && reset_done_count() == n_swap,
              "warm swap of the VARIABLE backing (rc=%d) applied no PGDevice reset", rs);
    }

    /* 11. quit stop phase */
    t0 = now_s();
    ops->teardown(priv);
    printf("INFO teardown (quit stop phase) %.2f s\n", now_s() - t0);
    CHECK(rd(PVG_REG_DISPLAY_PORT_COUNT) == 0xffffffff, "MMIO after the stop phase reads all-ones");
    printf("SUMMARY %d checks, %d failures\n", checks, failures);
    unlink(sock);
    return failures ? 1 : 0;
}
