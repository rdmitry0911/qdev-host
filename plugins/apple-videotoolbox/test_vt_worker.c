#define _GNU_SOURCE
/*
 * test_vt_worker.c - VT-DECODE-WORKER (D-AGVA-TIMEOUT-UNDER-LOAD; apple-virgl
 * analysis/20261128-FIX-CANDIDATE-VT-DECODE-WORKER.md): the queue-0 drain (vt_submit) on a synthetic split ring,
 * with the REAL session engine (aw_session), the REAL decode worker (vt_worker) and its single-entry backend lock,
 * and boundary doubles for the decoder backend (slow decode, unit ids recorded), the landing (copy_frame, with a
 * latch that holds it), the lent-buffer provider, the queue-1 sink and the device lock (a mutex standing for d->m).
 * No libva, no GPU, no guest.
 *
 * Contract VT-Q0-STATUS (contract B section 1, AppleVideoToolboxParavirtualization/analysis/CONTRACT-B-APPLE-VT-WIRE-
 * 20261116.md): a queue-0 chain is one message + a 4-byte status, the status is the delivery result; decode results
 * travel on queue 1. Engine source: a Dbuf reply's status is AW_NOERR on every branch. So no queue-0 status may wait
 * for a decode or a landing (the guest gives up after 5 s: `sendMessageToHost driver rc=0xe00002d6`).
 * Each scenario runs in two configurations: SYNC = the engine without the worker (the behaviour before the fix,
 * `worker=0`), WORKER = the device default after the fix. W-01/W-03/W-06/W-12 hold the contract; W-02/W-08/W-08b/
 * W-10/W-11 require the WORKER per-session queue-1 byte stream and decode order to equal SYNC's (messages that
 * arrive while a session's frame lands are delivered after it, in order); W-04/W-05/W-07/W-09/W-13 the lock,
 * reset, teardown and fallback rules (review findings R1-R5 of e22be84); W-14..W-17 the hold limits, the held
 * statuses and the teardown mute (review findings R6-R9 of fc547dc).
 * CLEAN-ROOM.
 */
#include "vt_submit.h"
#include "vt_worker.h"
#include "aw_session.h"
#include "aw_sbuf.h"
#include "aw_wire.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } \
                           else { printf("  ok: " __VA_ARGS__); printf("\n"); } } while (0)

/* ---- the device lock, guest memory lock, worker ---------------------------------------------- */
static pthread_mutex_t g_dm = PTHREAD_MUTEX_INITIALIZER;   /* stands for d->m */
static pthread_rwlock_t g_mem_lk = PTHREAD_RWLOCK_INITIALIZER;
static vt_worker g_w;
static int g_worker;                                       /* configuration of the current run */
static aw_engine *g_e;

/* ---- synthetic guest memory + queue 0 (32 = 16 two-descriptor chains) ------------------------ */
static uint8_t g_mem[1u << 17];
static void *map_mem(void *ctx, uint64_t iova, uint64_t len)
{ (void)ctx; return (iova + len > sizeof g_mem) ? NULL : g_mem + iova; }
#define QSZ   32
#define DESC  0x1000
#define AVAIL 0x1200
#define USED  0x1400
#define MSG   0x2000   /* chain i's message at MSG + i * 0x400 (16 chains max per run) */
#define STAT  0x8000
static void put16(uint64_t at, uint16_t v) { memcpy(g_mem + at, &v, 2); }
static uint16_t get16(uint64_t at) { uint16_t v; memcpy(&v, g_mem + at, 2); return v; }
static unsigned g_chains;
static vtpv_virtq g_q0;
/* the guest posts one chain (the device lock is held: the drain may run on another thread) */
static unsigned post_chain(const uint8_t *m, size_t n)
{
    pthread_mutex_lock(&g_dm);
    unsigned c = g_chains++;
    memcpy(g_mem + MSG + 0x400u * c, m, n);
    int32_t poison = 0x5a5a5a5a; memcpy(g_mem + STAT + 4u * c, &poison, 4);
    vtpv_vq_desc r = { MSG + 0x400u * c, (uint32_t)n, VTPV_VQ_DESC_F_NEXT, (uint16_t)(2 * c + 1) };
    vtpv_vq_desc w = { STAT + 4u * c, 4, VTPV_VQ_DESC_F_WRITE, 0 };
    memcpy(g_mem + DESC + 16u * (2 * c), &r, 16);
    memcpy(g_mem + DESC + 16u * (2 * c + 1), &w, 16);
    uint16_t idx = get16(AVAIL + 2);
    put16(AVAIL + 4 + 2u * (idx % QSZ), (uint16_t)(2 * c));
    put16(AVAIL + 2, (uint16_t)(idx + 1));
    pthread_mutex_unlock(&g_dm);
    return c;
}
static uint16_t used_idx_locked(void) { return get16(USED + 2); }
static uint16_t used_idx(void) { pthread_mutex_lock(&g_dm); uint16_t v = get16(USED + 2); pthread_mutex_unlock(&g_dm); return v; }
static int32_t stat_of(unsigned c)
{ pthread_mutex_lock(&g_dm); int32_t v; memcpy(&v, g_mem + STAT + 4u * c, 4); pthread_mutex_unlock(&g_dm); return v; }

/* ---- Apple-wire messages (as in test_aw_session.c) ------------------------------------------- */
static size_t msg0(uint8_t *buf, uint32_t type, uint32_t flags, const uint8_t *uuid)
{ aw_builder b; aw_builder_init(&b, buf, 256); aw_build_header(&b, type, flags, uuid, 7); return aw_build_finalize(&b); }
static size_t build_dsta(uint8_t *m, const uint8_t *uuid)
{
    uint8_t fds[160];
    static const uint8_t avcc[7] = { 0x01,0x42,0x00,0x1e,0xff,0xe1,0x00 };
    size_t fl = aw_imgdesc_build(fds, sizeof fds, AW_4CC('a','v','c','1'), 352, 288, AW_4CC('a','v','c','C'), avcc, sizeof avcc);
    aw_builder b; aw_builder_init(&b, m, 512);
    aw_build_header(&b, AW_MSG_DSTA, AW_FLAG_GUEST_REQ, uuid, 7);
    aw_put_bytes(&b, AW_KEY_FDS, AW_VT_FDS, fds, fl);
    return aw_build_finalize(&b);
}
/* unit id in au[5]; dfl 1 = async (early dfrm reply), 0 = sync */
static size_t build_dfrm(uint8_t *m, const uint8_t *uuid, int64_t frm, uint64_t rid, uint8_t unit, int async)
{
    uint8_t au[8] = { 0,0,0,4, 0x65, unit, 2, 3 };
    uint8_t sbf[256];
    size_t sl = aw_sbuf_write(sbf, sizeof sbf, NULL, au, sizeof au);
    aw_builder b; aw_builder_init(&b, m, 512);
    aw_build_header(&b, AW_MSG_DFRM, AW_FLAG_GUEST_REQ, uuid, rid);
    aw_put_s64(&b, AW_KEY_FRM, frm);
    aw_put_bytes(&b, AW_KEY_SBF, AW_VT_SBF, sbf, sl);
    aw_put_u32(&b, AW_KEY_DFL, async ? 1u : 0u);
    return aw_build_finalize(&b);
}
static size_t build_dbuf_reply(uint8_t *m, const uint8_t *uuid, uint64_t mapid)
{
    uint64_t ids[AW_SUBS_IDS] = { mapid, 0, 0, 0, 0 };
    size_t sl = aw_subs_write(m, 256, ids);
    uint8_t bu[16]; memset(bu, 0xB0, 16);
    aw_builder b; aw_builder_init(&b, m + sl, 256 - sl);
    aw_build_header(&b, AW_MSG_DBUF, AW_FLAG_GUEST_REPLY, uuid, 100);
    aw_put_s32(&b, AW_KEY_ERR, 0);
    aw_put_uui(&b, AW_KEY_UUI, bu);
    return sl + aw_build_finalize(&b);
}
static size_t build_demt_reply(uint8_t *m, const uint8_t *uuid)
{
    aw_builder b; aw_builder_init(&b, m, 256);
    aw_build_header(&b, AW_MSG_DEMT, AW_FLAG_GUEST_REPLY, uuid, 101);
    aw_put_s32(&b, AW_KEY_ERR, 0);
    return aw_build_finalize(&b);
}

/* ---- boundary doubles ------------------------------------------------------------------------- */
static int g_in_be, g_overlap;                 /* backend single entry (atomic)               */
static void be_enter(void) { if (__atomic_add_fetch(&g_in_be, 1, __ATOMIC_SEQ_CST) > 1) __atomic_add_fetch(&g_overlap, 1, __ATOMIC_SEQ_CST); }
static void be_leave(void) { __atomic_sub_fetch(&g_in_be, 1, __ATOMIC_SEQ_CST); }
static int g_decodes, g_decode_us, g_opens, g_closes, g_drop_at = -1, g_fail_at = -1;
static uint8_t g_order[64]; static int g_norder;  /* unit ids in decode order */
static int be_query(void *c, vtpv_caps *o, size_t cap)
{
    (void)c;
    vtpv_codec_cap *cc = (vtpv_codec_cap *)((uint8_t *)o + sizeof(*o));
    if (cap < sizeof(*o) + sizeof(*cc)) return -1;
    o->magic = VTPV_CAPS_MAGIC; o->abi = VTPV_ABI; o->n_codecs = 1;
    cc[0].codec = VTPV_CODEC_H264; cc[0].can_decode = 1; cc[0].can_encode = 0; cc[0].reserved = 0;
    cc[0].max_width = 4096; cc[0].max_height = 4096; cc[0].out_pixfmt = VTPV_PIXFMT_NV12;
    return 1;
}
static int be_open(void *c, uint32_t codec, uint32_t w, uint32_t h, const uint8_t *ex, size_t exl, void **dec)
{
    (void)c; (void)codec; (void)w; (void)h; (void)ex; (void)exl;
    be_enter(); if (g_decode_us) usleep(g_decode_us / 4); __atomic_add_fetch(&g_opens, 1, __ATOMIC_SEQ_CST); be_leave();
    *dec = (void *)0xD0D0; return 0;
}
static int be_decode(void *dec, const uint8_t *s, size_t sl, vtpv_decoded_frame *o)
{
    (void)dec;
    be_enter();
    if (g_decode_us) usleep(g_decode_us);
    int k = __atomic_fetch_add(&g_decodes, 1, __ATOMIC_SEQ_CST);
    if (g_norder < 64) g_order[g_norder++] = sl > 5 ? s[5] : 0xff;
    be_leave();
    if (k == g_drop_at) return VTPV_DROPPED;
    if (k == g_fail_at) return VTPV_ERR_BACKEND;
    o->surface_id = 42; o->width = 352; o->height = 288; o->pixfmt = VTPV_PIXFMT_NV12;
    return 0;
}
static void be_close(void *dec) { (void)dec; be_enter(); __atomic_add_fetch(&g_closes, 1, __ATOMIC_SEQ_CST); be_leave(); }
static const vtpv_decoder_backend SLOW = { be_query, be_open, be_decode, be_close, NULL, NULL };
static vt_locked_backend g_lbe;

/* the landing: holds while the latch is closed (3 s ceiling), records the used idx at its end */
static pthread_mutex_t g_lm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_lcv = PTHREAD_COND_INITIALIZER;
static int g_latch_closed, g_landing, g_landings, g_puts, g_resolve_fail;
static uint16_t g_used_at_land_end;
static int cpy(void *c, const vtpv_decoded_frame *f, const aw_out_buffer *b)
{
    (void)c; (void)f; (void)b;
    pthread_mutex_lock(&g_lm);
    g_landing = 1;
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 3;
    while (g_latch_closed) if (pthread_cond_timedwait(&g_lcv, &g_lm, &ts) == ETIMEDOUT) break;
    g_landing = 0; g_landings++;
    pthread_mutex_unlock(&g_lm);
    /* SYNC: inside the drain, which holds the device lock */
    if (!g_worker) g_used_at_land_end = used_idx_locked();
    return 0;
}
/* WORKER: the device's vt_copy_frame_locked shape (backend lock only); the used idx is read after the
 * backend lock is released, as production never takes the device lock inside a landing */
static int cpy_locked(void *c, const vtpv_decoded_frame *f, const aw_out_buffer *b)
{
    vt_backend_lock(); be_enter(); be_leave(); int r = cpy(c, f, b); vt_backend_unlock();
    if (g_worker) g_used_at_land_end = used_idx();   /* (a synchronous fallback lands inside the drain) */
    return r;
}
static int landing_now(void) { pthread_mutex_lock(&g_lm); int v = g_landing; pthread_mutex_unlock(&g_lm); return v; }
static void latch(int closed) { pthread_mutex_lock(&g_lm); g_latch_closed = closed; pthread_cond_broadcast(&g_lcv); pthread_mutex_unlock(&g_lm); }
static int res(void *c, const uint8_t u[16], uint64_t id, uint32_t w, uint32_t h, aw_out_buffer *o)
{ (void)c; (void)u; memset(o, 0, sizeof *o); memset(o->uuid, 0xB0, 16); o->mapping_id = id; o->width = w; o->height = h; return g_resolve_fail ? -1 : 0; }
static void put(void *c, const aw_out_buffer *b) { (void)c; (void)b; g_puts++; }

/* queue-1 sink: the byte stream (emit runs under the device lock) */
static uint8_t g_wire[96][512]; static size_t g_wirel[96]; static int g_nwire;
static void emit(void *c, const uint8_t *m, size_t n)
{ (void)c; if (g_nwire < 96 && n <= 512) { memcpy(g_wire[g_nwire], m, n); g_wirel[g_nwire] = n; } g_nwire++; }
static int g_posts;
static void post(void *c) { (void)c; g_posts++; }
static void raise_q0(void *c) { (void)c; }
static void flush_q1(void *c) { (void)c; }

static void reset_all(void)
{
    memset(g_mem, 0, sizeof g_mem); g_chains = 0;
    g_decodes = g_opens = g_closes = g_norder = g_landings = g_puts = g_posts = g_nwire = 0;
    g_decode_us = 0; g_drop_at = g_fail_at = -1; g_resolve_fail = 0; g_overlap = 0;
    latch(0);
    memset(&g_q0, 0, sizeof g_q0);
    g_q0.size = QSZ; g_q0.ready = 1; g_q0.desc = DESC; g_q0.avail = AVAIL; g_q0.used = USED;
}
static void start(int worker)
{
    reset_all();
    g_worker = worker;
    aw_session_cfg cfg; memset(&cfg, 0, sizeof cfg);
    cfg.emit = emit; cfg.resolve_buffer = res; cfg.put_buffer = put;
    if (worker) {
        vt_worker_init(&g_w, &g_dm, &g_mem_lk, post, NULL);
        vt_locked_backend_init(&g_lbe, &SLOW);
        cfg.be = &g_lbe.be; cfg.copy_frame = cpy_locked;
        cfg.submit_job = vt_worker_submit; cfg.wait_jobs = vt_worker_wait; cfg.job_ctx = &g_w;
    } else {
        cfg.be = &SLOW; cfg.copy_frame = cpy;
    }
    g_e = aw_engine_create(&cfg);
    if (worker) vt_worker_start(&g_w, g_e);
}
static void finish(void)
{
    if (g_worker) vt_worker_stop(&g_w);
    aw_engine_destroy(g_e);
    if (g_worker) vt_worker_destroy(&g_w);
    g_e = NULL;
}
/* the queue-0 notify: the device thread drains under its lock */
static void drain(void)
{
    static uint8_t buf[1u << 16];
    const vt_submit_io io = { .sq = &g_q0, .map = map_mem, .map_ctx = NULL, .engine = g_e,
                              .buf = buf, .buf_cap = sizeof buf, .ctx = NULL,
                              .raise = raise_q0, .flush = flush_q1, .rx = NULL, .served = NULL };
    pthread_mutex_lock(&g_dm);
    (void)vt_submit_process(&io, 2 * 256, 4);
    pthread_mutex_unlock(&g_dm);
}
/* the guest waits for the host's answers before its next message (causality of the replies) */
static void idle(void)
{
    if (!g_worker) return;
    pthread_mutex_lock(&g_dm);
    while (aw_engine_jobs(g_e)) vt_worker_wait(&g_w);
    pthread_mutex_unlock(&g_dm);
}
static int wait_landing(int ms) { for (int i = 0; i < ms; i++) { if (landing_now()) return 1; usleep(1000); } return 0; }
static int count_type(uint32_t t)
{
    int n = 0;
    for (int i = 0; i < g_nwire && i < 96; i++) { aw_msg_view v; if (aw_parse(g_wire[i], g_wirel[i], &v) == AW_PARSE_OK && v.type == t) n++; }
    return n;
}

static const uint8_t UA[16] = { 0xA1,0xA1,0xA1,0xA1,0xA1,0xA1,0xA1,0xA1,0xA1,0xA1,0xA1,0xA1,0xA1,0xA1,0xA1,0xA1 };
static const uint8_t UB[16] = { 0xB2,0xB2,0xB2,0xB2,0xB2,0xB2,0xB2,0xB2,0xB2,0xB2,0xB2,0xB2,0xB2,0xB2,0xB2,0xB2 };
static const uint8_t UC[16] = { 0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3 };
static uint8_t g_m[512];
#define POST(expr) post_chain(g_m, (expr))
static void open_session(const uint8_t *u, uint8_t unit)
{
    POST(msg0(g_m, AW_MSG_PDEC, AW_FLAG_GUEST_REQ, u));
    POST(build_dsta(g_m, u));
    POST(build_dfrm(g_m, u, 40, 801, unit, 1));
    drain(); idle();
}

/* W-01: a chain behind a Dbuf reply is retired while that frame still lands */
static int w01(int worker)
{
    start(worker);
    open_session(UA, 1);                                   /* chains 0-2, unit 1 decoded ahead */
    latch(1);
    POST(build_dbuf_reply(g_m, UA, 0xA100));               /* 3: the lend: decode + landing   */
    POST(msg0(g_m, AW_MSG_PDEC, AW_FLAG_GUEST_REQ, UC));   /* 4: an unrelated request          */
    drain();
    if (worker) (void)wait_landing(1000);
    int during = landing_now() && used_idx() == 5 && stat_of(4) == AW_NOERR;
    latch(0); idle();
    int ok = g_used_at_land_end == 5 && count_type(AW_MSG_DEMT) == 1;
    printf("  [%s] retired during the landing: %d; used idx at the landing's end %u (want 5)\n",
           worker ? "WORKER" : "SYNC", during, (unsigned)g_used_at_land_end);
    finish();
    return worker ? (during && ok) : ok;
}

/* the AW-P1 scenario through the ring (parked units, a dropped, a failed, an unusable and an absent lend) */
static void p1_scenario(void)
{
    POST(msg0(g_m, AW_MSG_PDEC, AW_FLAG_GUEST_REQ, UA)); drain(); idle();
    POST(build_dsta(g_m, UA)); drain(); idle();
    g_drop_at = 1; g_fail_at = 2;
    POST(build_dfrm(g_m, UA, 40, 801, 40, 1)); drain(); idle();
    POST(build_dbuf_reply(g_m, UA, 0xA100)); drain(); idle();            /* 40 ok            */
    POST(build_dfrm(g_m, UA, 41, 802, 41, 1)); drain(); idle();
    POST(build_demt_reply(g_m, UA)); drain(); idle();
    POST(build_dbuf_reply(g_m, UA, 0xA200)); drain(); idle();            /* 41 dropped       */
    POST(build_dfrm(g_m, UA, 42, 803, 42, 1)); drain(); idle();
    POST(build_dbuf_reply(g_m, UA, 0xA300)); drain(); idle();            /* 42 fails         */
    POST(build_dfrm(g_m, UA, 43, 804, 43, 1)); drain(); idle();
    g_resolve_fail = 1;
    POST(build_dbuf_reply(g_m, UA, 0xA400)); drain(); idle();            /* 43 lend unusable */
    g_resolve_fail = 0;
    POST(build_dfrm(g_m, UA, 44, 805, 44, 1)); drain(); idle();
    POST(build_dbuf_reply(g_m, UA, 0)); drain(); idle();                 /* 44 lent nothing  */
}
/* W-08: the guest gives up on a sync frame twice (D-GVA-SYNC-DFRM) */
static void giveup_scenario(void)
{
    POST(msg0(g_m, AW_MSG_PDEC, AW_FLAG_GUEST_REQ, UA)); drain(); idle();
    POST(build_dsta(g_m, UA)); drain(); idle();
    POST(build_dfrm(g_m, UA, 40, 801, 40, 0)); drain(); idle();          /* sync 40, Dbuf out  */
    POST(build_dfrm(g_m, UA, 41, 802, 41, 0)); drain(); idle();          /* gives 40 up        */
    POST(build_dbuf_reply(g_m, UA, 0xA200)); drain(); idle();            /* 41 lands           */
    POST(build_demt_reply(g_m, UA)); drain(); idle();
}
/* W-08b: the guest gives up twice while a sync frame lands (review R5) */
static void giveup2_scenario(void)
{
    POST(msg0(g_m, AW_MSG_PDEC, AW_FLAG_GUEST_REQ, UA)); POST(build_dsta(g_m, UA));
    POST(build_dfrm(g_m, UA, 40, 801, 40, 0)); drain(); idle();
    POST(build_dbuf_reply(g_m, UA, 0xA100));
    POST(build_dfrm(g_m, UA, 41, 802, 41, 0));
    POST(build_dfrm(g_m, UA, 42, 803, 42, 0));
    drain(); idle();
}
/* W-10 / W-11: a session message right behind an async frame's lend, in the same drain (review R2 / R3) */
static void behind_lend_scenario(uint32_t type)
{
    open_session(UA, 1);
    POST(build_dbuf_reply(g_m, UA, 0xA100));
    POST(msg0(g_m, type, AW_FLAG_GUEST_REQ, UA));
    drain(); idle();
}
/* W-13: the LAND job cannot be queued (review R4) */
static int g_w13_calls, g_w13_fail_at = -1;
static int w13_submit(void *w, aw_job *j)
{ if (g_w13_calls++ == g_w13_fail_at) return -1; return vt_worker_submit(w, j); }
static int g_wd_done;   /* atomic */
static void *deadlock_watchdog(void *arg)
{
    for (int i = 0; i < 300; i++) { if (__atomic_load_n(&g_wd_done, __ATOMIC_ACQUIRE)) return NULL; usleep(10000); }
    printf("  FAIL: DEADLOCK: %s did not return within 3 s\n", (const char *)arg);
    fflush(stdout);
    _exit(3);
}

/* direct delivery under the device lock (a message too large for the synthetic ring) */
static int32_t w14_deliver(const uint8_t *m, size_t n)
{ pthread_mutex_lock(&g_dm); int32_t st = aw_engine_deliver(g_e, m, n); pthread_mutex_unlock(&g_dm); return st; }
static size_t w14_dfrm_big(uint8_t *m, size_t cap, const uint8_t *uuid, int64_t frm, uint64_t rid, size_t aul)
{
    static uint8_t au[(1u << 20) + 16], sbf[(1u << 20) + 256];
    memset(au, 0, sizeof au); au[3] = 4; au[4] = 0x65; au[5] = 7;
    size_t sl = aw_sbuf_write(sbf, sizeof sbf, NULL, au, aul);
    aw_builder b; aw_builder_init(&b, m, cap);
    aw_build_header(&b, AW_MSG_DFRM, AW_FLAG_GUEST_REQ, uuid, rid);
    aw_put_s64(&b, AW_KEY_FRM, frm);
    aw_put_bytes(&b, AW_KEY_SBF, AW_VT_SBF, sbf, sl);
    aw_put_u32(&b, AW_KEY_DFL, 1);
    return aw_build_finalize(&b);
}
/* W-14: many async dfrms while one frame lands; record the decode order of the first 64 */
static uint8_t w14_ord[2][64]; static int w14_nord[2], w14_refused[2];
static void w14_run(int w)
{
    start(w);
    open_session(UA, 1);
    if (w) latch(1);
    uint8_t m[512];
    (void)w14_deliver(m, build_dbuf_reply(m, UA, 0xA100));
    if (w) (void)wait_landing(1000);
    w14_refused[w] = 0;
    for (int i = 0; i < 300; i++)
        if (w14_deliver(m, build_dfrm(m, UA, 41 + i, 900 + i, (uint8_t)(2 + (i % 200)), 1)) == AW_ERR_MALFUNCTION) w14_refused[w]++;
    if (w) latch(0);
    idle();
    w14_nord[w] = g_norder < 64 ? g_norder : 64; memcpy(w14_ord[w], g_order, (size_t)w14_nord[w]);
    finish();
}
static void *w17_stop(void *a) { (void)a; vt_worker_stop(&g_w); return NULL; }

static uint8_t s_wire[96][512]; static size_t s_wirel[96]; static int s_nwire; static uint8_t s_order[64]; static int s_norder, s_decodes;
static void keep(void) { s_nwire = g_nwire; memcpy(s_wire, g_wire, sizeof s_wire); memcpy(s_wirel, g_wirel, sizeof s_wirel);
                         s_norder = g_norder; memcpy(s_order, g_order, sizeof s_order); s_decodes = g_decodes; }
static int same_as_kept(void)
{
    if (g_nwire != s_nwire || g_norder != s_norder || g_decodes != s_decodes) return 0;
    for (int i = 0; i < g_nwire && i < 96; i++) if (g_wirel[i] != s_wirel[i] || memcmp(g_wire[i], s_wire[i], g_wirel[i])) return 0;
    return memcmp(g_order, s_order, (size_t)g_norder) == 0;
}

static void *release_after(void *arg) { usleep((useconds_t)(uintptr_t)arg); latch(0); return NULL; }
static int g_q_acquired_after_demt, g_q_held, g_q_resume;
static pthread_mutex_t g_qm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_qcv = PTHREAD_COND_INITIALIZER;
/* the host main loop: quiesce() .. resume() on ONE thread, as qdev-host calls them */
static void *quiesce_thread(void *arg)
{
    (void)arg;
    pthread_rwlock_wrlock(&g_mem_lk);                    /* quiesce()                       */
    pthread_mutex_lock(&g_dm); g_q_acquired_after_demt = count_type(AW_MSG_DEMT); pthread_mutex_unlock(&g_dm);
    pthread_mutex_lock(&g_qm); g_q_held = 1; pthread_cond_broadcast(&g_qcv);
    while (!g_q_resume) pthread_cond_wait(&g_qcv, &g_qm);
    pthread_mutex_unlock(&g_qm);
    pthread_rwlock_unlock(&g_mem_lk);                    /* resume()                        */
    return NULL;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("-- W-01 (VT-Q0-STATUS): a chain behind a Dbuf reply is retired while that frame lands --\n");
    int s1 = w01(0);
    int w1 = w01(1);
    printf("  (SYNC = before the fix: %s)\n", s1 ? "retired during the landing" : "waits for the landing");
    CHECK(w1, "W-01: WORKER retires chain 4 (status 0) while chain 3's frame still lands; Demt follows the landing");

    printf("-- W-02: the AW-P1 scenario: WORKER byte stream and decode order == SYNC --\n");
    start(0); p1_scenario(); keep(); finish();
    start(1); p1_scenario();
    CHECK(s_nwire > 20 && same_as_kept(), "W-02: identical queue-1 stream (%d / %d messages), decodes %d / %d", s_nwire, g_nwire, s_decodes, g_decodes);
    CHECK(g_decodes == 5, "W-02: every unit decoded exactly once (%d)", g_decodes);
    finish();

    printf("-- W-08: sync-frame give-up: WORKER stream and decode order == SYNC --\n");
    start(0); giveup_scenario(); keep(); finish();
    start(1); giveup_scenario();
    CHECK(same_as_kept(), "W-08: identical stream (%d / %d) and decode order (%d units)", s_nwire, g_nwire, g_norder);
    finish();

    printf("-- W-03: close while the session's frame lands --\n");
    start(1);
    open_session(UA, 1);
    latch(1);
    POST(build_dbuf_reply(g_m, UA, 0xA100)); drain();
    (void)wait_landing(1000);
    POST(msg0(g_m, AW_MSG_MDEC, AW_FLAG_GUEST_REQ, UA));
    pthread_t rt; pthread_create(&rt, NULL, release_after, (void *)(uintptr_t)200000);
    drain();                                             /* the close waits for the landing  */
    pthread_join(rt, NULL);
    idle();
    CHECK(count_type(AW_MSG_DEMT) == 1 && g_closes == 1 && g_puts == 1 && g_landings == 1,
          "W-03: the frame landed and its Demt went out before the close; close once (%d), buffer back once (%d)", g_closes, g_puts);
    CHECK(used_idx() == 5 && stat_of(4) == AW_NOERR, "W-03: the close chain retired with status 0");
    finish();

    printf("-- W-04: quiesce waits for the landing job, and no job runs while quiesced --\n");
    start(1);
    open_session(UA, 1);
    latch(1);
    POST(build_dbuf_reply(g_m, UA, 0xA100)); drain();
    (void)wait_landing(1000);
    pthread_t qt; g_q_held = g_q_resume = 0; pthread_create(&qt, NULL, quiesce_thread, NULL);
    usleep(100000);
    latch(0);
    pthread_mutex_lock(&g_qm); while (!g_q_held) pthread_cond_wait(&g_qcv, &g_qm); pthread_mutex_unlock(&g_qm);
    CHECK(g_q_acquired_after_demt == 1, "W-04: quiesce acquired after the job completed (Demt out)");
    int d0 = g_decodes;
    POST(msg0(g_m, AW_MSG_PDEC, AW_FLAG_GUEST_REQ, UB)); /* B's decode-ahead job queues ...   */
    POST(build_dsta(g_m, UB));
    POST(build_dfrm(g_m, UB, 40, 901, 9, 1));
    drain();                                             /* (no idle(): the worker is held)   */
    usleep(150000);
    int d1 = __atomic_load_n(&g_decodes, __ATOMIC_SEQ_CST);
    pthread_mutex_lock(&g_qm); g_q_resume = 1; pthread_cond_broadcast(&g_qcv); pthread_mutex_unlock(&g_qm);
    pthread_join(qt, NULL);                              /* resume()                          */
    idle();
    CHECK(d1 == d0 && g_decodes == d0 + 1, "W-04: B's job ran only after resume (%d -> %d -> %d)", d0, d1, g_decodes);
    finish();

    printf("-- W-05: teardown with queued jobs: discarded (no host service), decoders closed once --\n");
    start(1);
    g_decode_us = 20000;
    POST(msg0(g_m, AW_MSG_PDEC, AW_FLAG_GUEST_REQ, UA)); POST(build_dsta(g_m, UA));
    POST(msg0(g_m, AW_MSG_PDEC, AW_FLAG_GUEST_REQ, UB)); POST(build_dsta(g_m, UB));
    POST(build_dfrm(g_m, UA, 40, 801, 1, 1)); POST(build_dfrm(g_m, UB, 40, 901, 2, 1));
    drain();
    {   /* the device's vt_teardown: discard under the device lock, stop, destroy */
        int posts0 = g_posts;
        pthread_mutex_lock(&g_dm); vt_worker_discard(&g_w); pthread_mutex_unlock(&g_dm);
        vt_worker_stop(&g_w);
        int posts_after = g_posts - posts0;
        aw_engine_destroy(g_e); vt_worker_destroy(&g_w); g_e = NULL;
        CHECK(g_decodes <= 1 && g_closes == 2 && posts_after == 0,
              "W-05: queued units discarded (%d decoded), both decoders closed once (%d), no post hook after stop (%d)",
              g_decodes, g_closes, posts_after);
    }

    printf("-- W-06: two sessions: B's chains retire while A's frame lands --\n");
    start(1);
    open_session(UA, 1);
    open_session(UB, 2);
    latch(1);
    POST(build_dbuf_reply(g_m, UA, 0xA100)); drain();    /* A lands (held)                   */
    (void)wait_landing(1000);
    POST(build_dbuf_reply(g_m, UB, 0xB100));             /* B's lend                         */
    POST(build_dfrm(g_m, UB, 41, 902, 3, 1));            /* B's next unit (parks)            */
    drain();
    int during = landing_now() && used_idx() == 9 && stat_of(7) == AW_NOERR && stat_of(8) == AW_NOERR;
    latch(0); idle();
    CHECK(during, "W-06: B's lend and next dfrm retired with status 0 while A's frame lands");
    CHECK(count_type(AW_MSG_DEMT) == 2, "W-06: both frames' Demts followed (%d)", count_type(AW_MSG_DEMT));
    finish();

    printf("-- W-07: backend single entry (open on the device thread vs decode on the worker) --\n");
    start(1);
    g_decode_us = 50000;
    POST(msg0(g_m, AW_MSG_PDEC, AW_FLAG_GUEST_REQ, UA)); POST(build_dsta(g_m, UA));
    POST(build_dfrm(g_m, UA, 40, 801, 1, 1)); drain();   /* worker decodes A (50 ms)         */
    POST(msg0(g_m, AW_MSG_PDEC, AW_FLAG_GUEST_REQ, UB)); POST(build_dsta(g_m, UB)); drain(); /* open B */
    idle();
    CHECK(g_overlap == 0 && g_opens == 2, "W-07: no two backend calls overlapped (%d), both opened", g_overlap);
    finish();

    printf("-- W-08b: two sync give-ups while the frame lands: WORKER stream and decode order == SYNC --\n");
    start(0); giveup2_scenario(); keep(); finish();
    start(1); giveup2_scenario();
    CHECK(same_as_kept(), "W-08b: identical stream (%d / %d) and decode order (%d units)", s_nwire, g_nwire, g_norder);
    finish();

    printf("-- W-10 / W-11: dinv / dfin behind a lend: WORKER stream == SYNC (Demt first) --\n");
    start(0); behind_lend_scenario(AW_MSG_DINV); keep(); finish();
    start(1); behind_lend_scenario(AW_MSG_DINV);
    CHECK(same_as_kept() && g_closes == 1, "W-10: dinv: identical stream (%d / %d), decoder closed once", s_nwire, g_nwire);
    finish();
    start(0); behind_lend_scenario(AW_MSG_DFIN); keep(); finish();
    start(1); behind_lend_scenario(AW_MSG_DFIN);
    CHECK(same_as_kept(), "W-11: dfin: identical stream (%d / %d)", s_nwire, g_nwire);
    finish();

    printf("-- W-12: a close does not wait for the worker (another session's landing ahead of it) --\n");
    start(1);
    open_session(UA, 1);
    open_session(UB, 2);
    latch(1);
    POST(build_dbuf_reply(g_m, UA, 0xA100)); drain();    /* A lands (held)                     */
    (void)wait_landing(1000);
    POST(build_dbuf_reply(g_m, UB, 0xB100));             /* 7: B's LAND queues behind A's       */
    POST(msg0(g_m, AW_MSG_MDEC, AW_FLAG_GUEST_REQ, UB)); /* 8: close B                          */
    POST(msg0(g_m, AW_MSG_DINV, AW_FLAG_GUEST_REQ, UA)); /* 9: invalidate A                     */
    drain();                                             /* returns at once                     */
    int at_once = landing_now() && used_idx() == 10 && stat_of(8) == AW_NOERR && stat_of(9) == AW_NOERR && g_closes == 0;
    latch(0); idle();
    int demts = count_type(AW_MSG_DEMT), dinvs = count_type(AW_MSG_DINV);
    CHECK(at_once, "W-12: mdec(B) and dinv(A) retired with status 0 while A's frame lands, nothing closed yet");
    CHECK(demts == 2 && dinvs == 1 && g_closes == 2, "W-12: both frames landed (Demt %d), dinv replied (%d), both closed after (%d)", demts, dinvs, g_closes);
    finish();

    printf("-- W-13: the LAND job cannot be queued: the held picture lands without a second decode (review R4) --\n");
    {
        reset_all(); g_worker = 1;
        aw_session_cfg cfg; memset(&cfg, 0, sizeof cfg);
        cfg.emit = emit; cfg.resolve_buffer = res; cfg.put_buffer = put;
        vt_worker_init(&g_w, &g_dm, &g_mem_lk, post, NULL);
        vt_locked_backend_init(&g_lbe, &SLOW);
        cfg.be = &g_lbe.be; cfg.copy_frame = cpy_locked;
        cfg.submit_job = w13_submit; cfg.wait_jobs = vt_worker_wait; cfg.job_ctx = &g_w;
        g_e = aw_engine_create(&cfg); vt_worker_start(&g_w, g_e);
        g_w13_calls = 0; g_w13_fail_at = -1;
        open_session(UA, 1);                             /* the decode-ahead job: decode 1      */
        g_w13_fail_at = g_w13_calls;                     /* the LAND job's submit fails         */
        g_worker = 0;                                    /* (the fallback lands in the drain)   */
        POST(build_dbuf_reply(g_m, UA, 0xA100)); drain();
        g_worker = 1; idle();
        CHECK(g_decodes == 1 && count_type(AW_MSG_DEMT) == 1, "W-13: one decode for the unit (%d), Demt out", g_decodes);
        finish();
    }

    printf("-- W-14: past the hold limit a message is refused, never delivered ahead of the held ones (review R6) --\n");
    w14_run(0); w14_run(1);
    CHECK(w14_nord[0] == w14_nord[1] && !memcmp(w14_ord[0], w14_ord[1], (size_t)w14_nord[0]),
          "W-14: the first %d decodes are in the same order as SYNC", w14_nord[1]);
    CHECK(w14_refused[0] == 0 && w14_refused[1] == 300 - 256,
          "W-14: SYNC refuses none, WORKER refuses the %d past the 256 held (-12909)", w14_refused[1]);
    {   /* the byte cap: 1 MiB dfrms while a frame lands, 64 MiB held at most per engine */
        start(1);
        open_session(UA, 1);
        latch(1);
        uint8_t m[512];
        (void)w14_deliver(m, build_dbuf_reply(m, UA, 0xA100));
        (void)wait_landing(1000);
        static uint8_t big[(1u << 20) + 1024];
        int ok_n = 0, refused = 0;
        for (int i = 0; i < 70; i++) {
            int32_t st = w14_deliver(big, w14_dfrm_big(big, sizeof big, UA, 41 + i, 900 + i, 1u << 20));
            if (st == AW_NOERR) ok_n++; else if (st == AW_ERR_MALFUNCTION) refused++;
        }
        latch(0); idle();
        CHECK(ok_n == 63 && refused == 7, "W-14: byte cap: %d held, %d refused (64 MiB, ~1 MiB each)", ok_n, refused);
        finish();
    }

    printf("-- W-15 / W-16: statuses of held messages equal SYNC (review R7 / R8) --\n");
    {
        int32_t st[2][2];
        for (int w = 0; w <= 1; w++) {
            start(w);
            open_session(UA, 1);
            POST(build_dbuf_reply(g_m, UA, 0xA100));
            unsigned c = POST(msg0(g_m, AW_MSG_DSTA, AW_FLAG_GUEST_REQ, UA)); /* no ,fds */
            drain(); idle();
            st[0][w] = stat_of(c);
            finish();
            start(w);
            open_session(UA, 1);
            g_resolve_fail = 1;
            POST(build_dbuf_reply(g_m, UA, 0xA100));
            c = POST(build_demt_reply(g_m, UA));                             /* stray    */
            drain(); idle();
            st[1][w] = stat_of(c);
            finish();
        }
        CHECK(st[0][0] == st[0][1] && st[0][1] == AW_ERR_MALFORMED, "W-15: dsta without ,fds: %d == SYNC %d", st[0][1], st[0][0]);
        CHECK(st[1][0] == st[1][1] && st[1][1] == AW_ERR_MALFORMED, "W-16: stray Demt reply behind a failing lend: %d == SYNC %d", st[1][1], st[1][0]);
    }

    printf("-- W-17: teardown with a landing in progress: nothing emitted, no post hook (review R9) --\n");
    {
        start(1);
        open_session(UA, 1);
        latch(1);
        POST(build_dbuf_reply(g_m, UA, 0xA100)); drain();
        (void)wait_landing(1000);
        int before = g_nwire, posts = g_posts;
        pthread_mutex_lock(&g_dm); vt_worker_discard(&g_w); pthread_mutex_unlock(&g_dm);
        pthread_t st; pthread_create(&st, NULL, w17_stop, NULL);
        usleep(50000);
        latch(0);
        pthread_join(st, NULL);
        int emitted = g_nwire - before, posted = g_posts - posts;
        aw_engine_destroy(g_e); vt_worker_destroy(&g_w); g_e = NULL;
        CHECK(emitted == 0 && posted == 0 && g_puts == 1 && g_closes == 1,
              "W-17: %d messages, %d post hooks after teardown began; buffer back (%d), decoder closed (%d)", emitted, posted, g_puts, g_closes);
    }

    printf("-- W-09: device reset with a queued LAND and a parked dfrm (review R1) --\n");
    start(1);
    open_session(UA, 1);
    open_session(UB, 2);
    latch(1);
    POST(build_dbuf_reply(g_m, UB, 0xB100)); drain();    /* B lands (held): the worker is busy  */
    (void)wait_landing(1000);
    POST(build_dbuf_reply(g_m, UA, 0xA100));             /* A's LAND queued behind B            */
    POST(build_dfrm(g_m, UA, 41, 802, 3, 1));            /* A's next unit: held behind the lend */
    drain();
    {
        pthread_t rt; pthread_create(&rt, NULL, release_after, (void *)(uintptr_t)100000);
        pthread_rwlock_wrlock(&g_mem_lk);                /* quiesce(): waits for B's job        */
        pthread_join(rt, NULL);
        int wire0 = g_nwire, puts0 = g_puts, posts0 = g_posts;
        pthread_t wd; __atomic_store_n(&g_wd_done, 0, __ATOMIC_RELEASE); pthread_create(&wd, NULL, deadlock_watchdog, "vt_worker_discard");
        pthread_mutex_lock(&g_dm);                       /* vt_reset: d->m, then discard        */
        vt_worker_discard(&g_w);
        unsigned jobs = aw_engine_jobs(g_e);
        pthread_mutex_unlock(&g_dm);
        __atomic_store_n(&g_wd_done, 1, __ATOMIC_RELEASE); pthread_join(wd, NULL);
        CHECK(1, "W-09: the discard returned (no self-deadlock on the queue lock)");
        CHECK(g_nwire == wire0 && g_posts == posts0 && g_puts == puts0 + 1 && jobs == 0,
              "W-09: nothing emitted or flushed while quiesced, A's lent buffer back (%d), no job left (%u)", g_puts - puts0, jobs);
        pthread_rwlock_unlock(&g_mem_lk);                /* resume()                            */
        idle();
    }
    finish();

    printf("%s: VT-DECODE-WORKER\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
