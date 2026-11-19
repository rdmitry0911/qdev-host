/*
 * backing_B.c - uniq-vgpu VARIABLE backing B (warm-swap target, observable A/B).
 *
 * B advertises a BUMPED backend_version ("qmetal-backend-B [warmswapB]") -- the
 * observable the guest's ioreg triplet re-publishes after the in-place reset
 * re-handshake -- and holds the quiesce window open 250 ms so the basic-scanout
 * crux (does the resident keep the screen lit while the accelerated backend is
 * down?) is exercised with a visible window. No wire-framing change: the frozen
 * envelope (pvg_regs.h / protocol v6 / DeviceInfo mechanism) is untouched.
 * See pvg_backing.h. CLEAN-ROOM, L-WARMSWAP-2.
 */
#include "pvg_backing.h"

static const pvg_backing_v1 backing_B = {
    .abi             = PVG_BACKING_ABI,
    .tag             = "B",
    .backend_version = "qmetal-backend-B [warmswapB]",
    .hold_ms         = 250,
};

__attribute__((visibility("default")))
const pvg_backing_v1 *pvg_backing(void) { return &backing_B; }
