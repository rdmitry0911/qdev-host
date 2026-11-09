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
 * 3. QEMU's side of a vhost-user-vga (hw/display/vhost-user-gpu.c, QEMU 11)
 *    has two faults this process cannot reach.  After a device reset the
 *    console stays in the GPU's dma-buf scanout, so the next refresh asked for
 *    by a display client reads a NULL surface in vga_update_display() and QEMU
 *    crashes.  And a display client's GL update acknowledged while the GPU
 *    socket is closed makes it install a handler on descriptor -1, which QEMU's
 *    io_uring main loop then runs forever ("failed to read msg header").  Both
 *    need a display client attached while the GPU is stopped.  The library
 *    therefore publishes the GPU's state next to its socket, in gpu-state:
 *      state=running seq=N    the socket is up and the guest has a scanout
 *      state=stopping seq=N   QEMU stops the device (pause, reset, shutdown)
 *      state=stopped seq=N
 *    On a stop it waits, while QEMU waits for this reply and cannot run its
 *    main loop, up to 1.5 s for the display client named in gpu-state.client
 *    (pid=P) to write seq=N to gpu-state.ack once it has closed its display
 *    connection (qsm-pve-direct from 0.4.0+stock4.14~exp236 does).  A display
 *    client attaches only in "running".  A scanout the guest switches off also
 *    gets a blank surface (SCANOUT 0x0), as QEMU's own virtio-gpu does, so a
 *    later reset finds a surface.
 *
 * 4. 2D buffers through GBM.  Without VirGL (the firmware, OVMF's virtio-gpu
 *    driver, before the guest's own driver) vhost-user-gpu allocates 2D
 *    resources as linear GBM buffers on the render node.  NVIDIA's GBM refuses
 *    them and libgbm then dereferences NULL, so the process dies at the first
 *    firmware frame.  vhost-user-gpu's own fallback keeps 2D resources in
 *    shared memory and sends their pixels as SCANOUT/UPDATE messages; it is
 *    taken when its gbm_create_device() call returns NULL, which this library
 *    does for calls from the vhost-user-gpu binary itself.  virglrenderer's
 *    own GBM device, which renders 3D, is left alone.
 *
 * Not covered: commands still in flight when the guest resets the device are
 * completed later into its new queue, and virglrenderer keeps the contexts
 * and resources of the previous guest driver; vhost-user-gpu does not drop or
 * reset them, and neither does this library.  A GL update of the display
 * client that is still unanswered at the moment of a stop fails when the
 * client disconnects and still triggers QEMU's second fault.
 *
 * Environment, for tests:
 *   QDEV_VUGPU_FENCE_HOLD=0             do not hold fences
 *   QDEV_VUGPU_TEST_FENCE_IN_REPLY=1    report a fence while every display
 *                                       info/EDID reply is on its way;
 *                                       =edid or =info: only for that request
 *   QDEV_VUGPU_SOCKET_GUARD=0           leave the GPU socket alone
 *   QDEV_VUGPU_DISPLAY_HANDSHAKE=0      publish the state, do not wait on a stop
 *   QDEV_VUGPU_2D_GBM=1                 keep GBM buffers for 2D resources
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <link.h>
#include <glib.h>
#include <linux/virtio_gpu.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include <virgl/virglrenderer.h>

enum {
    GPU_GET_PROTOCOL_FEATURES = 1,
    GPU_GET_DISPLAY_INFO = 3,
    GPU_SCANOUT = 7,
    GPU_DMABUF_SCANOUT = 9,
    GPU_DMABUF_UPDATE = 10,
    GPU_DMABUF_SCANOUT2 = 12,
    GPU_GET_EDID = 11,
    GPU_HDR_SIZE = 12,
    GPU_FLAG_REPLY = 0x4,
    GPU_PROTOCOL_F_EDID = 0,
    GPU_PROTOCOL_F_DMABUF2 = 1,
};
enum { VHOST_USER_SET_VRING_BASE = 10, VHOST_USER_GET_VRING_BASE = 11, VHOST_USER_GPU_SET_SOCKET = 33,
       VHOST_USER_VERSION_MASK = 0x3, VHOST_USER_VERSION = 0x1 };

static int (*real_renderer_init)(void *, int, struct virgl_renderer_callbacks *);
static ssize_t (*real_sendmsg)(int, const struct msghdr *, int);
static ssize_t (*real_recvmsg)(int, struct msghdr *, int);
static ssize_t (*real_read)(int, void *, size_t);
static int (*real_close)(int);

static struct virgl_renderer_callbacks callbacks;   /* the caller's, with our write_fence */
static void (*real_write_fence)(void *, uint32_t);
static void *renderer_cookie;

static int hold_enabled = 1, socket_guard = 1, two_d_gbm;
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

static char state_dir[PATH_MAX];   /* directory of the vhost-user socket: gpu-state lives there */
static const char *gpu_state = "";
static unsigned long state_seq;
static int display_handshake = 1;
static int scanout_active;         /* the guest has a scanout on, as far as QEMU was told */
static int vring_base_fd = -1;     /* vhost-user socket whose SET_VRING_BASE payload comes next */

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

/* The directory of vhost-user-gpu's --socket-path (or -s): gpu-state is published there. */
static void find_state_dir(void)
{
    char args[8192];
    ssize_t n;
    int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    const char *path = NULL;

    if (fd < 0) {
        return;
    }
    n = real_read(fd, args, sizeof(args) - 1);
    real_close(fd);
    if (n <= 0) {
        return;
    }
    args[n] = '\0';
    for (char *a = args; a < args + n; a += strlen(a) + 1) {
        if (strncmp(a, "--socket-path=", 14) == 0) {
            path = a + 14;
        } else if ((strcmp(a, "--socket-path") == 0 || strcmp(a, "-s") == 0) && a + strlen(a) + 1 < args + n) {
            path = a + strlen(a) + 1;
        }
    }
    if (path && strrchr(path, '/') && (size_t)(strrchr(path, '/') - path) < sizeof(state_dir)) {
        memcpy(state_dir, path, (size_t)(strrchr(path, '/') - path));
        state_dir[strrchr(path, '/') - path] = '\0';
    }
}

static void write_state(const char *state)
{
    char path[PATH_MAX + 32], tmp[PATH_MAX + 32], line[64];
    int fd, len;

    gpu_state = state;
    if (!state_dir[0]) {
        return;
    }
    snprintf(path, sizeof(path), "%s/gpu-state", state_dir);
    snprintf(tmp, sizeof(tmp), "%s/gpu-state.tmp", state_dir);
    len = snprintf(line, sizeof(line), "state=%s seq=%lu\n", state, state_seq);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);  /* readers accept root-only files */
    if (fd >= 0) {
        fchmod(fd, 0600);
    }
    if (fd < 0) {
        return;
    }
    if (write(fd, line, (size_t)len) != len || rename(tmp, path) < 0) {
        log_line("could not publish the GPU state in %s: %s", state_dir, strerror(errno));
    }
    real_close(fd);
}

/* A small key=value file of the display client: the value of key, or -1. */
static long read_number(const char *name, const char *key)
{
    char path[PATH_MAX + 32], buf[128], *at;
    ssize_t n;
    int fd;

    snprintf(path, sizeof(path), "%s/%s", state_dir, name);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    n = real_read(fd, buf, sizeof(buf) - 1);
    real_close(fd);
    if (n <= 0) {
        return -1;
    }
    buf[n] = '\0';
    at = strstr(buf, key);
    return at ? strtol(at + strlen(key), NULL, 10) : -1;
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/*
 * QEMU stops the device (VHOST_USER_GET_VRING_BASE) and waits for this backend:
 * let the display client disconnect before QEMU's main loop runs again.
 */
static void display_stop(void)
{
    long client, start = now_ms(), acked = -1;

    if (strcmp(gpu_state, "running") != 0) {
        return;
    }
    state_seq++;
    write_state("stopping");
    client = read_number("gpu-state.client", "pid=");
    if (display_handshake && client > 0 && kill((pid_t)client, 0) == 0) {
        while (now_ms() - start < 1500) {
            if (read_number("gpu-state.ack", "seq=") == (long)state_seq) {
                acked = now_ms() - start;
                break;
            }
            usleep(5000);
        }
        usleep(30000);  /* QEMU's D-Bus thread sees the closed connection meanwhile */
    }
    write_state("stopped");
    if (client > 0) {
        if (acked >= 0) {
            log_line("QEMU stops the GPU; the display client disconnected in %ld ms", acked);
        } else {
            log_line("QEMU stops the GPU; the display client (pid %ld) did not confirm in 1.5 s", client);
        }
    }
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
    display_handshake = !env_is("QDEV_VUGPU_DISPLAY_HANDSHAKE", "0");
    two_d_gbm = env_is("QDEV_VUGPU_2D_GBM", "1");
    held = g_array_new(FALSE, FALSE, sizeof(uint32_t));
    find_state_dir();
    write_state("stopped");
    log_line("loaded (fence hold %s, socket guard %s, display handshake %s%s%s%s)",
             hold_enabled ? "on" : "OFF", socket_guard ? "on" : "OFF",
             display_handshake && state_dir[0] ? "on" : "OFF",
             state_dir[0] ? ", state in " : "", state_dir, test_fence_in_reply ? ", test fences" : "");
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
    if ((hdr[1] & VHOST_USER_VERSION_MASK) != VHOST_USER_VERSION) {
        return ret;
    }
    if (hdr[0] == VHOST_USER_GET_VRING_BASE) {
        display_stop();
        return ret;
    }
    if (hdr[0] == VHOST_USER_SET_VRING_BASE && hdr[2] >= 8) {
        vring_base_fd = fd;  /* its {index, num} payload is read next */
        return ret;
    }
    if (hdr[0] != VHOST_USER_GPU_SET_SOCKET) {
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
    if (ret >= GPU_HDR_SIZE && hdr[1] == 0) {
        int scanout = hdr[0] == GPU_DMABUF_SCANOUT || hdr[0] == GPU_DMABUF_SCANOUT2;
        int has_fd = msg->msg_controllen > 0;

        if ((scanout && has_fd) || (hdr[0] == GPU_SCANOUT && msg->msg_iov[0].iov_len >= GPU_HDR_SIZE + 12 &&
             ((const uint32_t *)msg->msg_iov[0].iov_base)[4] > 0)) {
            scanout_active = 1;
            if (strcmp(gpu_state, "running") != 0) {
                write_state("running");
            }
        } else if (scanout) {
            scanout_active = 0;
        }
        if (scanout && !has_fd && msg->msg_iov[0].iov_len >= GPU_HDR_SIZE + 4) {
            /* the guest switched the scanout off: give the console a blank surface */
            uint32_t blank[6] = { GPU_SCANOUT, 0, 12, ((const uint32_t *)msg->msg_iov[0].iov_base)[3], 0, 0 };
            if (send(fd, blank, sizeof(blank), MSG_NOSIGNAL) < 0) {
                log_line("could not blank the switched-off scanout: %s", strerror(errno));
            }
        }
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

/*
 * The device starts again.  Ring 0 resumes at its old index after a pause (the
 * guest's scanout is still on, QEMU kept it), and starts at 0 after a reset (wait
 * for the guest's next scanout).
 */
static void display_start(uint32_t index, uint32_t num)
{
    if (index == 0 && num != 0 && scanout_active && strcmp(gpu_state, "stopped") == 0) {
        write_state("running");
    }
}

ssize_t read(int fd, void *buf, size_t count)
{
    ssize_t ret;
    int request;

    resolve();
    if (fd >= 0 && fd == vring_base_fd) {
        ret = real_read(fd, buf, count);
        vring_base_fd = -1;
        if (ret >= 8) {
            uint32_t state[2];
            memcpy(state, buf, sizeof(state));
            display_start(state[0], state[1]);
        }
        return ret;
    }
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

/* ------------------------------------------------- 4. 2D buffers without GBM */

struct gbm_device;
static struct gbm_device *(*real_gbm_create_device)(int);

struct main_range {
    uintptr_t addr;
    int inside;
};

static int find_in_main_program(struct dl_phdr_info *info, size_t size, void *data)
{
    struct main_range *range = data;
    (void)size;
    /* the first object reported is the program itself */
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        uintptr_t start = info->dlpi_addr + ph->p_vaddr;
        if (ph->p_type == PT_LOAD && range->addr >= start && range->addr < start + ph->p_memsz) {
            range->inside = 1;
        }
    }
    return 1;  /* stop after the program */
}

struct gbm_device *gbm_create_device(int fd)
{
    struct main_range range = { (uintptr_t)__builtin_return_address(0), 0 };

    if (!real_gbm_create_device) {
        real_gbm_create_device = dlsym(RTLD_NEXT, "gbm_create_device");
    }
    if (!two_d_gbm) {
        dl_iterate_phdr(find_in_main_program, &range);
        if (range.inside) {
            log_line("2D resources use shared memory, not GBM buffers");
            return NULL;  /* vhost-user-gpu falls back to its memory buffers */
        }
    }
    return real_gbm_create_device(fd);
}
