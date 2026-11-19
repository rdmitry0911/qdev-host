/*
 * backing_A.c - uniq-vgpu VARIABLE backing A (boot default).
 *
 * The backend the device advertises at boot: the current QMetal/libmlapi
 * identity, no swap-window shaping. Warm-swapping to backing_B.so changes the
 * advertised backend_version (the observable in the guest ioreg triplet) and
 * widens the quiesce window (the basic-scanout-hold crux probe) without any
 * wire-framing change. See pvg_backing.h. CLEAN-ROOM, L-WARMSWAP-2.
 */
#include "pvg_backing.h"

static const pvg_backing_v1 backing_A = {
    .abi             = PVG_BACKING_ABI,
    .tag             = "A",
    .backend_version = "qmetal-backend-A",
    .hold_ms         = 0,
};

__attribute__((visibility("default")))
const pvg_backing_v1 *pvg_backing(void) { return &backing_A; }
