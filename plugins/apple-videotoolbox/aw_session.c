/*
 * aw_session.c - the host Apple-wire VideoToolbox session engine. See aw_session.h,
 * the WIRE-CONTRACT doc and contract-B. CLEAN-ROOM. Lane L-VIDEO-IMPL, 2026-11-16.
 */
#include "aw_session.h"
#include "aw_sbuf.h"
#include "aw_bplist.h"

#include <stdlib.h>
#include <string.h>

#define AW_MAX_SESSIONS 16u

/* 'avc1'/'hvc1' on the wire == the backend's VTPV_CODEC_* values. */
#define AW_CODEC_AVC1 AW_4CC('a','v','c','1')
#define AW_CODEC_HVC1 AW_4CC('h','v','c','1')
#define AW_EXT_AVCC   AW_4CC('a','v','c','C')
#define AW_EXT_HVCC   AW_4CC('h','v','c','C')
#define AW_PIXFMT_420V AW_4CC('4','2','0','v')

/* a session's in-flight frame (one at a time in this slice; up to ~3 is a later
 * refinement) progresses: IDLE -> AWAIT_DBUF (Dbuf sent) -> AWAIT_DEMT (frame
 * copied, Demt sent) -> IDLE. */
enum { AW_FRM_IDLE = 0, AW_FRM_AWAIT_DBUF, AW_FRM_AWAIT_DEMT };

typedef struct aw_dsession {
    int      used;
    int      invalidated;
    int      started;        /* dsta accepted + backend decoder open             */
    uint8_t  uuid[16];
    void    *dec;            /* backend decoder handle                           */
    uint32_t codec, width, height;
    uint64_t next_mapping_id;

    /* parked frame (event-driven dfrm, H7) */
    int          pend_state;
    uint8_t     *pend_au;    /* the access-unit bytes, kept until the Dbuf reply */
    size_t       pend_au_cap, pend_au_len;
    int64_t      pend_frm;
    uint32_t     pend_inf;   /* async flag echoed into the dfrm reply / Demt      */
    uint64_t     pend_dfrm_reply_id;
    aw_out_buffer pend_buf;  /* the lent buffer, held from Dbuf reply to rels     */
} aw_dsession;

struct aw_engine {
    aw_session_cfg cfg;
    aw_dsession    sess[AW_MAX_SESSIONS];
    uint64_t       next_reply_id; /* host reply clerk: numbers from 100          */
};

/* ---- binary-plist payloads (H4) ------------------------------------------ *
 * The guest's VideoToolbox parses these as bplist00 (contract-B §3). */

/* Dpba: {PixelFormatType [420v], Width, Height, IOSurfaceProperties {},
 * BytesPerRowAlignment 64, PlaneAlignment 256,
 * PreferRealTimeCacheModeIfEveryoneDoes true}. PixelFormatType is an array of
 * one OSType number ('420v'); if a guest shows it wants a string, swap
 * aw_bpl_int -> aw_bpl_string (a guest-confirm point, FIX-CANDIDATE §4.4). */
static size_t build_dpba_plist(uint8_t *out, size_t cap, uint32_t w, uint32_t h)
{
    aw_bplist *b = aw_bplist_new();
    if (!b) return 0;
    int fourcc = aw_bpl_int(b, (int64_t)AW_PIXFMT_420V);
    int v_pft  = aw_bpl_array(b, &fourcc, 1);
    int v_io   = aw_bpl_dict(b, NULL, NULL, 0);          /* IOSurfaceProperties {} */
    int keys[7] = {
        aw_bpl_string(b, "PixelFormatType"),
        aw_bpl_string(b, "Width"),
        aw_bpl_string(b, "Height"),
        aw_bpl_string(b, "IOSurfaceProperties"),
        aw_bpl_string(b, "BytesPerRowAlignment"),
        aw_bpl_string(b, "PlaneAlignment"),
        aw_bpl_string(b, "PreferRealTimeCacheModeIfEveryoneDoes"),
    };
    int vals[7] = {
        v_pft,
        aw_bpl_int(b, (int)w),
        aw_bpl_int(b, (int)h),
        v_io,
        aw_bpl_int(b, 64),
        aw_bpl_int(b, 256),
        aw_bpl_bool(b, 1),
    };
    int root = aw_bpl_dict(b, keys, vals, 7);
    size_t n = (root >= 0) ? aw_bplist_finalize(b, root, out, cap) : 0;
    aw_bplist_free(b);
    return n;
}

/* ,hin: {VTHostBuildVersion: "<our host id>"}. */
static size_t build_hin_plist(uint8_t *out, size_t cap)
{
    aw_bplist *b = aw_bplist_new();
    if (!b) return 0;
    int k = aw_bpl_string(b, "VTHostBuildVersion");
    int v = aw_bpl_string(b, "applegva-video 0.1");
    int root = aw_bpl_dict(b, &k, &v, 1);
    size_t n = (root >= 0) ? aw_bplist_finalize(b, root, out, cap) : 0;
    aw_bplist_free(b);
    return n;
}

/* A codec-list reply body (,dec / ,dca / ,enc). The decoder dictionaries built
 * from the live VA-API caps (CMClassImplementationID/Name, VTCodecType,
 * VTRating 400, VTPerProfileDetails) are a content follow-on; this emits a
 * well-formed (currently empty) bplist dict so the reply carries real plist
 * bytes the framework parses. */
static size_t build_codeclist_plist(uint8_t *out, size_t cap)
{
    aw_bplist *b = aw_bplist_new();
    if (!b) return 0;
    int root = aw_bpl_dict(b, NULL, NULL, 0);
    size_t n = (root >= 0) ? aw_bplist_finalize(b, root, out, cap) : 0;
    aw_bplist_free(b);
    return n;
}

/* ---- ImageDescription (=fds) --------------------------------------------- */

size_t aw_imgdesc_build(uint8_t *buf, size_t cap, uint32_t codec,
                        uint16_t width, uint16_t height,
                        uint32_t ext_fourcc, const uint8_t *ext, size_t ext_len)
{
    size_t ext_box = ext_len ? (8 + ext_len) : 0;
    size_t total   = 86 + ext_box + 4; /* header + extension + trailing zero u32 */
    if (cap < total) return 0;
    memset(buf, 0, total);
    aw_put_be32(buf + 0x00, (uint32_t)total);  /* idSize                         */
    aw_put_be32(buf + 0x04, codec);            /* cType                          */
    aw_put_be16(buf + 0x0e, 0xffff);           /* dataRefIndex (measured)        */
    aw_put_be32(buf + 0x18, 0x200);            /* temporalQuality                */
    aw_put_be32(buf + 0x1c, 0x200);            /* spatialQuality                 */
    aw_put_be16(buf + 0x20, width);
    aw_put_be16(buf + 0x22, height);
    aw_put_be32(buf + 0x24, 72u << 16);        /* hRes 72 dpi                    */
    aw_put_be32(buf + 0x28, 72u << 16);        /* vRes 72 dpi                    */
    aw_put_be16(buf + 0x30, 1);                /* frameCount                     */
    aw_put_be16(buf + 0x52, 24);               /* depth                          */
    aw_put_be16(buf + 0x54, 0xffff);           /* clutID -1                      */
    if (ext_box) {
        aw_put_be32(buf + 86 + 0, (uint32_t)ext_box);
        aw_put_be32(buf + 86 + 4, ext_fourcc);
        memcpy(buf + 86 + 8, ext, ext_len);
    }
    /* trailing zero u32 already zeroed by memset */
    return total;
}

int aw_imgdesc_parse(const uint8_t *fds, size_t len, uint32_t *codec,
                     uint16_t *w, uint16_t *h,
                     uint32_t *ext_fourcc, const uint8_t **ext, size_t *ext_len)
{
    if (!fds || len < 86) return AW_PARSE_BAD;
    if (codec) *codec = aw_get_be32(fds + 0x04);
    if (w) *w = aw_get_be16(fds + 0x20);
    if (h) *h = aw_get_be16(fds + 0x22);
    if (ext) { *ext = NULL; *ext_len = 0; }
    if (ext_fourcc) *ext_fourcc = 0;

    size_t off = 86;
    while (off + 8 <= len) {
        uint32_t asz = aw_get_be32(fds + off);
        uint32_t fcc = aw_get_be32(fds + off + 4);
        if (asz == 0) break;                 /* the trailing zero u32            */
        if (asz < 8 || asz > len - off) break;
        if (fcc == AW_EXT_AVCC || fcc == AW_EXT_HVCC) {
            if (ext) { *ext = fds + off + 8; *ext_len = asz - 8; }
            if (ext_fourcc) *ext_fourcc = fcc;
            break;
        }
        off += asz;
    }
    return 0;
}

/* ---- engine lifecycle ---------------------------------------------------- */

aw_engine *aw_engine_create(const aw_session_cfg *cfg)
{
    aw_engine *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->cfg = *cfg;
    e->next_reply_id = 100;
    return e;
}

static void close_session(aw_engine *e, aw_dsession *s)
{
    if (s->pend_state == AW_FRM_AWAIT_DEMT && e->cfg.put_buffer)
        e->cfg.put_buffer(e->cfg.buf_ctx, &s->pend_buf); /* let a held buffer go */
    if (s->pend_au) free(s->pend_au);
    if (s->dec && e->cfg.be && e->cfg.be->close_decoder)
        e->cfg.be->close_decoder(s->dec);
    memset(s, 0, sizeof(*s));
}

void aw_engine_destroy(aw_engine *e)
{
    if (!e) return;
    for (unsigned i = 0; i < AW_MAX_SESSIONS; i++)
        if (e->sess[i].used) close_session(e, &e->sess[i]);
    free(e);
}

unsigned aw_engine_open_sessions(const aw_engine *e)
{
    unsigned n = 0;
    for (unsigned i = 0; i < AW_MAX_SESSIONS; i++)
        if (e->sess[i].used) n++;
    return n;
}

static aw_dsession *find_session(aw_engine *e, const uint8_t uuid[16])
{
    for (unsigned i = 0; i < AW_MAX_SESSIONS; i++)
        if (e->sess[i].used && memcmp(e->sess[i].uuid, uuid, 16) == 0)
            return &e->sess[i];
    return NULL;
}

static aw_dsession *alloc_session(aw_engine *e, const uint8_t uuid[16])
{
    for (unsigned i = 0; i < AW_MAX_SESSIONS; i++) {
        if (!e->sess[i].used) {
            aw_dsession *s = &e->sess[i];
            memset(s, 0, sizeof(*s));
            s->used = 1;
            memcpy(s->uuid, uuid, 16);
            s->next_mapping_id = ((uint64_t)(i + 1)) << 32 | 1; /* distinct ids */
            return s;
        }
    }
    return NULL;
}

/* ---- emit helpers -------------------------------------------------------- */

static void emit(aw_engine *e, const uint8_t *msg, size_t len)
{
    if (e->cfg.emit) e->cfg.emit(e->cfg.emit_ctx, msg, len);
}

/* A reply to a guest request: echoes the request's type, UUID and reply-id,
 * flag 0x4. The caller adds params between the header and finalize via `fill`. */
static uint64_t next_req_id(aw_engine *e) { return e->next_reply_id++; }

/* ---- host-session (session-less) handlers -------------------------------- */

/* decs / dcap / encs: reply with ,ver 6 (+ a codec-list param placeholder).
 * The codec-list dictionaries are binary plists (H4); here we emit a structurally
 * valid reply carrying ,ver 6 and an empty =pls list, which H4 fills from the
 * live VA-API caps. The routing + reply framing are what this slice proves. */
static int32_t handle_listlike(aw_engine *e, const aw_msg_view *v, uint32_t list_key)
{
    uint8_t pls[128];
    size_t pls_len = build_codeclist_plist(pls, sizeof pls);
    uint8_t buf[256];
    aw_builder b;
    aw_builder_init(&b, buf, sizeof buf);
    aw_build_header(&b, v->type, AW_FLAG_HOST_REPLY, v->uuid, v->reply_id);
    aw_put_bytes(&b, list_key, AW_VT_PLS, pls, pls_len); /* bplist dict (H4)      */
    aw_put_u32(&b, AW_KEY_VER, 6);
    size_t n = aw_build_finalize(&b);
    if (n) emit(e, buf, n);
    return AW_NOERR;
}

static int32_t handle_dcap(aw_engine *e, const aw_msg_view *v)
{
    int32_t cod;
    int r = aw_get_s32(v, AW_KEY_COD, &cod);
    if (r == AW_PARSE_WRONGTYPE || r == AW_PARSE_SHORT)
        return AW_ERR_MALFORMED;     /* ,cod present with wrong type: -19093, no reply */
    /* absent ,cod -> default; reply ,dca (NULL for an unknown codec) + ,ver 6 */
    return handle_listlike(e, v, AW_KEY_DCA);
}

static int32_t handle_opendec(aw_engine *e, const aw_msg_view *v)
{
    if (find_session(e, v->uuid))
        return AW_ERR_MALFORMED;     /* a UUID that already has one: -19093, no reply */
    aw_dsession *s = alloc_session(e, v->uuid);
    if (!s) return AW_ERR_MALFORMED;

    uint8_t buf[256];
    aw_builder b;
    aw_builder_init(&b, buf, sizeof buf);
    uint8_t hin[128];
    size_t hin_len = build_hin_plist(hin, sizeof hin);
    aw_build_header(&b, v->type, AW_FLAG_HOST_REPLY, v->uuid, v->reply_id);
    aw_put_s32(&b, AW_KEY_ERR, AW_NOERR);  /* codec creation status (deferred to dsta) */
    aw_put_u32(&b, AW_KEY_VER, 6);
    aw_put_bytes(&b, AW_KEY_HIN, AW_VT_PLS, hin, hin_len); /* {VTHostBuildVersion} */
    size_t n = aw_build_finalize(&b);
    if (n) emit(e, buf, n);
    return AW_NOERR;
}

static int32_t handle_closedec(aw_engine *e, const aw_msg_view *v)
{
    aw_dsession *s = find_session(e, v->uuid);
    if (s) close_session(e, s);
    return AW_NOERR;                 /* unknown UUID is noErr too                */
}

/* ---- decoder-session handlers -------------------------------------------- */

/* Dpba: host->guest request (flag 0x2) sent from inside dsta, BEFORE the dsta
 * reply. Payload is the output-buffer-attributes dict as a =pls binary plist
 * {PixelFormatType [420v], Width, Height, BytesPerRowAlignment 64, PlaneAlignment
 * 256, ...}; the bplist encoder is H4, so this slice emits the Dpba message with
 * its flags/UUID/ordering correct and a placeholder =pls body. */
static void emit_dpba(aw_engine *e, aw_dsession *s)
{
    uint8_t pls[256];
    size_t pls_len = build_dpba_plist(pls, sizeof pls, s->width, s->height);
    uint8_t buf[512];
    aw_builder b;
    aw_builder_init(&b, buf, sizeof buf);
    aw_build_header(&b, AW_MSG_DPBA, AW_FLAG_HOST_REQ, s->uuid, next_req_id(e));
    aw_put_bytes(&b, AW_KEY_VAL, AW_VT_PLS, pls, pls_len); /* real bplist (H4)    */
    size_t n = aw_build_finalize(&b);
    if (n) emit(e, buf, n);
}

static int32_t handle_dsta(aw_engine *e, aw_dsession *s, const aw_msg_view *v)
{
    const uint8_t *fds; size_t fds_len;
    int r = aw_get_bytes(v, AW_KEY_FDS, AW_VT_FDS, &fds, &fds_len);
    if (r != AW_PARSE_OK)
        return AW_ERR_MALFORMED;     /* ,fds required: no reply, message refused  */

    if (s->started)
        return AW_ERR_MALFUNCTION;   /* a second dsta: -12909 (reply handled by caller path) */

    uint32_t codec; uint16_t w, h, extcc = 0;
    const uint8_t *ext = NULL; size_t ext_len = 0; uint32_t ext_fourcc = 0;
    if (aw_imgdesc_parse(fds, fds_len, &codec, &w, &h, &ext_fourcc, &ext, &ext_len) < 0)
        return AW_ERR_MALFORMED;
    (void)extcc;

    /* Open the backend decoder with the avcC/hvcC the ImageDescription carries. */
    void *dec = NULL;
    int br = -1;
    if (e->cfg.be && e->cfg.be->open_decoder)
        br = e->cfg.be->open_decoder(e->cfg.be_ctx, codec, w, h, ext, ext_len, &dec);

    uint8_t buf[256];
    aw_builder b;
    if (br != 0) {
        /* refused (e.g. coded side < backend min): -12911 and NO Dpba */
        aw_builder_init(&b, buf, sizeof buf);
        aw_build_header(&b, AW_MSG_DSTA, AW_FLAG_HOST_REPLY, v->uuid, v->reply_id);
        aw_put_s32(&b, AW_KEY_ERR, AW_ERR_UNSUPPORTED_FMT);
        size_t n = aw_build_finalize(&b);
        if (n) emit(e, buf, n);
        return AW_NOERR;             /* the refusal is in the reply ,err         */
    }

    s->dec = dec; s->codec = codec; s->width = w; s->height = h; s->started = 1;

    emit_dpba(e, s);                 /* Dpba BEFORE the dsta reply                */

    aw_builder_init(&b, buf, sizeof buf);
    aw_build_header(&b, AW_MSG_DSTA, AW_FLAG_HOST_REPLY, v->uuid, v->reply_id);
    aw_put_s32(&b, AW_KEY_ERR, AW_NOERR);
    size_t n = aw_build_finalize(&b);
    if (n) emit(e, buf, n);
    return AW_NOERR;
}

static void emit_dbuf(aw_engine *e, aw_dsession *s, int64_t frm)
{
    uint8_t buf[256];
    aw_builder b;
    aw_builder_init(&b, buf, sizeof buf);
    aw_build_header(&b, AW_MSG_DBUF, AW_FLAG_HOST_REQ, s->uuid, next_req_id(e));
    aw_put_s64(&b, AW_KEY_FRM, frm);
    aw_put_bytes(&b, AW_KEY_POP, AW_VT_PLS, NULL, 0); /* buffer options (H4)      */
    size_t n = aw_build_finalize(&b);
    if (n) emit(e, buf, n);
}

static void emit_demt(aw_engine *e, aw_dsession *s, int64_t frm, int32_t err,
                      uint32_t inf, const uint8_t uuid[16])
{
    uint8_t buf[256];
    aw_builder b;
    aw_builder_init(&b, buf, sizeof buf);
    aw_build_header(&b, AW_MSG_DEMT, AW_FLAG_HOST_REQ, s->uuid, next_req_id(e));
    aw_put_s64(&b, AW_KEY_FRM, frm);
    aw_put_s32(&b, AW_KEY_ERR, err);
    aw_put_u32(&b, AW_KEY_INF, inf);
    aw_put_uui(&b, AW_KEY_UUI, uuid);
    size_t n = aw_build_finalize(&b);
    if (n) emit(e, buf, n);
}

static void emit_dfub(aw_engine *e, aw_dsession *s, const uint8_t uuid[16])
{
    uint8_t buf[256];
    aw_builder b;
    aw_builder_init(&b, buf, sizeof buf);
    aw_build_header(&b, AW_MSG_DFUB, 0, s->uuid, 0);
    aw_put_uiplus(&b, AW_KEY_UIPLUS, uuid, 1);
    size_t n = aw_build_finalize(&b);
    if (n) emit(e, buf, n);
}

static void emit_rels(aw_engine *e, uint64_t mapping_id)
{
    uint8_t buf[32];
    size_t n = aw_rels_write(buf, sizeof buf, &mapping_id, 1);
    if (n) emit(e, buf, n);          /* a bare rels box, not a vtpv message       */
}

static void emit_dfrm_reply(aw_engine *e, aw_dsession *s, const aw_msg_view *v,
                            int32_t err, uint32_t inf)
{
    uint8_t buf[256];
    aw_builder b;
    aw_builder_init(&b, buf, sizeof buf);
    aw_build_header(&b, AW_MSG_DFRM, AW_FLAG_HOST_REPLY, s->uuid, v->reply_id);
    aw_put_s32(&b, AW_KEY_ERR, err);
    aw_put_u32(&b, AW_KEY_INF, inf);
    size_t n = aw_build_finalize(&b);
    if (n) emit(e, buf, n);
}

/* dfrm: park the access unit, emit Dbuf, and RETURN. The decode continues when
 * the guest's Dbuf reply arrives (handle_guest_reply). Async-only, one frame in
 * flight per session in this slice. */
static int32_t handle_dfrm(aw_engine *e, aw_dsession *s, const aw_msg_view *v)
{
    if (!s->started) {
        emit_dfrm_reply(e, s, v, AW_ERR_BAD_DATA, 0);   /* 311, nothing else       */
        return AW_NOERR;
    }
    if (s->pend_state != AW_FRM_IDLE)
        return AW_ERR_MALFUNCTION;   /* a second frame before the last finished   */

    const uint8_t *sbf; size_t sbf_len;
    if (aw_get_bytes(v, AW_KEY_SBF, AW_VT_SBF, &sbf, &sbf_len) != AW_PARSE_OK)
        return AW_ERR_MALFORMED;     /* ,sbf required                             */
    const uint8_t *au; size_t au_len;
    if (aw_sbuf_read(sbf, sbf_len, &au, &au_len, NULL, NULL) < 0)
        return AW_ERR_MALFORMED;

    /* keep the AU bytes until the Dbuf reply lets us decode into a lent buffer */
    if (s->pend_au_cap < au_len) {
        uint8_t *n = realloc(s->pend_au, au_len);
        if (!n) return AW_ERR_MALFUNCTION;
        s->pend_au = n; s->pend_au_cap = au_len;
    }
    memcpy(s->pend_au, au, au_len);
    s->pend_au_len = au_len;

    int64_t frm64 = 0; (void)aw_get_s64(v, AW_KEY_FRM, &frm64);
    uint32_t dfl = 0; aw_get_u32(v, AW_KEY_DFL, &dfl);
    s->pend_frm = frm64;
    s->pend_inf = (dfl & 1u) ? 0x1u : 0u; /* kVTDecodeInfo_Asynchronous           */
    s->pend_dfrm_reply_id = v->reply_id;
    s->pend_state = AW_FRM_AWAIT_DBUF;

    emit_dbuf(e, s, frm64);          /* -> guest replies Dbuf with a lent buffer   */
    return AW_NOERR;
}

/* A guest reply (flag 0x8). `msg`/`len` are the raw message so a Dbuf reply's
 * subs-prefixed mapping id can be read. */
static int32_t handle_guest_reply(aw_engine *e, aw_dsession *s,
                                  const aw_msg_view *v, const uint8_t *msg, size_t len)
{
    if (v->type == AW_MSG_DBUF && s->pend_state == AW_FRM_AWAIT_DBUF) {
        /* the lent surface: ,uui (buffer id) + a subs-prefixed mapping id */
        uint8_t bu[16]; memset(bu, 0, 16);
        (void)aw_get_uui(v, AW_KEY_UUI, bu);
        uint64_t ids[AW_SUBS_IDS]; uint64_t mapid = 0;
        if (aw_subs_read(msg, len, ids) == AW_PARSE_OK) mapid = ids[0];

        aw_out_buffer ob; memset(&ob, 0, sizeof ob);
        int got = e->cfg.resolve_buffer
            ? e->cfg.resolve_buffer(e->cfg.buf_ctx, bu, mapid, s->width, s->height, &ob) : -1;

        /* async: the dfrm reply goes EARLY, right after the Dbuf reply */
        {
            aw_msg_view fv = *v; fv.reply_id = s->pend_dfrm_reply_id;
            emit_dfrm_reply(e, s, &fv, AW_NOERR, s->pend_inf);
        }

        if (got != 0) {              /* guest lent nothing usable: drop the frame */
            s->pend_state = AW_FRM_IDLE;
            return AW_NOERR;
        }

        vtpv_decoded_frame f; memset(&f, 0, sizeof f);
        int dr = e->cfg.be->decode(s->dec, s->pend_au, s->pend_au_len, &f);
        if (dr != 0) {               /* failed frame: Demt -12909, Dfub, rels     */
            emit_demt(e, s, s->pend_frm, AW_ERR_MALFUNCTION, s->pend_inf, ob.uuid);
            emit_dfub(e, s, ob.uuid);
            emit_rels(e, ob.mapping_id);
            if (e->cfg.put_buffer) e->cfg.put_buffer(e->cfg.buf_ctx, &ob);
            s->pend_state = AW_FRM_IDLE;
            return AW_NOERR;
        }

        int cr = e->cfg.copy_frame ? e->cfg.copy_frame(e->cfg.buf_ctx, &f, &ob) : -1;
        s->pend_buf = ob;
        emit_demt(e, s, s->pend_frm, cr == 0 ? AW_NOERR : AW_ERR_MALFUNCTION,
                  s->pend_inf | 1u, ob.uuid);
        s->pend_state = AW_FRM_AWAIT_DEMT; /* -> guest replies Demt                */
        return AW_NOERR;
    }

    if (v->type == AW_MSG_DEMT && s->pend_state == AW_FRM_AWAIT_DEMT) {
        emit_rels(e, s->pend_buf.mapping_id);  /* the frame is done; release it    */
        if (e->cfg.put_buffer) e->cfg.put_buffer(e->cfg.buf_ctx, &s->pend_buf);
        s->pend_state = AW_FRM_IDLE;
        return AW_NOERR;
    }

    /* a reply to no pending request */
    return AW_ERR_MALFORMED;
}

/* dcan / dinv / dfin / dsup / dcop / dset: minimal correct replies for routing. */
static int32_t handle_dcan(aw_engine *e, aw_dsession *s, const aw_msg_view *v)
{
    (void)s;
    uint8_t buf[256]; aw_builder b;
    aw_builder_init(&b, buf, sizeof buf);
    aw_build_header(&b, AW_MSG_DCAN, AW_FLAG_HOST_REPLY, v->uuid, v->reply_id);
    aw_put_u32(&b, AW_KEY_CAN, 1);
    size_t n = aw_build_finalize(&b); if (n) emit(e, buf, n);
    return AW_NOERR;
}

static int32_t reply_err(aw_engine *e, const aw_msg_view *v, int32_t err)
{
    uint8_t buf[256]; aw_builder b;
    aw_builder_init(&b, buf, sizeof buf);
    aw_build_header(&b, v->type, AW_FLAG_HOST_REPLY, v->uuid, v->reply_id);
    aw_put_s32(&b, AW_KEY_ERR, err);
    size_t n = aw_build_finalize(&b); if (n) emit(e, buf, n);
    return AW_NOERR;
}

static int32_t handle_dinv(aw_engine *e, aw_dsession *s, const aw_msg_view *v)
{
    int32_t rc = reply_err(e, v, AW_NOERR);
    close_session(e, s);             /* dinv drops the decoder                    */
    return rc;
}

/* ---- the queue-0 switch -------------------------------------------------- */

int32_t aw_engine_deliver(aw_engine *e, const uint8_t *msg, size_t len)
{
    if (len > (16u << 20)) return AW_ERR_OVER_16MIB;

    aw_msg_view v;
    if (aw_parse(msg, len, &v) != AW_PARSE_OK)
        return AW_ERR_MALFORMED;

    switch (v.type) {
    /* session-less */
    case AW_MSG_DECS: return handle_listlike(e, &v, AW_KEY_DEC);
    case AW_MSG_ENCS: return handle_listlike(e, &v, AW_KEY_ENC);
    case AW_MSG_DCAP: return handle_dcap(e, &v);
    case AW_MSG_PDEC: return handle_opendec(e, &v);
    case AW_MSG_MDEC:
    case AW_MSG_MUUI: return handle_closedec(e, &v);
    case AW_MSG_MALL: return AW_NOERR;   /* customer build: noop                  */
    default: break;
    }

    /* session messages: route by UUID */
    aw_dsession *s = find_session(e, v.uuid);
    if (!s)              return AW_ERR_UNKNOWN_UUID; /* -19094                     */
    if (s->invalidated)  return AW_ERR_INVALIDATED;  /* -19098                     */

    /* a guest reply (flag 0x8) to one of our host requests (Dbuf / Demt) */
    if (v.flags & AW_FLAG_GUEST_REPLY)
        return handle_guest_reply(e, s, &v, msg, len);

    switch (v.type) {
    case AW_MSG_DSTA: return handle_dsta(e, s, &v);
    case AW_MSG_DFRM: return handle_dfrm(e, s, &v);
    case AW_MSG_DCAN: return handle_dcan(e, s, &v);
    case AW_MSG_DINV: return handle_dinv(e, s, &v);
    case AW_MSG_DFIN: return reply_err(e, &v, AW_ERR_UNIMPLEMENTED); /* -12782    */
    case AW_MSG_DSUP: return reply_err(e, &v, AW_NOERR);
    case AW_MSG_DCOP:
    case AW_MSG_DSET: return reply_err(e, &v, AW_NOERR);
    default:          return AW_ERR_UNKNOWN_UUID;
    }
}
