/*
 * backing_B.c - warm-swap VARIABLE backing variant B (the swapped-in content).
 * Advertises AVC (with a changed CodecName) PLUS a newly-added HEVC decoder,
 * proving a warm swap can both change existing behaviour and ADD a capability
 * without a guest reboot. See vtpv_backing.h. CLEAN-ROOM. Lane L-WARMSWAP-1.
 */
#include "vtpv_backing.h"

/* AW_CONTENT_AVC = 0x1, AW_CONTENT_HEVC = 0x2 (aw_session.h). */
static const vtpv_backing_v1 backing = {
    .abi         = VTPV_BACKING_ABI,
    .tag         = "B",
    .codec_mask  = 0x3u,          /* AVC + HEVC (HEVC is newly advertised vs A) */
    .name_suffix = " [warmswapB]", /* CodecName becomes "...AVC [warmswapB]"     */
};

const vtpv_backing_v1 *vtpv_backing(void) { return &backing; }
