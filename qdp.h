/*
 * qdp.h - qdev-host plugin ABI, version 1
 *
 * A plugin implements one PCI device for a stock QEMU through vfio-user.
 * The host owns the libvfio-user context, the socket, the main loop and the
 * optional display export (org.qemu.Display1 compatible, used by QSM Direct).
 * The plugin configures the device on the context in setup() and serves
 * region accesses through the callbacks it registers there.
 *
 * ABI rules: this header only; the plugin links against libvfio-user and
 * nothing of the host. New fields are appended; abi_version bumps on
 * incompatible changes.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef QDP_H
#define QDP_H

#include <stdbool.h>
#include <stdint.h>
#include "libvfio-user.h"

#define QDP_ABI_VERSION 1

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

/* What the host knows that a plugin may want: the EDID of the physical
 * display attached through the KMS sink (NULL without one). */
typedef struct qdp_host_env {
    uint32_t        size;          /* sizeof(qdp_host_env) */
    const uint8_t  *edid;
    uint32_t        edid_len;
} qdp_host_env;

typedef struct qdp_device_ops {
    uint32_t    abi_version;     /* QDP_ABI_VERSION */
    const char *name;            /* "tb-vga" */
    const char *version;         /* plugin's own version string */

    /* Configure PCI identity, capabilities, regions, IRQs, DMA on ctx.
     * Called before vfu_realize_ctx(). args: plugin-specific option string
     * (may be NULL), env: host environment (never NULL). Returns 0 or -errno.
     * *priv is handed back to all ops. */
    int  (*setup)(vfu_ctx_t *ctx, const char *args, const qdp_host_env *env, void **priv);
    void (*teardown)(void *priv);

    /* Device reset requested by the client (or connection lost). */
    int  (*reset)(void *priv, vfu_reset_type_t type);

    /* Optional: called from the host main loop every tick_ms (0 = never). */
    uint32_t tick_ms;
    void (*tick)(void *priv);

    /* Optional display: fill *out with the current scanout. Return false if
     * the plugin has no display. The host polls this and detects damage by
     * comparing the framebuffer contents. */
    bool (*get_scanout)(void *priv, qdp_scanout *out);
} qdp_device_ops;

/* The single exported symbol of a plugin shared object. */
typedef const qdp_device_ops *(*qdp_plugin_v1_fn)(void);
#define QDP_PLUGIN_SYMBOL "qdp_plugin_v1"

#endif /* QDP_H */
