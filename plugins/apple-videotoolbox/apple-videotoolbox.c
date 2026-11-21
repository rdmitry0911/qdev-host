/*
 * apple-videotoolbox.c - qdev-host plugin: the host side of Apple's
 * paravirtualized VideoToolbox codec device (PCI 1b36:eee2; Apple's own is 106b:1a03).
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

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "qdp.h"
#include "vtpv_vaapi.h"
#include "vtpv_nvdec.h"
#ifdef VTPV_HAVE_LAVC
#include "vtpv_lavc.h"     /* stream decode core (libavcodec + VA-API/CUDA), plan U1 */
#endif
#include "vtpv_virtq.h"
#include "aw_session.h"   /* the Apple contract-B wire session engine (H5/H6/H7) */
#include "vt_scatter.h"   /* page-aware landing into a non-contiguous guest surface */
#include "vt_land.h"      /* GVA-LAND-BOUNDS: only what fits the lent surface lands */
#ifdef VTPV_HAVE_LAVC
#define VT_LAVC_CLOSE(d) do { if ((d)->lavc) { vtpv_lavc_close((d)->lavc); (d)->lavc = NULL; } } while (0)
#else
#define VT_LAVC_CLOSE(d) do { (void)(d); } while (0)
#endif
#include "../uniq-vgpu/pvg_mapper_bridge.h" /* increment 2: resolve mapping_id -> gpa-ranges */

/* ---- device constants ---------------------------------------------- */

#define VT_PCI_VENDOR   0x1b36   /* Uni-Q vGPU family (same vendor as the eee1 GPU) */
#define VT_PCI_DEVICE   0xeee2   /* Uni-Q vGPU video (was Apple 106b:1a03; the guest
                                    * VideoToolbox finds the driver by IOService class
                                    * name only, analysis/20261121-GVA-VIDEO-IDENTITY-RENAME) */
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

/* virtio feature bits. Apple's base offers indirect-desc (28), event-idx (29),
 * VERSION_1 (32) and packed-ring (34), and requires VERSION_1 alone (devices.md
 * "Transport"). We advertise 28/29/32 and keep the SPLIT ring the plugin
 * implements; we do NOT yet offer packed (34), so the guest driver negotiates
 * split. (Packed is a follow-on if a real guest rejects the split ring.) */
#define VIRTIO_F_INDIRECT_DESC  (1ull << 28)
#define VIRTIO_F_RING_EVENT_IDX (1ull << 29)
#define VIRTIO_F_VERSION_1      (1ull << 32)
#define VIRTIO_F_RING_PACKED    (1ull << 34)
/* NOTE: we do NOT advertise VIRTIO_F_RING_EVENT_IDX (29). We never maintain the
 * used-ring `avail_event` field (vtpv_virtq_retire does not write it), so under
 * EVENT_IDX the guest's stock AppleVirtIO transport uses a stale avail_event and
 * suppresses the 2nd queue-0 notification. Proven live on macOS 26.2 (25C56):
 * with EVENT_IDX offered, the VTDecoderXPCService's own `decs` query (spawned by
 * a require-hw VTDecompressionSessionCreate) never kicks the host and times out
 * (kIOReturnTimeout 0xe00002d6); with it dropped, both decs round-trips land.
 * (Re-add only together with a correct avail_event write in vtpv_virtq_retire.) */
#define VT_DEVICE_FEATURES \
    (VIRTIO_F_INDIRECT_DESC | VIRTIO_F_VERSION_1)

/* virtio-modern PCI capability list (virtio 1.x §4.1.4). A virtio-modern driver
 * (Apple's AppleVirtIOPCITransport) reads these vendor caps to LOCATE the
 * common/notify/isr/device config structures in the BARs; without them it cannot
 * bring the device up (it resets before queue-enable). */
#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_ISR_CFG    3
#define VIRTIO_PCI_CAP_DEVICE_CFG 4
#define VIRTIO_PCI_CAP_PCI_CFG    5
#ifndef PCI_CAP_ID_VNDR
#define PCI_CAP_ID_VNDR 0x09
#endif

struct virtio_pci_cap {               /* starts with a struct cap_hdr {id,next}  */
    uint8_t  cap_vndr;                /* PCI_CAP_ID_VNDR (== cap_hdr.id)         */
    uint8_t  cap_next;                /* == cap_hdr.next (set by libvfio-user)   */
    uint8_t  cap_len;                 /* size of this capability                 */
    uint8_t  cfg_type;                /* VIRTIO_PCI_CAP_*                        */
    uint8_t  bar;                     /* which BAR                               */
    uint8_t  id;                      /* multiple caps of the same type          */
    uint8_t  padding[2];
    uint32_t offset;                  /* little-endian offset within the BAR     */
    uint32_t length;                  /* little-endian length of the structure   */
} __attribute__((packed));
struct virtio_pci_notify_cap {
    struct virtio_pci_cap cap;
    uint32_t notify_off_multiplier;   /* little-endian                           */
} __attribute__((packed));

#define VT_BAR2_SIZE       0x1000u    /* MSI-X table + PBA                       */
#define VT_MSIX_TABLE_OFF  0x0u
#define VT_MSIX_PBA_OFF    0x800u
#define VT_MSIX_BAR        2u

#define VT_MAX_REGIONS 64

/* Host->guest pending-message ring. Apple's queue 1 is a host->guest stream:
 * the guest posts 0x4000-byte writable buffers and "the host keeps its messages
 * in a ring and writes them into those buffers as they come back" (reims
 * devices.md, "The queues" -> "Queue 1, host to guest"). So a reply/request the
 * host emits while queue 1 has no posted buffer must NOT be dropped: it is held
 * here and delivered when the guest reposts and kicks queue 1. Dropping it (the
 * old behaviour) loses the early `decs` reply that fires during the guest's
 * decoder-registry dispatch_once before the kext has posted queue-1 buffers,
 * which caches an empty decoder list and makes every later selection fail with
 * kVTVideoDecoderNotAvailableNowErr (-12906). Biggest host->guest message is a
 * codec-list reply (<=2 KiB); 4 KiB slots give headroom. */
#define VT_PENDING_MSG_MAX 4096u
#define VT_PENDING_MAX     256u

typedef struct vt_dev {
    const qdp_host_ops *host;

    vtpv_vaapi              *va;
    vtpv_nvdec              *nvdec;
#ifdef VTPV_HAVE_LAVC
    vtpv_lavc               *lavc;     /* preferred: DPB-correct stream decode    */
#else
    void                    *lavc;
#endif
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

    /* BAR2 backing for the MSI-X table + PBA (accesses the client doesn't
     * intercept land here; libvfio-user owns the MSI-X cap itself). */
    uint8_t msix_bar[VT_BAR2_SIZE];

    /* host->guest pending-message ring (FIFO; see VT_PENDING_* above). */
    struct { uint32_t len; uint8_t buf[VT_PENDING_MSG_MAX]; } pending[VT_PENDING_MAX];
    unsigned pend_head;   /* index of the oldest pending message */
    unsigned pend_count;  /* number of pending messages */
    uint64_t pend_dropped;/* diagnostics: messages lost to a full ring */
} vt_dev;

/* the completion vq's MSI-X vector (set by the driver; config vector fallback) */
#define VT_MSIX_NO_VECTOR 0xffffu

/* libvfio-user region callbacks receive no private pointer (they get the
 * host's vfu_create_ctx private, not the plugin's), so the BAR0 callback
 * reaches the device through a global - one plugin instance per host process,
 * as the other plugins (test-v2) do. */
static vt_dev *g_dev;

/* format a FourCC as 4 printable chars for logging */
static void vt_fourcc(uint32_t v, char out[5])
{
    for (int i = 0; i < 4; i++) {
        uint8_t c = (uint8_t)(v >> (24 - 8 * i));
        out[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
    }
    out[4] = 0;
}

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

/* Scattered-page landing (non-contiguous guest surface): the frame is composed in a
 * host shadow of the surface allocation and moved to the guest pages by vt_copy_frame. */
typedef struct vt_landing {
    vt_dev   *d;
    uint8_t  *shadow;      /* shadow path (legacy backends): the composed allocation */
    size_t    total;
    uint32_t  npages;
    uint64_t  page_size;
    uint64_t  gpa_pages[PVG_BRIDGE_MAX_PAGES];
    /* direct path (stream core): the planes are written row by row into the guest
     * pages, no shadow (vt_scatter_resolve / vt_land_planes) */
    int       direct;
    size_t    y_off, y_stride, uv_off, uv_stride;
    vt_land_geom geom;    /* the lent surface's planes (GVA-LAND-BOUNDS): every write fits */
    uint32_t  surface_fmt; /* the lent surface's CoreVideo format (guest descriptor), 0 unknown */
    void     *ptrs[PVG_BRIDGE_MAX_PAGES];
    size_t    run[PVG_BRIDGE_MAX_PAGES];
} vt_landing;

static void *vt_scatter_map(void *ctx, uint64_t gpa, uint64_t len) { return vt_map(ctx, gpa, len); }

static void vt_raise(vt_dev *d, unsigned queue)
{
    uint16_t vec = queue < VT_QUEUES ? d->queue_msix[queue] : d->msix_config;
    if (d->host && d->host->irq_trigger && vec != VT_MSIX_NO_VECTOR)
        (void)d->host->irq_trigger(d->host->host, vec);
}

/* name a host->guest message's FourCC for logging (rels box or vtpv type). */
static void vt_msg_fourcc(const uint8_t *msg, size_t len, char fc[5])
{
    memcpy(fc, "????", 5);
    if (len >= 8) {
        uint32_t first = aw_get_be32(msg + 4);
        if (first == AW_ATOM_RELS) memcpy(fc, "rels", 5);
        else if (first == AW_ATOM_VTPV && len >= 12) vt_fourcc(aw_get_be32(msg + 8), fc);
    }
}

/* Deliver as many pending host->guest messages as queue 1 has posted buffers,
 * oldest first (FIFO, preserving the host's emit order). Stops at the first
 * message with no buffer available, leaving it and the rest pending for the
 * next queue-1 repost. Called under d->m. */
static void vt_flush_pending(vt_dev *d)
{
    vtpv_virtq *hq = &d->queues[1];
    unsigned delivered = 0;
    while (d->pend_count) {
        unsigned i = d->pend_head;
        const uint8_t *msg = d->pending[i].buf;
        uint32_t len = d->pending[i].len;
        uint16_t head = 0; uint32_t written = 0;
        int rc = vtpv_virtq_put_writable(hq, vt_map, d, msg, len, &head, &written);
        if (rc == 0) break;                      /* no posted buffer yet: keep it */
        char fc[5]; vt_msg_fourcc(msg, len, fc);
        if (rc < 0) {                            /* malformed ring */
            d->needs_reset = 1;
            vt_log(d, 1, "q1 '%s' ring error, needs reset", fc);
            break;
        }
        vtpv_virtq_retire(hq, vt_map, d, head, written);
        if (written < len)
            vt_log(d, 1, "q1 '%s' truncated %u/%u (guest buffer too small)", fc, written, len);
        else
            vt_log(d, 3, "q1 '%s' (%u B) -> guest", fc, len);
        d->pend_head = (d->pend_head + 1) % VT_PENDING_MAX;
        d->pend_count--;
        delivered++;
    }
    if (delivered) vt_raise(d, 1);
}

/* ---- queue 1 (host->guest) emit sink -------------------------------- *
 * The aw_session engine's emit callback (under d->m). Apple's queue 1 is a
 * host->guest message stream: the guest pre-posts 0x4000-byte writable buffers,
 * and the host writes each outbound message (a reply, or a Dpba/Dbuf/Demt/rels
 * request) into one and retires it. A message emitted while no buffer is posted
 * is NOT dropped: it is appended to the pending ring and delivered when the
 * guest reposts queue-1 buffers and kicks queue 1 (vt_flush_pending), as Apple's
 * real host does. This closes the early-`decs` race (see VT_PENDING_* above). */
static void vt_emit(void *ctx, const uint8_t *msg, size_t len)
{
    vt_dev *d = ctx;
    char fc[5]; vt_msg_fourcc(msg, len, fc);
    /* AGVA_DEBUG: ground-truth hexdump of the EXACT bytes emitted for a
     * host->guest message (what the guest framework receives, since delivery is
     * byte-faithful). Gated by the env var so it is off in production. Lets a
     * disposable-guest run capture the on-wire decs reply for an independent
     * CopyCFPropertyList/plistlib replay-parse (L-GVA-SELFIX). */
    if (getenv("AGVA_DEBUG")) {
        char line[VT_PENDING_MSG_MAX * 3 + 1];
        size_t o = 0, cap = sizeof line;
        for (size_t i = 0; i < len && o + 3 < cap; i++)
            o += (size_t)snprintf(line + o, cap - o, "%02x%s", msg[i],
                                  (i + 1 < len) ? " " : "");
        line[o] = 0;
        vt_log(d, 1, "AGVA_DEBUG q1 EMIT '%s' %zu B: %s", fc, len, line);
    }
    if (len > VT_PENDING_MSG_MAX) {       /* must not happen for host->guest msgs */
        d->pend_dropped++;
        vt_log(d, 1, "q1 '%s' DROPPED: %zu B exceeds pending slot %u", fc, len, VT_PENDING_MSG_MAX);
        return;
    }
    if (d->pend_count >= VT_PENDING_MAX) { /* backlog overflow: drop oldest, log */
        d->pend_dropped++;
        vt_log(d, 1, "q1 pending ring full (%u): dropping oldest to queue '%s'",
               VT_PENDING_MAX, fc);
        d->pend_head = (d->pend_head + 1) % VT_PENDING_MAX;
        d->pend_count--;
    }
    unsigned tail = (d->pend_head + d->pend_count) % VT_PENDING_MAX;
    d->pending[tail].len = (uint32_t)len;
    memcpy(d->pending[tail].buf, msg, len);
    d->pend_count++;
    vt_flush_pending(d);                  /* deliver now if a buffer is posted */
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
 * mapper lives inside uniq-vgpu/qmu (using_iosurface_mapper = 0 today) and is
 * NOT reachable from this plugin. To make the id a true opaque mapper token
 * (not a GPA), the plugin needs a plugin-boundary resolver
 * `map_guest_surface(mapper_ctx, mapping_id) -> {dma-buf fd or host ptr + plane
 * layout}`. Until that lands, the GPA convention above is the bring-up path. */
/* increment 2: open the uniq-vgpu<->apple-videotoolbox IOSurface-mapper bridge (shm), once,
 * lazily. The vfio-user launcher sets PVG_MAPPER_BRIDGE_SHM to the VM-scoped shm name that
 * uniq-vgpu creates. Returns NULL when no bridge is wired (then resolve falls back to the
 * GPA-as-mapping-id bring-up path below). */
static int vt_bridge_configured(void)
{
    const char *name = getenv("PVG_MAPPER_BRIDGE_SHM");
    return name && *name;
}

static const pvg_mapper_bridge *vt_bridge(void)
{
    /* Opened lazily and retried until it opens: uniq-vgpu creates the region at its
     * first mapper publish. With a bridge configured, a missing region is a miss
     * (the frame is dropped), never the GPA bring-up path. */
    static const pvg_mapper_bridge *b = NULL;
    if (!b) {
        const char *name = getenv("PVG_MAPPER_BRIDGE_SHM");
        if (name && *name) {
            int fd = shm_open(name, O_RDONLY, 0);
            struct stat st;
            /* only a region the writer has already sized (a read past its end would be SIGBUS) */
            if (fd >= 0 && fstat(fd, &st) == 0 && (size_t)st.st_size >= pvg_mapper_bridge_bytes()) {
                void *p = mmap(NULL, pvg_mapper_bridge_bytes(), PROT_READ, MAP_SHARED, fd, 0);
                if (p != MAP_FAILED && pvg_mapper_bridge_valid((const pvg_mapper_bridge *)p))
                    b = (const pvg_mapper_bridge *)p;
                else if (p != MAP_FAILED)
                    munmap(p, pvg_mapper_bridge_bytes()); /* another layout: not ours, retried later */
            }
            if (fd >= 0) close(fd);
        }
    }
    return b;
}

static int vt_resolve_buffer(void *ctx, const uint8_t uuid[16], uint64_t mapping_id,
                             uint32_t w, uint32_t h, aw_out_buffer *out)
{
    vt_dev *d = ctx;
    /* L-GVA-STRIDE: land luma at the guest surface's rowBytes, not VA-API's
     * 64-byte alignment. The host has NO knowledge of the guest surface layout
     * (the Dbuf reply carries only a mapping id - the H2-QMETAL-SURFACE-MAPPER-ASK
     * gap below), so bcb1d78 just imposed align(w,64) = 384. But the guest's
     * VideoToolbox output CVPixelBuffer is L008 with CoreVideo's default 16-byte
     * rowBytes = 352 (w=352). Writing luma at 384 leaves a 32-byte UNWRITTEN pad
     * per row (cols 352..383 keep prior guest-RAM garbage); the downstream
     * guest-side NV12->L008 delivery copy then reads that garbage, producing the
     * live 526/101376 luma artifact (a [13,00] value every 386 guest-linear bytes,
     * corrupting even row 0 - see evidence/runs/applegva-stride-20261117). Align
     * to 16 so the luma plane is written GAPLESSLY at the guest rowBytes, leaving
     * no pad for that copy to pick up. NB: the delivered buffer is provably a
     * separate buffer from this lent one (delivered[237]=13 while the host writes
     * 128 to every real column), so final pixel-exact closure also depends on that
     * guest-side copy honouring the same 352 stride - confirm live. The
     * authoritative rowBytes should arrive via the surface-mapper token. */
    /* L-GVA-SURFACE: a surface-less Dbuf reply carries no `subs` box, so
     * mapping_id arrives as 0 (the "no lent buffer" sentinel - reims' subs ids
     * stop at the first zero). mapping_id 0 is NOT a valid landing: vt_map(0,..)
     * resolves to the base of guest RAM (guest-physical 0), so the decode would
     * be DMA'd nowhere useful while the guest delivers its own never-written
     * output IOSurface (stale RAM that aliases the input bitstream - the
     * "delivered == sample.bin[156:]" / misleading-0.5% illusion, proven in
     * evidence/runs/applegva-stride-guest-20261117). Refuse it so the engine
     * drops the frame honestly (no Demt success) instead of reporting a false
     * delivery. A real NONZERO mapping_id is an opaque IOSurface-mapper token
     * (IOSurfaceGetID), not a GPA, whose (possibly scattered udmabuf) pages only
     * resolve through the paravirtual-GPU IOSurface mapper
     * (uniq-vgpu/qmu retain_io_surface_with_mapping_id /
     * address_ranges_for_virtual_page, gated by using_iosurface_mapper=0 today).
     * The GPA-as-mapping-id path below is the bring-up shortcut and lands pixels
     * in the guest only once that mapper is exposed to this plugin (the
     * H2-QMETAL-SURFACE-MAPPER-ASK / L-GVA-SURFACE cross-component dependency). */
    if (mapping_id == 0) {
        vt_log(d, 1, "Dbuf reply lent no output surface (mapping_id 0): dropping the "
               "frame. The decoded surface backing needs the uniq-vgpu IOSurface "
               "mapper (using_iosurface_mapper); see L-GVA-SURFACE.");
        return -1;
    }

    /* increment 2: the v10 IOSurface-mapper path. A nonzero mapping_id is an opaque mapper token;
     * resolve it through the uniq-vgpu bridge -> {guest-physical pages + AUTHORITATIVE NV12 layout
     * from the guest surface descriptor}. This supersedes the GPA-as-mapping-id bring-up shortcut
     * below AND the align(w,16) stride guess (the L-GVA-STRIDE fix: the rowBytes now comes from the
     * guest surface, not a guess). The honest drop stays for a genuinely-unmapped id (bridge miss
     * with a bridge present), and the GPA shortcut below stays for runs with no bridge. */
    {
        const pvg_mapper_bridge *bridge = vt_bridge();
        pvg_bridge_resolved r;
        if (bridge && pvg_mapper_bridge_lookup(bridge, (uint32_t)mapping_id, &r) == 0 &&
            r.page_count > 0) {
            const size_t ps = bridge->page_size ? (size_t)bridge->page_size : 4096u;
            size_t y_stride = (r.plane_count >= 1 && r.planes[0].bytes_per_row)
                                  ? (size_t)r.planes[0].bytes_per_row : (size_t)r.width;
            /* Plane addresses (IOSurface semantics, guest descriptor = reference
             * populate_plane_descriptor): kIOSurfacePlaneOffset is the plane's offset
             * from the START OF THE BUFFER, so a planar surface's planes are addressed
             * buffer + planes[i].offset; base_offset (kIOSurfaceOffset) places only a
             * non-planar image. */
            const int planar = r.plane_count >= 1;
            size_t y_off = planar ? (size_t)r.planes[0].offset : (size_t)r.base_offset;
            size_t uv_off = (r.plane_count >= 2) ? (size_t)r.planes[1].offset
                                                 : y_off + y_stride * (size_t)r.height;
            size_t uv_stride = (r.plane_count >= 2 && r.planes[1].bytes_per_row)
                                   ? (size_t)r.planes[1].bytes_per_row : y_stride;
            size_t total = (size_t)(r.alloc_size ? r.alloc_size
                                                 : uv_off + uv_stride * ((r.height + 1) / 2));
            uint8_t *p = NULL;
            vt_landing *land = NULL;
            /* GVA-LAND-BOUNDS: every bridge surface - contiguous or not - lands through the
             * page list with the surface's own plane geometry, so no write leaves it (the
             * old contiguous path wrote the picture's full size from gpa_base and ran past
             * small, physically contiguous surfaces into guest kernel memory). */
            {
                /* The surface's pages (scattered or contiguous): the stream core writes the
                 * fitting rows straight into them (vt_land_direct); the legacy backends compose
                 * the frame in a host shadow of the allocation (seeded from the guest pages so
                 * untouched bytes are preserved) and scatter it back in vt_copy_frame.
                 * Contract: vt_scatter.h, vt_land.h. */
                size_t need = uv_off + uv_stride * (((size_t)r.height + 1) / 2);
                if (total < need) total = need;
                if (r.page_count > PVG_BRIDGE_MAX_PAGES || total > (size_t)r.page_count * ps) {
                    vt_log(d, 1, "mapper id %llu: %u scattered pages cover less than the %zu B surface; dropping",
                           (unsigned long long)mapping_id, r.page_count, total);
                    return -1;
                }
                land = calloc(1, sizeof *land);
                if (!land) return -1;
                land->d = d; land->total = total; land->npages = r.page_count; land->page_size = ps;
                land->surface_fmt = r.pixel_format;
                memcpy(land->gpa_pages, r.gpa_pages, (size_t)r.page_count * sizeof(uint64_t));
                land->geom.total = total;
                land->geom.y_off = y_off; land->geom.y_stride = y_stride;
                land->geom.uv_off = uv_off; land->geom.uv_stride = uv_stride;
                if (r.plane_count >= 1) {
                    land->geom.y_w = (uint32_t)r.planes[0].width; land->geom.y_h = (uint32_t)r.planes[0].height;
                }
                if (r.plane_count >= 2) {
                    land->geom.uv_w = (uint32_t)r.planes[1].width; land->geom.uv_h = (uint32_t)r.planes[1].height;
                }
#ifdef VTPV_HAVE_LAVC
                /* The stream core exposes its picture's planes: land them straight into
                 * the guest pages in vt_copy_frame (one copy of the visible rows, no 12 MB
                 * shadow, gather or write-back per 4K frame). Bytes outside the rows keep
                 * the guest's content, as the shadow round trip leaves them. Every page is
                 * resolved here (drop if one is unmapped) and again before the first byte
                 * moves (all-or-nothing). */
                land->direct = d->lavc != NULL;
#endif
                if (land->direct) {
                    land->y_off = y_off; land->y_stride = y_stride;
                    land->uv_off = uv_off; land->uv_stride = uv_stride;
                    if (vt_scatter_resolve(land->ptrs, land->run, total, land->gpa_pages, land->npages, ps,
                                           vt_scatter_map, d) != VT_SCATTER_OK) {
                        vt_log(d, 1, "mapper id %llu: scattered pages not all in the guest DMA regions (%zu B); dropping",
                               (unsigned long long)mapping_id, total);
                        free(land);
                        return -1;
                    }
                } else {
                    land->shadow = malloc(total);
                    if (!land->shadow) { free(land); return -1; }
                    if (vt_scatter_gather(land->shadow, total, land->gpa_pages, land->npages, ps,
                                          vt_scatter_map, d) != VT_SCATTER_OK) {
                        vt_log(d, 1, "mapper id %llu: scattered pages not all in the guest DMA regions (%zu B); dropping",
                               (unsigned long long)mapping_id, total);
                        free(land->shadow); free(land);
                        return -1;
                    }
                }
                p = land->shadow;
            }
            if (!p && !(land && land->direct)) {
                vt_log(d, 1, "mapper id %llu gpa %#llx: no guest pages (%zu B) in the DMA regions",
                       (unsigned long long)mapping_id, (unsigned long long)r.gpa_base, total);
                return -1;
            }
            memset(out, 0, sizeof *out);
            memcpy(out->uuid, uuid, 16);
            out->mapping_id = mapping_id;
            out->landing = land;
            out->y = p ? p + y_off : NULL;                  out->y_stride = y_stride;
            out->uv = p ? p + uv_off : NULL;                out->uv_stride = uv_stride;
            vt_log(d, 3, "mapper id %llu layout: base_off %llu planes %u y_off %zu y_stride %zu uv_off %zu uv_stride %zu alloc %llu",
                   (unsigned long long)mapping_id, (unsigned long long)r.base_offset, r.plane_count,
                   y_off, y_stride, uv_off, uv_stride, (unsigned long long)r.alloc_size);
            out->width = w; out->height = h;
            vt_log(d, 3, "mapper id %llu resolved via bridge (%s): gpa %#llx %ux%u y_stride %zu uv@%zu",
                   (unsigned long long)mapping_id, land ? (land->direct ? "scatter, direct" : "scatter") : "contiguous",
                   (unsigned long long)r.gpa_base, w, h, y_stride, uv_off);
            return 0;
        }
    }

    if (vt_resolve_route_for(mapping_id, vt_bridge_configured(), 0) != VT_ROUTE_GPA) {
        vt_log(d, 1, "mapper id %llu is not in the mapper bridge: frame not landed (a mapping id is "
               "never taken as a guest-physical address while a bridge is configured)",
               (unsigned long long)mapping_id);
        return -1;
    }
    uint32_t ystride = (w + 15u) & ~15u;          /* guest CoreVideo rowBytes (16)  */
    size_t   ysize   = (size_t)ystride * h;
    size_t   csize   = (size_t)ystride * ((h + 1u) / 2u); /* NV12 interleaved UV   */
    uint8_t *p = vt_map(d, mapping_id, ysize + csize);
    if (!p) {
        vt_log(d, 1, "Dbuf surface gpa %#llx: NO guest pages (%zu B) in the DMA regions "
               "- is the lent buffer in memfd-shared guest RAM?",
               (unsigned long long)mapping_id, ysize + csize);
        return -1;
    }
    vt_log(d, 3, "Dbuf surface gpa %#llx resolved: %ux%u NV12, y_stride %u, %zu B",
           (unsigned long long)mapping_id, w, h, ystride, ysize + csize);
    memset(out, 0, sizeof *out);
    memcpy(out->uuid, uuid, 16);
    out->mapping_id = mapping_id;
    out->y = p;             out->y_stride = ystride;
    out->uv = p + ysize;    out->uv_stride = ystride;
    out->width = w;         out->height = h;
    return 0;
}
#ifdef VTPV_HAVE_LAVC
/* Direct landing of the stream core's picture into scattered guest pages: the same
 * rows and checks as read_luma/read_chroma into the shadow + vt_scatter_write (luma
 * width bytes x height rows at y_off/y_stride, CbCr width rounded to even x
 * ceil(height/2) rows at uv_off/uv_stride), all-or-nothing: every page resolved and
 * both planes' ranges checked before the first byte moves. */
static int vt_land_direct(vt_dev *d, const vtpv_decoded_frame *f, vt_landing *l)
{
    vtpv_lavc_frame fr;
    vt_land_rows lr;
    uint32_t w = f->width, h = f->height;
    if (!d->lavc || vtpv_lavc_get_frame(d->lavc, f->surface_id, &fr) != VTPV_OK ||
        w > fr.width || h > fr.height || !w || !h)
        return -1;
    /* D-GVA-P010: NV12 lands in a '420v'/'420f' surface a byte a sample, P010 in an
     * 'x420'/'xf20' one two bytes a sample (the pool Dpba asked for); any other pair
     * fails the frame rather than landing in a layout the guest reads differently. */
    const int bpp = vt_land_bpp(fr.pixfmt, l->surface_fmt);
    if (bpp < 0) {
        vt_log(d, 1, "landing: a %#x picture does not go into a %#x surface; frame not delivered",
               fr.pixfmt, l->surface_fmt);
        return -1;
    }
    if (vt_scatter_resolve(l->ptrs, l->run, l->total, l->gpa_pages, l->npages, l->page_size,
                           vt_scatter_map, d) != VT_SCATTER_OK) {
        vt_log(d, 1, "landing: surface pages not all in the guest DMA regions (%zu B); frame not delivered", l->total);
        return -1;
    }
    /* GVA-LAND-BOUNDS: the picture lands at the surface's top left, only what fits each
     * plane (reference: Apple's host writes the picture unscaled at the top left of a
     * buffer of the description's size); nothing outside the lent surface is written. */
    int rc = vt_land_planes(l->ptrs, l->run, l->page_size, &l->geom, fr.plane[0], fr.stride[0],
                            fr.plane[1], fr.stride[1], w, h, bpp, &lr);
    if (rc != 0) {
        vt_log(d, 1, "landing: %ux%u picture does not fit the lent surface (%zu B, rc %d); frame not delivered",
               w, h, l->total, rc);
        return -1;
    }
    if (lr.cropped)
        vt_log(d, 3, "landing: %ux%u picture larger than the lent surface: top-left %zux%zu landed",
               w, h, lr.y_bytes, lr.y_rows);
    return 0;
}
#endif

static int vt_copy_frame(void *ctx, const vtpv_decoded_frame *f, const aw_out_buffer *b)
{
    vt_dev *d = ctx;
    int rc;
#ifdef VTPV_HAVE_LAVC
    if (b->landing && ((const vt_landing *)b->landing)->direct)
        return vt_land_direct(d, f, b->landing);
#endif
    if (!b->y) return -1;
    /* land the NV12 frame: Y plane, then the interleaved UV plane. Route to
     * whichever backend is active (the libavcodec stream core, else the legacy
     * intra-only NVDEC / VA-API backends). GVA-LAND-BOUNDS: only what fits the lent
     * surface - the shadow of a bridge surface (its plane geometry) or the GPA
     * bring-up surface laid out for the description's size b->width x b->height. */
    uint32_t lw = f->width, lh = f->height, cw = f->width, ch = f->height;
    /* the legacy (no-stream-core) and bring-up paths land NV12 only (D-GVA-P010:
     * P010 goes through the stream core's direct landing) */
    if (vt_land_bpp(f->pixfmt, b->landing ? ((const vt_landing *)b->landing)->surface_fmt : 0) != 1) {
        vt_log(d, 1, "landing: a %#x picture does not go into this surface; frame not delivered", f->pixfmt);
        return -1;
    }
    if (b->landing) {
        vt_land_rows lr;
        const vt_landing *l = b->landing;
        if (vt_land_fit(&l->geom, f->width, f->height, 1, &lr) != 0) return -1;
        lw = (uint32_t)lr.y_bytes; lh = (uint32_t)lr.y_rows;
        cw = (uint32_t)lr.uv_bytes; ch = (uint32_t)lr.uv_rows * 2u;
        if (ch > f->height) ch = f->height;
        if (cw > f->width) cw = f->width;
    } else {
        if (b->width && lw > b->width) lw = b->width;
        if (b->height && lh > b->height) lh = b->height;
        cw = lw; ch = lh;
    }
#ifdef VTPV_HAVE_LAVC
    if (d->lavc) {
        rc = vtpv_lavc_read_luma(d->lavc, f->surface_id, b->y, b->y_stride, lw, lh);
        if (rc == 0 && b->uv && ch)
            rc = vtpv_lavc_read_chroma(d->lavc, f->surface_id, b->uv, b->uv_stride, cw, ch);
    } else
#endif
    if (d->nvdec) {
        rc = vtpv_nvdec_read_luma(d->nvdec, f->surface_id, b->y, b->y_stride, lw, lh);
        if (rc == 0 && b->uv && ch)
            rc = vtpv_nvdec_read_chroma(d->nvdec, f->surface_id, b->uv, b->uv_stride, cw, ch);
    } else {
        if (!d->va) return -1;
        rc = vtpv_vaapi_read_luma(d->va, f->surface_id, b->y, b->y_stride, lw, lh);
        if (rc == 0 && b->uv && ch)
            rc = vtpv_vaapi_read_chroma(d->va, f->surface_id, b->uv, b->uv_stride, cw, ch);
    }
    /* backend-independent: a scattered landing composes into the shadow above for
     * either backend, so the write-back must run for NVDEC as well as VA-API. */
    if (rc == 0 && b->landing) {
        /* scattered guest surface: move the composed frame to its pages (all-or-nothing). */
        vt_landing *l = b->landing;
        if (vt_scatter_write(l->shadow, l->total, l->gpa_pages, l->npages, l->page_size,
                             vt_scatter_map, d) != VT_SCATTER_OK) {
            vt_log(d, 1, "scatter write to guest pages failed (%zu B); frame not delivered", l->total);
            rc = -1;
        }
    }
    return rc;
}
static void vt_put_buffer(void *ctx, const aw_out_buffer *b)
{
    (void)ctx;
    if (b && b->landing) {
        vt_landing *l = b->landing;
        free(l->shadow);
        free(l);
    }
}

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
        { char hx[81*3]; size_t hn = len < 80 ? len : 80, o = 0;
          for (size_t i = 0; i < hn; i++) o += (size_t)snprintf(hx+o, sizeof hx-o, "%02x ", msg[i]);
          hx[o] = 0; vt_log(d, 3, "q0 RX len=%zu bytes: %s", len, hx); }
        int32_t status = aw_engine_deliver(d->engine, msg, len); /* emits on q1 */
        { aw_msg_view mv; char fc[5] = "????";
          if (aw_parse(msg, len, &mv) == AW_PARSE_OK) vt_fourcc(mv.type, fc);
          vt_log(d, 3, "q0 '%s' (%zu B, flags %#x) -> status %d", fc, len,
                 (aw_parse(msg, len, &mv) == AW_PARSE_OK) ? mv.flags : 0, status); }
        uint32_t written = 0;
        if (wptr && wcap >= 4) { memcpy(wptr, &status, 4); written = 4; } /* native i32 */
        vtpv_virtq_retire(sq, vt_map, d, head, written);
    }
    vt_raise(d, 0);        /* the queue-0 chains are now free on the used ring */
    vt_flush_pending(d);   /* deliver any replies/requests this drain queued on q1 */
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
            if (qn == 0) vt_process_submission(d);  /* drain guest->host submissions */
            else if (qn == 1) vt_flush_pending(d);  /* guest reposted q1 buffers: deliver backlog */
        }
    } else if (!is_write) {
        memset(buf, 0, count);
    }

    pthread_mutex_unlock(&d->m);
    return (ssize_t)count;
}

/* ---- BAR2: MSI-X table + PBA backing -------------------------------- *
 * libvfio-user owns the MSI-X capability (enable/mask) and vector delivery; this
 * region just backs any table/PBA access that reaches the device. */
static ssize_t vt_bar2(vfu_ctx_t *ctx, char *buf, size_t count, loff_t off,
                       bool is_write)
{
    vt_dev *d = g_dev;
    (void)ctx;
    if (!d || off < 0 || (size_t)off + count > VT_BAR2_SIZE)
        return is_write ? (ssize_t)count : -1;
    pthread_mutex_lock(&d->m);
    if (is_write) memcpy(d->msix_bar + off, buf, count);
    else          memcpy(buf, d->msix_bar + off, count);
    pthread_mutex_unlock(&d->m);
    return (ssize_t)count;
}

/* Add the MSI-X capability and the virtio-modern config-structure capabilities
 * so a virtio-modern guest driver can locate the config windows and MSI-X. */
static int vt_add_caps(vfu_ctx_t *ctx)
{
    struct msixcap msix;
    memset(&msix, 0, sizeof msix);
    msix.hdr.id = PCI_CAP_ID_MSIX;
    msix.mxc.ts = VT_MSIX_VECTORS - 1;        /* table size - 1                  */
    msix.mtab.tbir = VT_MSIX_BAR;
    msix.mtab.to   = VT_MSIX_TABLE_OFF >> 3;
    msix.mpba.pbir = VT_MSIX_BAR;
    msix.mpba.pbao = VT_MSIX_PBA_OFF >> 3;
    if (vfu_pci_add_capability(ctx, 0, 0, &msix) < 0) return -1;

    struct virtio_pci_cap c;
#define VCAP(type, boff, blen) do {                                            \
        memset(&c, 0, sizeof c);                                              \
        c.cap_vndr = PCI_CAP_ID_VNDR; c.cap_len = sizeof c;                    \
        c.cfg_type = (type); c.bar = 0;                                        \
        c.offset = (uint32_t)(boff); c.length = (uint32_t)(blen);             \
        if (vfu_pci_add_capability(ctx, 0, 0, &c) < 0) return -1;             \
    } while (0)
    VCAP(VIRTIO_PCI_CAP_COMMON_CFG, VT_CFG_COMMON, 0x1000);
    VCAP(VIRTIO_PCI_CAP_ISR_CFG,    VT_CFG_ISR,    0x1000);

    struct virtio_pci_notify_cap ncap;
    memset(&ncap, 0, sizeof ncap);
    ncap.cap.cap_vndr = PCI_CAP_ID_VNDR; ncap.cap.cap_len = sizeof ncap;
    ncap.cap.cfg_type = VIRTIO_PCI_CAP_NOTIFY_CFG; ncap.cap.bar = 0;
    ncap.cap.offset = VT_CFG_NOTIFY; ncap.cap.length = 0x1000;
    ncap.notify_off_multiplier = 4;           /* notify @ BAR0:0x4000 + q*4       */
    if (vfu_pci_add_capability(ctx, 0, 0, &ncap) < 0) return -1;

    VCAP(VIRTIO_PCI_CAP_DEVICE_CFG, VT_CFG_DEVICE, 0x1000);
    VCAP(VIRTIO_PCI_CAP_PCI_CFG,    0, 0);     /* the alternate PCI-config access  */
#undef VCAP
    return 0;
}

/* ---- test-only fallback backend (plugin arg force_h264=1) -----------
 * Advertise a single H.264 decoder in the decs/caps reply even when no working
 * VA-API decode backend is present (e.g. an NVIDIA-only host where libva cannot
 * init). This exists ONLY for the decoder-SELECTION experiment (L-VIDEO-267):
 * it lets the guest's VTDecompressionSession register + attempt to select our
 * paravirt avc1 decoder, which is decided BEFORE any frame is decoded. Actual
 * decode is unsupported here (open_decoder/decode return errors), so this must
 * never be used for a real decode run. The advertised caps are byte-identical
 * in shape to the live VA-API path (avc1, NV12, 4096x4096). */
static int fake_query_caps(void *ctx, vtpv_caps *out, size_t cap)
{
    (void)ctx;
    vtpv_codec_cap *c = (vtpv_codec_cap *)((uint8_t *)out + sizeof(*out));
    if (cap < sizeof(*out) + sizeof(*c)) return -1;
    out->magic = VTPV_CAPS_MAGIC;
    out->abi = VTPV_ABI;
    c[0].codec = VTPV_CODEC_H264; c[0].can_decode = 1; c[0].can_encode = 0;
    c[0].reserved = 0; c[0].max_width = 4096; c[0].max_height = 4096;
    c[0].out_pixfmt = VTPV_PIXFMT_NV12;
    out->n_codecs = 1;
    return 1;
}
static int fake_open_decoder(void *ctx, uint32_t codec, uint32_t w, uint32_t h,
                             const uint8_t *ex, size_t exl, void **dec)
{
    (void)ctx; (void)codec; (void)w; (void)h; (void)ex; (void)exl;
    if (dec) *dec = NULL;
    return -1;   /* decode not supported in the force_h264 fallback */
}
static int fake_decode(void *dec, const uint8_t *s, size_t sl,
                       vtpv_decoded_frame *o)
{ (void)dec; (void)s; (void)sl; (void)o; return -1; }
static void fake_close(void *dec) { (void)dec; }
static const vtpv_decoder_backend fake_h264_be = {
    fake_query_caps, fake_open_decoder, fake_decode, fake_close, NULL
};

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
    d->device_features = VT_DEVICE_FEATURES;

    /* Pick a decode backend. With libavcodec (VTPV_HAVE_LAVC) the stream core
     * is used: CUDA/NVDEC on an NVIDIA GPU, else VA-API on the DRM render node;
     * it keeps the DPB across frames (P/B pictures, HEVC). `backend=` selects:
     * auto (default: NVDEC first), nvdec, vaapi (stream core on that hardware),
     * nvdec-intra / vaapi-intra (the legacy one-frame-intra backends, kept for
     * comparison). `gpu=<cuda ordinal>` selects the NVIDIA GPU. */
    {
        char want[16] = "auto";
        const char *gpu = NULL, *q;
        int try_nvdec, try_vaapi, legacy;
        if (args && (q = strstr(args, "backend="))) {
            size_t n = strcspn(q + 8, ", ");
            if (n >= sizeof want) n = sizeof want - 1;
            memcpy(want, q + 8, n); want[n] = 0;
        }
        if (args && (q = strstr(args, "gpu=")))     gpu = q + 4;
        legacy    = strstr(want, "-intra") != NULL;
        try_nvdec = strncmp(want, "vaapi", 5) != 0; /* auto or nvdec */
        try_vaapi = strncmp(want, "nvdec", 5) != 0; /* auto or vaapi */
#ifdef VTPV_HAVE_LAVC
        if (!legacy) {
            if (try_nvdec) {
                char ord[16] = "0";
                if (gpu) { size_t n = strcspn(gpu, ", "); if (n >= sizeof ord) n = sizeof ord - 1; memcpy(ord, gpu, n); ord[n] = 0; }
                rc = vtpv_lavc_open(VTPV_LAVC_HW_CUDA, ord, &d->lavc, err, sizeof(err));
                if (rc != 0) { vt_log(d, 2, "stream core: CUDA/NVDEC unavailable: %s", err); d->lavc = NULL; }
            }
            if (!d->lavc && try_vaapi) {
                rc = vtpv_lavc_open(VTPV_LAVC_HW_VAAPI, node, &d->lavc, err, sizeof(err));
                if (rc != 0) { vt_log(d, 1, "stream core: VA-API unavailable on %s: %s", node, err); d->lavc = NULL; }
            }
            if (d->lavc) d->be = vtpv_lavc_backend();
        }
#else
        if (!legacy)
            vt_log(d, 1, "built without libavcodec: using the legacy intra-only backends");
#endif
        if (!d->be && try_nvdec) {
            rc = vtpv_nvdec_open(gpu, &d->nvdec, &d->be, NULL, err, sizeof(err));
            if (rc != 0) {
                vt_log(d, 2, "NVDEC backend unavailable: %s", err);
                d->nvdec = NULL; d->be = NULL;
            }
        }
        if (!d->be && try_vaapi) {
            rc = vtpv_vaapi_open(node, &d->va, &d->be, NULL, err, sizeof(err));
            if (rc != 0) {
                vt_log(d, 1, "VA-API backend unavailable on %s: %s; the device "
                             "will advertise no codecs", node, err);
                d->va = NULL; d->be = NULL;
            }
        }
    }
    if (!d->be && args && strstr(args, "force_h264=1")) {
        vt_log(d, 1, "force_h264=1: advertising a fallback H.264 decoder "
                     "(decode UNSUPPORTED; decoder-SELECTION experiment only)");
        d->be = &fake_h264_be;
    }
    {
        aw_session_cfg cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.be = d->be;
        cfg.be_ctx = d->lavc ? (void *)d->lavc
                   : d->nvdec ? vtpv_nvdec_ctx(d->nvdec)
                   : (d->va ? vtpv_vaapi_ctx(d->va) : NULL);
        cfg.emit = vt_emit; cfg.emit_ctx = d;
        cfg.resolve_buffer = vt_resolve_buffer;
        cfg.copy_frame = vt_copy_frame;
        cfg.put_buffer = vt_put_buffer;
        cfg.buf_ctx = d;
        d->engine = aw_engine_create(&cfg);
    }
    if (!d->engine) {
        VT_LAVC_CLOSE(d);
        if (d->nvdec) vtpv_nvdec_close(d->nvdec);
        if (d->va) vtpv_vaapi_close(d->va);
        free(d);
        return -ENOMEM;
    }

    /* PCI identity the guest kext matches (IOPCIPrimaryMatch 0xEEE21B36) */
    if (vfu_pci_init(ctx, VFU_PCI_TYPE_EXPRESS, PCI_HEADER_TYPE_NORMAL, 0) < 0)
        goto fail;
    vfu_pci_set_id(ctx, VT_PCI_VENDOR, VT_PCI_DEVICE, VT_PCI_VENDOR, VT_PCI_DEVICE);
    vfu_pci_set_class(ctx, VT_PCI_CLASS, VT_PCI_SUBCLASS, 0x00);

    /* MSI-X + the virtio-modern PCI capability list (so the guest driver can find
     * the config windows). Must be added before the device is realized. */
    if (vt_add_caps(ctx) < 0)
        goto fail;

    /* BAR0: virtio-modern config window (common/ISR/notify/device). */
    if (vfu_setup_region(ctx, VFU_PCI_DEV_BAR0_REGION_IDX, VT_BAR0_SIZE, vt_bar0,
                         VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM, NULL, 0, -1, 0) < 0)
        goto fail;

    /* BAR2: the MSI-X table + PBA. */
    if (vfu_setup_region(ctx, VFU_PCI_DEV_BAR2_REGION_IDX, VT_BAR2_SIZE, vt_bar2,
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
           d->lavc ? "stream decode (libavcodec)" : d->nvdec ? "NVDEC intra decode"
                    : d->va ? "VA-API intra decode" : d->be ? "stub decode" : "no backend");
    return 0;

fail:
    aw_engine_destroy(d->engine);
    VT_LAVC_CLOSE(d);
    if (d->nvdec) vtpv_nvdec_close(d->nvdec);
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
    VT_LAVC_CLOSE(d);
    if (d->nvdec) vtpv_nvdec_close(d->nvdec);
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
    d->pend_head = d->pend_count = 0;   /* drop any undelivered host->guest backlog */
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
