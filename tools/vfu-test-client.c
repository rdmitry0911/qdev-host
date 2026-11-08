/*
 * vfu-test-client: a minimal vfio-user client to drive qdev-host plugins without
 * a VM. It speaks the protocol itself (vfio-user.h only, no libvfio-user) and
 * runs the commands given on the command line in order:
 *
 *   info                  device info, regions, IRQ counts
 *   cfg                   PCI config header: IDs, class, command, BARs, capability list
 *   r32 REG OFF           32-bit read  (REG: cfg, bar0..bar5 or a region number)
 *   w32 REG OFF VAL       32-bit write
 *   r16 REG OFF / w16 REG OFF VAL / r8 REG OFF
 *   expect32 REG OFF VAL  read and fail unless equal (expect16: 16-bit)
 *   dma                   map guest RAM backed by memfds (see below)
 *   unmap IOVA SIZE       DMA_UNMAP
 *   msi | intx            SET_IRQS: an eventfd for vector 0 of MSI or INTx
 *   irqs SECONDS HZ       test-v2: let the plugin raise IRQs at HZ for SECONDS and
 *                         measure trigger -> eventfd latency through its DMA record
 *   swap N HZ             replace the MSI eventfd N times while IRQs fire at HZ
 *   remap N HZ            unmap/map region B N times while the plugin writes its
 *                         IRQ record into region B at HZ (quiesce check)
 *   reset                 DEVICE_RESET
 *   sleep MS
 *
 * "dma" maps (iova, size, file, offset):
 *   A 0x00100000 4 MiB memfd1 @0x100000     B 0x40000000 4 MiB memfd1 @0x800000
 *   C 0x80000000 1 MiB memfd2 @0            D 0xc0000000 64 KiB no fd (message-based)
 * and then A again (identical re-map, must not reach the plugin twice). Every
 * 8 bytes of a memfd hold 0xd0a0000000000000 | (file << 40) | offset.
 *
 * Exit status 0 when every check passed.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include "vfio-user.h"

#define DIE(...) do { fprintf(stderr, "vfu-test-client: " __VA_ARGS__); fputc('\n', stderr); exit(2); } while (0)
#define CFG_REGION 7

static int sock;
static uint16_t msg_id;
static int failures;
static pthread_mutex_t sock_lock = PTHREAD_MUTEX_INITIALIZER;

static void check(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void check(bool ok, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    printf("%s: ", ok ? "PASS" : "FAIL"); vprintf(fmt, ap); putchar('\n');
    va_end(ap);
    if (!ok) failures++;
}

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }

static void recv_all(void *buf, size_t len)
{
    for (size_t got = 0; got < len; ) {
        ssize_t n = recv(sock, (char *)buf + got, len - got, 0);
        if (n <= 0) DIE("recv: %s", n ? strerror(errno) : "server closed the connection");
        got += n;
    }
}

/* one request, one reply; returns 0 or -1 with errno = the server's error */
static int xfer(uint16_t cmd, const void *req, size_t req_len, const int *fds, int nfds, void *reply, size_t reply_max, size_t *reply_len)
{
    pthread_mutex_lock(&sock_lock);
    struct vfio_user_header h = { .msg_id = ++msg_id, .cmd = cmd, .msg_size = sizeof(h) + req_len, .flags = VFIO_USER_F_TYPE_COMMAND };
    struct iovec iov[2] = { { &h, sizeof(h) }, { (void *)req, req_len } };
    struct msghdr mh = { .msg_iov = iov, .msg_iovlen = req_len ? 2 : 1 };
    char cbuf[CMSG_SPACE(sizeof(int) * 8)];
    if (nfds) {
        memset(cbuf, 0, sizeof(cbuf));
        mh.msg_control = cbuf; mh.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
        memcpy(CMSG_DATA(c), fds, sizeof(int) * nfds);
    }
    if (sendmsg(sock, &mh, MSG_NOSIGNAL) != (ssize_t)h.msg_size) DIE("sendmsg: %s", strerror(errno));
    struct vfio_user_header rh;
    recv_all(&rh, sizeof(rh));
    size_t body = rh.msg_size - sizeof(rh), keep = body < reply_max ? body : reply_max;
    if (keep) recv_all(reply, keep);
    for (size_t left = body - keep; left; ) { char junk[256]; size_t n = left < sizeof(junk) ? left : sizeof(junk); recv_all(junk, n); left -= n; }
    pthread_mutex_unlock(&sock_lock);
    if (rh.msg_id != h.msg_id || (rh.flags & VFIO_USER_F_TYPE_MASK) != VFIO_USER_F_TYPE_REPLY)
        DIE("unexpected message id %u cmd %u flags %#x (wanted the reply to %u)", rh.msg_id, rh.cmd, rh.flags, h.msg_id);
    if (reply_len) *reply_len = keep;
    if (rh.flags & VFIO_USER_F_ERROR) { errno = rh.error_no; return -1; }
    return 0;
}

static void negotiate(void)
{
    uint8_t buf[512], rep[512];
    struct vfio_user_version *v = (void *)buf;
    v->major = 0; v->minor = 2;
    int n = snprintf((char *)v->data, sizeof(buf) - sizeof(*v),
                     "{\"capabilities\":{\"max_msg_fds\":8,\"max_data_xfer_size\":1048576}}");
    size_t rl;
    if (xfer(VFIO_USER_VERSION, buf, sizeof(*v) + n + 1, NULL, 0, rep, sizeof(rep) - 1, &rl) < 0) DIE("VERSION: %s", strerror(errno));
    rep[rl] = 0;
    struct vfio_user_version *sv = (void *)rep;
    printf("server version %u.%u %s\n", sv->major, sv->minor, rl > sizeof(*sv) ? (char *)sv->data : "");
}

static int region_arg(const char *s)
{
    if (!strcmp(s, "cfg")) return CFG_REGION;
    if (!strncmp(s, "bar", 3)) return atoi(s + 3);
    return atoi(s);
}

static int region_rw(int region, uint64_t off, void *data, uint32_t len, bool write)
{
    uint8_t buf[sizeof(struct vfio_user_region_access) + 4096];
    struct vfio_user_region_access *a = (void *)buf;
    if (len > 4096) DIE("access too large");
    a->offset = off; a->region = region; a->count = len;
    if (write) {
        memcpy(a->data, data, len);
        return xfer(VFIO_USER_REGION_WRITE, buf, sizeof(*a) + len, NULL, 0, buf, sizeof(*a), NULL);
    }
    size_t rl;
    if (xfer(VFIO_USER_REGION_READ, buf, sizeof(*a), NULL, 0, buf, sizeof(*a) + len, &rl) < 0) return -1;
    if (rl < sizeof(*a) + len || a->count != len) { errno = EIO; return -1; }
    memcpy(data, a->data, len);
    return 0;
}
static uint32_t rd(int region, uint64_t off, int size)
{
    uint32_t v = 0;
    if (region_rw(region, off, &v, size, false) < 0) DIE("read region %d off %#llx: %s", region, (unsigned long long)off, strerror(errno));
    return v;
}
static void wr(int region, uint64_t off, uint32_t v, int size)
{
    if (region_rw(region, off, &v, size, true) < 0) DIE("write region %d off %#llx: %s", region, (unsigned long long)off, strerror(errno));
}

static void cmd_info(void)
{
    struct vfio_user_device_info di = { .argsz = sizeof(di) };
    if (xfer(VFIO_USER_DEVICE_GET_INFO, &di, sizeof(di), NULL, 0, &di, sizeof(di), NULL) < 0) DIE("GET_INFO: %s", strerror(errno));
    printf("device: flags %#x, %u regions, %u irq types\n", di.flags, di.num_regions, di.num_irqs);
    for (uint32_t i = 0; i < di.num_regions; i++) {
        struct vfio_region_info ri = { .argsz = sizeof(ri), .index = i };
        if (xfer(VFIO_USER_DEVICE_GET_REGION_INFO, &ri, sizeof(ri), NULL, 0, &ri, sizeof(ri), NULL) < 0) continue;
        if (ri.size) printf("  region %u: size %#llx flags %#x%s\n", i, (unsigned long long)ri.size, ri.flags,
                            ri.flags & VFIO_REGION_INFO_FLAG_MMAP ? " (mmap)" : "");
    }
    for (uint32_t i = 0; i < di.num_irqs; i++) {
        struct vfio_irq_info ii = { .argsz = sizeof(ii), .index = i };
        if (xfer(VFIO_USER_DEVICE_GET_IRQ_INFO, &ii, sizeof(ii), NULL, 0, &ii, sizeof(ii), NULL) < 0) continue;
        if (ii.count) printf("  irq index %u: count %u flags %#x\n", i, ii.count, ii.flags);
    }
}

static void cmd_cfg(void)
{
    uint8_t c[256];
    if (region_rw(CFG_REGION, 0, c, 256, false) < 0) DIE("config read: %s", strerror(errno));
    printf("config: vendor %04x device %04x command %04x status %04x rev %02x class %02x%02x%02x header %02x irq-pin %u\n",
           c[0] | c[1] << 8, c[2] | c[3] << 8, c[4] | c[5] << 8, c[6] | c[7] << 8, c[8], c[11], c[10], c[9], c[14], c[0x3d]);
    printf("  subsystem %04x:%04x, BARs", c[0x2c] | c[0x2d] << 8, c[0x2e] | c[0x2f] << 8);
    for (int i = 0; i < 6; i++) { uint32_t b; memcpy(&b, c + 0x10 + 4 * i, 4); printf(" %08x", b); }
    printf("\n  capabilities:");
    for (int p = (c[6] & 0x10) ? c[0x34] : 0, n = 0; p && n < 48; p = c[p + 1], n++) printf(" [%02x] id %02x", p, c[p]);
    putchar('\n');
}

/* ------------------------------------------------------------ DMA */
#define MEMFD1_SIZE (16u << 20)
#define MEMFD2_SIZE (1u << 20)
static int memfd1 = -1, memfd2 = -1;
static uint8_t *mem1;
struct dma { uint64_t iova, size, offset; int *fd; };
static struct dma dmas[] = {
    { 0x00100000, 4u << 20, 0x100000, &memfd1 },
    { 0x40000000, 4u << 20, 0x800000, &memfd1 },
    { 0x80000000, 1u << 20, 0, &memfd2 },
    { 0xc0000000, 64u << 10, 0, NULL },
};
static uint64_t pattern(int file, uint64_t off) { return 0xd0a0000000000000ull | ((uint64_t)file << 40) | off; }

static int dma_map(const struct dma *m)
{
    struct vfio_user_dma_map dm = { .argsz = sizeof(dm), .flags = VFIO_USER_F_DMA_REGION_READ | VFIO_USER_F_DMA_REGION_WRITE,
                                    .offset = m->fd ? m->offset : 0, .addr = m->iova, .size = m->size };
    return xfer(VFIO_USER_DMA_MAP, &dm, sizeof(dm), m->fd ? m->fd : NULL, m->fd ? 1 : 0, NULL, 0, NULL);
}
static int dma_unmap(uint64_t iova, uint64_t size)
{
    struct vfio_user_dma_unmap du = { .argsz = sizeof(du), .addr = iova, .size = size };
    return xfer(VFIO_USER_DMA_UNMAP, &du, sizeof(du), NULL, 0, &du, sizeof(du), NULL);
}
static int fill_memfd(int file, size_t size)
{
    int fd = memfd_create(file == 1 ? "guest-ram-1" : "guest-ram-2", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0 || ftruncate(fd, size) < 0) DIE("memfd: %s", strerror(errno));
    uint64_t *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) DIE("mmap: %s", strerror(errno));
    for (size_t i = 0; i < size / 8; i++) p[i] = pattern(file, i * 8);
    if (file == 1) mem1 = (uint8_t *)p; else munmap(p, size);
    return fd;
}
static bool is_test_v2(void) { return rd(0, 0x00, 4) == 0x7e570002; }

static void cmd_dma(void)
{
    memfd1 = fill_memfd(1, MEMFD1_SIZE); memfd2 = fill_memfd(2, MEMFD2_SIZE);
    for (size_t i = 0; i < sizeof(dmas) / sizeof(dmas[0]); i++) {
        int r = dma_map(&dmas[i]);
        check(r == 0, "DMA_MAP %c iova %#llx size %#llx %s offset %#llx", (char)('A' + i), (unsigned long long)dmas[i].iova,
              (unsigned long long)dmas[i].size, !dmas[i].fd ? "no fd" : dmas[i].fd == &memfd1 ? "memfd1" : "memfd2",
              (unsigned long long)dmas[i].offset);
    }
    check(dma_map(&dmas[0]) == 0, "identical re-map of A accepted");
    if (!is_test_v2()) return;
    check(rd(0, 0x3c, 4) == 4, "plugin sees 4 regions (re-map not forwarded twice): %u", rd(0, 0x3c, 4));
    check(rd(0, 0x48, 4) == 3, "plugin compared vaddr == mmap(fd, fd_offset) for the 3 fd regions: %u", rd(0, 0x48, 4));
    /* probes at other places: through vaddr and fd, against the pattern the client wrote */
    struct { uint64_t iova; int file; uint64_t off; } pr[] = {
        { 0x00100000 + 0x12340, 1, 0x100000 + 0x12340 }, { 0x40000000 + 0x3ffff8, 1, 0x800000 + 0x3ffff8 },
        { 0x80000000 + 0x8, 2, 0x8 },
    };
    for (size_t i = 0; i < sizeof(pr) / sizeof(pr[0]); i++) {
        wr(0, 0x18, pr[i].iova, 4); wr(0, 0x1c, pr[i].iova >> 32, 4);
        uint64_t v = rd(0, 0x20, 4) | (uint64_t)rd(0, 0x24, 4) << 32;
        uint32_t st = rd(0, 0x28, 4);
        check(st == 1 && v == pattern(pr[i].file, pr[i].off), "probe iova %#llx: status %u value %#llx (expected %#llx at memfd%d+%#llx)",
              (unsigned long long)pr[i].iova, st, (unsigned long long)v, (unsigned long long)pattern(pr[i].file, pr[i].off),
              pr[i].file, (unsigned long long)pr[i].off);
    }
    wr(0, 0x18, 0xc0000000, 4); wr(0, 0x1c, 0, 4);
    check(rd(0, 0x28, 4) == 3, "probe of the fd-less region D: not readable from plugin threads (status %u)", rd(0, 0x28, 4));
    /* a client write into guest RAM is visible to the plugin at once (shared mapping) */
    uint64_t mark = 0x1122334455667788ull;
    memcpy(mem1 + 0x100000 + 0x2000, &mark, 8);
    wr(0, 0x18, 0x00102000, 4); wr(0, 0x1c, 0, 4);
    check((rd(0, 0x20, 4) | (uint64_t)rd(0, 0x24, 4) << 32) == mark, "client write to guest RAM seen by the plugin");
}

/* ------------------------------------------------------------ IRQs */
static int irq_fd = -1;
static int set_irq(int index, int fd)
{
    struct vfio_irq_set s = { .argsz = sizeof(s), .index = index, .start = 0, .count = fd >= 0 ? 1 : 0,
                              .flags = VFIO_IRQ_SET_ACTION_TRIGGER | (fd >= 0 ? VFIO_IRQ_SET_DATA_EVENTFD : VFIO_IRQ_SET_DATA_NONE) };
    return xfer(VFIO_USER_DEVICE_SET_IRQS, &s, sizeof(s), fd >= 0 ? &fd : NULL, fd >= 0 ? 1 : 0, NULL, 0, NULL);
}
static void cmd_irq(int index)
{
    irq_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    check(set_irq(index, irq_fd) == 0, "SET_IRQS %s vector 0 -> eventfd", index == VFIO_PCI_MSI_IRQ_INDEX ? "MSI" : "INTx");
}

static int cmp_u64(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }

/* record: test-v2 writes {seq, t_ns} at iova REC before each IRQ; REC in region A = memfd1 offset */
#define REC_A_IOVA 0x00101000ull
#define REC_A_OFF  0x00101000ull
static void cmd_irqs(double seconds, unsigned hz)
{
    if (irq_fd < 0 || !mem1) DIE("irqs needs dma and msi/intx first");
    volatile uint64_t *rec = (uint64_t *)(mem1 + REC_A_OFF);
    uint32_t ok0 = rd(0, 0x08, 4);
    wr(0, 0x10, (uint32_t)REC_A_IOVA, 4); wr(0, 0x14, REC_A_IOVA >> 32, 4);
    wr(0, 0x04, hz, 4);
    size_t cap = (size_t)(seconds * hz) + 1024, n = 0;
    uint64_t *lat = malloc(cap * sizeof(*lat)), events = 0, wakes = 0, end = now_ns() + (uint64_t)(seconds * 1e9);
    while (now_ns() < end) {
        struct pollfd p = { irq_fd, POLLIN };
        if (poll(&p, 1, 100) <= 0) continue;
        uint64_t cnt;
        if (read(irq_fd, &cnt, sizeof(cnt)) != sizeof(cnt)) continue;
        uint64_t t = __atomic_load_n(&rec[1], __ATOMIC_ACQUIRE), now = now_ns();
        events += cnt; wakes++;
        if (n < cap && t && now >= t) lat[n++] = now - t;
    }
    wr(0, 0x04, 0, 4);
    usleep(50000);
    uint64_t cnt;
    if (read(irq_fd, &cnt, sizeof(cnt)) == sizeof(cnt)) events += cnt;
    uint32_t ok = rd(0, 0x08, 4) - ok0, drop = rd(0, 0x0c, 4);
    qsort(lat, n, sizeof(*lat), cmp_u64);
    uint64_t sum = 0; for (size_t i = 0; i < n; i++) sum += lat[i];
    printf("irqs: %u Hz for %.1f s: plugin irq_trigger ok %u (dropped %u total), eventfd events %llu in %llu wakes\n",
           hz, seconds, ok, drop, (unsigned long long)events, (unsigned long long)wakes);
    if (n) printf("irqs: trigger->client wake latency (record write .. eventfd read): avg %.1f us, p50 %.1f, p99 %.1f, max %.1f us (%zu samples)\n",
                  sum / (double)n / 1e3, lat[n / 2] / 1e3, lat[n * 99 / 100] / 1e3, lat[n - 1] / 1e3, n);
    printf("irqs: plugin-side irq_trigger() call duration avg %u ns, max %u ns\n", rd(0, 0x40, 4), rd(0, 0x44, 4));
    check(ok > 0 && events == ok, "every successful irq_trigger reached the eventfd (%llu of %u)", (unsigned long long)events, ok);
    free(lat);
}

/* replace the eventfd under a running IRQ stream: SET_IRQS closes the old fd in the
 * server while the plugin thread may be writing to it */
static void cmd_swap(int n, unsigned hz)
{
    if (irq_fd < 0) DIE("swap needs msi first");
    uint32_t ok0 = rd(0, 0x08, 4);
    uint64_t events = 0, cnt;
    wr(0, 0x04, hz, 4);
    int failed = 0, disabled = 0;
    for (int i = 0; i < n; i++) {
        int nfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (i % 10 == 9) { if (set_irq(VFIO_PCI_MSI_IRQ_INDEX, -1) < 0) failed++; disabled++; usleep(200); }
        if (set_irq(VFIO_PCI_MSI_IRQ_INDEX, nfd) < 0) failed++;
        if (read(irq_fd, &cnt, sizeof(cnt)) == sizeof(cnt)) events += cnt;
        close(irq_fd); irq_fd = nfd;
        usleep(300);
    }
    wr(0, 0x04, 0, 4);
    usleep(50000);
    if (read(irq_fd, &cnt, sizeof(cnt)) == sizeof(cnt)) events += cnt;
    uint32_t ok = rd(0, 0x08, 4) - ok0;
    printf("swap: %d eventfd replacements (%d with a disable first) at %u Hz: plugin ok %u, events seen %llu\n",
           n, disabled, hz, ok, (unsigned long long)events);
    check(failed == 0 && events > 0 && events <= ok, "SET_IRQS swaps under load: no failure, events <= ok");
}

/* unmap/map region B while the plugin's IRQ thread writes its record into B */
static void cmd_remap(int n, unsigned hz)
{
    const struct dma *b = &dmas[1];
    uint64_t rec = b->iova + 0x100;
    uint32_t q0 = rd(0, 0x38, 4);
    wr(0, 0x10, (uint32_t)rec, 4); wr(0, 0x14, rec >> 32, 4);
    wr(0, 0x04, hz, 4);
    int failed = 0;
    uint64_t t0 = now_ns();
    for (int i = 0; i < n; i++) {
        if (dma_unmap(b->iova, b->size) < 0) failed++;
        usleep(200);
        if (dma_map(b) < 0) failed++;
        usleep(200);
    }
    double dt = (now_ns() - t0) / 1e9;
    wr(0, 0x04, 0, 4);
    uint32_t q = rd(0, 0x38, 4) - q0;
    printf("remap: %d unmap+map cycles of B in %.3f s with the IRQ record in B at %u Hz: %u quiesces\n", n, dt, hz, q);
    check(failed == 0 && q >= (uint32_t)(2 * n), "remap under load: no failure, a quiesce for every map change");
    check(rd(0, 0x3c, 4) == 4, "plugin region table back to 4 regions");
    wr(0, 0x10, 0, 4); wr(0, 0x14, 0, 4);
}

static const char *need(int argc, char **argv, int i, const char *cmd)
{
    if (i >= argc) DIE("%s: missing argument", cmd);
    return argv[i];
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: vfu-test-client SOCKET CMD...  (see the comment at the top of the source)\n"); return 2; }
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", argv[1]);
    sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (connect(sock, (struct sockaddr *)&a, sizeof(a)) < 0) DIE("connect %s: %s", argv[1], strerror(errno));
    negotiate();
    for (int i = 2; i < argc; i++) {
        const char *c = argv[i];
#define ARG(k) need(argc, argv, i + (k), c)
        if (!strcmp(c, "info")) cmd_info();
        else if (!strcmp(c, "cfg")) cmd_cfg();
        else if (!strcmp(c, "r32") || !strcmp(c, "r16") || !strcmp(c, "r8")) {
            int sz = c[1] == '3' ? 4 : c[1] == '1' ? 2 : 1;
            int reg = region_arg(ARG(1)); uint64_t off = strtoull(ARG(2), NULL, 0); i += 2;
            printf("read%d region %d off %#llx = %#x\n", sz * 8, reg, (unsigned long long)off, rd(reg, off, sz));
        } else if (!strcmp(c, "w32") || !strcmp(c, "w16")) {
            int sz = c[1] == '3' ? 4 : 2;
            int reg = region_arg(ARG(1)); uint64_t off = strtoull(ARG(2), NULL, 0); uint32_t v = strtoul(ARG(3), NULL, 0); i += 3;
            wr(reg, off, v, sz);
            printf("write%d region %d off %#llx = %#x\n", sz * 8, reg, (unsigned long long)off, v);
        } else if (!strcmp(c, "expect32") || !strcmp(c, "expect16")) {
            int sz = c[6] == '3' ? 4 : 2;
            int reg = region_arg(ARG(1)); uint64_t off = strtoull(ARG(2), NULL, 0); uint32_t v = strtoul(ARG(3), NULL, 0); i += 3;
            uint32_t got = rd(reg, off, sz);
            check(got == v, "region %d off %#llx = %#x (expected %#x)", reg, (unsigned long long)off, got, v);
        } else if (!strcmp(c, "dma")) cmd_dma();
        else if (!strcmp(c, "unmap")) {
            uint64_t iova = strtoull(ARG(1), NULL, 0), size = strtoull(ARG(2), NULL, 0); i += 2;
            check(dma_unmap(iova, size) == 0, "DMA_UNMAP %#llx size %#llx", (unsigned long long)iova, (unsigned long long)size);
        } else if (!strcmp(c, "msi")) cmd_irq(VFIO_PCI_MSI_IRQ_INDEX);
        else if (!strcmp(c, "intx")) cmd_irq(VFIO_PCI_INTX_IRQ_INDEX);
        else if (!strcmp(c, "irqs")) { double s = atof(ARG(1)); unsigned hz = atoi(ARG(2)); i += 2; cmd_irqs(s, hz); }
        else if (!strcmp(c, "swap")) { int n = atoi(ARG(1)); unsigned hz = atoi(ARG(2)); i += 2; cmd_swap(n, hz); }
        else if (!strcmp(c, "remap")) { int n = atoi(ARG(1)); unsigned hz = atoi(ARG(2)); i += 2; cmd_remap(n, hz); }
        else if (!strcmp(c, "reset")) check(xfer(VFIO_USER_DEVICE_RESET, NULL, 0, NULL, 0, NULL, 0, NULL) == 0, "DEVICE_RESET");
        else if (!strcmp(c, "sleep")) { usleep(atoi(ARG(1)) * 1000); i++; }
        else DIE("unknown command %s", c);
    }
    printf("%s (%d failed checks)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
