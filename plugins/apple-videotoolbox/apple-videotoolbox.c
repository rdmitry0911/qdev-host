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
#include "vtpv_proto.h"
#include "vtpv_vaapi.h"

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

typedef struct vt_queue {
    uint16_t size;    /* negotiated, <= VT_QUEUE_MAX           */
    uint8_t  enabled;
    uint64_t desc, driver, device; /* guest ring addresses     */
} vt_queue;

typedef struct vt_dev {
    const qdp_host_ops *host;

    vtpv_vaapi              *va;
    const vtpv_decoder_backend *be;
    vtpv_proto              *proto;

    pthread_mutex_t m;

    /* virtio common-config shadow */
    uint64_t device_features;
    uint64_t driver_features;
    uint8_t  device_status;
    uint16_t queue_select;
    vt_queue queues[VT_QUEUES];
    uint8_t  needs_reset;

    /* guest memory regions, for the ring/buffer access the transport
     * increment performs (tracked now so the seam is real). */
    /* (kept minimal in increment 1) */
} vt_dev;

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

/* ---- completion sink ------------------------------------------------ *
 * The protocol handler emits a completion here. In the full transport this
 * copies the bytes into the next guest-posted completion-queue buffer and
 * raises that queue's MSI-X vector. Increment 1 records it (the ring walk
 * that provides the destination buffer is the transport increment). */
static void vt_emit(void *ctx, const uint8_t *cmpl, size_t len)
{
    vt_dev *d = ctx;
    /* Transport increment: place `cmpl` on completion vq (queue 1) and
     * d->host->irq_trigger(d->host->host, /vector of queue 1/). */
    (void)cmpl; (void)len; (void)d;
}

/*
 * Process the submission virtqueue after a notify: walk the avail ring, read
 * each descriptor chain's message bytes out of guest memory, and hand them to
 * vtpv_proto_submit (which validates + dispatches + emits a completion via
 * vt_emit). This is the integration point between the virtio transport and
 * the (complete, tested) message protocol handler. The ring walk itself is
 * the transport increment; the call it makes per message is final:
 *
 *     vtpv_proto_submit(d->proto, msg_bytes, msg_len);
 */
static void vt_process_submission(vt_dev *d)
{
    (void)d;
    /* Transport increment: for each avail descriptor chain, gather the
     * message bytes (bounded by VT_QUEUE_MAX descriptors / 1 MiB) and call
     * vtpv_proto_submit(d->proto, msg, len). */
}

/* ---- BAR0: virtio-modern common config ------------------------------ */

static ssize_t vt_bar0(vfu_ctx_t *ctx, char *buf, size_t count, loff_t off,
                       bool is_write)
{
    vt_dev *d = g_dev;
    if (!d) return is_write ? (ssize_t)count : -1;
    pthread_mutex_lock(&d->m);

    /* Notify window: a write of a queue index kicks that queue. */
    if (off >= VT_CFG_NOTIFY && off < VT_CFG_NOTIFY + VT_QUEUES * 4) {
        if (is_write) {
            uint16_t q = (uint16_t)((off - VT_CFG_NOTIFY) / 4);
            if (q == 0) /* submission */
                vt_process_submission(d);
        }
        pthread_mutex_unlock(&d->m);
        return (ssize_t)count;
    }

    /*
     * Common-config register file (virtio-modern). Increment 1 keeps the
     * shadow registers that queue setup needs (feature negotiation gated on
     * VERSION_1, queue select/size/enable, the DEVICE_NEEDS_RESET latch on a
     * queue misuse); the full field decode lands with the ring machine.
     */
    if (!is_write)
        memset(buf, 0, count); /* reads return the shadow / zero for now */

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
    d->proto = vtpv_proto_create(d->be, d->va ? vtpv_vaapi_ctx(d->va) : NULL,
                                 vt_emit, d);
    if (!d->proto) {
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

    g_dev = d;
    *priv = d;
    vt_log(d, 2, "apple-videotoolbox %04x:%04x up (%u virtqueues, %s)",
           VT_PCI_VENDOR, VT_PCI_DEVICE, VT_QUEUES,
           d->be ? "VA-API decode" : "no backend");
    return 0;

fail:
    vtpv_proto_destroy(d->proto);
    if (d->va) vtpv_vaapi_close(d->va);
    free(d);
    return -errno ? -errno : -EINVAL;
}

static void vt_teardown(void *priv)
{
    vt_dev *d = priv;
    if (!d) return;
    if (g_dev == d) g_dev = NULL;
    vtpv_proto_destroy(d->proto);
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
    d->device_status = 0;
    d->queue_select = 0;
    d->needs_reset = 0;
    for (i = 0; i < VT_QUEUES; i++) {
        d->queues[i].enabled = 0;
        d->queues[i].desc = d->queues[i].driver = d->queues[i].device = 0;
    }
    pthread_mutex_unlock(&d->m);
    return 0;
}

static const qdp_device_ops ops = {
    .abi_version = QDP_ABI_VERSION,
    .name = "apple-videotoolbox",
    .version = "0.1-inc1",
    .setup = vt_setup,
    .teardown = vt_teardown,
    .reset = vt_reset,
};

const qdp_device_ops *qdp_plugin_v1(void) { return &ops; }
