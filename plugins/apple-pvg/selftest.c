/*
 * Self-test of the apple-pvg plugin without a VM.
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
 * Usage: selftest PLUGIN.so [--personality DEVICE_ID]
 * With --personality, only the PCI identity is checked, then the client
 * connection is dropped (VFU_RESET_LOST_CONN): the plugin must run its quit
 * stop phase and return QDP_RESET_EXIT (exit-on-disconnect); the host exits 0.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
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
static char last_refusal[4096];
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

static int get_int(int *v)
{
    int r;

    pthread_mutex_lock(&host_lock);
    r = *v;
    pthread_mutex_unlock(&host_lock);
    return r;
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

#define MiB (1024ull * 1024)
#define GUEST_FILE_SIZE (64 * MiB)
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
    const char *plugin = argc > 1 ? argv[1] : "plugins/apple-pvg/apple-pvg.so";
    unsigned personality = 0;
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
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* 1. plugin ABI */
    h = dlopen(plugin, RTLD_NOW | RTLD_LOCAL);
    CHECK(h != NULL, "dlopen %s%s%s", plugin, h ? "" : ": ", h ? "" : dlerror());
    if (!h) {
        return 1;
    }
    fn = (qdp_plugin_v1_fn)dlsym(h, QDP_PLUGIN_SYMBOL);
    ops = fn ? fn() : NULL;
    CHECK(ops && ops->abi_version == 2 && !strcmp(ops->name, "apple-pvg"),
          "qdp_plugin_v1: abi %u name %s", ops ? ops->abi_version : 0, ops ? ops->name : "-");
    if (!ops) {
        return 1;
    }
    CHECK((ops->flags & QDP_F_PLUGIN_DAMAGE) && ops->quiesce && ops->resume &&
          ops->dma_register && ops->dma_unregister && ops->get_scanout && ops->get_cursor &&
          ops->reset && ops->teardown, "v2 ops present, QDP_F_PLUGIN_DAMAGE set");

    /* 2. setup on a real vfu context */
    snprintf(sock, sizeof(sock), "/tmp/apple-pvg-selftest-%d.sock", getpid());
    snprintf(cache, sizeof(cache), "/tmp/apple-pvg-selftest-%d.cache", getpid());
    mkdir(cache, 0700);
    ctx = vfu_create_ctx(VFU_TRANS_SOCK, sock, LIBVFIO_USER_FLAG_ATTACH_NB, NULL,
                         VFU_DEV_TYPE_PCI);
    CHECK(ctx != NULL, "vfu_create_ctx %s", sock);
    if (!ctx) {
        return 1;
    }
    if (personality) {
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
        unsigned expect_did = personality ? personality : 0xeeee;

        CHECK(vid == 0x106b && did == expect_did, "IDs %04x:%04x (want 106b:%04x)", vid, did,
              expect_did);
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
        r = ops->reset(priv, VFU_RESET_LOST_CONN);
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

    /* 10. reset of the device keeps the device (fork semantics) */
    r = ops->reset(priv, VFU_RESET_DEVICE);
    CHECK(r == 0 && rd(PVG_REG_DISPLAY_PORT_COUNT) == 1, "VFU_RESET_DEVICE: %d, registers live", r);

    CHECK(get_int(&plugin_errors) == 0, "no plugin [ERROR] lines (%d host-log lines seen)",
          get_int(&log_lines));
    printf("INFO QMetal [ERROR] log lines: %d\n", get_int(&qmetal_errors));

    /* 11. quit stop phase */
    t0 = now_s();
    ops->teardown(priv);
    printf("INFO teardown (quit stop phase) %.2f s\n", now_s() - t0);
    CHECK(rd(PVG_REG_DISPLAY_PORT_COUNT) == 0xffffffff, "MMIO after the stop phase reads all-ones");
    printf("SUMMARY %d checks, %d failures\n", checks, failures);
    unlink(sock);
    return failures ? 1 : 0;
}
