/*
 * qdev-host: fence hold for QEMU's vhost-user-gpu (LD_PRELOAD)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * QEMU's contrib/vhost-user-gpu keeps a GET_DISPLAY_INFO or GET_EDID command on
 * its fence queue while it waits for the frontend's reply, and takes it back in
 * the reply handler as the last entry of that queue.  Its virgl fence callback
 * completes every queued command whose fence id is not above the signalled
 * fence, and such a command has none (fence id 0).  A virgl fence signalled
 * during that round trip therefore completes and frees the waiting command, and
 * the reply handler finds an empty queue (NULL dereference, "segfault at 28" in
 * get_edid_cb, seen on 2026-11-09) or a fenced command (assertion).
 *
 * This library holds virgl fence notifications back while such a reply is
 * outstanding and replays them, in order, as soon as the reply handler has
 * returned.  It relies only on interfaces outside vhost-user-gpu's own code:
 *  - virglrenderer's public API: virgl_renderer_init() and its write_fence
 *    callback, which vhost-user-gpu calls from its main loop (it does not ask
 *    for VIRGL_RENDERER_ASYNC_FENCE_CB);
 *  - the vhost-user-gpu protocol (QEMU docs/interop/vhost-user-gpu.rst): a
 *    backend request starts with {u32 request, u32 flags = 0, u32 size};
 *    GET_DISPLAY_INFO is 3 with no payload, GET_EDID 11 with a u32 scanout.
 *    The reply is read from the same socket; nothing else is read from it.
 * Without virgl, or if the requests are not seen, fences pass through as before.
 *
 * Environment, for tests:
 *   QDEV_VUGPU_FENCE_HOLD=0          do not hold fences (the unfixed behaviour)
 *   QDEV_VUGPU_TEST_FENCE_IN_REPLY=1 report a fence while every such reply is
 *                                    on its way, as a signalled fence would;
 *                                    =edid or =info: only for that request
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <glib.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <virgl/virglrenderer.h>

enum { GPU_GET_DISPLAY_INFO = 3, GPU_GET_EDID = 11, GPU_HDR_SIZE = 12 };

static int (*real_renderer_init)(void *, int, struct virgl_renderer_callbacks *);
static ssize_t (*real_sendmsg)(int, const struct msghdr *, int);
static ssize_t (*real_read)(int, void *, size_t);

static struct virgl_renderer_callbacks callbacks;   /* the caller's, with our write_fence */
static void (*real_write_fence)(void *, uint32_t);
static void *renderer_cookie;

static int hold_enabled = 1;
static int test_fence_in_reply;
static int test_request;          /* 0: both requests */

/* written by the main loop thread only; read() may also run on other threads */
static int gpu_fd = -1;          /* the vhost-user-gpu protocol socket */
static int awaiting;             /* request code of the outstanding reply, 0 if none */

static GArray *held;             /* fences that came in while a reply was awaited */
static uint32_t last_fence;
static unsigned long holds;

static void log_line(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void log_line(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("qdev-host fence hold: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static const char *request_name(int request)
{
    return request == GPU_GET_EDID ? "GET_EDID" : "GET_DISPLAY_INFO";
}

__attribute__((constructor)) static void setup(void)
{
    const char *value = getenv("QDEV_VUGPU_FENCE_HOLD");
    hold_enabled = !(value && strcmp(value, "0") == 0);
    value = getenv("QDEV_VUGPU_TEST_FENCE_IN_REPLY");
    test_fence_in_reply = value && (!strcmp(value, "1") || !strcmp(value, "edid") || !strcmp(value, "info"));
    test_request = !value ? 0 : !strcmp(value, "edid") ? GPU_GET_EDID : !strcmp(value, "info") ? GPU_GET_DISPLAY_INFO : 0;
    real_renderer_init = dlsym(RTLD_NEXT, "virgl_renderer_init");
    real_sendmsg = dlsym(RTLD_NEXT, "sendmsg");
    real_read = dlsym(RTLD_NEXT, "read");
    held = g_array_new(FALSE, FALSE, sizeof(uint32_t));
}

static void deliver(void *cookie, uint32_t fence)
{
    last_fence = fence;
    real_write_fence(cookie, fence);
}

static void hold_write_fence(void *cookie, uint32_t fence)
{
    renderer_cookie = cookie;
    if (hold_enabled && __atomic_load_n(&awaiting, __ATOMIC_ACQUIRE)) {
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

    if (!count || __atomic_load_n(&awaiting, __ATOMIC_ACQUIRE)) {
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

    if (__atomic_load_n(&awaiting, __ATOMIC_ACQUIRE) == request && real_write_fence) {
        log_line("test: fence %u reported while a %s reply is on its way (%s)",
                 last_fence, request_name(request), hold_enabled ? "held" : "passed through");
        hold_write_fence(renderer_cookie, last_fence);
    }
    return G_SOURCE_REMOVE;
}

int virgl_renderer_init(void *cookie, int flags, struct virgl_renderer_callbacks *cbs)
{
    if (!real_renderer_init) {
        real_renderer_init = dlsym(RTLD_NEXT, "virgl_renderer_init");
    }
    if (!cbs || !cbs->write_fence || (flags & VIRGL_RENDERER_ASYNC_FENCE_CB)) {
        return real_renderer_init(cookie, flags, cbs);  /* callbacks off the main loop: leave them */
    }
    callbacks = *cbs;
    real_write_fence = cbs->write_fence;
    callbacks.write_fence = hold_write_fence;
    renderer_cookie = cookie;
    log_line("active%s%s", hold_enabled ? "" : ", holding OFF",
             test_fence_in_reply ? ", test fence during every display info/EDID reply" : "");
    return real_renderer_init(cookie, flags, &callbacks);
}

ssize_t sendmsg(int fd, const struct msghdr *msg, int flags)
{
    ssize_t ret;
    uint32_t hdr[3];

    if (!real_sendmsg) {
        real_sendmsg = dlsym(RTLD_NEXT, "sendmsg");  /* called before our constructor */
    }
    ret = real_sendmsg(fd, msg, flags);

    if (ret >= GPU_HDR_SIZE && real_write_fence && msg && msg->msg_iovlen >= 1 &&
        msg->msg_iov[0].iov_len >= GPU_HDR_SIZE) {
        memcpy(hdr, msg->msg_iov[0].iov_base, sizeof(hdr));
        if (hdr[1] == 0 &&
            ((hdr[0] == GPU_GET_DISPLAY_INFO && hdr[2] == 0) ||
             (hdr[0] == GPU_GET_EDID && hdr[2] == sizeof(uint32_t)))) {
            __atomic_store_n(&gpu_fd, fd, __ATOMIC_RELEASE);
            __atomic_store_n(&awaiting, (int)hdr[0], __ATOMIC_RELEASE);
            if (test_fence_in_reply && (!test_request || test_request == (int)hdr[0])) {
                /* above the reply's fd source, so it runs first */
                g_idle_add_full(G_PRIORITY_HIGH, report_test_fence, GINT_TO_POINTER(hdr[0]), NULL);
            }
        }
    }
    return ret;
}

ssize_t read(int fd, void *buf, size_t count)
{
    ssize_t ret;
    int request;

    if (!real_read) {
        real_read = dlsym(RTLD_NEXT, "read");  /* called before our constructor */
    }
    ret = real_read(fd, buf, count);

    if (ret > 0 && fd >= 0 && fd == __atomic_load_n(&gpu_fd, __ATOMIC_ACQUIRE) &&
        (request = __atomic_load_n(&awaiting, __ATOMIC_ACQUIRE)) != 0) {
        /* the reply handler is running now; release once it has returned */
        __atomic_store_n(&awaiting, 0, __ATOMIC_RELEASE);
        if (held && held->len) {
            g_idle_add_full(G_PRIORITY_DEFAULT, release_held, GINT_TO_POINTER(request), NULL);
        }
    }
    return ret;
}
