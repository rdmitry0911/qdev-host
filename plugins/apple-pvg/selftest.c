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
 *   - guest RAM on a hugetlb memfd (memory-backend-memfd hugetlb=on): a
 *     region at file offset 1 MiB with an unaligned end carries a FIFO
 *     round trip, and an unregistered region leaves no mapping behind. On a
 *     real hugetlb memfd when 2 MiB pages are reserved (vm.nr_hugepages >= 4;
 *     otherwise SKIP, or FAIL with SELFTEST_REQUIRE_HUGETLB=1), and always on
 *     a TEST DOUBLE that applies the hugetlbfs mmap/munmap rules to a 4 KiB
 *     memfd;
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
#include <sys/vfs.h>
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
/* hugetlb guest RAM: 8 MiB in 2 MiB pages; E as QEMU's q35 RAM after the PAM
 * hole (file offset 1 MiB) with an end inside a huge page; F only inside the
 * huge page [2 MiB, 4 MiB) */
#ifndef HUGETLBFS_MAGIC
#define HUGETLBFS_MAGIC 0x958458f6
#endif
#ifndef MFD_HUGETLB
#define MFD_HUGETLB 0x0004U
#endif
#define HUGE_PAGE (2 * MiB)
#define HUGE_FILE_SIZE (8 * MiB)
#define E_IOVA 0x300000000ull
#define E_FILE_OFF (1 * MiB)
#define E_LEN (0x580000ull)           /* ends at file offset 6.5 MiB */
#define E_FIFO_OFF (0x180000ull)      /* FIFO at file offset 2.5 MiB */
#define F_IOVA_DELTA 0x80000000ull   /* F after E, in its own GPA range */
#define M_IOVA 0x400000000ull          /* the same layout on the hugetlbfs double */
#define F_FILE_OFF (3 * MiB)
#define F_LEN (0x80000ull)

static long read_long_file(const char *path)
{
    FILE *f = fopen(path, "r");
    long v = -1;

    if (f) {
        if (fscanf(f, "%ld", &v) != 1) {
            v = -1;
        }
        fclose(f);
    }
    return v;
}

/* ------------------------------------- hugetlbfs boundary double (TEST DOUBLE)
 * A plain 4 KiB memfd presented to the plugin as a hugetlbfs file with 2 MiB
 * pages, for hosts that cannot reserve huge pages (a container, a fragmented
 * host): fstatfs() of it reports HUGETLBFS_MAGIC and f_bsize 2 MiB, and
 * mmap()/munmap() of it apply the rules of mmap(2) "Huge page (Huge TLB)
 * mappings": the offset must be a multiple of the huge page (else EINVAL), the
 * length is rounded up to it and the address is huge-page aligned; munmap()
 * needs an address and a length that are multiples of the huge page. Calls
 * on any other fd go to libc. Scope: whether the plugin keeps that contract;
 * this is no evidence of kernel behaviour (the real-hugetlb case is). The
 * selftest is linked -rdynamic, so these definitions also serve the plugin. */
static struct {
    pthread_mutex_t lock;
    int on, nmaps, mmap_einval, munmap_einval;
    dev_t dev;
    ino_t ino;
    struct { uintptr_t addr; size_t len; } maps[64];
} mock = { .lock = PTHREAD_MUTEX_INITIALIZER };

static void *(*libc_mmap)(void *, size_t, int, int, int, off_t);
static int (*libc_munmap)(void *, size_t);
static int (*libc_fstatfs)(int, struct statfs *);

static void mock_resolve(void)
{
    if (!libc_mmap) {
        *(void **)&libc_munmap = dlsym(RTLD_NEXT, "munmap");
        *(void **)&libc_fstatfs = dlsym(RTLD_NEXT, "fstatfs");
        *(void **)&libc_mmap = dlsym(RTLD_NEXT, "mmap");
    }
}

static bool mock_fd(int fd)
{
    struct stat st;

    return __atomic_load_n(&mock.on, __ATOMIC_ACQUIRE) && fd >= 0 && fstat(fd, &st) == 0 &&
           st.st_dev == mock.dev && st.st_ino == mock.ino;
}

static int mock_hugetlb_attach(int fd)
{
    struct stat st;

    mock_resolve();
    if (fd < 0 || fstat(fd, &st) != 0) {
        return -1;
    }
    mock.dev = st.st_dev;
    mock.ino = st.st_ino;
    __atomic_store_n(&mock.on, 1, __ATOMIC_RELEASE);
    return 0;
}

static int mock_mappings(void)
{
    int n;

    pthread_mutex_lock(&mock.lock);
    n = mock.nmaps;
    pthread_mutex_unlock(&mock.lock);
    return n;
}

int fstatfs(int fd, struct statfs *buf)
{
    int r;

    mock_resolve();
    r = libc_fstatfs(fd, buf);
    if (r == 0 && mock_fd(fd)) {
        buf->f_type = HUGETLBFS_MAGIC;
        buf->f_bsize = HUGE_PAGE;
    }
    return r;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    uint8_t *res, *at;
    size_t want;

    mock_resolve();
    if (!mock_fd(fd)) {
        return libc_mmap(addr, len, prot, flags, fd, off);
    }
    if ((off & (HUGE_PAGE - 1)) || !len ||
        ((flags & MAP_FIXED) && ((uintptr_t)addr & (HUGE_PAGE - 1)))) {
        pthread_mutex_lock(&mock.lock);
        mock.mmap_einval++;
        pthread_mutex_unlock(&mock.lock);
        errno = EINVAL;
        return MAP_FAILED;
    }
    want = (len + HUGE_PAGE - 1) & ~(size_t)(HUGE_PAGE - 1);
    at = addr;
    if (!(flags & MAP_FIXED)) {
        /* a huge-page-aligned address, as hugetlb_get_unmapped_area gives */
        res = libc_mmap(NULL, want + HUGE_PAGE, PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (res == MAP_FAILED) {
            return MAP_FAILED;
        }
        at = (uint8_t *)(((uintptr_t)res + HUGE_PAGE - 1) & ~(uintptr_t)(HUGE_PAGE - 1));
        if (at > res) {
            libc_munmap(res, at - res);
        }
        if (res + want + HUGE_PAGE > at + want) {
            libc_munmap(at + want, res + want + HUGE_PAGE - (at + want));
        }
        flags |= MAP_FIXED;
    }
    res = libc_mmap(at, want, prot, flags, fd, off);
    if (res != MAP_FAILED) {
        pthread_mutex_lock(&mock.lock);
        if (mock.nmaps < 64) {
            mock.maps[mock.nmaps].addr = (uintptr_t)res;
            mock.maps[mock.nmaps].len = want;
            mock.nmaps++;
        }
        pthread_mutex_unlock(&mock.lock);
    }
    return res;
}

int munmap(void *addr, size_t len)
{
    uintptr_t a = (uintptr_t)addr;

    mock_resolve();
    pthread_mutex_lock(&mock.lock);
    for (int i = 0; i < mock.nmaps; i++) {
        uintptr_t b = mock.maps[i].addr;

        if (a < b + mock.maps[i].len && b < a + len) {
            if ((a | len) & (HUGE_PAGE - 1)) {
                mock.munmap_einval++;
                pthread_mutex_unlock(&mock.lock);
                errno = EINVAL;
                return -1;
            }
            if (a <= b && a + len >= b + mock.maps[i].len) {
                mock.maps[i--] = mock.maps[--mock.nmaps];
            }
        }
    }
    pthread_mutex_unlock(&mock.lock);
    return libc_munmap(addr, len);
}

/* mappings of the real hugetlb memfd in this process (the plugin runs here) */
static int real_hugetlb_mappings(void)
{
    char line[512];
    FILE *f = fopen("/proc/self/maps", "r");
    int n = 0;

    while (f && fgets(line, sizeof(line), f)) {
        n += strstr(line, "/memfd:selftest-guest-ram-hugetlb") != NULL;
    }
    if (f) {
        fclose(f);
    }
    return n;
}

/* Region E at file offset 1 MiB (QEMU's q35 RAM after the PAM hole) with an
 * end inside a huge page carries a FIFO round trip at a file offset that is
 * no huge-page multiple; region F, never pinned by QMetal, is registered and
 * unregistered and must leave no mapping behind (munmap of whole huge pages). */
static void hugetlb_case(const qdp_device_ops *ops, void *priv, int hfd, uint8_t *huge,
                         uint64_t iova, const char *what, uint32_t stamp_value)
{
    const bool real = strcmp(what, "real hugetlb") == 0;
    int (*mappings)(void) = real ? real_hugetlb_mappings : mock_mappings;
    qdp_dma_region e = { iova, E_LEN, huge + E_FILE_OFF, hfd, E_FILE_OFF, PROT_READ | PROT_WRITE };
    qdp_dma_region f = { iova + F_IOVA_DELTA, F_LEN, huge + F_FILE_OFF, hfd, F_FILE_OFF,
                         PROT_READ | PROT_WRITE };
    const uint64_t fifo_gpa = iova + (E_FIFO_OFF - E_FILE_OFF);
    uint8_t *fifo = huge + E_FIFO_OFF;
    uint32_t nop[3] = { 0x03, 12, stamp_value }, stamp = 0;
    int before = get_int(&refusals), maps0, maps1, maps2, delivered0, r;
    struct stat st;

    memset(huge, 0, HUGE_FILE_SIZE);
    if (real) {
        CHECK(fstat(hfd, &st) == 0 && st.st_blksize == (blksize_t)HUGE_PAGE,
              "%s: memfd %llu MiB, st_blksize 0x%lx", what, HUGE_FILE_SIZE / MiB,
              (unsigned long)st.st_blksize);
    }
    maps0 = mappings();
    ops->dma_register(priv, &e);
    CHECK(wait_refusals(before + 1) == before && mappings() == maps0 + 1,
          "%s: region iova 0x%llx len 0x%llx at file offset 0x%llx mapped by the plugin "
          "(%d -> %d mappings, %d refusals)", what, (unsigned long long)e.iova,
          (unsigned long long)E_LEN, (unsigned long long)E_FILE_OFF, maps0, mappings(),
          get_int(&refusals) - before);

    wr(PVG_REG_SHARED_STATE_PFN, (uint32_t)((iova + 0x40000) >> 12));
    CHECK(wait_refusals(before + 1) == before, "%s: map_gpa inside the first huge page", what);

    /* QMetal reads the NOP and writes the stamp through the plugin's mapping */
    msi_enabled = 1;
    memcpy(fifo + 0x1000, nop, sizeof(nop));
    memset(fifo, 0, 4);
    wr(PVG_REG_FIFO_PFN, (uint32_t)(fifo_gpa >> 12));
    wr(PVG_REG_FIFO_OFFSET, 0x1000);
    wr(PVG_REG_FIFO_SIZE, 0x10000);
    wr(PVG_REG_FIFO_ENABLE, 1);
    delivered0 = get_int(&irq_delivered);
    wr(PVG_REG_FIFO_WRITE, 12);
    CHECK(wait_reg(PVG_REG_FIFO_READ, 12, 5.0), "%s: FIFO NOP consumed: FIFO_READ %u", what,
          rd(PVG_REG_FIFO_READ));
    for (int i = 0; i < 5000 && get_int(&irq_delivered) == delivered0; i++) {
        usleep(1000);
    }
    memcpy(&stamp, fifo, 4);
    CHECK(stamp == stamp_value && get_int(&irq_delivered) > delivered0,
          "%s: stamp 0x%x at file offset 0x%llx (GPA 0x%llx), MSI delivered", what, stamp,
          (unsigned long long)E_FIFO_OFF, (unsigned long long)fifo_gpa);
    (void)rd(PVG_REG_EVENT_STAMPS);
    wr(PVG_REG_FIFO_ENABLE, 0);
    CHECK(wait_refusals(before + 1) == before, "%s: no refusal during the FIFO round trip (%d)",
          what, get_int(&refusals) - before);

    ops->dma_register(priv, &f);
    maps1 = mappings();
    r = ops->quiesce(priv);
    ops->dma_unregister(priv, &f);
    ops->resume(priv);
    maps2 = mappings();
    CHECK(r == 0 && wait_refusals(before + 1) == before && maps1 == maps0 + 2 && maps2 == maps1 - 1,
          "%s: region at file offset 0x%llx, 0x%llx bytes, registered and unregistered: "
          "%d -> %d mappings", what, (unsigned long long)F_FILE_OFF, (unsigned long long)F_LEN,
          maps1, maps2);
    wr(PVG_REG_SHARED_STATE_PFN, 0x100);
}

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
    CHECK(ops && ops->abi_version == 3 && !strcmp(ops->name, "apple-pvg"),
          "qdp_plugin_v1: abi %u name %s", ops ? ops->abi_version : 0, ops ? ops->name : "-");
    if (!ops) {
        return 1;
    }
    CHECK((ops->flags & QDP_F_PLUGIN_DAMAGE) && ops->quiesce && ops->resume &&
          ops->dma_register && ops->dma_unregister && ops->get_scanout && ops->get_cursor &&
          ops->reset && ops->teardown && ops->ui_info,
          "v2 ops and the v3 ui_info present, QDP_F_PLUGIN_DAMAGE set");

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

    /* 8b. guest RAM on a hugetlb memfd: a real one when 2 MiB pages are
     * reserved, and always the hugetlbfs boundary double */
    {
        const char *require = getenv("SELFTEST_REQUIRE_HUGETLB");
        long reserved = read_long_file("/proc/sys/vm/nr_hugepages");
        long free_pages = read_long_file("/sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages");
        int hfd = memfd_create("selftest-guest-ram-hugetlb",
                               MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_HUGETLB);
        uint8_t *huge = MAP_FAILED;

        if (hfd >= 0 && ftruncate(hfd, HUGE_FILE_SIZE) == 0) {
            fcntl(hfd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL);
            huge = mmap(NULL, HUGE_FILE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, hfd, 0);
        }
        if (huge == MAP_FAILED) {
            printf("%s hugetlb guest RAM (real kernel): no 2 MiB hugetlb memfd of %llu MiB (%s; "
                   "vm.nr_hugepages=%ld free=%ld)\n", require && *require == '1' ? "FAIL" : "SKIP",
                   HUGE_FILE_SIZE / MiB, strerror(errno), reserved, free_pages);
            if (require && *require == '1') {
                failures++;
                checks++;
            }
        } else {
            hugetlb_case(ops, priv, hfd, huge, E_IOVA, "real hugetlb", 0x2234);
        }

        hfd = memfd_create("selftest-guest-ram-mock-hugetlb", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        r = ftruncate(hfd, HUGE_FILE_SIZE);
        fcntl(hfd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL);
        CHECK(hfd >= 0 && r == 0 && mock_hugetlb_attach(hfd) == 0,
              "TEST DOUBLE: 4 KiB memfd presented as a hugetlbfs file with 2 MiB pages");
        huge = mmap(NULL, HUGE_FILE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, hfd, 0);
        if (huge != MAP_FAILED) {
            hugetlb_case(ops, priv, hfd, huge, M_IOVA, "hugetlb double", 0x3234);
        }
        CHECK(mock.mmap_einval == 0 && mock.munmap_einval == 0,
              "hugetlb double: the plugin made %d mmap and %d munmap calls the hugetlb rules refuse",
              mock.mmap_einval, mock.munmap_einval);
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
     * nothing, and the scanout keeps its size until the guest presents. */
    {
        qdp_ui_info ui = { .size = sizeof(ui), .width = 1600, .height = 900 };
        static const struct { uint32_t w, h, mhz, flags; const char *what; } bad[] = {
            { 799, 600, 0, 0, "799x600" }, { 800, 599, 0, 0, "800x599" },
            { 0x10000, 1000, 0, 0, "65536x1000" }, { 8192, 4096, 0, 0, "8192x4096" },
            { 1600, 900, 59940, 0, "59.94 Hz" }, { 1600, 900, 0, 1, "flag 1" },
        };
        int errors0 = get_int(&plugin_errors), refused = 0;
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
            int rb = ops->ui_info(priv, &b);

            refused += rb == -EINVAL;
            if (rb != -EINVAL) {
                printf("INFO ui_info %s returned %d\n", bad[i].what, rb);
            }
        }
        wr(PVG_REG_IOSFC_CMD, 0);
        m0 = rd(PVG_REG_IOSFC_DATA);
        CHECK(refused == (int)(sizeof(bad) / sizeof(bad[0])) && m0 == ((900u << 16) | 1600u),
              "%d of %zu out-of-bounds requests refused with -EINVAL (min 800x600, 16777216 "
              "pixels, 60 Hz, sides <= 65535, flags 0); list unchanged (0x%08x)", refused,
              sizeof(bad) / sizeof(bad[0]), m0);
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
