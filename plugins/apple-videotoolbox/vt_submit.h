/*
 * vt_submit.h - the queue-0 (guest->host) drain of the apple-videotoolbox device, with the
 * D-GVA-PIPELINE P1 decode-ahead. It is extracted from apple-videotoolbox.c
 * (vt_process_submission) so it runs in a unit test against a synthetic ring and the real
 * session engine. The device calls it from the queue-0 notify, under its mutex.
 *
 * Apple's queue 0 is asymmetric (contract B section 1): each chain is one message plus a
 * 4-byte i32 writable status tail. The drain gathers the message, delivers it to the
 * session engine (replies and requests go out on queue 1), writes the status and retires
 * the chain. CLEAN-ROOM.
 */
#ifndef VT_SUBMIT_H
#define VT_SUBMIT_H

#include <stddef.h>
#include <stdint.h>

#include "vtpv_virtq.h"
#include "aw_session.h"

typedef struct vt_submit_io {
    vtpv_virtq *sq;                 /* queue 0 */
    vtpv_map_fn map;                /* guest-physical -> host pointer */
    void       *map_ctx;
    aw_engine  *engine;             /* may be NULL (no decode-ahead) */
    uint8_t    *buf;                /* message gather buffer */
    size_t      buf_cap;
    void       *ctx;                /* passed to the callbacks below */
    void (*raise)(void *ctx);       /* queue-0 used-ring interrupt */
    void (*flush)(void *ctx);       /* deliver queued queue-1 messages */
    /* optional debug hooks: before delivery, and after it with the status */
    void (*rx)(void *ctx, const uint8_t *msg, size_t len);
    void (*served)(void *ctx, const uint8_t *msg, size_t len, int32_t status);
} vt_submit_io;

/* Drain queue 0 (at most `budget` chains in total), then decode ahead and repeat while
 * new chains arrive, for at most `max_rounds` further rounds. Returns 0, or -1 when a
 * malformed chain was seen (the device then needs a reset). */
int vt_submit_process(const vt_submit_io *io, unsigned budget, unsigned max_rounds);

#endif /* VT_SUBMIT_H */
