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

/* ---------- Bit writer (for VUI injection) ---------- */

/* Insert `nbits` from `value` (MSB-first) into `buf` at bit position `*cursor`,
 * advancing the cursor. buf must be large enough. */
static void bw_put_bits(uint8_t *buf, size_t *cursor, uint32_t value, int nbits) {
  for (int i = nbits - 1; i >= 0; i--) {
    uint8_t b = (uint8_t)((value >> i) & 1u);
    buf[*cursor >> 3] |= (uint8_t)(b << (7 - (*cursor & 7)));
    (*cursor)++;
  }
}

/* ---------- SPS navigator ---------- */

/* Result codes from find_pic_struct_present_position. */
typedef enum {
  SPS_FIND_ERROR = 0,     /* parse failure */
  SPS_FIND_VUI_PRESENT,   /* VUI exists; bit_pos_out points at pic_struct_present_flag */
  SPS_FIND_NO_VUI,        /* VUI absent; bit_pos_out points at vui_parameters_present_flag (=0) */
} sps_find_result_t;

/* Walk the SODB (NAL header byte + decoded RBSP body) to find either the
 * pic_struct_present_flag (when VUI exists) or the vui_parameters_present_flag
 * (when VUI is absent). Bails out on:
 *   - scaling matrix in SPS (would require full scaling-list parsing)
 *   - parser hit truncated data
 *
 * On SPS_FIND_VUI_PRESENT, *bit_pos_out is the position of pic_struct_present_flag
 * and *current_value is its value. On SPS_FIND_NO_VUI, *bit_pos_out is the
 * position of vui_parameters_present_flag (which is 0).
 */
static sps_find_result_t find_pic_struct_present_position(
                                             const uint8_t *sodb,
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

  size_t vui_flag_pos = br.bit_pos;
  uint32_t vui_present = br_read_bits(&br, 1);
  if (!vui_present) {
    blog(LOG_INFO,
         "[H.264 SPS patch] VUI not present at bit %zu — will inject "
         "minimal VUI", vui_flag_pos);
    *bit_pos_out = vui_flag_pos;
    *current_value = 0;
    *cpb_dpb_delays_present_out = false;
    *cpb_removal_delay_length_out = 0;
    *dpb_output_delay_length_out = 0;
    return SPS_FIND_NO_VUI;
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
    return SPS_FIND_ERROR;
  }

  /* Next bit is pic_struct_present_flag. Record position and read its value. */
  size_t pos = br.bit_pos;
  uint32_t value = br_read_bits(&br, 1);
  if (br.error) {
    blog(LOG_WARNING,
         "[H.264 SPS patch] bit reader error reading "
         "pic_struct_present_flag at bit %zu", pos);
    return SPS_FIND_ERROR;
  }

  blog(LOG_INFO,
       "[H.264 SPS patch] found pic_struct_present_flag at bit %zu, "
       "current value = %u", pos, (unsigned)value);

  *bit_pos_out = pos;
  *current_value = value;
  return SPS_FIND_VUI_PRESENT;
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
  sps_find_result_t find_result = find_pic_struct_present_position(
      sodb, sodb_size, &flag_bit_pos, &flag_current,
      &cpb_dpb_delays_present, &cpb_len, &dpb_len);
  if (find_result == SPS_FIND_ERROR) {
    bfree(sodb);
    return false;
  }

  if (info_out) {
    info_out->parsed_ok = true;
    info_out->cpb_dpb_delays_present = cpb_dpb_delays_present;
    info_out->cpb_removal_delay_length = cpb_len;
    info_out->dpb_output_delay_length = dpb_len;
  }

  if (find_result == SPS_FIND_VUI_PRESENT && flag_current == 1) {
    blog(LOG_INFO,
         "[H.264 SPS patch] pic_struct_present_flag already set in SPS");
    bfree(sodb);
    return false;
  }

  /* We need to produce a patched SODB. Two cases:
   *
   * SPS_FIND_VUI_PRESENT: VUI exists but pic_struct_present_flag=0.
   *   → flip the single bit at flag_bit_pos from 0 to 1.
   *
   * SPS_FIND_NO_VUI: vui_parameters_present_flag=0 at flag_bit_pos.
   *   → flip that bit to 1, then splice in a 9-bit minimal VUI body
   *     immediately after it. The minimal VUI per H.264 §E.1.1:
   *       aspect_ratio_info_present_flag  = 0   (1 bit)
   *       overscan_info_present_flag      = 0   (1 bit)
   *       video_signal_type_present_flag  = 0   (1 bit)
   *       chroma_loc_info_present_flag    = 0   (1 bit)
   *       timing_info_present_flag        = 0   (1 bit)
   *       nal_hrd_parameters_present_flag = 0   (1 bit)
   *       vcl_hrd_parameters_present_flag = 0   (1 bit)
   *       pic_struct_present_flag         = 1   (1 bit)
   *       bitstream_restriction_flag      = 0   (1 bit)
   *     No HRD → cpb_dpb_delays_present remains false.
   */

  uint8_t *new_sodb = NULL;
  size_t new_sodb_size = 0;

  if (find_result == SPS_FIND_VUI_PRESENT) {
    /* Simple case: flip the existing bit. */
    sodb[flag_bit_pos >> 3] |=
        (uint8_t)(1u << (7 - (flag_bit_pos & 7)));
    new_sodb = sodb;
    new_sodb_size = sodb_size;
    sodb = NULL; /* transferred ownership */
  } else {
    /* SPS_FIND_NO_VUI: splice in vui_parameters_present_flag=1 + 9-bit VUI
     * body after the flag position. Everything after the flag bit (RBSP
     * trailing bits, possibly RBSP stop bit) must shift right by 9 bits. */
    size_t insert_pos = flag_bit_pos;    /* position of vui_parameters_present_flag */
    size_t tail_start = insert_pos + 1;  /* first bit after the flag */
    size_t tail_bits = sodb_size * 8 - tail_start;

    /* New SODB: original bits up to and including insert_pos (which we'll
     * set to 1), then 9 bits of VUI, then the remaining tail bits. */
    size_t new_total_bits = insert_pos + 1 + 9 + tail_bits;
    new_sodb_size = (new_total_bits + 7) / 8;
    new_sodb = (uint8_t *)bmalloc(new_sodb_size);
    if (!new_sodb) {
      bfree(sodb);
      return false;
    }
    memset(new_sodb, 0, new_sodb_size);

    /* Copy bits [0, insert_pos) from old SODB. */
    for (size_t i = 0; i < insert_pos; i++) {
      uint8_t b = (sodb[i >> 3] >> (7 - (i & 7))) & 1u;
      new_sodb[i >> 3] |= (uint8_t)(b << (7 - (i & 7)));
    }

    /* Write vui_parameters_present_flag = 1. */
    size_t cursor = insert_pos;
    bw_put_bits(new_sodb, &cursor, 1, 1);

    /* Write minimal VUI body (9 bits): 7 "not present" flags + pic_struct=1 + restriction=0 */
    bw_put_bits(new_sodb, &cursor, 0, 1);  /* aspect_ratio_info_present_flag */
    bw_put_bits(new_sodb, &cursor, 0, 1);  /* overscan_info_present_flag */
    bw_put_bits(new_sodb, &cursor, 0, 1);  /* video_signal_type_present_flag */
    bw_put_bits(new_sodb, &cursor, 0, 1);  /* chroma_loc_info_present_flag */
    bw_put_bits(new_sodb, &cursor, 0, 1);  /* timing_info_present_flag */
    bw_put_bits(new_sodb, &cursor, 0, 1);  /* nal_hrd_parameters_present_flag */
    bw_put_bits(new_sodb, &cursor, 0, 1);  /* vcl_hrd_parameters_present_flag */
    bw_put_bits(new_sodb, &cursor, 1, 1);  /* pic_struct_present_flag */
    bw_put_bits(new_sodb, &cursor, 0, 1);  /* bitstream_restriction_flag */

    /* Copy remaining tail bits (RBSP trailing bits from original SPS). */
    for (size_t i = 0; i < tail_bits; i++) {
      size_t src_bit = tail_start + i;
      uint8_t b = (sodb[src_bit >> 3] >> (7 - (src_bit & 7))) & 1u;
      new_sodb[cursor >> 3] |= (uint8_t)(b << (7 - (cursor & 7)));
      cursor++;
    }

    bfree(sodb);
    blog(LOG_INFO,
         "[H.264 SPS patch] injected minimal VUI (9 bits) at bit %zu; "
         "new SODB %zu bytes (was %zu)",
         insert_pos, new_sodb_size, sodb_size);
  }

  /* Re-encode SODB → RBSP with EPBs. */
  size_t enc_capacity = new_sodb_size + new_sodb_size / 2 + 8;
  uint8_t *new_rbsp = (uint8_t *)bmalloc(enc_capacity);
  if (!new_rbsp) {
    bfree(new_sodb);
    return false;
  }
  size_t new_rbsp_size = epb_encode(new_sodb, new_sodb_size, new_rbsp);
  bfree(new_sodb);

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
