/*
 * test_aw_replay_lavc.c - host-only end to end of the AppleGVA decode path with
 * the libavcodec stream core: Apple-wire messages in (dsta ImageDescription with
 * avcC/hvcC, async dfrm with ,sbf), the session engine, the vtpv_lavc backend on
 * the VA-API render node, landing into lent buffers, Demt out. The guest side is
 * simulated: it answers every Dbuf with a lent buffer (subs mapping id + ,uui)
 * and every Demt with a reply, and, as the guest does, it sends the next dfrm
 * once the previous one is replied to (an async reply comes right after the
 * Dbuf reply), so it arrives while the previous frame waits for its Demt reply
 * and the engine parks it (D-GVA-SYNC-DFRM: a dfrm sent while a reply is still
 * owed is a frame the guest gave up on).
 *
 *  ENG-REPLAY-01  live-captured guest bytes (2026-11-17 stride run: the guest's
 *                 real 23-byte avcC + its avcC-framed I_PCM sample + its luma
 *                 reference) -> luma exact. Fixture dir = argv[2] (skipped if
 *                 absent).
 *  ENG-MULTI-01   H.264 High IPB 640x360, 30 units -> one Demt per dfrm, in
 *                 decode order, each landed frame == the software oracle.
 *  ENG-MULTI-02   HEVC Main IPB 1280x720, 16 units, same checks.
 *
 * Plan U1, apple-virgl analysis/20261121-APPLEGVA-RESUME-PLAN.md. Exit 77 = no
 * render node.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/md5.h>

#include "aw_session.h"
#include "aw_sbuf.h"
#include "aw_wire.h"
#include "vtpv_lavc.h"
#include "test_lavc_util.h"

static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* ---- emission capture ------------------------------------------------- */
#define MAXE 64
static uint8_t g_e[MAXE][2048];
static size_t  g_el[MAXE];
static int     g_ne;
static void cap_emit(void *ctx, const uint8_t *m, size_t n)
{
    (void)ctx;
    if (g_ne < MAXE && n <= sizeof g_e[0]) { memcpy(g_e[g_ne], m, n); g_el[g_ne] = n; }
    g_ne++;
}

/* ---- simulated guest buffers ------------------------------------------ */
#define NBUF 4
static vtpv_lavc *g_lavc;
static uint8_t   *g_buf[NBUF];
static size_t     g_stride;
static uint32_t   g_w, g_h;

static int sim_resolve(void *c, const uint8_t uuid[16], uint64_t mapid,
                       uint32_t w, uint32_t h, aw_out_buffer *o)
{
    (void)c;
    if (mapid < 1 || mapid > NBUF || w != g_w || h != g_h) return -1;
    memset(o, 0, sizeof *o);
    memcpy(o->uuid, uuid, 16);
    o->mapping_id = mapid;
    o->y = g_buf[mapid - 1];
    o->y_stride = g_stride;
    o->uv = g_buf[mapid - 1] + g_stride * h;
    o->uv_stride = g_stride;
    o->width = w; o->height = h;
    return 0;
}

static int sim_copy(void *c, const vtpv_decoded_frame *f, const aw_out_buffer *b)
{
    int rc;
    (void)c;
    rc = vtpv_lavc_read_luma(g_lavc, f->surface_id, b->y, b->y_stride, f->width, f->height);
    if (rc == 0) rc = vtpv_lavc_read_chroma(g_lavc, f->surface_id, b->uv, b->uv_stride, f->width, f->height);
    return rc;
}

static void sim_put(void *c, const aw_out_buffer *b) { (void)c; (void)b; }

static size_t msg0(uint8_t *buf, uint32_t type, const uint8_t *uuid)
{
    aw_builder b; aw_builder_init(&b, buf, 256);
    aw_build_header(&b, type, AW_FLAG_GUEST_REQ, uuid, 7);
    return aw_build_finalize(&b);
}

static size_t build_dsta(uint8_t *m, size_t cap, const uint8_t *uuid, uint32_t codec,
                         uint16_t w, uint16_t h, const uint8_t *rec, size_t rec_len)
{
    static uint8_t fds[8192];
    size_t fl = aw_imgdesc_build(fds, sizeof fds, codec, w, h,
                                 codec == AW_4CC('h','v','c','1') ? AW_4CC('h','v','c','C') : AW_4CC('a','v','c','C'),
                                 rec, rec_len);
    aw_builder b; aw_builder_init(&b, m, cap);
    aw_build_header(&b, AW_MSG_DSTA, AW_FLAG_GUEST_REQ, uuid, 7);
    aw_put_bytes(&b, AW_KEY_FDS, AW_VT_FDS, fds, fl);
    return aw_build_finalize(&b);
}

static size_t build_dfrm(uint8_t *m, size_t cap, const uint8_t *uuid, int64_t frm,
                         uint64_t rid, const uint8_t *au, size_t aul)
{
    static uint8_t sbf[4u << 20];
    size_t sl = aw_sbuf_write(sbf, sizeof sbf, NULL, au, aul);
    aw_builder b; aw_builder_init(&b, m, cap);
    aw_build_header(&b, AW_MSG_DFRM, AW_FLAG_GUEST_REQ, uuid, rid);
    aw_put_s64(&b, AW_KEY_FRM, frm);
    aw_put_bytes(&b, AW_KEY_SBF, AW_VT_SBF, sbf, sl);
    aw_put_u32(&b, AW_KEY_DFL, 1);
    return aw_build_finalize(&b);
}

static size_t build_dbuf_reply(uint8_t *m, const uint8_t *uuid, uint64_t mapid)
{
    uint64_t ids[AW_SUBS_IDS] = { mapid, 0, 0, 0, 0 };
    size_t sl = aw_subs_write(m, 256, ids);
    uint8_t bu[16]; memset(bu, 0xB0, 16); bu[0] = (uint8_t)mapid;
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

/* what the simulated guest saw */
typedef struct seen {
    int      n_demt, n_dfrm_reply, n_dropped, n_err;
    int64_t  frm[MAXF];
    uint8_t  md5[MAXF][16];
    uint8_t  luma_first[352 * 288];
} seen;

static uint64_t g_next_map = 1;

static void landed_md5(uint64_t mapid, uint8_t out[16], uint8_t *luma_copy, size_t luma_cap)
{
    struct AVMD5 *m = av_md5_alloc();
    const uint8_t *p = g_buf[mapid - 1];
    uint32_t r;
    av_md5_init(m);
    for (r = 0; r < g_h; r++) av_md5_update(m, p + g_stride * r, g_w);
    for (r = 0; r < (g_h + 1) / 2; r++) av_md5_update(m, p + g_stride * g_h + g_stride * r, (g_w + 1) / 2 * 2);
    av_md5_final(m, out);
    av_free(m);
    if (luma_copy && luma_cap >= (size_t)g_w * g_h)
        for (r = 0; r < g_h; r++) memcpy(luma_copy + (size_t)g_w * r, p + g_stride * r, g_w);
}

/* the next unit the simulated guest sends once a dfrm reply comes (-1: none) */
static int g_send_next = -1, g_n_units;
static const uint8_t *const *g_units; static const size_t *g_unit_len;
static size_t build_dfrm(uint8_t *m, size_t cap, const uint8_t *uuid, int64_t frm,
                         uint64_t rid, const uint8_t *au, size_t aul);

/* Service the engine's emissions like the guest: Dbuf -> lend a buffer,
 * Demt -> record + reply, a dfrm reply -> send the next dfrm (if any). Replies
 * may produce new emissions; loop. */
static void service(aw_engine *e, const uint8_t *uuid, seen *s, uint64_t *last_map)
{
    static uint8_t q[MAXE][2048];
    static size_t ql[MAXE];
    uint8_t m[512];
    for (;;) {
        int n = g_ne < MAXE ? g_ne : MAXE, i;
        if (n == 0) return;
        for (i = 0; i < n; i++) { memcpy(q[i], g_e[i], g_el[i]); ql[i] = g_el[i]; }
        g_ne = 0;
        for (i = 0; i < n; i++) {
            aw_msg_view v;
            if (aw_get_be32(q[i] + 4) == AW_ATOM_RELS) continue;
            if (aw_parse(q[i], ql[i], &v) != AW_PARSE_OK) continue;
            if (v.type == AW_MSG_DBUF && v.flags == AW_FLAG_HOST_REQ) {
                uint64_t mapid = g_next_map;
                g_next_map = g_next_map % NBUF + 1;
                *last_map = mapid;
                aw_engine_deliver(e, m, build_dbuf_reply(m, uuid, mapid));
            } else if (v.type == AW_MSG_DFRM && v.flags == AW_FLAG_HOST_REPLY) {
                s->n_dfrm_reply++;
                if (g_send_next >= 0 && g_send_next < g_n_units) {
                    static uint8_t dm[5u << 20];   /* a unit may be large */
                    int k = g_send_next++;
                    aw_engine_deliver(e, dm, build_dfrm(dm, sizeof dm, uuid, 1000 + k, 200 + (uint64_t)k,
                                                        g_units[k], g_unit_len[k]));
                }
            } else if (v.type == AW_MSG_DEMT && v.flags == AW_FLAG_HOST_REQ) {
                int64_t frm = -1; int32_t err = 0; uint32_t inf = 0; uint8_t u[16];
                aw_get_s64(&v, AW_KEY_FRM, &frm);
                aw_get_s32(&v, AW_KEY_ERR, &err);
                aw_get_u32(&v, AW_KEY_INF, &inf);
                if (err) s->n_err++;
                if (inf & 0x2u) s->n_dropped++;
                if (s->n_demt < MAXF) {
                    s->frm[s->n_demt] = frm;
                    if (aw_get_uui(&v, AW_KEY_UUI, u) == AW_PARSE_OK && !err)
                        landed_md5(u[0], s->md5[s->n_demt], s->n_demt == 0 ? s->luma_first : NULL,
                                   sizeof s->luma_first);
                }
                s->n_demt++;
                aw_engine_deliver(e, m, build_demt_reply(m, uuid));
            }
        }
    }
}

static aw_engine *mk_engine(void)
{
    aw_session_cfg cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.be = vtpv_lavc_backend(); cfg.be_ctx = g_lavc;
    cfg.emit = cap_emit;
    cfg.resolve_buffer = sim_resolve; cfg.copy_frame = sim_copy; cfg.put_buffer = sim_put;
    return aw_engine_create(&cfg);
}

static void set_geometry(uint32_t w, uint32_t h)
{
    int i;
    g_w = w; g_h = h;
    g_stride = ((size_t)w + 63) & ~(size_t)63;
    for (i = 0; i < NBUF; i++) {
        free(g_buf[i]);
        g_buf[i] = calloc(1, g_stride * h * 2);
    }
    g_next_map = 1;
}

static void run_stream(const char *name, const stream *st, uint32_t codec4cc)
{
    static oracle o;
    static seen s;
    static uint8_t m[5u << 20];
    uint8_t uuid[16]; memset(uuid, 0x77, 16);
    uint64_t last = 0;
    int i, ok = 0;
    aw_engine *e;

    memset(&s, 0, sizeof s);
    CHECK(run_oracle(st, &o) == 0 && o.n_out == st->n, "%s: oracle %d frames", name, o.n_out);
    set_geometry((uint32_t)st->w, (uint32_t)st->h);
    e = mk_engine();
    g_ne = 0;
    aw_engine_deliver(e, m, msg0(m, AW_MSG_PDEC, uuid));
    g_ne = 0;
    CHECK(aw_engine_deliver(e, m, build_dsta(m, sizeof m, uuid, codec4cc, (uint16_t)st->w, (uint16_t)st->h,
                                             st->extradata, st->extradata_len)) == AW_NOERR, "%s: dsta", name);
    {
        aw_msg_view v; int32_t err = 1;
        CHECK(g_ne == 2 && aw_parse(g_e[1], g_el[1], &v) == AW_PARSE_OK && v.type == AW_MSG_DSTA &&
              aw_get_s32(&v, AW_KEY_ERR, &err) == AW_PARSE_OK && err == 0, "%s: dsta accepted (Dpba + reply)", name);
    }
    g_ne = 0;
    /* the first unit; each next one goes once the previous dfrm is replied to, while that frame still
     * waits for its Demt reply (parked, then started) */
    g_units = (const uint8_t *const *)st->pkt; g_unit_len = st->pkt_len; g_n_units = st->n; g_send_next = 1;
    CHECK(aw_engine_deliver(e, m, build_dfrm(m, sizeof m, uuid, 1000, 200, st->pkt[0], st->pkt_len[0])) == AW_NOERR,
          "%s: dfrm 0", name);
    service(e, uuid, &s, &last);
    g_send_next = -1;
    CHECK(s.n_demt == st->n, "%s: %d Demt for %d dfrm", name, s.n_demt, st->n);
    CHECK(s.n_dfrm_reply == st->n, "%s: %d dfrm replies", name, s.n_dfrm_reply);
    CHECK(s.n_err == 0 && s.n_dropped == 0, "%s: %d errors, %d dropped", name, s.n_err, s.n_dropped);
    for (i = 0; i < s.n_demt && i < st->n; i++)
        if (s.frm[i] == 1000 + i && o.have[i] && !memcmp(s.md5[i], o.md5[i], 16)) ok++;
    CHECK(ok == st->n, "%s: %d/%d landed frames == oracle, decode order", name, ok, st->n);
    printf("  %s: %d dfrm -> %d Demt, %d/%d landed frames == software oracle (decode order, %dx%d)\n",
           name, st->n, s.n_demt, ok, st->n, st->w, st->h);
    aw_engine_destroy(e);
}

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    long l;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); l = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc((size_t)l + 1);
    if (b && fread(b, 1, (size_t)l, f) != (size_t)l) { free(b); b = NULL; }
    fclose(f);
    *n = (size_t)l;
    return b;
}

int main(int argc, char **argv)
{
    const char *node = argc > 1 ? argv[1] : "/dev/dri/renderD128";
    const char *fx = argc > 2 ? argv[2] : NULL;
    char err[256] = "";
    stream st;

    av_log_set_level(AV_LOG_QUIET);
    if (vtpv_lavc_open(VTPV_LAVC_HW_VAAPI, node, &g_lavc, err, sizeof err) != VTPV_OK) {
        printf("SKIP: %s\n", err);
        return 77;
    }

    /* ENG-REPLAY-01: the guest's own bytes */
    if (fx) {
        char p[512];
        size_t al = 0, sl = 0, ll = 0;
        uint8_t *avcc, *sample, *luma;
        snprintf(p, sizeof p, "%s/avcc.bin", fx);   avcc = slurp(p, &al);
        snprintf(p, sizeof p, "%s/sample.bin", fx); sample = slurp(p, &sl);
        snprintf(p, sizeof p, "%s/luma_ref.bin", fx); luma = slurp(p, &ll);
        if (avcc && sample && luma && ll == 352 * 288) {
            static seen s;
            static uint8_t m[1u << 20];
            uint8_t uuid[16]; memset(uuid, 0x66, 16);
            uint64_t last = 0;
            aw_engine *e;
            size_t diffs = 0, k;
            memset(&s, 0, sizeof s);
            set_geometry(352, 288);
            e = mk_engine();
            aw_engine_deliver(e, m, msg0(m, AW_MSG_PDEC, uuid));
            aw_engine_deliver(e, m, build_dsta(m, sizeof m, uuid, AW_4CC('a','v','c','1'), 352, 288, avcc, al));
            g_ne = 0;
            aw_engine_deliver(e, m, build_dfrm(m, sizeof m, uuid, 0, 300, sample, sl));
            service(e, uuid, &s, &last);
            CHECK(s.n_demt == 1 && s.n_err == 0, "replay: one clean Demt (got %d, err %d)", s.n_demt, s.n_err);
            for (k = 0; k < ll; k++) diffs += s.luma_first[k] != luma[k];
            CHECK(diffs == 0, "replay: luma %zu/%zu bytes differ", diffs, ll);
            printf("  replay (live guest bytes, I_PCM 352x288): luma diffs %zu/%zu\n", diffs, ll);
            aw_engine_destroy(e);
        } else printf("  replay: SKIP (fixtures missing under %s)\n", fx);
        free(avcc); free(sample); free(luma);
    }

    /* ENG-MULTI-01 */
    if (make_stream("libx264", "bframes=2:ref=3:keyint=12:min-keyint=12:scenecut=0", AV_PIX_FMT_YUV420P,
                    640, 360, 30, "eng264", &st) == 0) {
        run_stream("engine-h264-ipb-640x360", &st, AW_4CC('a','v','c','1'));
        stream_free(&st);
    }
    /* ENG-MULTI-02 */
    if (make_stream("libx265", "bframes=3:keyint=8:scenecut=0:log-level=error", AV_PIX_FMT_YUV420P,
                    1280, 720, 16, "eng265", &st) == 0) {
        run_stream("engine-hevc-ipb-1280x720", &st, AW_4CC('h','v','c','1'));
        stream_free(&st);
    }

    for (int i = 0; i < NBUF; i++) free(g_buf[i]);
    vtpv_lavc_close(g_lavc);
    printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
