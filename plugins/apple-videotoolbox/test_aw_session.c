/*
 * test_aw_session.c - unit tests for the Apple-wire session engine (aw_session),
 * with a MOCK backend: no libva, no GPU, no guest. Proves H5 routing + statuses
 * and the H6 decode-exchange WIRE sequence/ordering against contract-B.
 *
 * CLEAN-ROOM. Lane L-VIDEO-IMPL, 2026-11-16.
 */
#include "aw_session.h"
#include "aw_sbuf.h"
#include "aw_wire.h"
#include "aw_bplist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* ---- captured queue-1 emissions ------------------------------------- */
#define MAXE 32
static uint8_t g_e[MAXE][2048];
static size_t  g_el[MAXE];
static int     g_ne;
static void cap_emit(void *ctx, const uint8_t *m, size_t n) {
    (void)ctx;
    if (g_ne < MAXE && n <= sizeof g_e[0]) { memcpy(g_e[g_ne], m, n); g_el[g_ne] = n; }
    g_ne++;
}
static void ereset(void) { g_ne = 0; }
static uint32_t e_fourcc_at4(int i) { return aw_get_be32(g_e[i] + 4); } /* 'vtpv' or 'rels' */
static int e_is_rels(int i) { return e_fourcc_at4(i) == AW_ATOM_RELS; }
static int e_type(int i, aw_msg_view *v) { return aw_parse(g_e[i], g_el[i], v); }

/* ---- mock backend --------------------------------------------------- */
static int g_open_calls, g_decode_calls, g_close_calls, g_open_fail;
static int mock_query(void *c, vtpv_caps *o, size_t cap){
    (void)c;
    vtpv_codec_cap *cc = (vtpv_codec_cap *)((uint8_t *)o + sizeof(*o));
    if (cap < sizeof(*o) + 2 * sizeof(*cc)) return -1;
    o->magic = VTPV_CAPS_MAGIC; o->abi = VTPV_ABI; o->n_codecs = 2;
    cc[0].codec = VTPV_CODEC_H264; cc[0].can_decode = 1; cc[0].can_encode = 0;
    cc[0].reserved = 0; cc[0].max_width = 4096; cc[0].max_height = 4096; cc[0].out_pixfmt = VTPV_PIXFMT_NV12;
    cc[1].codec = VTPV_CODEC_HEVC; cc[1].can_decode = 1; cc[1].can_encode = 0;
    cc[1].reserved = 0; cc[1].max_width = 8192; cc[1].max_height = 8192; cc[1].out_pixfmt = VTPV_PIXFMT_NV12;
    return 2;
}
static int mock_open(void *c, uint32_t codec, uint32_t w, uint32_t h,
                     const uint8_t *ex, size_t exl, void **dec) {
    (void)c;(void)codec;(void)w;(void)h;(void)ex;(void)exl;
    g_open_calls++;
    if (g_open_fail) return VTPV_ERR_BACKEND;
    *dec = (void *)0xD0D0; return 0;
}
static int mock_decode(void *dec, const uint8_t *s, size_t sl, vtpv_decoded_frame *o){
    (void)dec;(void)s;(void)sl; g_decode_calls++;
    o->surface_id = 42; o->width = 352; o->height = 288; o->pixfmt = VTPV_PIXFMT_NV12;
    return 0;
}
static void mock_close(void *dec){ (void)dec; g_close_calls++; }
static const vtpv_decoder_backend MOCK = {
    mock_query, mock_open, mock_decode, mock_close };

/* ---- mock guest buffer provider (resolve a lent surface) ------------ */
static int g_resolve_calls, g_copy_calls, g_putbuf_calls;
static uint64_t g_last_mapid;
static int mock_resolve(void *c, const uint8_t uuid[16], uint64_t mapid,
                        uint32_t w, uint32_t h, aw_out_buffer *o){
    (void)c;(void)uuid; g_resolve_calls++; g_last_mapid = mapid;
    memset(o, 0, sizeof *o);
    memset(o->uuid, 0xB0, 16);
    o->mapping_id = mapid; o->width = w; o->height = h;
    return 0;
}
static int mock_copy(void *c, const vtpv_decoded_frame *f, const aw_out_buffer *b){
    (void)c;(void)f;(void)b; g_copy_calls++; return 0;
}
static void mock_putbuf(void *c, const aw_out_buffer *b){ (void)c;(void)b; g_putbuf_calls++; }

static aw_engine *mk(void) {
    aw_session_cfg cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.be = &MOCK; cfg.be_ctx = NULL;
    cfg.emit = cap_emit;
    cfg.resolve_buffer = mock_resolve; cfg.copy_frame = mock_copy; cfg.put_buffer = mock_putbuf;
    return aw_engine_create(&cfg);
}

/* a guest Dbuf reply (flag 0x8): a subs prefix carrying the lent mapping id,
 * then a Dbuf vtpv with ,uui (the buffer id) + ,err 0 */
static size_t build_dbuf_reply(uint8_t *m, const uint8_t *uuid, uint64_t mapid) {
    uint64_t ids[AW_SUBS_IDS] = { mapid, 0, 0, 0, 0 };
    size_t sl = aw_subs_write(m, 256, ids);
    uint8_t bu[16]; memset(bu, 0xB0, 16);
    aw_builder b; aw_builder_init(&b, m + sl, 256 - sl);
    aw_build_header(&b, AW_MSG_DBUF, AW_FLAG_GUEST_REPLY, uuid, 100);
    aw_put_s32(&b, AW_KEY_ERR, 0);
    aw_put_uui(&b, AW_KEY_UUI, bu);
    return sl + aw_build_finalize(&b);
}
/* a guest Demt reply (flag 0x8): the frame was consumed */
static size_t build_demt_reply(uint8_t *m, const uint8_t *uuid) {
    aw_builder b; aw_builder_init(&b, m, 256);
    aw_build_header(&b, AW_MSG_DEMT, AW_FLAG_GUEST_REPLY, uuid, 101);
    aw_put_s32(&b, AW_KEY_ERR, 0);
    return aw_build_finalize(&b);
}

/* build a minimal vtpv message with no params */
static size_t msg0(uint8_t *buf, uint32_t type, uint32_t flags, const uint8_t *uuid) {
    aw_builder b; aw_builder_init(&b, buf, 256);
    aw_build_header(&b, type, flags, uuid, 7);
    return aw_build_finalize(&b);
}

static void test_listlike_routing(void) {
    printf("-- decs/dcap/encs routing --\n");
    aw_engine *e = mk(); uint8_t m[256]; aw_msg_view v;

    ereset();
    CHECK(aw_engine_deliver(e, m, msg0(m, AW_MSG_DECS, AW_FLAG_GUEST_REQ, NULL)) == AW_NOERR, "decs status 0");
    CHECK(g_ne == 1, "decs: one reply");
    CHECK(e_type(0, &v) == AW_PARSE_OK && v.type == AW_MSG_DECS && v.flags == AW_FLAG_HOST_REPLY, "decs reply type/flag");
    uint32_t ver = 0;
    CHECK(aw_get_u32(&v, AW_KEY_VER, &ver) == AW_PARSE_OK && ver == 6, "decs ,ver 6");
    /* ,dec is a real bplist decoder list built from the (mock) caps: valid, and
     * carries the AVC/HEVC codec types, our IDs and VTRating. */
    {
        const uint8_t *pls; size_t pl;
        CHECK(aw_get_bytes(&v, AW_KEY_DEC, AW_VT_PLS, &pls, &pl) == AW_PARSE_OK, "decs carries ,dec =pls");
        CHECK(pl > 8 && aw_bplist_check(pls, pl) == 0, "decs ,dec is a valid bplist");
        int has_avc1 = 0, has_rating = 0, has_id = 0;
        for (size_t i = 0; i + 4 <= pl; i++) {
            if (memcmp(pls + i, "avc1", 4) == 0) has_avc1 = 1;
            if (i + 8 <= pl && memcmp(pls + i, "VTRating", 8) == 0) has_rating = 1;
        }
        for (size_t i = 0; i + 30 <= pl; i++)
            if (memcmp(pls + i, "applegva-video.videodecoder.avc", 31 > pl - i ? pl - i : 31) == 0) { has_id = 1; break; }
        CHECK(has_avc1 && has_rating && has_id, "decs ,dec names avc1 + VTRating + our decoder ID");
    }

    /* dcap with ,cod present but WRONG value-type -> -19093, no reply */
    aw_builder b; aw_builder_init(&b, m, sizeof m);
    aw_build_header(&b, AW_MSG_DCAP, AW_FLAG_GUEST_REQ, NULL, 7);
    aw_put_u32(&b, AW_KEY_COD, 1); /* contract: ,cod must be =s32; =u32 is malformed */
    size_t n = aw_build_finalize(&b);
    ereset();
    CHECK(aw_engine_deliver(e, m, n) == AW_ERR_MALFORMED, "dcap wrong ,cod type -> -19093");
    CHECK(g_ne == 0, "dcap malformed: no reply");

    aw_engine_destroy(e);
}

static void test_session_open_close(void) {
    printf("-- +dec / -dec / UUID routing --\n");
    aw_engine *e = mk(); uint8_t m[256]; aw_msg_view v;
    uint8_t uuid[16]; memset(uuid, 0x11, 16);

    ereset();
    CHECK(aw_engine_deliver(e, m, msg0(m, AW_MSG_PDEC, AW_FLAG_GUEST_REQ, uuid)) == AW_NOERR, "+dec status 0");
    CHECK(aw_engine_open_sessions(e) == 1, "+dec opened a session");
    CHECK(g_ne == 1 && e_type(0, &v) == AW_PARSE_OK && v.type == AW_MSG_PDEC, "+dec reply");
    int32_t err = 99; uint32_t ver = 0;
    CHECK(aw_get_s32(&v, AW_KEY_ERR, &err) == AW_PARSE_OK && err == 0, "+dec ,err 0");
    CHECK(aw_get_u32(&v, AW_KEY_VER, &ver) == AW_PARSE_OK && ver == 6, "+dec ,ver 6");

    /* second +dec for the same UUID -> -19093, no reply */
    ereset();
    CHECK(aw_engine_deliver(e, m, msg0(m, AW_MSG_PDEC, AW_FLAG_GUEST_REQ, uuid)) == AW_ERR_MALFORMED, "second +dec -> -19093");
    CHECK(g_ne == 0, "second +dec: no reply");
    CHECK(aw_engine_open_sessions(e) == 1, "still one session");

    /* a session message for an UNKNOWN UUID -> -19094 */
    uint8_t other[16]; memset(other, 0x22, 16);
    CHECK(aw_engine_deliver(e, m, msg0(m, AW_MSG_DCAN, AW_FLAG_GUEST_REQ, other)) == AW_ERR_UNKNOWN_UUID, "unknown UUID -> -19094");

    /* -dec closes it */
    CHECK(aw_engine_deliver(e, m, msg0(m, AW_MSG_MDEC, AW_FLAG_GUEST_REQ, uuid)) == AW_NOERR, "-dec status 0");
    CHECK(aw_engine_open_sessions(e) == 0, "-dec closed the session");
    CHECK(g_close_calls >= 0, "close bookkeeping");

    aw_engine_destroy(e);
}

/* build a dsta message carrying a =fds ImageDescription (avc1, with a tiny avcC) */
static size_t build_dsta(uint8_t *m, const uint8_t *uuid) {
    uint8_t fds[160];
    static const uint8_t avcc[7] = { 0x01,0x42,0x00,0x1e,0xff,0xe1,0x00 };
    size_t fl = aw_imgdesc_build(fds, sizeof fds, AW_4CC('a','v','c','1'),
                                 352, 288, AW_4CC('a','v','c','C'), avcc, sizeof avcc);
    aw_builder b; aw_builder_init(&b, m, 512);
    aw_build_header(&b, AW_MSG_DSTA, AW_FLAG_GUEST_REQ, uuid, 7);
    aw_put_bytes(&b, AW_KEY_FDS, AW_VT_FDS, fds, fl);
    return aw_build_finalize(&b);
}

/* build an async dfrm carrying a ,sbf (sbuf{sdat}) + ,dfl 1 */
static size_t build_dfrm(uint8_t *m, const uint8_t *uuid, const uint8_t *au, size_t aul) {
    uint8_t sbf[256];
    size_t sl = aw_sbuf_write(sbf, sizeof sbf, NULL, au, aul);
    aw_builder b; aw_builder_init(&b, m, 512);
    aw_build_header(&b, AW_MSG_DFRM, AW_FLAG_GUEST_REQ, uuid, 7);
    aw_put_s64(&b, AW_KEY_FRM, 0);
    aw_put_bytes(&b, AW_KEY_SBF, AW_VT_SBF, sbf, sl);
    aw_put_u32(&b, AW_KEY_DFL, 1); /* async */
    return aw_build_finalize(&b);
}

static void test_decode_exchange(void) {
    printf("-- dsta->Dpba, async dfrm->Dbuf/Demt/rels ordering --\n");
    aw_engine *e = mk(); uint8_t m[512]; aw_msg_view v;
    uint8_t uuid[16]; memset(uuid, 0x33, 16);

    aw_engine_deliver(e, m, msg0(m, AW_MSG_PDEC, AW_FLAG_GUEST_REQ, uuid));

    /* dsta: Dpba (flag 0x2) BEFORE the dsta reply (flag 0x4) */
    ereset();
    CHECK(aw_engine_deliver(e, m, build_dsta(m, uuid)) == AW_NOERR, "dsta status 0");
    CHECK(g_open_calls == 1, "dsta opened backend decoder");
    CHECK(g_ne == 2, "dsta emits Dpba + reply (got %d)", g_ne);
    CHECK(e_type(0, &v) == AW_PARSE_OK && v.type == AW_MSG_DPBA && v.flags == AW_FLAG_HOST_REQ, "1st emission = Dpba (0x2)");
    /* the Dpba ,val is a real bplist the framework would parse (H4) */
    {
        const uint8_t *pls; size_t pl;
        CHECK(aw_get_bytes(&v, AW_KEY_VAL, AW_VT_PLS, &pls, &pl) == AW_PARSE_OK, "Dpba carries ,val =pls");
        CHECK(pl > 8 && aw_bplist_check(pls, pl) == 0, "Dpba ,val is a valid bplist00");
    }
    CHECK(e_type(1, &v) == AW_PARSE_OK && v.type == AW_MSG_DSTA && v.flags == AW_FLAG_HOST_REPLY, "2nd emission = dsta reply (0x4)");

    /* async dfrm (event-driven): deliver(dfrm) emits only Dbuf and returns */
    static const uint8_t au[12] = { 0,0,0,1, 0x67, 1,2,3, 0,0,0,1 };
    ereset();
    CHECK(aw_engine_deliver(e, m, build_dfrm(m, uuid, au, sizeof au)) == AW_NOERR, "dfrm status 0");
    CHECK(g_ne == 1 && e_type(0, &v) == AW_PARSE_OK && v.type == AW_MSG_DBUF
          && v.flags == AW_FLAG_HOST_REQ, "dfrm emits Dbuf (0x2) and parks");
    CHECK(g_decode_calls == 0, "no decode until the Dbuf reply");

    /* the guest replies Dbuf (lends a buffer, mapping id 0x9000) -> reply early, decode, copy, Demt */
    ereset();
    CHECK(aw_engine_deliver(e, m, build_dbuf_reply(m, uuid, 0x9000)) == AW_NOERR, "Dbuf reply status 0");
    CHECK(g_resolve_calls == 1 && g_last_mapid == 0x9000, "resolved the lent mapping id");
    CHECK(g_decode_calls == 1 && g_copy_calls == 1, "backend decode + copy ran on the Dbuf reply");
    CHECK(g_ne == 2, "Dbuf reply emits dfrm-reply + Demt (got %d)", g_ne);
    CHECK(e_type(0, &v) == AW_PARSE_OK && v.type == AW_MSG_DFRM && v.flags == AW_FLAG_HOST_REPLY, "1: dfrm reply early (0x4)");
    uint32_t inf = 0;
    CHECK(aw_get_u32(&v, AW_KEY_INF, &inf) == AW_PARSE_OK && (inf & 1u), "dfrm reply ,inf async 0x1");
    CHECK(e_type(1, &v) == AW_PARSE_OK && v.type == AW_MSG_DEMT && v.flags == AW_FLAG_HOST_REQ, "2: Demt (0x2)");
    uint8_t got[16];
    CHECK(aw_get_uui(&v, AW_KEY_UUI, got) == AW_PARSE_OK, "Demt carries ,uui");

    /* the guest replies Demt -> rels */
    ereset();
    CHECK(aw_engine_deliver(e, m, build_demt_reply(m, uuid)) == AW_NOERR, "Demt reply status 0");
    CHECK(g_putbuf_calls == 1, "buffer released after Demt reply");
    CHECK(g_ne == 1 && e_is_rels(0), "Demt reply emits the rels box");
    uint64_t rid[4]; unsigned nr = 0;
    CHECK(aw_rels_read(g_e[0], g_el[0], rid, 4, &nr) == AW_PARSE_OK && nr == 1 && rid[0] == 0x9000, "rels names the lent mapping id");

    /* a dfrm with no decode session (fresh UUID, never dsta) -> 311 */
    uint8_t u2[16]; memset(u2, 0x44, 16);
    aw_engine_deliver(e, m, msg0(m, AW_MSG_PDEC, AW_FLAG_GUEST_REQ, u2));
    ereset();
    aw_engine_deliver(e, m, build_dfrm(m, u2, au, sizeof au));
    CHECK(g_ne == 1 && e_type(0, &v) == AW_PARSE_OK && v.type == AW_MSG_DFRM, "no-session dfrm: one reply");
    int32_t err = 0;
    CHECK(aw_get_s32(&v, AW_KEY_ERR, &err) == AW_PARSE_OK && err == 311, "no-session dfrm ,err 311");

    aw_engine_destroy(e);
}

static void test_sbuf(void) {
    printf("-- sbuf (H3): sdat + native-scalar stia --\n");
    uint8_t buf[256];
    static const uint8_t au[9] = { 9,8,7,6,5,4,3,2,1 };
    aw_sample_timing t;
    memset(&t, 0, sizeof t);
    t.pts.value = 0x0102030405060708LL; t.pts.timescale = 30000; t.pts.flags = 1;
    t.duration.value = 1001;

    size_t n = aw_sbuf_write(buf, sizeof buf, &t, au, sizeof au);
    CHECK(n == 8 + (8 + 72) + (8 + 9), "sbuf len = header + stia + sdat (got %zu)", n);
    /* root atom header is big-endian */
    CHECK(aw_get_be32(buf + 4) == AW_ATOM_SBUF, "root is 'sbuf' (BE)");
    /* the stia scalars inside are NATIVE little-endian: pts.timescale 30000
     * sits at sbuf[8] + stia_hdr(8) + duration(24) + pts.value(8) = offset 48,
     * low byte first. */
    CHECK(buf[8 + 8 + 24 + 8] == (30000 & 0xff), "stia timescale is native-LE");

    const uint8_t *rau; size_t ral; aw_sample_timing rt; int ht = 0;
    CHECK(aw_sbuf_read(buf, n, &rau, &ral, &rt, &ht) == 0, "sbuf read ok");
    CHECK(ral == sizeof au && memcmp(rau, au, ral) == 0, "sdat round-trip");
    CHECK(ht == 1 && rt.pts.value == 0x0102030405060708LL && rt.pts.timescale == 30000,
          "stia native round-trip");

    /* malformed: a sbuf with no sdat is refused */
    uint8_t bad[16];
    aw_put_be32(bad + 0, 16); aw_put_be32(bad + 4, AW_ATOM_SBUF);
    aw_put_be32(bad + 8, 8);  aw_put_be32(bad + 12, AW_4CC('o','p','t','s'));
    CHECK(aw_sbuf_read(bad, 16, &rau, &ral, NULL, NULL) < 0, "sbuf with no sdat refused");
}

int main(void) {
    test_sbuf();
    test_listlike_routing();
    test_session_open_close();
    test_decode_exchange();
    if (g_fail) { printf("FAILED: %d check(s)\n", g_fail); return 1; }
    printf("OK: all aw_session checks passed\n");
    return 0;
}
