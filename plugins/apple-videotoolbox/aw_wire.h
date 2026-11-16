/*
 * aw_wire.h - Apple paravirtual VideoToolbox on-wire protocol codec
 * (PCI 106b:1a03). Parse/emit the EXACT Apple wire: the 48-byte big-endian
 * `vtpv` header, `para` parameters with Apple's scan/length rules, every
 * value type the H.264 async-decode MVP needs, fragments, and the `subs`/`rels`
 * IOSurface-mapping boxes.
 *
 * Lane L-VIDEO-IMPL, 2026-11-16. This replaces the old clean-room invented
 * `vtpv` (vtpv_proto.h, "VTPV" magic + flat structs) with Apple's real wire.
 *
 * CLEAN-ROOM. The AUTHORITATIVE upstream is the guest-kext lane's closed
 * contract-B spec, cross-witnessed from (1) disassembly of Apple's arm64e
 * KDK_26.2 AppleVideoToolboxParavirtualization.kext and (2) the reims RE docs:
 *   worktrees/av-applegva-kext-20261116/AppleVideoToolboxParavirtualization/
 *     analysis/CONTRACT-B-APPLE-VT-WIRE-20261116.md   (the wire)
 *     analysis/B1-REAL-USERCLIENT-ABI-20261116.md     (the userclient ABI)
 * This header is the HOST-SIDE implementation of that contract-B, byte-identical
 * to it; the host companion doc with the frozen offsets + Dpba/sbuf detail is
 * evidence/runs/applegva-realproto-20261116/WIRE-CONTRACT-APPLE-VIDEOTOOLBOX.md.
 * NO reims source copied (GPL/LGPL); the wire is Apple's, reims merely documents
 * it. NB: the guest worktree's OLD core/vtpv_*.{c,h} + include/vtpv_wire.h codec
 * is the superseded contract-A invention (VTPV/VTPC magics, numbered ops, native
 * LE) and is NOT this wire - do not build the host plugin against it.
 *
 * ENDIANNESS (see the contract doc, §0): the `vtpv`/`para` framing, every para
 * payload scalar, `=fds`, and the `subs`/`rels` boxes and their IDs are ALL
 * big-endian. Only the queue-0 inline i32 status and the scalars *inside* an
 * `sbuf` atom are native (little-endian on x86_64). This module handles the
 * big-endian wire; `sbuf` native scalars are the sample-buffer reader's job.
 */
#ifndef AW_WIRE_H
#define AW_WIRE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- FourCC helper --------------------------------------------------- */
/* A FourCC is stored big-endian on the wire. AW_4CC('v','t','p','v') packs it
 * into a host uint32 as 0x76747076 so that a big-endian store writes 'v','t',
 * 'p','v' in order. */
#define AW_4CC(a,b,c,d) (((uint32_t)(uint8_t)(a) << 24) | \
                         ((uint32_t)(uint8_t)(b) << 16) | \
                         ((uint32_t)(uint8_t)(c) <<  8) | \
                         ((uint32_t)(uint8_t)(d)))

/* ---- atom / message / value-type / parameter FourCCs ----------------- */

#define AW_ATOM_VTPV  AW_4CC('v','t','p','v')
#define AW_ATOM_PARA  AW_4CC('p','a','r','a')
#define AW_ATOM_SUBS  AW_4CC('s','u','b','s')
#define AW_ATOM_RELS  AW_4CC('r','e','l','s')
#define AW_ATOM_SBUF  AW_4CC('s','b','u','f')

/* host-session + decoder-session message types (decode MVP subset) */
#define AW_MSG_DECS   AW_4CC('d','e','c','s') /* list decoders              */
#define AW_MSG_DCAP   AW_4CC('d','c','a','p') /* decoder capabilities       */
#define AW_MSG_ENCS   AW_4CC('e','n','c','s') /* list encoders              */
#define AW_MSG_PDEC   AW_4CC('+','d','e','c') /* open decoder session       */
#define AW_MSG_MDEC   AW_4CC('-','d','e','c') /* close decoder session      */
#define AW_MSG_MUUI   AW_4CC('-','u','u','i') /* close by UUID              */
#define AW_MSG_MALL   AW_4CC('-','a','l','l') /* close all (customer: noop) */
#define AW_MSG_DSUP   AW_4CC('d','s','u','p') /* supported properties       */
#define AW_MSG_DCOP   AW_4CC('d','c','o','p') /* get property               */
#define AW_MSG_DSET   AW_4CC('d','s','e','t') /* set property               */
#define AW_MSG_DSTA   AW_4CC('d','s','t','a') /* start decode session       */
#define AW_MSG_DFRM   AW_4CC('d','f','r','m') /* decode frame               */
#define AW_MSG_DFIN   AW_4CC('d','f','i','n') /* finish / flush             */
#define AW_MSG_DINV   AW_4CC('d','i','n','v') /* invalidate                 */
#define AW_MSG_DCAN   AW_4CC('d','c','a','n') /* can-accept probe           */

/* host->guest decoder requests (queue 1, flag AW_FLAG_HOST_REQ).
 * Dpba/Dbuf/Demt/Dfub + Bye! drive the decode MVP; the rest are contract-B §5
 * host->guest decoder messages named for the shared guest<->host catalog (tile/
 * display/colour paths), carried but not yet acted on by the decode MVP. */
#define AW_MSG_DPBA   AW_4CC('D','p','b','a') /* output buffer attributes   */
#define AW_MSG_DBUF   AW_4CC('D','b','u','f') /* ask guest for a buffer     */
#define AW_MSG_DEMT   AW_4CC('D','e','m','t') /* emit frame into buffer     */
#define AW_MSG_DFUB   AW_4CC('D','f','u','b') /* buffer died unemitted      */
#define AW_MSG_DDPA   AW_4CC('D','d','p','a') /* (contract-B §5, later)      */
#define AW_MSG_DRPA   AW_4CC('D','r','p','a') /* (contract-B §5, later)      */
#define AW_MSG_DMIF   AW_4CC('D','m','i','f') /* (contract-B §5, later)      */
#define AW_MSG_TDSR   AW_4CC('T','D','s','r') /* (contract-B §5, tiles)      */
#define AW_MSG_TDEM   AW_4CC('T','D','e','m') /* (contract-B §5, tiles)      */
#define AW_MSG_BYE    AW_4CC('B','y','e','!') /* terminate-all              */

/* reply + parameter keys (leading comma is literal byte 0x2c) */
#define AW_KEY_DEC    AW_4CC(',','d','e','c')
#define AW_KEY_DCA    AW_4CC(',','d','c','a')
#define AW_KEY_ENC    AW_4CC(',','e','n','c')
#define AW_KEY_VER    AW_4CC(',','v','e','r')
#define AW_KEY_HIN    AW_4CC(',','h','i','n')
#define AW_KEY_ERR    AW_4CC(',','e','r','r')
#define AW_KEY_SUP    AW_4CC(',','s','u','p')
#define AW_KEY_KEY    AW_4CC(',','k','e','y')
#define AW_KEY_VAL    AW_4CC(',','v','a','l')
#define AW_KEY_FDS    AW_4CC(',','f','d','s')
#define AW_KEY_FRM    AW_4CC(',','f','r','m')
#define AW_KEY_SBF    AW_4CC(',','s','b','f')
#define AW_KEY_DFL    AW_4CC(',','d','f','l')
#define AW_KEY_FOP    AW_4CC(',','f','o','p')
#define AW_KEY_INF    AW_4CC(',','i','n','f')
#define AW_KEY_UUI    AW_4CC(',','u','u','i')
#define AW_KEY_UIPLUS AW_4CC(',','u','i','+')
#define AW_KEY_POP    AW_4CC(',','p','o','p')
#define AW_KEY_CAN    AW_4CC(',','c','a','n')
#define AW_KEY_COD    AW_4CC(',','c','o','d')
#define AW_KEY_CID    AW_4CC(',','c','i','d')
#define AW_KEY_GIN    AW_4CC(',','g','i','n')
#define AW_KEY_BLU    AW_4CC(',','b','l','u')

/* value types (leading '=' is byte 0x3d) */
#define AW_VT_U32     AW_4CC('=','u','3','2')
#define AW_VT_S32     AW_4CC('=','s','3','2')
#define AW_VT_S64     AW_4CC('=','s','6','4')
#define AW_VT_TIM     AW_4CC('=','t','i','m')
#define AW_VT_WXH     AW_4CC('=','w','x','h')
#define AW_VT_32P     AW_4CC('=','3','2','p')
#define AW_VT_32S     AW_4CC('=','3','2','s')
#define AW_VT_UUI     AW_4CC('=','u','u','i')
#define AW_VT_UIPLUS  AW_4CC('=','u','i','+')
#define AW_VT_PLS     AW_4CC('=','p','l','s')
#define AW_VT_DAT     AW_4CC('=','d','a','t')
#define AW_VT_FDS     AW_4CC('=','f','d','s')
#define AW_VT_SBF     AW_4CC('=','s','b','f')

/* ---- flags ----------------------------------------------------------- */
#define AW_FLAG_GUEST_REQ    0x1u  /* guest request awaiting host reply     */
#define AW_FLAG_HOST_REQ     0x2u  /* host request awaiting guest reply      */
#define AW_FLAG_HOST_REPLY   0x4u  /* host's reply                          */
#define AW_FLAG_GUEST_REPLY  0x8u  /* guest's reply                         */
#define AW_FLAG_MORE_FRAGS   0x10u /* more fragments follow                 */

/* ---- status codes (the device's i32 answer / ,err) ------------------- */
enum {
    AW_NOERR               =      0,
    AW_ERR_BAD_DATA        =    311, /* dfrm with no decode session         */
    AW_ERR_OVER_16MIB      = -12904, /* queue-0 readable part over 16 MiB   */
    AW_ERR_MALFUNCTION     = -12909, /* second dsta; failed/dropped frame   */
    AW_ERR_BAD_NAL_LEN     = -12910, /* 3-byte NAL length size              */
    AW_ERR_UNSUPPORTED_FMT = -12911, /* dsta refused (no Dpba)              */
    AW_ERR_UNIMPLEMENTED   = -12782, /* dfin/dsav/dmul/dtfi/dsts/dtil       */
    AW_ERR_REF_MISSING     = -17694, /* dropped RASL picture (HEVC)         */
    AW_ERR_TIMEOUT         = -19092, /* reply timeout                       */
    AW_ERR_MALFORMED       = -19093, /* bad param / reply-to-none / 2nd +dec*/
    AW_ERR_UNKNOWN_UUID    = -19094, /* message for an unknown UUID         */
    AW_ERR_INVALIDATED     = -19098, /* message after invalidate            */
};

/* ---- fixed sizes ----------------------------------------------------- */
#define AW_HDR_SIZE   48u   /* the vtpv header atom, exactly               */
#define AW_PARA_HDR   16u   /* para framing before the payload             */
#define AW_SUBS_SIZE  0x30u /* the subs box, exactly                       */
#define AW_SUBS_IDS   5u    /* subs carries exactly five u64 mapping IDs   */
#define AW_RELS_HDR   8u    /* rels box header (size + 'rels')             */
#define AW_MSG_FRAG_MAX 0x200000u /* the guest decoder's fragment cut size */

/* ---- struct layouts (the byte-exact contract; offset-pinned in tests) -
 * These document the wire layout and are checked with offsetof/sizeof in
 * test_aw_wire.c. Serialization below is byte-accurate and does NOT depend on
 * the host's struct packing or endianness - the structs are the contract, the
 * functions are the codec. */
#pragma pack(push, 1)
typedef struct aw_hdr {       /* 48 bytes; all fields big-endian on the wire */
    uint32_t atom_size;       /* 0x00 total message length (header included) */
    uint32_t vtpv;            /* 0x04 AW_ATOM_VTPV                           */
    uint32_t type;            /* 0x08 message-type FourCC                    */
    uint32_t flags;           /* 0x0c AW_FLAG_*                              */
    uint8_t  uuid[16];        /* 0x10 session UUID (opaque)                  */
    uint64_t reply_id;        /* 0x20 reply identifier                       */
    uint64_t reserved;        /* 0x28 must be 0                              */
} aw_hdr;

typedef struct aw_para {      /* 16-byte header; payload follows            */
    uint32_t atom_size;       /* 0x00 16 + payload length                   */
    uint32_t para;            /* 0x04 AW_ATOM_PARA                          */
    uint32_t key;             /* 0x08 parameter key FourCC                  */
    uint32_t vtype;           /* 0x0c value-type FourCC                     */
} aw_para;

typedef struct aw_subs {      /* 48 bytes; all big-endian                   */
    uint32_t atom_size;       /* 0x00 = AW_SUBS_SIZE                        */
    uint32_t subs;            /* 0x04 AW_ATOM_SUBS                          */
    uint64_t ids[AW_SUBS_IDS];/* 0x08 five mapping IDs, used first, rest 0  */
} aw_subs;
#pragma pack(pop)

/* ---- big-endian byte access (endianness-independent) ----------------- */
static inline void aw_put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
static inline void aw_put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static inline void aw_put_be64(uint8_t *p, uint64_t v) {
    aw_put_be32(p, (uint32_t)(v >> 32)); aw_put_be32(p + 4, (uint32_t)v);
}
static inline uint16_t aw_get_be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static inline uint32_t aw_get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
static inline uint64_t aw_get_be64(const uint8_t *p) {
    return ((uint64_t)aw_get_be32(p) << 32) | aw_get_be32(p + 4);
}

/* ---- message builder ------------------------------------------------- *
 * Appends a vtpv header then para parameters into a caller buffer. On any
 * overflow the builder latches `overflow` and finalize returns 0. */
typedef struct aw_builder {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    int      overflow;
} aw_builder;

void aw_builder_init(aw_builder *b, uint8_t *buf, size_t cap);

/* Write the 48-byte header. uuid may be NULL (treated as all-zero). */
int aw_build_header(aw_builder *b, uint32_t type, uint32_t flags,
                    const uint8_t uuid[16], uint64_t reply_id);

/* Typed para emitters (big-endian payloads). Return 0 ok, <0 on overflow. */
int aw_put_u32(aw_builder *b, uint32_t key, uint32_t val);      /* =u32 */
int aw_put_s32(aw_builder *b, uint32_t key, int32_t val);       /* =s32 */
int aw_put_s64(aw_builder *b, uint32_t key, int64_t val);       /* =s64 */
int aw_put_wxh(aw_builder *b, uint32_t key, int32_t w, int32_t h); /* =wxh */
int aw_put_tim(aw_builder *b, uint32_t key, int64_t value, int32_t timescale,
               uint32_t flags, int64_t epoch);                  /* =tim */
int aw_put_uui(aw_builder *b, uint32_t key, const uint8_t uuid[16]); /* =uui */
int aw_put_uiplus(aw_builder *b, uint32_t key, const uint8_t *uuids,
                  unsigned n);                                  /* =ui+ */
/* Generic byte-payload para (for =pls / =dat / =fds / =sbf). data may be NULL
 * with len 0 (a =pls NULL). */
int aw_put_bytes(aw_builder *b, uint32_t key, uint32_t vtype,
                 const uint8_t *data, size_t len);

/* Patch the header atom_size to the final length and return it, or 0 on
 * overflow / if no header was written. */
size_t aw_build_finalize(aw_builder *b);

/* ---- message reader -------------------------------------------------- */

typedef struct aw_msg_view {
    const uint8_t *data;
    size_t   len;
    size_t   hdr_off;    /* offset of the vtpv atom (after any subs)        */
    size_t   para_off;   /* offset of the first para (hdr_off + 48)         */
    uint32_t type;
    uint32_t flags;
    uint8_t  uuid[16];
    uint64_t reply_id;
} aw_msg_view;

/* Parse results. */
enum {
    AW_PARSE_OK        =  0,
    AW_PARSE_BAD       = -1, /* structurally malformed (no valid vtpv atom)  */
    AW_PARSE_WRONGTYPE = -2, /* key present with the wrong value-type        */
    AW_PARSE_SHORT     = -3, /* value-type length check failed               */
    AW_PARSE_ABSENT    = -4, /* parameter not present (reads as default)     */
};

/* Locate the vtpv header (skipping a leading subs box), validate it, decode
 * the header fields. Returns AW_PARSE_OK or AW_PARSE_BAD. */
int aw_parse(const uint8_t *msg, size_t len, aw_msg_view *v);

/* Find the first para with `key`. On AW_PARSE_OK, the vtype/payload/plen
 * outputs are set. Returns AW_PARSE_ABSENT if no such key, AW_PARSE_BAD on a
 * corrupt atom. */
int aw_find_para(const aw_msg_view *v, uint32_t key, uint32_t *vtype,
                 const uint8_t **payload, size_t *plen);

/* Typed getters. Each returns AW_PARSE_OK (and sets *out), AW_PARSE_ABSENT,
 * AW_PARSE_WRONGTYPE, or AW_PARSE_SHORT, per Apple's scan rules. */
int aw_get_u32(const aw_msg_view *v, uint32_t key, uint32_t *out);
int aw_get_s32(const aw_msg_view *v, uint32_t key, int32_t *out);
int aw_get_s64(const aw_msg_view *v, uint32_t key, int64_t *out);
int aw_get_wxh(const aw_msg_view *v, uint32_t key, int32_t *w, int32_t *h);
int aw_get_uui(const aw_msg_view *v, uint32_t key, uint8_t out[16]);
/* Bytes payload (=pls/=dat/=fds/=sbf): returns the payload pointer+len, or
 * AW_PARSE_ABSENT. Does not copy. */
int aw_get_bytes(const aw_msg_view *v, uint32_t key, uint32_t vtype,
                 const uint8_t **payload, size_t *plen);

/* True if this message has AW_FLAG_MORE_FRAGS set. */
static inline int aw_more_fragments(const aw_msg_view *v) {
    return (v->flags & AW_FLAG_MORE_FRAGS) != 0;
}

/* ---- subs / rels boxes ----------------------------------------------- */

/* Write a subs box (exactly AW_SUBS_SIZE bytes) into buf. ids[] is five
 * entries, used first, rest 0. Returns AW_SUBS_SIZE or 0 if cap too small. */
size_t aw_subs_write(uint8_t *buf, size_t cap, const uint64_t ids[AW_SUBS_IDS]);

/* Read a subs box from the front of a message. Returns AW_PARSE_OK and fills
 * ids_out[5], or AW_PARSE_BAD (wrong size / not 'subs'): per Apple, a malformed
 * subs means the message is delivered with no surfaces. */
int aw_subs_read(const uint8_t *buf, size_t len, uint64_t ids_out[AW_SUBS_IDS]);

/* Write a rels box: [u32 8+8n]['rels'][n big-endian u64]. Sent alone, not in a
 * vtpv message. Returns the box length (8 + 8n) or 0 on overflow. */
size_t aw_rels_write(uint8_t *buf, size_t cap, const uint64_t *ids, unsigned n);

/* Read a rels box. Fills up to `max` ids and sets *n_out to the count present
 * (skipping zero IDs, per the guest kext's rule). Returns AW_PARSE_OK or
 * AW_PARSE_BAD. */
int aw_rels_read(const uint8_t *buf, size_t len, uint64_t *ids_out,
                 unsigned max, unsigned *n_out);

#ifdef __cplusplus
}
#endif

#endif /* AW_WIRE_H */
