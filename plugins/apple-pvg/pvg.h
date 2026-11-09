/*
 * apple-pvg plugin: internal declarations shared by its source files.
 *
 * Port of the apple-gfx-ml device of the apple-virgl QEMU fork
 * (hw/display/apple-gfx-ml.c and companions at c43589029b) to a qdev-host
 * plugin. The QEMU main loop / BQL of the fork becomes one plugin thread, the
 * "device loop" (agfx-main): every function the fork ran in a main-loop BH
 * runs there, in FIFO order. MMIO is served directly on the host's vfio-user
 * thread. Guest memory comes from the host's DMA regions (memfd + offset),
 * which the plugin maps itself, so a mapping handed to QMetal stays valid
 * while QMetal holds it, as a MemoryRegion reference does in the fork.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef APPLE_PVG_H
#define APPLE_PVG_H

#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <glib.h>

#include "qdp.h"
#include "qmu/qmetal_unified.h"
#include "qmu/qmu_owned_display_frame.h"
#include "qmu-private/qmu_mapped_transport_v1.h"
#include "qmu-private/qmu_ordinary_physical_capture_v2.h"
#include "qmu-private/qmu_ordinary_physical_comparison_v1.h"
#include "qmu-private/qmu_ordinary_task_memory_v1.h"

/* ------------------------------------------------------------ atomics */
#define pvg_atomic_read(p)        __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define pvg_atomic_set(p, v)      __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)
#define pvg_atomic_inc(p)         ((void)__atomic_fetch_add((p), 1, __ATOMIC_SEQ_CST))
#define pvg_atomic_dec(p)         ((void)__atomic_fetch_sub((p), 1, __ATOMIC_SEQ_CST))
#define pvg_atomic_fetch_inc(p)   __atomic_fetch_add((p), 1, __ATOMIC_SEQ_CST)
#define pvg_atomic_fetch_dec(p)   __atomic_fetch_sub((p), 1, __ATOMIC_SEQ_CST)
#define pvg_atomic_load_acquire(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)

static inline uint64_t pvg_page_size(void)
{
    static uint64_t page;
    if (!page) {
        page = (uint64_t)sysconf(_SC_PAGESIZE);
    }
    return page;
}

/* ------------------------------------------------------------ guest RAM */

/* One DMA region of the client (DMA_MAP), mapped by the plugin itself from a
 * dup of the region fd. It lives while the table or any pin references it:
 * the plugin analogue of the fork's MemoryRegion reference. */
typedef struct PvgRegion {
    unsigned refs;          /* under PvgMemory.live_lock */
    uint64_t iova;
    uint64_t len;
    int fd;                 /* own dup; -1: no fd (message-DMA only region) */
    uint64_t fd_offset;     /* file offset of iova */
    uint32_t prot;
    uint8_t *map;           /* own MAP_SHARED mapping of [iova, iova + len) */
    bool in_table;
    uint64_t serial;
} PvgRegion;

typedef struct PvgMemory {
    pthread_rwlock_t tab_lock;   /* lookup table; writers: host thread only */
    PvgRegion **tab;             /* sorted by iova */
    unsigned ntab;
    pthread_mutex_t live_lock;   /* refcounts and the live list */
    GPtrArray *live;             /* every region with refs > 0 */
    uint64_t next_serial;
    /* Topology publication seqlock (fork: AgfxTopologySeq): odd while the
     * host is changing the map (quiesce .. resume, or one register). */
    uint64_t topology_seq;
    bool quiesced;               /* host thread only: tab_lock held for write */
    uint64_t refused;            /* non-RAM accesses refused (logged) */
} PvgMemory;

/* ------------------------------------------------------------ device */

typedef struct PvgJob PvgJob;
typedef struct PvgRenderJob PvgRenderJob;
typedef struct PvgPresentCommand PvgPresentCommand;
typedef struct PvgLogEntry PvgLogEntry;
typedef struct AgfxEeeeProviderState AgfxEeeeProviderState;
typedef struct AgfxEeeeNativeContext AgfxEeeeNativeContext;
typedef struct AgfxOrdinaryPhysicalContext AgfxOrdinaryPhysicalContext;
typedef struct AgfxOrdinaryPhysicalOrigin AgfxOrdinaryPhysicalOrigin;

/* Fork AgfxHostWork: an accepted QMetal Session operation (or none, for
 * work outside a Session). */
typedef struct PvgHostWork {
    struct PvgState *state;
    qmu_session_work *work;
} PvgHostWork;

typedef struct PvgScanout {
    pthread_mutex_t lock;
    int fd;
    uint8_t *map;
    size_t slot_size;            /* bytes per buffer, page aligned */
    unsigned nslots;
    uint32_t width, height, stride;
    int front;                   /* last published buffer */
    int held;                    /* buffer the host fetched last */
    uint64_t seq;
} PvgScanout;

typedef struct PvgCursor {
    pthread_mutex_t lock;
    bool defined;
    bool visible;
    int32_t x, y;                /* pointer position (fork: cursor_x/y) */
    uint32_t hot_x, hot_y, width, height;
    uint32_t *data;              /* ARGB, as the fork's QEMUCursor */
    uint64_t serial;
    uint32_t *host_copy;         /* buffer handed to get_cursor() */
    size_t host_copy_len;
} PvgCursor;

enum { PVG_IRQ_OPEN, PVG_IRQ_HELD, PVG_IRQ_DROP };

typedef struct PvgState {
    /* configuration (fork qdev properties) */
    uint32_t vram_size_mb;
    uint32_t display_width, display_height;
    uint32_t pci_vendor_id, pci_device_id;
    bool vsync_enabled;
    uint32_t debug_level;
    bool direct_scanout;
    char *spirv_cache_dir;
    bool log_to_host;
    bool exit_on_disconnect;
    /* display-modes=follow (default): a UI size request (qdp v3 ui_info)
     * replaces the mode list by that one size; fixed: requests are refused */
    bool display_modes_follow;

    /* last accepted UI size request (host main loop only) */
    bool ui_info_accepted;
    qdp_ui_info ui_info_last;

    vfu_ctx_t *vfu;
    const qdp_host_ops *host_ops;
    /* qdp v2: host services only between setup() returning and teardown() */
    int host_open;                /* atomic */

    /* QMetal */
    qmu_session *qmu_dev;
    uint8_t *vram;
    uint64_t vram_size;
    AgfxEeeeProviderState *eeee_provider_state;
    AgfxEeeeNativeContext *eeee_native_context;
    AgfxOrdinaryPhysicalContext *ordinary_physical_context;
    const struct qmu_private_ordinary_physical_capture_v2 *ordinary_physical_peer;
    void *ordinary_physical_receipt;
    AgfxEeeeNativeContext *ordinary_task_memory_context;
    const struct qmu_private_ordinary_task_memory_v1 *ordinary_task_memory_peer;
    void *ordinary_task_memory_receipt;
    uint32_t ordinary_task_memory_install_result;
    const struct qmu_private_mapped_transport_v1 *mapped_transport_peer;
    bool mapped_transport_installed;

    PvgMemory mem;

    /* IRQ (fork: qemu_raise_irq + BH + msi_notify) */
    pthread_mutex_t irq_lock;
    int irq_gate;
    bool irq_pending;
    uint64_t irq_count;
    uint64_t irq_dropped;

    /* device loop: the fork's main-loop BH plane */
    pthread_t loop_thread;
    pthread_mutex_t loop_lock;
    pthread_cond_t loop_cond;
    PvgJob *loop_head, *loop_tail;
    bool loop_stop;
    bool loop_started;

    /* render pool (fork: render_pool ThreadPool) */
#define PVG_RENDER_THREADS 2
    pthread_t render_threads[PVG_RENDER_THREADS];
    pthread_mutex_t render_lock;
    pthread_cond_t render_cond;
    PvgRenderJob *render_head, *render_tail;
    bool render_stop;
    bool render_started;

    /* bootstrap present queue (fork: agfx-present, PGEFIPresentQueue) */
    pthread_t present_thread;
    pthread_mutex_t present_lock;
    pthread_cond_t present_cond;
    bool present_worker_stop;
    bool present_roots_stopped;
    bool present_started;
    PvgPresentCommand *present_cmd_head, *present_cmd_tail;
    bool present_timer_active;
    int64_t present_next_fire_us;
    bool present_source_pending;
    PvgHostWork present_source_owner;
    int iosfc_bootstrap_active;

    /* logging (fork: agfx-log) */
    pthread_t log_thread;
    pthread_mutex_t log_lock;
    pthread_cond_t log_cond;
    PvgLogEntry *log_head, *log_tail;
    uint32_t log_depth;
    uint64_t log_dropped;
    bool log_stop;
    bool log_started;

    /* display state, device loop only (fork: BQL) */
    struct qmu_display_target *display_target;
    uint32_t fb_width, fb_height, fb_stride;
    uint32_t rendering_frame_width, rendering_frame_height;
    uint32_t fb_iosurface_pixel_format;
    uint64_t fb_protection_requirements;
    bool display_published;
    int pending_frames;           /* atomic */
    PvgHostWork deferred_frame_owner;
    uint64_t frame_count;
    uint64_t present_count;
    uint64_t frame_completed_log_count;
    uint64_t new_frame_handler_log_count;
    uint64_t new_frame_signal_log_count;
    uint64_t render_worker_log_count;
    uint64_t bootstrap_present_log_count;
    PvgScanout scan;
    PvgCursor cursor;
    uint32_t cursor_display_id;
    int cursor_show;              /* atomic */

    /* lifecycle */
    bool exiting;
    bool stop_phase_done;
    bool retired;                 /* after the stop phase: no more MMIO */
    bool realized;
} PvgState;

/* ------------------------------------------------------------ logging */
void pvg_log(PvgState *s, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
void pvg_log_direct(PvgState *s, const char *text);

/* ------------------------------------------------------------ pvg-memory.c */
void pvg_memory_init(PvgMemory *m);
void pvg_memory_dma_register(PvgState *s, const qdp_dma_region *r);
void pvg_memory_dma_unregister(PvgState *s, const qdp_dma_region *r);
void pvg_memory_quiesce(PvgState *s);
void pvg_memory_resume(PvgState *s);
void pvg_memory_reopen_after_lost_conn(PvgState *s);

/* table read section (the fork's RCU read section); nests per thread */
void pvg_memory_read_enter(PvgState *s);
void pvg_memory_read_leave(PvgState *s);
/* inside a read section: the region covering gpa, or NULL */
PvgRegion *pvg_memory_lookup_locked(PvgState *s, uint64_t gpa);
/* region of a live mapping (registered or still pinned); NULL if none */
PvgRegion *pvg_memory_region_from_host(PvgState *s, const void *hva,
                                       uint64_t *offset);
void pvg_region_ref(PvgState *s, PvgRegion *r);
void pvg_region_unref(PvgState *s, PvgRegion *r);
void pvg_memory_refuse(PvgState *s, const char *what, uint64_t gpa, uint64_t size,
                       const char *why);

/* QMetal memory callbacks */
void *pvg_map_gpa(void *ctx, uint64_t gpa, size_t size, int writable);
void pvg_unmap_gpa(void *ctx, void *hva, size_t size, int dirty);
int pvg_mark_mapped_memory_dirty(void *ctx, void *hva, size_t size);
int pvg_read_memory(void *ctx, uint64_t gpa, void *buf, size_t size);
int pvg_read_ram_memory(void *ctx, uint64_t gpa, void *buf, size_t size);
int pvg_write_memory(void *ctx, uint64_t gpa, const void *buf, size_t size);
void *pvg_iosfc_map_memory(void *ctx, uint64_t gpa, uint64_t len, int read_only);
int pvg_iosfc_unmap_memory(void *ctx, void *hva, uint64_t len);
uint64_t pvg_topology_sequence(void *ctx);
void *pvg_map_retained(void *ctx, uint64_t gpa, size_t size, int writable,
                       uint64_t *sequence);

/* ------------------------------------------------------------ pvg-process-vm.c */
AgfxEeeeProviderState *agfx_eeee_provider_state_create(void);
bool agfx_eeee_provider_state_destroy(AgfxEeeeProviderState *state);
void agfx_eeee_provider_fill_callbacks(qmu_extended_callbacks *callbacks);

AgfxEeeeNativeContext *agfx_eeee_native_context_create(PvgState *s);
bool agfx_eeee_native_context_acquire(AgfxEeeeNativeContext *context,
                                      qmu_extended_callbacks *callbacks);
void agfx_eeee_native_context_release(void *context);
void agfx_eeee_native_context_stop_acquiring(AgfxEeeeNativeContext *context);
bool agfx_eeee_native_context_retire(AgfxEeeeNativeContext *context);

AgfxOrdinaryPhysicalContext *agfx_ordinary_physical_context_create(PvgState *s);
bool agfx_ordinary_physical_context_acquire(AgfxOrdinaryPhysicalContext *context,
    struct qmu_ordinary_physical_handoff_v2 *handoff);
bool agfx_ordinary_physical_context_stop(void *context);
void agfx_ordinary_physical_context_release(void *context);
void agfx_ordinary_physical_context_retire(AgfxOrdinaryPhysicalContext *context);
uint32_t agfx_ordinary_physical_compare(const void *, const void *, uint64_t,
    uint64_t, const void *, uint64_t, uint64_t);

#endif /* APPLE_PVG_H */
