/*
 * qdev-host: guard for QEMU's vhost-user-gpu (LD_PRELOAD)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Works around two faults of QEMU's contrib/vhost-user-gpu from outside its
 * code.  It relies only on virglrenderer's public API, the vhost-user-gpu
 * protocol (QEMU docs/interop/vhost-user-gpu.rst: every message starts with
 * {u32 request, u32 flags, u32 size}; the backend's requests have flags 0,
 * replies VHOST_USER_GPU_MSG_FLAG_REPLY) and the vhost-user front-end message
 * VHOST_USER_GPU_SET_SOCKET (docs/interop/vhost-user.rst, id 33, one fd).
 *
 * 1. A fence during a display info or EDID reply.  vhost-user-gpu keeps
 *    GET_DISPLAY_INFO and GET_EDID on its fence queue while it waits for the
 *    reply and takes the command back as the last entry of that queue.  Its
 *    virgl fence callback completes every queued command whose fence id is not
 *    above the signalled fence, these included (fence id 0), so a fence
 *    signalled during the round trip frees the waiting command and the reply
 *    handler dereferences NULL ("segfault at 28" in get_edid_cb).  Fence
 *    notifications are held while such a reply is outstanding and replayed,
 *    in order, once its handler has returned.
 *
 * 2. A new GPU socket after a stop.  QEMU closes its end of the GPU socket
 *    whenever it stops the device (the VM is paused, the guest resets the
 *    device, e.g. on reboot) and sends a new socket when it starts it again.
 *    vhost-user-gpu takes a socket only while it has none and drops the old
 *    one only after a failed read or write, so it refuses the new socket
 *    ("assertion 'g->sock_fd == -1' failed"), sends into the dead one, and a
 *    request waiting for a reply then blocks its control queue for good or,
 *    at the next start, aborts the process.  Here the new socket is put under
 *    the backend's descriptor number (dup2).  While QEMU's end is gone, that
 *    number points to a socket of this library, which drops the backend's
 *    messages and answers its requests (empty display info or EDID, OK for a
 *    dmabuf update); a request still unanswered when the new socket arrives
 *    is sent again on it.
 *
 * Not covered: commands still in flight when the guest resets the device are
 * completed later into its new queue, and virglrenderer keeps the contexts
 * and resources of the previous guest driver; vhost-user-gpu does not drop or
 * reset them, and neither does this library.
 *
 * Environment, for tests:
 *   QDEV_VUGPU_FENCE_HOLD=0             do not hold fences
 *   QDEV_VUGPU_TEST_FENCE_IN_REPLY=1    report a fence while every display
 *                                       info/EDID reply is on its way;
 *                                       =edid or =info: only for that request
 *   QDEV_VUGPU_SOCKET_GUARD=0           leave the GPU socket alone
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <glib.h>
#include <linux/virtio_gpu.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <virgl/virglrenderer.h>

enum {
    GPU_GET_PROTOCOL_FEATURES = 1,
    GPU_GET_DISPLAY_INFO = 3,
    GPU_DMABUF_UPDATE = 10,
    GPU_GET_EDID = 11,
    GPU_HDR_SIZE = 12,
    GPU_FLAG_REPLY = 0x4,
    GPU_PROTOCOL_F_EDID = 0,
    GPU_PROTOCOL_F_DMABUF2 = 1,
};
enum { VHOST_USER_GPU_SET_SOCKET = 33, VHOST_USER_VERSION_MASK = 0x3, VHOST_USER_VERSION = 0x1 };

static int (*real_renderer_init)(void *, int, struct virgl_renderer_callbacks *);
static ssize_t (*real_sendmsg)(int, const struct msghdr *, int);
static ssize_t (*real_recvmsg)(int, struct msghdr *, int);
static ssize_t (*real_read)(int, void *, size_t);
static int (*real_close)(int);

static struct virgl_renderer_callbacks callbacks;   /* the caller's, with our write_fence */
static void (*real_write_fence)(void *, uint32_t);
static void *renderer_cookie;

static int hold_enabled = 1, socket_guard = 1;
static int test_fence_in_reply, test_request;

/*
 * All GPU socket traffic runs on vhost-user-gpu's main loop thread; read() and
 * close() also run on other threads for other descriptors, hence the atomics.
 */
static int gpu_fd = -1;            /* the backend's GPU socket descriptor */
static int stand_in_peer = -1;     /* our end of the stand-in socket while QEMU's is gone */
static int pending;                /* request whose reply has not been read yet, 0 if none */
static unsigned char pending_msg[64];
static size_t pending_len;
static unsigned long sockets_replaced;

static GArray *held;               /* fences that came in while a reply was awaited */
static uint32_t last_fence;
static unsigned long holds;

static void log_line(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void log_line(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("qdev-host vugpu guard: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

#define LOAD(v) __atomic_load_n(&(v), __ATOMIC_ACQUIRE)
#define STORE(v, x) __atomic_store_n(&(v), (x), __ATOMIC_RELEASE)

static void resolve(void)
{
    if (!real_sendmsg) { real_sendmsg = dlsym(RTLD_NEXT, "sendmsg"); }
    if (!real_recvmsg) { real_recvmsg = dlsym(RTLD_NEXT, "recvmsg"); }
    if (!real_read) { real_read = dlsym(RTLD_NEXT, "read"); }
    if (!real_close) { real_close = dlsym(RTLD_NEXT, "close"); }
    if (!real_renderer_init) { real_renderer_init = dlsym(RTLD_NEXT, "virgl_renderer_init"); }
}

static int env_is(const char *name, const char *value)
{
    const char *v = getenv(name);
    return v && strcmp(v, value) == 0;
}

__attribute__((constructor)) static void setup(void)
{
    resolve();
    hold_enabled = !env_is("QDEV_VUGPU_FENCE_HOLD", "0");
    socket_guard = !env_is("QDEV_VUGPU_SOCKET_GUARD", "0");
    test_fence_in_reply = env_is("QDEV_VUGPU_TEST_FENCE_IN_REPLY", "1") ||
                          env_is("QDEV_VUGPU_TEST_FENCE_IN_REPLY", "edid") ||
                          env_is("QDEV_VUGPU_TEST_FENCE_IN_REPLY", "info");
    test_request = env_is("QDEV_VUGPU_TEST_FENCE_IN_REPLY", "edid") ? GPU_GET_EDID :
                   env_is("QDEV_VUGPU_TEST_FENCE_IN_REPLY", "info") ? GPU_GET_DISPLAY_INFO : 0;
    held = g_array_new(FALSE, FALSE, sizeof(uint32_t));
    log_line("loaded (fence hold %s, socket guard %s%s)", hold_enabled ? "on" : "OFF",
             socket_guard ? "on" : "OFF", test_fence_in_reply ? ", test fences" : "");
}

static const char *request_name(int request)
{
    switch (request) {
    case GPU_GET_PROTOCOL_FEATURES: return "GET_PROTOCOL_FEATURES";
    case GPU_GET_DISPLAY_INFO: return "GET_DISPLAY_INFO";
    case GPU_DMABUF_UPDATE: return "DMABUF_UPDATE";
    case GPU_GET_EDID: return "GET_EDID";
    default: return "?";
    }
}

static int expects_reply(uint32_t request)
{
    return request == GPU_GET_PROTOCOL_FEATURES || request == GPU_GET_DISPLAY_INFO ||
           request == GPU_DMABUF_UPDATE || request == GPU_GET_EDID;
}

static int waits_on_fence_queue(int request)
{
    return request == GPU_GET_DISPLAY_INFO || request == GPU_GET_EDID;
}

/* ------------------------------------------------------------ 1. fence hold */

static void deliver(void *cookie, uint32_t fence)
{
    last_fence = fence;
    real_write_fence(cookie, fence);
}

static void hold_write_fence(void *cookie, uint32_t fence)
{
    renderer_cookie = cookie;
    if (hold_enabled && waits_on_fence_queue(LOAD(pending))) {
        g_array_append_val(held, fence);
        return;
    }
    deliver(cookie, fence);
}

/* Runs after the reply handler has returned and taken its command off the queue. */
static gboolean release_held(gpointer data)
{
    int request = GPOINTER_TO_INT(data);
    guint i, count = held->len;

    if (!count || waits_on_fence_queue(LOAD(pending))) {
        return G_SOURCE_REMOVE;  /* the next reply's handler schedules this again */
    }
    holds++;
    if (holds <= 20 || test_fence_in_reply) {
        log_line("%u fence(s) held during a %s reply, delivered now (%lu so far)",
                 count, request_name(request), holds);
    }
    for (i = 0; i < count; i++) {
        deliver(renderer_cookie, g_array_index(held, uint32_t, i));
    }
    g_array_set_size(held, 0);
    return G_SOURCE_REMOVE;
}

/* Test only: a fence reported while the reply is on its way, before its handler runs. */
static gboolean report_test_fence(gpointer data)
{
    int request = GPOINTER_TO_INT(data);

    if (LOAD(pending) == request && real_write_fence) {
        log_line("test: fence %u reported while a %s reply is on its way (%s)",
                 last_fence, request_name(request), hold_enabled ? "held" : "passed through");
        hold_write_fence(renderer_cookie, last_fence);
    }
    return G_SOURCE_REMOVE;
}

int virgl_renderer_init(void *cookie, int flags, struct virgl_renderer_callbacks *cbs)
{
    resolve();
    if (!cbs || !cbs->write_fence || (flags & VIRGL_RENDERER_ASYNC_FENCE_CB)) {
        return real_renderer_init(cookie, flags, cbs);  /* callbacks off the main loop: leave them */
    }
    callbacks = *cbs;
    real_write_fence = cbs->write_fence;
    callbacks.write_fence = hold_write_fence;
    renderer_cookie = cookie;
    return real_renderer_init(cookie, flags, &callbacks);
}

/* ------------------------------------------------------- 2. socket guard */

static void remember_request(const struct msghdr *msg, uint32_t request)
{
    size_t i, len = 0;

    for (i = 0; i < msg->msg_iovlen && len < sizeof(pending_msg); i++) {
        size_t n = MIN(msg->msg_iov[i].iov_len, sizeof(pending_msg) - len);
        memcpy(pending_msg + len, msg->msg_iov[i].iov_base, n);
        len += n;
    }
    pending_len = len;
    STORE(pending, (int)request);
}

/* The reply QEMU would give while it is away: empty, or OK. */
static void answer_for_qemu(int request)
{
    union {
        unsigned char bytes[GPU_HDR_SIZE + sizeof(struct virtio_gpu_resp_edid)];
        uint32_t words[3];
    } buf;
    size_t size = 0;

    memset(&buf, 0, sizeof(buf));
    switch (request) {
    case GPU_GET_PROTOCOL_FEATURES: {
        uint64_t features = (1u << GPU_PROTOCOL_F_EDID) | (1u << GPU_PROTOCOL_F_DMABUF2);
        size = sizeof(features);
        memcpy(buf.bytes + GPU_HDR_SIZE, &features, size);
        break;
    }
    case GPU_GET_DISPLAY_INFO: {
        struct virtio_gpu_resp_display_info info = { .hdr.type = VIRTIO_GPU_RESP_OK_DISPLAY_INFO };
        size = sizeof(info);
        memcpy(buf.bytes + GPU_HDR_SIZE, &info, size);
        break;
    }
    case GPU_GET_EDID: {
        struct virtio_gpu_resp_edid edid = { .hdr.type = VIRTIO_GPU_RESP_OK_EDID };
        size = sizeof(edid);
        memcpy(buf.bytes + GPU_HDR_SIZE, &edid, size);
        break;
    }
    case GPU_DMABUF_UPDATE:
        break;
    default:
        return;
    }
    buf.words[0] = (uint32_t)request;
    buf.words[1] = GPU_FLAG_REPLY;
    buf.words[2] = (uint32_t)size;
    if (send(stand_in_peer, buf.bytes, GPU_HDR_SIZE + size, MSG_NOSIGNAL) < 0) {
        log_line("could not answer %s for QEMU: %s", request_name(request), strerror(errno));
    }
}

/* QEMU's end of the GPU socket is gone: put a socket of ours under the backend's number. */
static void stand_in(const char *why)
{
    int sv[2], fd = LOAD(gpu_fd);

    if (stand_in_peer >= 0 || fd < 0) {
        return;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0 || dup2(sv[0], fd) < 0) {
        log_line("no stand-in socket: %s", strerror(errno));
        return;
    }
    real_close(sv[0]);
    stand_in_peer = sv[1];
    log_line("QEMU closed the GPU socket (%s); answering for it until it sends a new one", why);
}

/* VHOST_USER_GPU_SET_SOCKET arrived with descriptor newfd. */
static void take_socket(int newfd)
{
    int fd = LOAD(gpu_fd);
    int request = LOAD(pending);

    if (fd < 0) {
        STORE(gpu_fd, newfd);  /* the backend has none and takes this one */
        return;
    }
    /* The backend still holds the old one and refuses this: put it under its number. */
    if (dup2(newfd, fd) < 0) {
        log_line("could not take the new GPU socket: %s", strerror(errno));
        return;
    }
    real_close(newfd);
    if (stand_in_peer >= 0) {
        real_close(stand_in_peer);
        stand_in_peer = -1;
    }
    sockets_replaced++;
    log_line("QEMU sent a new GPU socket; it replaces the old one under descriptor %d "
             "(the assertion about sock_fd that vhost-user-gpu logs next is expected)%s%s (%lu so far)",
             fd, request ? ", sent again: " : "", request ? request_name(request) : "", sockets_replaced);
    if (request && send(fd, pending_msg, pending_len, MSG_NOSIGNAL) < 0) {
        log_line("could not send %s again: %s", request_name(request), strerror(errno));
    }
}

ssize_t recvmsg(int fd, struct msghdr *msg, int flags)
{
    ssize_t ret;
    uint32_t hdr[3];
    struct cmsghdr *cmsg;

    resolve();
    ret = real_recvmsg(fd, msg, flags);
    if (!socket_guard || ret < GPU_HDR_SIZE || !msg || msg->msg_iovlen < 1 ||
        msg->msg_iov[0].iov_len < GPU_HDR_SIZE) {
        return ret;
    }
    memcpy(hdr, msg->msg_iov[0].iov_base, sizeof(hdr));
    if (hdr[0] != VHOST_USER_GPU_SET_SOCKET ||
        (hdr[1] & VHOST_USER_VERSION_MASK) != VHOST_USER_VERSION) {
        return ret;
    }
    for (cmsg = CMSG_FIRSTHDR(msg); cmsg; cmsg = CMSG_NXTHDR(msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
            cmsg->cmsg_len == CMSG_LEN(sizeof(int))) {
            int newfd;
            memcpy(&newfd, CMSG_DATA(cmsg), sizeof(newfd));
            take_socket(newfd);
            break;
        }
    }
    return ret;
}

ssize_t sendmsg(int fd, const struct msghdr *msg, int flags)
{
    ssize_t ret;
    size_t i, len = 0;
    uint32_t hdr[3] = { 0, 0, 0 };
    int request = 0;

    resolve();
    if (fd < 0 || !msg || msg->msg_iovlen < 1 || msg->msg_iov[0].iov_len < GPU_HDR_SIZE) {
        return real_sendmsg(fd, msg, flags);
    }
    memcpy(hdr, msg->msg_iov[0].iov_base, sizeof(hdr));
    for (i = 0; i < msg->msg_iovlen; i++) {
        len += msg->msg_iov[i].iov_len;
    }
    if (LOAD(gpu_fd) < 0 && hdr[0] == GPU_GET_PROTOCOL_FEATURES && hdr[1] == 0 && hdr[2] == 0) {
        STORE(gpu_fd, fd);  /* the SET_SOCKET was not seen: learn the socket from its first request */
    }
    if (fd != LOAD(gpu_fd)) {
        return real_sendmsg(fd, msg, flags);
    }
    if (hdr[1] == 0 && expects_reply(hdr[0])) {
        request = (int)hdr[0];
    }
    if (socket_guard && stand_in_peer >= 0) {
        if (request) {               /* QEMU is away: answer for it, drop the rest */
            remember_request(msg, hdr[0]);
            answer_for_qemu(request);
        }
        return (ssize_t)len;
    }
    ret = real_sendmsg(fd, msg, socket_guard ? flags | MSG_NOSIGNAL : flags);
    if (socket_guard && ret < 0 && (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN)) {
        stand_in("a send failed");
        if (stand_in_peer >= 0) {
            if (request) {
                remember_request(msg, hdr[0]);
                answer_for_qemu(request);
            }
            return (ssize_t)len;
        }
        return ret;
    }
    if (ret >= GPU_HDR_SIZE && request) {
        remember_request(msg, hdr[0]);
        if (test_fence_in_reply && waits_on_fence_queue(request) &&
            (!test_request || test_request == request)) {
            /* above the reply's fd source, so it runs first */
            g_idle_add_full(G_PRIORITY_HIGH, report_test_fence, GINT_TO_POINTER(request), NULL);
        }
    }
    return ret;
}

ssize_t read(int fd, void *buf, size_t count)
{
    ssize_t ret;
    int request;

    resolve();
    if (fd < 0 || fd != LOAD(gpu_fd)) {
        return real_read(fd, buf, count);
    }
    ret = real_read(fd, buf, count);
    request = LOAD(pending);
    if (socket_guard && request && stand_in_peer < 0 &&
        (ret == 0 || (ret < 0 && errno == ECONNRESET))) {
        stand_in("end of file while a reply was awaited");  /* answer for QEMU */
        if (stand_in_peer >= 0) {
            answer_for_qemu(request);
            ret = real_read(fd, buf, count);
        }
    }
    if (ret > 0 && request) {
        /* the reply handler is running now; release held fences once it has returned */
        STORE(pending, 0);
        if (waits_on_fence_queue(request) && held && held->len) {
            g_idle_add_full(G_PRIORITY_DEFAULT, release_held, GINT_TO_POINTER(request), NULL);
        }
    }
    return ret;
}

int close(int fd)
{
    resolve();
    if (fd >= 0 && fd == LOAD(gpu_fd)) {
        STORE(gpu_fd, -1);  /* the backend dropped its socket and will take the next one */
        STORE(pending, 0);
        if (stand_in_peer >= 0) {
            real_close(stand_in_peer);
            stand_in_peer = -1;
        }
    }
    return real_close(fd);
}
