/*
 * vtpv_backing.h - the apple-videotoolbox plugin's VARIABLE backing ABI.
 *
 * "fixed resident kext / variable host backend", increment 1. The guest-side
 * kext freezes the ENVELOPE (framing, virtqueue layout, handshake, the
 * capability-negotiation mechanism). Everything feature-specific lives behind
 * this boundary and can be warm-swapped under a live, un-rebooted guest: the
 * host quiesces, dlopen()s a new backing .so (this ABI), repoints, and the
 * guest picks up the new capability advertisement on its next re-handshake.
 *
 * A backing .so exports one symbol, VTPV_BACKING_SYMBOL, returning a pointer to
 * a static const vtpv_backing_v1. It describes ONLY the VARIABLE content the
 * device advertises; it must not change any wire framing.
 *
 * CLEAN-ROOM. Lane L-WARMSWAP-1, 2026-11-16.
 */
#ifndef VTPV_BACKING_H
#define VTPV_BACKING_H

#include <stdint.h>

#define VTPV_BACKING_ABI 1u

typedef struct vtpv_backing_v1 {
    uint32_t    abi;          /* VTPV_BACKING_ABI                               */
    const char *tag;          /* human label, logged on swap (e.g. "A"/"B")     */
    uint32_t    codec_mask;   /* AW_CONTENT_* bits: which decoders to advertise */
    const char *name_suffix;  /* appended to every advertised CodecName ("" ok) */
} vtpv_backing_v1;

typedef const vtpv_backing_v1 *(*vtpv_backing_fn)(void);
#define VTPV_BACKING_SYMBOL "vtpv_backing"

#endif /* VTPV_BACKING_H */
