/* scanout-flip.h - IDLE-PATH-1a (apple-virgl analysis/20261122-3080-GPU-TIMELINE-LEVERS.md 6.8, FIX_CANDIDATE IDLE-PATH-1a):
 * how qdev-host takes a new scanout from the plugin.  The uniq-vgpu plugin publishes every frame into the next free slot of ONE
 * scanout memfd (pvg_scanout_publish / DISP-D1 reserve), so consecutive scanouts differ only in their offset.  Today
 * disp_set_mode() treats any fd / offset / geometry difference as a mode change: munmap + mmap of the 8 MB region, a sink re-notify
 * and a "scanout on WxH" log line on EVERY frame (643 / min on the idle warm-tradebot, 2 832 / min on VM104).  Reference
 * (QEMU apple-gfx.m, the PVG host the plugin forks): a mode is set once per geometry (replace_surface); new frames are content
 * updates of that console surface.
 *   qdp_scanout_classify(cur, now): SAME (identical), FLIP (both enabled, same fd / width / height / stride / format, other offset),
 *     MODE (anything else, incl. enable / disable).
 *   qdp_scanout_maps: a small cache of the mapped slots of the current mode (keyed by offset), so a FLIP to a slot seen before maps
 *     nothing; a MODE change unmaps them all.  map / unmap are injected (tests count them; qdev-host passes mmap / munmap).
 * STATUS: implemented (DISPLAY lane 2026-11-24); the first-failing stub was today's behaviour (any difference MODE, map per get).
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QDEV_SCANOUT_FLIP_H
#define QDEV_SCANOUT_FLIP_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "qdp.h"

enum qdp_scanout_change { QDP_SC_SAME = 0, QDP_SC_FLIP = 1, QDP_SC_MODE = 2 };

static inline enum qdp_scanout_change qdp_scanout_classify(const qdp_scanout *cur, const qdp_scanout *now)
{
    const bool same_mode = cur->enabled == now->enabled && cur->fd == now->fd && cur->width == now->width &&
                           cur->height == now->height && cur->stride == now->stride &&
                           cur->pixman_format == now->pixman_format;
    if (same_mode && cur->offset == now->offset) return QDP_SC_SAME;
    if (same_mode && cur->enabled) return QDP_SC_FLIP; /* both enabled, another slot of the same mode */
    return QDP_SC_MODE;
}

#define QDP_SCANOUT_MAPS 4
typedef struct qdp_scanout_maps {
    void *(*map)(void *ud, int fd, size_t aligned_offset, size_t len); /* NULL on failure */
    void (*unmap)(void *ud, void *base, size_t len);
    void *ud;
    long page;
    struct { bool used; uint32_t offset; void *base; size_t len; uint8_t *pix; } e[QDP_SCANOUT_MAPS];
    int next; /* the entry replaced when every entry is in use (round robin) */
} qdp_scanout_maps;

/* The first pixel of scanout s (mapping its slot once per mode), NULL on failure.  The cache holds slots of ONE mode (fd,
 * geometry, format): a MODE change must clear it first (qdp_scanout_maps_clear). */
static inline uint8_t *qdp_scanout_maps_get(qdp_scanout_maps *m, const qdp_scanout *s)
{
    for (int i = 0; i < QDP_SCANOUT_MAPS; i++)
        if (m->e[i].used && m->e[i].offset == s->offset) return m->e[i].pix;
    int k = -1;
    for (int i = 0; i < QDP_SCANOUT_MAPS && k < 0; i++)
        if (!m->e[i].used) k = i;
    if (k < 0) {
        k = m->next;
        m->next = (m->next + 1) % QDP_SCANOUT_MAPS;
        m->unmap(m->ud, m->e[k].base, m->e[k].len);
        m->e[k].used = false;
    }
    const size_t aligned = s->offset & ~((size_t)m->page - 1);
    const size_t len = (s->offset - aligned) + (size_t)s->stride * s->height;
    void *b = m->map(m->ud, s->fd, aligned, len);
    if (!b) return NULL;
    m->e[k].used = true; m->e[k].offset = s->offset; m->e[k].base = b; m->e[k].len = len;
    m->e[k].pix = (uint8_t *)b + (s->offset - aligned);
    return m->e[k].pix;
}

/* A MODE change (or teardown): unmap every cached slot. */
static inline void qdp_scanout_maps_clear(qdp_scanout_maps *m)
{
    for (int i = 0; i < QDP_SCANOUT_MAPS; i++)
        if (m->e[i].used) { m->unmap(m->ud, m->e[i].base, m->e[i].len); m->e[i].used = false; }
    m->next = 0;
}

#endif
