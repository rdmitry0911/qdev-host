#define _GNU_SOURCE
/*
 * vt_worker.c - the decode worker of the apple-videotoolbox device. See vt_worker.h. CLEAN-ROOM.
 */
#include "vt_worker.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static void *worker_main(void *arg)
{
    vt_worker *w = arg;
    for (;;) {
        pthread_mutex_lock(&w->qm);
        while (!w->q_n && !w->stop) pthread_cond_wait(&w->qcv, &w->qm);
        if (!w->q_n && w->stop) { pthread_mutex_unlock(&w->qm); break; }
        pthread_mutex_unlock(&w->qm);

        /* guest memory first: a quiesce (write) holds the job back before it is popped,
         * so a reset in that window can discard it */
        if (w->mem) pthread_rwlock_rdlock((pthread_rwlock_t *)w->mem);
        pthread_mutex_lock(&w->qm);
        aw_job *j = NULL;
        if (w->q_n) { j = w->q[w->q_head]; w->q_head = (w->q_head + 1) % w->q_cap; w->q_n--; }
        pthread_mutex_unlock(&w->qm);
        if (j) {
            int again;
            do {
                aw_job_work(w->e, j);                 /* decode / landing: no device lock */
                pthread_mutex_lock(w->dev_m);
                again = aw_job_complete(w->e, j);     /* state + emission: device lock    */
                if (!again) {
                    if (w->post) w->post(w->post_ctx);
                    pthread_cond_broadcast(&w->done);
                }
                pthread_mutex_unlock(w->dev_m);
            } while (again);
        }
        if (w->mem) pthread_rwlock_unlock((pthread_rwlock_t *)w->mem);
    }
    return NULL;
}

int vt_worker_init(vt_worker *w, pthread_mutex_t *dev_m, void *mem,
                   void (*post)(void *ctx), void *post_ctx)
{
    memset(w, 0, sizeof(*w));
    if (pthread_mutex_init(&w->qm, NULL) != 0) return -1;
    if (pthread_cond_init(&w->qcv, NULL) != 0) { pthread_mutex_destroy(&w->qm); return -1; }
    if (pthread_cond_init(&w->done, NULL) != 0) {
        pthread_cond_destroy(&w->qcv); pthread_mutex_destroy(&w->qm); return -1;
    }
    w->dev_m = dev_m;
    w->mem = mem;
    w->post = post;
    w->post_ctx = post_ctx;
    return 0;
}

int vt_worker_start(vt_worker *w, aw_engine *e)
{
    w->e = e;
    if (pthread_create(&w->th, NULL, worker_main, w) != 0) return -1;
    w->running = 1;
    return 0;
}

int vt_worker_submit(void *arg, aw_job *job)
{
    vt_worker *w = arg;
    if (!w || !w->running) return -1;
    pthread_mutex_lock(&w->qm);
    if (w->q_n == w->q_cap) {
        size_t cap = w->q_cap ? w->q_cap * 2 : 64;
        aw_job **q = malloc(cap * sizeof(*q));
        if (!q) { pthread_mutex_unlock(&w->qm); return -1; }
        for (size_t i = 0; i < w->q_n; i++) q[i] = w->q[(w->q_head + i) % w->q_cap];
        free(w->q);
        w->q = q; w->q_cap = cap; w->q_head = 0;
    }
    w->q[(w->q_head + w->q_n) % w->q_cap] = job;
    w->q_n++;
    pthread_cond_signal(&w->qcv);
    pthread_mutex_unlock(&w->qm);
    return 0;
}

void vt_worker_wait(void *arg)
{
    vt_worker *w = arg;
    pthread_cond_wait(&w->done, w->dev_m);
}

void vt_worker_discard(vt_worker *w)
{
    if (!w || !w->running) return;
    pthread_mutex_lock(&w->qm);
    while (w->q_n) {
        aw_job *j = w->q[w->q_head];
        w->q_head = (w->q_head + 1) % w->q_cap;
        w->q_n--;
        aw_job_discard(w->e, j);
    }
    pthread_mutex_unlock(&w->qm);
    pthread_cond_broadcast(&w->done);
}

void vt_worker_stop(vt_worker *w)
{
    if (!w || !w->running) return;
    pthread_mutex_lock(&w->qm);
    w->stop = 1;
    pthread_cond_signal(&w->qcv);
    pthread_mutex_unlock(&w->qm);
    pthread_join(w->th, NULL);       /* it finishes the queued jobs first */
    w->running = 0;
}

void vt_worker_destroy(vt_worker *w)
{
    if (!w) return;
    vt_worker_stop(w);
    free(w->q);
    w->q = NULL;
    pthread_cond_destroy(&w->done);
    pthread_cond_destroy(&w->qcv);
    pthread_mutex_destroy(&w->qm);
}

/* ---- the single-entry backend (one per process: the device is one per host process) */

static pthread_mutex_t g_be_m = PTHREAD_MUTEX_INITIALIZER;
static const vtpv_decoder_backend *g_inner;

void vt_backend_lock(void)   { pthread_mutex_lock(&g_be_m); }
void vt_backend_unlock(void) { pthread_mutex_unlock(&g_be_m); }

static int lk_query(void *c, vtpv_caps *o, size_t cap)
{ vt_backend_lock(); int r = g_inner->query_caps(c, o, cap); vt_backend_unlock(); return r; }
static int lk_open(void *c, uint32_t codec, uint32_t w, uint32_t h, const uint8_t *ex, size_t exl, void **dec)
{ vt_backend_lock(); int r = g_inner->open_decoder(c, codec, w, h, ex, exl, dec); vt_backend_unlock(); return r; }
static int lk_decode(void *dec, const uint8_t *s, size_t sl, vtpv_decoded_frame *o)
{ vt_backend_lock(); int r = g_inner->decode(dec, s, sl, o); vt_backend_unlock(); return r; }
static void lk_close(void *dec)
{ vt_backend_lock(); g_inner->close_decoder(dec); vt_backend_unlock(); }
static uint32_t lk_outfmt(void *dec)
{ vt_backend_lock(); uint32_t r = g_inner->output_format(dec); vt_backend_unlock(); return r; }
static int lk_coded(void *dec, uint32_t *cw, uint32_t *ch)
{ vt_backend_lock(); int r = g_inner->coded_size(dec, cw, ch); vt_backend_unlock(); return r; }

void vt_locked_backend_init(vt_locked_backend *lb, const vtpv_decoder_backend *inner)
{
    memset(lb, 0, sizeof(*lb));
    lb->inner = inner;
    g_inner = inner;
    if (!inner) return;
    lb->be.query_caps   = inner->query_caps   ? lk_query  : NULL;
    lb->be.open_decoder = inner->open_decoder ? lk_open   : NULL;
    lb->be.decode       = inner->decode       ? lk_decode : NULL;
    lb->be.close_decoder= inner->close_decoder? lk_close  : NULL;
    lb->be.output_format= inner->output_format? lk_outfmt : NULL;
    lb->be.coded_size   = inner->coded_size   ? lk_coded  : NULL;
}
