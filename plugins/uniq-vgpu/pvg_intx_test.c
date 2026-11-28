/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * pvg_intx_test: native test of the uniq-vgpu interrupt line (D-ARM64-INTX): the production pvg-intx.c state and
 * dispatch, over the vendored (patched) libvfio-user, driven by a vfio-user client speaking the protocol itself.
 * No GPU, no VM, no libmlapi.
 *
 * Reference contract (the vfio-user client side, QEMU 11.1.3 hw/vfio/pci.c, stock):
 *   ASSERT = the device signals its INTx eventfd; the client raises the guest line and automasks the source
 *   (vfio_intx_interrupt).  At the guest's EOI it deasserts the line and sends SET_IRQS(INTx, UNMASK, DATA_NONE)
 *   (vfio_pci_intx_eoi, on the first BAR access after the assertion).  A level device re-asserts after UNMASK
 *   while its condition is pending.  The guest's acknowledgement is the stock handlers' status read: 0x1014 /
 *   0x1018 (GPU, read-and-clear) and 0x1250 RING_RD (IOSurface mapper).  Under KVM the client first offers its
 *   resamplefd as SET_IRQS(INTx, UNMASK, DATA_EVENTFD); refusing it keeps the userspace path above
 *   (vfio_intx_enable_kvm fail path; debt D-VFU-INTX-RESAMPLEFD).
 *
 * Part A: the level state (pvg_intx_*) alone.  Part B: one device per irq= mode on a real libvfio-user context;
 * the client checks config space, SET_IRQS handling, the trigger index, and every assert/deassert step by the
 * eventfd counts, with the client's UNMASK messages and BAR0 status reads in the reference order.
 */
#define _GNU_SOURCE
#include "pvg-intx.h"
#include "vfio-user.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

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

/* ------------------------------------------------------------------------------------------------ Part A */
static void part_a(void)
{
    PvgIntx x;

    printf("== A: level state\n");
    pvg_intx_reset(&x);
    check(pvg_intx_raise(&x), "A1 first raise asserts (line down, unmasked)");
    check(!pvg_intx_raise(&x), "A2 raise while asserted: no new edge (held)");
    check(pvg_intx_set_mask(&x, false), "A3 UNMASK with a raise after the assertion and no source acknowledged: re-assert");
    check(!pvg_intx_set_mask(&x, false), "A4 UNMASK with no raise since the last assertion: line stays down");
    check(!pvg_intx_set_mask(&x, false), "A5 repeated UNMASK without a raise: still nothing (no storm)");

    pvg_intx_reset(&x);
    (void)pvg_intx_raise(&x);
    (void)pvg_intx_raise(&x);
    pvg_intx_ack(&x, PVG_INTX_ACK_GPU);                   /* handler reads 0x1018 after both raises */
    {
        bool again = pvg_intx_set_mask(&x, false);
        check(!again && x.suppressed == 1,
              "A6 UNMASK after the GPU handler read every raise: deasserted (suppressed %llu)", (unsigned long long)x.suppressed);
    }

    (void)pvg_intx_raise(&x);
    pvg_intx_ack(&x, PVG_INTX_ACK_GPU);
    (void)pvg_intx_raise(&x);                             /* raise after the ack started: its cause may be unread */
    check(pvg_intx_set_mask(&x, false), "A7 raise after the handler's read: re-assert (no loss)");

    pvg_intx_reset(&x);
    pvg_intx_ack(&x, PVG_INTX_ACK_MAPPER);                /* the mapper handler has run once */
    (void)pvg_intx_raise(&x);
    (void)pvg_intx_raise(&x);
    pvg_intx_ack(&x, PVG_INTX_ACK_GPU);                   /* only the GPU read after the raises */
    check(pvg_intx_set_mask(&x, false), "A8 an acknowledging source (mapper) has not read since the raise: re-assert");
    (void)pvg_intx_raise(&x);
    pvg_intx_ack(&x, PVG_INTX_ACK_GPU);
    pvg_intx_ack(&x, PVG_INTX_ACK_MAPPER);
    check(!pvg_intx_set_mask(&x, false), "A9 both sources read after the raise: deasserted");

    pvg_intx_reset(&x);
    check(!pvg_intx_set_mask(&x, true), "A10 client MASK");
    check(!pvg_intx_raise(&x), "A11 raise while masked: no edge");
    check(pvg_intx_set_mask(&x, false), "A12 UNMASK: the masked raise asserts");

    pvg_intx_reset(&x);
    (void)pvg_intx_raise(&x);
    (void)pvg_intx_raise(&x);
    check(pvg_intx_set_mask(&x, false), "A13 re-assert requested ...");
    pvg_intx_cancel_assert(&x);                           /* device interrupts held: not signalled */
    check(!x.asserted && !x.masked && pvg_intx_raise(&x), "A14 ... cancelled: the next raise asserts");

    check(pvg_intx_ack_source(0x1014) == PVG_INTX_ACK_GPU && pvg_intx_ack_source(0x1018) == PVG_INTX_ACK_GPU &&
          pvg_intx_ack_source(0x1250) == PVG_INTX_ACK_MAPPER && pvg_intx_ack_source(0x1254) < 0 &&
          pvg_intx_ack_source(0x1020) < 0 && pvg_intx_ack_source(0x1230) < 0,
          "A15 ack registers: 0x1014/0x1018 GPU, 0x1250 mapper, nothing else (0x1254, KICK 0x1020, RING_BASE 0x1230)");
}

/* ------------------------------------------------------------------------------------------------ server */
typedef struct {
    int mode;
    char path[108];
    vfu_ctx_t *ctx;
    PvgIrqLine line;
    pthread_mutex_t lock;              /* the plugin's irq_lock */
    pthread_t thread;
    int ready;                          /* socket listening */
    pthread_mutex_t rlock;
    pthread_cond_t rcond;
} Dev;
static Dev *cur;

static int dev_trigger(void *host, uint32_t subindex)   /* qdev-host host_irq_trigger */
{
    Dev *d = host;

    if (vfu_irq_trigger(d->ctx, subindex) < 0) {
        return errno == ENOENT ? -EINVAL : -errno;
    }
    return 0;
}
static void dev_state_cb(vfu_ctx_t *ctx, uint32_t start, uint32_t count, bool mask)
{
    (void)ctx; (void)start; (void)count;
    pthread_mutex_lock(&cur->lock);
    (void)pvg_irq_line_mask(&cur->line, mask, true, NULL);
    pthread_mutex_unlock(&cur->lock);
}
static ssize_t dev_bar0(vfu_ctx_t *ctx, char *buf, size_t count, loff_t off, bool is_write)
{
    (void)ctx;
    if (!is_write) {
        pthread_mutex_lock(&cur->lock);
        pvg_irq_line_bar0_read(&cur->line, (uint32_t)off);
        pthread_mutex_unlock(&cur->lock);
        memset(buf, 0, count);
    }
    return (ssize_t)count;
}
static void *dev_main(void *arg)
{
    Dev *d = arg;
    int r;

    d->ctx = vfu_create_ctx(VFU_TRANS_SOCK, d->path, 0, NULL, VFU_DEV_TYPE_PCI);
    if (!d->ctx || vfu_pci_init(d->ctx, VFU_PCI_TYPE_EXPRESS, PCI_HEADER_TYPE_NORMAL, 0) < 0) {
        fprintf(stderr, "server: context: %s\n", strerror(errno));
        exit(2);
    }
    vfu_pci_set_id(d->ctx, 0x106b, 0xeeee, 0, 0);
    pvg_irq_line_init(&d->line, d->mode, d->ctx, dev_trigger, d);
    if (pvg_irq_line_setup(&d->line, dev_state_cb) < 0 ||
        vfu_setup_region(d->ctx, VFU_PCI_DEV_BAR0_REGION_IDX, 0x4000, dev_bar0, VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM,
                         NULL, 0, -1, 0) < 0 ||
        vfu_realize_ctx(d->ctx) < 0) {
        fprintf(stderr, "server: setup: %s\n", strerror(errno));
        exit(2);
    }
    pthread_mutex_lock(&d->rlock);
    d->ready = 1;
    pthread_cond_signal(&d->rcond);
    pthread_mutex_unlock(&d->rlock);
    if (vfu_attach_ctx(d->ctx) < 0) {
        fprintf(stderr, "server: attach: %s\n", strerror(errno));
        exit(2);
    }
    do {
        r = vfu_run_ctx(d->ctx);
    } while (r >= 0 || errno == EINTR || errno == EAGAIN || errno == EBUSY);
    vfu_destroy_ctx(d->ctx);
    return NULL;
}

static void dev_raise(Dev *d, bool *intx)
{
    pthread_mutex_lock(&d->lock);
    (void)pvg_irq_line_raise(&d->line, 0, intx);
    pthread_mutex_unlock(&d->lock);
}

/* ------------------------------------------------------------------------------------------------ client */
static int sock = -1;
static uint16_t msg_id;

static void recv_all(void *buf, size_t len)
{
    for (size_t got = 0; got < len;) {
        ssize_t n = recv(sock, (char *)buf + got, len - got, 0);
        if (n <= 0) {
            fprintf(stderr, "client: recv: %s\n", n ? strerror(errno) : "closed");
            exit(2);
        }
        got += (size_t)n;
    }
}
static int xfer(uint16_t cmd, const void *req, size_t req_len, const int *fds, int nfds, void *reply, size_t reply_max)
{
    struct vfio_user_header h = { .msg_id = ++msg_id, .cmd = cmd, .msg_size = (uint32_t)(sizeof(h) + req_len),
                                  .flags = VFIO_USER_F_TYPE_COMMAND };
    struct iovec iov[2] = { { &h, sizeof(h) }, { (void *)req, req_len } };
    struct msghdr mh = { .msg_iov = iov, .msg_iovlen = req_len ? 2 : 1 };
    char cbuf[CMSG_SPACE(sizeof(int) * 4)];
    struct vfio_user_header rh;
    size_t body, keep;

    if (nfds) {
        memset(cbuf, 0, sizeof(cbuf));
        mh.msg_control = cbuf;
        mh.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
        memcpy(CMSG_DATA(c), fds, sizeof(int) * nfds);
    }
    if (sendmsg(sock, &mh, MSG_NOSIGNAL) != (ssize_t)h.msg_size) {
        fprintf(stderr, "client: sendmsg: %s\n", strerror(errno));
        exit(2);
    }
    recv_all(&rh, sizeof(rh));
    body = rh.msg_size - sizeof(rh);
    keep = body < reply_max ? body : reply_max;
    if (keep) {
        recv_all(reply, keep);
    }
    for (size_t left = body - keep; left;) {
        char junk[256];
        size_t n = left < sizeof(junk) ? left : sizeof(junk);
        recv_all(junk, n);
        left -= n;
    }
    if (rh.flags & VFIO_USER_F_ERROR) {
        errno = (int)rh.error_no;
        return -1;
    }
    return 0;
}
static void negotiate(void)
{
    uint8_t buf[256], rep[256];
    struct vfio_user_version *v = (void *)buf;
    int n;

    v->major = 0;
    v->minor = 2;
    n = snprintf((char *)v->data, sizeof(buf) - sizeof(*v), "{\"capabilities\":{\"max_msg_fds\":8}}");
    if (xfer(VFIO_USER_VERSION, buf, sizeof(*v) + (size_t)n + 1, NULL, 0, rep, sizeof(rep)) < 0) {
        fprintf(stderr, "client: VERSION: %s\n", strerror(errno));
        exit(2);
    }
}
static int set_irqs(uint32_t index, uint32_t action, uint32_t data, int fd)
{
    struct vfio_irq_set s = { .argsz = sizeof(s), .flags = action | data, .index = index, .start = 0,
                              .count = (data == VFIO_IRQ_SET_DATA_NONE && action == VFIO_IRQ_SET_ACTION_TRIGGER && fd < 0) ? 0 : 1 };

    return xfer(VFIO_USER_DEVICE_SET_IRQS, &s, sizeof(s), fd >= 0 ? &fd : NULL, fd >= 0 ? 1 : 0, NULL, 0);
}
#define TRIGGER(index, fd) set_irqs(index, VFIO_IRQ_SET_ACTION_TRIGGER, (fd) >= 0 ? VFIO_IRQ_SET_DATA_EVENTFD : VFIO_IRQ_SET_DATA_NONE, fd)
#define UNMASK() set_irqs(VFIO_PCI_INTX_IRQ_INDEX, VFIO_IRQ_SET_ACTION_UNMASK, VFIO_IRQ_SET_DATA_NONE, -1)
#define MASK() set_irqs(VFIO_PCI_INTX_IRQ_INDEX, VFIO_IRQ_SET_ACTION_MASK, VFIO_IRQ_SET_DATA_NONE, -1)

static uint32_t rd(int region, uint64_t off, uint32_t len)
{
    uint8_t buf[sizeof(struct vfio_user_region_access) + 256];
    struct vfio_user_region_access *a = (void *)buf;
    uint32_t v = 0;

    a->offset = off;
    a->region = (uint32_t)region;
    a->count = len;
    if (xfer(VFIO_USER_REGION_READ, buf, sizeof(*a), NULL, 0, buf, sizeof(*a) + len) < 0) {
        fprintf(stderr, "client: read: %s\n", strerror(errno));
        exit(2);
    }
    memcpy(&v, a->data, len < 4 ? len : 4);
    return v;
}
static void rd_cfg(uint8_t *cfg)
{
    uint8_t buf[sizeof(struct vfio_user_region_access) + 256];
    struct vfio_user_region_access *a = (void *)buf;

    a->offset = 0;
    a->region = VFU_PCI_DEV_CFG_REGION_IDX;
    a->count = 256;
    if (xfer(VFIO_USER_REGION_READ, buf, sizeof(*a), NULL, 0, buf, sizeof(*a) + 256) < 0) {
        fprintf(stderr, "client: config read: %s\n", strerror(errno));
        exit(2);
    }
    memcpy(cfg, a->data, 256);
}
static uint64_t take(int efd)                        /* eventfd count since the last take (0 = not signalled) */
{
    uint64_t v = 0;

    if (read(efd, &v, sizeof(v)) != sizeof(v)) {
        return 0;
    }
    return v;
}

static Dev *start_dev(int mode, const char *dir, const char *name)
{
    Dev *d = calloc(1, sizeof(*d));
    struct sockaddr_un sa = { .sun_family = AF_UNIX };

    d->mode = mode;
    snprintf(d->path, sizeof(d->path), "%s/%s", dir, name);
    unlink(d->path);
    pthread_mutex_init(&d->lock, NULL);
    pthread_mutex_init(&d->rlock, NULL);
    pthread_cond_init(&d->rcond, NULL);
    cur = d;
    pthread_create(&d->thread, NULL, dev_main, d);
    pthread_mutex_lock(&d->rlock);
    while (!d->ready) {
        pthread_cond_wait(&d->rcond, &d->rlock);
    }
    pthread_mutex_unlock(&d->rlock);
    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", d->path);
    if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "client: connect %s: %s\n", d->path, strerror(errno));
        exit(2);
    }
    msg_id = 0;
    negotiate();
    return d;
}
static void stop_dev(Dev *d)
{
    close(sock);
    sock = -1;
    pthread_join(d->thread, NULL);
    unlink(d->path);
    free(d);
    cur = NULL;
}

static bool has_msi_cap(const uint8_t *c)
{
    for (int p = (c[6] & 0x10) ? c[0x34] : 0, n = 0; p && n < 48; p = c[p + 1], n++) {
        if (c[p] == 0x05) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------------------------------------ Part B */
static void part_b_auto(const char *dir)
{
    Dev *d = start_dev(PVG_IRQ_MODE_AUTO, dir, "intx-auto.sock");
    uint8_t cfg[256];
    int ei = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK), em = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    bool intx = false;

    printf("== B: irq=auto over libvfio-user\n");
    rd_cfg(cfg);
    check(cfg[0x3d] == 1 && has_msi_cap(cfg), "B1 config: INTA# (pin %u) and the MSI capability", cfg[0x3d]);
    check(vfu_irq_trigger_index(d->ctx) == -1, "B2 no trigger index before the client sets an eventfd");
    dev_raise(d, &intx);
    check(!intx && take(ei) == 0, "B3 raise with nothing enabled: dropped, as on PCI");

    check(TRIGGER(VFIO_PCI_INTX_IRQ_INDEX, ei) == 0 && vfu_irq_trigger_index(d->ctx) == VFU_DEV_INTX_IRQ,
          "B4 SET_IRQS INTx eventfd: trigger index INTx");
    dev_raise(d, &intx);
    check(intx && take(ei) == 1, "B5 raise: ASSERT (INTx eventfd signalled once)");
    dev_raise(d, &intx);
    check(take(ei) == 0, "B6 raise while asserted: no second edge");
    check(UNMASK() == 0 && take(ei) == 1, "B7 client EOI + UNMASK, no handler read yet: re-ASSERT");
    (void)rd(0, 0x1018, 4);
    check(UNMASK() == 0 && take(ei) == 0, "B8 handler read 0x1018, UNMASK, no raise since: line stays down");

    dev_raise(d, &intx);
    dev_raise(d, &intx);
    (void)rd(0, 0x1014, 4);
    check(take(ei) == 1 && UNMASK() == 0 && take(ei) == 0,
          "B9 two raises, handler read 0x1014 after both, UNMASK: deasserted (no spurious re-assert)");

    dev_raise(d, &intx);
    (void)rd(0, 0x1018, 4);
    dev_raise(d, &intx);                                   /* after the handler's read */
    check(take(ei) == 1 && UNMASK() == 0 && take(ei) == 1, "B10 raise after the handler's read: re-ASSERT after UNMASK (no loss)");
    (void)rd(0, 0x1018, 4);
    check(UNMASK() == 0 && take(ei) == 0, "B11 read and UNMASK: down");

    (void)rd(0, 0x1250, 4);                               /* the mapper handler runs (RING_RD) */
    dev_raise(d, &intx);
    dev_raise(d, &intx);
    (void)rd(0, 0x1018, 4);
    check(take(ei) == 1 && UNMASK() == 0 && take(ei) == 1,
          "B12 mapper active, only the GPU read after the raises: re-ASSERT");
    (void)rd(0, 0x1018, 4);
    (void)rd(0, 0x1250, 4);
    check(UNMASK() == 0 && take(ei) == 0, "B13 both handlers read: down");

    check(MASK() == 0, "B14 client MASK");
    dev_raise(d, &intx);
    check(take(ei) == 0 && UNMASK() == 0 && take(ei) == 1, "B15 raise while masked: no edge; UNMASK: ASSERT");
    (void)rd(0, 0x1018, 4);
    (void)rd(0, 0x1250, 4);
    check(UNMASK() == 0 && UNMASK() == 0 && UNMASK() == 0 && take(ei) == 0, "B16 three UNMASKs without a raise: nothing (no storm)");

    {
        int rfd = eventfd(0, EFD_CLOEXEC);
        int r1 = set_irqs(VFIO_PCI_INTX_IRQ_INDEX, VFIO_IRQ_SET_ACTION_UNMASK, VFIO_IRQ_SET_DATA_EVENTFD, rfd);
        int e1 = errno;
        int r2 = set_irqs(VFIO_PCI_INTX_IRQ_INDEX, VFIO_IRQ_SET_ACTION_MASK, VFIO_IRQ_SET_DATA_EVENTFD, rfd);
        int e2 = errno;
        check(r1 < 0 && e1 == EINVAL && r2 < 0 && e2 == EINVAL,
              "B17 resamplefd UNMASK/MASK with DATA_EVENTFD refused (EINVAL): the KVM client keeps the userspace INTx path");
        close(rfd);
        dev_raise(d, &intx);
        check(take(ei) == 1, "B18 INTx still works after the refusal");
        (void)rd(0, 0x1018, 4);
        (void)rd(0, 0x1250, 4);
        (void)UNMASK();
    }

    check(TRIGGER(VFIO_PCI_INTX_IRQ_INDEX, -1) == 0 && vfu_irq_trigger_index(d->ctx) == -1 &&
          TRIGGER(VFIO_PCI_MSI_IRQ_INDEX, em) == 0 && vfu_irq_trigger_index(d->ctx) == VFU_DEV_MSI_IRQ,
          "B19 guest enables MSI (client disables INTx, sets the MSI eventfd): trigger index MSI");
    dev_raise(d, &intx);
    dev_raise(d, &intx);
    dev_raise(d, &intx);
    check(!intx && take(em) == 3 && take(ei) == 0, "B20 MSI preferred: three raises = three messages, no INTx");

    check(TRIGGER(VFIO_PCI_MSI_IRQ_INDEX, -1) == 0 && TRIGGER(VFIO_PCI_INTX_IRQ_INDEX, ei) == 0 &&
          vfu_irq_trigger_index(d->ctx) == VFU_DEV_INTX_IRQ, "B21 back to INTx");
    dev_raise(d, &intx);
    check(intx && take(ei) == 1, "B22 INTx state started again (line down, unmasked): ASSERT");
    close(ei);
    close(em);
    stop_dev(d);
}

static void part_b_modes(const char *dir)
{
    uint8_t cfg[256];
    Dev *d;
    int efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    bool intx = false;

    printf("== B: irq=intx and irq=msi\n");
    d = start_dev(PVG_IRQ_MODE_INTX, dir, "intx-only.sock");
    rd_cfg(cfg);
    check(cfg[0x3d] == 1 && !has_msi_cap(cfg), "B23 irq=intx config: INTA#, no MSI capability (the guest must use INTx)");
    check(TRIGGER(VFIO_PCI_MSI_IRQ_INDEX, efd) < 0, "B24 irq=intx: the client cannot set an MSI eventfd");
    check(TRIGGER(VFIO_PCI_INTX_IRQ_INDEX, efd) == 0, "B25 irq=intx: INTx eventfd");
    dev_raise(d, &intx);
    check(intx && take(efd) == 1, "B26 irq=intx: ASSERT");
    stop_dev(d);

    d = start_dev(PVG_IRQ_MODE_MSI, dir, "intx-msi.sock");
    rd_cfg(cfg);
    check(cfg[0x3d] == 0 && has_msi_cap(cfg), "B27 irq=msi (default) config: unchanged, no INTx pin, MSI capability");
    check(TRIGGER(VFIO_PCI_INTX_IRQ_INDEX, efd) < 0, "B28 irq=msi: no INTx eventfd");
    check(TRIGGER(VFIO_PCI_MSI_IRQ_INDEX, efd) == 0, "B29 irq=msi: MSI eventfd");
    (void)rd(0, 0x1018, 4);
    dev_raise(d, &intx);
    dev_raise(d, &intx);
    check(!intx && take(efd) == 2, "B30 irq=msi: one message per raise, no INTx state");
    stop_dev(d);
    close(efd);
}

int main(int argc, char **argv)
{
    char dir[] = "/tmp/pvg-intx-test.XXXXXX";

    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 2;
    }
    part_a();
    part_b_auto(argc > 1 ? argv[1] : dir);
    part_b_modes(argc > 1 ? argv[1] : dir);
    rmdir(dir);
    printf("pvg_intx_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
