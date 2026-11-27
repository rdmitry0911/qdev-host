/*
 * vt_submit.c - queue-0 drain + P1 decode-ahead of the apple-videotoolbox device.
 * Extracted unchanged from apple-videotoolbox.c vt_process_submission (qdev-host c4b16d2).
 * See vt_submit.h. CLEAN-ROOM.
 */
#include "vt_submit.h"

#include <string.h>

int vt_submit_process(const vt_submit_io *io, unsigned budget, unsigned max_rounds)
{
    vtpv_virtq *sq = io->sq;
    unsigned rounds = 0;
    int bad = 0;
again:
    while (budget-- && vtpv_virtq_has_avail(sq, io->map, io->map_ctx) == 1) {
        size_t len = 0; uint16_t head = 0;
        uint8_t *wptr = NULL; uint32_t wcap = 0;
        int rc = vtpv_virtq_pop_inout(sq, io->map, io->map_ctx, io->buf, io->buf_cap, &len,
                                      &wptr, &wcap, &head);
        if (rc != 1) { bad = 1; break; } /* malformed ring */
        if (io->rx) io->rx(io->ctx, io->buf, len);
        int32_t status = aw_engine_deliver(io->engine, io->buf, len); /* emits on q1 */
        if (io->served) io->served(io->ctx, io->buf, len, status);
        uint32_t written = 0;
        if (wptr && wcap >= 4) { memcpy(wptr, &status, 4); written = 4; } /* native i32 */
        vtpv_virtq_retire(sq, io->map, io->map_ctx, head, written);
    }
    io->raise(io->ctx);    /* the queue-0 chains are now free on the used ring */
    io->flush(io->ctx);    /* deliver any replies/requests this drain queued on q1 */
    /* D-GVA-PIPELINE P1: the Dbufs of this drain are delivered and the dfrm chains
     * retired; decode those units now, while the guest lends their buffers. The
     * guest's Dbuf replies that arrived meanwhile are then picked up here. */
    if (io->engine && aw_engine_run_deferred(io->engine) && rounds++ < max_rounds &&
        vtpv_virtq_has_avail(sq, io->map, io->map_ctx) == 1)
        goto again;
    return bad ? -1 : 0;
}
