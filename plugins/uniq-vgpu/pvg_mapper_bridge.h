/* pvg_mapper_bridge.h — the uniq-vgpu <-> apple-videotoolbox IOSurface-mapper bridge.
 *
 * Increment 2, item 3 (the lead's decided architecture). reims co-locates the GPU device, the
 * IOSurface host device (mapper) and the video device in ONE process and resolves a mapping id by
 * an in-process call into the GPU-populated registry. Our stack runs uniq-vgpu (GPU + the mapper
 * ring, see QMetal qmu_iosurface_mapper) and apple-videotoolbox (video) as TWO independent
 * vfio-user host processes with no shared address space. This bridge is the cross-process registry
 * that replaces reims's in-process call: a small shared-memory table of mapping_id -> {guest
 * physical pages + authoritative NV12 plane layout}. uniq-vgpu's mapper ring WRITES it as the guest
 * registers each decode-output IOSurface; apple-videotoolbox READS it to resolve a Dbuf reply's
 * mapping id, maps those guest pages through its OWN view of the shared guest-RAM memfd (vt_map /
 * the qdp DMA-region table), and lands the decoded NV12 there.
 *
 * Header-only + lock-free (a per-slot seqlock) so both plugins share it with no extra TU and no
 * cross-process mutex. Backed by a POSIX shm object both processes open by a VM-scoped name; for
 * unit tests it operates on any plain memory buffer of pvg_mapper_bridge_bytes().
 */
#ifndef PVG_MAPPER_BRIDGE_H
#define PVG_MAPPER_BRIDGE_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
#include <atomic>
#define PVG_ATOMIC_U64 std::atomic<uint64_t>
#define PVG_ATOMIC_LOAD(p)      (p)->load(std::memory_order_acquire)
#define PVG_ATOMIC_STORE(p, v)  (p)->store((v), std::memory_order_release)
#define PVG_THREAD_FENCE()      std::atomic_thread_fence(std::memory_order_seq_cst)
#else
#include <stdatomic.h>
#define PVG_ATOMIC_U64 _Atomic uint64_t
#define PVG_ATOMIC_LOAD(p)      atomic_load_explicit((p), memory_order_acquire)
#define PVG_ATOMIC_STORE(p, v)  atomic_store_explicit((p), (v), memory_order_release)
#define PVG_THREAD_FENCE()      atomic_thread_fence(memory_order_seq_cst)
#endif

#define PVG_BRIDGE_MAGIC     0x50564742524447ULL /* "PVGBRDG" */
/* v2 (GVA cycle 3): 8192 pages a slot. A 3840x2160 'x420' 10-bit surface (two bytes a sample, the
 * HEVC Main 10 aerial wallpaper's pool) needs 6075 4 KiB pages; v1's 4096 (a 4K NV12 surface, ~3038)
 * made the writer refuse its publish. The slot layout changes, so the version does: a reader and a
 * writer of different versions see each other's region as invalid (no bridge), never as slots. */
#define PVG_BRIDGE_VERSION   2u
#define PVG_BRIDGE_SLOTS     64u    /* concurrent mapped decode-output surfaces (pools are small) */
#define PVG_BRIDGE_MAX_PAGES 8192u  /* up to 32 MiB: a 4K x420 surface (6075 4 KiB pages) */
#define PVG_BRIDGE_MAX_PLANES 4u

/* One plane's authoritative layout, straight from the guest IOSurface descriptor the mapper ring
 * walked (so the video side stops guessing the stride — the L-GVA-STRIDE fix). */
typedef struct pvg_bridge_plane {
    uint64_t offset;         /* byte offset of the plane within the surface allocation */
    uint64_t size;
    uint64_t bytes_per_row;  /* the authoritative rowBytes */
    uint64_t width;
    uint64_t height;
} pvg_bridge_plane;

typedef struct pvg_bridge_slot {
    PVG_ATOMIC_U64 generation; /* seqlock: odd = write in progress, even = stable */
    uint32_t mapping_id;       /* 0 = empty */
    uint32_t state;            /* 0 = empty, 1 = mapped */
    uint32_t pixel_format;
    uint32_t width;
    uint32_t height;
    uint32_t plane_count;
    uint64_t alloc_size;
    uint64_t base_offset;      /* the surface's offset within its allocation */
    pvg_bridge_plane planes[PVG_BRIDGE_MAX_PLANES];
    uint32_t page_count;
    uint32_t contiguous;       /* 1 = gpa pages are contiguous (gpa_base + i*page) */
    uint64_t gpa_base;         /* pages[0] (valid for the contiguous fast path) */
    uint64_t gpa_pages[PVG_BRIDGE_MAX_PAGES];
} pvg_bridge_slot;

typedef struct pvg_mapper_bridge {
    uint64_t magic;
    uint32_t version;
    uint32_t slot_count;
    uint64_t page_size;
    pvg_bridge_slot slots[PVG_BRIDGE_SLOTS];
} pvg_mapper_bridge;

/* The resolved view a reader gets back. */
typedef struct pvg_bridge_resolved {
    uint32_t mapping_id;
    uint32_t pixel_format;
    uint32_t width;
    uint32_t height;
    uint32_t plane_count;
    uint64_t alloc_size;
    uint64_t base_offset;
    pvg_bridge_plane planes[PVG_BRIDGE_MAX_PLANES];
    uint32_t page_count;
    uint32_t contiguous;
    uint64_t gpa_base;
    uint64_t gpa_pages[PVG_BRIDGE_MAX_PAGES];
} pvg_bridge_resolved;

/* What the writer (uniq-vgpu, from the mapper ring's Surface) hands publish. */
typedef struct pvg_bridge_publish_info {
    uint32_t pixel_format;
    uint32_t width;
    uint32_t height;
    uint32_t plane_count;
    uint64_t alloc_size;
    uint64_t base_offset;
    pvg_bridge_plane planes[PVG_BRIDGE_MAX_PLANES];
    uint32_t page_count;
    const uint64_t *gpa_pages; /* page_count entries */
    uint64_t page_size;
} pvg_bridge_publish_info;

static inline size_t pvg_mapper_bridge_bytes(void) { return sizeof(pvg_mapper_bridge); }

/* Writer: one-time init of a fresh region (uniq-vgpu, at device create). */
static inline void pvg_mapper_bridge_init(pvg_mapper_bridge *b, uint64_t page_size) {
    memset(b, 0, sizeof *b);
    b->magic = PVG_BRIDGE_MAGIC;
    b->version = PVG_BRIDGE_VERSION;
    b->slot_count = PVG_BRIDGE_SLOTS;
    b->page_size = page_size;
}

static inline int pvg_mapper_bridge_valid(const pvg_mapper_bridge *b) {
    return b && b->magic == PVG_BRIDGE_MAGIC && b->version == PVG_BRIDGE_VERSION &&
           b->slot_count == PVG_BRIDGE_SLOTS;
}

/* Find the slot holding mapping_id, or -1. (Linear scan over a small table.) */
static inline int pvg_bridge_find(const pvg_mapper_bridge *b, uint32_t mapping_id) {
    for (uint32_t i = 0; i < PVG_BRIDGE_SLOTS; i++)
        if (b->slots[i].state == 1 && b->slots[i].mapping_id == mapping_id) return (int)i;
    return -1;
}
static inline int pvg_bridge_find_free(const pvg_mapper_bridge *b) {
    for (uint32_t i = 0; i < PVG_BRIDGE_SLOTS; i++)
        if (b->slots[i].state == 0) return (int)i;
    return -1;
}

/* Writer: publish mapping_id -> info. Returns 0, or -1 if the table is full or the surface has too
 * many pages for a slot. Idempotent on the mapping_id (re-publish updates in place). */
static inline int pvg_mapper_bridge_publish(pvg_mapper_bridge *b, uint32_t mapping_id,
                                            const pvg_bridge_publish_info *info) {
    if (!pvg_mapper_bridge_valid(b) || mapping_id == 0) return -1;
    if (info->page_count > PVG_BRIDGE_MAX_PAGES) return -1;
    int idx = pvg_bridge_find(b, mapping_id);
    if (idx < 0) idx = pvg_bridge_find_free(b);
    if (idx < 0) return -1;
    pvg_bridge_slot *s = &b->slots[idx];

    uint64_t g = PVG_ATOMIC_LOAD(&s->generation);
    PVG_ATOMIC_STORE(&s->generation, g | 1ull); /* odd: write in progress */
    PVG_THREAD_FENCE();

    s->mapping_id = mapping_id;
    s->pixel_format = info->pixel_format;
    s->width = info->width;
    s->height = info->height;
    s->plane_count = info->plane_count > PVG_BRIDGE_MAX_PLANES ? PVG_BRIDGE_MAX_PLANES
                                                              : info->plane_count;
    s->alloc_size = info->alloc_size;
    s->base_offset = info->base_offset;
    memset(s->planes, 0, sizeof s->planes);
    for (uint32_t i = 0; i < s->plane_count; i++) s->planes[i] = info->planes[i];
    s->page_count = info->page_count;
    uint64_t ps = info->page_size ? info->page_size : b->page_size;
    int contiguous = info->page_count > 0;
    for (uint32_t i = 0; i < info->page_count; i++) {
        s->gpa_pages[i] = info->gpa_pages[i];
        if (i > 0 && info->gpa_pages[i] != info->gpa_pages[0] + (uint64_t)i * ps) contiguous = 0;
    }
    s->contiguous = (uint32_t)contiguous;
    s->gpa_base = info->page_count > 0 ? info->gpa_pages[0] : 0;
    s->state = 1;

    PVG_THREAD_FENCE();
    PVG_ATOMIC_STORE(&s->generation, (g | 1ull) + 1ull); /* even again: stable */
    return 0;
}

/* Writer: retract a mapping_id (on unmap / teardown). */
static inline void pvg_mapper_bridge_retract(pvg_mapper_bridge *b, uint32_t mapping_id) {
    if (!pvg_mapper_bridge_valid(b)) return;
    int idx = pvg_bridge_find(b, mapping_id);
    if (idx < 0) return;
    pvg_bridge_slot *s = &b->slots[idx];
    uint64_t g = PVG_ATOMIC_LOAD(&s->generation);
    PVG_ATOMIC_STORE(&s->generation, g | 1ull);
    PVG_THREAD_FENCE();
    s->state = 0;
    s->mapping_id = 0;
    PVG_THREAD_FENCE();
    PVG_ATOMIC_STORE(&s->generation, (g | 1ull) + 1ull);
}

/* Reader: resolve mapping_id into *out. Returns 0 on a stable hit, -1 on miss. Retries the seqlock
 * a bounded number of times if the writer is mid-publish. */
static inline int pvg_mapper_bridge_lookup(const pvg_mapper_bridge *b, uint32_t mapping_id,
                                           pvg_bridge_resolved *out) {
    if (!pvg_mapper_bridge_valid(b) || mapping_id == 0) return -1;
    for (int tries = 0; tries < 64; tries++) {
        int idx = pvg_bridge_find(b, mapping_id);
        if (idx < 0) return -1;
        const pvg_bridge_slot *s = &b->slots[idx];
        uint64_t g1 = PVG_ATOMIC_LOAD(&s->generation);
        if (g1 & 1ull) continue; /* writer busy */
        PVG_THREAD_FENCE();
        out->mapping_id = s->mapping_id;
        out->pixel_format = s->pixel_format;
        out->width = s->width;
        out->height = s->height;
        out->plane_count = s->plane_count;
        out->alloc_size = s->alloc_size;
        out->base_offset = s->base_offset;
        memcpy(out->planes, s->planes, sizeof out->planes);
        out->page_count = s->page_count;
        out->contiguous = s->contiguous;
        out->gpa_base = s->gpa_base;
        uint32_t n = s->page_count <= PVG_BRIDGE_MAX_PAGES ? s->page_count : PVG_BRIDGE_MAX_PAGES;
        memcpy(out->gpa_pages, s->gpa_pages, (size_t)n * sizeof(uint64_t));
        PVG_THREAD_FENCE();
        uint64_t g2 = PVG_ATOMIC_LOAD(&s->generation);
        if (g1 == g2 && s->state == 1 && s->mapping_id == mapping_id) return 0;
    }
    return -1;
}

#endif /* PVG_MAPPER_BRIDGE_H */
