/******************************************************************************
    H.264 SPS minimal patcher — implementation
******************************************************************************/

#include "h264-sps.h"
#include <obs-module.h> /* bmalloc, bfree, blog */
#include <string.h>

/* ---------- Bit reader (Exp-Golomb capable) ---------- */

typedef struct br {
  const uint8_t *data;
  size_t total_bits;
  size_t bit_pos;
  bool error;
} br_t;

static uint32_t br_read_bits(br_t *br, int n) {
  if (br->error)
    return 0;
  if (br->bit_pos + (size_t)n > br->total_bits) {
    br->error = true;
    return 0;
  }
  uint32_t v = 0;
  for (int i = 0; i < n; i++) {
    v = (v << 1) | ((uint32_t)((br->data[br->bit_pos >> 3] >>
                                 (7 - (br->bit_pos & 7))) &
                                1u));
    br->bit_pos++;
  }
  return v;
}

static void br_skip_bits(br_t *br, int n) {
  if (br->error)
    return;
  if (br->bit_pos + (size_t)n > br->total_bits) {
    br->error = true;
    return;
  }
  br->bit_pos += n;
}

static uint32_t br_read_ue(br_t *br) {
  int leading_zeros = 0;
  while (leading_zeros < 32) {
    if (br->error)
      return 0;
    if (br_read_bits(br, 1) == 1)
      break;
    leading_zeros++;
  }
  if (br->error || leading_zeros >= 32) {
    br->error = true;
    return 0;
  }
  if (leading_zeros == 0)
    return 0;
  uint32_t suffix = br_read_bits(br, leading_zeros);
  return (1u << leading_zeros) - 1u + suffix;
}

static int32_t br_read_se(br_t *br) {
  uint32_t ue = br_read_ue(br);
  if (ue & 1u)
    return (int32_t)((ue + 1u) / 2u);
  return -(int32_t)(ue / 2u);
}

/* ---------- EPB decode / encode ---------- */

/* Remove 0x03 emulation-prevention bytes that follow 0x00 0x00 pairs.
 * Returns the decoded size; output buffer must be at least in_size bytes. */
static size_t epb_decode(const uint8_t *in, size_t in_size, uint8_t *out) {
  size_t op = 0;
  int zero_count = 0;
  for (size_t i = 0; i < in_size; i++) {
    uint8_t b = in[i];
    if (zero_count >= 2 && b == 0x03) {
      /* Drop this emulation-prevention byte. */
      zero_count = 0;
      continue;
    }
    out[op++] = b;
    zero_count = (b == 0x00) ? (zero_count + 1) : 0;
  }
  return op;
}

/* Insert 0x03 emulation-prevention bytes per H.264 spec §7.4.1.1.
 * Returns the encoded size; output buffer must be at least
 * in_size + in_size/2 + 4 bytes (worst case). */
static size_t epb_encode(const uint8_t *in, size_t in_size, uint8_t *out) {
  size_t op = 0;
  int zero_count = 0;
  for (size_t i = 0; i < in_size; i++) {
    uint8_t b = in[i];
    if (zero_count >= 2 && b <= 0x03) {
      out[op++] = 0x03;
      zero_count = 0;
    }
    out[op++] = b;
    zero_count = (b == 0x00) ? (zero_count + 1) : 0;
  }
  return op;
}

/* ---------- SPS navigator ---------- */

/* Walk the SODB (NAL header byte + decoded RBSP body) to the bit position of
 * pic_struct_present_flag in the SPS VUI. Returns the absolute bit position
 * within `sodb`, or 0 on failure. Bails out on:
 *   - scaling matrix in SPS (would require full scaling-list parsing)
 *   - NAL HRD or VCL HRD parameters in VUI (would require hrd_parameters parser)
 *   - VUI not present (we don't synthesise one — would shift later bits)
 *   - parser hit truncated data
 *
 * On success, *current_value receives the existing value of the flag (0 or 1).
 */
static bool find_pic_struct_present_position(const uint8_t *sodb,
                                             size_t sodb_size,
                                             size_t *bit_pos_out,
                                             uint32_t *current_value,
                                             bool *cpb_dpb_delays_present_out,
                                             uint8_t *cpb_removal_delay_length_out,
                                             uint8_t *dpb_output_delay_length_out) {
  if (sodb_size < 5)
    return false;

  br_t br = {.data = sodb, .total_bits = sodb_size * 8, .bit_pos = 0};

  /* Skip the NAL header byte (1 byte for H.264). */
  br_skip_bits(&br, 8);

  /* profile_idc u(8) */
  uint32_t profile_idc = br_read_bits(&br, 8);
  /* constraint_setX_flag (6) + reserved_zero_2bits (2) */
  br_skip_bits(&br, 8);
  /* level_idc u(8) */
  br_skip_bits(&br, 8);
  /* seq_parameter_set_id ue(v) */
  br_read_ue(&br);

  /* High-profile-family extension fields. */
  switch (profile_idc) {
  case 100: case 110: case 122: case 244: case 44: case 83: case 86:
  case 118: case 128: case 138: case 139: case 134: case 135: {
    uint32_t chroma_format_idc = br_read_ue(&br);
    if (chroma_format_idc == 3)
      br_skip_bits(&br, 1); /* separate_colour_plane_flag */
    br_read_ue(&br);        /* bit_depth_luma_minus8 */
    br_read_ue(&br);        /* bit_depth_chroma_minus8 */
    br_skip_bits(&br, 1);   /* qpprime_y_zero_transform_bypass_flag */
    uint32_t scaling_matrix_present = br_read_bits(&br, 1);
    if (scaling_matrix_present) {
      blog(LOG_WARNING,
           "[H.264 SPS patch] scaling matrix present — patcher bails out");
      return false;
    }
    break;
  }
  default:
    break;
  }

  br_read_ue(&br); /* log2_max_frame_num_minus4 */
  uint32_t pic_order_cnt_type = br_read_ue(&br);
  if (pic_order_cnt_type == 0) {
    br_read_ue(&br); /* log2_max_pic_order_cnt_lsb_minus4 */
  } else if (pic_order_cnt_type == 1) {
    br_skip_bits(&br, 1); /* delta_pic_order_always_zero_flag */
    br_read_se(&br);      /* offset_for_non_ref_pic */
    br_read_se(&br);      /* offset_for_top_to_bottom_field */
    uint32_t n = br_read_ue(&br);
    /* Guard: cap to a sane bound to avoid runaway loops on bad input. */
    if (n > 255) {
      return false;
    }
    for (uint32_t i = 0; i < n; i++)
      br_read_se(&br);
  }
  br_read_ue(&br);      /* max_num_ref_frames */
  br_skip_bits(&br, 1); /* gaps_in_frame_num_value_allowed_flag */
  br_read_ue(&br);      /* pic_width_in_mbs_minus1 */
  br_read_ue(&br);      /* pic_height_in_map_units_minus1 */
  uint32_t frame_mbs_only_flag = br_read_bits(&br, 1);
  if (!frame_mbs_only_flag)
    br_skip_bits(&br, 1); /* mb_adaptive_frame_field_flag */
  br_skip_bits(&br, 1);   /* direct_8x8_inference_flag */
  uint32_t frame_cropping_flag = br_read_bits(&br, 1);
  if (frame_cropping_flag) {
    br_read_ue(&br); /* frame_crop_left_offset */
    br_read_ue(&br); /* frame_crop_right_offset */
    br_read_ue(&br); /* frame_crop_top_offset */
    br_read_ue(&br); /* frame_crop_bottom_offset */
  }

  uint32_t vui_present = br_read_bits(&br, 1);
  if (!vui_present) {
    blog(LOG_WARNING,
         "[H.264 SPS patch] VUI not present — patcher bails out "
         "(would need to inject one and shift subsequent bits)");
    return false;
  }

  /* --- VUI parameters --- */
  if (br_read_bits(&br, 1) /* aspect_ratio_info_present_flag */) {
    uint32_t idc = br_read_bits(&br, 8); /* aspect_ratio_idc */
    if (idc == 255)
      br_skip_bits(&br, 32); /* sar_width + sar_height */
  }
  if (br_read_bits(&br, 1) /* overscan_info_present_flag */)
    br_skip_bits(&br, 1);  /* overscan_appropriate_flag */
  if (br_read_bits(&br, 1) /* video_signal_type_present_flag */) {
    br_skip_bits(&br, 4); /* video_format(3) + video_full_range_flag(1) */
    if (br_read_bits(&br, 1) /* colour_description_present_flag */)
      br_skip_bits(&br, 24); /* 3 × u(8) */
  }
  if (br_read_bits(&br, 1) /* chroma_loc_info_present_flag */) {
    br_read_ue(&br); /* top */
    br_read_ue(&br); /* bottom */
  }
  if (br_read_bits(&br, 1) /* timing_info_present_flag */) {
    br_skip_bits(&br, 64); /* num_units_in_tick + time_scale */
    br_skip_bits(&br, 1);  /* fixed_frame_rate_flag */
  }
  /* hrd_parameters() per H.264 §E.1.2 — skip the scheduler array and capture
   * the cpb/dpb delay length fields we need for pic_timing SEI emission.
   * If both NAL and VCL HRD are present, both define the same length fields
   * (spec requires equality); we record the values from the first HRD seen. */
  uint8_t cpb_len_seen = 0;
  uint8_t dpb_len_seen = 0;

  uint32_t nal_hrd_present = br_read_bits(&br, 1);
  if (nal_hrd_present) {
    uint32_t cpb_cnt_minus1 = br_read_ue(&br);
    if (cpb_cnt_minus1 > 31)
      return false;
    br_skip_bits(&br, 8); /* bit_rate_scale + cpb_size_scale */
    for (uint32_t i = 0; i <= cpb_cnt_minus1; i++) {
      br_read_ue(&br);
      br_read_ue(&br);
      br_skip_bits(&br, 1);
    }
    br_skip_bits(&br, 5);                          /* initial_cpb_removal_delay_length_minus1 */
    cpb_len_seen = (uint8_t)(br_read_bits(&br, 5) + 1); /* cpb_removal_delay_length */
    dpb_len_seen = (uint8_t)(br_read_bits(&br, 5) + 1); /* dpb_output_delay_length */
    br_skip_bits(&br, 5);                          /* time_offset_length */
  }
  uint32_t vcl_hrd_present = br_read_bits(&br, 1);
  if (vcl_hrd_present) {
    uint32_t cpb_cnt_minus1 = br_read_ue(&br);
    if (cpb_cnt_minus1 > 31)
      return false;
    br_skip_bits(&br, 8);
    for (uint32_t i = 0; i <= cpb_cnt_minus1; i++) {
      br_read_ue(&br);
      br_read_ue(&br);
      br_skip_bits(&br, 1);
    }
    br_skip_bits(&br, 5);
    uint8_t cpb_len_vcl = (uint8_t)(br_read_bits(&br, 5) + 1);
    uint8_t dpb_len_vcl = (uint8_t)(br_read_bits(&br, 5) + 1);
    br_skip_bits(&br, 5);
    if (cpb_len_seen == 0) {
      cpb_len_seen = cpb_len_vcl;
      dpb_len_seen = dpb_len_vcl;
    }
  }
  if (nal_hrd_present || vcl_hrd_present)
    br_skip_bits(&br, 1); /* low_delay_hrd_flag */

  *cpb_removal_delay_length_out = cpb_len_seen;
  *dpb_output_delay_length_out = dpb_len_seen;
  *cpb_dpb_delays_present_out = (nal_hrd_present || vcl_hrd_present) != 0;

  if (br.error) {
    blog(LOG_WARNING,
         "[H.264 SPS patch] bit reader error before reaching "
         "pic_struct_present_flag (truncated/malformed SPS?)");
    return false;
  }

  /* Next bit is pic_struct_present_flag. Record position and read its value. */
  size_t pos = br.bit_pos;
  uint32_t value = br_read_bits(&br, 1);
  if (br.error) {
    blog(LOG_WARNING,
         "[H.264 SPS patch] bit reader error reading "
         "pic_struct_present_flag at bit %zu", pos);
    return false;
  }

  blog(LOG_INFO,
       "[H.264 SPS patch] found pic_struct_present_flag at bit %zu, "
       "current value = %u", pos, (unsigned)value);

  *bit_pos_out = pos;
  *current_value = value;
  return true;
}

/* ---------- Public entry point ---------- */

/* Find the next H.264 Annex-B start code starting at `from`. Returns the
 * offset of the byte AFTER the start code (i.e. the first byte of the NAL),
 * or in_size if none found. *sc_len receives 3 or 4. */
static size_t next_nal_offset(const uint8_t *in, size_t in_size, size_t from,
                              size_t *sc_len) {
  for (size_t i = from; i + 2 < in_size; i++) {
    if (in[i] == 0 && in[i + 1] == 0) {
      if (in[i + 2] == 1) {
        *sc_len = 3;
        return i + 3;
      }
      if (i + 3 < in_size && in[i + 2] == 0 && in[i + 3] == 1) {
        *sc_len = 4;
        return i + 4;
      }
    }
  }
  return in_size;
}

bool h264_sps_patch_pic_struct_present(const uint8_t *in, size_t in_size,
                                       uint8_t **out, size_t *out_size,
                                       h264_sps_info_t *info_out) {
  if (info_out)
    memset(info_out, 0, sizeof(*info_out));
  if (!in || !out || !out_size || in_size < 6)
    return false;
  *out = NULL;
  *out_size = 0;

  /* Locate the first SPS NAL. */
  size_t sps_nal_start = 0; /* offset of NAL byte (not start code) */
  size_t sps_sc_len = 0;    /* length of preceding start code (3 or 4) */
  size_t sps_nal_end = 0;   /* offset one past last byte of SPS NAL */
  bool sps_found = false;
  {
    size_t scan = 0;
    while (scan < in_size) {
      size_t sc;
      size_t nal_off = next_nal_offset(in, in_size, scan, &sc);
      if (nal_off >= in_size)
        break;
      uint8_t nal_type = in[nal_off] & 0x1F;
      /* Find end of this NAL: position just before next start code. */
      size_t next_sc;
      size_t next_nal = next_nal_offset(in, in_size, nal_off, &next_sc);
      size_t end = next_nal < in_size ? (next_nal - next_sc) : in_size;
      if (nal_type == 7 && !sps_found) {
        sps_nal_start = nal_off;
        sps_sc_len = sc;
        sps_nal_end = end;
        sps_found = true;
        break;
      }
      scan = end;
    }
  }
  if (!sps_found)
    return false;

  size_t sps_size = sps_nal_end - sps_nal_start; /* includes NAL header byte */

  /* Decode RBSP → SODB (NAL header byte + body without EPBs). */
  uint8_t *sodb = (uint8_t *)bmalloc(sps_size);
  if (!sodb)
    return false;
  size_t sodb_size =
      epb_decode(in + sps_nal_start, sps_size, sodb);

  /* Find pic_struct_present_flag position and capture HRD-derived info. */
  size_t flag_bit_pos = 0;
  uint32_t flag_current = 0;
  bool cpb_dpb_delays_present = false;
  uint8_t cpb_len = 0, dpb_len = 0;
  bool ok = find_pic_struct_present_position(
      sodb, sodb_size, &flag_bit_pos, &flag_current,
      &cpb_dpb_delays_present, &cpb_len, &dpb_len);
  if (!ok) {
    bfree(sodb);
    return false;
  }

  if (info_out) {
    info_out->parsed_ok = true;
    info_out->cpb_dpb_delays_present = cpb_dpb_delays_present;
    info_out->cpb_removal_delay_length = cpb_len;
    info_out->dpb_output_delay_length = dpb_len;
  }

  if (flag_current == 1) {
    /* Already set — nothing to do. */
    blog(LOG_INFO,
         "[H.264 SPS patch] pic_struct_present_flag already set in SPS");
    bfree(sodb);
    return false;
  }

  /* Set the bit to 1. */
  sodb[flag_bit_pos >> 3] |=
      (uint8_t)(1u << (7 - (flag_bit_pos & 7)));

  /* Re-encode SODB → RBSP with EPBs. */
  size_t enc_capacity = sodb_size + sodb_size / 2 + 8;
  uint8_t *new_rbsp = (uint8_t *)bmalloc(enc_capacity);
  if (!new_rbsp) {
    bfree(sodb);
    return false;
  }
  size_t new_rbsp_size = epb_encode(sodb, sodb_size, new_rbsp);
  bfree(sodb);

  /* Reassemble: everything before SPS start code (inclusive of start code)
   * + new SPS NAL bytes + everything from end of original SPS. */
  size_t prefix_size = sps_nal_start;            /* up to start of NAL byte */
  size_t suffix_off = sps_nal_end;
  size_t suffix_size = in_size - suffix_off;
  size_t total = prefix_size + new_rbsp_size + suffix_size;

  uint8_t *result = (uint8_t *)bmalloc(total);
  if (!result) {
    bfree(new_rbsp);
    return false;
  }

  memcpy(result, in, prefix_size);
  memcpy(result + prefix_size, new_rbsp, new_rbsp_size);
  if (suffix_size > 0)
    memcpy(result + prefix_size + new_rbsp_size, in + suffix_off, suffix_size);

  bfree(new_rbsp);

  *out = result;
  *out_size = total;
  (void)sps_sc_len; /* kept for documentation; not needed here */
  return true;
}
