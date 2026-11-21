/*
 * vtpv_proto.h - host-side codec-session protocol handler for the Apple
 * paravirtualized VideoToolbox device (PCI 106b:1a03).
 *
 * AppleGVA host backend, increment 1 (lane L-M2V03, 2026-11-16).
 *
 * CLEAN-ROOM: this is an original design written from the device/ABI
 * contract (analysis/20261115-APPLEGVA-VIDEO-HOST-CONTRACT.md, §7 F0-GVA
 * verdict) and the H.264/HEVC/VAAPI specifications. It does NOT copy, port,
 * or transcribe any code from the reims reference under research/reims/ (GPL/LGPL). The reims docs
 * were read for the device *contract* (what Apple's host does) only.
 *
 * Scope note (increment 1): the guest half (the x86_64 AppleVideoToolbox
 * paravirt kext, PCI match 0x1a03106b) is blocked (lane C), so the exact
 * Apple on-wire framing (the `vtpv`/`para`/`sbuf` boxes) cannot be exercised
 * E2E and is bound to that guest contract. This handler therefore defines a
 * clean, self-contained submission/completion message format that carries
 * the SAME session semantics Apple's device carries - create a codec
 * session, decode an access unit into a surface, end the session - so the
 * dispatch/backend/completion logic and the VAAPI backend are real and
 * unit-tested now; the full Apple `sbuf` fidelity is a later increment.
 */
#ifndef VTPV_PROTO_H
#define VTPV_PROTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- wire constants ------------------------------------------------- */

#define VTPV_SUBMIT_MAGIC  0x56545056u /* 'VTPV' - a guest->host submission  */
#define VTPV_CMPL_MAGIC    0x43545056u /* 'VTPC' - a host->guest completion  */
#define VTPV_CAPS_MAGIC    0x44545056u /* 'VTPD' - the D-GVA-2 caps report   */
#define VTPV_ABI           1u

/* Codec four-CCs on the wire (match the guest's ImageDescription record).   */
#define VTPV_CODEC_H264    0x61766331u /* 'avc1' */
#define VTPV_CODEC_HEVC    0x68766331u /* 'hvc1' */

/* Decoded-surface pixel format four-CCs.                                    */
#define VTPV_PIXFMT_NV12   0x4e563132u /* 'NV12' - 8-bit 4:2:0 biplanar       */
#define VTPV_PIXFMT_P010   0x50303130u /* 'P010' - 10-bit 4:2:0 biplanar      */

/*
 * Transport limits from the device contract (devices.md "Transport",
 * verified against the x86_64 AppleVirtIO.kext, 2026-10-24):
 *   - Apple's virtqueues hold at most 256 descriptors; a transaction with
 *     more physical segments than the queue holds is refused
 *     (kIOReturnBadArgument).
 *   - A sample buffer may be up to 0x200000 bytes, but on an x86_64 guest
 *     (4 KiB pages) 256 direct descriptors carry only 1 MiB, so a sample
 *     larger than that never reaches the host. We enforce the reachable
 *     1 MiB bound (a later increment may lift it with indirect descriptors
 *     / the 1024-deep queue this host could offer instead).
 */
#define VTPV_MAX_QUEUE_DESCRIPTORS 256u
#define VTPV_MAX_SAMPLE_BYTES      (1u << 20) /* 1 MiB */
#define VTPV_MAX_EXTRADATA_BYTES   4096u      /* avcC/hvcC codec record       */
#define VTPV_MAX_SESSIONS          64u
#define VTPV_MAX_DIM               8192u

/* Message types (submission.type and the completion echoes it).             */
enum vtpv_msg_type {
    VTPV_MSG_QUERY_CAPS    = 1, /* -> completion carries a vtpv_caps report  */
    VTPV_MSG_SESSION_CREATE= 2, /* open a decoder session for a codec        */
    VTPV_MSG_DECODE_FRAME  = 3, /* decode one access unit into a surface     */
    VTPV_MSG_SESSION_END   = 4, /* close a decoder session                   */
};

/* Completion status: 0 ok, else a negative errno-style code.                */
enum {
    VTPV_OK              =  0,
    VTPV_ERR_MALFORMED   = -1, /* header/length/magic bad                    */
    VTPV_ERR_UNSUPPORTED = -2, /* codec/type not served                      */
    VTPV_ERR_TOO_LARGE   = -3, /* sample/extradata over a transport limit    */
    VTPV_ERR_NO_SESSION  = -4, /* session id not open                        */
    VTPV_ERR_NO_SLOTS    = -5, /* session table full                         */
    VTPV_ERR_BACKEND     = -6, /* the decode backend failed                  */
};
/* decode(): the access unit carried no coded picture (not an error).         */
#define VTPV_DROPPED 1

#pragma pack(push, 1)

/* Submission header (guest -> host, submission virtqueue).                  */
typedef struct vtpv_submit_hdr {
    uint32_t magic;    /* VTPV_SUBMIT_MAGIC                                   */
    uint16_t abi;      /* VTPV_ABI                                           */
    uint16_t type;     /* enum vtpv_msg_type                                 */
    uint32_t length;   /* total bytes of this message, header included       */
    uint32_t session;  /* session id (0 on SESSION_CREATE/QUERY_CAPS)        */
    uint64_t tag;      /* opaque request id, echoed in the completion        */
} vtpv_submit_hdr;

/* SESSION_CREATE payload: codec record (avcC/hvcC) follows inline.          */
typedef struct vtpv_create_req {
    uint32_t codec;         /* VTPV_CODEC_*                                  */
    uint32_t width;
    uint32_t height;
    uint32_t extradata_len; /* bytes of codec record that follow            */
    /* uint8_t extradata[extradata_len]; */
} vtpv_create_req;

/* DECODE_FRAME payload: the access-unit sample follows inline.              */
typedef struct vtpv_decode_req {
    uint64_t pts;
    uint32_t flags;
    uint32_t sample_len;    /* bytes of bitstream that follow               */
    /* uint8_t sample[sample_len]; */
} vtpv_decode_req;

/* Completion header (host -> guest, completion virtqueue).                  */
typedef struct vtpv_cmpl_hdr {
    uint32_t magic;    /* VTPV_CMPL_MAGIC                                    */
    uint16_t abi;      /* VTPV_ABI                                           */
    uint16_t type;     /* echoes the request type                           */
    uint32_t length;   /* total bytes, header included                       */
    uint32_t session;  /* session id (allocated id on SESSION_CREATE)        */
    uint64_t tag;      /* echoes the request tag                             */
    int32_t  status;   /* VTPV_OK or a VTPV_ERR_* code                       */
    uint32_t reserved;
} vtpv_cmpl_hdr;

/* DECODE_FRAME completion payload: the decoded-surface descriptor.
 * This is the surface-handoff point - QMetal owns decoded-surface<->GPU
 * sharing (IOSurface/DMA-BUF) in a later increment; here the backend owns
 * the surface and names it by id. */
typedef struct vtpv_decoded_desc {
    uint64_t pts;
    uint32_t surface_id;
    uint32_t width;
    uint32_t height;
    uint32_t pixfmt;   /* VTPV_PIXFMT_*                                      */
    uint32_t flags;
} vtpv_decoded_desc;

/* SESSION_CREATE completion payload.                                        */
typedef struct vtpv_create_desc {
    uint32_t session;
} vtpv_create_desc;

/* ---- D-GVA-2: the host video-capability advertisement --------------- *
 * A NEW device-feature report (NOT a METAL_FEATURES bit - video is a
 * separate device). The guest consumer learns which codecs the host offers
 * and their limits. Lives in the completion of VTPV_MSG_QUERY_CAPS. */
typedef struct vtpv_codec_cap {
    uint32_t codec;      /* VTPV_CODEC_*                                     */
    uint8_t  can_decode;
    uint8_t  can_encode; /* increment 1: decode only; encode is later        */
    uint16_t reserved;
    uint32_t max_width;
    uint32_t max_height;
    uint32_t out_pixfmt; /* VTPV_PIXFMT_* the decoder writes                 */
} vtpv_codec_cap;

typedef struct vtpv_caps {
    uint32_t magic;      /* VTPV_CAPS_MAGIC                                  */
    uint16_t abi;        /* VTPV_ABI                                         */
    uint16_t n_codecs;
    /* vtpv_codec_cap codecs[n_codecs]; */
} vtpv_caps;

#pragma pack(pop)

/* ---- decoder-backend vtable ---------------------------------------- *
 * A codec backend (VAAPI, later NVDEC/VideoToolbox) implements this. The
 * protocol handler owns dispatch; the backend owns the codec + the surface. */

typedef struct vtpv_decoded_frame {
    uint32_t surface_id; /* backend-local surface handle                    */
    uint32_t width;
    uint32_t height;
    uint32_t pixfmt;     /* VTPV_PIXFMT_*                                   */
} vtpv_decoded_frame;

typedef struct vtpv_decoder_backend {
    /* Fill *out (a caps buffer of out_cap bytes, header + codecs[]) with the
     * codecs this backend serves. Returns the number of codecs, or <0. */
    int (*query_caps)(void *ctx, vtpv_caps *out, size_t out_cap);

    /* Open a decoder for codec at width x height with the codec record
     * (avcC/hvcC) in extradata. *dec receives an opaque session handle.
     * Returns 0 or a VTPV_ERR_* code. */
    int (*open_decoder)(void *ctx, uint32_t codec, uint32_t width, uint32_t height,
                        const uint8_t *extradata, size_t extradata_len, void **dec);

    /* Decode one access unit (sample) into a surface described in *out.
     * Returns 0, VTPV_DROPPED (the unit carried no picture: the engine emits
     * it as a dropped frame without a buffer), or a VTPV_ERR_* code. */
    int (*decode)(void *dec, const uint8_t *sample, size_t sample_len,
                  vtpv_decoded_frame *out);

    /* Close a decoder opened by open_decoder. */
    void (*close_decoder)(void *dec);
} vtpv_decoder_backend;

/* ---- the protocol handler ------------------------------------------ */

/* Emit callback: the handler hands one complete completion message (header +
 * payload) to the device, which places it on the completion virtqueue and
 * raises the queue's interrupt. It must copy or queue and return promptly. */
typedef void (*vtpv_emit_fn)(void *emit_ctx, const uint8_t *cmpl, size_t len);

typedef struct vtpv_proto vtpv_proto;

/* Create a handler over a backend and an emit sink. Returns NULL on OOM. */
vtpv_proto *vtpv_proto_create(const vtpv_decoder_backend *be, void *be_ctx,
                              vtpv_emit_fn emit, void *emit_ctx);

/* Feed one submission message (as read from the submission virtqueue). The
 * handler validates it, dispatches to the backend, and emits exactly one
 * completion through the emit sink. Returns 0 if a completion was emitted
 * (even an error completion), or <0 only if the message was too malformed to
 * carry a tag to answer on (no completion emitted). */
int vtpv_proto_submit(vtpv_proto *p, const uint8_t *msg, size_t len);

/* Number of sessions currently open (for tests/telemetry). */
unsigned vtpv_proto_open_sessions(const vtpv_proto *p);

void vtpv_proto_destroy(vtpv_proto *p);

#ifdef __cplusplus
}
#endif

#endif /* VTPV_PROTO_H */
