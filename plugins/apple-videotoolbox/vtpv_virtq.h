/*
 * vtpv_virtq.h - the virtio 1.x split-virtqueue mechanics for the AppleGVA
 * host VideoToolbox device, separated from the plugin so they are unit-
 * testable against a synthetic ring (no guest VM).
 *
 * AppleGVA host backend, increment 2 (lane L-M2V03, 2026-11-16).
 *
 * CLEAN-ROOM: written from the VIRTIO 1.x split-virtqueue spec (desc table /
 * available ring / used ring) and the device contract. It does not copy code
 * from the reims reference. Guest memory is reached through a caller map
 * callback (the plugin backs it with the qdev-host/vfio-user DMA regions; the
 * test backs it with a flat buffer).
 */
#ifndef VTPV_VIRTQ_H
#define VTPV_VIRTQ_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Split-virtqueue structures (little-endian on the wire; this host is LE). */
#pragma pack(push, 1)
typedef struct vtpv_vq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} vtpv_vq_desc;
typedef struct vtpv_vq_used_elem {
    uint32_t id;
    uint32_t len;
} vtpv_vq_used_elem;
#pragma pack(pop)

#define VTPV_VQ_DESC_F_NEXT     1u
#define VTPV_VQ_DESC_F_WRITE    2u
#define VTPV_VQ_DESC_F_INDIRECT 4u

#define VTPV_VQ_MAX_SIZE        256u /* Apple's descriptor cap */

/* Resolve a guest-physical range [iova, iova+len) to a host pointer, or NULL
 * if it is not mapped / not that long. */
typedef void *(*vtpv_map_fn)(void *ctx, uint64_t iova, uint64_t len);

/* Per-queue state the device keeps. desc/avail/used are the guest ring
 * addresses the driver programmed; last_avail is the next available index to
 * consume; used_idx is the device's used-ring producer index. */
typedef struct vtpv_virtq {
    uint16_t size;        /* <= VTPV_VQ_MAX_SIZE, a power of two */
    uint8_t  ready;       /* queue_enable */
    uint64_t desc;
    uint64_t avail;
    uint64_t used;
    uint16_t last_avail;
    uint16_t used_idx;
} vtpv_virtq;

/* Is there an unconsumed available entry? 1 yes, 0 no, <0 on a bad ring. */
int vtpv_virtq_has_avail(const vtpv_virtq *vq, vtpv_map_fn map, void *ctx);

/*
 * Pop the next available DESCRIPTOR CHAIN and gather its DEVICE-READABLE bytes
 * (the guest->host message) contiguously into buf[cap]. On success returns 1
 * and sets *len (gathered bytes) and *head (the chain head id, for retiring);
 * 0 if none available; <0 on a malformed chain (over the size/byte bound, a
 * cycle, or an unmapped buffer). Advances last_avail on success.
 */
int vtpv_virtq_get_readable(vtpv_virtq *vq, vtpv_map_fn map, void *ctx,
                            uint8_t *buf, size_t cap, size_t *len, uint16_t *head);

/*
 * Pop the next available DESCRIPTOR CHAIN and scatter src[src_len] into its
 * DEVICE-WRITABLE bytes (the host->guest completion). On success returns 1 and
 * sets *head and *written (bytes placed, <= src_len and <= the chain's
 * writable capacity); 0 if none available; <0 on a malformed chain. Advances
 * last_avail on success.
 */
int vtpv_virtq_put_writable(vtpv_virtq *vq, vtpv_map_fn map, void *ctx,
                            const uint8_t *src, size_t src_len,
                            uint16_t *head, uint32_t *written);

/*
 * Queue-0 ASYMMETRIC pop (Apple's model, H7): a single chain carries BOTH the
 * guest->host message (device-readable) AND a device-writable tail for the i32
 * status the host returns. Gathers the readable bytes into buf[cap] (*rlen), and
 * exposes the chain's FIRST writable segment as a host pointer *wptr with *wcap
 * bytes (the 4-byte status fits in one descriptor; *wcap may be 0 if the guest
 * posted no writable tail). Sets *head for retiring. Returns 1, 0 (none), or <0
 * on a malformed chain. Advances last_avail on success.
 */
int vtpv_virtq_pop_inout(vtpv_virtq *vq, vtpv_map_fn map, void *ctx,
                         uint8_t *buf, size_t cap, size_t *rlen,
                         uint8_t **wptr, uint32_t *wcap, uint16_t *head);

/*
 * Retire a consumed chain on the USED ring: publish {head, written} and bump
 * the used index. Returns 0 or <0 on an unmapped used ring.
 */
int vtpv_virtq_retire(vtpv_virtq *vq, vtpv_map_fn map, void *ctx,
                      uint16_t head, uint32_t written);

#ifdef __cplusplus
}
#endif

#endif /* VTPV_VIRTQ_H */
