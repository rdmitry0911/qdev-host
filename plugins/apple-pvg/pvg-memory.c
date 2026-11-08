/*
 * apple-pvg plugin: guest memory over the host's DMA regions.
 *
 * Replaces the fork's QEMU memory-API leaves (include/hw/display/
 * apple-gfx-ml-memory.h and the memory callbacks of apple-gfx-ml.c at
 * c43589029b). Each fd-backed DMA region is mapped by the plugin from its own
 * dup of the region fd. A map_gpa() pin keeps that mapping (and the memfd
 * pages) alive until the matching unmap, even after the client unmaps the
 * range: the fork's MemoryRegion reference. The lookup table is protected by
 * a rwlock whose read side is the fork's RCU read section and whose write
 * side is held by the host thread from quiesce() to resume(), so a QMetal
 * lookup sees either the old or the new map, never the gap between them.
 *
 * Non-RAM targets (no region, or a region without an fd) fail: there is no
 * message-DMA fallback. Every refusal is logged.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "pvg.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

/* depth of the table read section of this thread (nesting, as RCU) */
static __thread unsigned pvg_rd_depth;

void pvg_memory_init(PvgMemory *m)
{
    pthread_rwlockattr_t attr;

    memset(m, 0, sizeof(*m));
    pthread_rwlockattr_init(&attr);
    /* Reader preference: like an RCU reader in the fork, a lookup never waits
     * for a merely pending map change, so QMetal may take the read side in
     * any order with its own locks. Only a map change in progress (quiesce ..
     * resume) holds lookups back. */
    pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_READER_NP);
    pthread_rwlock_init(&m->tab_lock, &attr);
    pthread_rwlockattr_destroy(&attr);
    pthread_mutex_init(&m->live_lock, NULL);
    m->live = g_ptr_array_new();
    m->next_serial = 1;
}

void pvg_memory_read_enter(PvgState *s)
{
    if (pvg_rd_depth++ == 0) {
        pthread_rwlock_rdlock(&s->mem.tab_lock);
    }
}

void pvg_memory_read_leave(PvgState *s)
{
    g_assert(pvg_rd_depth > 0);
    if (--pvg_rd_depth == 0) {
        pthread_rwlock_unlock(&s->mem.tab_lock);
    }
}

PvgRegion *pvg_memory_lookup_locked(PvgState *s, uint64_t gpa)
{
    PvgMemory *m = &s->mem;
    unsigned lo = 0, hi = m->ntab;

    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        PvgRegion *r = m->tab[mid];

        if (gpa < r->iova) {
            hi = mid;
        } else if (gpa - r->iova >= r->len) {
            lo = mid + 1;
        } else {
            return r;
        }
    }
    return NULL;
}

void pvg_region_ref(PvgState *s, PvgRegion *r)
{
    pthread_mutex_lock(&s->mem.live_lock);
    g_assert(r->refs > 0);
    r->refs++;
    pthread_mutex_unlock(&s->mem.live_lock);
}

void pvg_region_unref(PvgState *s, PvgRegion *r)
{
    bool last;

    pthread_mutex_lock(&s->mem.live_lock);
    g_assert(r->refs > 0);
    last = --r->refs == 0;
    if (last) {
        g_ptr_array_remove_fast(s->mem.live, r);
    }
    pthread_mutex_unlock(&s->mem.live_lock);
    if (!last) {
        return;
    }
    if (r->map) {
        uint64_t delta = r->fd_offset % pvg_page_size();
        munmap(r->map - delta, r->len + delta);
    }
    if (r->fd >= 0) {
        close(r->fd);
    }
    g_free(r);
}

PvgRegion *pvg_memory_region_from_host(PvgState *s, const void *hva,
                                       uint64_t *offset)
{
    const uint8_t *p = hva;
    PvgRegion *found = NULL;

    pthread_mutex_lock(&s->mem.live_lock);
    for (guint i = 0; i < s->mem.live->len; i++) {
        PvgRegion *r = g_ptr_array_index(s->mem.live, i);

        if (r->map && p >= r->map && (uint64_t)(p - r->map) < r->len) {
            found = r;
            if (offset) {
                *offset = (uint64_t)(p - r->map);
            }
            break;
        }
    }
    pthread_mutex_unlock(&s->mem.live_lock);
    return found;
}

void pvg_memory_refuse(PvgState *s, const char *what, uint64_t gpa,
                       uint64_t size, const char *why)
{
    uint64_t n = pvg_atomic_fetch_inc(&s->mem.refused) + 1;

    if (n <= 10 || n % 60 == 0) {
        pvg_log(s, "[apple-gfx-ml][memory][ERROR] %s gpa=0x%" PRIx64
                " size=0x%" PRIx64 " refused: %s (refused=%" PRIu64 ")\n",
                what, gpa, size, why, n);
    }
}

/* Inside a read section: [gpa, gpa + size) in one mapped region. */
static PvgRegion *pvg_memory_span(PvgState *s, uint64_t gpa, uint64_t size,
                                  bool write, const char **why)
{
    PvgRegion *r = pvg_memory_lookup_locked(s, gpa);

    if (!r) {
        *why = "not in a DMA region";
        return NULL;
    }
    if (!r->map) {
        *why = "DMA region has no fd";
        return NULL;
    }
    if (size > r->len - (gpa - r->iova)) {
        *why = "crosses the end of its DMA region";
        return NULL;
    }
    if (write && !(r->prot & PROT_WRITE)) {
        *why = "DMA region is read-only";
        return NULL;
    }
    return r;
}

/* fork agfx_memory_map_gpa: one pin per successful map */
static void *pvg_map(PvgState *s, const char *what, uint64_t gpa, uint64_t size,
                     int writable, bool log)
{
    const char *why = NULL;
    PvgRegion *r;
    void *ptr = NULL;

    pvg_memory_read_enter(s);
    r = pvg_memory_span(s, gpa, size, writable != 0, &why);
    if (r) {
        pvg_region_ref(s, r);
        ptr = r->map + (gpa - r->iova);
    }
    pvg_memory_read_leave(s);
    if (!ptr && log) {
        pvg_memory_refuse(s, what, gpa, size, why);
    }
    return ptr;
}

void *pvg_map_gpa(void *ctx, uint64_t gpa, size_t size, int writable)
{
    return pvg_map(ctx, "map_gpa", gpa, size, writable, true);
}

/* fork agfx_memory_unmap_gpa: the sole matching unpin; dirty is a no-op
 * (no migration, no dirty log). */
void pvg_unmap_gpa(void *ctx, void *hva, size_t size, int dirty)
{
    PvgState *s = ctx;
    PvgRegion *r = pvg_memory_region_from_host(s, hva, NULL);

    (void)size;
    (void)dirty;
    if (r) {
        pvg_region_unref(s, r);
    } else if (s->debug_level >= 1) {
        pvg_log(s, "[apple-gfx-ml][memory] unmap_gpa: hva=%p is no live mapping\n", hva);
    }
}

/* fork qemu_mark_mapped_memory_dirty: success for every live mapping (the
 * RETAINED_RAM_V1 contract requires it); there is no dirty log to update. */
int pvg_mark_mapped_memory_dirty(void *ctx, void *hva, size_t size)
{
    PvgState *s = ctx;
    uint64_t offset = 0;
    PvgRegion *r;

    if (!s || !hva || size == 0) {
        return -1;
    }
    r = pvg_memory_region_from_host(s, hva, &offset);
    return r && size <= r->len - offset ? 0 : -1;
}

static int pvg_read(PvgState *s, uint64_t gpa, void *buf, size_t size, bool log)
{
    uint8_t *dst = buf;
    const char *why = NULL;
    uint64_t at = gpa, left = size;

    if (size == 0) {
        return 0;
    }
    pvg_memory_read_enter(s);
    while (left) {
        PvgRegion *r = pvg_memory_span(s, at, 1, false, &why);
        uint64_t n;

        if (!r) {
            break;
        }
        n = MIN(left, r->len - (at - r->iova));
        memcpy(dst, r->map + (at - r->iova), n);
        dst += n;
        at += n;
        left -= n;
    }
    pvg_memory_read_leave(s);
    if (left && log) {
        pvg_memory_refuse(s, "read_memory", gpa, size, why);
    }
    return left ? -1 : 0;
}

int pvg_read_memory(void *ctx, uint64_t gpa, void *buf, size_t size)
{
    return pvg_read(ctx, gpa, buf, size, true);
}

/* Guest RAM only: nonzero when any byte is not RAM. A probe: not logged. */
int pvg_read_ram_memory(void *ctx, uint64_t gpa, void *buf, size_t size)
{
    return pvg_read(ctx, gpa, buf, size, false);
}

int pvg_write_memory(void *ctx, uint64_t gpa, const void *buf, size_t size)
{
    PvgState *s = ctx;
    const uint8_t *src = buf;
    const char *why = NULL;
    uint64_t at = gpa, left = size;
    bool ok = true;

    if (size == 0) {
        return 0;
    }
    pvg_memory_read_enter(s);
    /* Validate every span first: a refused write writes nothing. */
    while (left) {
        PvgRegion *r = pvg_memory_span(s, at, 1, true, &why);
        uint64_t n;

        if (!r) {
            ok = false;
            break;
        }
        n = MIN(left, r->len - (at - r->iova));
        at += n;
        left -= n;
    }
    for (at = gpa, left = ok ? size : 0; left;) {
        PvgRegion *r = pvg_memory_lookup_locked(s, at);
        uint64_t n = MIN(left, r->len - (at - r->iova));

        /* memmove: the source may alias guest RAM (fork: core RAM write) */
        memmove(r->map + (at - r->iova), src, n);
        src += n;
        at += n;
        left -= n;
    }
    pvg_memory_read_leave(s);
    if (!ok) {
        pvg_memory_refuse(s, "write_memory", gpa, size, why);
        return -1;
    }
    return 0;
}

/* 1:1 reference apple_gfx_mmio_map_surface_memory: same leaf as map_gpa */
void *pvg_iosfc_map_memory(void *ctx, uint64_t gpa, uint64_t len, int read_only)
{
    return pvg_map(ctx, "iosfc_map_memory", gpa, len, !read_only, true);
}

int pvg_iosfc_unmap_memory(void *ctx, void *hva, uint64_t len)
{
    PvgState *s = ctx;
    PvgRegion *r = pvg_memory_region_from_host(s, hva, NULL);

    (void)len;
    if (!r) {
        pvg_log_direct(s, "[apple-gfx-ml] iosfc_unmap: memory not found\n");
        return -1;
    }
    pvg_region_unref(s, r);
    return 0;
}

uint64_t pvg_topology_sequence(void *ctx)
{
    PvgState *s = ctx;

    return pvg_atomic_load_acquire(&s->mem.topology_seq);
}

/* RETAINED_RAM_V1 capture (fork agfx_memory_capture_retained_with): accepted
 * only when no map change was in progress at its start and none began before
 * its end. A capture rejected after its map is unpinned here. */
void *pvg_map_retained(void *ctx, uint64_t gpa, size_t size, int writable,
                       uint64_t *sequence)
{
    PvgState *s = ctx;
    uint64_t begin;
    void *ptr;

    if (!sequence) {
        return NULL;
    }
    begin = pvg_atomic_load_acquire(&s->mem.topology_seq);
    if (begin & 1) {
        return NULL;
    }
    ptr = pvg_map(s, "map_retained", gpa, size, writable, true);
    if (!ptr) {
        return NULL;
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (pvg_atomic_read(&s->mem.topology_seq) != begin) {
        pvg_unmap_gpa(s, ptr, size, 0);
        return NULL;
    }
    *sequence = begin;
    return ptr;
}

/* --------------------------------------------------------- map changes */

static void pvg_topology_write_begin(PvgState *s)
{
    if (!s->mem.quiesced) {
        pvg_atomic_inc(&s->mem.topology_seq);   /* odd: publication */
        pthread_rwlock_wrlock(&s->mem.tab_lock);
    }
}

static void pvg_topology_write_end(PvgState *s)
{
    if (!s->mem.quiesced) {
        pthread_rwlock_unlock(&s->mem.tab_lock);
        pvg_atomic_inc(&s->mem.topology_seq);   /* even: published */
    }
}

static void pvg_table_remove_at(PvgState *s, unsigned i)
{
    PvgRegion *r = s->mem.tab[i];

    memmove(&s->mem.tab[i], &s->mem.tab[i + 1],
            (s->mem.ntab - i - 1) * sizeof(s->mem.tab[0]));
    s->mem.ntab--;
    r->in_table = false;
    pvg_region_unref(s, r);
}

void pvg_memory_dma_register(PvgState *s, const qdp_dma_region *in)
{
    PvgMemory *m = &s->mem;
    PvgRegion *r;
    unsigned i;

    if (!in || in->len == 0 || in->iova > UINT64_MAX - in->len) {
        pvg_log(s, "[apple-gfx-ml][memory][ERROR] dma_register: invalid region\n");
        return;
    }
    r = g_new0(PvgRegion, 1);
    r->refs = 1;                    /* the table */
    r->iova = in->iova;
    r->len = in->len;
    r->fd = -1;
    r->fd_offset = in->fd_offset;
    r->prot = in->prot;
    if (in->fd >= 0) {
        uint64_t delta = in->fd_offset % pvg_page_size();
        int prot = (in->prot & PROT_WRITE) ? PROT_READ | PROT_WRITE : PROT_READ;
        void *map;

        r->fd = fcntl(in->fd, F_DUPFD_CLOEXEC, 0);
        if (r->fd < 0) {
            pvg_log(s, "[apple-gfx-ml][memory][ERROR] dma_register: dup fd %d: %s\n",
                    in->fd, strerror(errno));
        } else {
            map = mmap(NULL, in->len + delta, prot, MAP_SHARED, r->fd,
                       (off_t)(in->fd_offset - delta));
            if (map == MAP_FAILED) {
                pvg_log(s, "[apple-gfx-ml][memory][ERROR] dma_register: mmap "
                        "iova=0x%" PRIx64 " len=0x%" PRIx64 ": %s\n",
                        in->iova, in->len, strerror(errno));
                close(r->fd);
                r->fd = -1;
            } else {
                r->map = (uint8_t *)map + delta;
            }
        }
    }

    pvg_topology_write_begin(s);
    /* A replacing register should be preceded by its unregister; drop any
     * overlapped entry so the table never holds two owners of one GPA. */
    for (i = 0; i < m->ntab;) {
        PvgRegion *o = m->tab[i];

        if (o->iova < r->iova + r->len && r->iova < o->iova + o->len) {
            pvg_log(s, "[apple-gfx-ml][memory][ERROR] dma_register: iova=0x%" PRIx64
                    " len=0x%" PRIx64 " overlaps iova=0x%" PRIx64 " len=0x%" PRIx64
                    "; the old region is dropped\n", r->iova, r->len, o->iova, o->len);
            pvg_table_remove_at(s, i);
            continue;
        }
        i++;
    }
    pthread_mutex_lock(&m->live_lock);
    r->serial = m->next_serial++;
    g_ptr_array_add(m->live, r);
    pthread_mutex_unlock(&m->live_lock);
    m->tab = g_renew(PvgRegion *, m->tab, m->ntab + 1);
    for (i = m->ntab; i > 0 && m->tab[i - 1]->iova > r->iova; i--) {
        m->tab[i] = m->tab[i - 1];
    }
    m->tab[i] = r;
    m->ntab++;
    r->in_table = true;
    pvg_topology_write_end(s);

    pvg_log(s, "[apple-gfx-ml][memory] dma_register iova=0x%" PRIx64 " len=0x%" PRIx64
            " fd=%s offset=0x%" PRIx64 " prot=%s%s regions=%u\n",
            r->iova, r->len, r->map ? "yes" : "no", r->fd_offset,
            (r->prot & PROT_READ) ? "r" : "-", (r->prot & PROT_WRITE) ? "w" : "-",
            m->ntab);
}

void pvg_memory_dma_unregister(PvgState *s, const qdp_dma_region *in)
{
    PvgMemory *m = &s->mem;
    bool found = false;

    if (!in) {
        return;
    }
    pvg_topology_write_begin(s);
    for (unsigned i = 0; i < m->ntab; i++) {
        if (m->tab[i]->iova == in->iova && m->tab[i]->len == in->len) {
            pvg_table_remove_at(s, i);
            found = true;
            break;
        }
    }
    pvg_topology_write_end(s);
    pvg_log(s, "[apple-gfx-ml][memory]%s dma_unregister iova=0x%" PRIx64
            " len=0x%" PRIx64 "%s regions=%u\n", found ? "" : "[WARN]",
            in->iova, in->len, found ? "" : " (not registered)", m->ntab);
}

/* Host thread, before a map change: no lookup may run until resume. */
void pvg_memory_quiesce(PvgState *s)
{
    if (s->mem.quiesced) {
        return;
    }
    pvg_atomic_inc(&s->mem.topology_seq);
    pthread_rwlock_wrlock(&s->mem.tab_lock);
    s->mem.quiesced = true;
}

void pvg_memory_resume(PvgState *s)
{
    if (!s->mem.quiesced) {
        return;
    }
    s->mem.quiesced = false;
    pthread_rwlock_unlock(&s->mem.tab_lock);
    pvg_atomic_inc(&s->mem.topology_seq);
}

/* Lost connection: the client is gone and its regions with it. Lookups must
 * fail fast (not wait for a resume that never comes) while the stop phase
 * drains QMetal. */
void pvg_memory_reopen_after_lost_conn(PvgState *s)
{
    pvg_memory_resume(s);
}
