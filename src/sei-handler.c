/******************************************************************************
    SEI Handler Module - Implementation
    Copyright (C) 2026

    Handles SEI (Supplemental Enhancement Information) construction and parsing
******************************************************************************/

#include "sei-handler.h"
#include <obs-module.h> /* blog, bmalloc, LOG_ERROR, LOG_DEBUG */
#include <stdlib.h>
#include <string.h>

/* UUID for our custom SEI: a5b3c2d1-e4f5-6789-abcd-ef0123456789 */
const uint8_t SEI_STAMPER_UUID[16] = {0xa5, 0xb3, 0xc2, 0xd1, 0xe4, 0xf5,
                                      0x67, 0x89, 0xab, 0xcd, 0xef, 0x01,
                                      0x23, 0x45, 0x67, 0x89};

/* 日志宏 */
#define sei_log(level, format, ...)                                            \
  blog(level, "[SEI Handler] " format, ##__VA_ARGS__)

/* 辅助函数:写入可变长度编码(用于SEI size) */
static size_t write_variable_length(uint8_t *buf, size_t value) {
  size_t written = 0;
  while (value >= 0xFF) {
    buf[written++] = 0xFF;
    value -= 0xFF;
  }
  buf[written++] = (uint8_t)value;
  return written;
}

/* 辅助函数:读取可变长度编码 */
static size_t read_variable_length(const uint8_t *buf, size_t max_size,
                                   size_t *value_out) {
  size_t value = 0;
  size_t read = 0;

  while (read < max_size && buf[read] == 0xFF) {
    value += 0xFF;
    read++;
  }

  if (read < max_size) {
    value += buf[read];
    read++;
  }

  *value_out = value;
  return read;
}

/* 构建NTP时间戳SEI payload */
bool build_ntp_sei_payload(int64_t pts, const ntp_timestamp_t *ntp_time,
                           uint8_t **payload_out, size_t *payload_size) {
  if (!ntp_time || !payload_out || !payload_size) {
    sei_log(LOG_ERROR, "Invalid parameters for build_ntp_sei_payload");
    return false;
  }

  /* Payload结构:
   * - UUID: 16字节
   * - PTS: 8字节(int64_t, big-endian)
   * - NTP seconds: 4字节(uint32_t, big-endian)
   * - NTP fraction: 4字节(uint32_t, big-endian)
   * 总计: 32字节
   */
  size_t payload_sz = 16 + 8 + 4 + 4;
  uint8_t *payload = (uint8_t *)bmalloc(payload_sz);
  if (!payload) {
    sei_log(LOG_ERROR, "Failed to allocate memory for SEI payload");
    return false;
  }

  size_t offset = 0;

  /* UUID */
  memcpy(payload + offset, SEI_STAMPER_UUID, 16);
  offset += 16;

  /* PTS (big-endian) */
  payload[offset++] = (uint8_t)((pts >> 56) & 0xFF);
  payload[offset++] = (uint8_t)((pts >> 48) & 0xFF);
  payload[offset++] = (uint8_t)((pts >> 40) & 0xFF);
  payload[offset++] = (uint8_t)((pts >> 32) & 0xFF);
  payload[offset++] = (uint8_t)((pts >> 24) & 0xFF);
  payload[offset++] = (uint8_t)((pts >> 16) & 0xFF);
  payload[offset++] = (uint8_t)((pts >> 8) & 0xFF);
  payload[offset++] = (uint8_t)(pts & 0xFF);

  /* NTP seconds (big-endian) */
  payload[offset++] = (uint8_t)((ntp_time->seconds >> 24) & 0xFF);
  payload[offset++] = (uint8_t)((ntp_time->seconds >> 16) & 0xFF);
  payload[offset++] = (uint8_t)((ntp_time->seconds >> 8) & 0xFF);
  payload[offset++] = (uint8_t)(ntp_time->seconds & 0xFF);

  /* NTP fraction (big-endian) */
  payload[offset++] = (uint8_t)((ntp_time->fraction >> 24) & 0xFF);
  payload[offset++] = (uint8_t)((ntp_time->fraction >> 16) & 0xFF);
  payload[offset++] = (uint8_t)((ntp_time->fraction >> 8) & 0xFF);
  payload[offset++] = (uint8_t)(ntp_time->fraction & 0xFF);

  *payload_out = payload;
  *payload_size = payload_sz;

  return true;
}

/* 构建完整的SEI NAL单元 */
bool build_sei_nal_unit(const uint8_t *payload, size_t payload_size,
                        sei_nal_type_t nal_type, unsigned payload_type,
                        uint8_t **nal_unit_out, size_t *nal_unit_size) {
  if (!payload || !nal_unit_out || !nal_unit_size) {
    sei_log(LOG_ERROR, "Invalid parameters for build_sei_nal_unit");
    return false;
  }

  /* NAL单元结构 (Annex B):
   * - 起始码: 0x00 0x00 0x00 0x01 (4字节，不属于RBSP)
   * - NAL header: 1字节(H.264)或2字节(H.265)
   * - SEI payloadType: 可变长度编码
   * - SEI payloadSize: 记录原始（转义前）字节数，符合 ISO/IEC 23008-2 规范
   * - Payload: 需做EPB (Emulation Prevention Bytes) 转义
   * - RBSP trailing bits: 0x80 (1字节)
   */

  size_t header_size = (nal_type == SEI_NAL_H264) ? 1 : 2;
  uint8_t type_buf[10];
  uint8_t size_buf[10];

  size_t type_len = write_variable_length(type_buf, payload_type);
  /* payloadSize 写入 RBSP 原始长度（EPB转义前），符合规范 */
  size_t size_len = write_variable_length(size_buf, payload_size);

  /* 最坏情况下每2字节的00序列就插入一个EPB（0x03），payload最多膨胀1.5倍
   * 预留足够空间：4(start) + header + type + size + payload*1.5 + 4margin + 1(trailing) */
  size_t max_escaped_payload = payload_size + (payload_size / 2) + 4;
  size_t max_total = 4 + header_size + type_len + size_len + max_escaped_payload + 1;
  uint8_t *nal_unit = (uint8_t *)bmalloc(max_total);
  if (!nal_unit) {
    sei_log(LOG_ERROR, "Failed to allocate memory for SEI NAL unit");
    return false;
  }

  size_t offset = 0;

  /* 起始码（Annex B start code，不属于RBSP，不做EPB计数）*/
  nal_unit[offset++] = 0x00;
  nal_unit[offset++] = 0x00;
  nal_unit[offset++] = 0x00;
  nal_unit[offset++] = 0x01;

  /* NAL header */
  if (nal_type == SEI_NAL_H264) {
    /* H.264: forbidden_bit(1) + nal_ref_idc(2) + nal_unit_type(5) */
    nal_unit[offset++] = (0 << 7) | (0 << 5) | SEI_NAL_H264;
  } else {
    /* H.265: forbidden_zero(1) + nal_unit_type(6) + nuh_layer_id(6) + nuh_temporal_id_plus1(3)
     * Type=39(Prefix SEI) → byte1: 0x4E, LayerId=0,TID=1 → byte2: 0x01 */
    nal_unit[offset++] = (0 << 7) | (nal_type << 1) | 0; /* 0x4E */
    nal_unit[offset++] = (0 << 5) | 1;                   /* 0x01 */
  }

  /* SEI payloadType */
  memcpy(nal_unit + offset, type_buf, type_len);
  offset += type_len;

  /* SEI payloadSize（记录原始未转义长度）*/
  memcpy(nal_unit + offset, size_buf, size_len);
  offset += size_len;

  /* Payload（需做EPB转义）
   * EPB规则（ISO/IEC 23008-2 §7.4.1）：
   *   在RBSP中，若出现 00 00 {00 | 01 | 02 | 03} 序列，
   *   则在第三字节之前插入 emulation prevention byte 0x03。
   * NAL header+type+size 写完后连续零计数重置（因为它们固定值不会以00结尾到达此处）
   * 实际上需要连续跟踪整个RBSP的zero_count，此处保守地从0开始，
   * 对32字节的UUID+时间戳payload是足够的 */
  int zero_count = 0; /* 当前RBSP中连续0x00字节数 */
  for (size_t i = 0; i < payload_size; i++) {
    uint8_t b = payload[i];
    /* 当前已连续>=2个00，且当前字节 <= 0x03，需插入EPB */
    if (zero_count >= 2 && b <= 0x03) {
      nal_unit[offset++] = 0x03;
      zero_count = 0;
    }
    nal_unit[offset++] = b;
    zero_count = (b == 0x00) ? (zero_count + 1) : 0;
  }

  /* RBSP trailing bits: 0x80（bit=1，其余补0以字节对齐）*/
  nal_unit[offset++] = 0x80;

  *nal_unit_out = nal_unit;
  *nal_unit_size = offset;

  sei_log(LOG_DEBUG, "Built SEI NAL unit (%zu bytes, raw payload=%zu bytes)",
          offset, payload_size);

  return true;
}


/* Convert NTP wall-clock to SMPTE 12M timecode for the time-of-day moment.
 *
 * NTP seconds field is seconds since 1900-01-01 UTC. SMPTE 12M only carries
 * HH:MM:SS:FF, so we discard the day component via modulo 86400. The
 * fractional second field is a fixed-point fraction (numerator over 2^32);
 * multiplying by fps gives the sub-second frame number.
 */
void ntp_to_smpte_timecode(const ntp_timestamp_t *ntp,
                           uint32_t fps_num, uint32_t fps_den,
                           smpte_timecode_t *out) {
  if (!ntp || !out || fps_num == 0 || fps_den == 0) {
    if (out) {
      memset(out, 0, sizeof(*out));
    }
    return;
  }

  /* Time-of-day in whole seconds (UTC). */
  uint32_t seconds_in_day = ntp->seconds % 86400u;
  out->hours = (uint8_t)(seconds_in_day / 3600u);
  out->minutes = (uint8_t)((seconds_in_day % 3600u) / 60u);
  out->seconds = (uint8_t)(seconds_in_day % 60u);

  /* Frame number = floor(fraction * fps). NTP fraction is q32, so
   * frame = (fraction * fps_num) / (fps_den * 2^32). The intermediate product
   * fits comfortably in uint64_t for any realistic fps. */
  uint64_t product = (uint64_t)ntp->fraction * (uint64_t)fps_num;
  uint64_t denom = (uint64_t)fps_den * 4294967296ULL;
  uint32_t frame = (uint32_t)(product / denom);

  /* Clamp to ceil(fps_num/fps_den) - 1 just in case of edge rounding. */
  uint32_t fps_ceil = (fps_num + fps_den - 1) / fps_den;
  if (fps_ceil > 0 && frame >= fps_ceil) {
    frame = fps_ceil - 1;
  }
  out->frames = (uint16_t)frame;

  /* Drop-frame applies only to NTSC rates derived from 1001 denominator. */
  out->drop_frame = (fps_den == 1001 &&
                     (fps_num == 30000 || fps_num == 60000));
}

/* Build the 5-byte HEVC time_code SEI payload (one clock timestamp,
 * full_timestamp_flag=1, no time_offset_value). Bit layout per
 * ITU-T H.265 §D.2.27 — total 38 bits + 2 trailing bits = 5 bytes. */
bool build_hevc_time_code_sei_payload(const smpte_timecode_t *tc,
                                      uint8_t **payload_out,
                                      size_t *payload_size) {
  if (!tc || !payload_out || !payload_size) {
    return false;
  }

  /* Fields, packed MSB-first into a 5-byte buffer:
   *   num_clock_ts            u(2)   = 1
   *   clock_timestamp_flag    u(1)   = 1
   *   units_field_based_flag  u(1)   = 0
   *   counting_type           u(5)   = 0
   *   full_timestamp_flag     u(1)   = 1
   *   discontinuity_flag      u(1)   = 0
   *   cnt_dropped_flag        u(1)   = tc->drop_frame
   *   n_frames                u(9)   = tc->frames
   *   seconds_value           u(6)   = tc->seconds
   *   minutes_value           u(6)   = tc->minutes
   *   hours_value             u(5)   = tc->hours
   * Then SEI payload byte-alignment: trailing '1' + zero-fill.
   */

  uint8_t *p = (uint8_t *)bmalloc(5);
  if (!p) {
    return false;
  }
  memset(p, 0, 5);

  /* Pack bits via a running cursor. */
  size_t bit_cursor = 0;
  #define PUT_BITS(value, nbits)                                               \
    do {                                                                       \
      uint32_t _v = (uint32_t)(value);                                         \
      for (int _i = (int)(nbits) - 1; _i >= 0; _i--) {                         \
        uint8_t _b = (uint8_t)((_v >> _i) & 1u);                               \
        p[bit_cursor >> 3] |= (uint8_t)(_b << (7 - (bit_cursor & 7)));         \
        bit_cursor++;                                                          \
      }                                                                        \
    } while (0)

  PUT_BITS(1, 2);                       /* num_clock_ts */
  PUT_BITS(1, 1);                       /* clock_timestamp_flag */
  PUT_BITS(0, 1);                       /* units_field_based_flag */
  PUT_BITS(0, 5);                       /* counting_type */
  PUT_BITS(1, 1);                       /* full_timestamp_flag */
  PUT_BITS(0, 1);                       /* discontinuity_flag */
  PUT_BITS(tc->drop_frame ? 1 : 0, 1);  /* cnt_dropped_flag */
  PUT_BITS(tc->frames & 0x1FF, 9);      /* n_frames */
  PUT_BITS(tc->seconds & 0x3F, 6);      /* seconds_value */
  PUT_BITS(tc->minutes & 0x3F, 6);      /* minutes_value */
  PUT_BITS(tc->hours & 0x1F, 5);        /* hours_value */
  /* time_offset_length is 0 (no VUI override) → no time_offset_value. */

  /* SEI payload byte-alignment: append '1' then zeros to next byte boundary. */
  PUT_BITS(1, 1);
  while ((bit_cursor & 7) != 0) {
    PUT_BITS(0, 1);
  }
  #undef PUT_BITS

  *payload_out = p;
  *payload_size = bit_cursor >> 3;  /* always 5 */
  return true;
}

/* Build the H.264 pic_timing SEI payload. Bit layout per ITU-T H.264 §D.1.2.
 *
 * Total data bits = (cpb_len + dpb_len if delays present, else 0) + 41
 *                   (the 41 fields after pic_struct presence is assumed).
 * Then trailing '1' + zero fill to next byte boundary.
 *
 * For NVENC's default SPS, cpb/dpb lengths are typically 24+24 = 48, giving
 * 89 data bits + 1 trailing + 6 zero pad = 96 bits = 12 bytes. With delays
 * absent, the payload is 6 bytes (legacy size). */
bool build_h264_pic_timing_sei_payload(const smpte_timecode_t *tc,
                                       bool cpb_dpb_delays_present,
                                       uint8_t cpb_removal_delay_length,
                                       uint8_t dpb_output_delay_length,
                                       uint8_t **payload_out,
                                       size_t *payload_size) {
  if (!tc || !payload_out || !payload_size) {
    return false;
  }
  if (cpb_dpb_delays_present) {
    /* Spec range 1..32 inclusive. */
    if (cpb_removal_delay_length < 1 || cpb_removal_delay_length > 32 ||
        dpb_output_delay_length < 1 || dpb_output_delay_length > 32) {
      return false;
    }
  }

  /* Compute total bits + allocate appropriately. */
  size_t prefix_bits =
      cpb_dpb_delays_present
          ? ((size_t)cpb_removal_delay_length + (size_t)dpb_output_delay_length)
          : 0;
  size_t data_bits = prefix_bits + 41;          /* 41 = pic_struct + clock_ts */
  size_t total_bits = data_bits + 1;            /* trailing '1' */
  size_t buf_bytes = (total_bits + 7) / 8;      /* byte-align */

  uint8_t *p = (uint8_t *)bmalloc(buf_bytes);
  if (!p) {
    return false;
  }
  memset(p, 0, buf_bytes);

  size_t bit_cursor = 0;
  #define PUT_BITS(value, nbits)                                               \
    do {                                                                       \
      uint32_t _v = (uint32_t)(value);                                         \
      for (int _i = (int)(nbits) - 1; _i >= 0; _i--) {                         \
        uint8_t _b = (uint8_t)((_v >> _i) & 1u);                               \
        p[bit_cursor >> 3] |= (uint8_t)(_b << (7 - (bit_cursor & 7)));         \
        bit_cursor++;                                                          \
      }                                                                        \
    } while (0)

  /* CPB/DPB delays (zero-valued — we don't simulate the buffer model, but
   * the decoder just reads them; ffmpeg's pic_timing parser then proceeds
   * to the pic_struct fields which it cares about). */
  if (cpb_dpb_delays_present) {
    PUT_BITS(0, cpb_removal_delay_length);
    PUT_BITS(0, dpb_output_delay_length);
  }

  PUT_BITS(0, 4);                       /* pic_struct = 0 (frame) */
  PUT_BITS(1, 1);                       /* clock_timestamp_flag */
  PUT_BITS(0, 2);                       /* ct_type */
  PUT_BITS(0, 1);                       /* nuit_field_based_flag */
  PUT_BITS(0, 5);                       /* counting_type */
  PUT_BITS(1, 1);                       /* full_timestamp_flag */
  PUT_BITS(0, 1);                       /* discontinuity_flag */
  PUT_BITS(tc->drop_frame ? 1 : 0, 1);  /* cnt_dropped_flag */
  PUT_BITS(tc->frames & 0xFF, 8);       /* n_frames (8 bits in H.264) */
  PUT_BITS(tc->seconds & 0x3F, 6);
  PUT_BITS(tc->minutes & 0x3F, 6);
  PUT_BITS(tc->hours & 0x1F, 5);

  PUT_BITS(1, 1);                       /* trailing '1' */
  while ((bit_cursor & 7) != 0) {
    PUT_BITS(0, 1);
  }
  #undef PUT_BITS

  *payload_out = p;
  *payload_size = bit_cursor >> 3;
  return true;
}

/* Build the per-frame SEI bundle (timecode + optional UUID). */
bool build_sei_bundle(int codec_type, bool is_keyframe,
                      int64_t pts, const ntp_timestamp_t *ntp_time,
                      uint32_t fps_num, uint32_t fps_den,
                      const sei_bundle_codec_info_t *codec_info,
                      uint8_t **bundle_out, size_t *bundle_size) {
  if (!bundle_out || !bundle_size) {
    return false;
  }
  *bundle_out = NULL;
  *bundle_size = 0;

  uint8_t *tc_nal = NULL;
  size_t tc_nal_size = 0;
  uint8_t *uuid_nal = NULL;
  size_t uuid_nal_size = 0;

  /* Standards-compliant timecode SEI — emitted every frame so stock
   * ffmpeg surfaces AV_FRAME_DATA_S12M_TIMECODE on every decoded frame —
   * the data VOD post-processing pipelines read to derive HLS
   * PROGRAM-DATE-TIME.
   *   - HEVC:  time_code SEI (payload type 136, ITU-T H.265 §D.2.27).
   *   - H.264: pic_timing SEI (payload type 1, ITU-T H.264 §D.1.2).
   *            Requires pic_struct_present_flag=1 in the SPS VUI — set
   *            via the h264_metadata BSF at encoder init (see
   *            nvenc-encoder.c).
   *   - AV1:   out of scope; metadata OBUs are a separate mechanism. */
  if (codec_type == 1 && ntp_time) {
    smpte_timecode_t tc;
    ntp_to_smpte_timecode(ntp_time, fps_num, fps_den, &tc);
    uint8_t *tc_payload = NULL;
    size_t tc_payload_size = 0;
    if (build_hevc_time_code_sei_payload(&tc, &tc_payload, &tc_payload_size)) {
      build_sei_nal_unit(tc_payload, tc_payload_size, SEI_NAL_H265_PREFIX,
                         SEI_TYPE_TIME_CODE_HEVC, &tc_nal, &tc_nal_size);
      bfree(tc_payload);
    }
  } else if (codec_type == 0 && ntp_time) {
    smpte_timecode_t tc;
    ntp_to_smpte_timecode(ntp_time, fps_num, fps_den, &tc);
    uint8_t *pt_payload = NULL;
    size_t pt_payload_size = 0;
    bool cpb_dpb = codec_info && codec_info->h264_cpb_dpb_delays_present;
    uint8_t cpb_len = codec_info ? codec_info->h264_cpb_removal_delay_length : 0;
    uint8_t dpb_len = codec_info ? codec_info->h264_dpb_output_delay_length : 0;
    if (build_h264_pic_timing_sei_payload(&tc, cpb_dpb, cpb_len, dpb_len,
                                          &pt_payload, &pt_payload_size)) {
      build_sei_nal_unit(pt_payload, pt_payload_size, SEI_NAL_H264,
                         SEI_TYPE_PIC_TIMING, &tc_nal, &tc_nal_size);
      bfree(pt_payload);
    }
  }

  /* UUID SEI — only at keyframes (live-sync receiver only needs cadence,
   * matches current v1.2.2 behaviour bit-for-bit). */
  if (is_keyframe && ntp_time) {
    uint8_t *uuid_payload = NULL;
    size_t uuid_payload_size = 0;
    if (build_ntp_sei_payload(pts, ntp_time, &uuid_payload, &uuid_payload_size)) {
      sei_nal_type_t nal_type = (codec_type == 1) ? SEI_NAL_H265_PREFIX
                                                  : SEI_NAL_H264;
      build_sei_nal_unit(uuid_payload, uuid_payload_size, nal_type,
                         SEI_TYPE_USER_DATA_UNREGISTERED,
                         &uuid_nal, &uuid_nal_size);
      bfree(uuid_payload);
    }
  }

  size_t total = tc_nal_size + uuid_nal_size;
  if (total == 0) {
    return true;  /* No SEI for this frame — not an error. */
  }

  uint8_t *bundle = (uint8_t *)bmalloc(total);
  if (!bundle) {
    if (tc_nal) bfree(tc_nal);
    if (uuid_nal) bfree(uuid_nal);
    return false;
  }
  size_t off = 0;
  if (tc_nal_size > 0) {
    memcpy(bundle + off, tc_nal, tc_nal_size);
    off += tc_nal_size;
    bfree(tc_nal);
  }
  if (uuid_nal_size > 0) {
    memcpy(bundle + off, uuid_nal, uuid_nal_size);
    bfree(uuid_nal);
  }

  *bundle_out = bundle;
  *bundle_size = total;
  return true;
}

/* 合并SEI数据 */
bool merge_sei_data(const uint8_t *original_sei, size_t original_size,
                    const uint8_t *custom_sei, size_t custom_size,
                    uint8_t **merged_sei_out, size_t *merged_size) {
  if (!custom_sei || !merged_sei_out || !merged_size) {
    sei_log(LOG_ERROR, "Invalid parameters for merge_sei_data");
    return false;
  }

  /* 如果没有原始SEI,直接返回自定义SEI */
  if (!original_sei || original_size == 0) {
    uint8_t *merged = (uint8_t *)bmalloc(custom_size);
    if (!merged) {
      return false;
    }
    memcpy(merged, custom_sei, custom_size);
    *merged_sei_out = merged;
    *merged_size = custom_size;
    return true;
  }

  /* 合并:自定义SEI + 原始SEI */
  size_t total_size = custom_size + original_size;
  uint8_t *merged = (uint8_t *)bmalloc(total_size);
  if (!merged) {
    sei_log(LOG_ERROR, "Failed to allocate memory for merged SEI");
    return false;
  }

  memcpy(merged, custom_sei, custom_size);
  memcpy(merged + custom_size, original_sei, original_size);

  *merged_sei_out = merged;
  *merged_size = total_size;

  sei_log(LOG_DEBUG, "Merged SEI data (custom: %zu, original: %zu, total: %zu)",
          custom_size, original_size, total_size);

  return true;
}

/* 从SEI payload中解析NTP时间戳 */
bool parse_ntp_sei(const uint8_t *sei_data, size_t sei_size,
                   ntp_sei_data_t *ntp_data_out) {
  if (!sei_data || !ntp_data_out) {
    return false;
  }

  uint8_t unescaped[2048];
  size_t unescaped_size = 0;
  for (size_t i = 0; i < sei_size; i++) {
    if (i >= 2 && sei_data[i] == 0x03 && sei_data[i-1] == 0 && sei_data[i-2] == 0) {
      continue;
    }
    if (unescaped_size < sizeof(unescaped)) {
      unescaped[unescaped_size++] = sei_data[i];
    }
  }

  /* 查找我们的UUID */
  for (size_t i = 0; i + 32 <= unescaped_size; i++) {
    if (memcmp(unescaped + i, SEI_STAMPER_UUID, 16) == 0) {
      /* 找到了!解析数据 */
      size_t offset = i;

      /* UUID */
      memcpy(ntp_data_out->uuid, unescaped + offset, 16);
      offset += 16;

      /* PTS */
      int64_t pts = 0;
      pts |= ((int64_t)unescaped[offset++] << 56);
      pts |= ((int64_t)unescaped[offset++] << 48);
      pts |= ((int64_t)unescaped[offset++] << 40);
      pts |= ((int64_t)unescaped[offset++] << 32);
      pts |= ((int64_t)unescaped[offset++] << 24);
      pts |= ((int64_t)unescaped[offset++] << 16);
      pts |= ((int64_t)unescaped[offset++] << 8);
      pts |= (int64_t)unescaped[offset++];
      ntp_data_out->pts = pts;

      /* NTP seconds */
      uint32_t seconds = 0;
      seconds |= ((uint32_t)unescaped[offset++] << 24);
      seconds |= ((uint32_t)unescaped[offset++] << 16);
      seconds |= ((uint32_t)unescaped[offset++] << 8);
      seconds |= (uint32_t)unescaped[offset++];
      ntp_data_out->ntp_time.seconds = seconds;

      /* NTP fraction */
      uint32_t fraction = 0;
      fraction |= ((uint32_t)unescaped[offset++] << 24);
      fraction |= ((uint32_t)unescaped[offset++] << 16);
      fraction |= ((uint32_t)unescaped[offset++] << 8);
      fraction |= (uint32_t)unescaped[offset++];
      ntp_data_out->ntp_time.fraction = fraction;

      sei_log(LOG_DEBUG, "Parsed NTP SEI (PTS: %lld, NTP: %u.%u)", pts, seconds,
              fraction);

      return true;
    }
  }

  return false;
}

/* 从NAL单元中提取SEI payload */
static const uint8_t *find_start_code_sei(const uint8_t *data, size_t size, size_t *sc_size) {
  if (size < 3) return NULL;
  for (size_t i = 0; i < size - 2; i++) {
    if (data[i] == 0 && data[i+1] == 0) {
      if (data[i+2] == 1) {
        *sc_size = 3; return data + i;
      } else if (i < size - 3 && data[i+2] == 0 && data[i+3] == 1) {
        *sc_size = 4; return data + i;
      }
    }
  }
  return NULL;
}

bool extract_sei_payload(const uint8_t *nal_data, size_t nal_size,
                         const uint8_t **payload_out, size_t *payload_size) {
  if (!nal_data || !payload_out || !payload_size || nal_size < 5) {
    return false;
  }

  const uint8_t *current = nal_data;
  size_t remaining = nal_size;

  while (remaining > 0) {
    size_t sc_size = 0;
    const uint8_t *nal_start = find_start_code_sei(current, remaining, &sc_size);
    if (!nal_start) break;

    const uint8_t *data = nal_start + sc_size;
    size_t data_rem = remaining - (data - current);
    if (data_rem < 2) break; /* Need at least 2 bytes for H.265 header */

    uint8_t nal_type_byte = data[0];
    uint8_t nal_type = nal_type_byte & 0x1F;
    size_t offset = 0;
    bool is_sei = false;

    if (nal_type == SEI_NAL_H264) {
      is_sei = true;
      offset = 1;
    } else {
      uint8_t h265_type = (nal_type_byte >> 1) & 0x3F;
      if (h265_type == SEI_NAL_H265_PREFIX || h265_type == SEI_NAL_H265_SUFFIX) {
        is_sei = true;
        offset = 2;
      }
    }

    if (is_sei && data_rem > offset) {
      size_t sei_type;
      size_t type_read = read_variable_length(data + offset, data_rem - offset, &sei_type);
      offset += type_read;
      
      size_t sei_size;
      size_t size_read = read_variable_length(data + offset, data_rem - offset, &sei_size);
      offset += size_read;

      if (sei_type == SEI_TYPE_USER_DATA_UNREGISTERED && offset + sei_size <= data_rem) {
        *payload_out = data + offset;
        *payload_size = data_rem - offset;
        return true;
      }
    }

    current = nal_start + 1; // Advance past current start code
    remaining = nal_size - (current - nal_data);
  }

  return false;
}
