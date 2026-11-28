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
/* teardownDisplays, host side: the plugin returned its display mode target before the PGDevice
 * reset and published the mode again after it ("phase=display_republished target=1 restart=0"). */
static int display_republish_ok;
/* DISP-D1 knob: the plugin's "[display] DISP-D1 dest=<0|1> dmabuf=<fd|-1>" line at scanout init (-2 = not seen) */
static int d1_dest_seen = -2, d1_dmabuf_seen = -2;
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
    if (strstr(line, "[uniq-vgpu-ml] DEVICE_RESET_LIFECYCLE type=") &&
        strstr(line, "phase=qmu_reset_complete")) {
        plugin_reset_ok++;
        ord_plugin_reset = line_seq;
    }
    if (strstr(line, "[uniq-vgpu-ml][reset][ERROR]")) {
        plugin_reset_err++;
    }
    if (strstr(line, "DEVICE_RESET_LIFECYCLE") &&
        strstr(line, "phase=display_republished target=1 restart=0")) {
        display_republish_ok++;
    }
    if (strstr(line, "firmware PCI identity") && strstr(line, "armed (")) {
        ord_rearm = line_seq;
    }
    if (strstr(line, "[uniq-vgpu-ml][display] DISP-D1 dest=")) {
        (void)sscanf(strstr(line, "dest="), "dest=%d dmabuf=%d", &d1_dest_seen, &d1_dmabuf_seen);
    }
    if (strstr(line, "[uniq-vgpu-ml][display][WARN] ui_info ") && strstr(line, " refused: ")) {
        ui_info_refusals++;
        snprintf(last_ui_info_refusal, sizeof(last_ui_info_refusal), "%s", line);
    }
    if (strstr(line, "[uniq-vgpu-ml][memory][ERROR]")) {
        refusals++;
        snprintf(last_refusal, sizeof(last_refusal), "%s", line);
    } else if (strstr(line, "[uniq-vgpu-ml][qmetal][ERROR]")) {
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

/* --------------------------------------------- reload residue (D-RELOAD-RESIDUE / R5)
 * Per-process resources a retired libmlapi instance may keep after its reload:
 * threads, resident memory (anonymous = heap/stacks; file = mapped code), the
 * mappings of retired unique-SONAME copies, and the GPU device fds that a live
 * VkInstance/VkDevice holds (RADV: /dev/dri/renderD*, NVIDIA: /dev/nvidia*). */
#define RESIDUE_WARM_CYCLES 3
#define RESIDUE_MIN_CYCLES 13
#define RESIDUE_RSS_KB_PER_CYCLE 1024
struct residue {
    long threads, rss_kb, anon_kb, file_kb;
    int copy_maps, gpu_fds;
};

static long status_kb(const char *text, const char *key)
{
    const char *p = strstr(text, key);
    return p ? strtol(p + strlen(key), NULL, 10) : -1;
}

static void sample_residue(struct residue *o)
{
    char buf[8192], line[1024], path[64], link[256];
    FILE *f;
    size_t n;

    memset(o, 0, sizeof(*o));
    f = fopen("/proc/self/status", "r");
    n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
    if (f) fclose(f);
    buf[n] = 0;
    o->threads = status_kb(buf, "\nThreads:");
    o->rss_kb = status_kb(buf, "\nVmRSS:");
    o->anon_kb = status_kb(buf, "\nRssAnon:");
    o->file_kb = status_kb(buf, "\nRssFile:");
    f = fopen("/proc/self/maps", "r");
    while (f && fgets(line, sizeof(line), f)) {
        if (strstr(line, ".pvgns")) o->copy_maps++;
    }
    if (f) fclose(f);
    for (int fd = 0; fd < 4096; fd++) {
        ssize_t l;
        snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
        l = readlink(path, link, sizeof(link) - 1);
        if (l <= 0) continue;
        link[l] = 0;
        if (!strncmp(link, "/dev/dri/", 9) || !strncmp(link, "/dev/nvidia", 11)) o->gpu_fds++;
    }
}

static void print_residue(const char *when, int cycle, const struct residue *r)
{
    printf("INFO reload-residue %s cycle=%d threads=%ld rss_kb=%ld anon_kb=%ld file_kb=%ld "
           "copy_maps=%d gpu_fds=%d\n", when, cycle, r->threads, r->rss_kb, r->anon_kb,
           r->file_kb, r->copy_maps, r->gpu_fds);
    fflush(stdout);
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
 * overwrite the compiled-in build MARKER (QMETAL_BUILD_MARKER, CMake default
 * "20260402_1308_FORCE_REBUILD_V3", fixed length) with a cycle-distinct value.
 * The golden is the SAME libmlapi the plugin is built and booted against (the
 * production session ABI), so the fixture no longer pins a historical build's
 * git hash (it required ae78d0ef's "2994552d"; main's builds carry a 9-char
 * short hash): the git field is left as built and only the marker, which the
 * per-cycle NEW-banner check matches, is rewritten.  Each load of the result
 * is an independent co-resident instance whose live banner differs from every
 * other.  Returns a malloc'd path to the minted libmlapi.so, or NULL. */
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
    char marker[31];
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

    /* cycle-distinct, fixed-length marker; the golden must carry main's banner
     * format and the default build marker (else the mint is refused, loudly) */
    snprintf(marker, sizeof(marker), "RELOADCYCLE_%02d_INPROC_WARMSWAP", cycle); /* 30 chars */
    if (strlen(marker) != 30 ||
        !memmem(buf, n, "libmlapi built %s git=%s marker=%s", 34) ||
        mint_replace(buf, n, "20260402_1308_FORCE_REBUILD_V3", marker)) {
        fprintf(stderr, "mint: %s lacks the libmlapi banner format or the default build marker\n",
                golden);
        free(buf); return NULL;
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

static const char *inflight_phase = "?";
static void reload_inflight_watchdog(int sig)
{
    (void)sig;
    printf("FAIL in-flight reload: %s did not return within the watchdog (wait cycle)\n", inflight_phase);
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
    /* --reload-noapi LIB : a libmlapi WITHOUT the suspend/resume API (an older
     * build) for step 10e's rollback check. */
    const char *reload_noapi = NULL;
    for (int i = 2; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--reload-noapi")) reload_noapi = argv[i + 1];
    }
    /* --reload-unloadable LIB : a successor whose dlopen fails (undefined symbol,
     * plugins/uniq-vgpu/stub_unloadable.so) for step 10f (D-QDEV-RELOAD-DLOPEN-NO-ROLLBACK). */
    const char *reload_unloadable = NULL;
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--reload-unloadable")) reload_unloadable = argv[i + 1];
    }
    /* --reload-mapper refuse|handover : step 10e-m (D-MAPPER-RELOAD) with a live iosfc_mapper
     * mapping.  "refuse": the loaded library refuses will_suspend with live mappings (QMetal
     * before the mapper hand-over): the reload must be refused (-EBUSY) and the old Session keep
     * serving.  "handover": the library hands the mapper over: the reload succeeds and the new
     * Session serves the mapping. */
    const char *reload_mapper = NULL;
    for (int i = 2; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--reload-mapper")) reload_mapper = argv[i + 1];
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
        /* DISP-D1 knob (UVG_DISP_D1_DEST, default off): off = the pre-DISP-D1 scanout (no udmabuf over the memfd, so no
         * import, forget or reservation can follow); on = the udmabuf exists whenever /dev/udmabuf can be opened. */
        {
            const char *k = getenv("UVG_DISP_D1_DEST");
            int want = (k && strcmp(k, "1") == 0) ? 1 : 0;
            int udev = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
            int dest, dmabuf;

            if (udev >= 0) {
                close(udev);
            }
            pthread_mutex_lock(&host_lock);
            dest = d1_dest_seen;
            dmabuf = d1_dmabuf_seen;
            pthread_mutex_unlock(&host_lock);
            CHECK(dest == want && (want ? (udev >= 0 ? dmabuf >= 0 : dmabuf == -1) : dmabuf == -1),
                  "DISP-D1 knob UVG_DISP_D1_DEST=%s: plugin dest=%d dmabuf=%d (want dest=%d, %s)", k ? k : "unset",
                  dest, dmabuf, want, want ? (udev >= 0 ? "udmabuf present" : "no /dev/udmabuf -> -1") : "no udmabuf");
        }
    }

    /* 9b. UI size requests (qdp v3 ui_info, display-modes=follow): the mode
     * list becomes the one requested size (served by the EFI mode registers
     * too), a repeat is reported unchanged, out-of-bounds requests change
     * nothing, and the scanout keeps its size until the guest presents.
     * Bounds are the reference's (uniq-vgpu.c pvg_ui_info_refusal): 1..16384
     * per side (Virtualization.framework -[VZMacGraphicsDisplayConfiguration
     * validateWithError:] @0x7ffb165b9eb0) and no other minimum (host
     * testUpdateModeList u16 page fields,
     * _PGDisplayMode init without a size check; guest edidInit u16 (w-1),(h-1),
     * validateDetailedTiming active < 0x10000), at most 16777216 pixels (64 MiB
     * IOFBMemorySize), the guest edidInit pixel clock ((h+10000)(w+10000)*
     * refresh16 + 0x270fffff) / 0x27100000 <= 0x1000000, flags 0, and 60 Hz only
     * (design debt E1). The clock cases: 16384x1024 (the largest clock within
     * the side and pixel bounds) is 1745144 units at 60 Hz, 16777197 at
     * 576.819 Hz (in bounds, then refused as not 60 Hz) and 16777227 at
     * 576.820 Hz (above the clock limit). */
    {
        qdp_ui_info ui = { .size = sizeof(ui), .width = 1600, .height = 900 };
        static const struct { uint32_t w, h, mhz, flags; const char *what, *reason; } bad[] = {
            { 0, 900, 0, 0, "0x900", "refused: a side outside 1..16384" },
            { 1600, 0, 0, 0, "1600x0", "refused: a side outside 1..16384" },
            { 0x10000, 1000, 0, 0, "65536x1000", "refused: a side outside 1..16384" },
            { 1000, 0x10000, 0, 0, "1000x65536", "refused: a side outside 1..16384" },
            { 16385, 1024, 0, 0, "16385x1024 (VZ maximum 16384)", "refused: a side outside 1..16384" },
            { 1024, 16385, 0, 0, "1024x16385 (VZ maximum 16384)", "refused: a side outside 1..16384" },
            { 65535, 256, 0, 0, "65535x256 (PG u16, above VZ 16384)", "refused: a side outside 1..16384" },
            { 4097, 4096, 0, 0, "4097x4096 (16781312 pixels)", "refused: more than 16777216 pixels" },
            { 8192, 4096, 0, 0, "8192x4096", "refused: more than 16777216 pixels" },
            { 16384, 1024, 576820, 0, "16384x1024 @ 576.820 Hz (clock 16777227 > 0x1000000)",
              "refused: pixel clock above the guest EDID limit" },
            { 16384, 1024, 576819, 0, "16384x1024 @ 576.819 Hz (clock 16777197, in bounds)",
              "refused: only 60 Hz is offered" },
            { 1600, 900, 59940, 0, "1600x900 @ 59.94 Hz", "refused: only 60 Hz is offered" },
            { 1600, 900, 120000, 0, "1600x900 @ 120 Hz", "refused: only 60 Hz is offered" },
            { 1600, 900, 0, 1, "1600x900 flag 1", "refused: unknown flags" },
        };
        /* accepted: the VM118 window sizes, the 1x1 minimum, the former 800x600
         * policy's edges, the pixel bound and the largest 60 Hz clock */
        static const struct { uint32_t w, h; } good[] = {
            { 1630, 572 }, { 1324, 626 }, { 1, 1 }, { 799, 600 }, { 800, 599 },
            { 4096, 4096 }, { 16384, 1024 }, { 1024, 16384 },
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
        const int d0 = get_int(&display_republish_ok);
        r = ops->quiesce(priv);
        rr = ops->reset(priv, VFU_RESET_DEVICE);
        ops->resume(priv);
        /* -[PGDevice teardownDisplays] (host decompile :47793) ends the old boot's displays: the
         * mode target held on the QMetal renderer is returned before the PGDevice reset (a reset
         * that rebuilds a lost VkDevice refuses a remaining owned output lease) and the mode is
         * published again on the Session's renderer; the scanout keeps its geometry. */
        {
            qdp_scanout so;

            for (int i = 0; i < 200 && get_int(&display_republish_ok) == d0; i++) {
                usleep(10 * 1000);   /* the host-log writer thread is asynchronous */
            }
            CHECK(get_int(&display_republish_ok) == d0 + 1,
                  "device reset returned the display target and published the mode again (+%d)",
                  get_int(&display_republish_ok) - d0);
            CHECK(ops->get_scanout(priv, &so) && so.enabled && so.width == 1920 && so.height == 1080,
                  "scanout after the device reset: %ux%u enabled %d", so.width, so.height, so.enabled);
        }
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
            snprintf(root, sizeof(root), "/tmp/uniq-vgpu-reload-%d", getpid());
            mkdir(root, 0700);
            snprintf(sib, sizeof(sib), "%.*s", slash ? (int)(slash - reload_golden) : 1,
                     slash ? reload_golden : ".");

            printf("INFO reload: %d cycles, golden=%s sibdir=%s\n", reload_n, reload_golden, sib);
            banner_capture_start();   /* tee stderr to catch each build's live banner */
            struct residue res0, res_warm, res_last;
            sample_residue(&res0);
            print_residue("before", 0, &res0);
            res_warm = res0;
            res_last = res0;
            /* 10a. D-QDEV-RELOAD-ROLLBACK-EIO (apple-virgl analysis/20261127-FIX-CANDIDATE-D-QDEV-RELOAD-ROLLBACK-EIO.md):
             * the FIRST reload after boot is a transparent hand-over whose B refuses the state, so the
             * plugin must roll back to the BOOT instance (loaded by its SONAME at realize).  Live
             * (warm-4080, v66, 2026-11-27): "stamp unique SONAME on libmlapi.so: Input/output error",
             * rollback failed, the device died.  The backend's recorded path must be the base
             * instance's absolute file, the reload reports failure, and the boot instance serves on
             * from the same state (PROTOCOL_VERSION 6, FIFO_READ continues, new work consumed). */
            if (reload_noapi) {
                const uint64_t fifo_gpa = B_IOVA + 0x100000, root_gpa = B_IOVA + 0x0f0000;
                uint8_t *fifo = guest + B_FILE_OFF + 0x100000;
                const uint32_t half = 12 * 100;
                qdp_dma_region b = { B_IOVA, B_LEN, guest + B_FILE_OFF, memfd, B_FILE_OFF,
                                     PROT_READ | PROT_WRITE };
                char marker[64], before_path[512] = "", after_path[512] = "";
                char *noapi = mint_instance(reload_noapi, sib, 95, root, marker, sizeof(marker));
                ops->backend_report(priv, rep, sizeof(rep));
                { char *p = strstr(rep, "path="); if (p) sscanf(p, "%511s", before_path); }
                CHECK(strstr(rep, "reloads=0 ") != NULL && !strncmp(before_path, "path=/", 6),
                      "boot rollback: before the first reload the boot instance's path is an absolute file (%s)",
                      before_path);
                r = ops->quiesce(priv); ops->dma_register(priv, &b); ops->resume(priv);
                wr(PVG_REG_FIFO_ENABLE, 0);
                memset(guest + B_FILE_OFF + 0x0f0000, 0, 0x1000);
                wr(PVG_REG_PROTOCOL_VERSION, 6);
                wr(PVG_REG_SHARED_STATE_PFN, (uint32_t)(root_gpa >> 12));
                wr(PVG_REG_FIFO_PFN, (uint32_t)(fifo_gpa >> 12));
                wr(PVG_REG_FIFO_OFFSET, 0x1000);
                wr(PVG_REG_FIFO_SIZE, 0x10000);
                wr(PVG_REG_FIFO_ENABLE, 1);
                for (uint32_t i = 0; i < 101; i++) {
                    uint32_t nop[3] = { 0x03, 12, 0x5000 + i };
                    memcpy(fifo + 0x1000 + 12 * i, nop, sizeof(nop));
                }
                wr(PVG_REG_FIFO_WRITE, half);
                CHECK(wait_reg(PVG_REG_FIFO_READ, half, 5.0), "boot rollback: the boot instance consumed the ring (FIFO_READ %u)",
                      rd(PVG_REG_FIFO_READ));
                r = ops->quiesce(priv);
                int rr = noapi ? ops->reload(priv, noapi, 0) : 0;   /* transparent: suspend -> B refuses -> roll back */
                ops->resume(priv);
                CHECK(noapi && rr != 0, "boot rollback: B without the resume API refuses the boot instance's state (rc=%d)", rr);
                ops->backend_report(priv, rep, sizeof(rep));
                { char *p = strstr(rep, "path="); if (p) sscanf(p, "%511s", after_path); }
                CHECK(!strcmp(before_path, after_path) && strstr(rep, "reloads=0 ") != NULL,
                      "boot rollback: the boot instance is live again (%s, %s)", after_path,
                      strstr(rep, "reloads=0 ") ? "reloads=0" : "reload count moved");
                CHECK(rd(PVG_REG_PROTOCOL_VERSION) == 6 && rd(PVG_REG_FIFO_READ) == half,
                      "boot rollback: resumed from the same state (protocol %u, FIFO_READ %u)",
                      rd(PVG_REG_PROTOCOL_VERSION), rd(PVG_REG_FIFO_READ));
                wr(PVG_REG_FIFO_WRITE, half + 12);
                CHECK(wait_reg(PVG_REG_FIFO_READ, half + 12, 5.0), "boot rollback: the device serves new work (FIFO_READ %u)",
                      rd(PVG_REG_FIFO_READ));
                wr(PVG_REG_FIFO_ENABLE, 0);
                r = ops->quiesce(priv); ops->dma_unregister(priv, &b); ops->resume(priv);
                free(noapi);
            }
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
                sample_residue(&res_last);
                print_residue("after", cyc, &res_last);
                if (cyc == RESIDUE_WARM_CYCLES) res_warm = res_last;
            }
            printf("INFO reload: %d/%d cycles OK, 0 new QMU errors=%s\n", ok_cycles, reload_n,
                   get_int(&qmetal_errors) == errs_before_all ? "yes" : "NO");
            /* 10b'. A retired instance keeps nothing (D-RELOAD-RESIDUE): after the
             * warm-up cycles (first-use caches of the shared ICD/loader), further
             * reloads leave the thread count, the GPU device fds and the retired
             * copies' mappings flat and the resident memory within a fixed bound
             * per cycle.  Measured only on runs long enough to separate growth
             * from warm-up. */
            if (ok_cycles == reload_n && reload_n >= RESIDUE_MIN_CYCLES) {
                const int span = reload_n - RESIDUE_WARM_CYCLES;
                const long rss_per = (res_last.rss_kb - res_warm.rss_kb) / span;
                CHECK(res_last.threads <= res_warm.threads,
                      "reload residue: threads %ld after cycle %d -> %ld after cycle %d (no retired-instance thread)",
                      res_warm.threads, RESIDUE_WARM_CYCLES, res_last.threads, reload_n);
                CHECK(res_last.gpu_fds <= res_warm.gpu_fds,
                      "reload residue: GPU device fds %d -> %d (each retired VkInstance/VkDevice released)",
                      res_warm.gpu_fds, res_last.gpu_fds);
                CHECK(res_last.copy_maps <= res_warm.copy_maps,
                      "reload residue: retired unique-SONAME copy mappings %d -> %d (retired copies unmapped)",
                      res_warm.copy_maps, res_last.copy_maps);
                CHECK(rss_per <= RESIDUE_RSS_KB_PER_CYCLE,
                      "reload residue: RSS %ld -> %ld kB over %d cycles = %ld kB/cycle (bound %d; anon %ld -> %ld, file %ld -> %ld)",
                      res_warm.rss_kb, res_last.rss_kb, span, rss_per, RESIDUE_RSS_KB_PER_CYCLE,
                      res_warm.anon_kb, res_last.anon_kb, res_warm.file_kb, res_last.file_kb);
            }

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
                /* D-QDEV-RELOAD-ROLLBACK-EIO: a missing library reports its real cause (was EIO) */
                int rn = ops->reload(priv, "/nonexistent-uniq-vgpu-selftest/libmlapi.so", QDP_RELOAD_RESET);
                CHECK(rn == -ENOENT, "rollback: a missing library is reported as ENOENT, not EIO (rc=%d %s)", rn, strerror(-rn));
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

            /* 10d. RELOAD WITH ACCEPTED GUEST-MEMORY WORK IN FLIGHT -- the live hang shape
             * (apple-virgl evidence/runs/uiinfo-main-candidate-20261118T1813Z G7 and
             * reload-deadlock-20261118T2110Z): qdev-host serializes BAR0 MMIO and the control
             * socket on its main loop, so the guest's doorbell is ACCEPTED before the host
             * quiesces for `reload`, and QMetal's FIFO worker is still consuming the ring when
             * the quiesce takes the guest-memory table write side.  The reload drain
             * (qmu_session_quiesce_host_callback_producers: accepted-work barrier, FIFO/EXEC3/
             * display worker joins, qmu_vk_wait_all_retired) waits for that worker, whose next
             * lookup waits for the write side the SAME thread holds.  QMetal's contract for
             * that drain: "no main-AIO/BQL dependency locks may be held" (qmu_session.cpp).
             * The precondition is ASSERTED, not assumed: FIFO_READ < FIFO_WRITE after a 100 ms
             * hold under the quiesce (the worker is parked behind the write side).  Watchdog
             * = a wait cycle (SIGALRM -> FAIL + _exit). */
            {
                const uint64_t fifo_gpa = B_IOVA + 0x100000;
                uint8_t *fifo = guest + B_FILE_OFF + 0x100000;
                const uint32_t nops = 5000, wptr = 12 * 5000;   /* 60000 of the 64 KiB ring */
                qdp_dma_region b = { B_IOVA, B_LEN, guest + B_FILE_OFF, memfd, B_FILE_OFF,
                                     PROT_READ | PROT_WRITE };
                char marker[64];
                char *inst = mint_instance(reload_golden, sib, 98, root, marker, sizeof(marker));
                uint32_t fr = 0;
                int attempt, parked = 0;

                CHECK(inst != NULL, "in-flight reload: mint instance (marker=%s)", inst ? marker : "FAILED");
                r = ops->quiesce(priv); ops->dma_register(priv, &b); ops->resume(priv);
                signal(SIGALRM, reload_inflight_watchdog);
                for (attempt = 1; inst && attempt <= 5 && !parked; attempt++) {
                    /* the guest's boot handshake on the current Session, then a full ring */
                    wr(PVG_REG_FIFO_ENABLE, 0);
                    wr(PVG_REG_PROTOCOL_VERSION, 6);
                    wr(PVG_REG_SHARED_STATE_PFN, (uint32_t)(fifo_gpa >> 12));
                    wr(PVG_REG_FIFO_PFN, (uint32_t)(fifo_gpa >> 12));
                    wr(PVG_REG_FIFO_OFFSET, 0x1000);
                    wr(PVG_REG_FIFO_SIZE, 0x10000);
                    wr(PVG_REG_FIFO_ENABLE, 1);
                    for (uint32_t i = 0; i < nops; i++) {
                        uint32_t nop[3] = { 0x03, 12, 0x5000 + i };
                        memcpy(fifo + 0x1000 + 12 * i, nop, sizeof(nop));
                    }
                    memset(fifo, 0, 4);
                    wr(PVG_REG_FIFO_WRITE, wptr);          /* doorbell accepted ... */
                    r = ops->quiesce(priv);                /* ... then the host quiesces */
                    usleep(100 * 1000);
                    inflight_phase = "precondition FIFO_READ readback under the quiesce";
                    alarm(30);
                    fr = rd(PVG_REG_FIFO_READ);
                    alarm(0);
                    parked = r == 0 && fr < wptr;
                    if (!parked) {
                        ops->resume(priv);                 /* the worker beat the quiesce: retry */
                    }
                }
                CHECK(parked, "in-flight reload precondition: accepted FIFO work parked behind the "
                      "quiesce (FIFO_READ %u < FIFO_WRITE %u, attempt %d)", fr, wptr, attempt - 1);
                if (parked) {
                    double tr0 = now_s();
                    int errs_before = get_int(&qmetal_errors);

                    inflight_phase = "reload under the quiesce with accepted FIFO work in flight";
                    alarm(30);
                    int rr = ops->reload(priv, inst, QDP_RELOAD_RESET);
                    alarm(0);
                    ops->resume(priv);
                    CHECK(rr == 0, "in-flight reload: quiesce+reload+resume with accepted FIFO work "
                          "in flight returned rc=%d in %.0f ms (no wait cycle)", rr, (now_s() - tr0) * 1000.0);
                    usleep(50 * 1000);
                    char banner[256]; get_live_banner(banner, sizeof(banner));
                    CHECK(strstr(banner, marker) != NULL, "in-flight reload: NEW banner live: %s", banner);
                    /* the re-created Session serves the guest's next re-handshake */
                    uint32_t nop[3] = { 0x03, 12, 0x4298 };
                    wr(PVG_REG_PROTOCOL_VERSION, 6);
                    wr(PVG_REG_SHARED_STATE_PFN, (uint32_t)(fifo_gpa >> 12));
                    wr(PVG_REG_FIFO_PFN, (uint32_t)(fifo_gpa >> 12));
                    wr(PVG_REG_FIFO_OFFSET, 0x1000);
                    wr(PVG_REG_FIFO_SIZE, 0x10000);
                    wr(PVG_REG_FIFO_ENABLE, 1);
                    memcpy(fifo + 0x1000, nop, sizeof(nop));
                    wr(PVG_REG_FIFO_WRITE, 12);
                    CHECK(wait_reg(PVG_REG_FIFO_READ, 12, 5.0),
                          "in-flight reload: re-created session consumes FIFO NOP (FIFO_READ %u)",
                          rd(PVG_REG_FIFO_READ));
                    CHECK(get_int(&qmetal_errors) == errs_before,
                          "in-flight reload: 0 new QMU [ERROR] lines (%d)", get_int(&qmetal_errors) - errs_before);
                    wr(PVG_REG_FIFO_ENABLE, 0);
                } else {
                    ops->resume(priv);
                }
                r = ops->quiesce(priv); ops->dma_unregister(priv, &b); ops->resume(priv);
                free(inst);
            }
            /* 10e. TRANSPARENT RELOAD WITH STATE HAND-OVER (apple-virgl plan 20261118 H1): a
             * reload WITHOUT --reset under a booted guest suspends the old Session (reference
             * willSuspend/finishSuspend -> DSGP v2 blob) and resumes the new one from it
             * (willResumeWithSuspendState/didResume).  The guest does NOT re-handshake: the
             * new Session must already be past UNINITIALIZED (PROTOCOL_VERSION reads 6), its
             * FIFO_READ continues from the old Session's, the queued rest of the ring is
             * consumed exactly once, no handshake bump, 0 new QMU errors.  With
             * --reload-noapi, a B without the resume API refuses the state and the plugin
             * rolls back to A resumed from the same blob (the reload reports failure, the
             * guest keeps running). */
                    {
                const uint64_t fifo_gpa = B_IOVA + 0x100000;
                uint8_t *fifo = guest + B_FILE_OFF + 0x100000;
                const uint32_t nops = 200, half = 12 * 100, full = 12 * 200;
                qdp_dma_region b = { B_IOVA, B_LEN, guest + B_FILE_OFF, memfd, B_FILE_OFF,
                                     PROT_READ | PROT_WRITE };
                char marker[64];
                char *inst = mint_instance(reload_golden, sib, 97, root, marker, sizeof(marker));
                CHECK(inst != NULL, "hand-over reload: mint instance (marker=%s)", inst ? marker : "FAILED");
                r = ops->quiesce(priv); ops->dma_register(priv, &b); ops->resume(priv);
                if (inst) {
                    /* The root node is its own page (as a guest allocates it): the root FIFO's
                     * stamp memory lives at the ring base page (createRootFifo), so sharing
                     * that page would make stamp values read as task-root words. */
                    const uint64_t root_gpa = B_IOVA + 0x0f0000;
                    wr(PVG_REG_FIFO_ENABLE, 0);
                    memset(guest + B_FILE_OFF + 0x0f0000, 0, 0x1000);   /* no children, tasks, displays */
                    wr(PVG_REG_PROTOCOL_VERSION, 6);
                    wr(PVG_REG_SHARED_STATE_PFN, (uint32_t)(root_gpa >> 12));
                    wr(PVG_REG_FIFO_PFN, (uint32_t)(fifo_gpa >> 12));
                    wr(PVG_REG_FIFO_OFFSET, 0x1000);
                    wr(PVG_REG_FIFO_SIZE, 0x10000);
                    wr(PVG_REG_FIFO_ENABLE, 1);
                    for (uint32_t i = 0; i < nops; i++) {
                        uint32_t nop[3] = { 0x03, 12, 0x6000 + i };
                        memcpy(fifo + 0x1000 + 12 * i, nop, sizeof(nop));
                    }
                    wr(PVG_REG_FIFO_WRITE, half);
                    CHECK(wait_reg(PVG_REG_FIFO_READ, half, 5.0), "hand-over reload: A consumed the first half (FIFO_READ %u)",
                          rd(PVG_REG_FIFO_READ));
                    ops->backend_report(priv, rep, sizeof(rep));
                    CHECK(strstr(rep, " state_handover=1 ") != NULL, "hand-over reload: backend reports state_handover=1");
                    char hs_before[64] = "";
                    { char *p = strstr(rep, "handshake_gen="); if (p) sscanf(p, "%63s", hs_before); }
                    int errs_before = get_int(&qmetal_errors);
                    double t0r = now_s();
                    r = ops->quiesce(priv);
                    int rr = ops->reload(priv, inst, 0);     /* transparent: no --reset */
                    ops->resume(priv);
                    CHECK(r == 0 && rr == 0, "hand-over reload: quiesce+reload(no reset)+resume rc=%d/%d in %.0f ms",
                          r, rr, (now_s() - t0r) * 1000.0);
                    usleep(50 * 1000);
                    char banner[256]; get_live_banner(banner, sizeof(banner));
                    CHECK(strstr(banner, marker) != NULL, "hand-over reload: NEW banner live: %s", banner);
                    CHECK(rd(PVG_REG_PROTOCOL_VERSION) == 6, "hand-over reload: B is past UNINITIALIZED "
                          "without a re-handshake (PROTOCOL_VERSION %u)", rd(PVG_REG_PROTOCOL_VERSION));
                    CHECK(rd(PVG_REG_FIFO_READ) == half, "hand-over reload: B continues A's FIFO_READ (%u)",
                          rd(PVG_REG_FIFO_READ));
                    wr(PVG_REG_FIFO_WRITE, full);
                    CHECK(wait_reg(PVG_REG_FIFO_READ, full, 5.0), "hand-over reload: B consumes the queued rest "
                          "(FIFO_READ %u)", rd(PVG_REG_FIFO_READ));
                    {
                        /* D-RELOAD-MODELIST: the display client's list (step 9b accepted
                         * [2560x1440]) is client state (-[_PGDisplay setNub:] hands its
                         * modeList to every Nub, 7ffb1405b4ef); B serves it, not its
                         * configured three modes. */
                        uint32_t m0, m1;
                        wr(PVG_REG_IOSFC_CMD, 0);
                        m0 = rd(PVG_REG_IOSFC_DATA);
                        wr(PVG_REG_IOSFC_CMD, 1);
                        m1 = rd(PVG_REG_IOSFC_DATA);
                        CHECK(m0 == ((1440u << 16) | 2560u) && m1 == 0,
                              "hand-over reload: B serves the accepted ui_info mode list [2560x1440] "
                              "(IOSFC_DATA 0x%08x, then 0x%08x)", m0, m1);
                    }
                    ops->backend_report(priv, rep, sizeof(rep));
                    char hs_after[64] = "";
                    { char *p = strstr(rep, "handshake_gen="); if (p) sscanf(p, "%63s", hs_after); }
                    CHECK(!strcmp(hs_before, hs_after), "hand-over reload: no handshake bump (%s -> %s)", hs_before, hs_after);
                    CHECK(get_int(&qmetal_errors) == errs_before, "hand-over reload: 0 new QMU [ERROR] lines (%d)",
                          get_int(&qmetal_errors) - errs_before);

                    if (reload_noapi) {
                        char *noapi = mint_instance(reload_noapi, sib, 96, root, marker, sizeof(marker));
                        char before_path[512] = "";
                        ops->backend_report(priv, rep, sizeof(rep));
                        { char *p = strstr(rep, "path="); if (p) sscanf(p, "%511s", before_path); }
                        int errs_rb = get_int(&qmetal_errors);
                        r = ops->quiesce(priv);
                        rr = noapi ? ops->reload(priv, noapi, 0) : 0;
                        ops->resume(priv);
                        CHECK(noapi && rr != 0, "hand-over rollback: B without the resume API refuses the state (rc=%d)", rr);
                        ops->backend_report(priv, rep, sizeof(rep));
                        char after_path[512] = "";
                        { char *p = strstr(rep, "path="); if (p) sscanf(p, "%511s", after_path); }
                        CHECK(!strcmp(before_path, after_path), "hand-over rollback: A is live again (%s)", after_path);
                        CHECK(rd(PVG_REG_PROTOCOL_VERSION) == 6 && rd(PVG_REG_FIFO_READ) == full,
                              "hand-over rollback: A resumed from the same state (protocol %u, FIFO_READ %u)",
                              rd(PVG_REG_PROTOCOL_VERSION), rd(PVG_REG_FIFO_READ));
                        uint32_t nop[3] = { 0x03, 12, 0x6fff };
                        memcpy(fifo + 0x1000 + full, nop, sizeof(nop));
                        wr(PVG_REG_FIFO_WRITE, full + 12);
                        CHECK(wait_reg(PVG_REG_FIFO_READ, full + 12, 5.0), "hand-over rollback: A consumes new work "
                              "(FIFO_READ %u)", rd(PVG_REG_FIFO_READ));
                        printf("INFO hand-over rollback: QMU [ERROR] lines during the refused reload: %d (expected: the refusal)\n",
                               get_int(&qmetal_errors) - errs_rb);
                        free(noapi);
                    }
                    if (reload_unloadable) {
                        /* 10f. D-QDEV-RELOAD-DLOPEN-NO-ROLLBACK (apple-virgl evidence/runs/main-v79-20261128
                         * hpcaps): a transparent reload to a successor whose dlopen fails ("undefined
                         * symbol") must be refused BEFORE anything is torn down: the live Session keeps
                         * serving the guest, the refusal names the dlopen reason, and the next good
                         * transparent reload still finds a Session to hand over.  Before the fix the old
                         * Session was destroyed first, the dlopen failed, nothing rolled back (device
                         * dead, reply ENOENT) and a later reload re-created a Session without the
                         * guest's state (guest display dark). */
                        char before_path[512] = "", after_path[512] = "", m93[64];
                        long reloads_before = -1, reloads_after = -2;
                        ops->backend_report(priv, rep, sizeof(rep));
                        { char *p = strstr(rep, "path="); if (p) sscanf(p, "%511s", before_path); }
                        { char *p = strstr(rep, "reloads="); if (p) reloads_before = atol(p + 8); }
                        const uint32_t pos = rd(PVG_REG_FIFO_READ);
                        r = ops->quiesce(priv);
                        int ru = ops->reload(priv, reload_unloadable, 0);   /* transparent */
                        ops->resume(priv);
                        CHECK(ru == -ELIBACC, "unloadable successor: the reload is refused as a library load "
                              "failure (rc=%d %s)", ru, strerror(-ru));
                        ops->backend_report(priv, rep, sizeof(rep));
                        { char *p = strstr(rep, "path="); if (p) sscanf(p, "%511s", after_path); }
                        { char *p = strstr(rep, "reloads="); if (p) reloads_after = atol(p + 8); }
                        CHECK(!strcmp(before_path, after_path) && reloads_after == reloads_before,
                              "unloadable successor: the served library and reload count are unchanged (%s, reloads "
                              "%ld -> %ld)", after_path, reloads_before, reloads_after);
                        {
                            const char *e = strstr(rep, "last_reload_error='");
                            CHECK(e && strstr(e, "undefined symbol") && strstr(e, "uvg_selftest_unresolved_symbol"),
                                  "unloadable successor: the backend report names the dlopen reason (%.200s)",
                                  e ? e : "no last_reload_error");
                        }
                        CHECK(rd(PVG_REG_PROTOCOL_VERSION) == 6 && rd(PVG_REG_FIFO_READ) == pos,
                              "unloadable successor: the live Session is untouched (protocol %u, FIFO_READ %u, was %u)",
                              rd(PVG_REG_PROTOCOL_VERSION), rd(PVG_REG_FIFO_READ), pos);
                        uint32_t nop[3] = { 0x03, 12, 0x6ffe };
                        memcpy(fifo + 0x1000 + pos, nop, sizeof(nop));
                        wr(PVG_REG_FIFO_WRITE, pos + 12);
                        CHECK(wait_reg(PVG_REG_FIFO_READ, pos + 12, 5.0),
                              "unloadable successor: the live Session consumes new work (FIFO_READ %u)",
                              rd(PVG_REG_FIFO_READ));
                        char *good = mint_instance(reload_golden, sib, 93, root, m93, sizeof(m93));
                        r = ops->quiesce(priv);
                        int rg = good ? ops->reload(priv, good, 0) : -1;   /* transparent */
                        ops->resume(priv);
                        CHECK(rg == 0 && rd(PVG_REG_PROTOCOL_VERSION) == 6 && rd(PVG_REG_FIFO_READ) == pos + 12,
                              "unloadable successor: the next transparent reload hands the live state over (rc=%d, "
                              "protocol %u, FIFO_READ %u)", rg, rd(PVG_REG_PROTOCOL_VERSION), rd(PVG_REG_FIFO_READ));
                        free(good);
                    }
                    wr(PVG_REG_FIFO_ENABLE, 0);
                }
                r = ops->quiesce(priv); ops->dma_unregister(priv, &b); ops->resume(priv);
                free(inst);
            }
            /* 10e-m. D-MAPPER-RELOAD (apple-virgl analysis/20261123-FIX-CANDIDATE-D-MAPPER-RELOAD.md): a
             * transparent reload while the guest's IOSurface-mapper ring holds a live mapping (AppleGVA
             * video; live warm-hzmain main v40/v41: "QMU_SUSPEND phase=will_suspend refused: 2 live
             * iosfc_mapper mappings (D-MAPPER)" then "reload: guest GPU state lost").  Guest side as the
             * UniqVGPU kext programs it (reference IOSurfaceParavirtMapperDevice::start order: FEATURES,
             * RING_BASE, DESC_BASE, RING_WR=0, RING_RD=0, RING_SIZE) and a two-page first-form NV12
             * descriptor for id 1 (below 4 GiB: BAR0 takes 4-byte accesses, an 8-byte access is the low
             * dword then the high dword, and the mapper's 64-bit registers keep the low dword). */
            if (reload_mapper) {
                const bool want_handover = !strcmp(reload_mapper, "handover");
                const uint64_t mring = A_IOVA + 0x2000000, mtable = mring + 0x1000, mdpage = mring + 0x2000,
                               mroot = mring + 0x3000, msurf = mring + 0x10000;
                const uint64_t mdesc1 = mdpage + 0x200;           /* id 1: descriptor 1 of page 0 (first form) */
                const uint64_t fifo_gpa = B_IOVA + 0x100000, root_gpa = B_IOVA + 0x0f0000;
                uint8_t *fifo = guest + B_FILE_OFF + 0x100000;
                qdp_dma_region b = { B_IOVA, B_LEN, guest + B_FILE_OFF, memfd, B_FILE_OFF,
                                     PROT_READ | PROT_WRITE };
#define MPTE(gpa) ((uint32_t)(((gpa) >> 12) << 2) | 1u)
#define MPUT32(gpa, v) do { uint32_t v_ = (v); memcpy(guest + (gpa), &v_, 4); } while (0)
#define MGET32(gpa) ({ uint32_t v_; memcpy(&v_, guest + (gpa), 4); v_; })
                memset(guest + mring, 0, 0x20000);
                MPUT32(mtable, MPTE(mdpage));
                MPUT32(mdesc1 + 0x00, MPTE(mroot));
                MPUT32(mdesc1 + 0x04, 0x34323076u);               /* '420v' */
                MPUT32(mdesc1 + 0x10, 2 * 4096);
                MPUT32(mdesc1 + 0x14, (64u << 8) | 4);
                MPUT32(mdesc1 + 0x18, (64u << 8) | 5);
                MPUT32(mdesc1 + 0x1c, 64);
                guest[mdesc1 + 0x24] = 2;
                MPUT32(mdesc1 + 0x40 + 0x10, 64 * 64);
                MPUT32(mdesc1 + 0x40 + 0x1c, 64);
                MPUT32(mdesc1 + 0x80 + 0x08, 64 * 64);
                MPUT32(mdesc1 + 0x80 + 0x10, 64 * 32);
                MPUT32(mdesc1 + 0x80 + 0x1c, 64);
                MPUT32(mroot + 0, MPTE(msurf));
                MPUT32(mroot + 4, MPTE(msurf + 0x1000));
#define MCMD(slot, cmd, id) do { uint32_t c_[4] = { (cmd), (id), 0x5555u, 0 }; \
                    memcpy(guest + mring + 16u * (slot), c_, 16); wr(0x1248, (slot) + 1u); } while (0)
#define MSTATUS(slot) MGET32(mring + 16u * (slot) + 8u)
                char marker[64];
                char *inst = mint_instance(reload_golden, sib, 95, root, marker, sizeof(marker));
                CHECK(inst != NULL, "mapper reload: mint instance (marker=%s)", inst ? marker : "FAILED");
                r = ops->quiesce(priv); ops->dma_register(priv, &b); ops->resume(priv);
                if (inst) {
                    wr(PVG_REG_FIFO_ENABLE, 0);
                    memset(guest + B_FILE_OFF + 0x0f0000, 0, 0x1000);
                    wr(PVG_REG_PROTOCOL_VERSION, 6);
                    wr(PVG_REG_SHARED_STATE_PFN, (uint32_t)(root_gpa >> 12));
                    wr(PVG_REG_FIFO_PFN, (uint32_t)(fifo_gpa >> 12));
                    wr(PVG_REG_FIFO_OFFSET, 0x1000);
                    wr(PVG_REG_FIFO_SIZE, 0x10000);
                    wr(PVG_REG_FIFO_ENABLE, 1);
                    for (uint32_t i = 0; i < 2; i++) {
                        uint32_t nop[3] = { 0x03, 12, 0x7000 + i };
                        memcpy(fifo + 0x1000 + 12 * i, nop, sizeof(nop));
                    }
                    wr(PVG_REG_FIFO_WRITE, 12);
                    CHECK(wait_reg(PVG_REG_FIFO_READ, 12, 5.0), "mapper reload: root FIFO live (FIFO_READ %u)",
                          rd(PVG_REG_FIFO_READ));
                    (void)rd(0x1258);                                  /* FEATURES */
                    wr(0x1230, (uint32_t)mring);                       /* RING_BASE */
                    wr(0x1240, (uint32_t)mtable);                      /* DESC_BASE */
                    wr(0x1248, 0);                                     /* RING_WR */
                    wr(0x1250, 0);                                     /* RING_RD */
                    wr(0x1238, 256);                                   /* RING_SIZE */
                    MCMD(0, 1u, 1u);                                   /* MAP id 1 */
                    CHECK(MSTATUS(0) == 0 && (MGET32(mdesc1) & 2u) != 0,
                          "mapper reload: id 1 mapped before the reload (status 0x%x, word0 0x%x)",
                          MSTATUS(0), MGET32(mdesc1));
                    char before_path[512] = "";
                    ops->backend_report(priv, rep, sizeof(rep));
                    { char *p = strstr(rep, "path="); if (p) sscanf(p, "%511s", before_path); }
                    int errs_before = get_int(&qmetal_errors);
                    r = ops->quiesce(priv);
                    int rr = ops->reload(priv, inst, 0);
                    ops->resume(priv);
                    usleep(50 * 1000);
                    char after_path[512] = "";
                    ops->backend_report(priv, rep, sizeof(rep));
                    { char *p = strstr(rep, "path="); if (p) sscanf(p, "%511s", after_path); }
                    if (want_handover) {
                        char banner[256]; get_live_banner(banner, sizeof(banner));
                        CHECK(r == 0 && rr == 0, "mapper reload (handover): reload with a live mapping succeeds (rc=%d/%d)",
                              r, rr);
                        CHECK(strstr(banner, marker) != NULL, "mapper reload (handover): NEW banner live: %s", banner);
                        CHECK(get_int(&qmetal_errors) == errs_before, "mapper reload (handover): 0 new QMU [ERROR] lines (%d)",
                              get_int(&qmetal_errors) - errs_before);
                    } else {
                        CHECK(rr == -EBUSY, "mapper reload (refuse): the refused suspend refuses the reload with -EBUSY "
                              "(rc=%d)", rr);
                        CHECK(!strcmp(before_path, after_path), "mapper reload (refuse): the old Session stays loaded (%s)",
                              after_path);
                    }
                    CHECK(rd(PVG_REG_PROTOCOL_VERSION) == 6 && rd(PVG_REG_FIFO_READ) == 12,
                          "mapper reload: device state kept (protocol %u, FIFO_READ %u)",
                          rd(PVG_REG_PROTOCOL_VERSION), rd(PVG_REG_FIFO_READ));
                    wr(PVG_REG_FIFO_WRITE, 24);
                    CHECK(wait_reg(PVG_REG_FIFO_READ, 24, 5.0), "mapper reload: the root FIFO consumes new work "
                          "(FIFO_READ %u)", rd(PVG_REG_FIFO_READ));
                    MCMD(1, 2u, 1u);                                   /* UNMAP id 1 */
                    CHECK(MSTATUS(1) == 0 && (MGET32(mdesc1) & 2u) == 0,
                          "mapper reload: the Session after the reload still serves live id 1 (UNMAP status 0x%x, "
                          "word0 0x%x)", MSTATUS(1), MGET32(mdesc1));
                    MCMD(2, 1u, 1u);                                   /* MAP id 1 again */
                    CHECK(MSTATUS(2) == 0 && (MGET32(mdesc1) & 2u) != 0,
                          "mapper reload: the mapper ring keeps serving (MAP status 0x%x)", MSTATUS(2));
                    MCMD(3, 2u, 1u);                                   /* leave nothing mapped */
                    wr(PVG_REG_FIFO_ENABLE, 0);
                }
#undef MCMD
#undef MSTATUS
#undef MPTE
#undef MPUT32
#undef MGET32
                r = ops->quiesce(priv); ops->dma_unregister(priv, &b); ops->resume(priv);
                free(inst);
            }
            /* 10f. D-RELOAD-MODELIST: a transparent reload of a display that a ui_info
             * resized posts NO display transaction at resume.  Reference: the display
             * client keeps its modeList and hands it to every Nub (-[_PGDisplay setNub:]
             * 7ffb1405b4ef -> -[_PGDisplayNub setModeList:] 7ffb1403d0af), including the
             * restoring attach of -[_PGDevice didResume] (7ffb1403bad8); finishRestore's
             * state-machine pass (7ffb14040035, 7ffb1403d2cb) posts a transaction only when
             * testUpdateSettings: (7ffb1403f3e4) or testUpdateModeList: (7ffb1403f011)
             * finds a field of that client differing from the page.  The guest side here
             * is this test: the root node's display word (+0x900, as SET_SHARED_STATE
             * leaves it), the page's enable mask, and each ACK's page effect (+0xfa9 = 0,
             * +0xfc0 = 2, as the ch5 ACK pass leaves an acknowledged online) with the
             * pending flags consumed.  Red without the plugin's re-install: B2 compares
             * its configured three-mode list and default settings -> txn + 1, 0x0C. */
            {
                const uint64_t fifo_gpa = B_IOVA + 0x100000, root_gpa = B_IOVA + 0x0f0000,
                               page_gpa = B_IOVA + 0x0e0000;
                uint8_t *rootp = guest + B_FILE_OFF + 0x0f0000, *page = guest + B_FILE_OFF + 0x0e0000;
                qdp_dma_region b = { B_IOVA, B_LEN, guest + B_FILE_OFF, memfd, B_FILE_OFF,
                                     PROT_READ | PROT_WRITE };
                char m1[64], m2[64];
                char *b1 = mint_instance(reload_golden, sib, 95, root, m1, sizeof(m1));
                char *b2 = mint_instance(reload_golden, sib, 94, root, m2, sizeof(m2));
                CHECK(b1 && b2, "ui_info hand-over: mint instances (markers %s, %s)", b1 ? m1 : "FAILED",
                      b2 ? m2 : "FAILED");
                r = ops->quiesce(priv); ops->dma_register(priv, &b); ops->resume(priv);
                if (b1 && b2) {
                    const uint32_t page_pfn = (uint32_t)(page_gpa >> 12), enable = 0x0c, two = 2, zero = 0;
                    qdp_ui_info ui = { .size = sizeof(ui), .width = 1280, .height = 800 };
                    uint32_t txn0 = 0, txn1 = 0, txn2 = 0, st = 0, flags = 0, m0, mm1;
                    int rr, errs_before;
#define PAGE_ACK() do { page[0xfa9] = 0; memcpy(page + 0xfc0, &two, 4); memcpy(page + 0x100, &zero, 4); \
                        (void)rd(PVG_REG_DISPLAY_IRQ); } while (0)

                    wr(PVG_REG_FIFO_ENABLE, 0);
                    memset(rootp, 0, 0x1000);
                    memset(page, 0, 0x1000);
                    memcpy(rootp + 0x900, &page_pfn, 4);
                    memcpy(page + 0x104, &enable, 4);
                    wr(PVG_REG_PROTOCOL_VERSION, 6);
                    wr(PVG_REG_SHARED_STATE_PFN, (uint32_t)(root_gpa >> 12));
                    wr(PVG_REG_FIFO_PFN, (uint32_t)(fifo_gpa >> 12));
                    wr(PVG_REG_FIFO_OFFSET, 0x1000);
                    wr(PVG_REG_FIFO_SIZE, 0x10000);
                    wr(PVG_REG_FIFO_ENABLE, 1);
                    /* hand-over 1: B1's restoring attach brings the page's display online */
                    r = ops->quiesce(priv);
                    rr = ops->reload(priv, b1, 0);
                    ops->resume(priv);
                    memcpy(&txn0, page + 0x200, 4);
                    memcpy(&st, page + 0xfc0, 4);
                    CHECK(r == 0 && rr == 0 && st == 1 && page[0xfa9] == 1 && txn0 != 0,
                          "ui_info hand-over: B1 resumes the display page and posts its online "
                          "(rc=%d/%d, state %u, ack %u, txn %u)", r, rr, st, page[0xfa9], txn0);
                    PAGE_ACK();
                    r = ops->ui_info(priv, &ui);
                    memcpy(&txn1, page + 0x200, 4);
                    memcpy(&st, page + 0xfc0, 4);
                    memcpy(&flags, page + 0x100, 4);
                    CHECK(r == 0 && txn1 == txn0 + 1 && st == 1 && page[0xfa9] == 1 && (flags & 0x0c) == 0x0c,
                          "ui_info 1280x800 on the online display: %d, one transaction (txn %u -> %u, "
                          "state %u, flags 0x%x)", r, txn0, txn1, st, flags);
                    PAGE_ACK();
                    errs_before = get_int(&qmetal_errors);
                    /* D-R7-PRESENT, host side: the scanout keeps the guest's last frame across
                     * a hand-over (the display client survives didResume; finishRestore
                     * re-presents the page's surface, 7ffb14040035).  This page never presented
                     * (+0xf98 = 0), so no present follows here: the front buffer, its geometry
                     * and the frame count must stay as they were (red: the reload published a
                     * black buffer at the configured mode).  The test writes a marker into the
                     * front buffer as the stand-in for that last frame. */
                    qdp_scanout so0, so1;
                    int fc0;
                    bool marker_ok = false;
                    uint8_t *fmap = MAP_FAILED;
                    size_t fmap_len = 0;
                    CHECK(ops->get_scanout(priv, &so0) && so0.enabled, "ui_info hand-over: scanout %ux%u offset %u "
                          "before the reload", so0.width, so0.height, so0.offset);
                    {
                        struct stat fst;
                        if (fstat(so0.fd, &fst) == 0) {
                            fmap_len = (size_t)fst.st_size;
                            fmap = mmap(NULL, fmap_len, PROT_READ | PROT_WRITE, MAP_SHARED, so0.fd, 0);
                        }
                        if (fmap != MAP_FAILED) {
                            for (size_t i = 0; i < (size_t)so0.stride * so0.height; i += 4096) {
                                fmap[so0.offset + i] = 0x5a;
                            }
                        }
                    }
                    fc0 = get_int(&frame_calls);
                    /* hand-over 2: the client's list is unchanged across the reload */
                    r = ops->quiesce(priv);
                    rr = ops->reload(priv, b2, 0);
                    ops->resume(priv);
                    usleep(50 * 1000);
                    CHECK(ops->get_scanout(priv, &so1) && so1.enabled && so1.width == so0.width &&
                          so1.height == so0.height && so1.offset == so0.offset && get_int(&frame_calls) == fc0,
                          "ui_info hand-over: the scanout keeps its front buffer (%ux%u offset %u -> %ux%u offset %u, "
                          "frame_ready %d -> %d)", so0.width, so0.height, so0.offset, so1.width, so1.height,
                          so1.offset, fc0, get_int(&frame_calls));
                    if (fmap != MAP_FAILED) {
                        marker_ok = true;
                        for (size_t i = 0; i < (size_t)so1.stride * so1.height; i += 4096) {
                            marker_ok = marker_ok && fmap[so1.offset + i] == 0x5a;
                        }
                        CHECK(marker_ok, "ui_info hand-over: the front buffer keeps the last frame (marker intact)");
                        munmap(fmap, fmap_len);
                    } else {
                        printf("INFO ui_info hand-over: scanout memfd not writable here, marker check skipped\n");
                    }
                    {
                        char banner[256];
                        get_live_banner(banner, sizeof(banner));
                        CHECK(r == 0 && rr == 0 && strstr(banner, m2) != NULL,
                              "ui_info hand-over: reload to B2 rc=%d/%d, NEW banner live: %s", r, rr, banner);
                    }
                    memcpy(&txn2, page + 0x200, 4);
                    memcpy(&st, page + 0xfc0, 4);
                    memcpy(&flags, page + 0x100, 4);
                    CHECK(txn2 == txn1 && st == 2 && page[0xfa9] == 0 && (flags & 0x0c) == 0 &&
                          rd(PVG_REG_DISPLAY_IRQ) == 0,
                          "ui_info hand-over: NO display transaction at resume (txn %u -> %u, state %u, "
                          "ack %u, flags 0x%x)", txn1, txn2, st, page[0xfa9], flags);
                    wr(PVG_REG_IOSFC_CMD, 0);
                    m0 = rd(PVG_REG_IOSFC_DATA);
                    wr(PVG_REG_IOSFC_CMD, 1);
                    mm1 = rd(PVG_REG_IOSFC_DATA);
                    CHECK(m0 == ((800u << 16) | 1280u) && mm1 == 0,
                          "ui_info hand-over: B2 serves [1280x800] (IOSFC_DATA 0x%08x, then 0x%08x)", m0, mm1);
                    CHECK(get_int(&qmetal_errors) == errs_before, "ui_info hand-over: 0 new QMU [ERROR] lines (%d)",
                          get_int(&qmetal_errors) - errs_before);
#undef PAGE_ACK
                    wr(PVG_REG_FIFO_ENABLE, 0);
                }
                r = ops->quiesce(priv); ops->dma_unregister(priv, &b); ops->resume(priv);
                free(b1);
                free(b2);
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
