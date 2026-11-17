/*
 * h264_gen.c - TEST-ONLY minimal H.264 I_PCM I-frame generator (see h264_gen.h).
 * CLEAN-ROOM from ITU-T H.264. Increment 1 stage 2, lane L-M2V03, 2026-11-16.
 */
#include "h264_gen.h"

#include <string.h>

/* ---- bit writer ----------------------------------------------------- */

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   bitpos;
    int      err;
} bw;

static void bw_bit(bw *w, int bit)
{
    size_t byte = w->bitpos >> 3;
    int    off  = 7 - (int)(w->bitpos & 7);
    if (byte >= w->cap) { w->err = 1; return; }
    if (off == 7) w->buf[byte] = 0; /* clear on first touch of a byte */
    if (bit) w->buf[byte] |= (uint8_t)(1 << off);
    w->bitpos++;
}
static void bw_u(bw *w, uint32_t val, int bits)
{
    while (bits-- > 0) bw_bit(w, (val >> bits) & 1);
}
static void bw_ue(bw *w, uint32_t val)
{
    uint32_t v = val + 1;
    int bits = 0;
    while ((v >> bits) > 0) bits++;           /* bit length of v */
    bw_u(w, 0, bits - 1);                      /* leading zeros */
    bw_u(w, v, bits);                          /* the value */
}
static void bw_se(bw *w, int32_t val)
{
    uint32_t k = val <= 0 ? (uint32_t)(-2 * val) : (uint32_t)(2 * val - 1);
    bw_ue(w, k);
}
static void bw_align(bw *w) { while (w->bitpos & 7) bw_bit(w, 0); }
static size_t bw_bytes(const bw *w) { return (w->bitpos + 7) >> 3; }

/* ---- emulation prevention + start codes ----------------------------- */

/* Append `rbsp[0..len)` to `out` as a NAL (start code + EBSP). */
static size_t emit_nal(uint8_t *out, size_t cap, size_t at,
                       const uint8_t *rbsp, size_t len, int *ok)
{
    size_t i;
    int zeros = 0;
    const uint8_t sc[4] = { 0, 0, 0, 1 };
    if (at + 4 > cap) { *ok = 0; return at; }
    memcpy(out + at, sc, 4); at += 4;
    for (i = 0; i < len; i++) {
        if (zeros >= 2 && rbsp[i] <= 3) {
            if (at >= cap) { *ok = 0; return at; }
            out[at++] = 0x03;
            zeros = 0;
        }
        if (at >= cap) { *ok = 0; return at; }
        out[at++] = rbsp[i];
        zeros = (rbsp[i] == 0) ? zeros + 1 : 0;
    }
    return at;
}

uint8_t h264_gen_luma_at(int x, int y)
{
    /* non-zero, no long zero runs (keeps the PCM region emulation-free);
     * distinct enough to catch a wrong plane/stride. */
    return (uint8_t)(0x40 + ((x * 3 + y * 5) & 0x3f));
}

size_t h264_gen_ipcm_frame(int width, int height, uint8_t *out, size_t out_cap,
                           uint8_t *luma_ref)
{
    uint8_t rbsp[256];
    bw w;
    size_t at = 0;
    int ok = 1;
    int mbw = width / 16, mbh = height / 16;
    int mbx, mby, i;

    if (width <= 0 || height <= 0 || (width & 15) || (height & 15))
        return 0;

    /* ---- SPS (Baseline 66, level 3.0) ---- */
    memset(rbsp, 0, sizeof(rbsp));
    w = (bw){ rbsp, sizeof(rbsp), 0, 0 };
    bw_u(&w, 0, 1); bw_u(&w, 3, 2); bw_u(&w, 7, 5); /* nal: ref_idc 3, type 7 */
    bw_u(&w, 66, 8);                 /* profile_idc Baseline */
    bw_u(&w, 0, 8);                  /* constraint flags + reserved */
    bw_u(&w, 30, 8);                 /* level_idc 3.0 */
    bw_ue(&w, 0);                    /* seq_parameter_set_id */
    bw_ue(&w, 0);                    /* log2_max_frame_num_minus4 */
    bw_ue(&w, 0);                    /* pic_order_cnt_type */
    bw_ue(&w, 0);                    /* log2_max_pic_order_cnt_lsb_minus4 */
    bw_ue(&w, 1);                    /* max_num_ref_frames */
    bw_u(&w, 0, 1);                  /* gaps_in_frame_num_value_allowed_flag */
    bw_ue(&w, (uint32_t)(mbw - 1));  /* pic_width_in_mbs_minus1 */
    bw_ue(&w, (uint32_t)(mbh - 1));  /* pic_height_in_map_units_minus1 */
    bw_u(&w, 1, 1);                  /* frame_mbs_only_flag */
    bw_u(&w, 0, 1);                  /* direct_8x8_inference_flag */
    bw_u(&w, 0, 1);                  /* frame_cropping_flag */
    bw_u(&w, 0, 1);                  /* vui_parameters_present_flag */
    bw_u(&w, 1, 1); bw_align(&w);    /* rbsp_trailing_bits */
    if (w.err) return 0;
    at = emit_nal(out, out_cap, at, rbsp, bw_bytes(&w), &ok);

    /* ---- PPS (CAVLC) ---- */
    memset(rbsp, 0, sizeof(rbsp));
    w = (bw){ rbsp, sizeof(rbsp), 0, 0 };
    bw_u(&w, 0, 1); bw_u(&w, 3, 2); bw_u(&w, 8, 5); /* nal: type 8 */
    bw_ue(&w, 0);                    /* pic_parameter_set_id */
    bw_ue(&w, 0);                    /* seq_parameter_set_id */
    bw_u(&w, 0, 1);                  /* entropy_coding_mode_flag (CAVLC) */
    bw_u(&w, 0, 1);                  /* bottom_field_pic_order_in_frame_present */
    bw_ue(&w, 0);                    /* num_slice_groups_minus1 */
    bw_ue(&w, 0);                    /* num_ref_idx_l0_default_active_minus1 */
    bw_ue(&w, 0);                    /* num_ref_idx_l1_default_active_minus1 */
    bw_u(&w, 0, 1);                  /* weighted_pred_flag */
    bw_u(&w, 0, 2);                  /* weighted_bipred_idc */
    bw_se(&w, 0);                    /* pic_init_qp_minus26 */
    bw_se(&w, 0);                    /* pic_init_qs_minus26 */
    bw_se(&w, 0);                    /* chroma_qp_index_offset */
    bw_u(&w, 0, 1);                  /* deblocking_filter_control_present_flag */
    bw_u(&w, 0, 1);                  /* constrained_intra_pred_flag */
    bw_u(&w, 0, 1);                  /* redundant_pic_cnt_present_flag */
    bw_u(&w, 1, 1); bw_align(&w);    /* rbsp_trailing_bits */
    if (w.err) return 0;
    at = emit_nal(out, out_cap, at, rbsp, bw_bytes(&w), &ok);

    /* ---- IDR slice of I_PCM macroblocks ---- */
    /* The slice RBSP can be large (384 bytes/MB); build it directly in `out`
     * after the headers via a temporary bit writer over a scratch buffer.
     * Use a dedicated heap-free bound: cap the fixture at out_cap. */
    {
        static uint8_t slice[1u << 20]; /* up to 1 MiB RBSP (test only) */
        bw sw = (bw){ slice, sizeof(slice), 0, 0 };
        bw_u(&sw, 0, 1); bw_u(&sw, 3, 2); bw_u(&sw, 5, 5); /* nal: ref_idc 3, IDR type 5 */
        bw_ue(&sw, 0);               /* first_mb_in_slice */
        bw_ue(&sw, 7);               /* slice_type 7 (I, all) */
        bw_ue(&sw, 0);               /* pic_parameter_set_id */
        bw_u(&sw, 0, 4);             /* frame_num (log2_max=4) */
        bw_ue(&sw, 0);               /* idr_pic_id */
        bw_u(&sw, 0, 4);             /* pic_order_cnt_lsb (log2=4) */
        bw_u(&sw, 0, 1);             /* no_output_of_prior_pics_flag */
        bw_u(&sw, 0, 1);             /* long_term_reference_flag */
        bw_se(&sw, 0);               /* slice_qp_delta */
        /* slice_data: one I_PCM macroblock per MB, raster order */
        for (mby = 0; mby < mbh; mby++) {
            for (mbx = 0; mbx < mbw; mbx++) {
                int px, py;
                bw_ue(&sw, 25);      /* mb_type I_PCM (I-slice ue 25) */
                bw_align(&sw);       /* pcm_alignment_zero_bit(s) */
                for (py = 0; py < 16; py++)
                    for (px = 0; px < 16; px++) {
                        int gx = mbx * 16 + px, gy = mby * 16 + py;
                        uint8_t s = h264_gen_luma_at(gx, gy);
                        bw_u(&sw, s, 8);
                        if (luma_ref) luma_ref[gy * width + gx] = s;
                    }
                for (i = 0; i < 128; i++) bw_u(&sw, 0x80, 8); /* Cb+Cr neutral */
            }
        }
        bw_u(&sw, 1, 1); bw_align(&sw); /* rbsp_slice_trailing_bits */
        if (sw.err) return 0;
        at = emit_nal(out, out_cap, at, slice, bw_bytes(&sw), &ok);
    }

    return ok ? at : 0;
}
