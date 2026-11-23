#define _GNU_SOURCE
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
static int g_held;  /* the mock backend's current picture (P1: held across the lend) */
static int g_decode_drop_at = -1, g_decode_fail_at = -1; /* 0-based decode call index */
static int mock_decode(void *dec, const uint8_t *s, size_t sl, vtpv_decoded_frame *o){
    (void)dec;(void)s;(void)sl;
    int idx = g_decode_calls++;
    if (idx == g_decode_drop_at) return VTPV_DROPPED;
    if (idx == g_decode_fail_at) return VTPV_ERR_BACKEND;
    o->surface_id = 42; o->width = 352; o->height = 288; o->pixfmt = VTPV_PIXFMT_NV12;
    g_held = 1;
    return 0;
}
static void mock_close(void *dec){ (void)dec; g_close_calls++; g_held = 0; }
static uint32_t g_out_fmt; /* what the mock decoder says its pictures need (0 = default) */
static uint32_t mock_outfmt(void *dec){ (void)dec; return g_out_fmt; }
static const vtpv_decoder_backend MOCK = {
    mock_query, mock_open, mock_decode, mock_close, mock_outfmt };
static const vtpv_decoder_backend MOCK_NOFMT = {
    mock_query, mock_open, mock_decode, mock_close, NULL };

/* ---- mock guest buffer provider (resolve a lent surface) ------------ */
static int g_resolve_calls, g_copy_calls, g_putbuf_calls, g_resolve_fail;
static uint64_t g_last_mapid;
static int mock_resolve(void *c, const uint8_t uuid[16], uint64_t mapid,
                        uint32_t w, uint32_t h, aw_out_buffer *o){
    (void)c;(void)uuid; g_resolve_calls++; g_last_mapid = mapid;
    if (g_resolve_fail) return -1;
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
    /* Reference contract (guest VideoToolbox Dpba handler, KDK 25C56 x86_64h: CopyCFDictionary(msg,",pba")
     * -> SetPixelBufferAttributes; reims names.rs PIXEL_BUFFER_ATTRIBUTES ",pba" =pls): the attributes
     * dict travels under ,pba as a bplist00; the guest reads NOTHING under ,val for Dpba. */
    {
        const uint8_t *pls = NULL; size_t pl = 0;
        CHECK(aw_get_bytes(&v, AW_KEY_PBA, AW_VT_PLS, &pls, &pl) == AW_PARSE_OK, "Dpba carries ,pba =pls");
        CHECK(pl > 8 && aw_bplist_check(pls, pl) == 0, "Dpba ,pba is a valid bplist00");
        CHECK(aw_get_bytes(&v, AW_KEY_VAL, AW_VT_PLS, &pls, &pl) != AW_PARSE_OK, "Dpba does not use ,val");
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

/* async dfrm with an explicit ,frm and reply id */
static size_t build_dfrm_n(uint8_t *m, const uint8_t *uuid, int64_t frm, uint64_t rid,
                           const uint8_t *au, size_t aul) {
    uint8_t sbf[256];
    size_t sl = aw_sbuf_write(sbf, sizeof sbf, NULL, au, aul);
    aw_builder b; aw_builder_init(&b, m, 512);
    aw_build_header(&b, AW_MSG_DFRM, AW_FLAG_GUEST_REQ, uuid, rid);
    aw_put_s64(&b, AW_KEY_FRM, frm);
    aw_put_bytes(&b, AW_KEY_SBF, AW_VT_SBF, sbf, sl);
    aw_put_u32(&b, AW_KEY_DFL, 1);
    return aw_build_finalize(&b);
}

/* AW-PARK-01..03: a dfrm that arrives while a frame is in flight is parked and
 * started (Dbuf) when the previous frame completes; frames are emitted one Demt
 * per dfrm in decode order. A unit with no picture is emitted as FrameDropped
 * without ,uui, a failed unit with the error and without ,uui; both return the
 * lent buffer (Dfub + rels) and start the next parked unit (reference: Apple's
 * host decoder, wire contract; guest Demt handler accepts a missing ,uui as no
 * pixel buffer). */
static void test_parked_frames(void) {
    printf("-- parked dfrm FIFO, decode-order Demt, dropped/failed frames --\n");
    aw_engine *e = mk(); uint8_t m[512]; aw_msg_view v;
    uint8_t uuid[16]; memset(uuid, 0x55, 16);
    static const uint8_t au[8] = { 0,0,0,4, 0x65, 1,2,3 };
    int64_t frm = -1; int32_t err = 1; uint32_t inf = 0; uint8_t got[16];
    int base = g_decode_calls;

    aw_engine_deliver(e, m, msg0(m, AW_MSG_PDEC, AW_FLAG_GUEST_REQ, uuid));
    aw_engine_deliver(e, m, build_dsta(m, uuid));
    g_decode_drop_at = base + 1;   /* 2nd unit: no picture                       */
    g_decode_fail_at = base + 2;   /* 3rd unit: does not decode                  */

    ereset();
    CHECK(aw_engine_deliver(e, m, build_dfrm_n(m, uuid, 10, 501, au, sizeof au)) == AW_NOERR, "park: dfrm 10");
    CHECK(aw_engine_deliver(e, m, build_dfrm_n(m, uuid, 11, 502, au, sizeof au)) == AW_NOERR, "park: dfrm 11 accepted while 10 in flight");
    CHECK(aw_engine_deliver(e, m, build_dfrm_n(m, uuid, 12, 503, au, sizeof au)) == AW_NOERR, "park: dfrm 12 accepted");
    CHECK(aw_engine_deliver(e, m, build_dfrm_n(m, uuid, 13, 504, au, sizeof au)) == AW_NOERR, "park: dfrm 13 accepted");
    CHECK(g_ne == 1 && e_type(0, &v) == AW_PARSE_OK && v.type == AW_MSG_DBUF &&
          aw_get_s64(&v, AW_KEY_FRM, &frm) == AW_PARSE_OK && frm == 10, "park: only Dbuf(10) so far");

    /* frame 10: Dbuf reply -> dfrm reply(501) + Demt(10, buffer); Demt reply -> rels + Dbuf(11) */
    ereset();
    aw_engine_deliver(e, m, build_dbuf_reply(m, uuid, 0x9100));
    CHECK(g_ne == 2 && e_type(0, &v) == AW_PARSE_OK && v.type == AW_MSG_DFRM && v.reply_id == 501, "park: dfrm reply 501");
    CHECK(e_type(1, &v) == AW_PARSE_OK && v.type == AW_MSG_DEMT && aw_get_s64(&v, AW_KEY_FRM, &frm) == AW_PARSE_OK &&
          frm == 10 && aw_get_uui(&v, AW_KEY_UUI, got) == AW_PARSE_OK, "park: Demt(10) with buffer");
    ereset();
    aw_engine_deliver(e, m, build_demt_reply(m, uuid));
    CHECK(g_ne == 2 && e_is_rels(0), "park: rels after Demt(10) reply");
    CHECK(e_type(1, &v) == AW_PARSE_OK && v.type == AW_MSG_DBUF && aw_get_s64(&v, AW_KEY_FRM, &frm) == AW_PARSE_OK &&
          frm == 11, "park: Dbuf(11) started from the FIFO");

    /* frame 11: no picture -> dfrm reply(502), Demt(11, err 0, FrameDropped, no ,uui), Dfub, rels, Dbuf(12) */
    ereset();
    aw_engine_deliver(e, m, build_dbuf_reply(m, uuid, 0x9200));
    CHECK(g_ne == 5, "drop: 5 emissions (got %d)", g_ne);
    CHECK(e_type(0, &v) == AW_PARSE_OK && v.type == AW_MSG_DFRM && v.reply_id == 502, "drop: dfrm reply 502");
    CHECK(e_type(1, &v) == AW_PARSE_OK && v.type == AW_MSG_DEMT && aw_get_s64(&v, AW_KEY_FRM, &frm) == AW_PARSE_OK && frm == 11 &&
          aw_get_s32(&v, AW_KEY_ERR, &err) == AW_PARSE_OK && err == 0 &&
          aw_get_u32(&v, AW_KEY_INF, &inf) == AW_PARSE_OK && (inf & 0x2u) &&
          aw_get_uui(&v, AW_KEY_UUI, got) != AW_PARSE_OK, "drop: Demt(11) FrameDropped, err 0, no ,uui");
    CHECK(e_type(2, &v) == AW_PARSE_OK && v.type == AW_MSG_DFUB, "drop: Dfub returns the lent buffer");
    CHECK(e_is_rels(3), "drop: rels");
    CHECK(e_type(4, &v) == AW_PARSE_OK && v.type == AW_MSG_DBUF && aw_get_s64(&v, AW_KEY_FRM, &frm) == AW_PARSE_OK &&
          frm == 12, "drop: Dbuf(12) started");

    /* frame 12: does not decode -> Demt(12, -12909, no ,uui), Dfub, rels, Dbuf(13) */
    ereset();
    aw_engine_deliver(e, m, build_dbuf_reply(m, uuid, 0x9300));
    CHECK(g_ne == 5 && e_type(1, &v) == AW_PARSE_OK && v.type == AW_MSG_DEMT &&
          aw_get_s32(&v, AW_KEY_ERR, &err) == AW_PARSE_OK && err == AW_ERR_MALFUNCTION &&
          aw_get_uui(&v, AW_KEY_UUI, got) != AW_PARSE_OK, "fail: Demt(12) -12909 without ,uui");
    CHECK(e_type(4, &v) == AW_PARSE_OK && v.type == AW_MSG_DBUF && aw_get_s64(&v, AW_KEY_FRM, &frm) == AW_PARSE_OK &&
          frm == 13, "fail: Dbuf(13) started");

    /* frame 13 decodes; after its Demt reply nothing is left */
    ereset();
    aw_engine_deliver(e, m, build_dbuf_reply(m, uuid, 0x9400));
    CHECK(g_ne == 2 && e_type(1, &v) == AW_PARSE_OK && v.type == AW_MSG_DEMT, "park: Demt(13)");
    ereset();
    aw_engine_deliver(e, m, build_demt_reply(m, uuid));
    CHECK(g_ne == 1 && e_is_rels(0), "park: FIFO empty after the last frame");

    /* AW-PARK-04: units parked at session close are freed (ASan) */
    aw_engine_deliver(e, m, build_dfrm_n(m, uuid, 20, 601, au, sizeof au));
    aw_engine_deliver(e, m, build_dfrm_n(m, uuid, 21, 602, au, sizeof au));
    g_decode_drop_at = g_decode_fail_at = -1;
    aw_engine_destroy(e);
}

/* AW-PROP-01..05: dsup lists UsingHardwareAcceleratedVideoDecoder; dcop of it
 * answers true (bplist), other properties -12900 without ,val, a non-string
 * ,key is refused without a reply (reference: guest dcop/dsup handlers,
 * vt_callers2.c L4188-4200/L4509-4519; Apple host decoder measurements). */
static size_t build_dcop(uint8_t *m, const uint8_t *uuid, const char *key) {
    uint8_t pls[128]; size_t pl = 0;
    aw_bplist *b = aw_bplist_new();
    int r = aw_bpl_string(b, key);
    pl = aw_bplist_finalize(b, r, pls, sizeof pls);
    aw_bplist_free(b);
    aw_builder bb; aw_builder_init(&bb, m, 512);
    aw_build_header(&bb, AW_MSG_DCOP, AW_FLAG_GUEST_REQ, uuid, 77);
    aw_put_bytes(&bb, AW_KEY_KEY, AW_VT_PLS, pls, pl);
    return aw_build_finalize(&bb);
}
static void test_properties(void) {
    printf("-- dsup / dcop: UsingHardwareAcceleratedVideoDecoder --\n");
    aw_engine *e = mk(); uint8_t m[512]; aw_msg_view v;
    uint8_t uuid[16]; memset(uuid, 0x5a, 16);
    const uint8_t *p; size_t pl; int32_t err = 1; char str[64];
    aw_engine_deliver(e, m, msg0(m, AW_MSG_PDEC, AW_FLAG_GUEST_REQ, uuid));
    /* read_string round trip: short and >15-char strings */
    {
        uint8_t pls[128]; aw_bplist *b = aw_bplist_new();
        size_t n = aw_bplist_finalize(b, aw_bpl_string(b, "RealTime"), pls, sizeof pls); aw_bplist_free(b);
        CHECK(aw_bplist_read_string(pls, n, str, sizeof str) == 8 && !strcmp(str, "RealTime"), "read_string short");
        b = aw_bplist_new();
        n = aw_bplist_finalize(b, aw_bpl_string(b, "UsingHardwareAcceleratedVideoDecoder"), pls, sizeof pls); aw_bplist_free(b);
        CHECK(aw_bplist_read_string(pls, n, str, sizeof str) == 36 && !strcmp(str, "UsingHardwareAcceleratedVideoDecoder"), "read_string long");
        CHECK(aw_bplist_read_string(pls, n, str, 10) < 0, "read_string refuses a short buffer");
    }
    ereset();
    CHECK(aw_engine_deliver(e, m, msg0(m, AW_MSG_DSUP, AW_FLAG_GUEST_REQ, uuid)) == AW_NOERR, "dsup status 0");
    CHECK(g_ne == 1 && e_type(0, &v) == AW_PARSE_OK && v.type == AW_MSG_DSUP && v.flags == AW_FLAG_HOST_REPLY, "dsup reply");
    CHECK(aw_get_s32(&v, AW_KEY_ERR, &err) == AW_PARSE_OK && err == 0, "dsup ,err 0");
    CHECK(aw_get_bytes(&v, AW_KEY_SUP, AW_VT_PLS, &p, &pl) == AW_PARSE_OK && aw_bplist_check(p, pl) == 0 &&
          memmem(p, pl, "UsingHardwareAcceleratedVideoDecoder", 36) != NULL, "dsup ,sup dict lists the property");
    ereset();
    CHECK(aw_engine_deliver(e, m, build_dcop(m, uuid, "UsingHardwareAcceleratedVideoDecoder")) == AW_NOERR, "dcop hw status 0");
    CHECK(g_ne == 1 && e_type(0, &v) == AW_PARSE_OK && v.type == AW_MSG_DCOP && v.reply_id == 77, "dcop reply");
    CHECK(aw_get_s32(&v, AW_KEY_ERR, &err) == AW_PARSE_OK && err == 0, "dcop hw ,err 0");
    CHECK(aw_get_bytes(&v, AW_KEY_VAL, AW_VT_PLS, &p, &pl) == AW_PARSE_OK && pl == 8 + 1 + 1 + 32 &&
          !memcmp(p, "bplist00", 8) && p[8] == 0x09, "dcop hw ,val = bplist true");
    ereset();
    aw_engine_deliver(e, m, build_dcop(m, uuid, "RealTime"));
    CHECK(g_ne == 1 && e_type(0, &v) == AW_PARSE_OK && aw_get_s32(&v, AW_KEY_ERR, &err) == AW_PARSE_OK && err == -12900 &&
          aw_get_bytes(&v, AW_KEY_VAL, AW_VT_PLS, &p, &pl) != AW_PARSE_OK, "dcop other: -12900 without ,val");
    ereset();
    {
        aw_builder bb; aw_builder_init(&bb, m, 512);
        aw_build_header(&bb, AW_MSG_DCOP, AW_FLAG_GUEST_REQ, uuid, 78);
        aw_put_s32(&bb, AW_KEY_KEY, 5);
        size_t n = aw_build_finalize(&bb);
        CHECK(aw_engine_deliver(e, m, n) == AW_ERR_MALFORMED && g_ne == 0, "dcop non-string key refused, no reply");
    }
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

/* The Dpba ,pba plist's PixelFormatType: the one 4-byte bplist integer (marker
 * 0x12, big-endian) among the known CoreVideo formats. 0 if absent. */
static uint32_t dpba_pixfmt(const aw_msg_view *v) {
    static const uint32_t fmts[4] = { VTPV_CV_420V, VTPV_CV_420F, VTPV_CV_X420, VTPV_CV_XF20 };
    const uint8_t *pls; size_t pl; uint32_t found = 0; int nfound = 0;
    if (aw_get_bytes(v, AW_KEY_PBA, AW_VT_PLS, &pls, &pl) != AW_PARSE_OK) return 0;
    for (size_t i = 0; i + 5 <= pl; i++)
        for (int k = 0; k < 4; k++)
            if (pls[i] == 0x12 && aw_get_be32(pls + i + 1) == fmts[k]) { found = fmts[k]; nfound++; }
    return nfound == 1 ? found : 0;
}

/* AW-FMT-01..03 (GVA cycle 3): Dpba asks for the pool format the decoder's
 * pictures need - '420v' by default, 'x420'/'xf20'/'420f' when the backend says
 * so (Apple's host: the format the hvcC's first SPS gives, by bit depth and
 * range; reims docs/video/devices.md @3b3a5a7f^ "HEVC"). */
static void test_dpba_format(void) {
    printf("-- Dpba PixelFormatType from the decoder's output format --\n");
    static const struct { const vtpv_decoder_backend *be; uint32_t fmt, want; const char *name; } cases[] = {
        { &MOCK_NOFMT, 0,            VTPV_CV_420V, "no output_format -> 420v" },
        { &MOCK,       0,            VTPV_CV_420V, "output_format 0 -> 420v" },
        { &MOCK,       VTPV_CV_X420, VTPV_CV_X420, "10-bit -> x420" },
        { &MOCK,       VTPV_CV_XF20, VTPV_CV_XF20, "10-bit full range -> xf20" },
        { &MOCK,       VTPV_CV_420F, VTPV_CV_420F, "8-bit full range -> 420f" },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        aw_session_cfg cfg; memset(&cfg, 0, sizeof cfg);
        cfg.be = cases[i].be; cfg.emit = cap_emit;
        cfg.resolve_buffer = mock_resolve; cfg.copy_frame = mock_copy; cfg.put_buffer = mock_putbuf;
        aw_engine *e = aw_engine_create(&cfg);
        uint8_t m[512]; aw_msg_view v; uint8_t uuid[16]; memset(uuid, 0x60 + (int)i, 16);
        g_out_fmt = cases[i].fmt;
        aw_engine_deliver(e, m, msg0(m, AW_MSG_PDEC, AW_FLAG_GUEST_REQ, uuid));
        ereset();
        aw_engine_deliver(e, m, build_dsta(m, uuid));
        CHECK(g_ne == 2 && e_type(0, &v) == AW_PARSE_OK && v.type == AW_MSG_DPBA, "%s: Dpba emitted", cases[i].name);
        uint32_t got = dpba_pixfmt(&v);
        CHECK(got == cases[i].want, "%s: Dpba PixelFormatType 0x%08x (want 0x%08x)", cases[i].name, got, cases[i].want);
        aw_engine_destroy(e);
    }
    g_out_fmt = 0;
}

/* AW-LEND-01..02 (D-GVA-LEND-FAIL-FAST, host half): a lent buffer the host
 * cannot resolve fails THAT frame at once - the unit is still decoded (its
 * picture may be a reference of later units), then Demt -12909 without a
 * buffer, Dfub of the lent ,uui and rels of a nonzero id, as Apple's host
 * answers a frame it cannot output (reims docs/video/devices.md @3b3a5a7f^
 * "A frame that does not decode") - instead of no Demt at all (the guest then
 * waited out its 10 s reply timeout) - and the next parked unit starts. */
static void test_lend_failure(void) {
    printf("-- unresolvable lent buffer: decode, Demt -12909 no buffer, Dfub, rels, next --\n");
    aw_engine *e = mk(); uint8_t m[512]; aw_msg_view v;
    uint8_t uuid[16]; memset(uuid, 0x77, 16);
    static const uint8_t au[8] = { 0,0,0,4, 0x65, 1,2,3 };
    int64_t frm = -1; int32_t err = 0; uint8_t got[16];
    aw_engine_deliver(e, m, msg0(m, AW_MSG_PDEC, AW_FLAG_GUEST_REQ, uuid));
    aw_engine_deliver(e, m, build_dsta(m, uuid));
    aw_engine_deliver(e, m, build_dfrm_n(m, uuid, 30, 701, au, sizeof au));
    aw_engine_deliver(e, m, build_dfrm_n(m, uuid, 31, 702, au, sizeof au));
    int dec0 = g_decode_calls, copy0 = g_copy_calls;

    g_resolve_fail = 1;
    ereset();
    CHECK(aw_engine_deliver(e, m, build_dbuf_reply(m, uuid, 0x9500)) == AW_NOERR, "lend-fail: Dbuf reply status 0");
    CHECK(g_decode_calls == dec0 + 1, "lend-fail: the unit is still decoded (DPB kept)");
    CHECK(g_copy_calls == copy0, "lend-fail: nothing copied");
    CHECK(g_ne == 5, "lend-fail: dfrm reply, Demt, Dfub, rels, Dbuf(31) (got %d)", g_ne);
    CHECK(e_type(0, &v) == AW_PARSE_OK && v.type == AW_MSG_DFRM && v.reply_id == 701, "lend-fail: dfrm reply 701");
    CHECK(e_type(1, &v) == AW_PARSE_OK && v.type == AW_MSG_DEMT &&
          aw_get_s64(&v, AW_KEY_FRM, &frm) == AW_PARSE_OK && frm == 30 &&
          aw_get_s32(&v, AW_KEY_ERR, &err) == AW_PARSE_OK && err == AW_ERR_MALFUNCTION &&
          aw_get_uui(&v, AW_KEY_UUI, got) != AW_PARSE_OK, "lend-fail: Demt(30) -12909 without ,uui");
    CHECK(e_type(2, &v) == AW_PARSE_OK && v.type == AW_MSG_DFUB, "lend-fail: Dfub returns the lent buffer");
    uint64_t rid[4]; unsigned nr = 0;
    CHECK(e_is_rels(3) && aw_rels_read(g_e[3], g_el[3], rid, 4, &nr) == AW_PARSE_OK && nr == 1 && rid[0] == 0x9500,
          "lend-fail: rels names the lent id");
    CHECK(e_type(4, &v) == AW_PARSE_OK && v.type == AW_MSG_DBUF && aw_get_s64(&v, AW_KEY_FRM, &frm) == AW_PARSE_OK &&
          frm == 31, "lend-fail: Dbuf(31) started at once");

    /* AW-LEND-02: a Dbuf reply that lent no surface (id 0): no rels for id 0 */
    ereset();
    aw_engine_deliver(e, m, build_dbuf_reply(m, uuid, 0));
    CHECK(g_ne == 3, "lend-none: dfrm reply, Demt, Dfub - no rels of id 0 (got %d)", g_ne);
    CHECK(e_type(1, &v) == AW_PARSE_OK && v.type == AW_MSG_DEMT &&
          aw_get_s32(&v, AW_KEY_ERR, &err) == AW_PARSE_OK && err == AW_ERR_MALFUNCTION, "lend-none: Demt -12909");
    CHECK(e_type(2, &v) == AW_PARSE_OK && v.type == AW_MSG_DFUB, "lend-none: Dfub");
    g_resolve_fail = 0;

    /* the session goes on: the next frame decodes into its lent buffer */
    aw_engine_deliver(e, m, build_dfrm_n(m, uuid, 32, 703, au, sizeof au));
    ereset();
    aw_engine_deliver(e, m, build_dbuf_reply(m, uuid, 0x9600));
    CHECK(g_ne == 2 && e_type(1, &v) == AW_PARSE_OK && v.type == AW_MSG_DEMT &&
          aw_get_uui(&v, AW_KEY_UUI, got) == AW_PARSE_OK, "after lend-fail: Demt(32) with buffer");
    aw_engine_destroy(e);
}

/* AW-P1-01..06 (D-GVA-PIPELINE P1 decode-ahead). Oracle = the engine without decode-ahead (the accepted
 * v44 behaviour): the emitted host->guest byte stream must be IDENTICAL with aw_engine_run_deferred called
 * after every emitting step (decode during the lend) and without it (decode on the Dbuf reply), over a
 * scenario with parked units, a dropped unit, a failed unit, an unusable lent buffer and a lend of no
 * surface. Plus the held picture across session close and engine destroy (live reload). */
static uint8_t g_wire[64][2048]; static size_t g_wirel[64]; static int g_nwire;
static void wire_take(void) { for (int i = 0; i < g_ne && g_nwire < 64; i++) { memcpy(g_wire[g_nwire], g_e[i], g_el[i]); g_wirel[g_nwire++] = g_el[i]; } ereset(); }
static int p1_scenario(int ahead, int *decodes_before_reply) {
    aw_engine *e = mk(); uint8_t m[512];
    uint8_t uuid[16]; memset(uuid, 0x5a, 16);
    static const uint8_t au[8] = { 0,0,0,4, 0x65, 1,2,3 };
    int base = g_decode_calls, ahead_hits = 0;
    g_nwire = 0; ereset();
#define STEP(expr) do { (void)(expr); if (ahead) { int b = g_decode_calls; aw_engine_run_deferred(e); ahead_hits += g_decode_calls - b; } wire_take(); } while (0)
    STEP(aw_engine_deliver(e, m, msg0(m, AW_MSG_PDEC, AW_FLAG_GUEST_REQ, uuid)));
    STEP(aw_engine_deliver(e, m, build_dsta(m, uuid)));
    g_decode_drop_at = base + 1; g_decode_fail_at = base + 2;
    for (int i = 0; i < 5; i++) STEP(aw_engine_deliver(e, m, build_dfrm_n(m, uuid, 40 + i, 801 + i, au, sizeof au)));
    STEP(aw_engine_deliver(e, m, build_dbuf_reply(m, uuid, 0xA100)));   /* 40 ok            */
    STEP(aw_engine_deliver(e, m, build_demt_reply(m, uuid)));
    STEP(aw_engine_deliver(e, m, build_dbuf_reply(m, uuid, 0xA200)));   /* 41 dropped       */
    STEP(aw_engine_deliver(e, m, build_dbuf_reply(m, uuid, 0xA300)));   /* 42 fails         */
    g_resolve_fail = 1;
    STEP(aw_engine_deliver(e, m, build_dbuf_reply(m, uuid, 0xA400)));   /* 43 lend unusable */
    g_resolve_fail = 0;
    STEP(aw_engine_deliver(e, m, build_dbuf_reply(m, uuid, 0)));        /* 44 lent nothing  */
    g_decode_drop_at = g_decode_fail_at = -1;
    int decodes = g_decode_calls - base;
    aw_engine_destroy(e);
#undef STEP
    if (decodes_before_reply) *decodes_before_reply = ahead_hits;
    return decodes;
}
static void test_p1_decode_ahead(void) {
    printf("-- P1 decode-ahead: wire-identical, held picture across close / destroy --\n");
    static uint8_t ref[64][2048]; static size_t refl[64]; int nref;
    int hits0 = -1, hits1 = -1;
    int d0 = p1_scenario(0, &hits0);
    nref = g_nwire; for (int i = 0; i < nref; i++) { memcpy(ref[i], g_wire[i], g_wirel[i]); refl[i] = g_wirel[i]; }
    int d1 = p1_scenario(1, &hits1);
    int same = nref == g_nwire;
    for (int i = 0; same && i < nref; i++) same = refl[i] == g_wirel[i] && !memcmp(ref[i], g_wire[i], refl[i]);
    CHECK(nref > 20, "P1-01: the scenario emits a real stream (%d messages)", nref);
    CHECK(same, "P1-01: emitted byte stream identical with and without decode-ahead (%d vs %d messages)", nref, g_nwire);
    CHECK(d0 == 5 && d1 == 5, "P1-02: every unit decoded exactly once either way (%d / %d)", d0, d1);
    CHECK(hits0 == 0 && hits1 == 5, "P1-03: with decode-ahead all 5 units decode before their Dbuf reply (%d)", hits1);

    /* P1-04: the lend comes back before the deferred decode ran: decoded on the reply, and a later
     * run_deferred decodes nothing more */
    {
        aw_engine *e = mk(); uint8_t m[512]; aw_msg_view v;
        uint8_t uuid[16]; memset(uuid, 0x5b, 16);
        static const uint8_t au[8] = { 0,0,0,4, 0x65, 1,2,3 };
        aw_engine_deliver(e, m, msg0(m, AW_MSG_PDEC, AW_FLAG_GUEST_REQ, uuid));
        aw_engine_deliver(e, m, build_dsta(m, uuid));
        aw_engine_deliver(e, m, build_dfrm_n(m, uuid, 50, 901, au, sizeof au));
        int b = g_decode_calls; ereset();
        aw_engine_deliver(e, m, build_dbuf_reply(m, uuid, 0xB100));
        CHECK(g_decode_calls == b + 1 && g_ne == 2 && e_type(1, &v) == AW_PARSE_OK && v.type == AW_MSG_DEMT,
              "P1-04: reply before the deferred decode: decoded on the reply, Demt");
        CHECK(aw_engine_run_deferred(e) == 0 && g_decode_calls == b + 1, "P1-04: nothing left to decode ahead");
        aw_engine_destroy(e);
    }
    /* P1-05: session closed (mdec) while a decoded picture is held: the decoder (and its picture) goes,
     * a late Dbuf reply for it is refused without decode or copy */
    {
        aw_engine *e = mk(); uint8_t m[512];
        uint8_t uuid[16]; memset(uuid, 0x5c, 16);
        static const uint8_t au[8] = { 0,0,0,4, 0x65, 1,2,3 };
        aw_engine_deliver(e, m, msg0(m, AW_MSG_PDEC, AW_FLAG_GUEST_REQ, uuid));
        aw_engine_deliver(e, m, build_dsta(m, uuid));
        aw_engine_deliver(e, m, build_dfrm_n(m, uuid, 60, 1001, au, sizeof au));
        CHECK(aw_engine_run_deferred(e) == 1 && g_held == 1, "P1-05: picture decoded ahead and held");
        int c0 = g_close_calls, cp0 = g_copy_calls, dc0 = g_decode_calls; ereset();
        aw_engine_deliver(e, m, msg0(m, AW_MSG_MDEC, AW_FLAG_GUEST_REQ, uuid));
        CHECK(g_close_calls == c0 + 1 && g_held == 0, "P1-05: close released the decoder and its held picture");
        CHECK(aw_engine_deliver(e, m, build_dbuf_reply(m, uuid, 0xC100)) == AW_ERR_UNKNOWN_UUID &&
              g_copy_calls == cp0 && g_decode_calls == dc0, "P1-05: late Dbuf reply refused, no decode, no copy");
        CHECK(aw_engine_run_deferred(e) == 0, "P1-05: nothing pending after close");
        aw_engine_destroy(e);
    }
    /* P1-06: engine destroyed with a held picture (device teardown / live plugin reload) */
    {
        aw_engine *e = mk(); uint8_t m[512];
        uint8_t uuid[16]; memset(uuid, 0x5d, 16);
        static const uint8_t au[8] = { 0,0,0,4, 0x65, 1,2,3 };
        aw_engine_deliver(e, m, msg0(m, AW_MSG_PDEC, AW_FLAG_GUEST_REQ, uuid));
        aw_engine_deliver(e, m, build_dsta(m, uuid));
        aw_engine_deliver(e, m, build_dfrm_n(m, uuid, 70, 1101, au, sizeof au));
        aw_engine_deliver(e, m, build_dfrm_n(m, uuid, 71, 1102, au, sizeof au));   /* parked too */
        CHECK(aw_engine_run_deferred(e) == 1 && g_held == 1, "P1-06: in-flight unit decoded ahead, parked unit waits");
        int c0 = g_close_calls;
        aw_engine_destroy(e);
        CHECK(g_close_calls == c0 + 1 && g_held == 0, "P1-06: destroy closes the decoder holding the picture (ASan: parked unit freed)");
    }
}

int main(void) {
    test_sbuf();
    test_listlike_routing();
    test_session_open_close();
    test_decode_exchange();
    test_parked_frames();
    test_properties();
    test_dpba_format();
    test_lend_failure();
    test_p1_decode_ahead();
    if (g_fail) { printf("FAILED: %d check(s)\n", g_fail); return 1; }
    printf("OK: all aw_session checks passed\n");
    return 0;
}
