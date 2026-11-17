/*
 * vtpv_virtq.c - virtio 1.x split-virtqueue mechanics (see vtpv_virtq.h).
 * CLEAN-ROOM from the VIRTIO 1.x spec. Increment 2, lane L-M2V03, 2026-11-16.
 * This host is little-endian, as is the virtio wire, so no byte swapping.
 */
#include "vtpv_virtq.h"

#include <string.h>

/* Guest-memory field access through the map callback (per-field map keeps a
 * ring that spans DMA regions correct). */
static int rd(vtpv_map_fn map, void *ctx, uint64_t addr, void *out, size_t n)
{
    void *p = map(ctx, addr, n);
    if (!p) return -1;
    memcpy(out, p, n);
    return 0;
}
static int wr(vtpv_map_fn map, void *ctx, uint64_t addr, const void *in, size_t n)
{
    void *p = map(ctx, addr, n);
    if (!p) return -1;
    memcpy(p, in, n);
    return 0;
}

/* avail ring: le16 flags; le16 idx; le16 ring[size]; le16 used_event. */
static int avail_idx(const vtpv_virtq *vq, vtpv_map_fn map, void *ctx, uint16_t *idx)
{
    return rd(map, ctx, vq->avail + 2, idx, 2);
}
static int avail_ring(const vtpv_virtq *vq, vtpv_map_fn map, void *ctx,
                      uint16_t slot, uint16_t *head)
{
    return rd(map, ctx, vq->avail + 4 + (uint64_t)2 * (slot % vq->size), head, 2);
}

static int ready(const vtpv_virtq *vq)
{
    return vq && vq->ready && vq->size && vq->size <= VTPV_VQ_MAX_SIZE &&
           (vq->size & (vq->size - 1)) == 0 && vq->desc && vq->avail && vq->used;
}

int vtpv_virtq_has_avail(const vtpv_virtq *vq, vtpv_map_fn map, void *ctx)
{
    uint16_t idx;
    if (!ready(vq)) return -1;
    if (avail_idx(vq, map, ctx, &idx)) return -1;
    return idx != vq->last_avail;
}

/* Pop the next available chain head (does NOT advance last_avail). Returns 1,
 * 0 (none), or <0. */
static int peek_head(const vtpv_virtq *vq, vtpv_map_fn map, void *ctx, uint16_t *head)
{
    uint16_t idx;
    if (avail_idx(vq, map, ctx, &idx)) return -1;
    if (idx == vq->last_avail) return 0;
    if (avail_ring(vq, map, ctx, vq->last_avail, head)) return -1;
    return (*head < vq->size) ? 1 : -1;
}

int vtpv_virtq_get_readable(vtpv_virtq *vq, vtpv_map_fn map, void *ctx,
                            uint8_t *buf, size_t cap, size_t *len, uint16_t *head)
{
    uint16_t i, chain = 0, h;
    size_t got = 0;
    int rc;
    if (!ready(vq) || !buf || !len || !head) return -1;
    rc = peek_head(vq, map, ctx, &h);
    if (rc <= 0) return rc;

    i = h;
    for (;;) {
        vtpv_vq_desc d;
        if (rd(map, ctx, vq->desc + (uint64_t)16 * i, &d, sizeof(d))) return -1;
        if (d.flags & VTPV_VQ_DESC_F_INDIRECT) return -1; /* guest uses direct */
        if (!(d.flags & VTPV_VQ_DESC_F_WRITE)) {
            void *p;
            if ((size_t)d.len > cap - got) return -1; /* over the byte bound */
            p = map(ctx, d.addr, d.len);
            if (!p) return -1;
            memcpy(buf + got, p, d.len);
            got += d.len;
        }
        if (!(d.flags & VTPV_VQ_DESC_F_NEXT)) break;
        i = d.next;
        if (i >= vq->size || ++chain >= vq->size) return -1; /* bad / cyclic */
    }
    *len = got;
    *head = h;
    vq->last_avail++;
    return 1;
}

int vtpv_virtq_put_writable(vtpv_virtq *vq, vtpv_map_fn map, void *ctx,
                            const uint8_t *src, size_t src_len,
                            uint16_t *head, uint32_t *written)
{
    uint16_t i, chain = 0, h;
    size_t done = 0;
    int rc;
    if (!ready(vq) || (!src && src_len) || !head || !written) return -1;
    rc = peek_head(vq, map, ctx, &h);
    if (rc <= 0) return rc;

    i = h;
    for (;;) {
        vtpv_vq_desc d;
        if (rd(map, ctx, vq->desc + (uint64_t)16 * i, &d, sizeof(d))) return -1;
        if (d.flags & VTPV_VQ_DESC_F_INDIRECT) return -1;
        if (d.flags & VTPV_VQ_DESC_F_WRITE) {
            size_t n = src_len - done;
            if (n > d.len) n = d.len;
            if (n) {
                void *p = map(ctx, d.addr, n);
                if (!p) return -1;
                memcpy(p, src + done, n);
                done += n;
            }
        }
        if (!(d.flags & VTPV_VQ_DESC_F_NEXT)) break;
        i = d.next;
        if (i >= vq->size || ++chain >= vq->size) return -1;
    }
    *head = h;
    *written = (uint32_t)done;
    vq->last_avail++;
    return 1;
}

int vtpv_virtq_pop_inout(vtpv_virtq *vq, vtpv_map_fn map, void *ctx,
                         uint8_t *buf, size_t cap, size_t *rlen,
                         uint8_t **wptr, uint32_t *wcap, uint16_t *head)
{
    uint16_t i, chain = 0, h;
    size_t got = 0;
    int rc;
    if (!ready(vq) || !buf || !rlen || !wptr || !wcap || !head) return -1;
    rc = peek_head(vq, map, ctx, &h);
    if (rc <= 0) return rc;

    *wptr = NULL;
    *wcap = 0;
    i = h;
    for (;;) {
        vtpv_vq_desc d;
        if (rd(map, ctx, vq->desc + (uint64_t)16 * i, &d, sizeof(d))) return -1;
        if (d.flags & VTPV_VQ_DESC_F_INDIRECT) return -1;
        if (d.flags & VTPV_VQ_DESC_F_WRITE) {
            if (!*wptr && d.len) {           /* first writable segment = status tail */
                void *p = map(ctx, d.addr, d.len);
                if (!p) return -1;
                *wptr = p;
                *wcap = d.len;
            }
        } else {
            void *p;
            if ((size_t)d.len > cap - got) return -1;
            p = map(ctx, d.addr, d.len);
            if (!p) return -1;
            memcpy(buf + got, p, d.len);
            got += d.len;
        }
        if (!(d.flags & VTPV_VQ_DESC_F_NEXT)) break;
        i = d.next;
        if (i >= vq->size || ++chain >= vq->size) return -1;
    }
    *rlen = got;
    *head = h;
    vq->last_avail++;
    return 1;
}

/* used ring: le16 flags; le16 idx; vtpv_vq_used_elem ring[size]; le16 avail_event. */
int vtpv_virtq_retire(vtpv_virtq *vq, vtpv_map_fn map, void *ctx,
                      uint16_t head, uint32_t written)
{
    vtpv_vq_used_elem elem;
    uint16_t slot = vq->used_idx % vq->size;
    uint16_t next;
    if (!ready(vq)) return -1;
    elem.id = head;
    elem.len = written;
    if (wr(map, ctx, vq->used + 4 + (uint64_t)8 * slot, &elem, sizeof(elem))) return -1;
    /* publish the element before the index (a store fence on a real device;
     * the LE memcpy order here is the device-visible order for the test). */
    next = (uint16_t)(vq->used_idx + 1);
    if (wr(map, ctx, vq->used + 2, &next, 2)) return -1;
    vq->used_idx = next;
    return 0;
}
