/*
 * vtpv_h264.h - a minimal H.264 Annex-B parser for the VA-API decode backend:
 * enough SPS / PPS / slice-header syntax to build the VA decode parameter
 * buffers for an intra frame. CLEAN-ROOM from ITU-T H.264. Increment 1
 * stage 2, lane L-M2V03, 2026-11-16.
 *
 * Not a full decoder: it reads the headers above the slice data (the hardware
 * does the macroblock decode). Scope: Baseline/High intra, CAVLC, POC type 0,
 * no field coding. Enough for a hand-crafted I-frame (I_PCM or a DC I-slice).
 */
#ifndef VTPV_H264_H
#define VTPV_H264_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h264_sps {
    int profile_idc, level_idc;
    int chroma_format_idc;              /* 1 = 4:2:0 (default) */
    int bit_depth_luma, bit_depth_chroma;
    int log2_max_frame_num;             /* _minus4 + 4 */
    int pic_order_cnt_type;
    int log2_max_pic_order_cnt_lsb;     /* _minus4 + 4 (poc type 0) */
    int delta_pic_order_always_zero_flag;
    int max_num_ref_frames;
    int gaps_in_frame_num_value_allowed_flag;
    int pic_width_in_mbs;               /* _minus1 + 1 */
    int pic_height_in_map_units;        /* _minus1 + 1 */
    int frame_mbs_only_flag;
    int mb_adaptive_frame_field_flag;
    int direct_8x8_inference_flag;
    int valid;
} h264_sps;

typedef struct h264_pps {
    int pic_parameter_set_id, seq_parameter_set_id;
    int entropy_coding_mode_flag;
    int bottom_field_pic_order_in_frame_present_flag;
    int num_slice_groups;
    int num_ref_idx_l0_default_active, num_ref_idx_l1_default_active;
    int weighted_pred_flag, weighted_bipred_idc;
    int pic_init_qp;                    /* _minus26 + 26 */
    int chroma_qp_index_offset, second_chroma_qp_index_offset;
    int deblocking_filter_control_present_flag;
    int constrained_intra_pred_flag;
    int redundant_pic_cnt_present_flag;
    int transform_8x8_mode_flag;
    int valid;
} h264_pps;

typedef struct h264_slice {
    int nal_ref_idc, nal_unit_type;     /* 5 = IDR slice */
    int first_mb_in_slice, slice_type;  /* 2 or 7 = I */
    int pic_parameter_set_id;
    int frame_num, idr_pic_id, pic_order_cnt_lsb;
    int slice_qp;                       /* pic_init_qp + slice_qp_delta */
    /* bits from the first byte of the slice-data buffer (the NAL header byte,
     * the start code excluded) to the first macroblock (slice_data). This is
     * VASliceParameterBufferH264.slice_data_bit_offset. */
    uint32_t data_bit_offset;
    int valid;
} h264_slice;

/* A split of an Annex-B stream into its NAL units (pointers into the input,
 * start codes excluded; emulation-prevention NOT removed - the parsers below
 * strip 0x03 as they read). */
typedef struct h264_nal {
    const uint8_t *data;  /* first byte = NAL header */
    size_t         size;  /* bytes, start code excluded */
} h264_nal;

#define H264_MAX_NALS 16

/* Split Annex-B bytes into NAL units. Returns the count (<= H264_MAX_NALS). */
int h264_split_nals(const uint8_t *buf, size_t len, h264_nal *out, int max);

/* Parse. Each returns 0 on success, <0 on malformed. `nal` points at the NAL
 * header byte (start code excluded). */
int h264_parse_sps(const h264_nal *nal, h264_sps *sps);
int h264_parse_pps(const h264_nal *nal, const h264_sps *sps, h264_pps *pps);
int h264_parse_slice_header(const h264_nal *nal, const h264_sps *sps,
                            const h264_pps *pps, h264_slice *sh);

#ifdef __cplusplus
}
#endif

#endif /* VTPV_H264_H */
