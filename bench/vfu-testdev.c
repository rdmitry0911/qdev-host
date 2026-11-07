#define _GNU_SOURCE
/*
 * vfu-testdev: a libvfio-user server emulating QEMU's pci-testdev protocol
 * (hw/misc/pci-testdev.c) so that kvm-unit-tests x86/vmexit.flat can drive it
 * through stock QEMU's vfio-user-pci. Measures the message path cost.
 *
 * BAR0: 4 KiB MMIO, message based (REGION_READ/WRITE)
 * BAR1: 256 B  PIO,  message based
 * BAR2: 4 KiB MMIO, optional (-m): mmap-able (sparse mmap fd), i.e. no exits
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <err.h>
#include <sys/mman.h>
#include <time.h>
#include "libvfio-user.h"

#define PCI_VENDOR_ID_REDHAT      0x1b36
#define PCI_DEVICE_ID_REDHAT_TEST 0x0005
#define IOTEST_MEMSIZE 2048
#define IOTEST_IOSIZE  128

typedef struct __attribute__((packed)) {
    uint8_t  test;
    uint8_t  width;
    uint8_t  pad0[2];
    uint32_t offset;
    uint8_t  data;
    uint8_t  pad1[3];
    uint32_t count;
    char     name[32];
} hdr_t;

static int ntests = 1;            /* tests per type (mem / io) */
static hdr_t tests[2][3];         /* [type][idx] */
static int current = -1;          /* type*3 + idx, -1 = none */
static int verbose;
static int polling;
static int dma_done;
static vfu_ctx_t *g_ctx;

static double now_us(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }

/* DMA benchmark: read guest RAM at IOVA 1 MiB through libvfio-user.
 * If the client passed an fd (shared guest RAM), the region is mmapped and
 * vfu_sgl_read is a memcpy; otherwise it is a VFIO_USER_DMA_READ round trip. */
static void dma_bench(void)
{
    static const size_t sizes[] = { 64, 4096, 65536, 1048576 };
    char *buf = malloc(1048576);
    for (unsigned k = 0; k < 4; k++) {
        size_t sz = sizes[k];
        int iters = sz >= 1048576 ? 20 : 100;
        dma_sg_t *sg = malloc(dma_sg_size() * 8);
        struct iovec iov[8];
        int n = vfu_addr_to_sgl(g_ctx, (vfu_dma_addr_t)0x100000, sz, sg, 8, PROT_READ);
        if (n <= 0) { fprintf(stderr, "dma: addr_to_sgl failed n=%d errno=%d\n", n, errno); free(sg); continue; }
        int mapped = (vfu_sgl_get(g_ctx, sg, iov, n, 0) == 0);
        if (mapped) vfu_sgl_put(g_ctx, sg, iov, n);
        /* 1. IPC round trip: VFIO_USER_DMA_READ handled by QEMU */
        double t0 = now_us();
        for (int i = 0; i < iters; i++) {
            if (vfu_sgl_read(g_ctx, sg, n, buf, 0) < 0) { fprintf(stderr, "dma: sgl_read(ipc) failed errno=%d\n", errno); break; }
        }
        double ipc = (now_us() - t0) / iters;
        /* 2. direct access through the client-supplied fd (pread) */
        double direct = -1;
        if (mapped) {
            t0 = now_us();
            for (int i = 0; i < iters; i++) {
                if (vfu_sgl_read(g_ctx, sg, n, buf, VFU_SGL_DIRECT_ACCESS) < 0) { fprintf(stderr, "dma: sgl_read(direct) failed errno=%d\n", errno); break; }
            }
            direct = (now_us() - t0) / iters;
        }
        /* 3. mmap: vfu_sgl_get + memcpy */
        double mm = -1;
        if (mapped) {
            t0 = now_us();
            for (int i = 0; i < iters; i++) { vfu_sgl_get(g_ctx, sg, iov, n, 0); memcpy(buf, iov[0].iov_base, iov[0].iov_len); vfu_sgl_put(g_ctx, sg, iov, n); }
            mm = (now_us() - t0) / iters;
        }
        fprintf(stderr, "DMA_BENCH size=%zu fd_backed=%d ipc_read=%.2f us/op fd_pread=%.2f us/op mmap_memcpy=%.2f us/op\n",
                sz, mapped, ipc, direct, mm);
        free(sg);
    }
    free(buf);
}

static void init_tests(void)
{
    static const char *tn[3] = { "no-eventfd", "no-eventfd-b", "no-eventfd-c" };
    for (int type = 0; type < 2; type++) {
        for (int i = 0; i < 3; i++) {
            hdr_t *h = &tests[type][i];
            memset(h, 0, sizeof(*h));
            h->test = type * 3 + i;
            h->width = (i < ntests) ? 1 : 0;   /* width 0 ends enumeration */
            h->offset = (type == 0 ? IOTEST_MEMSIZE : IOTEST_IOSIZE) + type * 3 + i;
            h->data = 0xFA;                     /* match_data like datamatch */
            snprintf(h->name, sizeof(h->name), "%s-vfu-%s",
                     type == 0 ? "mmio" : "portio", tn[i]);
        }
    }
}

static ssize_t access_cb(int type, char *buf, size_t count, loff_t offset,
                         bool is_write)
{
    if (is_write) {
        if (offset == 0x800) {            /* DMA benchmark kick (posted write, guest pauses after it) */
            if (!dma_done) { dma_done = 1; dma_bench(); }
            return count;
        }
        if (offset == 0) {                /* select test */
            uint8_t val = (uint8_t)buf[0];
            current = -1;
            if (val < 3) {
                current = type * 3 + val;
                tests[type][val].count = 0;
            }
            return count;
        }
        if (current < 0) return count;
        hdr_t *h = &tests[current / 3][current % 3];
        if ((uint32_t)offset != h->offset) return count;
        if (count != 1 || (uint8_t)buf[0] != h->data) return count;
        h->count++;
        return count;
    }
    /* read */
    memset(buf, 0, count);
    if (offset == 0x801) {                /* sync read: single IPC DMA read inside the handler */
        static int done; 
        if (!done) {
            done = 1;
            dma_sg_t *sg = malloc(dma_sg_size() * 8); char *b = malloc(4096);
            int n = vfu_addr_to_sgl(g_ctx, (vfu_dma_addr_t)0x100000, 4096, sg, 8, PROT_READ);
            double t0 = now_us(); int r = (n > 0) ? vfu_sgl_read(g_ctx, sg, n, b, 0) : -1; int e = errno;
            fprintf(stderr, "SYNC_DMA_IN_READ_HANDLER: n=%d ret=%d errno=%d elapsed=%.0f us\n", n, r, e, now_us() - t0);
            free(sg); free(b);
        }
        return count;
    }
    if (current < 0) return count;
    hdr_t *h = &tests[current / 3][current % 3];
    size_t len = offsetof(hdr_t, name) + strlen(h->name) + 1;
    for (size_t i = 0; i < count; i++) {
        size_t a = offset + i;
        buf[i] = (a < len) ? ((char *)h)[a] : 0;
    }
    return count;
}

static ssize_t bar0_cb(vfu_ctx_t *c, char *buf, size_t n, loff_t off, bool w)
{ (void)c; return access_cb(0, buf, n, off, w); }
static ssize_t bar1_cb(vfu_ctx_t *c, char *buf, size_t n, loff_t off, bool w)
{ (void)c; return access_cb(1, buf, n, off, w); }
static ssize_t bar2_cb(vfu_ctx_t *c, char *buf, size_t n, loff_t off, bool w)
{ (void)c; (void)off; if (!w) memset(buf, 0, n); return n; }

static void dma_register(vfu_ctx_t *c, vfu_dma_info_t *info)
{ (void)c; fprintf(stderr, "dma map iova=%p len=%zu fd-backed=%d\n",
    info->iova.iov_base, info->iova.iov_len, info->vaddr != NULL); }
static void dma_unregister(vfu_ctx_t *c, vfu_dma_info_t *info)
{ (void)c; (void)info; }

static void logf_(vfu_ctx_t *c, int level, const char *msg)
{ (void)c; (void)level; fprintf(stderr, "vfu: %s\n", msg); }

int main(int argc, char **argv)
{
    const char *sock = NULL;
    bool with_mmap = false;
    int opt;
    while ((opt = getopt(argc, argv, "mvpn:")) != -1) {
        switch (opt) {
        case 'm': with_mmap = true; break;
        case 'v': verbose = 1; break;
        case 'p': polling = 1; break;
        case 'n': ntests = atoi(optarg); if (ntests < 1 || ntests > 3) ntests = 1; break;
        default: errx(1, "usage: %s [-m] [-v] [-n tests] <socket>", argv[0]);
        }
    }
    if (optind >= argc) errx(1, "missing socket path");
    sock = argv[optind];
    init_tests();

    vfu_ctx_t *ctx = vfu_create_ctx(VFU_TRANS_SOCK, sock, polling ? LIBVFIO_USER_FLAG_ATTACH_NB : 0, NULL, VFU_DEV_TYPE_PCI);
    if (!ctx) err(1, "vfu_create_ctx");
    if (verbose) vfu_setup_log(ctx, logf_, LOG_DEBUG);
    if (vfu_pci_init(ctx, VFU_PCI_TYPE_CONVENTIONAL, PCI_HEADER_TYPE_NORMAL, 0) < 0)
        err(1, "vfu_pci_init");
    vfu_pci_set_id(ctx, PCI_VENDOR_ID_REDHAT, PCI_DEVICE_ID_REDHAT_TEST, 0x1af4, 0x1100);
    vfu_pci_set_class(ctx, 0xff, 0x00, 0x00);

    if (vfu_setup_region(ctx, VFU_PCI_DEV_BAR0_REGION_IDX, 4096, bar0_cb,
                         VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM, NULL, 0, -1, 0) < 0)
        err(1, "setup bar0");
    if (vfu_setup_region(ctx, VFU_PCI_DEV_BAR1_REGION_IDX, 256, bar1_cb,
                         VFU_REGION_FLAG_RW, NULL, 0, -1, 0) < 0)
        err(1, "setup bar1");
    if (with_mmap) {
        int fd = memfd_create("bar2", 0);
        if (fd < 0 || ftruncate(fd, 4096) < 0) err(1, "memfd");
        struct iovec area = { .iov_base = (void *)0, .iov_len = 4096 };
        if (vfu_setup_region(ctx, VFU_PCI_DEV_BAR2_REGION_IDX, 4096, bar2_cb,
                             VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM, &area, 1, fd, 0) < 0)
            err(1, "setup bar2");
    }
    if (vfu_setup_device_dma(ctx, LIBVFIO_USER_MAX_DMA_REGIONS, dma_register, dma_unregister) < 0)
        err(1, "setup dma");
    g_ctx = ctx;
    if (vfu_realize_ctx(ctx) < 0) err(1, "realize");
    fprintf(stderr, "vfu-testdev: listening on %s (mmap bar2: %d, tests/type: %d)\n",
            sock, with_mmap, ntests);
    while (vfu_attach_ctx(ctx) < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(1000); continue; } err(1, "attach"); }
    fprintf(stderr, "vfu-testdev: client attached\n");
    for (;;) {
        if (vfu_run_ctx(ctx) < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue; /* busy poll */
            if (errno == ENOTCONN) { fprintf(stderr, "vfu-testdev: client gone\n"); break; }
            err(1, "vfu_run_ctx");
        }
    }
    for (int t = 0; t < 2; t++) for (int i = 0; i < ntests; i++)
        fprintf(stderr, "  %s: count=%u\n", tests[t][i].name, tests[t][i].count);
    vfu_destroy_ctx(ctx);
    return 0;
}
