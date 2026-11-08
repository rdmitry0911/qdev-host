/*
 * qdp.h - qdev-host plugin ABI, versions 1 and 2
 *
 * A plugin implements one PCI device for a stock QEMU through vfio-user.
 * The host owns the libvfio-user context, the socket, the main loop and the
 * optional display export (org.qemu.Display1 compatible, used by QSM Direct).
 * The plugin configures the device on the context in setup() and serves
 * region accesses through the callbacks it registers there.
 *
 * ABI rules: this header only; the plugin links against libvfio-user and
 * nothing of the host. New fields are appended; abi_version bumps on
 * incompatible changes. The host loads plugins of every version it knows
 * (1 and 2) and touches only the fields of the plugin's version.
 *
 * Version 2 (GPU plugins) adds:
 *  - guest RAM regions with their backing fd and file offset (dma_register /
 *    dma_unregister), so a plugin can mmap them itself or build dma-bufs
 *    (udmabuf) over guest memory;
 *  - a synchronous quiesce request before the guest memory map changes;
 *  - host services usable from the plugin's own threads (qdp_host_ops):
 *    IRQ trigger, frame-ready notification, cursor change, log;
 *  - plugin-driven damage (no shadow comparison) and a cursor plane.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef QDP_H
#define QDP_H

#include <stdbool.h>
#include <stdint.h>
#include "libvfio-user.h"

#define QDP_ABI_VERSION 2
#define QDP_ABI_VERSION_MIN 1

/* Current scanout of a display plugin. fd is a memfd/shm fd the host may dup
 * and pass to display listeners (ScanoutMap); the plugin keeps it open. */
typedef struct qdp_scanout {
    bool     enabled;
    int      fd;
    uint32_t offset;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t pixman_format;      /* e.g. 0x20020888 = PIXMAN_x8r8g8b8 */
} qdp_scanout;

/* ---------------------------------------------------------------- ABI v2 */

/* One guest RAM region as mapped by the vfio-user client (DMA_MAP).
 * iova equals the guest-physical address when the VM has no vIOMMU.
 * vaddr is the host's own mapping of [iova, iova + len), NULL when the region
 * has no fd (message-based DMA: the plugin must not use such a region from its
 * threads). fd stays owned by the host and is valid until dma_unregister of
 * this region returns; a plugin that needs it longer dup()s it. A region
 * without an fd arrives with fd == -1 and vaddr == NULL. Regions backed
 * by the same file may carry the same fd (descriptors are de-duplicated), so
 * do not key anything by fd alone. vaddr may also be NULL with fd >= 0 if the
 * client asked for file I/O access (QEMU does not). fd_offset is the file
 * offset that corresponds to iova. */
typedef struct qdp_dma_region {
    uint64_t iova;
    uint64_t len;
    void    *vaddr;
    int      fd;
    uint64_t fd_offset;
    uint32_t prot;               /* PROT_READ | PROT_WRITE */
} qdp_dma_region;

/* A new frame is in the scanout returned by get_scanout(). seq increases by
 * one per presented frame; a rectangle with w == 0 means the whole scanout. */
typedef struct qdp_frame_info {
    uint64_t seq;
    uint32_t x, y, w, h;
} qdp_frame_info;

/* Cursor plane. pixels: width*height premultiplied ARGB8888 (host byte order),
 * valid until the next get_cursor() call; serial changes when the image
 * changes, so the host can skip unchanged images. x, y: position of the
 * image's top-left corner in scanout pixels, i.e. the pointer position minus
 * the hotspot (the host reports x + hot_x, y + hot_y as the pointer). */
typedef struct qdp_cursor {
    bool            visible;
    int32_t         x, y;
    uint32_t        hot_x, hot_y;
    uint32_t        width, height;
    const uint32_t *pixels;
    uint64_t        serial;
} qdp_cursor;

/* Services the host provides to a v2 plugin. Unless noted, every function may
 * be called from any thread of the plugin, at any time between setup()
 * returning and teardown() being called. */
typedef struct qdp_host_ops {
    uint32_t size;               /* sizeof(qdp_host_ops) */
    void    *host;               /* first argument of every call */

    /* Raise MSI/MSI-X vector (or INTx when subindex == 0 and no MSI) of the
     * device. Returns 0, or -errno (e.g. -EINVAL when the guest has not
     * enabled the interrupt; the event is then dropped, as on real PCI).
     * It takes only a lock internal to libvfio-user that the host never holds
     * while it calls into the plugin (quiesce, resume, reset, dma_register,
     * dma_unregister, region and IRQ-state callbacks), and never waits for the
     * main loop: a plugin may hold its own mutex across irq_trigger() and take
     * that mutex in any of those callbacks. */
    int  (*irq_trigger)(void *host, uint32_t subindex);

    /* The plugin has presented a new frame (QDP_F_PLUGIN_DAMAGE plugins). */
    void (*frame_ready)(void *host, const qdp_frame_info *frame);

    /* The cursor changed (shape, visibility or position); the host calls
     * get_cursor() from its main loop soon after. */
    void (*cursor_changed)(void *host);

    /* Log through the host (stderr, with the host's prefix and verbosity).
     * level: 0 error, 1 warning, 2 info, 3 debug. Each call is one line
     * "qdev-host: <name>: [error: |warning: |debug: ]<text>"; level 3 only
     * with -v. A plugin may also write to stderr itself: the host neither
     * captures nor rewrites fd 2, so the plugin's own lines pass through
     * unchanged. */
    void (*log)(void *host, int level, const char *fmt, ...)
        __attribute__((format(printf, 3, 4)));
} qdp_host_ops;

/* Plugin flags (qdp_device_ops.flags, v2). */
#define QDP_F_PLUGIN_DAMAGE  (1u << 0)   /* frames are announced with frame_ready();
                                            the host does not poll/compare */

/* v2: returned by reset(VFU_RESET_LOST_CONN) when the device cannot serve
 * another client. The host then calls teardown() (not resume()) and exits
 * with status 0 instead of waiting for the next client. */
#define QDP_RESET_EXIT 1

/* What the host knows that a plugin may want: the EDID of the physical
 * display attached through the KMS sink (NULL without one). v2 appends the
 * host services. */
typedef struct qdp_host_env {
    uint32_t        size;          /* sizeof(qdp_host_env) of the host */
    const uint8_t  *edid;
    uint32_t        edid_len;
    /* v2 */
    const qdp_host_ops *host_ops;  /* never NULL for a v2 plugin */
} qdp_host_env;

typedef struct qdp_device_ops {
    uint32_t    abi_version;     /* QDP_ABI_VERSION the plugin was built for */
    const char *name;            /* "tb-vga" */
    const char *version;         /* plugin's own version string */

    /* Configure PCI identity, capabilities, regions, IRQs, DMA on ctx.
     * Called before vfu_realize_ctx(). args: plugin-specific option string
     * (may be NULL), env: host environment (never NULL). Returns 0 or -errno.
     * *priv is handed back to all ops. A v2 plugin must not call
     * vfu_setup_device_dma() or vfu_setup_device_quiesce_cb(): the host owns
     * them and forwards the events below. v2: right after setup() returns
     * the host calls get_scanout() once, on the main loop and before it serves
     * the client, to pick up the initial scanout. */
    int  (*setup)(vfu_ctx_t *ctx, const char *args, const qdp_host_env *env, void **priv);
    /* The last call into the plugin (on SIGTERM/SIGINT or after
     * QDP_RESET_EXIT); no host service may be called after it starts. The
     * host never dlclose()s a plugin, and after the teardown of a v2 plugin it
     * leaves with _exit() (stdio flushed): threads the plugin could not join
     * keep running until the process ends and do not race library
     * destructors. */
    void (*teardown)(void *priv);

    /* Device reset requested by the client (or connection lost). Returns 0
     * or -errno. v2 with a quiesce op: reset() runs quiesced. On a lost
     * connection the order is quiesce(), reset(VFU_RESET_LOST_CONN) while
     * the guest RAM regions are still mapped, dma_unregister() of every
     * region, the IRQ eventfds are closed, then resume() and the host waits
     * for the next client; reset(LOST_CONN) may return QDP_RESET_EXIT. */
    int  (*reset)(void *priv, vfu_reset_type_t type);

    /* Optional: called from the host main loop every tick_ms (0 = never). */
    uint32_t tick_ms;
    void (*tick)(void *priv);

    /* Optional display: fill *out with the current scanout. Return false if
     * the plugin has no display. Without QDP_F_PLUGIN_DAMAGE the host polls
     * this and detects damage by comparing the framebuffer contents; with it
     * the host calls it once after setup() and after each batch of
     * frame_ready() calls. Always on the host main loop.
     * Scanout custody: the host reads only the buffer of its latest
     * get_scanout() result, and all its reads (inline Display1 Scanout/Update,
     * PPM dumps and screendumps, the KMS copy path) happen on the main loop,
     * so when it calls get_scanout() again it is done with the previous buffer
     * (a different buffer is unmapped before that call returns to the main
     * loop). Not covered: a Display1 listener that took the memfd with
     * ScanoutMap reads it on its own, asynchronously, until the next
     * ScanoutMap, and the zero-copy KMS sink scans the memfd out until it
     * switches to the new buffer; a plugin that reuses an older buffer may
     * show torn frames there. */
    bool (*get_scanout)(void *priv, qdp_scanout *out);

    /* ------------------------------------------------------------ v2 */
    uint32_t flags;              /* QDP_F_* */

    /* Guest memory map changes, called on the host main loop, in client
     * order. Before any unregister (and before a register that replaces a
     * range) the host calls quiesce(): the plugin must stop every access to
     * guest memory and every irq_trigger() and return 0 once it has (it may
     * block for that), or -errno to refuse (the host then fails the client
     * request). resume() follows the map change. All four are optional.
     * The host may quiesce more often than that: with libvfio-user it asks
     * before every DMA_MAP and DMA_UNMAP, before a device reset or FLR and
     * before dropping a lost client, so dma_register() and reset() also run
     * between quiesce() and resume(). */
    int  (*quiesce)(void *priv);
    void (*resume)(void *priv);
    void (*dma_register)(void *priv, const qdp_dma_region *region);
    void (*dma_unregister)(void *priv, const qdp_dma_region *region);

    /* Optional cursor plane (host main loop). Return false without one. */
    bool (*get_cursor)(void *priv, qdp_cursor *out);
} qdp_device_ops;

/* The single exported symbol of a plugin shared object. */
typedef const qdp_device_ops *(*qdp_plugin_v1_fn)(void);
#define QDP_PLUGIN_SYMBOL "qdp_plugin_v1"   /* kept for every ABI version */

#endif /* QDP_H */
