/******************************************************************************
    SEI Handler Module - Header File
    Copyright (C) 2026

    Handles SEI (Supplemental Enhancement Information) construction and parsing
    for NTP timestamp embedding in H.264/H.265 video streams
******************************************************************************/

#pragma once

#include "ntp-client.h"
#include <stdbool.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

/* UUID for our custom SEI (用于识别我们的自定义SEI) */
/* 格式: a5b3c2d1-e4f5-6789-abcd-ef0123456789 */
extern const uint8_t SEI_STAMPER_UUID[16];

/* NTP时间戳SEI数据结构 */
typedef struct ntp_sei_data {
  uint8_t uuid[16];         /* UUID标识符 */
  int64_t pts;              /* 帧的PTS */
  ntp_timestamp_t ntp_time; /* NTP时间戳 */
} ntp_sei_data_t;

/* SMPTE 12M timecode for HEVC time_code SEI payloads. */
typedef struct smpte_timecode {
  uint8_t hours;     /* 0-23 (UTC wall-clock) */
  uint8_t minutes;   /* 0-59 */
  uint8_t seconds;   /* 0-59 */
  uint16_t frames;   /* 0..fps-1; HEVC time_code reserves 9 bits */
  bool drop_frame;   /* true for 29.97 / 59.94 fps modes */
} smpte_timecode_t;

/* SEI NAL单元类型 */
typedef enum sei_nal_type {
  SEI_NAL_H264 = 6,         /* H.264 SEI NAL单元类型 */
  SEI_NAL_H265_PREFIX = 39, /* H.265 PREFIX_SEI_NUT */
  SEI_NAL_H265_SUFFIX = 40  /* H.265 SUFFIX_SEI_NUT */
} sei_nal_type_t;

/* SEI payload类型 */
#define SEI_TYPE_USER_DATA_UNREGISTERED 5
#define SEI_TYPE_PIC_TIMING             1   /* H.264 / HEVC pic_timing */
#define SEI_TYPE_TIME_CODE_HEVC         136 /* HEVC time_code (ITU-T H.265 D.2.27) */

/*
 * 构建NTP时间戳SEI payload
 * 参数:
 *   pts - 当前帧的PTS
 *   ntp_time - NTP时间戳
 *   payload_out - 输出的SEI payload数据(需要调用者释放)
 *   payload_size - 输出的payload大小
 * 返回:
 *   true - 成功
 *   false - 失败
 */
bool build_ntp_sei_payload(int64_t pts, const ntp_timestamp_t *ntp_time,
                           uint8_t **payload_out, size_t *payload_size);

/*
 * 构建完整的SEI NAL单元(包含起始码)
 * 参数:
 *   payload - SEI payload数据 (raw, will be EPB-escaped automatically)
 *   payload_size - payload大小 (before escaping)
 *   nal_type - NAL单元类型(H264或H265)
 *   payload_type - SEI payload type (see SEI_TYPE_* macros)
 *   nal_unit_out - 输出的完整NAL单元(需要调用者释放)
 *   nal_unit_size - 输出的NAL单元大小
 * 返回:
 *   true - 成功
 *   false - 失败
 */
bool build_sei_nal_unit(const uint8_t *payload, size_t payload_size,
                        sei_nal_type_t nal_type, unsigned payload_type,
                        uint8_t **nal_unit_out, size_t *nal_unit_size);

/*
 * Convert an NTP timestamp + frame rate to a SMPTE 12M wall-clock timecode.
 * fps_num/fps_den give the encoder's frame rate (e.g. 30/1, 60000/1001).
 * Non-drop-frame counting is used regardless of rate; the drop_frame flag is
 * signalled in the output so downstream tooling can recompute if it needs
 * strict NTSC-compliant numbering. For wall-clock-aligned VOD timestamping
 * (PROGRAM-DATE-TIME), the non-drop value is what's wanted.
 *
 * Returns the time-of-day timecode for the NTP moment (00:00:00 - 23:59:59
 * UTC); the day component is discarded.
 */
void ntp_to_smpte_timecode(const ntp_timestamp_t *ntp,
                           uint32_t fps_num, uint32_t fps_den,
                           smpte_timecode_t *out);

/*
 * Build a HEVC time_code SEI payload (ITU-T H.265 §D.2.27 / D.3.27) with one
 * clock timestamp, full_timestamp_flag=1, no time_offset. The payload is the
 * raw 5-byte SEI message body (no NAL header, no start code, no EPB escaping
 * — pass it to build_sei_nal_unit with SEI_TYPE_TIME_CODE_HEVC for that).
 */
bool build_hevc_time_code_sei_payload(const smpte_timecode_t *tc,
                                      uint8_t **payload_out,
                                      size_t *payload_size);

/*
 * Build the per-frame SEI NAL bundle for an encoded packet. The bundle is a
 * single bmalloc'd buffer containing zero or more concatenated Annex-B SEI
 * NALs in this order:
 *
 *   [HEVC time_code SEI]   (codec_type==1 only, every frame)
 *   [UUID NTP SEI]         (is_keyframe only, every codec)
 *
 * Callers insert the bundle into the encoded packet between the parameter
 * sets and the slice (for keyframes) or before the slice (for non-keyframes).
 * On a non-keyframe with codec_type != 1, *bundle_size will be 0 and the
 * pointer will be NULL — callers should treat that as "no insertion needed".
 *
 * codec_type: 0 = H.264, 1 = H.265, 2 = AV1
 * Returns false only on allocation failure.
 */
bool build_sei_bundle(int codec_type, bool is_keyframe,
                      int64_t pts, const ntp_timestamp_t *ntp_time,
                      uint32_t fps_num, uint32_t fps_den,
                      uint8_t **bundle_out, size_t *bundle_size);

/*
 * 合并SEI数据(将自定义SEI与原有SEI合并)
 * 参数:
 *   original_sei - 原始SEI数据(可以为NULL)
 *   original_size - 原始SEI大小
 *   custom_sei - 自定义SEI数据
 *   custom_size - 自定义SEI大小
 *   merged_sei_out - 输出的合并后SEI(需要调用者释放)
 *   merged_size - 输出的合并后大小
 * 返回:
 *   true - 成功
 *   false - 失败
 */
bool merge_sei_data(const uint8_t *original_sei, size_t original_size,
                    const uint8_t *custom_sei, size_t custom_size,
                    uint8_t **merged_sei_out, size_t *merged_size);

/*
 * 从SEI payload中解析NTP时间戳
 * 参数:
 *   sei_data - SEI数据
 *   sei_size - SEI数据大小
 *   ntp_data_out - 输出的NTP SEI数据
 * 返回:
 *   true - 成功找到并解析
 *   false - 未找到或解析失败
 */
bool parse_ntp_sei(const uint8_t *sei_data, size_t sei_size,
                   ntp_sei_data_t *ntp_data_out);

/*
 * 从NAL单元中提取SEI payload
 * 参数:
 *   nal_data - NAL单元数据
 *   nal_size - NAL单元大小
 *   payload_out - 输出的payload数据(指向nal_data内部,不需要释放)
 *   payload_size - 输出的payload大小
 * 返回:
 *   true - 成功
 *   false - 失败
 */
bool extract_sei_payload(const uint8_t *nal_data, size_t nal_size,
                         const uint8_t **payload_out, size_t *payload_size);

#ifdef __cplusplus
}
#endif
