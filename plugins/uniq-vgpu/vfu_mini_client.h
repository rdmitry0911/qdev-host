/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * vfu_mini_client.h: a minimal in-process vfio-user client for native plugin tests (pvg_intx_test,
 * apple-videotoolbox test_vt_identity).  It speaks the protocol itself (vfio-user.h): VERSION, region read, config
 * read, SET_IRQS.  Test code only; one connection per process (the static `sock`).
 */
#ifndef VFU_MINI_CLIENT_H
#define VFU_MINI_CLIENT_H
#include "vfio-user.h"
#include "libvfio-user.h"
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int sock __attribute__((unused)) = -1;
static uint16_t msg_id __attribute__((unused));

static inline void recv_all(void *buf, size_t len)
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
static inline int xfer(uint16_t cmd, const void *req, size_t req_len, const int *fds, int nfds, void *reply, size_t reply_max)
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
static inline void negotiate(void)
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
static inline int set_irqs(uint32_t index, uint32_t action, uint32_t data, int fd)
{
    struct vfio_irq_set s = { .argsz = sizeof(s), .flags = action | data, .index = index, .start = 0,
                              .count = (data == VFIO_IRQ_SET_DATA_NONE && action == VFIO_IRQ_SET_ACTION_TRIGGER && fd < 0) ? 0 : 1 };

    return xfer(VFIO_USER_DEVICE_SET_IRQS, &s, sizeof(s), fd >= 0 ? &fd : NULL, fd >= 0 ? 1 : 0, NULL, 0);
}
#define TRIGGER(index, fd) set_irqs(index, VFIO_IRQ_SET_ACTION_TRIGGER, (fd) >= 0 ? VFIO_IRQ_SET_DATA_EVENTFD : VFIO_IRQ_SET_DATA_NONE, fd)
#define UNMASK() set_irqs(VFIO_PCI_INTX_IRQ_INDEX, VFIO_IRQ_SET_ACTION_UNMASK, VFIO_IRQ_SET_DATA_NONE, -1)
#define MASK() set_irqs(VFIO_PCI_INTX_IRQ_INDEX, VFIO_IRQ_SET_ACTION_MASK, VFIO_IRQ_SET_DATA_NONE, -1)

static inline uint32_t rd(int region, uint64_t off, uint32_t len)
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
static inline void rd_cfg(uint8_t *cfg)
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
static inline uint64_t take(int efd)                        /* eventfd count since the last take (0 = not signalled) */
{
    uint64_t v = 0;

    if (read(efd, &v, sizeof(v)) != sizeof(v)) {
        return 0;
    }
    return v;
}


static inline void vfu_mini_connect(const char *path)
{
    struct sockaddr_un sa = { .sun_family = AF_UNIX };

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "client: connect %s: %s\n", path, strerror(errno));
        exit(2);
    }
    msg_id = 0;
    negotiate();
}

static inline bool vfu_mini_has_cap(const uint8_t *c, uint8_t id)
{
    for (int p = (c[6] & 0x10) ? c[0x34] : 0, n = 0; p && n < 48; p = c[p + 1], n++) {
        if (c[p] == id) {
            return true;
        }
    }
    return false;
}
#endif
