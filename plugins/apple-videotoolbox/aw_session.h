/*
 * aw_session.h - the host Apple-wire VideoToolbox session engine (contract-B).
 *
 * Takes queue-0 guest->host messages (Apple `vtpv` bytes), routes them by the
 * 16-byte guest UUID, runs the codec-discovery + decoder state machine, drives a
 * vtpv_decoder_backend (the existing VA-API H.264 backend, reused verbatim), and
 * emits the host->guest queue-1 messages (Dpba / Dbuf / Demt / Dfub / rels) and
 * replies. This is H5 (session engine) + H6 (decode exchange) of the FIX-CANDIDATE
 * plan, built on aw_wire (the shared contract-B codec) and aw_sbuf.
 *
 * Async-only decode (contract-B / FIX-CANDIDATE §4.4): the dfrm reply goes early,
 * before Demt; ,inf carries kVTDecodeInfo_Asynchronous (0x1).
 *
 * The GUEST round-trip seam. Apple's host does not allocate output buffers: it
 * sends Dbuf and the guest replies with the pixel buffer (UUID + IOSurface). That
 * reply is genuinely asynchronous and belongs to the device/kext layer (H7 + the
 * guest kext). This engine abstracts it behind get_buffer/copy_frame/put_buffer
 * callbacks so the protocol + decode are complete and unit-testable in-host now;
 * the Dbuf/Demt/rels WIRE messages are still emitted for the real device. H7 backs
 * these callbacks with the real queue-1 Dbuf request + the guest's reply.
 *
 * CLEAN-ROOM. Lane L-VIDEO-IMPL, 2026-11-16.
 */
#ifndef AW_SESSION_H
#define AW_SESSION_H

#include <stddef.h>
#include <stdint.h>

#include "aw_wire.h"
#include "vtpv_proto.h"   /* vtpv_decoder_backend, vtpv_decoded_frame (reused)   */

#ifdef __cplusplus
extern "C" {
#endif

/* One output buffer the guest lends in reply to Dbuf. The pixel memory + layout
 * are the integrator's (the subs-mapped IOSurface); the engine never allocates
 * it. For host tests it is plain host memory. */
typedef struct aw_out_buffer {
    uint8_t   uuid[16];    /* the pixel buffer's UUID (the guest's ,uui)         */
    uint64_t  mapping_id;  /* the subs IOSurface mapping id (named by rels)      */
    uint8_t  *y;           /* luma plane destination                            */
    size_t    y_stride;
    uint8_t  *uv;          /* NV12 chroma plane destination (may be NULL)        */
    size_t    uv_stride;
    uint32_t  width, height;
    void     *landing;     /* integrator-private (e.g. a scatter shadow); engine copies/ignores it */
} aw_out_buffer;

/* queue-1 emit sink: one complete host->guest message (a vtpv message or a bare
 * rels box). Must copy/queue and return. */
typedef void (*aw_emit_fn)(void *ctx, const uint8_t *msg, size_t len);

/* EVENT-DRIVEN async (H7). The engine never blocks for the guest: on dfrm it
 * emits Dbuf and returns; the guest's Dbuf reply (flag 0x8, with the lent
 * surface's mapping id in a subs prefix and the buffer UUID in ,uui) is
 * delivered back through aw_engine_deliver, and THAT is when the engine resolves
 * the buffer, decodes, copies and emits Demt. This matches the single-threaded
 * device: the queue-0 notify that carries the reply cannot block on itself.
 *
 * resolve_buffer turns a guest-lent buffer (its ,uui + the subs mapping id) into
 * host-addressable planes. In the device this maps the guest IOSurface's pages;
 * in host tests it returns plain host memory. Returns 0 and fills *out, or <0 to
 * fail the frame. */
typedef int (*aw_resolve_buffer_fn)(void *ctx, const uint8_t uuid[16],
                                    uint64_t mapping_id, uint32_t w, uint32_t h,
                                    aw_out_buffer *out);

/* Copy a decoded backend frame into the lent buffer (the host writing NV12 into
 * the guest's IOSurface between Dbuf's reply and Demt). Returns 0 or <0. */
typedef int (*aw_copy_frame_fn)(void *ctx, const vtpv_decoded_frame *f,
                                const aw_out_buffer *b);

/* Release a buffer the host is done with (models the rels sweep). */
typedef void (*aw_put_buffer_fn)(void *ctx, const aw_out_buffer *b);

typedef struct aw_session_cfg {
    const vtpv_decoder_backend *be;
    void                       *be_ctx;
    aw_emit_fn           emit;
    void                *emit_ctx;
    aw_resolve_buffer_fn resolve_buffer;
    aw_copy_frame_fn     copy_frame;
    aw_put_buffer_fn     put_buffer;
    void                *buf_ctx;
} aw_session_cfg;

typedef struct aw_engine aw_engine;

aw_engine *aw_engine_create(const aw_session_cfg *cfg);
void       aw_engine_destroy(aw_engine *e);

/* Deliver one queue-0 guest->host message. Returns the i32 device status the
 * device writes to the chain's writable tail (0 noErr, or an AW_ERR_* code).
 * Replies and host->guest requests go out through cfg.emit on queue 1. */
int32_t aw_engine_deliver(aw_engine *e, const uint8_t *msg, size_t len);

unsigned aw_engine_open_sessions(const aw_engine *e);

/* D-GVA-PIPELINE P1 (decode-ahead): decode each in-flight unit whose Dbuf has been
 * emitted and whose Dbuf reply has not arrived yet, keeping the result for that
 * reply. Emits nothing (the wire order is unchanged). The device calls it after the
 * drain that emitted the Dbuf has delivered it to the guest, so the decode overlaps
 * the guest's lend. Returns the number of units decoded. A session closed (mdec /
 * dinv / engine destroy) while a picture is held drops it with its decoder. */
unsigned aw_engine_run_deferred(aw_engine *e);

/* ---- QuickTime ImageDescription (=fds) helpers (contract-B §5 / WIRE §5.2) --
 * 86-byte big-endian header, then extension atoms (avcC/hvcC), then a zero u32. */

/* Build an ImageDescription for `codec` (e.g. AW_4CC('a','v','c','1')) at
 * width x height carrying one extension atom `ext_fourcc` with `ext`/`ext_len`
 * (the avcC/hvcC record). Returns the total length, or 0 on overflow. */
size_t aw_imgdesc_build(uint8_t *buf, size_t cap, uint32_t codec,
                        uint16_t width, uint16_t height,
                        uint32_t ext_fourcc, const uint8_t *ext, size_t ext_len);

/* Parse an ImageDescription. On success returns 0 and sets codec/w/h and, if
 * an avcC or hvcC extension is present, the ext/ext_fourcc/ext_len outputs (pointer into
 * `fds`). Returns <0 when `fds` is too short / not a valid ImageDescription. */
int aw_imgdesc_parse(const uint8_t *fds, size_t len, uint32_t *codec,
                     uint16_t *w, uint16_t *h,
                     uint32_t *ext_fourcc, const uint8_t **ext, size_t *ext_len);

#ifdef __cplusplus
}
#endif

#endif /* AW_SESSION_H */
