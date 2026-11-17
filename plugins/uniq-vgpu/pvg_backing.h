/*
 * pvg_backing.h - the apple-pvg (display GPU) plugin's VARIABLE backing ABI.
 *
 * "fixed resident kext / variable host backend", increment 2 (DISPLAY GPU).
 * The guest-side UniqVGPU kext (dev.uniqvgpu, the FIXED resident) freezes the
 * ENVELOPE: the PVG MMIO/FIFO register contract (pvg_regs.h, protocol v6), the
 * virtqueue/ch5 framing and the DeviceInfo capability-negotiation MECHANISM.
 * Everything backend-specific -- the accelerated GPU backend (QMetal / libmlapi
 * + libair2spirv) and the backend identity/version it advertises -- lives behind
 * this boundary and can be warm-swapped under a live, un-rebooted guest with the
 * desktop up: the host quiesces the backend (the memory table write-lock stalls
 * all guest-RAM access and IRQ delivery, so the backend freezes; the host's memfd
 * scanout keeps presenting the LAST frame -> the screen is held, not blacked),
 * repoints the VARIABLE backing, and resumes. The guest picks up the new backend
 * on its next capability re-negotiation, which an in-place reset triggers.
 *
 * A backing .so exports one symbol, PVG_BACKING_SYMBOL, returning a pointer to a
 * static const pvg_backing_v1. It describes ONLY the VARIABLE backend identity
 * the device advertises and the swap-window shaping; it must NOT change any wire
 * framing or the MMIO register contract (that is the frozen envelope).
 *
 * CLEAN-ROOM. Lane L-WARMSWAP-2, 2026-11-17. Mirrors vtpv_backing.h (inc1).
 */
#ifndef PVG_BACKING_H
#define PVG_BACKING_H

#include <stdint.h>

#define PVG_BACKING_ABI 1u

typedef struct pvg_backing_v1 {
    uint32_t    abi;             /* PVG_BACKING_ABI                                   */
    const char *tag;             /* human label, logged on swap (e.g. "A"/"B")        */
    const char *backend_version; /* observable backend identity string the guest's    *
                                  * ioreg triplet re-publishes on re-handshake        */
    uint32_t    hold_ms;         /* widen the quiesce window by this long (crux probe: *
                                  * how long the resident holds basic scanout); 0 = no */
} pvg_backing_v1;

typedef const pvg_backing_v1 *(*pvg_backing_fn)(void);
#define PVG_BACKING_SYMBOL "pvg_backing"

#endif /* PVG_BACKING_H */
