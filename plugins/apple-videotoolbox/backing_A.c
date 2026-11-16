/*
 * backing_A.c - warm-swap VARIABLE backing variant A (the boot default).
 * Advertises only the AVC decoder with its stock CodecName. See vtpv_backing.h.
 * CLEAN-ROOM. Lane L-WARMSWAP-1.
 */
#include "vtpv_backing.h"

/* AW_CONTENT_AVC = 0x1, AW_CONTENT_HEVC = 0x2 (aw_session.h). */
static const vtpv_backing_v1 backing = {
    .abi         = VTPV_BACKING_ABI,
    .tag         = "A",
    .codec_mask  = 0x1u,          /* AVC only */
    .name_suffix = "",            /* stock CodecName: "AppleGVA Video Decoder - AVC" */
};

const vtpv_backing_v1 *vtpv_backing(void) { return &backing; }
