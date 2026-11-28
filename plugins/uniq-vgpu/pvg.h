/*
 * uniq-vgpu plugin: internal declarations shared by its source files.
 *
 * Port of the apple-gfx-ml device of the uniq-vgpu QEMU fork
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
#ifndef UNIQ_VGPU_H
#define UNIQ_VGPU_H

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
#include "qmu-private/qmu_guest_ram_region_v1.h"
#include "qmu-private/qmu_mapped_transport_v1.h"
#include "qmu-private/qmu_ordinary_physical_capture_v2.h"
#include "qmu-private/qmu_ordinary_physical_comparison_v1.h"
#include "qmu-private/qmu_ordinary_task_memory_v1.h"

/* Declared in QMetal's C++ qmu_vulkan.h, which C cannot include. Forward it
 * here so the backend vtable below can take its exact type. */
struct qmu_vulkan_ctx;
void qmu_vk_consume_current_frame_signal(struct qmu_vulkan_ctx *ctx);

/* ============================================================ backend vtable
 * (L-WARMSWAP-INPROC-RELOAD) The REAL renderer (libmlapi / QMetal) is reloaded
 * in-process on stock QEMU by destroying the live Session, dlmopen()-ing a new,
 * co-resident libmlapi build into a fresh link-map namespace, and re-creating
 * the Session on it from the PARKED PvgState.  struct layouts can differ
 * between builds, so EVERY qmu_* call on a Session must go to the instance that
 * CREATED it: the whole qmu_* surface this plugin imports (29 functions + the
 * added qmu_destroy, 35 call sites) is routed through this per-process
 * function-pointer vtable, bound from the active instance's dlopen handle.
 *
 * The field types are taken with __typeof__ from the real prototypes above, so
 * the vtable is exact and tracks the headers automatically; the call sites are
 * then rewritten to the vtable by the object-like macros at the end of this
 * block.  Because the only remaining references to the qmu_* names are these
 * compile-time __typeof__ uses, uniq-vgpu.so carries no direct link-time
 * dependency on any routed symbol: the base instance is bound explicitly by
 * dlopen("libmlapi.so") at setup, and every reload target by dlmopen(NEWLM). */
struct pvg_qmu_vt {
    __typeof__(qmu_create_extended)                                   *create_extended;
    __typeof__(qmu_destroy)                                           *destroy;
    __typeof__(qmu_set_debug_level)                                   *set_debug_level;
    __typeof__(qmu_log_set_callback)                                  *log_set_callback;
    __typeof__(qmu_log_category_name)                                 *log_category_name;
    __typeof__(qmu_log_level_name)                                    *log_level_name;
    __typeof__(qmu_mmio_read)                                         *mmio_read;
    __typeof__(qmu_mmio_write)                                        *mmio_write;
    __typeof__(qmu_session_quiesce_host_callback_producers_deferred)  *session_quiesce_host_callback_producers_deferred;
    __typeof__(qmu_session_rethrow_quiesce)                           *session_rethrow_quiesce;
    __typeof__(qmu_session_finish_retirement_for_exit)                *session_finish_retirement_for_exit;
    __typeof__(qmu_session_get_vulkan)                                *session_get_vulkan;
    __typeof__(qmu_session_acquire_host_work)                         *session_acquire_host_work;
    __typeof__(qmu_session_current_host_work)                         *session_current_host_work;
    __typeof__(qmu_session_invoke_host_work)                          *session_invoke_host_work;
    __typeof__(qmu_session_release_host_work)                         *session_release_host_work;
    __typeof__(qmu_session_retain_host_work)                          *session_retain_host_work;
    __typeof__(qmu_get_private_mapped_transport_v1)                   *get_private_mapped_transport_v1;
    __typeof__(qmu_get_private_guest_ram_region_v1)                   *get_private_guest_ram_region_v1;
    __typeof__(qmu_get_private_ordinary_physical_capture_v2)          *get_private_ordinary_physical_capture_v2;
    __typeof__(qmu_get_private_ordinary_physical_comparison_v1)       *get_private_ordinary_physical_comparison_v1;
    __typeof__(qmu_get_private_ordinary_task_memory_v1)               *get_private_ordinary_task_memory_v1;
    __typeof__(qmu_iosfc_present_tick)                                *iosfc_present_tick;
    __typeof__(qmu_display_copy_selected_current_target)              *display_copy_selected_current_target;
    __typeof__(qmu_display_publish_mode_target)                       *display_publish_mode_target;
    __typeof__(qmu_display_target_release)                            *display_target_release;
    __typeof__(qmu_get_display_cursor_position)                       *get_display_cursor_position;
    __typeof__(qmu_owned_display_frame_release)                       *owned_display_frame_release;
    __typeof__(qmu_vk_consume_current_frame_signal)                   *vk_consume_current_frame_signal;
    __typeof__(qmu_vk_encode_submit_owned_display_frame)              *vk_encode_submit_owned_display_frame;
    __typeof__(qmu_vk_prepare_owned_display_frame)                    *vk_prepare_owned_display_frame;
    /* Optional (newer than the first qdev package's QMetal): NULL when the
     * instance does not export it, exactly like the private getters. */
    __typeof__(qmu_display_set_mode_list)                             *display_set_mode_list;
    /* Optional: the PGDevice reset boundary (reference -[PGDevice reset]).
     * NULL on a libmlapi older than the port of the reset lifecycle; pvg_reset
     * then logs an [ERROR] (the guest's next boot would stall) and goes on. */
    __typeof__(qmu_session_reset_for_device_reset)                    *session_reset_for_device_reset;
    /* Optional (DISP-D1, QMetal newer than 2522102b1): the frame's readback lands straight in the scanout
     * buffer reserved for it. Typed here, not by __typeof__, so the plugin still builds against an older
     * QMetal include; NULL on an instance that does not export them -> the BH read copies as before. */
    int (*display_frame_attach_destination)(qmu_owned_display_frame *, void *bytes, size_t length,
                                            uint32_t stride);
    int (*display_frame_attach_destination_fd)(qmu_owned_display_frame *, void *bytes, size_t length,
                                               uint32_t stride, int dmabuf_fd, uint64_t dmabuf_offset);
    void (*display_forget_destinations)(struct qmu_vulkan_ctx *, const void *base, size_t length);
    /* Optional: the PGDevice suspend/resume state hand-over (apple-virgl plan
     * 20261118 Q4-Q6; DSGP v2 blob = reference serializeSuspendState format).
     * Plain pointer types, so the plugin builds against an older header; NULL
     * on a libmlapi without the port -> reload uses the legacy re-create. */
    int (*session_will_suspend)(qmu_session *s);
    int (*session_finish_suspend)(qmu_session *s, void **blob, size_t *length);
    int (*session_will_resume)(qmu_session *s, const void *blob, size_t length, uint32_t *error_code);
    int (*session_did_resume)(qmu_session *s);
    int (*suspend_state_validate)(const void *blob, size_t length, uint32_t *error_code);
    void (*suspend_state_free)(void *blob);   /* the blob's owning instance releases it */
    /* bit0: finishSuspend flushes every GPU-written resource to its guest backing
     * (reference flushResources).  Absent = no flush. */
    uint32_t (*suspend_state_capabilities)(void);
    /* D-RELOAD-RESIDUE: joins the instance's process-lifetime threads after its
     * last Session was destroyed.  Absent on an older build -> the retired
     * instance stays resident (RTLD_NODELETE semantics, as before). */
    int (*library_retire)(void);
    /* identity of the instance this vtable was bound from */
    void       *handle;        /* dlopen/dlmopen handle (base = the LM_ID_BASE copy) */
    long        lmid;          /* link-map namespace id (LM_ID_BASE for the base)    */
};

/* The active backend vtable (one device per process; the call-site macros read
 * it).  Bound to the base instance at setup, re-bound to the dlmopen target by
 * pvg_backend_reload(). */
extern const struct pvg_qmu_vt *pvg_qvt;

/* Resolve all routed symbols from `handle` into `vt`.  The four optional
 * private getters may be absent (stored NULL, as the old weak imports were);
 * every other symbol is mandatory and a miss fails the bind.  On failure,
 * `*err` (if non-NULL) names the first missing mandatory symbol. */
bool pvg_qmu_vt_bind(struct pvg_qmu_vt *vt, void *handle, long lmid, const char **err);

/* Route every qmu_* call site through the active vtable.  Object-like macros,
 * so both a call `qmu_foo(a)` -> `(pvg_qvt->foo)(a)` and the NULL-test of an
 * optional getter `qmu_get_private_x ? qmu_get_private_x() : NULL` rewrite
 * correctly.  Defined AFTER the struct so the __typeof__ uses above see the
 * real prototypes. */
#define qmu_create_extended                                  (pvg_qvt->create_extended)
#define qmu_destroy                                          (pvg_qvt->destroy)
#define qmu_set_debug_level                                  (pvg_qvt->set_debug_level)
#define qmu_log_set_callback                                 (pvg_qvt->log_set_callback)
#define qmu_log_category_name                                (pvg_qvt->log_category_name)
#define qmu_log_level_name                                   (pvg_qvt->log_level_name)
#define qmu_mmio_read                                        (pvg_qvt->mmio_read)
#define qmu_mmio_write                                       (pvg_qvt->mmio_write)
#define qmu_session_quiesce_host_callback_producers_deferred (pvg_qvt->session_quiesce_host_callback_producers_deferred)
#define qmu_session_rethrow_quiesce                          (pvg_qvt->session_rethrow_quiesce)
#define qmu_session_finish_retirement_for_exit               (pvg_qvt->session_finish_retirement_for_exit)
#define qmu_session_get_vulkan                               (pvg_qvt->session_get_vulkan)
#define qmu_session_acquire_host_work                        (pvg_qvt->session_acquire_host_work)
#define qmu_session_current_host_work                        (pvg_qvt->session_current_host_work)
#define qmu_session_invoke_host_work                         (pvg_qvt->session_invoke_host_work)
#define qmu_session_release_host_work                        (pvg_qvt->session_release_host_work)
#define qmu_session_retain_host_work                         (pvg_qvt->session_retain_host_work)
#define qmu_get_private_mapped_transport_v1                  (pvg_qvt->get_private_mapped_transport_v1)
#define qmu_get_private_guest_ram_region_v1                  (pvg_qvt->get_private_guest_ram_region_v1)
#define qmu_get_private_ordinary_physical_capture_v2         (pvg_qvt->get_private_ordinary_physical_capture_v2)
#define qmu_get_private_ordinary_physical_comparison_v1      (pvg_qvt->get_private_ordinary_physical_comparison_v1)
#define qmu_get_private_ordinary_task_memory_v1              (pvg_qvt->get_private_ordinary_task_memory_v1)
#define qmu_iosfc_present_tick                               (pvg_qvt->iosfc_present_tick)
#define qmu_display_copy_selected_current_target             (pvg_qvt->display_copy_selected_current_target)
#define qmu_display_publish_mode_target                      (pvg_qvt->display_publish_mode_target)
#define qmu_display_target_release                           (pvg_qvt->display_target_release)
#define qmu_get_display_cursor_position                      (pvg_qvt->get_display_cursor_position)
#define qmu_owned_display_frame_release                      (pvg_qvt->owned_display_frame_release)
#define qmu_vk_consume_current_frame_signal                  (pvg_qvt->vk_consume_current_frame_signal)
#define qmu_vk_encode_submit_owned_display_frame             (pvg_qvt->vk_encode_submit_owned_display_frame)
#define qmu_vk_prepare_owned_display_frame                   (pvg_qvt->vk_prepare_owned_display_frame)
#define qmu_display_set_mode_list                            (pvg_qvt->display_set_mode_list)
#define qmu_session_reset_for_device_reset                   (pvg_qvt->session_reset_for_device_reset)

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
    uint32_t reserved;           /* DISP-D1: buffers a frame in flight reads back into (bit per slot) */
    int dmabuf_fd;               /* DISP-D1: udmabuf over the whole memfd (RADV import path), -1 if none */
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
    /* Firmware-phase PCI identity (fw-vendor-id/fw-device-id; both or neither).
     * While armed, config space carries these ids; the first BAR0 access after
     * a device reset switches it to pci_vendor_id/pci_device_id; a device or
     * FLR reset re-arms.  Accessed on the vfu loop thread only. */
    uint32_t fw_vendor_id, fw_device_id;
    bool fw_identity;             /* configured */
    /* guest-page-shift= (ARM64-PVG-PFN-01): the guest's page unit for every guest page number QMetal decodes:
     * 12 = 4 KiB (x86-64 guests, the default), 14 = 16 KiB (arm64 guests). */
    uint32_t guest_page_shift;
    bool fw_identity_armed;
    uint64_t fw_identity_switches;
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
    const struct qmu_private_guest_ram_region_v1 *guest_ram_region_peer;
    bool guest_ram_region_installed;

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

    /* warm-swap VARIABLE backing (qdp.h v3 / pvg_backing.h, L-WARMSWAP-2).
     * The FROZEN envelope is the PVG MMIO/FIFO register contract; only the
     * advertised backend identity behind it is swapped under quiesce. */
    pthread_mutex_t backing_lock;
    void  *backing_h;             /* dlopen handle of the active backing .so (NULL = builtin A) */
    char   backing_tag[32];       /* current backing tag, for logging + device-abi               */
    char   backend_version[64];   /* advertised backend identity (the guest ioreg-triplet value) */

    /* in-process REAL-renderer reload (L-WARMSWAP-INPROC-RELOAD): destroy the
     * live Session, dlmopen a new co-resident libmlapi build, re-create the
     * Session from the parked state.  All under backing_lock. */
    struct pvg_qmu_vt qvt_storage; /* storage the global pvg_qvt points at          */
    void  *qmu_ns;                 /* current reload instance handle (NULL = base)   */
    void  *qmu_retired_ns;         /* destroyed + retired copy, unloaded once the next
                                    * instance's vtable is live (NULL = none pending) */
    long    qmu_lmid;              /* its link-map namespace id                      */
    char   *qmu_path;             /* path the active instance was loaded from (g_strdup) */
    char    last_reload_error[512]; /* why the last reload was refused before any teardown
                                    * (dlopen / entry-point reason; "" after a good one) */
    uint64_t reload_count;         /* successful in-process reloads so far           */
    uint64_t handshake_generation; /* bumped on every reload device-reset: the host- *
                                    * side stand-in for the guest kext's Handshake-  *
                                    * Generation re-handshake (live gate d measures  *
                                    * the real guest bump).                          */
    char   backend_banner[192];    /* last "[qmetal] libmlapi built ... git=.. marker=.." seen */
    uint64_t qmu_error_count;      /* QMU [ERROR] log lines (health gate)            */
    uint64_t qmu_gpu_restart_count;/* QMetal gpuRestart/GPU-hang log lines (health)  */
} PvgState;

/* ------------------------------------------------------------ logging */
void pvg_log(PvgState *s, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
void pvg_log_direct(PvgState *s, const char *text);

/* ------------------------------------------- in-process real-renderer reload */
/* Destroy the live Session, load `new_path` as a co-resident libmlapi via
 * dlmopen(LM_ID_NEWLM), re-bind the backend vtable to it and re-create the
 * Session from the parked PvgState.  reset != 0 arms the firmware identity and
 * bumps handshake_generation (the in-place device-reset re-handshake).  The
 * caller (qdev-host) has already quiesced the device (memory write-lock held,
 * IRQ held) and resumes after.  Returns 0 or -errno; on any failure the prior
 * instance is left torn down and the Session NULL (the caller rolls back by
 * reloading the previous path).  Not thread-safe against MMIO: quiesced only. */
int pvg_backend_reload(PvgState *s, const char *new_path, int reset);

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
/* qmu_guest_ram_region_v1.h: bounds of the mappable region holding gpa; no pin, no log */
int pvg_guest_ram_region(void *ctx, uint64_t gpa, uint64_t *base, uint64_t *length);

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

#endif /* UNIQ_VGPU_H */
