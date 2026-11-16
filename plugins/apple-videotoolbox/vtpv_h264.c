/*
 * vtpv_h264.c - minimal H.264 Annex-B header parser (see vtpv_h264.h).
 * CLEAN-ROOM from ITU-T H.264. Increment 1 stage 2, lane L-M2V03, 2026-11-16.
 */
#include "vtpv_h264.h"

#include <string.h>

/* ---- bit reader over a raw NAL (EBSP), emulation-prevention aware -----
 * pos counts RAW bits (emulation bytes included) so the slice-header end
 * gives VASliceParameterBufferH264.slice_data_bit_offset directly. */
typedef struct {
    const uint8_t *p;
    size_t         n;    /* bytes, NAL header included, start code excluded */
    size_t         pos;  /* raw bit position */
    int            err;
} br;

static void br_init(br *b, const uint8_t *p, size_t n)
{
    b->p = p; b->n = n; b->pos = 0; b->err = 0;
}

static int br_bit(br *b)
{
    size_t byte = b->pos >> 3;
    int    off  = 7 - (int)(b->pos & 7);
    /* at a byte boundary, skip an emulation_prevention_three_byte (00 00 03) */
    if (off == 7 && byte >= 2 && byte < b->n &&
        b->p[byte] == 0x03 && b->p[byte - 1] == 0 && b->p[byte - 2] == 0) {
        b->pos += 8;
        byte++;
    }
    if (byte >= b->n) { b->err = 1; b->pos++; return 0; }
    b->pos++;
    return (b->p[byte] >> off) & 1;
}

static uint32_t br_u(br *b, int bits)
{
    uint32_t v = 0;
    while (bits-- > 0) v = (v << 1) | (uint32_t)br_bit(b);
    return v;
}

static uint32_t br_ue(br *b)
{
    int zeros = 0;
    while (!b->err && br_bit(b) == 0 && zeros < 32) zeros++;
    if (zeros == 0) return 0;
    return ((1u << zeros) - 1u) + br_u(b, zeros);
}

static int32_t br_se(br *b)
{
    uint32_t k = br_ue(b);
    uint32_t a = (k + 1) >> 1;
    return (k & 1) ? (int32_t)a : -(int32_t)a;
}

/* ---- NAL splitting -------------------------------------------------- */

int h264_split_nals(const uint8_t *buf, size_t len, h264_nal *out, int max)
{
    int n = 0;
    size_t i = 0;
    while (i + 3 <= len && n < max) {
        /* find a start code 00 00 01 (3-byte) possibly preceded by an extra 00 */
        size_t sc = (size_t)-1, j;
        for (j = i; j + 3 <= len; j++) {
            if (buf[j] == 0 && buf[j + 1] == 0 && buf[j + 2] == 1) { sc = j; break; }
        }
        if (sc == (size_t)-1) break;
        size_t start = sc + 3; /* NAL header byte */
        /* find the next start code */
        size_t k, end = len;
        for (k = start; k + 3 <= len; k++) {
            if (buf[k] == 0 && buf[k + 1] == 0 && buf[k + 2] == 1) { end = k; break; }
        }
        /* trim a trailing 00 that belongs to the next start code (00 00 00 01) */
        while (end > start && buf[end - 1] == 0 && end == len) break;
        if (end > start && end < len && buf[end - 1] == 0) end--;
        out[n].data = buf + start;
        out[n].size = end - start;
        n++;
        i = end;
    }
    return n;
}

int h264_is_annexb(const uint8_t *buf, size_t len)
{
    if (len >= 3 && buf[0] == 0 && buf[1] == 0 && buf[2] == 1) return 1;
    if (len >= 4 && buf[0] == 0 && buf[1] == 0 && buf[2] == 0 && buf[3] == 1) return 1;
    return 0;
}

int h264_split_avcc_nals(const uint8_t *buf, size_t len, int nal_length_size,
                         h264_nal *out, int max)
{
    int n = 0;
    size_t i = 0;
    if (nal_length_size < 1 || nal_length_size > 4) return 0;
    while (i + (size_t)nal_length_size <= len && n < max) {
        size_t sz = 0;
        for (int k = 0; k < nal_length_size; k++)
            sz = (sz << 8) | buf[i + k];
        i += (size_t)nal_length_size;
        if (sz == 0 || i + sz > len) break;   /* truncated / malformed length */
        out[n].data = buf + i;
        out[n].size = sz;
        n++;
        i += sz;
    }
    return n;
}

/* avcC record: [1]configurationVersion [1]profile [1]compat [1]level
 * [1](6 reserved | 2 lengthSizeMinusOne) [1](3 reserved | 5 numSPS)
 * then numSPS * ([2]len [len]SPS), [1]numPPS, then numPPS * ([2]len [len]PPS). */
int h264_parse_avcc(const uint8_t *avcc, size_t len,
                    h264_nal *sps, h264_nal *pps, int *nal_length_size)
{
    if (!avcc || len < 7 || avcc[0] != 1) return -1;
    if (nal_length_size) *nal_length_size = (avcc[4] & 3) + 1;
    size_t off = 5;
    int num_sps = avcc[off++] & 0x1f;
    if (sps) { sps->data = NULL; sps->size = 0; }
    if (pps) { pps->data = NULL; pps->size = 0; }
    for (int i = 0; i < num_sps; i++) {
        if (off + 2 > len) return -1;
        size_t sl = ((size_t)avcc[off] << 8) | avcc[off + 1];
        off += 2;
        if (off + sl > len) return -1;
        if (i == 0 && sps) { sps->data = avcc + off; sps->size = sl; }
        off += sl;
    }
    if (off >= len) return -1;
    int num_pps = avcc[off++];
    for (int i = 0; i < num_pps; i++) {
        if (off + 2 > len) return -1;
        size_t pl = ((size_t)avcc[off] << 8) | avcc[off + 1];
        off += 2;
        if (off + pl > len) return -1;
        if (i == 0 && pps) { pps->data = avcc + off; pps->size = pl; }
        off += pl;
    }
    if (sps && !sps->data) return -1;
    if (pps && !pps->data) return -1;
    return 0;
}

/* ---- parsers -------------------------------------------------------- */

int h264_parse_sps(const h264_nal *nal, h264_sps *sps)
{
    br b;
    int nut;
    if (!nal || nal->size < 4) return -1;
    br_init(&b, nal->data, nal->size);
    (void)br_u(&b, 1);                 /* forbidden_zero_bit */
    (void)br_u(&b, 2);                 /* nal_ref_idc */
    nut = (int)br_u(&b, 5);
    if (nut != 7) return -1;

    memset(sps, 0, sizeof(*sps));
    sps->chroma_format_idc = 1;        /* 4:2:0 default */
    sps->bit_depth_luma = sps->bit_depth_chroma = 8;

    sps->profile_idc = (int)br_u(&b, 8);
    (void)br_u(&b, 8);                 /* constraint flags + reserved */
    sps->level_idc = (int)br_u(&b, 8);
    (void)br_ue(&b);                   /* seq_parameter_set_id */

    switch (sps->profile_idc) {
    case 100: case 110: case 122: case 244: case 44:
    case 83: case 86: case 118: case 128: case 138: case 139: case 134: case 135:
        /* High-profile prelude (chroma_format_idc, bit depths, scaling lists).
         * Minimal path targets Baseline (66); refuse High for now. */
        return -2;
    default:
        break;
    }

    sps->log2_max_frame_num = (int)br_ue(&b) + 4;
    sps->pic_order_cnt_type = (int)br_ue(&b);
    if (sps->pic_order_cnt_type == 0) {
        sps->log2_max_pic_order_cnt_lsb = (int)br_ue(&b) + 4;
    } else if (sps->pic_order_cnt_type == 1) {
        return -2; /* poc type 1 not in the minimal path */
    }
    sps->max_num_ref_frames = (int)br_ue(&b);
    sps->gaps_in_frame_num_value_allowed_flag = (int)br_u(&b, 1);
    sps->pic_width_in_mbs = (int)br_ue(&b) + 1;
    sps->pic_height_in_map_units = (int)br_ue(&b) + 1;
    sps->frame_mbs_only_flag = (int)br_u(&b, 1);
    if (!sps->frame_mbs_only_flag)
        sps->mb_adaptive_frame_field_flag = (int)br_u(&b, 1);
    sps->direct_8x8_inference_flag = (int)br_u(&b, 1);
    /* frame_cropping_flag, vui - not needed for the decode params */
    if (b.err) return -1;
    sps->valid = 1;
    return 0;
}

int h264_parse_pps(const h264_nal *nal, const h264_sps *sps, h264_pps *pps)
{
    br b;
    int nut;
    (void)sps;
    if (!nal || nal->size < 2) return -1;
    br_init(&b, nal->data, nal->size);
    (void)br_u(&b, 1); (void)br_u(&b, 2);
    nut = (int)br_u(&b, 5);
    if (nut != 8) return -1;

    memset(pps, 0, sizeof(*pps));
    pps->pic_parameter_set_id = (int)br_ue(&b);
    pps->seq_parameter_set_id = (int)br_ue(&b);
    pps->entropy_coding_mode_flag = (int)br_u(&b, 1);
    pps->bottom_field_pic_order_in_frame_present_flag = (int)br_u(&b, 1);
    pps->num_slice_groups = (int)br_ue(&b) + 1;
    if (pps->num_slice_groups > 1)
        return -2; /* slice groups not in the minimal path */
    pps->num_ref_idx_l0_default_active = (int)br_ue(&b) + 1;
    pps->num_ref_idx_l1_default_active = (int)br_ue(&b) + 1;
    pps->weighted_pred_flag = (int)br_u(&b, 1);
    pps->weighted_bipred_idc = (int)br_u(&b, 2);
    pps->pic_init_qp = (int)br_se(&b) + 26;
    (void)br_se(&b);                   /* pic_init_qs_minus26 */
    pps->chroma_qp_index_offset = (int)br_se(&b);
    pps->deblocking_filter_control_present_flag = (int)br_u(&b, 1);
    pps->constrained_intra_pred_flag = (int)br_u(&b, 1);
    pps->redundant_pic_cnt_present_flag = (int)br_u(&b, 1);
    pps->second_chroma_qp_index_offset = pps->chroma_qp_index_offset;
    /* the optional High-profile PPS tail (transform_8x8_mode etc.) is absent
     * in a Baseline PPS; left at 0. */
    if (b.err) return -1;
    pps->valid = 1;
    return 0;
}

int h264_parse_slice_header(const h264_nal *nal, const h264_sps *sps,
                            const h264_pps *pps, h264_slice *sh)
{
    br b;
    if (!nal || nal->size < 2 || !sps->valid || !pps->valid) return -1;
    br_init(&b, nal->data, nal->size);
    memset(sh, 0, sizeof(*sh));
    (void)br_u(&b, 1);
    sh->nal_ref_idc = (int)br_u(&b, 2);
    sh->nal_unit_type = (int)br_u(&b, 5);
    if (sh->nal_unit_type != 1 && sh->nal_unit_type != 5) return -1;

    sh->first_mb_in_slice = (int)br_ue(&b);
    sh->slice_type = (int)br_ue(&b);
    sh->pic_parameter_set_id = (int)br_ue(&b);
    sh->frame_num = (int)br_u(&b, sps->log2_max_frame_num);
    /* frame_mbs_only_flag assumed 1 (no field_pic_flag) */
    if (sh->nal_unit_type == 5)
        sh->idr_pic_id = (int)br_ue(&b);
    if (sps->pic_order_cnt_type == 0) {
        sh->pic_order_cnt_lsb = (int)br_u(&b, sps->log2_max_pic_order_cnt_lsb);
        if (pps->bottom_field_pic_order_in_frame_present_flag)
            (void)br_se(&b);           /* delta_pic_order_cnt_bottom */
    }
    if (pps->redundant_pic_cnt_present_flag)
        (void)br_ue(&b);               /* redundant_pic_cnt */
    /* I slice (type 2 or 7): no direct/ref-idx/ref-list-modification */
    if (sh->slice_type != 2 && sh->slice_type != 7) return -2;
    /* dec_ref_pic_marking */
    if (sh->nal_ref_idc != 0) {
        if (sh->nal_unit_type == 5) {
            (void)br_u(&b, 1);         /* no_output_of_prior_pics_flag */
            (void)br_u(&b, 1);         /* long_term_reference_flag */
        } else {
            int adaptive = (int)br_u(&b, 1);
            if (adaptive) {
                int op;
                do {
                    op = (int)br_ue(&b);
                    if (op == 1 || op == 3) (void)br_ue(&b);
                    if (op == 2) (void)br_ue(&b);
                    if (op == 3 || op == 6) (void)br_ue(&b);
                    if (op == 4) (void)br_ue(&b);
                } while (op != 0 && !b.err);
            }
        }
    }
    /* entropy CAVLC: no cabac_init_idc */
    sh->slice_qp = pps->pic_init_qp + (int)br_se(&b);
    /* deblocking_filter_control_present_flag assumed 0 */
    if (b.err) return -1;
    sh->data_bit_offset = (uint32_t)b.pos; /* slice_data starts here */
    sh->valid = 1;
    return 0;
}
