/*
 * vtpv_proto.c - the AppleGVA host codec-session protocol handler.
 * CLEAN-ROOM (see vtpv_proto.h). Increment 1, lane L-M2V03, 2026-11-16.
 */
#include "vtpv_proto.h"

#include <stdlib.h>
#include <string.h>

typedef struct vtpv_session {
    int      in_use;
    uint32_t id;
    uint32_t codec;
    void    *dec;   /* backend decoder handle */
} vtpv_session;

struct vtpv_proto {
    const vtpv_decoder_backend *be;
    void                       *be_ctx;
    vtpv_emit_fn                emit;
    void                       *emit_ctx;
    uint32_t                    next_id;
    vtpv_session                sessions[VTPV_MAX_SESSIONS];
};

/* A completion is at most the header plus the largest fixed payload or a
 * capped caps report. The caps report is the largest: header + N codecs. */
#define VTPV_MAX_CODECS 8
#define VTPV_CMPL_MAX   (sizeof(vtpv_cmpl_hdr) + sizeof(vtpv_caps) + \
                         VTPV_MAX_CODECS * sizeof(vtpv_codec_cap))

/* Build a completion header into buf and return the payload offset. */
static size_t cmpl_begin(uint8_t *buf, uint16_t type, uint32_t session,
                         uint64_t tag, int32_t status)
{
    vtpv_cmpl_hdr h;
    memset(&h, 0, sizeof(h));
    h.magic   = VTPV_CMPL_MAGIC;
    h.abi     = VTPV_ABI;
    h.type    = type;
    h.session = session;
    h.tag     = tag;
    h.status  = status;
    h.length  = (uint32_t)sizeof(h); /* finalized in cmpl_emit */
    memcpy(buf, &h, sizeof(h));
    return sizeof(h);
}

static void cmpl_emit(vtpv_proto *p, uint8_t *buf, size_t total)
{
    /* patch length */
    uint32_t len32 = (uint32_t)total;
    memcpy(buf + offsetof(vtpv_cmpl_hdr, length), &len32, sizeof(len32));
    p->emit(p->emit_ctx, buf, total);
}

/* Emit a bare status-only completion (no payload). */
static void emit_status(vtpv_proto *p, uint16_t type, uint32_t session,
                        uint64_t tag, int32_t status)
{
    uint8_t buf[sizeof(vtpv_cmpl_hdr)];
    size_t off = cmpl_begin(buf, type, session, tag, status);
    cmpl_emit(p, buf, off);
}

static vtpv_session *session_find(vtpv_proto *p, uint32_t id)
{
    unsigned i;
    if (id == 0)
        return NULL;
    for (i = 0; i < VTPV_MAX_SESSIONS; i++)
        if (p->sessions[i].in_use && p->sessions[i].id == id)
            return &p->sessions[i];
    return NULL;
}

static vtpv_session *session_alloc(vtpv_proto *p)
{
    unsigned i;
    for (i = 0; i < VTPV_MAX_SESSIONS; i++) {
        if (!p->sessions[i].in_use) {
            vtpv_session *s = &p->sessions[i];
            memset(s, 0, sizeof(*s));
            s->in_use = 1;
            s->id = ++p->next_id;
            if (s->id == 0) /* never hand out 0 */
                s->id = ++p->next_id;
            return s;
        }
    }
    return NULL;
}

/* ---- per-type handlers --------------------------------------------- */

static void do_query_caps(vtpv_proto *p, const vtpv_submit_hdr *h)
{
    uint8_t buf[VTPV_CMPL_MAX];
    size_t off = cmpl_begin(buf, h->type, 0, h->tag, VTPV_OK);
    vtpv_caps *caps = (vtpv_caps *)(buf + off);
    int n;

    if (!p->be || !p->be->query_caps) {
        emit_status(p, h->type, 0, h->tag, VTPV_ERR_UNSUPPORTED);
        return;
    }
    n = p->be->query_caps(p->be_ctx, caps, sizeof(buf) - off);
    if (n < 0) {
        emit_status(p, h->type, 0, h->tag, VTPV_ERR_BACKEND);
        return;
    }
    off += sizeof(vtpv_caps) + (size_t)n * sizeof(vtpv_codec_cap);
    cmpl_emit(p, buf, off);
}

static void do_session_create(vtpv_proto *p, const vtpv_submit_hdr *h,
                              const uint8_t *msg, size_t len)
{
    vtpv_create_req req;
    const uint8_t *extradata;
    vtpv_session *s;
    void *dec = NULL;
    int rc;
    uint8_t buf[sizeof(vtpv_cmpl_hdr) + sizeof(vtpv_create_desc)];
    size_t off;
    vtpv_create_desc *desc;

    if (len < sizeof(*h) + sizeof(req)) {
        emit_status(p, h->type, 0, h->tag, VTPV_ERR_MALFORMED);
        return;
    }
    memcpy(&req, msg + sizeof(*h), sizeof(req));

    if (req.codec != VTPV_CODEC_H264 && req.codec != VTPV_CODEC_HEVC) {
        emit_status(p, h->type, 0, h->tag, VTPV_ERR_UNSUPPORTED);
        return;
    }
    if (req.width == 0 || req.height == 0 ||
        req.width > VTPV_MAX_DIM || req.height > VTPV_MAX_DIM) {
        emit_status(p, h->type, 0, h->tag, VTPV_ERR_MALFORMED);
        return;
    }
    if (req.extradata_len > VTPV_MAX_EXTRADATA_BYTES ||
        len < sizeof(*h) + sizeof(req) + req.extradata_len) {
        emit_status(p, h->type, 0, h->tag,
                    req.extradata_len > VTPV_MAX_EXTRADATA_BYTES
                        ? VTPV_ERR_TOO_LARGE : VTPV_ERR_MALFORMED);
        return;
    }
    extradata = msg + sizeof(*h) + sizeof(req);

    s = session_alloc(p);
    if (!s) {
        emit_status(p, h->type, 0, h->tag, VTPV_ERR_NO_SLOTS);
        return;
    }
    if (!p->be || !p->be->open_decoder) {
        s->in_use = 0;
        emit_status(p, h->type, 0, h->tag, VTPV_ERR_UNSUPPORTED);
        return;
    }
    rc = p->be->open_decoder(p->be_ctx, req.codec, req.width, req.height,
                             extradata, req.extradata_len, &dec);
    if (rc != VTPV_OK) {
        s->in_use = 0;
        emit_status(p, h->type, 0, h->tag, rc);
        return;
    }
    s->codec = req.codec;
    s->dec = dec;

    off = cmpl_begin(buf, h->type, s->id, h->tag, VTPV_OK);
    desc = (vtpv_create_desc *)(buf + off);
    desc->session = s->id;
    off += sizeof(*desc);
    cmpl_emit(p, buf, off);
}

static void do_decode_frame(vtpv_proto *p, const vtpv_submit_hdr *h,
                            const uint8_t *msg, size_t len)
{
    vtpv_decode_req req;
    const uint8_t *sample;
    vtpv_session *s;
    vtpv_decoded_frame frame;
    int rc;
    uint8_t buf[sizeof(vtpv_cmpl_hdr) + sizeof(vtpv_decoded_desc)];
    size_t off;
    vtpv_decoded_desc *desc;

    if (len < sizeof(*h) + sizeof(req)) {
        emit_status(p, h->type, h->session, h->tag, VTPV_ERR_MALFORMED);
        return;
    }
    memcpy(&req, msg + sizeof(*h), sizeof(req));

    if (req.sample_len > VTPV_MAX_SAMPLE_BYTES) {
        emit_status(p, h->type, h->session, h->tag, VTPV_ERR_TOO_LARGE);
        return;
    }
    if (len < sizeof(*h) + sizeof(req) + req.sample_len) {
        emit_status(p, h->type, h->session, h->tag, VTPV_ERR_MALFORMED);
        return;
    }
    sample = msg + sizeof(*h) + sizeof(req);

    s = session_find(p, h->session);
    if (!s) {
        emit_status(p, h->type, h->session, h->tag, VTPV_ERR_NO_SESSION);
        return;
    }
    memset(&frame, 0, sizeof(frame));
    rc = p->be->decode(s->dec, sample, req.sample_len, &frame);
    if (rc != VTPV_OK) {
        emit_status(p, h->type, h->session, h->tag, rc);
        return;
    }
    off = cmpl_begin(buf, h->type, h->session, h->tag, VTPV_OK);
    desc = (vtpv_decoded_desc *)(buf + off);
    desc->pts        = req.pts;
    desc->surface_id = frame.surface_id;
    desc->width      = frame.width;
    desc->height     = frame.height;
    desc->pixfmt     = frame.pixfmt;
    desc->flags      = 0;
    off += sizeof(*desc);
    cmpl_emit(p, buf, off);
}

static void do_session_end(vtpv_proto *p, const vtpv_submit_hdr *h)
{
    vtpv_session *s = session_find(p, h->session);
    if (!s) {
        emit_status(p, h->type, h->session, h->tag, VTPV_ERR_NO_SESSION);
        return;
    }
    if (p->be && p->be->close_decoder)
        p->be->close_decoder(s->dec);
    s->in_use = 0;
    s->dec = NULL;
    emit_status(p, h->type, h->session, h->tag, VTPV_OK);
}

/* ---- public API ---------------------------------------------------- */

vtpv_proto *vtpv_proto_create(const vtpv_decoder_backend *be, void *be_ctx,
                              vtpv_emit_fn emit, void *emit_ctx)
{
    vtpv_proto *p;
    if (!emit)
        return NULL;
    p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->be = be;
    p->be_ctx = be_ctx;
    p->emit = emit;
    p->emit_ctx = emit_ctx;
    p->next_id = 0;
    return p;
}

int vtpv_proto_submit(vtpv_proto *p, const uint8_t *msg, size_t len)
{
    vtpv_submit_hdr h;

    if (!p || !msg || len < sizeof(h))
        return -1; /* too short to even carry a tag to answer on */

    memcpy(&h, msg, sizeof(h));
    if (h.magic != VTPV_SUBMIT_MAGIC || h.abi != VTPV_ABI)
        return -1; /* not ours / unknown ABI: no completion */

    /* h.length is the guest's claim; clamp dispatch to the bytes we got. */
    if (h.length < sizeof(h) || h.length > len) {
        emit_status(p, h.type, h.session, h.tag, VTPV_ERR_MALFORMED);
        return 0;
    }
    len = h.length;

    switch (h.type) {
    case VTPV_MSG_QUERY_CAPS:     do_query_caps(p, &h);            break;
    case VTPV_MSG_SESSION_CREATE: do_session_create(p, &h, msg, len); break;
    case VTPV_MSG_DECODE_FRAME:   do_decode_frame(p, &h, msg, len);   break;
    case VTPV_MSG_SESSION_END:    do_session_end(p, &h);           break;
    default:
        emit_status(p, h.type, h.session, h.tag, VTPV_ERR_UNSUPPORTED);
        break;
    }
    return 0;
}

unsigned vtpv_proto_open_sessions(const vtpv_proto *p)
{
    unsigned i, n = 0;
    if (!p)
        return 0;
    for (i = 0; i < VTPV_MAX_SESSIONS; i++)
        if (p->sessions[i].in_use)
            n++;
    return n;
}

void vtpv_proto_destroy(vtpv_proto *p)
{
    unsigned i;
    if (!p)
        return;
    for (i = 0; i < VTPV_MAX_SESSIONS; i++) {
        if (p->sessions[i].in_use && p->be && p->be->close_decoder)
            p->be->close_decoder(p->sessions[i].dec);
    }
    free(p);
}
