/*
 * apple-videotoolbox.c - qdev-host plugin: the host side of Apple's
 * paravirtualized VideoToolbox codec device (PCI 106b:1a03).
 *
 * AppleGVA host backend, increment 1 (lane L-M2V03, 2026-11-16).
 *
 * CLEAN-ROOM: written from the qdev-host plugin ABI (qdp.h), libvfio-user,
 * and the device contract (analysis/20261115-APPLEGVA-VIDEO-HOST-CONTRACT.md;
 * devices.md "Transport"). It does NOT copy code from the reims reference
 * (research/reims, GPL/LGPL) nor from reims's QEMU apple-videotoolbox-pci.c.
 *
 * What this file is (increment 1, "device skeleton"):
 *   - presents the PCI identity Apple's guest kext matches
 *     (IOPCIPrimaryMatch 0x1a03106b): vendor 0x106b, device 0x1a03, as a
 *     virtio-1.x modern PCI device;
 *   - lays out BAR0 the virtio-modern way (common cfg @0, ISR @0x1000,
 *     notify @0x4000 x4, device cfg @0x8000) and BAR2 for MSI-X, with one
 *     vector per virtqueue plus one for config;
 *   - declares the TWO virtqueues (0 submission, 1 completion), each capped
 *     at 256 descriptors (the Apple limit);
 *   - instantiates the codec-session protocol handler (vtpv_proto) over the
 *     VA-API decode backend (vtpv_vaapi), and wires the submission-queue
 *     notify -> handler -> completion-queue seam.
 *
 * What is the NEXT increment (guest-blocked, see §7 F0-GVA: the x86_64 guest
 * kext does not ship, lane C must write it, so none of this runs E2E yet):
 *   - the virtio ring state machine (avail/used rings, descriptor-chain walk,
 *     feature negotiation beyond VERSION_1). The register map and the queue
 *     semantics are laid out here; walking a live guest's rings needs the
 *     guest half. The message protocol the rings carry is already complete
 *     and unit-tested (vtpv_proto, test_proto.c).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libvfio-user.h>

#include "qdp.h"
#include "vtpv_vaapi.h"
#include "vtpv_virtq.h"
#include "aw_session.h"   /* the Apple contract-B wire session engine (H5/H6/H7) */

/* ---- device constants ---------------------------------------------- */

#define VT_PCI_VENDOR   0x106b   /* Apple */
#define VT_PCI_DEVICE   0x1a03   /* com.apple.virtualization.avp.videotoolbox */
#define VT_PCI_CLASS    0x04     /* multimedia controller                     */
#define VT_PCI_SUBCLASS 0x80     /* other multimedia                          */

#define VT_BAR0_SIZE    0x10000u /* virtio-modern cfg window                  */
#define VT_CFG_COMMON   0x0000u
#define VT_CFG_ISR      0x1000u
#define VT_CFG_NOTIFY   0x4000u  /* notify multiplier 4                       */
#define VT_CFG_DEVICE   0x8000u

#define VT_QUEUES       2u       /* 0 = submission, 1 = completion            */
#define VT_QUEUE_MAX    256u     /* Apple's descriptor cap                    */
#define VT_MSIX_VECTORS (VT_QUEUES + 1) /* per-queue + config                 */

/* virtio feature bits this device offers / requires. Apple's base offers
 * indirect descriptors, event index, VERSION_1 and packed ring, and requires
 * VERSION_1 alone (devices.md "Transport"). Increment 1 advertises VERSION_1
 * only; the richer features come with the ring state machine. */
#define VIRTIO_F_VERSION_1  (1ull << 32)

#define VT_MAX_REGIONS 64

typedef struct vt_dev {
    const qdp_host_ops *host;

    vtpv_vaapi              *va;
    const vtpv_decoder_backend *be;
    aw_engine               *engine;   /* contract-B wire session engine          */

    pthread_mutex_t m;

    /* virtio-modern common-config shadow */
    uint64_t device_features;
    uint32_t device_feature_select, driver_feature_select;
    uint64_t driver_features;
    uint8_t  device_status;
    uint8_t  config_generation;
    uint16_t msix_config;
    uint16_t queue_select;
    vtpv_virtq queues[VT_QUEUES];     /* 0 submission, 1 completion */
    uint16_t queue_msix[VT_QUEUES];
    uint8_t  needs_reset;

    /* guest DMA regions (from the host's dma_register/unregister). */
    qdp_dma_region regions[VT_MAX_REGIONS];
    int n_regions;
} vt_dev;

/* the completion vq's MSI-X vector (set by the driver; config vector fallback) */
#define VT_MSIX_NO_VECTOR 0xffffu

/* libvfio-user region callbacks receive no private pointer (they get the
 * host's vfu_create_ctx private, not the plugin's), so the BAR0 callback
 * reaches the device through a global - one plugin instance per host process,
 * as the other plugins (test-v2) do. */
static vt_dev *g_dev;

static void vt_log(vt_dev *d, int level, const char *fmt, ...)
{
    va_list ap;
    char buf[256];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (d->host && d->host->log)
        d->host->log(d->host->host, level, "%s", buf);
    else
        fprintf(stderr, "apple-videotoolbox: %s\n", buf);
}

/* ---- guest memory via the DMA regions (called under d->m) ----------- */
static void *vt_map(void *ctx, uint64_t iova, uint64_t len)
{
    vt_dev *d = ctx;
    int i;
    if (!len || iova + len < iova) return NULL;
    for (i = 0; i < d->n_regions; i++) {
        const qdp_dma_region *r = &d->regions[i];
        if (r->vaddr && iova >= r->iova && iova + len <= r->iova + r->len)
            return (uint8_t *)r->vaddr + (iova - r->iova);
    }
    return NULL;
}

static void vt_raise(vt_dev *d, unsigned queue)
{
    uint16_t vec = queue < VT_QUEUES ? d->queue_msix[queue] : d->msix_config;
    if (d->host && d->host->irq_trigger && vec != VT_MSIX_NO_VECTOR)
        (void)d->host->irq_trigger(d->host->host, vec);
}

/* ---- queue 1 (host->guest) emit sink -------------------------------- *
 * The aw_session engine's emit callback (under d->m). Apple's queue 1 is a
 * host->guest message stream: the guest pre-posts 0x4000-byte writable buffers,
 * and the host writes each outbound message (a reply, or a Dpba/Dbuf/Demt/rels
 * request) into one and retires it. A message with no posted buffer is dropped
 * (as a real device would, until the guest reposts). */
static void vt_emit(void *ctx, const uint8_t *msg, size_t len)
{
    vt_dev *d = ctx;
    vtpv_virtq *hq = &d->queues[1];
    uint16_t head = 0; uint32_t written = 0;
    int rc = vtpv_virtq_put_writable(hq, vt_map, d, msg, len, &head, &written);
    if (rc == 1) {
        vtpv_virtq_retire(hq, vt_map, d, head, written);
        vt_raise(d, 1);
        if (written < len)
            vt_log(d, 1, "host->guest message truncated %u/%zu (guest buffer too small)",
                   written, len);
    } else if (rc == 0) {
        vt_log(d, 1, "host->guest message dropped: no posted queue-1 buffer");
    } else {
        d->needs_reset = 1;
    }
}

/* ---- H2: the decoded-output buffer seam (the guest lends it via Dbuf) --- *
 * resolve_buffer turns the guest-lent output surface (named by the Dbuf reply's
 * `subs` mapping id) into host-addressable NV12 planes, so the host can land the
 * decoded frame into the guest's own buffer.
 *
 * Guest-memory channel. The only guest memory this plugin can reach is the qdp
 * DMA-region table (iova/GPA -> host vaddr or fd+offset; qdp.h), the same guest
 * RAM the paravirtual-GPU path shares as udmabuf-backed buffers. We reuse it via
 * vt_map rather than inventing a second channel.
 *
 * BRING-UP CONVENTION (host <-> guest kext): the guest lends its output IOSurface
 * by the surface's guest-physical base address as the `subs` mapping id, with an
 * NV12 (biplanar 420v) layout at the Dpba alignment (BytesPerRowAlignment 64).
 * The host maps those pages through the DMA-region table.
 *
 * FAITHFUL PATH / PRECISE QMETAL API ASK (for L-M2V03; see
 * evidence/runs/applegva-realproto-20261116/H2-QMETAL-SURFACE-MAPPER-ASK.md):
 * Apple resolves the mapping id through a shared IOSurface mapper (reims'
 * retain_io_surface_with_mapping_id / address_ranges_for_virtual_page). That
 * mapper lives inside apple-pvg/qmu (using_iosurface_mapper = 0 today) and is
 * NOT reachable from this plugin. To make the id a true opaque mapper token
 * (not a GPA), the plugin needs a plugin-boundary resolver
 * `map_guest_surface(mapper_ctx, mapping_id) -> {dma-buf fd or host ptr + plane
 * layout}`. Until that lands, the GPA convention above is the bring-up path. */
static int vt_resolve_buffer(void *ctx, const uint8_t uuid[16], uint64_t mapping_id,
                             uint32_t w, uint32_t h, aw_out_buffer *out)
{
    vt_dev *d = ctx;
    uint32_t ystride = (w + 63u) & ~63u;          /* Dpba BytesPerRowAlignment 64 */
    size_t   ysize   = (size_t)ystride * h;
    size_t   csize   = (size_t)ystride * ((h + 1u) / 2u); /* NV12 interleaved UV   */
    uint8_t *p = vt_map(d, mapping_id, ysize + csize);
    if (!p) {
        vt_log(d, 1, "Dbuf surface %#llx: no guest pages for the lent buffer",
               (unsigned long long)mapping_id);
        return -1;
    }
    memset(out, 0, sizeof *out);
    memcpy(out->uuid, uuid, 16);
    out->mapping_id = mapping_id;
    out->y = p;             out->y_stride = ystride;
    out->uv = p + ysize;    out->uv_stride = ystride;
    out->width = w;         out->height = h;
    return 0;
}
static int vt_copy_frame(void *ctx, const vtpv_decoded_frame *f, const aw_out_buffer *b)
{
    vt_dev *d = ctx;
    if (!d->va || !b->y) return -1;
    /* land the full NV12 frame: Y plane, then the interleaved UV plane. */
    int rc = vtpv_vaapi_read_luma(d->va, f->surface_id, b->y, b->y_stride, f->width, f->height);
    if (rc == 0 && b->uv)
        rc = vtpv_vaapi_read_chroma(d->va, f->surface_id, b->uv, b->uv_stride, f->width, f->height);
    return rc;
}
static void vt_put_buffer(void *ctx, const aw_out_buffer *b) { (void)ctx; (void)b; }

/* ---- queue 0 (guest->host): drain into the session engine ----------- *
 * Apple's queue 0 is asymmetric: each chain is one message plus a 4-byte i32
 * writable status tail. Gather the message, run it through the engine, write the
 * returned status into the tail, retire. Replies/requests go out on queue 1 via
 * vt_emit. */
static void vt_process_submission(vt_dev *d)
{
    vtpv_virtq *sq = &d->queues[0];
    unsigned budget = 2 * VT_QUEUE_MAX; /* bound the drain */
    static uint8_t msg[VTPV_MAX_SAMPLE_BYTES + 4096];
    while (budget-- && vtpv_virtq_has_avail(sq, vt_map, d) == 1) {
        size_t len = 0; uint16_t head = 0;
        uint8_t *wptr = NULL; uint32_t wcap = 0;
        int rc = vtpv_virtq_pop_inout(sq, vt_map, d, msg, sizeof(msg), &len,
                                      &wptr, &wcap, &head);
        if (rc != 1) { d->needs_reset = 1; break; } /* malformed ring */
        int32_t status = aw_engine_deliver(d->engine, msg, len); /* emits on q1 */
        uint32_t written = 0;
        if (wptr && wcap >= 4) { memcpy(wptr, &status, 4); written = 4; } /* native i32 */
        vtpv_virtq_retire(sq, vt_map, d, head, written);
    }
    vt_raise(d, 0); /* the queue-0 chains are now free on the used ring */
}

/* ---- BAR0: virtio-modern common config + notify --------------------- */

static uint64_t ld(const void *p, size_t n) { uint64_t v = 0; memcpy(&v, p, n < 8 ? n : 8); return v; }
static void st(void *p, size_t n, uint64_t v) { memcpy(p, &v, n < 8 ? n : 8); }

/* set bytes [field_off, field_off+w) of a 64-bit field from val (for split
 * lo/hi register writes of queue_desc/driver/device). */
static void set64_part(uint64_t *field, unsigned byte, size_t w, uint64_t val)
{
    uint64_t f = *field;
    unsigned bit = byte * 8, i;
    for (i = 0; i < w && byte + i < 8; i++) {
        f &= ~((uint64_t)0xff << (bit + i * 8));
        f |= ((val >> (i * 8)) & 0xff) << (bit + i * 8);
    }
    *field = f;
}

/* Common-config register file (virtio_pci_common_cfg). off is within [0,0x40). */
static void vt_common_cfg(vt_dev *d, uint64_t off, char *buf, size_t n, bool wr)
{
    const uint16_t q = d->queue_select;
    vtpv_virtq *vq = q < VT_QUEUES ? &d->queues[q] : NULL;
    const uint64_t v = wr ? ld(buf, n) : 0;

#define RD(x) do { if (!wr) st(buf, n, (uint64_t)(x)); } while (0)
    switch (off) {
    case 0x00: if (wr) d->device_feature_select = (uint32_t)v; else RD(d->device_feature_select); break;
    case 0x04: RD((d->device_features >> (d->device_feature_select ? 32 : 0)) & 0xffffffffu); break;
    case 0x08: if (wr) d->driver_feature_select = (uint32_t)v; else RD(d->driver_feature_select); break;
    case 0x0c:
        if (wr) {
            uint64_t shift = d->driver_feature_select ? 32 : 0;
            d->driver_features &= ~((uint64_t)0xffffffffu << shift);
            d->driver_features |= (v & 0xffffffffu) << shift;
        } else RD((d->driver_features >> (d->driver_feature_select ? 32 : 0)) & 0xffffffffu);
        break;
    case 0x10: if (wr) d->msix_config = (uint16_t)v; else RD(d->msix_config); break;
    case 0x12: RD(VT_QUEUES); break;                       /* num_queues (RO) */
    case 0x14: if (wr) { d->device_status = (uint8_t)v; if (!v) d->needs_reset = 0; } else RD(d->device_status); break;
    case 0x15: RD(d->config_generation); break;            /* config_generation (RO) */
    case 0x16: if (wr) { if (v < VT_QUEUES) d->queue_select = (uint16_t)v; } else RD(d->queue_select); break;
    case 0x18: if (wr && vq) { if (v && v <= VTPV_VQ_MAX_SIZE) vq->size = (uint16_t)v; } else RD(vq ? vq->size : 0); break;
    case 0x1a: if (wr) { if (q < VT_QUEUES) d->queue_msix[q] = (uint16_t)v; } else RD(q < VT_QUEUES ? d->queue_msix[q] : VT_MSIX_NO_VECTOR); break;
    case 0x1c:
        if (wr && vq) {
            vq->ready = v ? 1 : 0;
            if (vq->ready) { vq->last_avail = 0; vq->used_idx = 0; }
        } else RD(vq ? vq->ready : 0);
        break;
    case 0x1e: RD(q); break;                               /* queue_notify_off = queue index */
    case 0x20: case 0x21: case 0x22: case 0x23:            /* queue_desc (8 bytes) */
    case 0x24: case 0x25: case 0x26: case 0x27:
        if (wr && vq) set64_part(&vq->desc, (unsigned)(off - 0x20), n, v);
        else RD(vq ? (vq->desc >> ((off - 0x20) * 8)) : 0);
        break;
    case 0x28: case 0x29: case 0x2a: case 0x2b:            /* queue_driver = avail (8 bytes) */
    case 0x2c: case 0x2d: case 0x2e: case 0x2f:
        if (wr && vq) set64_part(&vq->avail, (unsigned)(off - 0x28), n, v);
        else RD(vq ? (vq->avail >> ((off - 0x28) * 8)) : 0);
        break;
    case 0x30: case 0x31: case 0x32: case 0x33:            /* queue_device = used (8 bytes) */
    case 0x34: case 0x35: case 0x36: case 0x37:
        if (wr && vq) set64_part(&vq->used, (unsigned)(off - 0x30), n, v);
        else RD(vq ? (vq->used >> ((off - 0x30) * 8)) : 0);
        break;
    default: if (!wr) memset(buf, 0, n); break;
    }
#undef RD
}

static ssize_t vt_bar0(vfu_ctx_t *ctx, char *buf, size_t count, loff_t off,
                       bool is_write)
{
    vt_dev *d = g_dev;
    (void)ctx;
    if (!d || count > 8) return is_write ? (ssize_t)count : -1;
    pthread_mutex_lock(&d->m);

    if (off >= VT_CFG_COMMON && off < VT_CFG_COMMON + 0x40) {
        vt_common_cfg(d, off - VT_CFG_COMMON, buf, count, is_write);
    } else if (off >= VT_CFG_ISR && off < VT_CFG_ISR + 0x100) {
        if (!is_write) st(buf, count, 0); /* ISR status: read-to-clear, MSI-X in use */
    } else if (off >= VT_CFG_NOTIFY && off < VT_CFG_NOTIFY + VT_QUEUES * 4) {
        if (is_write) {
            uint16_t qn = (uint16_t)((off - VT_CFG_NOTIFY) / 4);
            if (qn == 0) vt_process_submission(d); /* only the submission vq is kicked */
        }
    } else if (!is_write) {
        memset(buf, 0, count);
    }

    pthread_mutex_unlock(&d->m);
    return (ssize_t)count;
}

/* ---- qdp_device_ops ------------------------------------------------- */

static int vt_setup(vfu_ctx_t *ctx, const char *args, const qdp_host_env *env,
                    void **priv)
{
    vt_dev *d;
    char err[256] = {0};
    const char *node = "/dev/dri/renderD128";
    int rc;

    if (env->size < offsetof(qdp_host_env, host_ops) + sizeof(env->host_ops) ||
        !env->host_ops || env->host_ops->size < sizeof(qdp_host_ops))
        return -EINVAL;

    if (args) {
        const char *p = strstr(args, "render_node=");
        if (p) node = p + strlen("render_node=");
    }

    d = calloc(1, sizeof(*d));
    if (!d) return -ENOMEM;
    d->host = env->host_ops;
    pthread_mutex_init(&d->m, NULL);
    d->device_features = VIRTIO_F_VERSION_1;

    /* bring up the VA-API decode backend + the protocol handler */
    rc = vtpv_vaapi_open(node, &d->va, &d->be, NULL, err, sizeof(err));
    if (rc != 0) {
        vt_log(d, 1, "VA-API backend unavailable on %s: %s; the device will "
                     "advertise no codecs", node, err);
        d->va = NULL; d->be = NULL;
    }
    {
        aw_session_cfg cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.be = d->be;
        cfg.be_ctx = d->va ? vtpv_vaapi_ctx(d->va) : NULL;
        cfg.emit = vt_emit; cfg.emit_ctx = d;
        cfg.resolve_buffer = vt_resolve_buffer;
        cfg.copy_frame = vt_copy_frame;
        cfg.put_buffer = vt_put_buffer;
        cfg.buf_ctx = d;
        d->engine = aw_engine_create(&cfg);
    }
    if (!d->engine) {
        if (d->va) vtpv_vaapi_close(d->va);
        free(d);
        return -ENOMEM;
    }

    /* PCI identity the guest kext matches (IOPCIPrimaryMatch 0x1a03106b) */
    if (vfu_pci_init(ctx, VFU_PCI_TYPE_EXPRESS, PCI_HEADER_TYPE_NORMAL, 0) < 0)
        goto fail;
    vfu_pci_set_id(ctx, VT_PCI_VENDOR, VT_PCI_DEVICE, VT_PCI_VENDOR, VT_PCI_DEVICE);
    vfu_pci_set_class(ctx, VT_PCI_CLASS, VT_PCI_SUBCLASS, 0x00);

    /* BAR0: virtio-modern config window (common/ISR/notify/device). */
    if (vfu_setup_region(ctx, VFU_PCI_DEV_BAR0_REGION_IDX, VT_BAR0_SIZE, vt_bar0,
                         VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM, NULL, 0, -1, 0) < 0)
        goto fail;

    /* MSI-X: one vector per virtqueue plus one for config. */
    if (vfu_setup_device_nr_irqs(ctx, VFU_DEV_MSIX_IRQ, VT_MSIX_VECTORS) < 0)
        goto fail;
    if (vfu_setup_device_nr_irqs(ctx, VFU_DEV_INTX_IRQ, 1) < 0)
        goto fail;

    d->queues[0].size = VT_QUEUE_MAX; /* submission */
    d->queues[1].size = VT_QUEUE_MAX; /* completion */
    d->queue_msix[0] = d->queue_msix[1] = VT_MSIX_NO_VECTOR;
    d->msix_config = VT_MSIX_NO_VECTOR;

    g_dev = d;
    *priv = d;
    vt_log(d, 2, "apple-videotoolbox %04x:%04x up (%u virtqueues, %s)",
           VT_PCI_VENDOR, VT_PCI_DEVICE, VT_QUEUES,
           d->be ? "VA-API decode" : "no backend");
    return 0;

fail:
    aw_engine_destroy(d->engine);
    if (d->va) vtpv_vaapi_close(d->va);
    free(d);
    return -errno ? -errno : -EINVAL;
}

static void vt_teardown(void *priv)
{
    vt_dev *d = priv;
    if (!d) return;
    if (g_dev == d) g_dev = NULL;
    aw_engine_destroy(d->engine);
    if (d->va) vtpv_vaapi_close(d->va);
    pthread_mutex_destroy(&d->m);
    free(d);
}

static int vt_reset(void *priv, vfu_reset_type_t type)
{
    vt_dev *d = priv;
    unsigned i;
    (void)type;
    if (!d) return 0;
    pthread_mutex_lock(&d->m);
    d->driver_features = 0;
    d->device_feature_select = d->driver_feature_select = 0;
    d->device_status = 0;
    d->queue_select = 0;
    d->needs_reset = 0;
    d->msix_config = VT_MSIX_NO_VECTOR;
    for (i = 0; i < VT_QUEUES; i++) {
        vtpv_virtq *vq = &d->queues[i];
        vq->ready = 0; vq->desc = vq->avail = vq->used = 0;
        vq->last_avail = vq->used_idx = 0; vq->size = VT_QUEUE_MAX;
        d->queue_msix[i] = VT_MSIX_NO_VECTOR;
    }
    pthread_mutex_unlock(&d->m);
    return 0;
}

/* Guest memory map changes (host main loop, quiesced around unmaps). */
static void vt_dma_register(void *priv, const qdp_dma_region *region)
{
    vt_dev *d = priv;
    pthread_mutex_lock(&d->m);
    if (d->n_regions < VT_MAX_REGIONS)
        d->regions[d->n_regions++] = *region;
    else
        vt_log(d, 1, "DMA region table full; region %#llx dropped",
               (unsigned long long)region->iova);
    pthread_mutex_unlock(&d->m);
}
static void vt_dma_unregister(void *priv, const qdp_dma_region *region)
{
    vt_dev *d = priv;
    int i;
    pthread_mutex_lock(&d->m);
    for (i = 0; i < d->n_regions; i++)
        if (d->regions[i].iova == region->iova && d->regions[i].len == region->len) {
            d->regions[i] = d->regions[--d->n_regions];
            break;
        }
    pthread_mutex_unlock(&d->m);
}

static const qdp_device_ops ops = {
    .abi_version = QDP_ABI_VERSION,
    .name = "apple-videotoolbox",
    .version = "0.2-inc2",
    .setup = vt_setup,
    .teardown = vt_teardown,
    .reset = vt_reset,
    .dma_register = vt_dma_register,
    .dma_unregister = vt_dma_unregister,
};

const qdp_device_ops *qdp_plugin_v1(void) { return &ops; }
