/*
 * vt_submit.c - queue-0 drain + P1 decode-ahead of the apple-videotoolbox device.
 * Extracted from apple-videotoolbox.c vt_process_submission (qdev-host c4b16d2); since
 * D-AGVA-TIMEOUT-UNDER-LOAD the chains that arrive during a deferred unit are retired after it.
 * See vt_submit.h. CLEAN-ROOM.
 */
#include "vt_submit.h"

#include <string.h>

/* Drain every available chain (within the budget): gather, deliver, write the status, retire.
 * Returns the number retired. The budget does not wrap at 0 (the pre-extraction `budget--`
 * test did, which left later rounds unbounded once 2 x VT_QUEUE_MAX chains were drained). */
static unsigned drain(const vt_submit_io *io, unsigned *budget, int *bad)
{
    vtpv_virtq *sq = io->sq;
    unsigned n = 0;
    while (*budget && vtpv_virtq_has_avail(sq, io->map, io->map_ctx) == 1) {
        (*budget)--;
        size_t len = 0; uint16_t head = 0;
        uint8_t *wptr = NULL; uint32_t wcap = 0;
        int rc = vtpv_virtq_pop_inout(sq, io->map, io->map_ctx, io->buf, io->buf_cap, &len,
                                      &wptr, &wcap, &head);
        if (rc != 1) { *bad = 1; break; } /* malformed ring */
        if (io->rx) io->rx(io->ctx, io->buf, len);
        int32_t status = aw_engine_deliver(io->engine, io->buf, len); /* emits on q1 */
        if (io->served) io->served(io->ctx, io->buf, len, status);
        uint32_t written = 0;
        if (wptr && wcap >= 4) { memcpy(wptr, &status, 4); written = 4; } /* native i32 */
        vtpv_virtq_retire(sq, io->map, io->map_ctx, head, written);
        n++;
    }
    return n;
}

typedef struct unit_hook {
    const vt_submit_io *io;
    unsigned *budget;
    int *bad;
    unsigned served;    /* chains retired by the hook in this round */
} unit_hook;

/* After each deferred unit: retire the chains the guest posted while it decoded, so their
 * status does not wait for the remaining units (D-AGVA-TIMEOUT-UNDER-LOAD; contract B: the
 * queue-0 status is the delivery result, decode results go out on queue 1). */
static void after_unit(void *ctx)
{
    unit_hook *h = ctx;
    unsigned n = drain(h->io, h->budget, h->bad);
    if (n) {
        h->served += n;
        h->io->raise(h->io->ctx);
        h->io->flush(h->io->ctx);
    }
}

int vt_submit_process(const vt_submit_io *io, unsigned budget, unsigned max_rounds)
{
    vtpv_virtq *sq = io->sq;
    unsigned rounds = 0;
    int bad = 0;
again:
    (void)drain(io, &budget, &bad);
    io->raise(io->ctx);    /* the queue-0 chains are now free on the used ring */
    io->flush(io->ctx);    /* deliver any replies/requests this drain queued on q1 */
    /* D-GVA-PIPELINE P1: the Dbufs of this drain are delivered and the dfrm chains
     * retired; decode those units now, while the guest lends their buffers. Chains
     * that arrive meanwhile are retired after each unit (after_unit); the guest's Dbuf
     * replies among them can make more units decodable, so another round runs when
     * the hook served any chain or more are available. */
    if (io->engine) {
        unit_hook h = { io, &budget, &bad, 0 };
        if (aw_engine_run_deferred_hooked(io->engine, after_unit, &h) && rounds++ < max_rounds &&
            (h.served || vtpv_virtq_has_avail(sq, io->map, io->map_ctx) == 1))
            goto again;
    }
    return bad ? -1 : 0;
}
