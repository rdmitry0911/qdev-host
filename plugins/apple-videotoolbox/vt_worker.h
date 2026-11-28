/*
 * vt_worker.h - the decode worker of the apple-videotoolbox device (VT-DECODE-WORKER,
 * D-AGVA-TIMEOUT-UNDER-LOAD; apple-virgl analysis/20261128-FIX-CANDIDATE-VT-DECODE-WORKER.md).
 *
 * ONE thread per device runs the session engine's jobs (aw_session.h, worker mode) in submission order:
 * a job's decode / landing (aw_job_work) runs without the device lock, its completion (aw_job_complete +
 * the device's post hook, e.g. the queue-1 flush and interrupt) under it. So a queue-0 status never waits
 * for a decode or a landing (contract B section 1: the status is the delivery result).
 *
 * Locks, in the order a thread may take them: the guest-memory lock `mem` (read: one job; write: the
 * host's quiesce .. resume window), then the device lock. The backend lock (vt_locked_backend) is taken
 * inside either, never while waiting for another. CLEAN-ROOM.
 */
#ifndef VT_WORKER_H
#define VT_WORKER_H

#include <pthread.h>
#include <stddef.h>

#include "aw_session.h"
#include "vtpv_proto.h"

typedef struct vt_worker {
    pthread_t        th;
    int              running;
    pthread_mutex_t  qm;            /* the job FIFO                                  */
    pthread_cond_t   qcv;
    aw_job         **q;
    size_t           q_cap, q_head, q_n;
    int              stop;
    aw_engine       *e;
    pthread_mutex_t *dev_m;         /* the device lock (engine state, emission)     */
    pthread_cond_t   done;          /* broadcast after each completion (dev_m)      */
    void            *mem;           /* pthread_rwlock_t *: guest memory, read per job, write = quiesced
                                       (void * so includers need no _GNU_SOURCE / XOPEN feature macros) */
    void           (*post)(void *ctx); /* after a completion, under dev_m            */
    void            *post_ctx;
} vt_worker;

/* Prepare w (no thread yet): the engine is created with cfg.submit_job = vt_worker_submit,
 * cfg.wait_jobs = vt_worker_wait, cfg.job_ctx = w, then vt_worker_start(w, engine). */
int  vt_worker_init(vt_worker *w, pthread_mutex_t *dev_m, void *mem /* pthread_rwlock_t * or NULL */,
                    void (*post)(void *ctx), void *post_ctx);
int  vt_worker_start(vt_worker *w, aw_engine *e);
/* aw_submit_job_fn / aw_wait_jobs_fn (device lock held) */
int  vt_worker_submit(void *w, aw_job *job);
void vt_worker_wait(void *w);
/* Discard every queued job (device reset; device lock held, guest memory quiesced). */
void vt_worker_discard(vt_worker *w);
/* Finish the queued jobs, stop and join (teardown; device lock NOT held). */
void vt_worker_stop(vt_worker *w);
void vt_worker_destroy(vt_worker *w);

/* A backend whose every call holds one mutex (the stream core and the legacy backends are
 * single-entry: the worker's decode / landing and the device thread's open / close / query
 * never overlap). */
typedef struct vt_locked_backend {
    vtpv_decoder_backend        be;     /* the wrapper table (pass &lb->be as cfg.be)    */
    const vtpv_decoder_backend *inner;
} vt_locked_backend;

void vt_locked_backend_init(vt_locked_backend *lb, const vtpv_decoder_backend *inner);
/* The lock itself, for other backend reads (the landing's frame readers). */
void vt_backend_lock(void);
void vt_backend_unlock(void);

#endif /* VT_WORKER_H */
