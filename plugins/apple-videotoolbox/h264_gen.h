/*
 * h264_gen.h - TEST-ONLY generator of a minimal H.264 Annex-B I-frame made of
 * I_PCM macroblocks (raw samples, no entropy-coded residuals): a self-
 * contained, deterministic decode fixture with known pixels. CLEAN-ROOM from
 * ITU-T H.264. Increment 1 stage 2, lane L-M2V03, 2026-11-16.
 *
 * Not part of the shipped plugin - only the unit test links it.
 */
#ifndef H264_GEN_H
#define H264_GEN_H

#include <stddef.h>
#include <stdint.h>

/*
 * Emit SPS + PPS + an IDR slice of I_PCM macroblocks for a width x height
 * frame (both multiples of 16) into out[out_cap]. The luma sample at (x, y) is
 * set to a deterministic value; the same values are written into luma_ref
 * (width*height bytes) so the caller can verify the decoded surface. Chroma is
 * a neutral 0x80. Returns the Annex-B byte count, or 0 on error.
 */
size_t h264_gen_ipcm_frame(int width, int height, uint8_t *out, size_t out_cap,
                           uint8_t *luma_ref);

/* The sample value the generator writes at luma (x, y). */
uint8_t h264_gen_luma_at(int x, int y);

#endif /* H264_GEN_H */
