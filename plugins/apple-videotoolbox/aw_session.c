/*
 * aw_session.c - the host Apple-wire VideoToolbox session engine. See aw_session.h,
 * the WIRE-CONTRACT doc and contract-B. CLEAN-ROOM. Lane L-VIDEO-IMPL, 2026-11-16.
 */
#include "aw_session.h"
#include "aw_sbuf.h"

#include <stdlib.h>
#include <string.h>

#define AW_MAX_SESSIONS 16u

/* 'avc1'/'hvc1' on the wire == the backend's VTPV_CODEC_* values. */
#define AW_CODEC_AVC1 AW_4CC('a','v','c','1')
#define AW_CODEC_HVC1 AW_4CC('h','v','c','1')
#define AW_EXT_AVCC   AW_4CC('a','v','c','C')
#define AW_EXT_HVCC   AW_4CC('h','v','c','C')
#define AW_PIXFMT_420V AW_4CC('4','2','0','v')

typedef struct aw_dsession {
    int      used;
    int      invalidated;
    int      started;        /* dsta accepted + backend decoder open             */
    uint8_t  uuid[16];
    void    *dec;            /* backend decoder handle                           */
    uint32_t codec, width, height;
    uint64_t next_mapping_id;
} aw_dsession;

struct aw_engine {
    aw_session_cfg cfg;
    aw_dsession    sess[AW_MAX_SESSIONS];
    uint64_t       next_reply_id; /* host reply clerk: numbers from 100          */
};

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
    uint8_t buf[256];
    aw_builder b;
    aw_builder_init(&b, buf, sizeof buf);
    aw_build_header(&b, v->type, AW_FLAG_HOST_REPLY, v->uuid, v->reply_id);
    aw_put_bytes(&b, list_key, AW_VT_PLS, NULL, 0); /* =pls NULL (H4 fills dict) */
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
    aw_build_header(&b, v->type, AW_FLAG_HOST_REPLY, v->uuid, v->reply_id);
    aw_put_s32(&b, AW_KEY_ERR, AW_NOERR);  /* codec creation status (deferred to dsta) */
    aw_put_u32(&b, AW_KEY_VER, 6);
    aw_put_bytes(&b, AW_KEY_HIN, AW_VT_PLS, NULL, 0); /* {VTHostBuildVersion}: H4 */
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
    uint8_t buf[256];
    aw_builder b;
    aw_builder_init(&b, buf, sizeof buf);
    aw_build_header(&b, AW_MSG_DPBA, AW_FLAG_HOST_REQ, s->uuid, next_req_id(e));
    aw_put_bytes(&b, AW_KEY_VAL, AW_VT_PLS, NULL, 0); /* H4: the attributes dict */
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

static int32_t handle_dfrm(aw_engine *e, aw_dsession *s, const aw_msg_view *v)
{
    if (!s->started) {
        /* frame with no decode session: 311, nothing else emitted */
        emit_dfrm_reply(e, s, v, AW_ERR_BAD_DATA, 0);
        return AW_NOERR;
    }

    const uint8_t *sbf; size_t sbf_len;
    if (aw_get_bytes(v, AW_KEY_SBF, AW_VT_SBF, &sbf, &sbf_len) != AW_PARSE_OK)
        return AW_ERR_MALFORMED;     /* ,sbf required                             */

    const uint8_t *au; size_t au_len;
    if (aw_sbuf_read(sbf, sbf_len, &au, &au_len, NULL, NULL) < 0)
        return AW_ERR_MALFORMED;

    int64_t frm64 = 0;
    (void)aw_get_s64(v, AW_KEY_FRM, &frm64);
    uint32_t dfl = 0; aw_get_u32(v, AW_KEY_DFL, &dfl);
    uint32_t inf = (dfl & 1u) ? 0x1u : 0u; /* kVTDecodeInfo_Asynchronous          */

    /* 1. Ask the guest for an output buffer (Dbuf + its reply). */
    emit_dbuf(e, s, frm64);
    aw_out_buffer ob;
    memset(&ob, 0, sizeof ob);
    int got = e->cfg.get_buffer
                ? e->cfg.get_buffer(e->cfg.buf_ctx, s->width, s->height, &ob) : -1;

    /* 2. Async: the dfrm reply goes EARLY, before Demt. */
    emit_dfrm_reply(e, s, v, AW_NOERR, inf);

    if (got != 0) {                  /* no buffer: drop the frame                 */
        return AW_NOERR;
    }

    /* 3. Decode the access unit on the backend. */
    vtpv_decoded_frame f;
    memset(&f, 0, sizeof f);
    int dr = e->cfg.be->decode(s->dec, au, au_len, &f);
    if (dr != 0) {
        /* a frame that fails: Demt with -12909 and no buffer, Dfub, rels */
        emit_demt(e, s, frm64, AW_ERR_MALFUNCTION, inf, ob.uuid);
        emit_dfub(e, s, ob.uuid);
        emit_rels(e, ob.mapping_id);
        if (e->cfg.put_buffer) e->cfg.put_buffer(e->cfg.buf_ctx, &ob);
        return AW_NOERR;
    }

    /* 4. Copy the decoded NV12 into the lent buffer (between Dbuf reply and Demt). */
    int cr = e->cfg.copy_frame ? e->cfg.copy_frame(e->cfg.buf_ctx, &f, &ob) : -1;

    /* 5. Emit, then release. */
    emit_demt(e, s, frm64, cr == 0 ? AW_NOERR : AW_ERR_MALFUNCTION,
              inf | 1u /* kVTDecodeInfo_FrameDecoded tracking */, ob.uuid);
    emit_rels(e, ob.mapping_id);
    if (e->cfg.put_buffer) e->cfg.put_buffer(e->cfg.buf_ctx, &ob);
    return AW_NOERR;
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
