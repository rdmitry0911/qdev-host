/*
 * aw_wire.c - Apple paravirtual VideoToolbox on-wire codec. See aw_wire.h and
 * the frozen contract WIRE-CONTRACT-APPLE-VIDEOTOOLBOX.md. CLEAN-ROOM.
 * Lane L-VIDEO-IMPL, 2026-11-16.
 */
#include "aw_wire.h"

/* ---- builder --------------------------------------------------------- */

void aw_builder_init(aw_builder *b, uint8_t *buf, size_t cap)
{
    b->buf = buf;
    b->cap = cap;
    b->len = 0;
    b->overflow = 0;
}

/* Reserve n bytes at the tail; return a pointer or NULL (latching overflow). */
static uint8_t *aw_reserve(aw_builder *b, size_t n)
{
    if (b->overflow || n > b->cap - b->len) {
        b->overflow = 1;
        return NULL;
    }
    uint8_t *p = b->buf + b->len;
    b->len += n;
    return p;
}

int aw_build_header(aw_builder *b, uint32_t type, uint32_t flags,
                    const uint8_t uuid[16], uint64_t reply_id)
{
    uint8_t *p = aw_reserve(b, AW_HDR_SIZE);
    if (!p) return -1;
    aw_put_be32(p + 0x00, 0);            /* atom_size: patched by finalize */
    aw_put_be32(p + 0x04, AW_ATOM_VTPV);
    aw_put_be32(p + 0x08, type);
    aw_put_be32(p + 0x0c, flags);
    if (uuid) memcpy(p + 0x10, uuid, 16);
    else      memset(p + 0x10, 0, 16);
    aw_put_be64(p + 0x20, reply_id);
    aw_put_be64(p + 0x28, 0);
    return 0;
}

/* Emit a para header + reserve its payload; return the payload pointer. */
static uint8_t *aw_put_para(aw_builder *b, uint32_t key, uint32_t vtype,
                            size_t payload_len)
{
    uint8_t *p = aw_reserve(b, AW_PARA_HDR + payload_len);
    if (!p) return NULL;
    aw_put_be32(p + 0x00, (uint32_t)(AW_PARA_HDR + payload_len));
    aw_put_be32(p + 0x04, AW_ATOM_PARA);
    aw_put_be32(p + 0x08, key);
    aw_put_be32(p + 0x0c, vtype);
    return p + AW_PARA_HDR;
}

int aw_put_u32(aw_builder *b, uint32_t key, uint32_t val)
{
    uint8_t *p = aw_put_para(b, key, AW_VT_U32, 4);
    if (!p) return -1;
    aw_put_be32(p, val);
    return 0;
}

int aw_put_s32(aw_builder *b, uint32_t key, int32_t val)
{
    uint8_t *p = aw_put_para(b, key, AW_VT_S32, 4);
    if (!p) return -1;
    aw_put_be32(p, (uint32_t)val);
    return 0;
}

int aw_put_s64(aw_builder *b, uint32_t key, int64_t val)
{
    uint8_t *p = aw_put_para(b, key, AW_VT_S64, 8);
    if (!p) return -1;
    aw_put_be64(p, (uint64_t)val);
    return 0;
}

int aw_put_wxh(aw_builder *b, uint32_t key, int32_t w, int32_t h)
{
    uint8_t *p = aw_put_para(b, key, AW_VT_WXH, 8);
    if (!p) return -1;
    aw_put_be32(p + 0, (uint32_t)w);
    aw_put_be32(p + 4, (uint32_t)h);
    return 0;
}

int aw_put_tim(aw_builder *b, uint32_t key, int64_t value, int32_t timescale,
               uint32_t flags, int64_t epoch)
{
    uint8_t *p = aw_put_para(b, key, AW_VT_TIM, 24);
    if (!p) return -1;
    aw_put_be64(p + 0,  (uint64_t)value);
    aw_put_be32(p + 8,  (uint32_t)timescale);
    aw_put_be32(p + 12, flags);
    aw_put_be64(p + 16, (uint64_t)epoch);
    return 0;
}

int aw_put_uui(aw_builder *b, uint32_t key, const uint8_t uuid[16])
{
    uint8_t *p = aw_put_para(b, key, AW_VT_UUI, 16);
    if (!p) return -1;
    if (uuid) memcpy(p, uuid, 16);
    else      memset(p, 0, 16);
    return 0;
}

int aw_put_uiplus(aw_builder *b, uint32_t key, const uint8_t *uuids, unsigned n)
{
    uint8_t *p = aw_put_para(b, key, AW_VT_UIPLUS, (size_t)n * 16);
    if (!p) return -1;
    if (n) memcpy(p, uuids, (size_t)n * 16);
    return 0;
}

int aw_put_bytes(aw_builder *b, uint32_t key, uint32_t vtype,
                 const uint8_t *data, size_t len)
{
    uint8_t *p = aw_put_para(b, key, vtype, len);
    if (!p) return -1;
    if (len) memcpy(p, data, len);
    return 0;
}

size_t aw_build_finalize(aw_builder *b)
{
    if (b->overflow || b->len < AW_HDR_SIZE) return 0;
    /* The vtpv header is a 48-byte SIBLING atom; its atom_size field is the
     * header atom's own length (AW_HDR_SIZE), NOT the whole message. The para
     * atoms follow as siblings, and the total message length is carried by the
     * transport (the queue element / CMBlockBuffer length), not this field.
     *
     * Apple's guest builds it exactly this way: FigAtomWriterBeginAtom('vtpv')
     * + the 40 bytes of header fields + FigAtomWriterEndAtom() closes the atom
     * at 48, then each para is appended via its own BeginAtom('para')/EndAtom
     * (CreateMessageAndByteStream 0x7ff814d66210 / AppendUInt32 0x7ff8151582b7
     * in 26.2 x86_64h VideoToolbox). The guest's reply param scanner
     * (0x7ff814d66591, used by CopyCFPropertyList 0x7ff814d66730) walks the
     * reply from offset 0 and ADVANCES past each non-'para' atom by its
     * atom_size; if the vtpv atom_size were the whole message it would skip the
     * entire reply and never find ',dec', so CopyCFPropertyList returns a NULL
     * plist and no decoder registers (the live -12906 / "0 registered, no
     * error" symptom, L-GVA-SELFIX). Emitting 48 makes the scanner land on the
     * first para. Our own aw_parse is unaffected (it locates paras at the fixed
     * hdr_off + AW_HDR_SIZE and never trusted this field), which is why unit
     * tests and the dump_decs replay-parse passed despite the wrong value. */
    aw_put_be32(b->buf + 0x00, AW_HDR_SIZE);
    return b->len;
}

/* ---- reader ---------------------------------------------------------- */

int aw_parse(const uint8_t *msg, size_t len, aw_msg_view *v)
{
    if (!msg || len < 8) return AW_PARSE_BAD;

    /* Walk top-level atoms until the first vtpv (skip a leading subs, etc). */
    size_t off = 0;
    for (;;) {
        if (off + 8 > len) return AW_PARSE_BAD;
        uint32_t asz = aw_get_be32(msg + off);
        uint32_t fcc = aw_get_be32(msg + off + 4);
        if (asz < 8 || asz > len - off) return AW_PARSE_BAD;
        if (fcc == AW_ATOM_VTPV) {
            if (asz < AW_HDR_SIZE) return AW_PARSE_BAD;
            break;
        }
        off += asz;
    }

    const uint8_t *h = msg + off;
    v->data     = msg;
    v->len      = len;
    v->hdr_off  = off;
    v->para_off = off + AW_HDR_SIZE;
    v->type     = aw_get_be32(h + 0x08);
    v->flags    = aw_get_be32(h + 0x0c);
    memcpy(v->uuid, h + 0x10, 16);
    v->reply_id = aw_get_be64(h + 0x20);
    return AW_PARSE_OK;
}

int aw_find_para(const aw_msg_view *v, uint32_t key, uint32_t *vtype,
                 const uint8_t **payload, size_t *plen)
{
    size_t off = v->para_off;
    while (off + AW_PARA_HDR <= v->len) {
        uint32_t asz = aw_get_be32(v->data + off);
        uint32_t fcc = aw_get_be32(v->data + off + 4);
        if (asz < AW_PARA_HDR || asz > v->len - off) return AW_PARSE_BAD;
        if (fcc == AW_ATOM_PARA && aw_get_be32(v->data + off + 8) == key) {
            if (vtype)   *vtype   = aw_get_be32(v->data + off + 12);
            if (payload) *payload = v->data + off + AW_PARA_HDR;
            if (plen)    *plen    = asz - AW_PARA_HDR;
            return AW_PARSE_OK;
        }
        off += asz;
    }
    return AW_PARSE_ABSENT;
}

/* Common path for a fixed-width scalar getter: find the key, check the exact
 * value-type, check the payload length. */
static int aw_get_fixed(const aw_msg_view *v, uint32_t key, uint32_t want_vt,
                        size_t want_len, const uint8_t **payload)
{
    uint32_t vt;
    size_t plen;
    int r = aw_find_para(v, key, &vt, payload, &plen);
    if (r != AW_PARSE_OK) return r;             /* ABSENT or BAD */
    if (vt != want_vt)   return AW_PARSE_WRONGTYPE;
    if (plen != want_len) return AW_PARSE_SHORT;
    return AW_PARSE_OK;
}

int aw_get_u32(const aw_msg_view *v, uint32_t key, uint32_t *out)
{
    const uint8_t *p;
    int r = aw_get_fixed(v, key, AW_VT_U32, 4, &p);
    if (r == AW_PARSE_OK) *out = aw_get_be32(p);
    return r;
}

int aw_get_s32(const aw_msg_view *v, uint32_t key, int32_t *out)
{
    const uint8_t *p;
    int r = aw_get_fixed(v, key, AW_VT_S32, 4, &p);
    if (r == AW_PARSE_OK) *out = (int32_t)aw_get_be32(p);
    return r;
}

int aw_get_s64(const aw_msg_view *v, uint32_t key, int64_t *out)
{
    const uint8_t *p;
    int r = aw_get_fixed(v, key, AW_VT_S64, 8, &p);
    if (r == AW_PARSE_OK) *out = (int64_t)aw_get_be64(p);
    return r;
}

int aw_get_wxh(const aw_msg_view *v, uint32_t key, int32_t *w, int32_t *h)
{
    const uint8_t *p;
    int r = aw_get_fixed(v, key, AW_VT_WXH, 8, &p);
    if (r == AW_PARSE_OK) { *w = (int32_t)aw_get_be32(p); *h = (int32_t)aw_get_be32(p + 4); }
    return r;
}

int aw_get_uui(const aw_msg_view *v, uint32_t key, uint8_t out[16])
{
    const uint8_t *p;
    int r = aw_get_fixed(v, key, AW_VT_UUI, 16, &p);
    if (r == AW_PARSE_OK) memcpy(out, p, 16);
    return r;
}

int aw_get_bytes(const aw_msg_view *v, uint32_t key, uint32_t vtype,
                 const uint8_t **payload, size_t *plen)
{
    uint32_t vt;
    int r = aw_find_para(v, key, &vt, payload, plen);
    if (r != AW_PARSE_OK) return r;
    if (vt != vtype) return AW_PARSE_WRONGTYPE;
    return AW_PARSE_OK;
}

/* ---- subs / rels ----------------------------------------------------- */

size_t aw_subs_write(uint8_t *buf, size_t cap, const uint64_t ids[AW_SUBS_IDS])
{
    if (cap < AW_SUBS_SIZE) return 0;
    aw_put_be32(buf + 0x00, AW_SUBS_SIZE);
    aw_put_be32(buf + 0x04, AW_ATOM_SUBS);
    for (unsigned i = 0; i < AW_SUBS_IDS; i++)
        aw_put_be64(buf + 0x08 + i * 8, ids[i]);
    return AW_SUBS_SIZE;
}

int aw_subs_read(const uint8_t *buf, size_t len, uint64_t ids_out[AW_SUBS_IDS])
{
    if (len < AW_SUBS_SIZE) return AW_PARSE_BAD;
    if (aw_get_be32(buf + 0x00) != AW_SUBS_SIZE) return AW_PARSE_BAD;
    if (aw_get_be32(buf + 0x04) != AW_ATOM_SUBS) return AW_PARSE_BAD;
    for (unsigned i = 0; i < AW_SUBS_IDS; i++)
        ids_out[i] = aw_get_be64(buf + 0x08 + i * 8);
    return AW_PARSE_OK;
}

size_t aw_rels_write(uint8_t *buf, size_t cap, const uint64_t *ids, unsigned n)
{
    size_t box = AW_RELS_HDR + (size_t)n * 8;
    if (cap < box) return 0;
    aw_put_be32(buf + 0x00, (uint32_t)box);
    aw_put_be32(buf + 0x04, AW_ATOM_RELS);
    for (unsigned i = 0; i < n; i++)
        aw_put_be64(buf + AW_RELS_HDR + i * 8, ids[i]);
    return box;
}

int aw_rels_read(const uint8_t *buf, size_t len, uint64_t *ids_out,
                 unsigned max, unsigned *n_out)
{
    if (len < AW_RELS_HDR) return AW_PARSE_BAD;
    uint32_t box = aw_get_be32(buf + 0x00);
    if (aw_get_be32(buf + 0x04) != AW_ATOM_RELS) return AW_PARSE_BAD;
    if (box < AW_RELS_HDR || box > len) return AW_PARSE_BAD;
    if ((box - AW_RELS_HDR) % 8 != 0) return AW_PARSE_BAD;
    unsigned avail = (box - AW_RELS_HDR) / 8;
    unsigned out = 0;
    for (unsigned i = 0; i < avail && out < max; i++) {
        uint64_t id = aw_get_be64(buf + AW_RELS_HDR + i * 8);
        if (id != 0) ids_out[out++] = id; /* guest kext skips zero IDs */
    }
    *n_out = out;
    return AW_PARSE_OK;
}
