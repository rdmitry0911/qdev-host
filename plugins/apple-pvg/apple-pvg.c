/*
 * apple-pvg plugin for qdev-host: Apple ParavirtualizedGraphics (PVG) PCI
 * personality 106b:eeee (class 03.80), optionally 106b:eee1, served to a
 * stock QEMU through vfio-user-pci. Port of the apple-gfx-ml device of the
 * apple-virgl QEMU fork (hw/display/apple-gfx-ml.c at c43589029b) without its
 * env-gated guest-scheduler diagnostics (vCPU state, breakpoints).
 *
 *   PCI   106b:eeee rev 0, no subsystem IDs, class 03.80, PCIe config space,
 *         one capability: MSI, 1 vector, 64-bit, no per-vector mask; no INTx
 *   BAR0  16 KiB MMIO, 32-bit non-prefetchable, every access trapped and
 *         served by QMetal's qmu_mmio_read/qmu_mmio_write (4-byte units)
 *   ROM   none here: the client loads it (vfio-user-pci,romfile=...)
 *
 * Device rendering, FIFOs and guest protocol live in QMetal (libmlapi.so,
 * qmu_create_extended). The plugin provides QMetal's host callbacks over the
 * host's DMA regions, raises MSI through qdp_host_ops, and presents the
 * display through a triple-buffered memfd scanout with plugin damage.
 *
 * Plugin args (comma separated; the fork's property names are accepted):
 *   xres=1920,yres=1080           primary mode (mode 0)
 *   vram_size_mb=256 (vram=)      internal VRAM size (not a BAR); default 512
 *   debug=0                       QMetal debug level
 *   vsync=on, direct_scanout=off  passed to QMetal
 *   spirv_cache_dir=PATH          SPIR-V / VkPipelineCache directory
 *   device-id=0xeee1 (x-pci-device-id=), vendor-id= (x-pci-vendor-id=)
 *   log=stderr|host               device log sink (default stderr, the fork's
 *                                 "[apple-gfx-ml]" lines unchanged)
 *   exit-on-disconnect=on         after the client is gone: stop the device
 *                                 and exit the process (default on)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "pvg.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "qmu/pvg_regs.h"
#include "pci_caps/msi.h"

/* Declared in QMetal's C++ qmu_vulkan.h, which C cannot include. */
struct qmu_vulkan_ctx;
void qmu_vk_consume_current_frame_signal(struct qmu_vulkan_ctx *ctx);

/* QMetal private peers; absence is checked like the fork's weak imports. */
#pragma weak qmu_get_private_ordinary_physical_capture_v2
#pragma weak qmu_get_private_ordinary_physical_comparison_v1
#pragma weak qmu_get_private_ordinary_task_memory_v1
#pragma weak qmu_get_private_mapped_transport_v1

#define APPLE_GFX_ML_VENDOR_ID          0x106b
#define APPLE_GFX_ML_DEVICE_ID          0xeeee
#define APPLE_GFX_ML_MMIO_SIZE          (16 * 1024)
#define APPLE_GFX_ML_DEFAULT_VRAM_MB    512

/* Log throttling: first N events, then every Mth (fork values) */
#define AGFX_LOG_INITIAL_COUNT  10
#define AGFX_LOG_INTERVAL       60
#define AGFX_LOG_QUEUE_LIMIT    8192
#define AGFX_DISPLAY_FRAME_INTERVAL_MS  100  /* ~10Hz, matches reference */
#define AGFX_QUIT_STOP_LIMIT_MS 20000
#define AGFX_QUIT_NOTICE_LIMIT_MS 1000
#define PIXMAN_x8r8g8b8 0x20020888u
#define PVG_SCANOUT_SLOTS 3

/* libvfio-user region callbacks get no private pointer (one device per
 * process, as tb-vga). */
static PvgState *g;

static bool pvg_diag_mmio_roundtrip;
static bool pvg_diag_mmio_all;

static int64_t pvg_now_us(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static bool pvg_log_should_emit(uint64_t *counter)
{
    uint64_t value = ++(*counter);

    return value <= AGFX_LOG_INITIAL_COUNT || (value % AGFX_LOG_INTERVAL) == 0;
}

/* =========================================================== logging
 * Fork: agfx-log writer thread; hot paths enqueue formatted lines and one
 * thread writes them (to stderr, the fork's qemu_log sink). */
struct PvgLogEntry {
    PvgLogEntry *next;
    char *text;
};

static bool pvg_host_open(PvgState *s)
{
    return s && pvg_atomic_read(&s->host_open);
}

static void pvg_log_sink(PvgState *s, const char *text)
{
    /* outside the host-services window the device log goes to stderr */
    if (s && s->log_to_host && s->host_ops && pvg_host_open(s)) {
        size_t n = strlen(text);
        int level = strstr(text, "[ERROR]") ? 0 : strstr(text, "[WARN]") ? 1 : 2;

        while (n && text[n - 1] == '\n') {
            n--;
        }
        s->host_ops->log(s->host_ops->host, level, "%.*s", (int)n, text);
        return;
    }
    fputs(text, stderr);
}

void pvg_log_direct(PvgState *s, const char *text)
{
    if (text) {
        pvg_log_sink(s, text);
    }
}

static void *pvg_log_thread(void *opaque)
{
    PvgState *s = opaque;

    pthread_mutex_lock(&s->log_lock);
    for (;;) {
        PvgLogEntry *entry = s->log_head;
        uint64_t dropped = s->log_dropped;

        if (!entry && !dropped) {
            if (s->log_stop) {
                break;
            }
            pthread_cond_wait(&s->log_cond, &s->log_lock);
            continue;
        }
        if (entry) {
            s->log_head = entry->next;
            if (!s->log_head) {
                s->log_tail = NULL;
            }
            s->log_depth--;
        }
        s->log_dropped = 0;
        pthread_mutex_unlock(&s->log_lock);
        if (dropped) {
            char line[96];

            snprintf(line, sizeof(line), "[apple-gfx-ml][logger] dropped %" PRIu64
                     " log lines\n", dropped);
            pvg_log_sink(s, line);
        }
        if (entry) {
            pvg_log_sink(s, entry->text);
            g_free(entry->text);
            g_free(entry);
        }
        pthread_mutex_lock(&s->log_lock);
    }
    pthread_mutex_unlock(&s->log_lock);
    return NULL;
}

static void pvg_log_init(PvgState *s)
{
    pthread_mutex_init(&s->log_lock, NULL);
    pthread_cond_init(&s->log_cond, NULL);
    s->log_started = pthread_create(&s->log_thread, NULL, pvg_log_thread, s) == 0;
    if (s->log_started) {
        pthread_setname_np(s->log_thread, "agfx-log");
    }
}

/* Drains the queue, then stops the writer. */
static void pvg_log_stop(PvgState *s)
{
    if (!s->log_started) {
        return;
    }
    pthread_mutex_lock(&s->log_lock);
    s->log_stop = true;
    pthread_cond_signal(&s->log_cond);
    pthread_mutex_unlock(&s->log_lock);
    pthread_join(s->log_thread, NULL);
    s->log_started = false;
}

static void pvg_enqueue_log_owned(PvgState *s, char *text)
{
    PvgLogEntry *entry;

    if (!text) {
        return;
    }
    if (!s || !s->log_started) {
        pvg_log_sink(s, text);
        g_free(text);
        return;
    }
    pthread_mutex_lock(&s->log_lock);
    if (s->log_stop || s->log_depth >= AGFX_LOG_QUEUE_LIMIT) {
        if (s->log_stop) {
            pthread_mutex_unlock(&s->log_lock);
            pvg_log_sink(s, text);
            g_free(text);
            return;
        }
        s->log_dropped++;
        pthread_mutex_unlock(&s->log_lock);
        g_free(text);
        return;
    }
    entry = g_new0(PvgLogEntry, 1);
    entry->text = text;
    if (s->log_tail) {
        s->log_tail->next = entry;
    } else {
        s->log_head = entry;
    }
    s->log_tail = entry;
    s->log_depth++;
    pthread_cond_signal(&s->log_cond);
    pthread_mutex_unlock(&s->log_lock);
}

void pvg_log(PvgState *s, const char *fmt, ...)
{
    va_list args;
    char *text;

    va_start(args, fmt);
    text = g_strdup_vprintf(fmt, args);
    va_end(args);
    pvg_enqueue_log_owned(s, text);
}

/* fork agfx_qmu_log_callback: same line format */
static void pvg_qmu_log_callback(void *ctx, qmu_log_level level,
                                 qmu_log_category category, const char *file,
                                 int line, const char *func, const char *fmt,
                                 va_list args) G_GNUC_PRINTF(7, 0);
static void pvg_qmu_log_callback(void *ctx, qmu_log_level level,
                                 qmu_log_category category, const char *file,
                                 int line, const char *func, const char *fmt,
                                 va_list args)
{
    PvgState *s = ctx;
    const char *base = file;
    char *body, *full;

    if (!fmt) {
        return;
    }
    if (file) {
        const char *slash = strrchr(file, '/');
        if (slash && slash[1]) {
            base = slash + 1;
        }
    }
    body = g_strdup_vprintf(fmt, args);
    full = g_strdup_printf("[apple-gfx-ml][qmetal][%s][%s] %s:%d %s: %s\n",
                           qmu_log_level_name(level),
                           qmu_log_category_name(category),
                           base ? base : "?", line, func ? func : "?",
                           body ? body : "");
    g_free(body);
    pvg_enqueue_log_owned(s, full);
}

/* =========================================================== host work
 * Fork AgfxHostWork: one accepted Session operation. A child retains its
 * parent's work before publication and releases it after its function has
 * returned. */
static PvgHostWork pvg_host_work_ref(PvgState *s, bool root)
{
    PvgHostWork owner = { .state = s };

    if (s->qmu_dev) {
        owner.work = root ? qmu_session_acquire_host_work(s->qmu_dev) :
            qmu_session_retain_host_work(qmu_session_current_host_work(s->qmu_dev));
        /* callbacks arrive in their own Session scope (fork: assert) */
        if (!root) {
            g_assert(owner.work);
        }
    }
    return owner;
}

static void pvg_host_work_unref(PvgHostWork *owner)
{
    qmu_session_work *work = owner->work;

    owner->work = NULL;
    owner->state = NULL;
    if (work) {
        qmu_session_release_host_work(work);
    }
}

static void pvg_host_work_invoke(const PvgHostWork *owner, void (*fn)(void *),
                                 void *opaque)
{
    if (owner->work) {
        qmu_session_invoke_host_work(owner->work, fn, opaque);
    } else {
        fn(opaque);
    }
}

/* =========================================================== device loop
 * The fork's QEMU main loop (BQL) plane: one thread runs these jobs in FIFO
 * order. A job is the fork's AgfxHostBH envelope. */
struct PvgJob {
    PvgJob *next;
    PvgHostWork owner;
    void (*fn)(void *);
    void *opaque;
    /* one transferred terminal reference spans the leaf's return */
    qmu_owned_display_frame *terminal_frame;
    /* synchronous call from a non-loop thread */
    bool sync;
    bool done;
};

static void pvg_loop_enqueue(PvgState *s, PvgJob *job)
{
    job->next = NULL;
    pthread_mutex_lock(&s->loop_lock);
    if (s->loop_tail) {
        s->loop_tail->next = job;
    } else {
        s->loop_head = job;
    }
    s->loop_tail = job;
    pthread_cond_broadcast(&s->loop_cond);
    pthread_mutex_unlock(&s->loop_lock);
}

static void *pvg_loop_thread(void *opaque)
{
    PvgState *s = opaque;

    pthread_mutex_lock(&s->loop_lock);
    for (;;) {
        PvgJob *job = s->loop_head;

        if (!job) {
            if (s->loop_stop) {
                break;
            }
            pthread_cond_wait(&s->loop_cond, &s->loop_lock);
            continue;
        }
        s->loop_head = job->next;
        if (!s->loop_head) {
            s->loop_tail = NULL;
        }
        pthread_mutex_unlock(&s->loop_lock);

        pvg_host_work_invoke(&job->owner, job->fn, job->opaque);
        if (job->terminal_frame) {
            qmu_owned_display_frame_release(job->terminal_frame);
            job->terminal_frame = NULL;
        }
        pvg_host_work_unref(&job->owner);

        pthread_mutex_lock(&s->loop_lock);
        if (job->sync) {
            job->done = true;
            pthread_cond_broadcast(&s->loop_cond);
        } else {
            g_free(job);
        }
    }
    pthread_mutex_unlock(&s->loop_lock);
    return NULL;
}

/* fork agfx_schedule_host_bh */
static void pvg_schedule_host_bh(PvgState *s, void (*fn)(void *), void *opaque)
{
    PvgJob *job = g_new0(PvgJob, 1);

    job->owner = pvg_host_work_ref(s, false);
    job->fn = fn;
    job->opaque = opaque;
    pvg_loop_enqueue(s, job);
}

/* Run fn on the device loop outside any Session work and wait for it. */
static void pvg_loop_run_sync(PvgState *s, void (*fn)(void *), void *opaque)
{
    PvgJob job = { .fn = fn, .opaque = opaque, .sync = true };

    pvg_loop_enqueue(s, &job);
    pthread_mutex_lock(&s->loop_lock);
    while (!job.done) {
        pthread_cond_wait(&s->loop_cond, &s->loop_lock);
    }
    pthread_mutex_unlock(&s->loop_lock);
}

/* =========================================================== interrupts
 * Fork qemu_raise_irq: any QMetal thread; msi_notify only if the guest has
 * enabled MSI, else the IRQ is dropped. Here irq_trigger() does both (it
 * returns -EINVAL and drops the event while MSI is disabled). While the host
 * changes the guest memory map (quiesce .. resume) no trigger runs: a raise
 * is latched and delivered by resume (MSI is edge-like; the guest reads the
 * read-and-clear status registers). */
static void pvg_raise_irq(void *ctx, uint32_t vector)
{
    PvgState *s = ctx;
    uint64_t n;
    int rc = -EINVAL;
    int gate;

    if (!s) {
        return;
    }
    pthread_mutex_lock(&s->irq_lock);
    gate = s->irq_gate;
    if (gate == PVG_IRQ_OPEN) {
        rc = s->host_ops->irq_trigger(s->host_ops->host, vector);
    } else if (gate == PVG_IRQ_HELD) {
        s->irq_pending = true;
    } else {
        s->irq_dropped++;
    }
    pthread_mutex_unlock(&s->irq_lock);

    n = pvg_atomic_fetch_inc(&s->irq_count) + 1;
    if (n <= AGFX_LOG_INITIAL_COUNT || (n % AGFX_LOG_INTERVAL) == 0) {
        if (gate == PVG_IRQ_OPEN) {
            pvg_log(s, "[apple-gfx-ml] raise_irq #%lu: msi_enabled=%d\n",
                    (unsigned long)n, rc == 0);
        } else {
            pvg_log(s, "[apple-gfx-ml] raise_irq #%lu: %s\n", (unsigned long)n,
                    gate == PVG_IRQ_HELD ? "held over a guest memory map change"
                                         : "dropped, device stopped");
        }
    }
}

static void pvg_irq_hold(PvgState *s)
{
    pthread_mutex_lock(&s->irq_lock);
    if (s->irq_gate == PVG_IRQ_OPEN) {
        s->irq_gate = PVG_IRQ_HELD;
    }
    pthread_mutex_unlock(&s->irq_lock);
}

static void pvg_irq_release(PvgState *s)
{
    pthread_mutex_lock(&s->irq_lock);
    if (s->irq_gate == PVG_IRQ_HELD) {
        s->irq_gate = PVG_IRQ_OPEN;
        if (s->irq_pending) {
            s->irq_pending = false;
            (void)s->host_ops->irq_trigger(s->host_ops->host, 0);
        }
    }
    pthread_mutex_unlock(&s->irq_lock);
}

static void pvg_irq_drop(PvgState *s)
{
    pthread_mutex_lock(&s->irq_lock);
    s->irq_gate = PVG_IRQ_DROP;
    s->irq_pending = false;
    pthread_mutex_unlock(&s->irq_lock);
}

/* =========================================================== scanout
 * The fork copied each completed frame into the QEMU console surface. Here
 * QMetal reads the CURRENT target straight into one of three memfd buffers.
 * A buffer is never written while it is the published front or the one the
 * host fetched last through get_scanout(), so the host, which reads the
 * buffer of its latest get_scanout() call, never sees a half-written frame. */
static int pvg_scanout_init(PvgState *s, size_t max_frame)
{
    PvgScanout *sc = &s->scan;
    size_t slot = (max_frame + pvg_page_size() - 1) & ~(pvg_page_size() - 1);

    pthread_mutex_init(&sc->lock, NULL);
    sc->front = -1;
    sc->held = -1;
    sc->nslots = PVG_SCANOUT_SLOTS;
    sc->fd = memfd_create("apple-pvg-scanout", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (sc->fd < 0 || ftruncate(sc->fd, (off_t)(slot * sc->nslots)) < 0) {
        return -errno;
    }
    /* lets the host wrap it into a dma-buf (udmabuf) */
    fcntl(sc->fd, F_ADD_SEALS, F_SEAL_SHRINK);
    sc->map = mmap(NULL, slot * sc->nslots, PROT_READ | PROT_WRITE, MAP_SHARED, sc->fd, 0);
    if (sc->map == MAP_FAILED) {
        sc->map = NULL;
        return -errno;
    }
    sc->slot_size = slot;
    return 0;
}

/* device loop: room for one frame of frame_size bytes in every buffer */
static bool pvg_scanout_ensure(PvgState *s, size_t frame_size)
{
    PvgScanout *sc = &s->scan;
    size_t slot = (frame_size + pvg_page_size() - 1) & ~(pvg_page_size() - 1);
    uint8_t *map;

    if (frame_size <= sc->slot_size) {
        return true;
    }
    if (ftruncate(sc->fd, (off_t)(slot * sc->nslots)) < 0) {
        pvg_log(s, "[apple-gfx-ml][display][ERROR] scanout grow to %zu: %s\n",
                slot * sc->nslots, strerror(errno));
        return false;
    }
    map = mmap(NULL, slot * sc->nslots, PROT_READ | PROT_WRITE, MAP_SHARED, sc->fd, 0);
    if (map == MAP_FAILED) {
        pvg_log(s, "[apple-gfx-ml][display][ERROR] scanout map: %s\n", strerror(errno));
        return false;
    }
    pthread_mutex_lock(&sc->lock);
    munmap(sc->map, sc->slot_size * sc->nslots);
    sc->map = map;
    sc->slot_size = slot;
    sc->front = -1;          /* offsets moved: the host refetches */
    sc->held = -1;
    pthread_mutex_unlock(&sc->lock);
    pvg_log(s, "[apple-gfx-ml][display] scanout buffers grown to %zu bytes\n", slot);
    return true;
}

static int pvg_scanout_back(PvgState *s)
{
    PvgScanout *sc = &s->scan;
    int idx = 0;

    pthread_mutex_lock(&sc->lock);
    while (idx == sc->front || idx == sc->held) {
        idx++;
    }
    pthread_mutex_unlock(&sc->lock);
    return idx;
}

static uint8_t *pvg_scanout_buffer(PvgState *s, int idx)
{
    return s->scan.map + (size_t)idx * s->scan.slot_size;
}

/* Publish buffer idx (or, with idx < 0, the unchanged front) as a frame. */
static void pvg_scanout_publish(PvgState *s, int idx)
{
    PvgScanout *sc = &s->scan;
    qdp_frame_info info = { 0 };
    bool have;

    pthread_mutex_lock(&sc->lock);
    if (idx >= 0) {
        sc->front = idx;
        sc->width = s->fb_width;
        sc->height = s->fb_height;
        sc->stride = s->fb_stride;
    }
    have = sc->front >= 0;
    info.seq = ++sc->seq;
    pthread_mutex_unlock(&sc->lock);
    /* the initial (setup-time) frame is fetched by the host's get_scanout() */
    if (have && pvg_host_open(s)) {
        s->host_ops->frame_ready(s->host_ops->host, &info);
    }
}

/* =========================================================== display mode
 * fork agfx_publish_display_mode: a new geometry gets a new (black) surface,
 * published at once as the console's replace_surface did. */
static void pvg_publish_display_mode(PvgState *s, uint32_t width, uint32_t height,
                                     uint32_t iosurface_pixel_format,
                                     uint64_t protection_requirements)
{
    bool replace;
    uint32_t stride = width * 4u;

    if (width == 0 || height == 0) {
        return;
    }
    replace = !s->display_published || s->fb_width != width || s->fb_height != height;
    s->fb_width = width;
    s->fb_height = height;
    s->fb_stride = stride;
    s->fb_iosurface_pixel_format = iosurface_pixel_format;
    s->fb_protection_requirements = protection_requirements;
    s->display_published = true;
    if (replace && pvg_scanout_ensure(s, (size_t)height * stride)) {
        int idx = pvg_scanout_back(s);

        memset(pvg_scanout_buffer(s, idx), 0, (size_t)height * stride);
        pvg_scanout_publish(s, idx);
    }
}

/* fork apple_gfx_ml_apply_mode_change, device loop */
static void pvg_apply_mode_change(PvgState *s, uint32_t width, uint32_t height,
                                  uint32_t iosurface_pixel_format,
                                  uint64_t protection_requirements)
{
    struct qmu_display_target *next = NULL;
    struct qmu_display_target *outgoing;
    qmu_status status = QMU_OK;

    if (!s || width == 0 || height == 0) {
        return;
    }
    /* Equal geometry keeps the physical owner, including nil after an
     * allocation failure; only metadata changes. */
    if (s->display_published && s->fb_width == width && s->fb_height == height) {
        pvg_publish_display_mode(s, width, height, iosurface_pixel_format,
                                 protection_requirements);
        return;
    }
    pvg_log(s, "[apple-gfx-ml] mode_change: %ux%u iosurface_pf=0x%08x protection=0x%016"
            PRIx64 "\n", width, height, iosurface_pixel_format, protection_requirements);
    if (s->qmu_dev) {
        struct qmu_vulkan_ctx *vk = qmu_session_get_vulkan(s->qmu_dev);

        status = qmu_display_publish_mode_target(vk, width, height, &next);
        g_assert(status == QMU_OK || next == NULL);
    }
    outgoing = s->display_target;
    s->display_target = next;
    pvg_publish_display_mode(s, width, height, iosurface_pixel_format,
                             protection_requirements);
    qmu_display_target_release(outgoing);
    if (status != QMU_OK) {
        pvg_log(s, "[apple-gfx-ml] mode_change: native target unavailable status=%d\n",
                status);
    }
}

static void pvg_record_render_start_mode(PvgState *s)
{
    s->rendering_frame_width = s->fb_width;
    s->rendering_frame_height = s->fb_height;
}

static bool pvg_render_start_mode_is_current(const PvgState *s)
{
    return s->rendering_frame_width == s->fb_width &&
           s->rendering_frame_height == s->fb_height;
}

/* =========================================================== frame chain */
typedef struct PvgFrameJob {
    PvgState *state;
    bool frame_expected;
    bool chain_needed;
    /* borrowed from the enclosing envelope through the leaf's return */
    qmu_owned_display_frame *terminal_frame;
    uint8_t *pixels;          /* compatibility producers only */
    uint32_t width, height, stride;
} PvgFrameJob;

struct PvgRenderJob {
    PvgRenderJob *next;
    PvgState *state;
    PvgHostWork owner;
    qmu_owned_display_frame *prepared_frame;
    /* completion envelope and its work, owned before native prepare */
    PvgJob *completion;
};

static void pvg_frame_completed_bh(void *opaque);
static void pvg_frame_completed(void *ctx, int frame_expected);

static void pvg_free_frame_job(PvgFrameJob *job)
{
    if (job) {
        g_free(job->pixels);
        g_free(job);
    }
}

static void pvg_discard_unsubmitted_completion(PvgRenderJob *job)
{
    PvgJob *completion = job->completion;

    g_assert(completion && !completion->terminal_frame);
    pvg_free_frame_job(completion->opaque);
    completion->opaque = NULL;
    pvg_host_work_unref(&completion->owner);
    g_free(completion);
    job->completion = NULL;
}

static void pvg_render_job_destroy(PvgRenderJob *job)
{
    g_assert(!job->completion);
    qmu_owned_display_frame_release(job->prepared_frame);
    pvg_host_work_unref(&job->owner);
    g_free(job);
}

static void pvg_render_submit(PvgState *s, PvgRenderJob *job)
{
    job->next = NULL;
    pthread_mutex_lock(&s->render_lock);
    if (s->render_tail) {
        s->render_tail->next = job;
    } else {
        s->render_head = job;
    }
    s->render_tail = job;
    pthread_cond_signal(&s->render_cond);
    pthread_mutex_unlock(&s->render_lock);
}

/* fork agfx_render_submit_call: background encode of the owned frame */
static void pvg_render_submit_call(void *opaque)
{
    PvgRenderJob *job = opaque;
    PvgState *s = job->state;
    int rc;

    g_assert(s && s->qmu_dev && job->prepared_frame);
    rc = qmu_vk_encode_submit_owned_display_frame(job->prepared_frame);
    if (rc > 0) {
        /* an inline done may already own the envelope */
        job->completion = NULL;
    }
    qmu_owned_display_frame_release(job->prepared_frame);
    job->prepared_frame = NULL;
    if (rc <= 0) {
        pvg_discard_unsubmitted_completion(job);
    }
    if (rc > 0) {
        if (pvg_log_should_emit(&s->render_worker_log_count)) {
            pvg_log(s, "[apple-gfx-ml] render_worker: submit_owned_output pending_frames=%d"
                    " mmio_wait=%d\n", pvg_atomic_read(&s->pending_frames), 0);
        }
        return;
    }
    if (pvg_log_should_emit(&s->render_worker_log_count)) {
        pvg_log(s, "[apple-gfx-ml] render_worker: submit_owned_output rc=%d pending_frames=%d\n",
                rc, pvg_atomic_read(&s->pending_frames));
    }
    pvg_frame_completed(s, 0);
}

static void *pvg_render_thread(void *opaque)
{
    PvgState *s = opaque;

    pthread_mutex_lock(&s->render_lock);
    for (;;) {
        PvgRenderJob *job = s->render_head;

        if (!job) {
            if (s->render_stop) {
                break;
            }
            pthread_cond_wait(&s->render_cond, &s->render_lock);
            continue;
        }
        s->render_head = job->next;
        if (!s->render_head) {
            s->render_tail = NULL;
        }
        pthread_mutex_unlock(&s->render_lock);
        pvg_host_work_invoke(&job->owner, pvg_render_submit_call, job);
        pvg_render_job_destroy(job);
        pthread_mutex_lock(&s->render_lock);
    }
    pthread_mutex_unlock(&s->render_lock);
    return NULL;
}

/* fork qemu_owned_display_done: any QMetal thread, adopts the terminal
 * reference into the preowned envelope and queues it on the device loop. */
static void pvg_owned_display_done(void *ctx, qmu_owned_display_frame *owned_terminal,
                                   const qmu_render_frame_completion *completion)
{
    PvgJob *envelope = ctx;
    PvgFrameJob *job = envelope ? envelope->opaque : NULL;
    PvgState *s = job ? job->state : NULL;

    g_assert(s && owned_terminal && completion);
    g_assert(!envelope->terminal_frame && !job->terminal_frame);
    envelope->terminal_frame = owned_terminal;
    job->terminal_frame = owned_terminal;
    job->frame_expected = completion->frame_expected != 0;
    pvg_loop_enqueue(s, envelope);
}

/* fork agfx_kick_display_render, device loop */
static void pvg_kick_display_render(PvgState *s, struct qmu_vulkan_ctx *vk)
{
    PvgRenderJob *job;
    PvgFrameJob *frame;
    int rc;

    if (!s || !vk) {
        return;
    }
    pvg_record_render_start_mode(s);
    job = g_new0(PvgRenderJob, 1);
    job->state = s;
    job->owner = pvg_host_work_ref(s, false);
    job->completion = g_new0(PvgJob, 1);
    job->completion->owner = pvg_host_work_ref(s, false);
    job->completion->fn = pvg_frame_completed_bh;
    frame = g_new0(PvgFrameJob, 1);
    frame->state = s;
    job->completion->opaque = frame;
    rc = qmu_vk_prepare_owned_display_frame(vk, s->display_target, pvg_owned_display_done,
                                            job->completion, &job->prepared_frame);
    if (rc > 0) {
        if (pvg_log_should_emit(&s->render_worker_log_count)) {
            pvg_log(s, "[apple-gfx-ml] kick_display_render: prepared_output pending_frames=%d"
                    " mmio_wait=%d\n", pvg_atomic_read(&s->pending_frames), 0);
        }
        pvg_render_submit(s, job);
        return;
    }
    if (pvg_log_should_emit(&s->render_worker_log_count)) {
        pvg_log(s, "[apple-gfx-ml] kick_display_render: prepare_output rc=%d pending_frames=%d\n",
                rc, pvg_atomic_read(&s->pending_frames));
    }
    /* a returned rejection is definitely unsubmitted and owes no callback */
    g_assert(!job->prepared_frame);
    pvg_discard_unsubmitted_completion(job);
    pvg_render_job_destroy(job);
    pvg_frame_completed(s, 0);
}

typedef struct PvgRenderKickoff {
    PvgState *state;
    struct qmu_vulkan_ctx *vk;
} PvgRenderKickoff;

static void pvg_render_kickoff_call(void *opaque)
{
    PvgRenderKickoff *kick = opaque;

    pvg_kick_display_render(kick->state, kick->vk);
}

/* Env-gated frame diagnostics of the fork: AGFX_FRAME_CONTENT (hash) and
 * AGFX_FRAME_CAPTURE_DIR_ALL/_START/_MAX/_ARM_FILE (one PPM per present). */
static void pvg_frame_diagnostics(PvgState *s, const uint8_t *fb, uint32_t width,
                                  uint32_t height, uint32_t stride, uint64_t pc)
{
    static int fc_gate = -1;
    static int cap_init, cap_ok;
    static const char *cap_dir, *cap_arm_file;
    static unsigned long cap_start, cap_max, cap_attempts;
    static bool cap_armed;

    if (fc_gate == -1) {
        fc_gate = getenv("AGFX_FRAME_CONTENT") ? 1 : 0;
    }
    if (fc_gate) {
        uint64_t h = 1469598103934665603ULL;   /* FNV-1a */
        uint64_t nonblack = 0, total = 0;

        for (uint32_t y = 0; y < height; y += 4) {
            const uint8_t *row = fb + (size_t)y * stride;
            for (uint32_t x = 0; x < width; x += 4) {
                const uint8_t *px = row + (size_t)x * 4;
                uint32_t v = ((uint32_t)px[2] << 16) | ((uint32_t)px[1] << 8) | px[0];
                h = (h ^ v) * 1099511628211ULL;
                if (v > 0x0f0f0f) {
                    nonblack++;
                }
                total++;
            }
        }
        pvg_log(s, "[apple-gfx-ml] FRAME_CONTENT: #%lu hash=0x%08x nonblack=%d%%\n",
                (unsigned long)pc, (unsigned)(h ^ (h >> 32)),
                (int)(total ? (nonblack * 100 / total) : 0));
    }
    if (!cap_init) {
        const char *env = getenv("AGFX_FRAME_CAPTURE_DIR_ALL");
        const char *start_env = getenv("AGFX_FRAME_CAPTURE_START");
        const char *max_env = getenv("AGFX_FRAME_CAPTURE_MAX");
        const char *arm_env = getenv("AGFX_FRAME_CAPTURE_ARM_FILE");

        if (start_env && start_env[0]) {
            cap_start = strtoul(start_env, NULL, 10);
        }
        if (env && env[0]) {
            cap_dir = env;
            if (mkdir(env, 0755) == 0 || errno == EEXIST) {
                cap_ok = 1;
                pvg_log(s, "[apple-gfx-ml] AGFX_FRAME_CAPTURE_DIR_ALL=%s "
                        "(per-present PPM capture enabled)\n", env);
            } else {
                pvg_log(s, "[apple-gfx-ml] AGFX_FRAME_CAPTURE_DIR_ALL=%s "
                        "mkdir failed errno=%d\n", env, errno);
            }
        }
        if (max_env && max_env[0]) {
            char *end = NULL;

            errno = 0;
            cap_max = strtoul(max_env, &end, 10);
            if (errno || end == max_env || *end || !cap_max || cap_max > 4096) {
                cap_ok = 0;
                pvg_log(s, "FRAME_CAPTURE_FAILED reason=max_invalid\n");
            }
        }
        if (arm_env && arm_env[0]) {
            cap_arm_file = arm_env;
            if (arm_env[0] != '/' || !cap_max) {
                cap_ok = 0;
                pvg_log(s, "FRAME_CAPTURE_FAILED reason=arm_requires_absolute_path_and_max\n");
            }
        } else {
            cap_armed = true;
        }
        cap_init = 1;
    }
    if (cap_ok && !cap_armed && cap_arm_file) {
        struct stat arm;

        if (lstat(cap_arm_file, &arm) == 0) {
            if (S_ISREG(arm.st_mode) && !S_ISLNK(arm.st_mode)) {
                cap_armed = true;
                pvg_log(s, "FRAME_CAPTURE_ARMED next_present=%lu\n", (unsigned long)pc);
            } else {
                cap_ok = 0;
                pvg_log(s, "FRAME_CAPTURE_FAILED reason=arm_invalid\n");
            }
        } else if (errno != ENOENT) {
            cap_ok = 0;
            pvg_log(s, "FRAME_CAPTURE_FAILED reason=arm_unreadable\n");
        }
    }
    if (cap_ok && cap_dir && cap_armed && (!cap_max || cap_attempts < cap_max) &&
        (unsigned long)pc >= cap_start) {
        char path[PATH_MAX];
        int n;

        ++cap_attempts;
        n = snprintf(path, sizeof(path), "%s/frame_%06lu.ppm", cap_dir, (unsigned long)pc);
        if (n > 0 && n < (int)sizeof(path)) {
            FILE *f = fopen(path, "wb");

            if (f) {
                bool complete = fprintf(f, "P6\n%u %u\n255\n", width, height) > 0;
                uint8_t *rgb = g_try_malloc((size_t)width * 3);

                complete = complete && rgb;
                for (uint32_t y = 0; complete && y < height; y++) {
                    const uint8_t *row = fb + (size_t)y * stride;
                    for (uint32_t x = 0; x < width; x++) {
                        /* x8r8g8b8 is BGRX in memory; PPM is RGB */
                        rgb[x * 3 + 0] = row[x * 4 + 2];
                        rgb[x * 3 + 1] = row[x * 4 + 1];
                        rgb[x * 3 + 2] = row[x * 4 + 0];
                    }
                    complete = fwrite(rgb, 1, (size_t)width * 3, f) == (size_t)width * 3;
                }
                g_free(rgb);
                if (fclose(f) != 0) {
                    complete = false;
                }
                if (complete) {
                    pvg_log(s, "FRAME_CAPTURED n=%lu fb=%p width=%u height=%u stride=%u"
                            " path=%s\n", (unsigned long)pc, (const void *)fb, width,
                            height, stride, path);
                } else {
                    cap_ok = 0;
                    pvg_log(s, "FRAME_CAPTURE_FAILED n=%lu reason=write_or_close\n",
                            (unsigned long)pc);
                }
            } else {
                cap_ok = 0;
                pvg_log(s, "FRAME_CAPTURE_FAILED n=%lu reason=open errno=%d\n",
                        (unsigned long)pc, errno);
            }
        } else {
            cap_ok = 0;
            pvg_log(s, "FRAME_CAPTURE_FAILED n=%lu reason=path_length\n", (unsigned long)pc);
        }
        if (cap_max && cap_attempts == cap_max) {
            pvg_log(s, "FRAME_CAPTURE_LIMIT_REACHED attempts=%lu max=%lu\n",
                    cap_attempts, cap_max);
        }
    }
}

/* fork apple_gfx_ml_apply_staged_frame, device loop. QMetal reads the
 * explicit CURRENT target straight into a free scanout buffer (the fork read
 * it into a heap copy, then copied that into the console surface). */
static bool pvg_apply_staged_frame(PvgState *s, PvgFrameJob *job)
{
    uint32_t width, height, stride;
    size_t frame_size;
    bool copy_pixels = true;
    int idx;
    uint64_t pc;

    if (!s || !job || !s->qmu_dev) {
        return false;
    }
    /* A completed output whose start mode is no longer current is dropped. */
    if (!pvg_render_start_mode_is_current(s)) {
        return false;
    }
    if (job->terminal_frame) {
        qmu_render_frame_completion actual_read;

        width = s->fb_width;
        height = s->fb_height;
        stride = s->fb_stride;
        frame_size = (size_t)height * stride;
        g_assert(width && height && stride >= (uint64_t)width * 4);
        if (!pvg_scanout_ensure(s, frame_size)) {
            return false;
        }
        idx = pvg_scanout_back(s);
        /* returns only after a real read, or a nil-receiver no-op */
        (void)qmu_display_copy_selected_current_target(job->terminal_frame,
            s->display_target, pvg_scanout_buffer(s, idx), width, height, stride,
            &actual_read);
        copy_pixels = actual_read.pixels != NULL;
        job->frame_expected = actual_read.frame_expected != 0;
    } else {
        /* compatibility producer: its own immutable copy */
        if (!job->pixels || !job->width || !job->height || !job->stride) {
            return false;
        }
        width = job->width;
        height = job->height;
        stride = job->stride;
        frame_size = (size_t)height * stride;
        pvg_publish_display_mode(s, width, height, s->fb_iosurface_pixel_format,
                                 s->fb_protection_requirements);
        if (stride < width * 4u ||
            !pvg_scanout_ensure(s, (size_t)height * s->fb_stride)) {
            return false;
        }
        idx = pvg_scanout_back(s);
        /* rows of the payload's stride into the scanout's stride */
        for (uint32_t y = 0; y < height; y++) {
            memcpy(pvg_scanout_buffer(s, idx) + (size_t)y * s->fb_stride,
                   job->pixels + (size_t)y * stride, (size_t)width * 4);
        }
        stride = s->fb_stride;
    }
    /* nil read: the displayed bytes stay as they are (fork: console bytes) */
    pvg_scanout_publish(s, copy_pixels ? idx : -1);

    s->frame_count++;
    pc = pvg_atomic_fetch_inc(&s->present_count) + 1;
    if (s->scan.front >= 0) {
        pvg_frame_diagnostics(s, pvg_scanout_buffer(s, s->scan.front), width, height,
                              stride, pc);
    }
    if (pc <= AGFX_LOG_INITIAL_COUNT || (pc % AGFX_LOG_INTERVAL) == 0) {
        pvg_log(s, "[apple-gfx-ml] present_frame #%lu: %ux%u stride=%u (owner payload)\n",
                (unsigned long)pc, width, height, stride);
    }
    if (s->frame_count <= AGFX_LOG_INITIAL_COUNT || (s->frame_count % AGFX_LOG_INTERVAL) == 0) {
        pvg_log(s, "[apple-gfx-ml] frame_completed_bh: present #%lu %ux%u stride=%u\n",
                (unsigned long)s->frame_count, width, height, stride);
    }
    return true;
}

/* fork apple_gfx_ml_frame_completed_bh, device loop */
static void pvg_frame_completed_bh(void *opaque)
{
    PvgFrameJob *job = opaque;
    PvgState *s = job ? job->state : NULL;
    bool frame_applied;
    int pending;

    if (!job || !s) {
        pvg_free_frame_job(job);
        return;
    }
    pending = pvg_atomic_read(&s->pending_frames);
    if (pending > 0) {
        pending = __atomic_sub_fetch(&s->pending_frames, 1, __ATOMIC_SEQ_CST);
    }
    job->chain_needed = pending > 0;
    frame_applied = job->frame_expected ? pvg_apply_staged_frame(s, job) : false;
    if (pvg_log_should_emit(&s->frame_completed_log_count)) {
        pvg_log(s, "[apple-gfx-ml] frame_completed_bh: pending_frames=%d frame_applied=%d"
                " mmio_wait=%d\n", pvg_atomic_read(&s->pending_frames),
                frame_applied ? 1 : 0, 0);
    }
    if (job->chain_needed && s->qmu_dev) {
        struct qmu_vulkan_ctx *vk = qmu_session_get_vulkan(s->qmu_dev);

        if (!vk) {
            pvg_log(s, "[apple-gfx-ml] frame_completed_bh: vk=NULL\n");
        } else {
            PvgHostWork owner = s->deferred_frame_owner;
            PvgRenderKickoff kick = { .state = s, .vk = vk };

            g_assert(owner.state == s);
            memset(&s->deferred_frame_owner, 0, sizeof(s->deferred_frame_owner));
            pvg_host_work_invoke(&owner, pvg_render_kickoff_call, &kick);
            pvg_host_work_unref(&owner);
        }
    }
    pvg_free_frame_job(job);
}

/* fork qemu_render_frame_complete: copies the inline payload */
static void pvg_render_frame_complete(void *ctx, const qmu_render_frame_completion *completion)
{
    PvgState *s = ctx;
    PvgFrameJob *job;

    if (!s || !completion) {
        return;
    }
    job = g_new0(PvgFrameJob, 1);
    job->state = s;
    job->frame_expected = completion->frame_expected != 0;
    if (job->frame_expected && completion->pixels && completion->width &&
        completion->height && completion->stride) {
        job->pixels = g_memdup2(completion->pixels,
                                (size_t)completion->height * completion->stride);
        job->width = completion->width;
        job->height = completion->height;
        job->stride = completion->stride;
    }
    pvg_schedule_host_bh(s, pvg_frame_completed_bh, job);
}

static void pvg_frame_completed(void *ctx, int frame_expected)
{
    PvgState *s = ctx;
    PvgFrameJob *job;

    if (!s) {
        return;
    }
    job = g_new0(PvgFrameJob, 1);
    job->state = s;
    job->frame_expected = frame_expected != 0;
    pvg_schedule_host_bh(s, pvg_frame_completed_bh, job);
}

/* fork agfx_new_frame_handler_bh: pending_frames throttle and kickoff */
static void pvg_new_frame_handler_bh(void *opaque)
{
    PvgState *s = opaque;
    struct qmu_vulkan_ctx *vk;
    int pending;

    if (!s || !s->qmu_dev) {
        return;
    }
    vk = qmu_session_get_vulkan(s->qmu_dev);
    if (!vk) {
        pvg_log(s, "[apple-gfx-ml] new_frame_handler_bh: vk=NULL\n");
        return;
    }
    if (pvg_log_should_emit(&s->new_frame_handler_log_count)) {
        pvg_log(s, "[apple-gfx-ml] new_frame_handler_bh: pending_frames=%d mmio_wait=%d\n",
                pvg_atomic_read(&s->pending_frames), 0);
    }
    /* reference throttle: pending_frames >= 2 drops (apple-gfx.m:2672) */
    pending = pvg_atomic_read(&s->pending_frames);
    if (pending >= 2) {
        if (pvg_log_should_emit(&s->new_frame_handler_log_count)) {
            pvg_log(s, "[apple-gfx-ml] new_frame_handler_bh: drop pending_frames=%d\n",
                    pending);
        }
        return;
    }
    pending = __atomic_add_fetch(&s->pending_frames, 1, __ATOMIC_SEQ_CST);
    /* reference: with pending > 1 the completion chains the next frame */
    if (pending > 1) {
        g_assert(!s->deferred_frame_owner.state);
        s->deferred_frame_owner = pvg_host_work_ref(s, false);
        if (pvg_log_should_emit(&s->new_frame_handler_log_count)) {
            pvg_log(s, "[apple-gfx-ml] new_frame_handler_bh: chain-only pending_frames=%d\n",
                    pending);
        }
        return;
    }
    if (pvg_log_should_emit(&s->new_frame_handler_log_count)) {
        pvg_log(s, "[apple-gfx-ml] new_frame_handler_bh: queue_render pending_frames=%d\n",
                pending);
    }
    pvg_kick_display_render(s, vk);
}

/* fork qemu_new_frame_signal: PGDisplay newFrameEventHandler analogue */
static void pvg_new_frame_signal(void *ctx)
{
    PvgState *s = ctx;
    struct qmu_vulkan_ctx *vk = NULL;

    if (!s) {
        return;
    }
    if (s->qmu_dev) {
        vk = qmu_session_get_vulkan(s->qmu_dev);
    }
    /* acknowledge the merged signal before the callback returns */
    if (vk) {
        qmu_vk_consume_current_frame_signal(vk);
    }
    if (pvg_log_should_emit(&s->new_frame_signal_log_count)) {
        pvg_log(s, "[apple-gfx-ml] new_frame_signal: enqueue pending_frames=%d mmio_wait=%d\n",
                pvg_atomic_read(&s->pending_frames), 0);
    }
    pvg_schedule_host_bh(s, pvg_new_frame_handler_bh, s);
}

/* fork qemu_schedule_display_completion: the serial PGDisplay queue; each
 * item runs as its own device-loop job, in order. */
static void pvg_schedule_display_completion(void *ctx, void (*fn)(void *), void *comp_ctx)
{
    PvgState *s = ctx;
    PvgJob *job;

    if (!s || !fn) {
        return;
    }
    job = g_new0(PvgJob, 1);
    job->owner = pvg_host_work_ref(s, false);
    job->fn = fn;
    job->opaque = comp_ctx;
    pvg_log(s, "[apple-gfx-ml] schedule_display_completion: enqueue mmio_wait=%d\n", 0);
    pvg_loop_enqueue(s, job);
}

static void pvg_mode_change(void *ctx, uint32_t width, uint32_t height,
                            uint32_t iosurface_pixel_format,
                            uint64_t protection_requirements)
{
    PvgState *s = ctx;

    if (!s || width == 0 || height == 0) {
        return;
    }
    pvg_apply_mode_change(s, width, height, iosurface_pixel_format,
                          protection_requirements);
}

/* =========================================================== bootstrap presents
 * fork agfx-present: the PGEFIPresentQueue analogue owning the 100 ms timer
 * of scheduleFramePresents and its mergeable present source. */
typedef enum { PVG_PRESENT_SCHEDULE, PVG_PRESENT_CANCEL } PvgPresentKind;

struct PvgPresentCommand {
    PvgHostWork owner;
    PvgPresentCommand *next;
    PvgPresentKind kind;
};

static void pvg_present_merge_source_locked(PvgState *s, bool root)
{
    if (!s->present_source_pending) {
        s->present_source_owner = pvg_host_work_ref(s, root);
        s->present_source_pending = true;
        pthread_cond_signal(&s->present_cond);
    }
}

static void pvg_present_cancel_locked(PvgState *s)
{
    s->present_timer_active = false;
    s->present_next_fire_us = 0;
    if (s->present_source_pending) {
        s->present_source_pending = false;
        pvg_host_work_unref(&s->present_source_owner);
    }
}

static void pvg_present_command(PvgState *s, PvgPresentKind kind)
{
    PvgPresentCommand *cmd = g_new0(PvgPresentCommand, 1);

    cmd->owner = pvg_host_work_ref(s, false);
    cmd->kind = kind;
    pthread_mutex_lock(&s->present_lock);
    if (s->present_cmd_tail) {
        s->present_cmd_tail->next = cmd;
    } else {
        s->present_cmd_head = cmd;
    }
    s->present_cmd_tail = cmd;
    pthread_cond_signal(&s->present_cond);
    pthread_mutex_unlock(&s->present_lock);
}

static void pvg_present_on_queue(void *opaque)
{
    PvgState *s = opaque;

    if (!s || !s->qmu_dev || !pvg_atomic_read(&s->iosfc_bootstrap_active)) {
        return;
    }
    if (pvg_log_should_emit(&s->bootstrap_present_log_count)) {
        pvg_log(s, "[apple-gfx-ml] bootstrap_present_queue: iosfc_present_tick mmio_wait=%d"
                " pending_frames=%d\n", 0, pvg_atomic_read(&s->pending_frames));
    }
    (void)qmu_iosfc_present_tick(s->qmu_dev);
}

typedef struct PvgPresentCall {
    PvgState *state;
    PvgPresentCommand *cmd;
} PvgPresentCall;

/* runs with present_lock held, under the command's work */
static void pvg_present_command_call(void *opaque)
{
    PvgPresentCall *call = opaque;
    PvgState *s = call->state;

    switch (call->cmd->kind) {
    case PVG_PRESENT_SCHEDULE:
        pvg_atomic_set(&s->iosfc_bootstrap_active, 1);
        if (!s->present_timer_active) {
            s->present_timer_active = true;
            s->present_next_fire_us = pvg_now_us();
            pthread_cond_signal(&s->present_cond);
            pvg_log(s, "[apple-gfx-ml] display frame timer started (%dms interval, present queue)\n",
                    AGFX_DISPLAY_FRAME_INTERVAL_MS);
        } else {
            pvg_present_merge_source_locked(s, false);
        }
        break;
    case PVG_PRESENT_CANCEL:
        pvg_atomic_set(&s->iosfc_bootstrap_active, 0);
        pvg_present_cancel_locked(s);
        break;
    }
}

static void pvg_cond_timedwait_ms(pthread_cond_t *cond, pthread_mutex_t *lock, int64_t ms)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    t.tv_sec += ms / 1000;
    t.tv_nsec += (ms % 1000) * 1000000;
    if (t.tv_nsec >= 1000000000) {
        t.tv_sec++;
        t.tv_nsec -= 1000000000;
    }
    pthread_cond_timedwait(cond, lock, &t);
}

static void *pvg_present_thread(void *opaque)
{
    PvgState *s = opaque;
    const int64_t interval_us = (int64_t)AGFX_DISPLAY_FRAME_INTERVAL_MS * 1000;

    pthread_mutex_lock(&s->present_lock);
    for (;;) {
        PvgPresentCommand *cmd = s->present_cmd_head;
        int64_t now_us, wait_ms;

        if (cmd) {
            PvgPresentCall call = { .state = s, .cmd = cmd };

            s->present_cmd_head = cmd->next;
            if (!s->present_cmd_head) {
                s->present_cmd_tail = NULL;
            }
            /* the serial queue alone mutates the timer/source state */
            pvg_host_work_invoke(&cmd->owner, pvg_present_command_call, &call);
            pvg_host_work_unref(&cmd->owner);
            g_free(cmd);
            continue;
        }
        if (s->present_source_pending) {
            PvgHostWork owner = s->present_source_owner;

            s->present_source_pending = false;
            memset(&s->present_source_owner, 0, sizeof(owner));
            pthread_mutex_unlock(&s->present_lock);
            pvg_host_work_invoke(&owner, pvg_present_on_queue, s);
            pvg_host_work_unref(&owner);
            pthread_mutex_lock(&s->present_lock);
            continue;
        }
        if (s->present_worker_stop) {
            break;
        }
        /* final stop closes only new autonomous ticks */
        if (s->present_roots_stopped || !s->present_timer_active) {
            pthread_cond_wait(&s->present_cond, &s->present_lock);
            continue;
        }
        now_us = pvg_now_us();
        if (s->present_next_fire_us != 0 && now_us >= s->present_next_fire_us) {
            do {
                s->present_next_fire_us += interval_us;
            } while (s->present_next_fire_us <= now_us);
            if (pvg_atomic_read(&s->iosfc_bootstrap_active)) {
                /* the timer merges the present source; it runs later on
                 * the same queue */
                pvg_present_merge_source_locked(s, true);
            }
            continue;
        }
        wait_ms = AGFX_DISPLAY_FRAME_INTERVAL_MS;
        if (s->present_next_fire_us != 0) {
            int64_t delta_us = s->present_next_fire_us - now_us;
            wait_ms = delta_us <= 0 ? 0 : (delta_us + 999) / 1000;
        }
        pvg_cond_timedwait_ms(&s->present_cond, &s->present_lock, wait_ms);
    }
    pthread_mutex_unlock(&s->present_lock);
    return NULL;
}

static void pvg_display_live_takeover(void *ctx)
{
    PvgState *s = ctx;

    if (s && pvg_atomic_read(&s->iosfc_bootstrap_active)) {
        pvg_log(s, "[apple-gfx-ml] display_live_takeover: cancel bootstrap presents\n");
        pvg_present_command(s, PVG_PRESENT_CANCEL);
    }
}

/* =========================================================== cursor
 * fork apple_gfx_ml_cursor_*: QMetal's display queue publishes; the device
 * loop converts and announces cursor_changed(); get_cursor() reads it. */
typedef struct PvgCursorGlyphJob {
    PvgState *state;
    uint8_t *pixels;
    uint64_t mapped_length, stride;
    uint32_t width, height, hot_x, hot_y, sum;
} PvgCursorGlyphJob;

typedef struct PvgCursorJob {
    PvgState *state;
    uint32_t display_id;
    bool visible;
} PvgCursorJob;

static void pvg_update_cursor(PvgState *s, uint32_t display_id)
{
    uint32_t packed = 0xffffffffu;
    bool have = s->qmu_dev &&
        qmu_get_display_cursor_position(s->qmu_dev, display_id, &packed) == QMU_OK;

    pthread_mutex_lock(&s->cursor.lock);
    if (have) {
        s->cursor.x = (int16_t)(packed & 0xffffu);
        s->cursor.y = (int16_t)((packed >> 16) & 0xffffu);
    }
    s->cursor.visible = pvg_atomic_read(&s->cursor_show) != 0;
    pthread_mutex_unlock(&s->cursor.lock);
    if (pvg_host_open(s)) {
        s->host_ops->cursor_changed(s->host_ops->host);
    }
}

static void pvg_cursor_glyph_bh(void *opaque)
{
    PvgCursorGlyphJob *job = opaque;
    PvgState *s = job->state;
    uint32_t *data;

    data = g_new(uint32_t, (size_t)job->width * job->height);
    /* Reference copy_GuestBGRA_ToHostBGRA: row y at y * stride, packed rows;
     * QMetal validated (height - 1) * stride + width * 4 <= mapped_length.
     * Same channel conversion as the fork's QEMUCursor (apple-gfx.m). */
    for (uint32_t y = 0; y < job->height; ++y) {
        const uint8_t *src = job->pixels + (size_t)y * job->stride;
        for (uint32_t x = 0; x < job->width; ++x) {
            data[(size_t)y * job->width + x] = ((uint32_t)src[0] << 16u) |
                                               ((uint32_t)src[1] << 8u) |
                                               ((uint32_t)src[2] << 0u) |
                                               ((uint32_t)src[3] << 24u);
            src += 4;
        }
    }
    pvg_log(s, "[apple-gfx-ml] cursor_glyph: %ux%u stride=%" PRIu64 " hot=%u,%u sum=0x%08x\n",
            job->width, job->height, job->stride, job->hot_x, job->hot_y, job->sum);
    pthread_mutex_lock(&s->cursor.lock);
    g_free(s->cursor.data);
    s->cursor.data = data;
    s->cursor.width = job->width;
    s->cursor.height = job->height;
    s->cursor.hot_x = job->hot_x;
    s->cursor.hot_y = job->hot_y;
    s->cursor.defined = true;
    s->cursor.serial++;
    pthread_mutex_unlock(&s->cursor.lock);
    pvg_update_cursor(s, s->cursor_display_id);
    g_free(job->pixels);
    g_free(job);
}

static void pvg_cursor_show_bh(void *opaque)
{
    PvgCursorJob *job = opaque;
    PvgState *s = job->state;

    s->cursor_display_id = job->display_id;
    pvg_log(s, "[apple-gfx-ml] cursor_show: display=%u visible=%d\n", job->display_id,
            job->visible ? 1 : 0);
    pvg_update_cursor(s, job->display_id);
    g_free(job);
}

static void pvg_cursor_move_bh(void *opaque)
{
    PvgCursorJob *job = opaque;
    PvgState *s = job->state;

    s->cursor_display_id = job->display_id;
    pvg_log(s, "[apple-gfx-ml] cursor_move: display=%u\n", job->display_id);
    pvg_update_cursor(s, job->display_id);
    g_free(job);
}

static void pvg_cursor_glyph(void *ctx, const void *pixels, uint64_t mapped_length,
                             uint64_t stride, uint32_t width, uint32_t height,
                             uint32_t hot_x, uint32_t hot_y, uint32_t sum)
{
    PvgState *s = ctx;
    PvgCursorGlyphJob *job;

    if (!s || !pixels || mapped_length == 0) {
        return;
    }
    job = g_new0(PvgCursorGlyphJob, 1);
    job->state = s;
    job->pixels = g_memdup2(pixels, mapped_length);
    job->mapped_length = mapped_length;
    job->stride = stride;
    job->width = width;
    job->height = height;
    job->hot_x = hot_x;
    job->hot_y = hot_y;
    job->sum = sum;
    pvg_schedule_host_bh(s, pvg_cursor_glyph_bh, job);
}

static void pvg_cursor_show(void *ctx, uint32_t display_id, int visible)
{
    PvgState *s = ctx;
    PvgCursorJob *job;

    if (!s) {
        return;
    }
    /* publish before scheduling: a pending update reads the current flag */
    pvg_atomic_set(&s->cursor_show, visible != 0);
    job = g_new0(PvgCursorJob, 1);
    job->state = s;
    job->display_id = display_id;
    job->visible = visible != 0;
    pvg_schedule_host_bh(s, pvg_cursor_show_bh, job);
}

static void pvg_cursor_move(void *ctx, uint32_t display_id)
{
    PvgState *s = ctx;
    PvgCursorJob *job;

    if (!s) {
        return;
    }
    job = g_new0(PvgCursorJob, 1);
    job->state = s;
    job->display_id = display_id;
    pvg_schedule_host_bh(s, pvg_cursor_move_bh, job);
}

/* =========================================================== VRAM */
static int pvg_read_vram(void *ctx, uint64_t vram_offset, void *buf, size_t size)
{
    PvgState *s = ctx;

    if (!s->vram || vram_offset > s->vram_size || size > s->vram_size - vram_offset) {
        return -1;
    }
    memcpy(buf, s->vram + vram_offset, size);
    return 0;
}

/* =========================================================== MMIO (BAR0) */
static const char *pvg_mmio_offset_name(uint64_t offset)
{
    switch ((uint32_t)offset) {
    case PVG_REG_FIFO_ENABLE: return "FIFO_ENABLE";
    case PVG_REG_FIFO_SIZE: return "FIFO_SIZE";
    case PVG_REG_FIFO_WRITE: return "FIFO_WRITE";
    case PVG_REG_FIFO_READ: return "FIFO_READ";
    case PVG_REG_FIFO_OFFSET: return "FIFO_OFFSET";
    case PVG_REG_SHARED_STATE_PFN: return "SHARED_STATE_PFN";
    case PVG_REG_KICK: return "KICK";
    case PVG_REG_EVENT_STAMPS: return "EVENT_STAMPS";
    case PVG_REG_DISPLAY_IRQ: return "DISPLAY_IRQ";
    case PVG_REG_PENDING_COMP: return "PENDING_COMP";
    case PVG_REG_RESUME_CHILD_FIFO: return "RESUME_CHILD_FIFO";
    case PVG_REG_FAULT_STATUS: return "FAULT_STATUS";
    case PVG_REG_FIFO_PFN: return "FIFO_PFN";
    case PVG_REG_PROTOCOL_VERSION: return "PROTOCOL_VERSION";
    case PVG_REG_IOSFC_ENABLE: return "IOSFC_ENABLE";
    case PVG_REG_IOSFC_CAPS: return "IOSFC_CAPS";
    case PVG_REG_IOSFC_CMD: return "IOSFC_CMD";
    case PVG_REG_IOSFC_DATA: return "IOSFC_DATA";
    case PVG_REG_IOSFC_TASK_PFN: return "IOSFC_TASK_PFN";
    case PVG_REG_IOSFC_TASK_SIZE: return "IOSFC_TASK_SIZE";
    case PVG_REG_IOSFC_MAP_FLAGS: return "IOSFC_MAP_FLAGS";
    case PVG_REG_IOSFC_MAP_ADDR: return "IOSFC_MAP_ADDR";
    case PVG_REG_DISPLAY_DOORBELL: return "DISPLAY_DOORBELL";
    case PVG_REG_IOSFC_RESULT: return "IOSFC_RESULT";
    case PVG_REG_DISPLAY_PORT_COUNT: return "DISPLAY_PORT_COUNT";
    default: return "OTHER";
    }
}

/* fork agfx_log_mmio_roundtrip (QMU_DIAG_MMIO_ROUNDTRIP, QMU_DIAG_MMIO_ALL) */
static void pvg_log_mmio_roundtrip(PvgState *s, const char *where, uint64_t offset,
                                   uint64_t value, unsigned size)
{
    if (!pvg_diag_mmio_roundtrip) {
        return;
    }
    if (!pvg_diag_mmio_all) {
        switch ((uint32_t)offset) {
        case PVG_REG_FIFO_WRITE: case PVG_REG_FIFO_READ: case PVG_REG_KICK:
        case PVG_REG_EVENT_STAMPS: case PVG_REG_DISPLAY_IRQ: case PVG_REG_PENDING_COMP:
        case PVG_REG_RESUME_CHILD_FIFO:
            break;
        default:
            return;
        }
    }
    pvg_log(s, "[apple-gfx-ml] DIAG_MMIO_ROUNDTRIP where=%s ts_ns=%llu tid=0x%lx job=%p "
            "off=0x%llx reg=%s value=0x%llx size=%u mmio_wait=%d\n", where,
            (unsigned long long)pvg_now_us() * 1000ULL, (unsigned long)pthread_self(),
            NULL, (unsigned long long)offset, pvg_mmio_offset_name(offset),
            (unsigned long long)value, size, 0);
}

typedef struct PvgMmioCall {
    PvgState *state;
    uint32_t offset;
    uint64_t value;
    unsigned size;
} PvgMmioCall;

static void pvg_mmio_read_call(void *opaque)
{
    PvgMmioCall *call = opaque;

    pvg_log_mmio_roundtrip(call->state, "caller_read_begin", call->offset, 0, call->size);
    call->value = qmu_mmio_read(call->state->qmu_dev, call->offset, call->size);
    pvg_log_mmio_roundtrip(call->state, "caller_read_return", call->offset, call->value,
                           call->size);
}

/* fork agfx_mmio_write_original minus the agfx-io hop: the write runs on
 * the vfio-user thread; KICK/FIFO_WRITE stay posted inside QMetal, every other
 * register first orders after queued doorbells (qmu_mmio_write). */
static void pvg_mmio_write_call(void *opaque)
{
    PvgMmioCall *call = opaque;
    PvgState *s = call->state;

    pvg_log_mmio_roundtrip(s, "caller_write_enqueue", call->offset, call->value, call->size);
    qmu_mmio_write(s->qmu_dev, call->offset, call->value, call->size);
    pvg_log_mmio_roundtrip(s, "caller_write_return", call->offset, call->value, call->size);
    /* scheduleFramePresents is owned by the wrapper: the first MAP_ADDR
     * starts the timer, later ones merge one tick; IOSFC_ENABLE=0 cancels. */
    if (call->offset == PVG_REG_IOSFC_MAP_ADDR && call->value != 0) {
        pvg_present_command(s, PVG_PRESENT_SCHEDULE);
    } else if (call->offset == PVG_REG_IOSFC_ENABLE && call->value == 0) {
        pvg_present_command(s, PVG_PRESENT_CANCEL);
    }
}

static bool pvg_mmio_admit(PvgState *s, PvgHostWork *owner)
{
    static bool reported;

    if (!s->retired && s->qmu_dev) {
        *owner = pvg_host_work_ref(s, true);
        if (owner->work) {
            return true;
        }
    }
    if (!reported) {
        reported = true;
        pvg_log(s, "[apple-gfx-ml][mmio][ERROR] access after the device stopped; "
                "reads return all-ones, writes are dropped\n");
    }
    return false;
}

static uint32_t pvg_mmio_read(PvgState *s, uint32_t offset)
{
    PvgHostWork owner;
    PvgMmioCall call = { .state = s, .offset = offset, .size = 4 };

    if (!pvg_mmio_admit(s, &owner)) {
        return 0xffffffffu;
    }
    pvg_host_work_invoke(&owner, pvg_mmio_read_call, &call);
    pvg_host_work_unref(&owner);
    return (uint32_t)call.value;
}

static void pvg_mmio_write(PvgState *s, uint32_t offset, uint32_t value)
{
    PvgHostWork owner;
    PvgMmioCall call = { .state = s, .offset = offset, .value = value, .size = 4 };

    if (!pvg_mmio_admit(s, &owner)) {
        return;
    }
    pvg_host_work_invoke(&owner, pvg_mmio_write_call, &call);
    pvg_host_work_unref(&owner);
}

/* BAR0, on the host's vfio-user thread. Fork MemoryRegionOps: valid 4..8
 * bytes, aligned, impl 4 bytes little-endian (an 8-byte access is the low
 * dword at offset, then the high dword at offset + 4). Invalid accesses read
 * 0 and drop writes, as QEMU does for a rejected access. */
static ssize_t pvg_bar0_access(vfu_ctx_t *ctx, char *buf, size_t count, loff_t off,
                               bool is_write)
{
    PvgState *s = g;
    static uint64_t invalid;

    (void)ctx;
    if (off < 0 || (count != 4 && count != 8) || ((uint64_t)off & (count - 1)) ||
        (uint64_t)off + count > APPLE_GFX_ML_MMIO_SIZE) {
        if (!is_write) {
            memset(buf, 0, count);
        }
        if (++invalid <= AGFX_LOG_INITIAL_COUNT || invalid % AGFX_LOG_INTERVAL == 0) {
            pvg_log(s, "[apple-gfx-ml][mmio][WARN] invalid %s off=0x%llx size=%zu (#%" PRIu64 ")\n",
                    is_write ? "write" : "read", (unsigned long long)off, count, invalid);
        }
        return (ssize_t)count;
    }
    for (size_t i = 0; i < count; i += 4) {
        uint32_t v;

        if (is_write) {
            memcpy(&v, buf + i, 4);
            pvg_mmio_write(s, (uint32_t)off + i, v);
        } else {
            v = pvg_mmio_read(s, (uint32_t)off + i);
            memcpy(buf + i, &v, 4);
        }
    }
    return (ssize_t)count;
}

/* =========================================================== QMetal peers */
enum PvgInstallOutcome { PVG_INSTALLED, PVG_RETURNED_FAILURE, PVG_RETAINED };

/* result < 0: the marker has no result field (fork
 * agfx_ordinary_physical_retained); else as agfx_ordinary_task_memory_retained */
static void G_GNUC_NORETURN pvg_retained_forever(const char *what, const char *stage,
                                                 int64_t result)
{
    /* A known failed disposition is terminal evidence: the owners are kept,
     * nothing is retried or freed (fork: an endless main loop). */
    if (result < 0) {
        fprintf(stderr, "%s stage=%s outcome=UNRETURNED owners_preserved=1\n", what, stage);
    } else {
        fprintf(stderr, "%s stage=%s result=%u outcome=UNRETURNED owners_preserved=1\n",
                what, stage, (uint32_t)result);
    }
    fflush(stderr);
    for (;;) {
        pause();
    }
}

static int pvg_negotiate_peers(PvgState *s)
{
    const struct qmu_private_ordinary_physical_capture_v2 *physical =
        qmu_get_private_ordinary_physical_capture_v2 ?
        qmu_get_private_ordinary_physical_capture_v2() : NULL;

    if (!physical || physical->version != QMU_PRIVATE_ORDINARY_PHYSICAL_CAPTURE_V2_VERSION ||
        physical->struct_size != sizeof(*physical) || !physical->prepare ||
        !physical->install || !physical->dispose_returned || !physical->destroy_returned) {
        pvg_log(s, "[apple-gfx-ml] [ERROR] realize: Missing exact private ordinary physical "
                "capture peer\n");
        return -ENOTSUP;
    }
    s->ordinary_physical_peer = physical;
    if (qmu_get_private_ordinary_task_memory_v1) {
        const struct qmu_private_ordinary_task_memory_v1 *peer =
            qmu_get_private_ordinary_task_memory_v1();

        if (!peer || peer->version != QMU_PRIVATE_ORDINARY_TASK_MEMORY_V1_VERSION ||
            peer->struct_size != sizeof(*peer) || !peer->prepare || !peer->install ||
            !peer->dispose_returned) {
            pvg_log(s, "[apple-gfx-ml] [ERROR] realize: Mismatched private ordinary "
                    "task-memory peer\n");
            return -ENOTSUP;
        }
        s->ordinary_task_memory_peer = peer;
    }
    if (qmu_get_private_mapped_transport_v1) {
        const struct qmu_private_mapped_transport_v1 *api =
            qmu_get_private_mapped_transport_v1();

        if (!api || api->version != QMU_PRIVATE_MAPPED_TRANSPORT_V1_VERSION ||
            api->struct_size != sizeof(*api) || !api->install) {
            pvg_log(s, "[apple-gfx-ml] [ERROR] realize: Mismatched private mapped-transport "
                    "peer\n");
            return -ENOTSUP;
        }
        s->mapped_transport_peer = api;
    }
    return 0;
}

/* fork agfx_destroy_ordinary_physical: the V2 owned destroy of the Session */
static void pvg_destroy_ordinary_physical(PvgState *s)
{
    g_assert(s->qmu_dev && s->ordinary_physical_peer);
    if (!s->ordinary_physical_peer->destroy_returned(s->ordinary_physical_receipt,
                                                     s->qmu_dev)) {
        pvg_retained_forever("AGFX_ORDINARY_PHYSICAL_RETAINED", "destroy", -1);
    }
    s->qmu_dev = NULL;
}

static enum PvgInstallOutcome pvg_install_ordinary_physical(PvgState *s)
{
    const struct qmu_private_ordinary_physical_capture_v2 *peer = s->ordinary_physical_peer;
    struct qmu_ordinary_physical_handoff_v2 handoff = { 0 };
    uint32_t result;

    if (!agfx_ordinary_physical_context_create(s) ||
        !agfx_ordinary_physical_context_acquire(s->ordinary_physical_context, &handoff)) {
        pvg_log(s, "[apple-gfx-ml] [ERROR] realize: Cannot acquire ordinary physical context\n");
        return PVG_RETURNED_FAILURE;
    }
    s->ordinary_physical_receipt = peer->prepare(s->qmu_dev, &handoff);
    if (!s->ordinary_physical_receipt) {
        agfx_ordinary_physical_context_release(handoff.acquired_context);
        pvg_log(s, "[apple-gfx-ml] [ERROR] realize: Ordinary physical prepare returned no "
                "receipt\n");
        return PVG_RETURNED_FAILURE;
    }
    result = peer->install(s->ordinary_physical_receipt);
    if (result == QMU_ORDINARY_PHYSICAL_V2_INSTALLED) {
        return PVG_INSTALLED;
    }
    agfx_ordinary_physical_context_stop(s->ordinary_physical_context);
    if (result != QMU_ORDINARY_PHYSICAL_V2_REJECTED ||
        !peer->dispose_returned(s->ordinary_physical_receipt)) {
        fprintf(stderr, "AGFX_ORDINARY_PHYSICAL_RETAINED stage=install result=%u "
                "outcome=UNRETURNED owners_preserved=1\n", result);
        fflush(stderr);
        return PVG_RETAINED;
    }
    s->ordinary_physical_receipt = NULL;
    pvg_log(s, "[apple-gfx-ml] [ERROR] realize: Ordinary physical install returned rejection\n");
    return PVG_RETURNED_FAILURE;
}

static bool pvg_install_ordinary_physical_comparison(PvgState *s)
{
    const struct qmu_private_ordinary_physical_comparison_v1 *peer;
    struct qmu_ordinary_physical_comparison_handoff_v1 handoff = {
        .version = QMU_PRIVATE_ORDINARY_PHYSICAL_COMPARISON_V1_VERSION,
        .struct_size = sizeof(handoff),
        .memory_ctx_identity = s,
        .physical_context_identity = s->ordinary_physical_context,
        .compare = agfx_ordinary_physical_compare,
    };

    if (!qmu_get_private_ordinary_physical_comparison_v1) {
        return true;   /* older paired peer: comparison unavailable */
    }
    peer = qmu_get_private_ordinary_physical_comparison_v1();
    if (!peer || peer->version != QMU_PRIVATE_ORDINARY_PHYSICAL_COMPARISON_V1_VERSION ||
        peer->struct_size != sizeof(*peer) || !peer->install ||
        !peer->install(s->qmu_dev, &handoff)) {
        pvg_log(s, "[apple-gfx-ml] [ERROR] realize: Cannot install exact optional physical "
                "comparison peer\n");
        return false;
    }
    return true;
}

static bool pvg_task_memory_callbacks_complete(PvgState *s, const qmu_extended_callbacks *cb)
{
    return cb->abi_magic == QMU_EXTENDED_CALLBACKS_ABI_MAGIC &&
           cb->abi_version == QMU_EXTENDED_ABI_VERSION &&
           cb->struct_size == sizeof(*cb) && cb->user_ctx == s &&
           cb->map_gpa && cb->unmap_gpa && cb->read_memory &&
           cb->mark_mapped_memory_dirty && cb->eeee_task_allocate &&
           cb->eeee_task_remap_shared_fixed_overwrite &&
           cb->eeee_task_protect && cb->eeee_task_deallocate &&
           cb->eeee_task_host_page_size && cb->eeee_task_rcu_enter &&
           cb->eeee_task_rcu_leave && cb->eeee_task_lock &&
           cb->eeee_task_unlock && cb->eeee_task_translate &&
           cb->eeee_task_direct && cb->eeee_task_ram_pointer &&
           cb->eeee_task_region_ref && cb->eeee_task_region_unref &&
           cb->eeee_task_publish && cb->eeee_task_remove &&
           cb->eeee_task_host_write && cb->eeee_task_fatal;
}

static void pvg_task_memory_stop_acquiring(void *opaque)
{
    agfx_eeee_native_context_stop_acquiring(opaque);
}

static enum PvgInstallOutcome pvg_install_ordinary_task_memory(PvgState *s)
{
    const struct qmu_private_ordinary_task_memory_v1 *peer = s->ordinary_task_memory_peer;
    struct qmu_ordinary_task_memory_handoff_v1 handoff = {
        .version = QMU_PRIVATE_ORDINARY_TASK_MEMORY_V1_VERSION,
        .struct_size = sizeof(handoff),
        .profile = QMU_ORDINARY_TASK_MEMORY_LEGACY_CALLBACKS,
    };

    if (!peer) {
        return PVG_INSTALLED;
    }
    s->ordinary_task_memory_context = agfx_eeee_native_context_create(s);
    if (!s->ordinary_task_memory_context) {
        pvg_log(s, "[apple-gfx-ml] [ERROR] realize: Cannot create ordinary CPU facilities "
                "context\n");
        return PVG_RETURNED_FAILURE;
    }
    if (!agfx_eeee_native_context_acquire(s->ordinary_task_memory_context, &handoff.facilities)) {
        pvg_log(s, "[apple-gfx-ml] [ERROR] realize: Cannot acquire ordinary CPU facilities "
                "context\n");
        return PVG_RETURNED_FAILURE;
    }
    handoff.acquired_context = s->ordinary_task_memory_context;
    handoff.release_context = agfx_eeee_native_context_release;
    handoff.stop_acquiring = pvg_task_memory_stop_acquiring;
    handoff.memory_ctx_identity = s;
    if (!pvg_task_memory_callbacks_complete(s, &handoff.facilities)) {
        agfx_eeee_native_context_release(handoff.acquired_context);
        pvg_log(s, "[apple-gfx-ml] [ERROR] realize: Incomplete ordinary CPU facilities "
                "acquisition\n");
        return PVG_RETURNED_FAILURE;
    }
    s->ordinary_task_memory_receipt = peer->prepare(s->qmu_dev, &handoff);
    if (!s->ordinary_task_memory_receipt) {
        agfx_eeee_native_context_release(handoff.acquired_context);
        pvg_log(s, "[apple-gfx-ml] [ERROR] realize: Ordinary task-memory prepare returned no "
                "receipt\n");
        return PVG_RETURNED_FAILURE;
    }
    s->ordinary_task_memory_install_result = peer->install(s->ordinary_task_memory_receipt);
    if (s->ordinary_task_memory_install_result == QMU_ORDINARY_TASK_MEMORY_INSTALLED) {
        return PVG_INSTALLED;
    }
    agfx_eeee_native_context_stop_acquiring(s->ordinary_task_memory_context);
    if (s->ordinary_task_memory_install_result != QMU_ORDINARY_TASK_MEMORY_REJECTED) {
        return PVG_RETAINED;
    }
    pvg_log(s, "[apple-gfx-ml] [ERROR] realize: Ordinary task-memory install returned "
            "rejection\n");
    return PVG_RETURNED_FAILURE;
}

static void pvg_return_ordinary_task_memory_after_destroy(PvgState *s)
{
    g_assert(!s->qmu_dev);
    if (s->ordinary_task_memory_receipt) {
        if (!s->ordinary_task_memory_peer->dispose_returned(s->ordinary_task_memory_receipt)) {
            pvg_retained_forever("AGFX_ORDINARY_TASK_MEMORY_RETAINED", "dispose_after_destroy",
                                 s->ordinary_task_memory_install_result);
        }
        s->ordinary_task_memory_receipt = NULL;
    }
    if (s->ordinary_task_memory_context) {
        if (!agfx_eeee_native_context_retire(s->ordinary_task_memory_context)) {
            pvg_retained_forever("AGFX_ORDINARY_TASK_MEMORY_RETAINED", "context_retire",
                                 s->ordinary_task_memory_install_result);
        }
        s->ordinary_task_memory_context = NULL;
    }
}

/* Before any FIFO ingress; a rejection keeps the callback path. */
static void pvg_install_mapped_transport(PvgState *s)
{
    struct qmu_mapped_transport_provider_v1 provider = {
        .version = QMU_PRIVATE_MAPPED_TRANSPORT_V1_VERSION,
        .struct_size = sizeof(provider),
        .contract = QMU_MAPPED_TRANSPORT_RETAINED_RAM_V1,
        .ctx = s,
        .map_retained = pvg_map_retained,
        .topology_sequence = pvg_topology_sequence,
    };

    if (!s->mapped_transport_peer) {
        return;
    }
    s->mapped_transport_installed =
        s->mapped_transport_peer->install(s->qmu_dev, &provider) == QMU_MAPPED_TRANSPORT_INSTALLED;
    if (!s->mapped_transport_installed) {
        pvg_log(s, "[apple-gfx-ml][WARN] apple-gfx-ml: mapped transport rejected; callback path\n");
    }
}

/* Task-memory install failure: retire what was installed, in reverse order
 * (fork realize fail path). */
static void pvg_unwind_session(PvgState *s)
{
    agfx_eeee_native_context_stop_acquiring(s->ordinary_task_memory_context);
    agfx_ordinary_physical_context_stop(s->ordinary_physical_context);
    pvg_destroy_ordinary_physical(s);
    pvg_return_ordinary_task_memory_after_destroy(s);
    if (s->ordinary_physical_receipt) {
        if (!s->ordinary_physical_peer->dispose_returned(s->ordinary_physical_receipt)) {
            pvg_retained_forever("AGFX_ORDINARY_PHYSICAL_RETAINED",
                                 "startup_dispose_after_destroy", -1);
        }
        s->ordinary_physical_receipt = NULL;
    }
    agfx_ordinary_physical_context_retire(s->ordinary_physical_context);
}

/* =========================================================== stop phase
 * fork agfx_stop_phase + agfx_cleanup_notifier: stop every producer that can
 * call into or run inside the library before the process exits, so library
 * finalizers never run with live users (d441). */
static void pvg_stop_phase(PvgState *s)
{
    qmu_status status;

    if (s->stop_phase_done || !s->qmu_dev) {
        return;
    }
    /* guest MMIO ingress: served on this same thread, now closed */
    s->retired = true;

    /* close autonomous wrapper roots, not the accepted-control executor */
    pthread_mutex_lock(&s->present_lock);
    s->present_roots_stopped = true;
    pthread_cond_signal(&s->present_cond);
    pthread_mutex_unlock(&s->present_lock);

    /* The device loop keeps running jobs while this waits (the fork pumped
     * the main loop for the same reason). */
    status = qmu_session_quiesce_host_callback_producers_deferred(s->qmu_dev);
    qmu_session_rethrow_quiesce(s->qmu_dev);
    if (status != QMU_OK) {
        /* never authorizes teardown: owners stay; the watchdog decides */
        fprintf(stderr, "[apple-gfx-ml] [ERROR] stop phase: Session quiesce status=%d\n",
                (int)status);
        fflush(stderr);
        for (;;) {
            pause();
        }
    }
    g_assert(!s->deferred_frame_owner.state);

    pthread_mutex_lock(&s->present_lock);
    s->present_worker_stop = true;
    pthread_cond_signal(&s->present_cond);
    pthread_mutex_unlock(&s->present_lock);
    if (s->present_started) {
        pthread_join(s->present_thread, NULL);
        s->present_started = false;
    }
    pthread_mutex_lock(&s->present_lock);
    pvg_present_cancel_locked(s);
    pthread_mutex_unlock(&s->present_lock);

    if (s->render_started) {
        pthread_mutex_lock(&s->render_lock);
        s->render_stop = true;
        pthread_cond_broadcast(&s->render_cond);
        pthread_mutex_unlock(&s->render_lock);
        for (int i = 0; i < PVG_RENDER_THREADS; i++) {
            pthread_join(s->render_threads[i], NULL);
        }
        s->render_started = false;
    }
    s->stop_phase_done = true;
}

typedef struct PvgQuitWatchdog {
    bool done;
} PvgQuitWatchdog;

static void *pvg_quit_notice_thread(void *opaque)
{
    bool *delivered = opaque;

    fprintf(stderr, "[apple-gfx-ml] [ERROR] quit stop phase did not finish in %d ms, "
            "exit without finalizers\n", AGFX_QUIT_STOP_LIMIT_MS);
    fflush(stderr);
    pvg_atomic_set(delivered, true);
    return NULL;
}

static void G_GNUC_NORETURN pvg_quit_fail(void)
{
    static bool delivered;
    pthread_t notice;
    const int64_t limit = pvg_now_us() + (int64_t)AGFX_QUIT_NOTICE_LIMIT_MS * 1000;

    if (pthread_create(&notice, NULL, pvg_quit_notice_thread, &delivered) == 0) {
        pthread_detach(notice);
        while (!pvg_atomic_read(&delivered) && pvg_now_us() < limit) {
            usleep(10 * 1000);
        }
    }
    _exit(EXIT_FAILURE);
}

static void *pvg_quit_watchdog_thread(void *opaque)
{
    PvgQuitWatchdog *watchdog = opaque;
    const int64_t limit = pvg_now_us() + (int64_t)AGFX_QUIT_STOP_LIMIT_MS * 1000;

    while (!pvg_atomic_read(&watchdog->done)) {
        if (pvg_now_us() >= limit) {
            pvg_quit_fail();
        }
        usleep(50 * 1000);
    }
    return NULL;
}

/* fork agfx_cleanup_notifier: the quit stop phase, bounded by a watchdog */
static void pvg_quit_stop(PvgState *s)
{
    PvgQuitWatchdog *watchdog;
    pthread_t thread;
    qmu_status status;
    const int64_t begin = pvg_now_us();

    if (!s->qmu_dev || s->stop_phase_done) {
        return;
    }
    s->exiting = true;
    watchdog = g_new0(PvgQuitWatchdog, 1);   /* heap: survives an escaping stop */
    if (pthread_create(&thread, NULL, pvg_quit_watchdog_thread, watchdog) != 0) {
        _exit(EXIT_FAILURE);
    }
    fprintf(stderr, "[apple-gfx-ml] quit stop phase begin\n");
    pvg_stop_phase(s);
    status = qmu_session_finish_retirement_for_exit(s->qmu_dev);
    if (status != QMU_OK) {
        fprintf(stderr, "[apple-gfx-ml] [ERROR] retirement worker not finished "
                "after stop, status=%d, exit without finalizers\n", (int)status);
        _exit(EXIT_FAILURE);
    }
    fprintf(stderr, "[apple-gfx-ml] quit stop phase done ms=%lld\n",
            (long long)((pvg_now_us() - begin) / 1000));
    pvg_atomic_set(&watchdog->done, true);
    pthread_join(thread, NULL);
    g_free(watchdog);
}

/* =========================================================== args */
static bool pvg_parse_bool(const char *v, bool *out)
{
    if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) {
        *out = true;
    } else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) {
        *out = false;
    } else {
        return false;
    }
    return true;
}

static bool pvg_parse_u32(const char *v, uint32_t *out)
{
    char *end = NULL;
    unsigned long n;

    errno = 0;
    n = strtoul(v, &end, 0);
    if (errno || end == v || *end || n > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)n;
    return true;
}

static int pvg_parse_args(PvgState *s, const char *args)
{
    char *copy, *save = NULL;
    int r = 0;

    if (!args || !args[0]) {
        return 0;
    }
    copy = g_strdup(args);
    for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char *val = strchr(tok, '=');
        bool ok;

        if (!val) {
            fprintf(stderr, "[apple-gfx-ml] [ERROR] arg '%s' has no value\n", tok);
            r = -EINVAL;
            break;
        }
        *val++ = 0;
        if (!strcmp(tok, "xres")) {
            ok = pvg_parse_u32(val, &s->display_width);
        } else if (!strcmp(tok, "yres")) {
            ok = pvg_parse_u32(val, &s->display_height);
        } else if (!strcmp(tok, "vram_size_mb") || !strcmp(tok, "vram")) {
            ok = pvg_parse_u32(val, &s->vram_size_mb);
        } else if (!strcmp(tok, "debug")) {
            ok = pvg_parse_u32(val, &s->debug_level);
        } else if (!strcmp(tok, "vsync")) {
            ok = pvg_parse_bool(val, &s->vsync_enabled);
        } else if (!strcmp(tok, "direct_scanout")) {
            ok = pvg_parse_bool(val, &s->direct_scanout);
        } else if (!strcmp(tok, "spirv_cache_dir")) {
            g_free(s->spirv_cache_dir);
            s->spirv_cache_dir = g_strdup(val);
            ok = true;
        } else if (!strcmp(tok, "device-id") || !strcmp(tok, "x-pci-device-id")) {
            ok = pvg_parse_u32(val, &s->pci_device_id) && s->pci_device_id <= 0xffff;
        } else if (!strcmp(tok, "vendor-id") || !strcmp(tok, "x-pci-vendor-id")) {
            ok = pvg_parse_u32(val, &s->pci_vendor_id) && s->pci_vendor_id <= 0xffff;
        } else if (!strcmp(tok, "log")) {
            ok = !strcmp(val, "stderr") || !strcmp(val, "host");
            s->log_to_host = !strcmp(val, "host");
        } else if (!strcmp(tok, "exit-on-disconnect")) {
            ok = pvg_parse_bool(val, &s->exit_on_disconnect);
        } else {
            fprintf(stderr, "[apple-gfx-ml] [ERROR] unknown arg '%s'\n", tok);
            r = -EINVAL;
            break;
        }
        if (!ok) {
            fprintf(stderr, "[apple-gfx-ml] [ERROR] bad value '%s' for arg '%s'\n", val, tok);
            r = -EINVAL;
            break;
        }
    }
    g_free(copy);
    if (!r && (!s->display_width || !s->display_height || s->display_width > 0xffff ||
               s->display_height > 0xffff || !s->vram_size_mb)) {
        fprintf(stderr, "[apple-gfx-ml] [ERROR] bad mode %ux%u or VRAM %u MiB\n",
                s->display_width, s->display_height, s->vram_size_mb);
        r = -EINVAL;
    }
    return r;
}

/* =========================================================== realize */
typedef struct PvgModeCall {
    PvgState *state;
} PvgModeCall;

static void pvg_initial_mode_call(void *opaque)
{
    PvgState *s = ((PvgModeCall *)opaque)->state;

    pvg_apply_mode_change(s, s->display_width, s->display_height,
                          s->fb_iosurface_pixel_format, s->fb_protection_requirements);
    s->rendering_frame_width = s->fb_width;
    s->rendering_frame_height = s->fb_height;
}

/* Keep libmlapi and this plugin mapped for the life of the process: QMetal's
 * threads survive the stop phase (as in the fork, where the session is never
 * destroyed on quit), so neither object may be unloaded under them. */
static void pvg_pin_libraries(void)
{
    const void *syms[] = { (const void *)qmu_create_extended, (const void *)pvg_pin_libraries };

    for (size_t i = 0; i < G_N_ELEMENTS(syms); i++) {
        Dl_info info;

        if (dladdr(syms[i], &info) && info.dli_fname) {
            (void)dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);
        }
    }
}

static int pvg_pci_setup(PvgState *s, vfu_ctx_t *ctx)
{
    struct msicap msi = {
        .hdr = { .id = PCI_CAP_ID_MSI },
        .mc = { .c64 = 1 },      /* 1 vector (mmc 0), 64-bit, no per-vector mask */
    };
    ssize_t pos;

    /* PCIe config space (the fork's type is an INTERFACE_PCIE_DEVICE) with
     * no PCIe capability, header type 0, revision 0. */
    if (vfu_pci_init(ctx, VFU_PCI_TYPE_EXPRESS, PCI_HEADER_TYPE_NORMAL, 0) < 0) {
        return -errno;
    }
    vfu_pci_set_id(ctx, (uint16_t)s->pci_vendor_id, (uint16_t)s->pci_device_id, 0, 0);
    vfu_pci_set_class(ctx, 0x03, 0x80, 0x00);
    if (s->pci_vendor_id != APPLE_GFX_ML_VENDOR_ID) {
        pvg_log(s, "[apple-gfx-ml] PCI vendor id overridden to 0x%04x\n", s->pci_vendor_id);
    }
    if (s->pci_device_id != APPLE_GFX_ML_DEVICE_ID) {
        pvg_log(s, "[apple-gfx-ml] PCI device id overridden to 0x%04x\n", s->pci_device_id);
    }
    pos = vfu_pci_add_capability(ctx, 0, 0, &msi);
    if (pos < 0 || vfu_setup_device_nr_irqs(ctx, VFU_DEV_MSI_IRQ, 1) < 0) {
        int err = errno;

        pvg_log(s, "[apple-gfx-ml] msi_init FAILED: %d\n", -err);
        return -err;
    }
    pvg_log(s, "[apple-gfx-ml] msi_init OK, 1 vector\n");
    if (vfu_setup_region(ctx, VFU_PCI_DEV_BAR0_REGION_IDX, APPLE_GFX_ML_MMIO_SIZE,
                         pvg_bar0_access, VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM,
                         NULL, 0, -1, 0) < 0) {
        return -errno;
    }
    return 0;
}

static int pvg_create_session(PvgState *s)
{
    qmu_extended_callbacks callbacks = {
        .abi_magic = QMU_EXTENDED_CALLBACKS_ABI_MAGIC,
        .abi_version = QMU_EXTENDED_ABI_VERSION,
        .struct_size = sizeof(qmu_extended_callbacks),
        .user_ctx = s,
        .map_gpa = pvg_map_gpa,
        .unmap_gpa = pvg_unmap_gpa,
        .mark_mapped_memory_dirty = pvg_mark_mapped_memory_dirty,
        .read_memory = pvg_read_memory,
        .read_ram_memory = pvg_read_ram_memory,
        .write_memory = pvg_write_memory,
        .raise_irq = pvg_raise_irq,
        .cursor_glyph = pvg_cursor_glyph,
        .cursor_move = pvg_cursor_move,
        .cursor_show = pvg_cursor_show,
        .mode_change = pvg_mode_change,
        .display_live_takeover = pvg_display_live_takeover,
        .read_vram = pvg_read_vram,
        /* one IRQ line: iosfc_raise_irq is raise_irq (apple-gfx-pci.m:107) */
        .iosfc_map_memory = pvg_iosfc_map_memory,
        .iosfc_unmap_memory = pvg_iosfc_unmap_memory,
        .iosfc_raise_irq = pvg_raise_irq,
        .schedule_display_completion = pvg_schedule_display_completion,
        /* no BQL out of process: the "main loop" forms are the same leaves */
        .read_memory_mainloop = pvg_read_memory,
        .write_memory_mainloop = pvg_write_memory,
        .new_frame_signal = pvg_new_frame_signal,
        .frame_completed = NULL,
        .render_frame_complete = pvg_render_frame_complete,
    };
    qmu_extended_config config = {
        .abi_magic = QMU_EXTENDED_CONFIG_ABI_MAGIC,
        .abi_version = QMU_EXTENDED_ABI_VERSION,
        .struct_size = sizeof(qmu_extended_config),
        .ram_base = 0,
        .ram_size = 0,
        .max_protocol_version = 6,
        .iosfc_caps = 0x03,
        .display_port_count = 1,
        .vram_size = s->vram_size,
        .direct_scanout = s->direct_scanout,
        .vsync_enabled = s->vsync_enabled,
        .spirv_cache_dir = s->spirv_cache_dir,
        .using_iosurface_mapper = 0,
    };
    int r;

    /* reference apple-gfx.m:45-49: three modes, mode 0 the primary one */
    config.mode_data[0] = (s->display_height << 16) | s->display_width;
    config.mode_data[1] = (1080u << 16) | 1440u;
    config.mode_data[2] = (1024u << 16) | 1280u;
    config.mode_count = 3;

    r = pvg_negotiate_peers(s);
    if (r) {
        return r;
    }
    s->qmu_dev = qmu_create_extended(&config, &callbacks);
    if (!s->qmu_dev) {
        pvg_log(s, "[apple-gfx-ml] [ERROR] realize: Failed to create qmetal device\n");
        return -ENODEV;
    }
    switch (pvg_install_ordinary_physical(s)) {
    case PVG_INSTALLED:
        break;
    case PVG_RETAINED:
        /* marker printed; the SAME receipt, Session and context stay owned
         * (fork: realize returns its error without any finalization) */
        pvg_log(s, "[apple-gfx-ml] [ERROR] realize: Ordinary physical install retained "
                "original unknown receipt\n");
        return -ENODEV;
    case PVG_RETURNED_FAILURE:
        /* a null receipt is covered by the V2 destroy (prepare rejection) */
        pvg_destroy_ordinary_physical(s);
        agfx_ordinary_physical_context_retire(s->ordinary_physical_context);
        return -ENODEV;
    }
    if (!pvg_install_ordinary_physical_comparison(s)) {
        agfx_ordinary_physical_context_stop(s->ordinary_physical_context);
        pvg_destroy_ordinary_physical(s);
        if (!s->ordinary_physical_peer->dispose_returned(s->ordinary_physical_receipt)) {
            pvg_retained_forever("AGFX_ORDINARY_PHYSICAL_RETAINED",
                                 "comparison_install_dispose_after_destroy", -1);
        }
        s->ordinary_physical_receipt = NULL;
        agfx_ordinary_physical_context_retire(s->ordinary_physical_context);
        return -ENODEV;
    }
    switch (pvg_install_ordinary_task_memory(s)) {
    case PVG_INSTALLED:
        break;
    case PVG_RETAINED:
        pvg_retained_forever("AGFX_ORDINARY_TASK_MEMORY_RETAINED", "install",
                             s->ordinary_task_memory_install_result);
    case PVG_RETURNED_FAILURE:
        pvg_unwind_session(s);
        return -ENODEV;
    }
    qmu_log_set_callback(pvg_qmu_log_callback, s);
    qmu_set_debug_level(s->qmu_dev, (int)s->debug_level);
    pvg_install_mapped_transport(s);
    return 0;
}

static int pvg_start_thread(pthread_t *t, void *(*fn)(void *), void *arg, const char *name)
{
    int r = pthread_create(t, NULL, fn, arg);

    if (r == 0) {
        pthread_setname_np(*t, name);
    }
    return -r;
}

static int pvg_setup(vfu_ctx_t *ctx, const char *args, const qdp_host_env *env, void **priv)
{
    PvgState *s;
    pthread_condattr_t cattr;
    size_t max_frame;
    PvgModeCall mode = { 0 };
    int r;

    if (g) {
        fprintf(stderr, "[apple-gfx-ml] [ERROR] one device per process\n");
        return -EBUSY;
    }
    s = g_new0(PvgState, 1);
    s->vram_size_mb = APPLE_GFX_ML_DEFAULT_VRAM_MB;
    s->display_width = 1920;
    s->display_height = 1080;
    s->pci_vendor_id = APPLE_GFX_ML_VENDOR_ID;
    s->pci_device_id = APPLE_GFX_ML_DEVICE_ID;
    s->vsync_enabled = true;
    s->exit_on_disconnect = true;
    r = pvg_parse_args(s, args);
    if (r) {
        g_free(s);
        return r;
    }
    if (!env || env->size < offsetof(qdp_host_env, host_ops) + sizeof(env->host_ops) ||
        !env->host_ops || env->host_ops->size < sizeof(qdp_host_ops) ||
        !env->host_ops->irq_trigger || !env->host_ops->frame_ready ||
        !env->host_ops->cursor_changed || !env->host_ops->log) {
        fprintf(stderr, "[apple-gfx-ml] [ERROR] the host provides no qdp v2 host services "
                "(irq_trigger, frame_ready, cursor_changed, log)\n");
        g_free(s);
        return -ENOTSUP;
    }
    s->host_ops = env->host_ops;
    s->vfu = ctx;
    s->vram_size = (uint64_t)s->vram_size_mb << 20;
    g = s;
    pvg_diag_mmio_roundtrip = getenv("QMU_DIAG_MMIO_ROUNDTRIP") != NULL;
    {
        const char *all = getenv("QMU_DIAG_MMIO_ALL");
        pvg_diag_mmio_all = all && all[0] && strcmp(all, "0") != 0;
    }

    pvg_log_init(s);
    pvg_log(s, "[apple-gfx-ml] Realizing device: %ux%u, VRAM=%uMB\n",
            s->display_width, s->display_height, s->vram_size_mb);

    r = pvg_pci_setup(s, ctx);
    if (r) {
        goto fail;
    }

    /* internal VRAM, not a BAR (fork: host_vram RAM region) */
    s->vram = mmap(NULL, s->vram_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (s->vram == MAP_FAILED) {
        s->vram = NULL;
        r = -errno;
        goto fail;
    }

    pvg_memory_init(&s->mem);
    pthread_mutex_init(&s->irq_lock, NULL);
    s->irq_gate = PVG_IRQ_DROP;   /* opened when setup returns */
    pthread_mutex_init(&s->cursor.lock, NULL);
    pthread_mutex_init(&s->loop_lock, NULL);
    pthread_cond_init(&s->loop_cond, NULL);
    pthread_mutex_init(&s->render_lock, NULL);
    pthread_cond_init(&s->render_cond, NULL);
    pthread_mutex_init(&s->present_lock, NULL);
    pthread_condattr_init(&cattr);
    pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
    pthread_cond_init(&s->present_cond, &cattr);
    pthread_condattr_destroy(&cattr);

    max_frame = MAX((size_t)s->display_width * s->display_height,
                    MAX((size_t)1440 * 1080, (size_t)1280 * 1024)) * 4;
    r = pvg_scanout_init(s, max_frame);
    if (r) {
        goto fail;
    }
    r = pvg_start_thread(&s->loop_thread, pvg_loop_thread, s, "agfx-main");
    if (r) {
        goto fail;
    }
    s->loop_started = true;

    r = pvg_create_session(s);
    if (r) {
        goto fail;
    }

    for (int i = 0; i < PVG_RENDER_THREADS; i++) {
        r = pvg_start_thread(&s->render_threads[i], pvg_render_thread, s, "agfx-render");
        if (r) {
            goto fail;
        }
    }
    s->render_started = true;
    r = pvg_start_thread(&s->present_thread, pvg_present_thread, s, "agfx-present");
    if (r) {
        goto fail;
    }
    s->present_started = true;

    /* initial mode publication on the device loop (fork: console init) */
    mode.state = s;
    pvg_loop_run_sync(s, pvg_initial_mode_call, &mode);
    pvg_pin_libraries();
    s->realized = true;
    pvg_log(s, "[apple-gfx-ml] Device realized successfully\n");
    /* No guest access is possible before setup returns: open the window. */
    pthread_mutex_lock(&s->irq_lock);
    s->irq_gate = PVG_IRQ_OPEN;
    pthread_mutex_unlock(&s->irq_lock);
    pvg_atomic_set(&s->host_open, 1);
    *priv = s;
    return 0;

fail:
    /* A failed setup ends the host process (qdev-host exits); threads and
     * the Session are left to process exit rather than torn down here. */
    pvg_log(s, "[apple-gfx-ml] [ERROR] realize failed: %s\n", strerror(-r));
    pvg_log_stop(s);
    return r;
}

/* =========================================================== qdp ops */
static void pvg_teardown(void *priv)
{
    PvgState *s = priv;

    if (!s) {
        return;
    }
    /* teardown closes the host-services window: no IRQ, frame, cursor or
     * host-log call from here on (the VM is going away with the host) */
    pvg_irq_drop(s);
    pvg_atomic_set(&s->host_open, 0);
    pvg_memory_resume(s);     /* never stop QMetal behind a closed map */
    pvg_quit_stop(s);
    pvg_log_stop(s);
    fflush(stderr);
    /* No destroy: like the fork's normal quit, the stopped Session and its
     * threads stay until process exit (libraries pinned). */
}

static int pvg_reset(void *priv, vfu_reset_type_t type)
{
    PvgState *s = priv;

    if (type == VFU_RESET_LOST_CONN) {
        pvg_log(s, "[apple-gfx-ml] client connection lost: stopping the device\n");
        pvg_irq_drop(s);
        pvg_memory_reopen_after_lost_conn(s);
        pvg_quit_stop(s);
        if (s->exit_on_disconnect) {
            /* The host calls teardown() (idempotent stop, log flush) and exits 0. */
            pvg_log(s, "[apple-gfx-ml] exit after the lost connection (exit-on-disconnect)\n");
            return QDP_RESET_EXIT;
        }
        pvg_log(s, "[apple-gfx-ml][WARN] device stopped; a new client gets a dead device\n");
        return 0;
    }
    /* Fork agfx_reset: no device state change; QMetal resets through the
     * guest's MMIO (FIFO_ENABLE 1->0 is the FIFO teardown boundary). */
    pvg_log(s, "[apple-gfx-ml] device reset type=%d: no device state change\n", (int)type);
    return 0;
}

static bool pvg_get_scanout(void *priv, qdp_scanout *out)
{
    PvgState *s = priv;
    PvgScanout *sc = &s->scan;

    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&sc->lock);
    out->fd = sc->fd;
    if (sc->front >= 0 && sc->width && sc->height) {
        sc->held = sc->front;
        out->enabled = true;
        out->offset = (uint32_t)((size_t)sc->front * sc->slot_size);
        out->width = sc->width;
        out->height = sc->height;
        out->stride = sc->stride;
        out->pixman_format = PIXMAN_x8r8g8b8;
    }
    pthread_mutex_unlock(&sc->lock);
    return true;
}

static bool pvg_get_cursor(void *priv, qdp_cursor *out)
{
    PvgState *s = priv;
    PvgCursor *c = &s->cursor;

    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&c->lock);
    if (c->defined && c->data) {
        size_t n = (size_t)c->width * c->height;

        if (c->host_copy_len < n) {
            g_free(c->host_copy);
            c->host_copy = g_new(uint32_t, n);
            c->host_copy_len = n;
        }
        memcpy(c->host_copy, c->data, n * sizeof(uint32_t));
        out->visible = c->visible;
        out->width = c->width;
        out->height = c->height;
        out->hot_x = c->hot_x;
        out->hot_y = c->hot_y;
        out->pixels = c->host_copy;
        out->serial = c->serial;
        /* PVG reports the pointer (hot spot) position */
        out->x = c->x - (int32_t)c->hot_x;
        out->y = c->y - (int32_t)c->hot_y;
    }
    pthread_mutex_unlock(&c->lock);
    return true;
}

/* Host main loop, before a guest memory map change. */
static int pvg_quiesce(void *priv)
{
    PvgState *s = priv;

    pvg_irq_hold(s);
    pvg_memory_quiesce(s);
    return 0;
}

static void pvg_resume(void *priv)
{
    PvgState *s = priv;

    pvg_memory_resume(s);
    pvg_irq_release(s);
}

static void pvg_dma_register(void *priv, const qdp_dma_region *region)
{
    pvg_memory_dma_register(priv, region);
}

static void pvg_dma_unregister(void *priv, const qdp_dma_region *region)
{
    pvg_memory_dma_unregister(priv, region);
}

static const qdp_device_ops ops = {
    .abi_version = QDP_ABI_VERSION,
    .name = "apple-pvg",
    .version = "0.1",
    .setup = pvg_setup,
    .teardown = pvg_teardown,
    .reset = pvg_reset,
    .get_scanout = pvg_get_scanout,
    .flags = QDP_F_PLUGIN_DAMAGE,
    .quiesce = pvg_quiesce,
    .resume = pvg_resume,
    .dma_register = pvg_dma_register,
    .dma_unregister = pvg_dma_unregister,
    .get_cursor = pvg_get_cursor,
};

__attribute__((visibility("default")))
const qdp_device_ops *qdp_plugin_v1(void) { return &ops; }
