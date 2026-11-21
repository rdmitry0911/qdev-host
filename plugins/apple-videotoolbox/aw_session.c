/*
 * aw_session.c - the host Apple-wire VideoToolbox session engine. See aw_session.h,
 * the WIRE-CONTRACT doc and contract-B. CLEAN-ROOM. Lane L-VIDEO-IMPL, 2026-11-16.
 */
#include "aw_session.h"
#include "aw_sbuf.h"
#include "aw_bplist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AW_MAX_SESSIONS 16u

/* 'avc1'/'hvc1' on the wire == the backend's VTPV_CODEC_* values. */
#define AW_CODEC_AVC1 AW_4CC('a','v','c','1')
#define AW_CODEC_HVC1 AW_4CC('h','v','c','1')
#define AW_EXT_AVCC   AW_4CC('a','v','c','C')
#define AW_EXT_HVCC   AW_4CC('h','v','c','C')
#define AW_PIXFMT_420V AW_4CC('4','2','0','v')

/* a session's in-flight frame progresses: IDLE -> AWAIT_DBUF (Dbuf sent) ->
 * AWAIT_DEMT (frame copied, Demt sent) -> IDLE. One frame is in flight at a
 * time (frames are emitted in decode order, one Demt per dfrm, as Apple's host
 * decoder does); a dfrm that arrives while one is in flight is parked in a
 * FIFO and started when the previous frame completes. Its dfrm reply then goes
 * after its own Dbuf reply, as for the first (Apple's host holds the dfrm reply
 * the same way). */
enum { AW_FRM_IDLE = 0, AW_FRM_AWAIT_DBUF, AW_FRM_AWAIT_DEMT };

#define AW_PARK_MAX 16u
#define AW_INFO_ASYNC        0x1u /* kVTDecodeInfo_Asynchronous                 */
#define AW_INFO_FRAMEDROPPED 0x2u /* kVTDecodeInfo_FrameDropped                 */

typedef struct aw_parked {
    uint8_t  *au;
    size_t    au_len;
    int64_t   frm;
    uint32_t  inf;
    uint64_t  reply_id;
} aw_parked;

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

    /* dfrm units waiting for the in-flight frame to complete (FIFO)          */
    aw_parked    park[AW_PARK_MAX];
    unsigned     park_head, park_n;
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
 * one OSType NUMBER ('420v') — confirmed by L-GUEST-KEXT as the CoreVideo
 * kCVPixelBufferPixelFormatTypeKey convention (CFNumber); keep NUMBER unless the
 * shipping framework rejects it at dsta. */
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

/* ---- codec metadata (our IDs/names; VA-API reports no level, so Apple's
 * per-codec level is used, per devices.md "This host") ---------------------- */
static const char *codec_fourcc_str(uint32_t codec)
{
    return codec == AW_CODEC_AVC1 ? "avc1" : codec == AW_CODEC_HVC1 ? "hvc1" : NULL;
}
static const char *codec_id(uint32_t codec)
{
    return codec == AW_CODEC_AVC1 ? "applegva-video.videodecoder.avc"
         : codec == AW_CODEC_HVC1 ? "applegva-video.videodecoder.hevc" : NULL;
}
static const char *codec_name(uint32_t codec)
{
    return codec == AW_CODEC_AVC1 ? "AppleGVA Video Decoder - AVC"
         : codec == AW_CODEC_HVC1 ? "AppleGVA Video Decoder - HEVC" : NULL;
}
/* profiles (as the bitstream's profile_idc / HEVC profile) and the decode level
 * to advertise for each. */
static void codec_profiles(uint32_t codec, const int **profs, int *n, int *level)
{
    static const int avc[]  = { 66, 77, 100 };    /* Baseline, Main, High        */
    static const int hevc[] = { 1, 2, 3, 4 };
    if (codec == AW_CODEC_AVC1) { *profs = avc;  *n = 3; *level = 80;  }
    else if (codec == AW_CODEC_HVC1) { *profs = hevc; *n = 4; *level = 186; }
    else { *profs = NULL; *n = 0; *level = 0; }
}

/* VTPerProfileDetails { "<profile>": { VTMaxDecodeLevel: level } } and the
 * VTSupportedProfileArray, shared by ,dec and ,dca. */
static int build_profile_details(aw_bplist *b, const int *profs, int n, int level)
{
    int keys[8], vals[8];
    for (int i = 0; i < n && i < 8; i++) {
        char ps[16]; snprintf(ps, sizeof ps, "%d", profs[i]);
        int lk = aw_bpl_string(b, "VTMaxDecodeLevel");
        int lv = aw_bpl_int(b, level);
        keys[i] = aw_bpl_string(b, ps);
        vals[i] = aw_bpl_dict(b, &lk, &lv, 1);
    }
    return aw_bpl_dict(b, keys, vals, n);
}
static int build_profile_array(aw_bplist *b, const int *profs, int n)
{
    int refs[8];
    for (int i = 0; i < n && i < 8; i++) refs[i] = aw_bpl_int(b, profs[i]);
    return aw_bpl_array(b, refs, n);
}

/* One decoder-list entry: CMClassImplementationID/Name, IsPreferredInternalPlugin
 * false, VTCodecType [fourcc], VTDecoderCapabilities {VTPerProfileDetails,
 * VTSupportedProfileArray}, VTRating 400 (devices.md "What an entry holds"). */
static int build_decoder_entry(aw_bplist *b, uint32_t codec)
{
    const int *profs; int np, level;
    codec_profiles(codec, &profs, &np, &level);

    int cstr  = aw_bpl_string(b, codec_fourcc_str(codec));
    int vtct  = aw_bpl_array(b, &cstr, 1);
    int dck[2] = { aw_bpl_string(b, "VTPerProfileDetails"),
                   aw_bpl_string(b, "VTSupportedProfileArray") };
    int dcv[2] = { build_profile_details(b, profs, np, level),
                   build_profile_array(b, profs, np) };
    int dcaps = aw_bpl_dict(b, dck, dcv, 2);

    int ek[6] = { aw_bpl_string(b, "CMClassImplementationID"),
                  aw_bpl_string(b, "CMClassImplementationName"),
                  aw_bpl_string(b, "IsPreferredInternalPlugin"),
                  aw_bpl_string(b, "VTCodecType"),
                  aw_bpl_string(b, "VTDecoderCapabilities"),
                  aw_bpl_string(b, "VTRating") };
    int ev[6] = { aw_bpl_string(b, codec_id(codec)),
                  aw_bpl_string(b, codec_name(codec)),
                  aw_bpl_bool(b, 0),            /* IsPreferredInternalPlugin false */
                  vtct, dcaps, aw_bpl_int(b, 400) };
    return aw_bpl_dict(b, ek, ev, 6);
}

/* ,dec: an array of decoder entries, one per codec the backend decodes. */
static size_t build_decoder_list_plist(uint8_t *out, size_t cap,
                                       const uint32_t *codecs, int ncodec)
{
    aw_bplist *b = aw_bplist_new();
    if (!b) return 0;
    int entries[8];
    int ne = 0;
    for (int i = 0; i < ncodec && ne < 8; i++)
        if (codec_fourcc_str(codecs[i])) entries[ne++] = build_decoder_entry(b, codecs[i]);
    int root = aw_bpl_array(b, entries, ne);
    size_t n = (root >= 0) ? aw_bplist_finalize(b, root, out, cap) : 0;
    aw_bplist_free(b);
    return n;
}

/* ,dca for one codec: {VTIsHDRAllowedOnDevice true, VTPerProfileSupport
 * {profile: {VTIsHardwareAccelerated, VTMaxDecodeLevel, VTMaxPlaybackLevel}},
 * VTSupportedProfiles} (devices.md "dcap for avc1"). NULL plist for an
 * unsupported codec. */
static size_t build_dcap_plist(uint8_t *out, size_t cap, uint32_t codec)
{
    const int *profs; int np, level;
    codec_profiles(codec, &profs, &np, &level);
    if (!np) return 0;              /* unsupported codec -> NULL (=pls no bytes)  */

    aw_bplist *b = aw_bplist_new();
    if (!b) return 0;
    int pk[8], pv[8];
    for (int i = 0; i < np && i < 8; i++) {
        char ps[16]; snprintf(ps, sizeof ps, "%d", profs[i]);
        int k[3] = { aw_bpl_string(b, "VTIsHardwareAccelerated"),
                     aw_bpl_string(b, "VTMaxDecodeLevel"),
                     aw_bpl_string(b, "VTMaxPlaybackLevel") };
        int vv[3] = { aw_bpl_bool(b, 1), aw_bpl_int(b, level), aw_bpl_int(b, level) };
        pk[i] = aw_bpl_string(b, ps);
        pv[i] = aw_bpl_dict(b, k, vv, 3);
    }
    int per = aw_bpl_dict(b, pk, pv, np);
    int rk[3] = { aw_bpl_string(b, "VTIsHDRAllowedOnDevice"),
                  aw_bpl_string(b, "VTPerProfileSupport"),
                  aw_bpl_string(b, "VTSupportedProfiles") };
    int rv[3] = { aw_bpl_bool(b, 1), per, build_profile_array(b, profs, np) };
    int root = aw_bpl_dict(b, rk, rv, 3);
    size_t n = (root >= 0) ? aw_bplist_finalize(b, root, out, cap) : 0;
    aw_bplist_free(b);
    return n;
}

/* Enumerate the codecs the backend decodes into `out` (up to `max`) from the
 * live VA-API caps; returns the count. */
static int engine_decode_codecs(aw_engine *e, uint32_t *out, int max)
{
    if (!e->cfg.be || !e->cfg.be->query_caps) return 0;
    uint8_t buf[256];
    vtpv_caps *caps = (vtpv_caps *)buf;
    if (e->cfg.be->query_caps(e->cfg.be_ctx, caps, sizeof buf) < 0) return 0;
    const vtpv_codec_cap *cc = (const vtpv_codec_cap *)(buf + sizeof(vtpv_caps));
    int n = 0;
    for (uint16_t i = 0; i < caps->n_codecs && n < max; i++)
        if (cc[i].can_decode) out[n++] = cc[i].codec;
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
    for (unsigned i = 0; i < s->park_n; i++)
        free(s->park[(s->park_head + i) % AW_PARK_MAX].au);
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
/* The ,ver to reply with: echo the request's ,ver (the protocol version the
 * guest speaks), defaulting to 6. Apple's 26.7 host replies a fixed 6, but the
 * 26.2 guest sends ,ver 5; echoing avoids any "host ver must be <= guest ver"
 * gate in the guest's registration path. */
static uint32_t reply_ver(const aw_msg_view *v)
{
    uint32_t ver;
    if (aw_get_u32(v, AW_KEY_VER, &ver) == AW_PARSE_OK) return ver;
    return 6;
}

/* Emit a list-style reply: a single `=pls` list/dict body under `list_key`, plus
 * ,ver. `pls`/`pls_len` may be an empty list or NULL (pls_len 0). */
static void emit_list_reply(aw_engine *e, const aw_msg_view *v, uint32_t list_key,
                            const uint8_t *pls, size_t pls_len)
{
    uint8_t buf[2048];               /* a decoder list is a few hundred bytes      */
    aw_builder b;
    aw_builder_init(&b, buf, sizeof buf);
    aw_build_header(&b, v->type, AW_FLAG_HOST_REPLY, v->uuid, v->reply_id);
    aw_put_bytes(&b, list_key, AW_VT_PLS, pls, pls_len);
    aw_put_u32(&b, AW_KEY_VER, reply_ver(v));
    size_t n = aw_build_finalize(&b);
    if (n) emit(e, buf, n);
}

/* decs: ,dec = the decoder entries built from the live VA-API caps, + ,ver 6. */
static int32_t handle_decs(aw_engine *e, const aw_msg_view *v)
{
    uint32_t codecs[8];
    int nc = engine_decode_codecs(e, codecs, 8);
    uint8_t pls[1536];
    size_t pls_len = build_decoder_list_plist(pls, sizeof pls, codecs, nc);
    emit_list_reply(e, v, AW_KEY_DEC, pls, pls_len);
    return AW_NOERR;
}

/* encs: no encoders yet -> ,enc empty list + ,ver 6. */
static int32_t handle_encs(aw_engine *e, const aw_msg_view *v)
{
    uint8_t pls[64];
    size_t pls_len = build_decoder_list_plist(pls, sizeof pls, NULL, 0); /* [] */
    emit_list_reply(e, v, AW_KEY_ENC, pls, pls_len);
    return AW_NOERR;
}

/* dcap: ,dca = the capabilities of the decoder for ,cod (=s32), NULL for a
 * codec the backend lacks, + ,ver 6. */
static int32_t handle_dcap(aw_engine *e, const aw_msg_view *v)
{
    int32_t cod = 0;
    int r = aw_get_s32(v, AW_KEY_COD, &cod);
    if (r == AW_PARSE_WRONGTYPE || r == AW_PARSE_SHORT)
        return AW_ERR_MALFORMED;     /* ,cod present with wrong type: -19093, no reply */

    /* only advertise caps for a codec the backend actually decodes */
    uint32_t codecs[8];
    int nc = engine_decode_codecs(e, codecs, 8), served = 0;
    for (int i = 0; i < nc; i++) if (codecs[i] == (uint32_t)cod) served = 1;

    uint8_t pls[1024];
    size_t pls_len = served ? build_dcap_plist(pls, sizeof pls, (uint32_t)cod) : 0;
    emit_list_reply(e, v, AW_KEY_DCA, pls, pls_len);  /* NULL =pls when unserved   */
    return AW_NOERR;
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
    aw_put_u32(&b, AW_KEY_VER, reply_ver(v));
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
    aw_put_bytes(&b, AW_KEY_PBA, AW_VT_PLS, pls, pls_len); /* ,pba =pls (reference: guest VT Dpba handler) */
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
    if (uuid) aw_put_uui(&b, AW_KEY_UUI, uuid); /* no ,uui = no buffer (dropped/failed) */
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

/* Start a frame: make it the in-flight one and ask the guest for a buffer. */
static void start_frame(aw_engine *e, aw_dsession *s, const uint8_t *au, size_t au_len,
                        int64_t frm, uint32_t inf, uint64_t reply_id)
{
    s->pend_au_len = au_len;
    if (au != s->pend_au) memcpy(s->pend_au, au, au_len);
    s->pend_frm = frm;
    s->pend_inf = inf;
    s->pend_dfrm_reply_id = reply_id;
    s->pend_state = AW_FRM_AWAIT_DBUF;
    emit_dbuf(e, s, frm);            /* -> guest replies Dbuf with a lent buffer   */
}

static int reserve_au(aw_dsession *s, size_t au_len)
{
    if (s->pend_au_cap < au_len) {
        uint8_t *n = realloc(s->pend_au, au_len);
        if (!n) return -1;
        s->pend_au = n; s->pend_au_cap = au_len;
    }
    return 0;
}

/* The in-flight frame completed: start the oldest parked unit, if any. */
static void start_next(aw_engine *e, aw_dsession *s)
{
    if (s->pend_state != AW_FRM_IDLE || s->park_n == 0) return;
    aw_parked *p = &s->park[s->park_head];
    s->park_head = (s->park_head + 1) % AW_PARK_MAX;
    s->park_n--;
    if (reserve_au(s, p->au_len) == 0) {
        memcpy(s->pend_au, p->au, p->au_len);
        start_frame(e, s, s->pend_au, p->au_len, p->frm, p->inf, p->reply_id);
    }
    free(p->au);
    p->au = NULL;
}

/* dfrm: park the access unit, emit Dbuf, and RETURN. The decode continues when
 * the guest's Dbuf reply arrives (handle_guest_reply). Async-only; one frame in
 * flight per session, later units wait in the FIFO. */
static int32_t handle_dfrm(aw_engine *e, aw_dsession *s, const aw_msg_view *v)
{
    if (!s->started) {
        emit_dfrm_reply(e, s, v, AW_ERR_BAD_DATA, 0);   /* 311, nothing else       */
        return AW_NOERR;
    }

    const uint8_t *sbf; size_t sbf_len;
    if (aw_get_bytes(v, AW_KEY_SBF, AW_VT_SBF, &sbf, &sbf_len) != AW_PARSE_OK)
        return AW_ERR_MALFORMED;     /* ,sbf required                             */
    const uint8_t *au; size_t au_len;
    if (aw_sbuf_read(sbf, sbf_len, &au, &au_len, NULL, NULL) < 0)
        return AW_ERR_MALFORMED;

    int64_t frm64 = 0; (void)aw_get_s64(v, AW_KEY_FRM, &frm64);
    uint32_t dfl = 0; aw_get_u32(v, AW_KEY_DFL, &dfl);
    uint32_t inf = (dfl & 1u) ? AW_INFO_ASYNC : 0u;

    if (s->pend_state != AW_FRM_IDLE) {
        /* a frame is in flight: park this unit until it completes            */
        if (s->park_n >= AW_PARK_MAX)
            return AW_ERR_MALFUNCTION;
        aw_parked *p = &s->park[(s->park_head + s->park_n) % AW_PARK_MAX];
        p->au = malloc(au_len ? au_len : 1);
        if (!p->au) return AW_ERR_MALFUNCTION;
        memcpy(p->au, au, au_len);
        p->au_len = au_len; p->frm = frm64; p->inf = inf; p->reply_id = v->reply_id;
        s->park_n++;
        return AW_NOERR;
    }

    /* keep the AU bytes until the Dbuf reply lets us decode into a lent buffer */
    if (reserve_au(s, au_len) != 0) return AW_ERR_MALFUNCTION;
    memcpy(s->pend_au, au, au_len);
    start_frame(e, s, s->pend_au, au_len, frm64, inf, v->reply_id);
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
            start_next(e, s);
            return AW_NOERR;
        }

        vtpv_decoded_frame f; memset(&f, 0, sizeof f);
        int dr = e->cfg.be->decode(s->dec, s->pend_au, s->pend_au_len, &f);
        if (dr != 0) {
            /* Apple's host: a sample with no picture is emitted with
             * FrameDropped and no buffer; a frame that does not decode with the
             * decoder's status and no buffer; the unused buffer goes back with
             * Dfub, then rels. */
            if (dr == VTPV_DROPPED)
                emit_demt(e, s, s->pend_frm, AW_NOERR, s->pend_inf | AW_INFO_FRAMEDROPPED, NULL);
            else
                emit_demt(e, s, s->pend_frm, AW_ERR_MALFUNCTION, s->pend_inf, NULL);
            emit_dfub(e, s, ob.uuid);
            emit_rels(e, ob.mapping_id);
            if (e->cfg.put_buffer) e->cfg.put_buffer(e->cfg.buf_ctx, &ob);
            s->pend_state = AW_FRM_IDLE;
            start_next(e, s);
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
        start_next(e, s);
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
    case AW_MSG_DECS: return handle_decs(e, &v);
    case AW_MSG_ENCS: return handle_encs(e, &v);
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
    case AW_MSG_DFIN: return reply_err(e, &v, AW_NOERR); /* finish/flush: frames
                                        complete synchronously on the Dbuf reply,
                                        so a drain has nothing outstanding -> noErr */
    case AW_MSG_DSUP: return reply_err(e, &v, AW_NOERR);
    case AW_MSG_DCOP:
    case AW_MSG_DSET: return reply_err(e, &v, AW_NOERR);
    default:          return AW_ERR_UNKNOWN_UUID;
    }
}
