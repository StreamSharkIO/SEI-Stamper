#include "videotoolbox-encoder.h"
#include "h264-sps.h"
#include <util/dstr.h>
#include <util/platform.h>

#ifdef ENABLE_VIDEOTOOLBOX

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sei-handler.h"
#include "timecode-render.h"

#define encoder_log(level, enc, format, ...)                                   \
  blog(level, "[VT Encoder: '%s'] " format,                                   \
       obs_encoder_get_name(enc->encoder), ##__VA_ARGS__)

/* Translate unified UI preset to x264 preset */
static const char *vt_translate_x264_preset(const char *in) {
  if (!in || !*in)
    return "medium";
  if (strcmp(in, "fast") == 0)
    return "veryfast";
  if (strcmp(in, "balanced") == 0)
    return "medium";
  if (strcmp(in, "quality") == 0)
    return "slow";
  return "medium";
}

/* Convert FFmpeg extradata to Annex-B (same logic as nvenc_extradata_to_annexb).
 * Returns a bmalloc'd buffer, or NULL on failure. */
static uint8_t *vt_extradata_to_annexb(const uint8_t *extradata,
                                       size_t extradata_size,
                                       size_t *out_size) {
  *out_size = 0;
  if (!extradata || extradata_size < 4)
    return NULL;

  bool is_annexb =
      (extradata[0] == 0 && extradata[1] == 0 &&
       ((extradata[2] == 0 && extradata[3] == 1) || extradata[2] == 1));
  if (is_annexb) {
    uint8_t *out = bmalloc(extradata_size);
    memcpy(out, extradata, extradata_size);
    *out_size = extradata_size;
    return out;
  }

  if (extradata[0] != 0x01 || extradata_size < 7)
    return NULL;

  size_t pos = 5;
  int num_sps = extradata[pos++] & 0x1F;
  size_t total = 0;

  size_t scan = pos;
  for (int i = 0; i < num_sps; i++) {
    if (scan + 2 > extradata_size)
      return NULL;
    uint16_t len = ((uint16_t)extradata[scan] << 8) | extradata[scan + 1];
    scan += 2;
    if (scan + len > extradata_size)
      return NULL;
    total += 4 + len;
    scan += len;
  }
  if (scan + 1 > extradata_size)
    return NULL;
  int num_pps = extradata[scan++];
  for (int i = 0; i < num_pps; i++) {
    if (scan + 2 > extradata_size)
      return NULL;
    uint16_t len = ((uint16_t)extradata[scan] << 8) | extradata[scan + 1];
    scan += 2;
    if (scan + len > extradata_size)
      return NULL;
    total += 4 + len;
    scan += len;
  }
  if (total == 0)
    return NULL;

  uint8_t *out = bmalloc(total);
  uint8_t *wp = out;
  scan = pos;
  for (int i = 0; i < num_sps; i++) {
    uint16_t len = ((uint16_t)extradata[scan] << 8) | extradata[scan + 1];
    scan += 2;
    *wp++ = 0; *wp++ = 0; *wp++ = 0; *wp++ = 1;
    memcpy(wp, extradata + scan, len);
    wp += len;
    scan += len;
  }
  scan++;
  for (int i = 0; i < num_pps; i++) {
    uint16_t len = ((uint16_t)extradata[scan] << 8) | extradata[scan + 1];
    scan += 2;
    *wp++ = 0; *wp++ = 0; *wp++ = 0; *wp++ = 1;
    memcpy(wp, extradata + scan, len);
    wp += len;
    scan += len;
  }

  *out_size = total;
  return out;
}

/* Patch H.264 SPS VUI to set pic_struct_present_flag=1 */
static void vt_patch_h264_sps_vui(vt_encoder_t *enc, uint8_t **extradata,
                                  size_t *extradata_size) {
  uint8_t *patched = NULL;
  size_t patched_size = 0;
  h264_sps_info_t info = {0};
  bool applied = h264_sps_patch_pic_struct_present(
      *extradata, *extradata_size, &patched, &patched_size, &info);
  if (applied) {
    bfree(*extradata);
    *extradata = patched;
    *extradata_size = patched_size;
    encoder_log(LOG_INFO, enc,
                "Patched H.264 SPS VUI (pic_struct_present_flag=1)");
  } else if (info.parsed_ok) {
    encoder_log(LOG_INFO, enc,
                "H.264 SPS VUI patch not needed (already set)");
  } else {
    encoder_log(LOG_WARNING, enc,
                "H.264 SPS parse failed; pic_timing SEI may not surface");
  }

  if (info.parsed_ok) {
    enc->h264_cpb_dpb_delays_present = info.cpb_dpb_delays_present;
    enc->h264_cpb_removal_delay_length = info.cpb_removal_delay_length;
    enc->h264_dpb_output_delay_length = info.dpb_output_delay_length;
  }
}

/* Find start code */
static const uint8_t *find_nal_start_code_vt(const uint8_t *data, size_t size,
                                             size_t *start_code_size) {
  if (size < 3)
    return NULL;
  for (size_t i = 0; i < size - 2; i++) {
    if (data[i] == 0 && data[i + 1] == 0) {
      if (data[i + 2] == 1) {
        *start_code_size = 3;
        return data + i;
      } else if (i < size - 3 && data[i + 2] == 0 && data[i + 3] == 1) {
        *start_code_size = 4;
        return data + i;
      }
    }
  }
  return NULL;
}

/* Find offset of first VCL NAL (slice) — insert SEI before this */
static size_t find_first_vcl_offset_vt(const uint8_t *data, size_t size) {
  if (size < 4)
    return size;
  size_t scan = 0;
  while (scan < size) {
    size_t sc_size = 0;
    const uint8_t *sc =
        find_nal_start_code_vt(data + scan, size - scan, &sc_size);
    if (!sc)
      return size;
    size_t sc_off = (size_t)(sc - data);
    size_t header_off = sc_off + sc_size;
    if (header_off >= size)
      return size;
    uint8_t t = data[header_off] & 0x1F;
    if (t == 1 || t == 5 || t == 19 || t == 20 || t == 21)
      return sc_off;
    scan = header_off;
  }
  return size;
}

/* Find end of parameter sets (SPS/PPS) */
static size_t find_parameter_sets_end_vt(const uint8_t *data, size_t size) {
  const uint8_t *current = data;
  size_t remaining = size;
  size_t last_param_end = 0;
  while (remaining > 0) {
    size_t sc_size = 0;
    const uint8_t *nal_start =
        find_nal_start_code_vt(current, remaining, &sc_size);
    if (!nal_start)
      break;
    const uint8_t *nal_data = nal_start + sc_size;
    size_t nal_remaining = remaining - (nal_data - current);
    if (nal_remaining < 1)
      break;
    uint8_t nal_type = nal_data[0] & 0x1F;
    bool is_param_set = (nal_type == 7 || nal_type == 8 || nal_type == 9);
    size_t next_sc_size = 0;
    const uint8_t *next_nal =
        find_nal_start_code_vt(nal_data, nal_remaining, &next_sc_size);
    if (is_param_set) {
      last_param_end = next_nal ? (size_t)(next_nal - data) : size;
    } else {
      return last_param_end;
    }
    if (!next_nal)
      break;
    current = next_nal;
    remaining = size - (current - data);
  }
  return last_param_end;
}

void vt_encoder_destroy(vt_encoder_t *enc) {
  if (!enc)
    return;

  encoder_log(LOG_INFO, enc, "Destroying %s encoder",
              enc->is_hardware ? "VideoToolbox" : "x264");

  if (enc->codec_context)
    avcodec_free_context(&enc->codec_context);
  if (enc->frame)
    av_frame_free(&enc->frame);
  if (enc->packet)
    av_packet_free(&enc->packet);
  if (enc->hw_device_ctx)
    av_buffer_unref(&enc->hw_device_ctx);

  if (enc->extra_data)
    bfree(enc->extra_data);
  if (enc->inline_params)
    bfree(enc->inline_params);
  if (enc->profile)
    bfree(enc->profile);
  if (enc->preset)
    bfree(enc->preset);
  if (enc->packet_buffer)
    bfree(enc->packet_buffer);

  ntp_client_destroy(&enc->ntp_client);
  bfree(enc);
}

void *vt_encoder_create_internal(obs_data_t *settings, obs_encoder_t *encoder,
                                 bool is_hardware) {
  vt_encoder_t *enc = bzalloc(sizeof(vt_encoder_t));
  enc->encoder = encoder;
  enc->is_hardware = is_hardware;

  video_t *video = obs_encoder_video(encoder);
  const struct video_output_info *voi = video_output_get_info(video);

  enc->width = voi->width;
  enc->height = voi->height;
  enc->fps_num = voi->fps_num;
  enc->fps_den = voi->fps_den;
  enc->bitrate = (int)obs_data_get_int(settings, "bitrate");
  enc->keyint =
      (int)obs_data_get_int(settings, "keyint_sec") * enc->fps_num /
      enc->fps_den;
  enc->bframes = (int)obs_data_get_int(settings, "bframes");
  enc->preset = bstrdup(obs_data_get_string(settings, "preset"));
  enc->profile = bstrdup(obs_data_get_string(settings, "profile"));

  /* NTP init */
  const char *ntp_server = obs_data_get_string(settings, "ntp_server");
  ntp_client_init(&enc->ntp_client, ntp_server, 123);
  enc->ntp_enabled = true;
  enc->ntp_sync_interval_ms =
      (uint32_t)obs_data_get_int(settings, "ntp_sync_interval_ms");
  if (enc->ntp_sync_interval_ms == 0)
    enc->ntp_sync_interval_ms = 60000;

  enc->burn_in_timecode = obs_data_get_bool(settings, "burn_in_timecode");
  enc->timecode_position = (int)obs_data_get_int(settings, "timecode_position");

  const char *encoder_name =
      is_hardware ? "h264_videotoolbox" : "libx264";
  encoder_log(LOG_INFO, enc, "Creating %s encoder", encoder_name);

  enc->codec = avcodec_find_encoder_by_name(encoder_name);
  if (!enc->codec) {
    encoder_log(LOG_ERROR, enc, "Encoder not found: %s", encoder_name);
    vt_encoder_destroy(enc);
    return NULL;
  }

  enc->codec_context = avcodec_alloc_context3(enc->codec);
  if (!enc->codec_context) {
    encoder_log(LOG_ERROR, enc, "Failed to allocate codec context");
    vt_encoder_destroy(enc);
    return NULL;
  }

  enc->codec_context->width = enc->width;
  enc->codec_context->height = enc->height;
  enc->codec_context->time_base = (AVRational){voi->fps_den, voi->fps_num};
  enc->codec_context->framerate = (AVRational){voi->fps_num, voi->fps_den};
  enc->codec_context->bit_rate = enc->bitrate * 1000;
  enc->codec_context->gop_size = enc->keyint;
  enc->codec_context->max_b_frames = enc->bframes;
  enc->codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  AVDictionary *opts = NULL;

  if (is_hardware) {
    /* VideoToolbox: set up hardware device context */
    int ret = av_hwdevice_ctx_create(&enc->hw_device_ctx,
                                     AV_HWDEVICE_TYPE_VIDEOTOOLBOX, NULL,
                                     NULL, 0);
    if (ret < 0) {
      encoder_log(LOG_WARNING, enc,
                  "Failed to create VT hw device ctx (%d), trying NV12 "
                  "software path",
                  ret);
      enc->codec_context->pix_fmt = AV_PIX_FMT_NV12;
    } else {
      enc->codec_context->hw_device_ctx =
          av_buffer_ref(enc->hw_device_ctx);
      enc->codec_context->pix_fmt = AV_PIX_FMT_NV12;
    }

    if (enc->preset && strcmp(enc->preset, "fast") == 0)
      av_dict_set(&opts, "realtime", "1", 0);
    av_dict_set(&opts, "allow_sw", "1", 0);

    if (enc->profile && *enc->profile)
      av_dict_set(&opts, "profile", enc->profile, 0);
  } else {
    /* x264 */
    enc->codec_context->pix_fmt = AV_PIX_FMT_YUV420P;

    const char *x264_preset = vt_translate_x264_preset(enc->preset);
    av_dict_set(&opts, "preset", x264_preset, 0);
    encoder_log(LOG_INFO, enc, "Using x264 preset: %s (requested: %s)",
                x264_preset, enc->preset ? enc->preset : "(default)");

    if (enc->profile && *enc->profile)
      av_dict_set(&opts, "profile", enc->profile, 0);

    av_dict_set(&opts, "nal-hrd", "vbr", 0);
  }

  char errbuf[128];
  int ret = avcodec_open2(enc->codec_context, enc->codec, &opts);
  if (ret < 0) {
    av_strerror(ret, errbuf, sizeof(errbuf));
    encoder_log(LOG_ERROR, enc, "Failed to open encoder: %s (%d)", errbuf,
                ret);
    if (opts)
      av_dict_free(&opts);
    vt_encoder_destroy(enc);
    return NULL;
  }
  if (opts)
    av_dict_free(&opts);

  enc->frame = av_frame_alloc();
  enc->packet = av_packet_alloc();

  /* Extract and process extra data.
   * VideoToolbox emits AVCC-format extradata (length-prefixed); x264 emits
   * Annex-B (start-code-prefixed). The SPS patcher requires Annex-B, so
   * convert first, then patch, then use the patched Annex-B as both the
   * extra_data returned to OBS and the inline parameter sets. */
  if (enc->codec_context->extradata_size > 0) {
    uint8_t *annexb = vt_extradata_to_annexb(
        enc->codec_context->extradata,
        (size_t)enc->codec_context->extradata_size,
        &enc->extra_data_size);
    if (annexb && enc->extra_data_size > 0) {
      enc->extra_data = annexb;
    } else {
      enc->extra_data_size = enc->codec_context->extradata_size;
      enc->extra_data = bmalloc(enc->extra_data_size);
      memcpy(enc->extra_data, enc->codec_context->extradata,
             enc->extra_data_size);
    }

    vt_patch_h264_sps_vui(enc, &enc->extra_data, &enc->extra_data_size);

    enc->inline_params_size = enc->extra_data_size;
    enc->inline_params = bmalloc(enc->inline_params_size);
    memcpy(enc->inline_params, enc->extra_data, enc->inline_params_size);
    if (enc->inline_params_size > 0) {
      encoder_log(LOG_INFO, enc,
                  "Inline parameter set payload built: %zu bytes",
                  enc->inline_params_size);
    }
  }

  encoder_log(LOG_INFO, enc,
              "%s encoder created successfully (%dx%d @ %d kbps)",
              is_hardware ? "VideoToolbox" : "x264", enc->width,
              enc->height, enc->bitrate);

  return enc;
}

bool vt_encoder_encode_internal(void *data, struct encoder_frame *frame,
                                struct encoder_packet *packet,
                                bool *received_packet) {
  vt_encoder_t *enc = data;
  char errbuf[128];

  if (!frame || !packet || !received_packet)
    return false;

  /* NTP time update — captured before frame submission so the burn-in
   * and SEI bundle use the same timestamp for this frame. */
  uint64_t now = os_gettime_ns();
  uint64_t sync_interval_ns =
      (uint64_t)enc->ntp_sync_interval_ms * 1000000ULL;
  if (enc->last_ntp_sync_time == 0 ||
      (now - enc->last_ntp_sync_time) > sync_interval_ns) {
    enc->last_ntp_sync_time = now;
    ntp_client_sync(&enc->ntp_client);
  }
  ntp_client_get_time(&enc->ntp_client, &enc->current_ntp_time);
  pts_ntp_map_store(&enc->pts_ntp_map, frame->pts, &enc->current_ntp_time);

  av_frame_unref(enc->frame);

  enc->frame->format = enc->codec_context->pix_fmt;
  enc->frame->width = enc->codec_context->width;
  enc->frame->height = enc->codec_context->height;
  enc->frame->pts = frame->pts;

  if (enc->codec_context->pix_fmt == AV_PIX_FMT_NV12) {
    enc->frame->linesize[0] = frame->linesize[0];
    enc->frame->linesize[1] = frame->linesize[1];
    enc->frame->data[0] = frame->data[0];
    enc->frame->data[1] = frame->data[1];
  } else if (enc->codec_context->pix_fmt == AV_PIX_FMT_YUV420P) {
    enc->frame->linesize[0] = frame->linesize[0];
    enc->frame->linesize[1] = frame->linesize[1];
    enc->frame->linesize[2] = frame->linesize[2];
    enc->frame->data[0] = frame->data[0];
    enc->frame->data[1] = frame->data[1];
    enc->frame->data[2] = frame->data[2];
  } else {
    encoder_log(LOG_ERROR, enc, "Unsupported pixel format: %d",
                enc->codec_context->pix_fmt);
    return false;
  }

  if (enc->burn_in_timecode) {
    timecode_frame_t tc_frame = {
        .data = {frame->data[0], frame->data[1], frame->data[2]},
        .linesize = {frame->linesize[0], frame->linesize[1], frame->linesize[2]},
        .width = enc->width,
        .height = enc->height,
        .pixfmt = (enc->codec_context->pix_fmt == AV_PIX_FMT_NV12)
                      ? TC_PIX_NV12 : TC_PIX_YUV420P,
    };
    timecode_render_draw(&enc->current_ntp_time,
                         (uint32_t)enc->fps_num, (uint32_t)enc->fps_den,
                         &tc_frame,
                         (timecode_position_t)enc->timecode_position);
  }

  int ret = avcodec_send_frame(enc->codec_context, enc->frame);
  av_frame_unref(enc->frame);

  if (ret < 0) {
    av_strerror(ret, errbuf, sizeof(errbuf));
    encoder_log(LOG_ERROR, enc, "Error sending frame: %s (%d)", errbuf,
                ret);
    return false;
  }

  ret = avcodec_receive_packet(enc->codec_context, enc->packet);
  if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
    *received_packet = false;
    return true;
  } else if (ret < 0) {
    av_strerror(ret, errbuf, sizeof(errbuf));
    encoder_log(LOG_ERROR, enc, "Error receiving packet: %s (%d)", errbuf,
                ret);
    return false;
  }

  *received_packet = true;

  /* Build SEI bundle (H.264 only, codec_type=0) */
  bool keyframe = (enc->packet->flags & AV_PKT_FLAG_KEY) != 0;
  uint8_t *sei_bundle = NULL;
  size_t sei_bundle_size = 0;
  sei_bundle_codec_info_t codec_info = {
      .h264_cpb_dpb_delays_present = enc->h264_cpb_dpb_delays_present,
      .h264_cpb_removal_delay_length = enc->h264_cpb_removal_delay_length,
      .h264_dpb_output_delay_length = enc->h264_dpb_output_delay_length,
  };
  ntp_timestamp_t packet_ntp_time;
  if (!pts_ntp_map_lookup(&enc->pts_ntp_map, enc->packet->pts,
                          &packet_ntp_time)) {
    encoder_log(LOG_WARNING, enc,
                "PTS->NTP lookup miss for PTS=%" PRId64 ", using current NTP",
                (int64_t)enc->packet->pts);
    packet_ntp_time = enc->current_ntp_time;
  }
  build_sei_bundle(0 /* H.264 */, keyframe, enc->packet->pts,
                   &packet_ntp_time, (uint32_t)enc->fps_num,
                   (uint32_t)enc->fps_den, &codec_info, &sei_bundle,
                   &sei_bundle_size);

  /* Assemble final packet */
  size_t total_size = enc->packet->size + sei_bundle_size;
  if (enc->packet_buffer_size < total_size) {
    bfree(enc->packet_buffer);
    enc->packet_buffer = bmalloc(total_size);
    enc->packet_buffer_size = total_size;
  }

  if (sei_bundle_size == 0) {
    memcpy(enc->packet_buffer, enc->packet->data, enc->packet->size);
  } else if (keyframe) {
    size_t param_sets_end = find_parameter_sets_end_vt(
        enc->packet->data, enc->packet->size);

    if (param_sets_end > 0 && param_sets_end < (size_t)enc->packet->size) {
      memcpy(enc->packet_buffer, enc->packet->data, param_sets_end);
      size_t offset = param_sets_end;
      memcpy(enc->packet_buffer + offset, sei_bundle, sei_bundle_size);
      offset += sei_bundle_size;
      memcpy(enc->packet_buffer + offset,
             enc->packet->data + param_sets_end,
             enc->packet->size - param_sets_end);
    } else if (enc->inline_params && enc->inline_params_size > 0) {
      size_t vcl_off = find_first_vcl_offset_vt(enc->packet->data,
                                                 enc->packet->size);
      total_size =
          enc->inline_params_size + enc->packet->size + sei_bundle_size;
      if (enc->packet_buffer_size < total_size) {
        bfree(enc->packet_buffer);
        enc->packet_buffer = bmalloc(total_size);
        enc->packet_buffer_size = total_size;
      }
      size_t offset = 0;
      memcpy(enc->packet_buffer, enc->inline_params,
             enc->inline_params_size);
      offset += enc->inline_params_size;
      memcpy(enc->packet_buffer + offset, enc->packet->data, vcl_off);
      offset += vcl_off;
      memcpy(enc->packet_buffer + offset, sei_bundle, sei_bundle_size);
      offset += sei_bundle_size;
      memcpy(enc->packet_buffer + offset, enc->packet->data + vcl_off,
             enc->packet->size - vcl_off);
    } else {
      memcpy(enc->packet_buffer, sei_bundle, sei_bundle_size);
      memcpy(enc->packet_buffer + sei_bundle_size, enc->packet->data,
             enc->packet->size);
    }
  } else {
    size_t vcl_off =
        find_first_vcl_offset_vt(enc->packet->data, enc->packet->size);
    memcpy(enc->packet_buffer, enc->packet->data, vcl_off);
    memcpy(enc->packet_buffer + vcl_off, sei_bundle, sei_bundle_size);
    memcpy(enc->packet_buffer + vcl_off + sei_bundle_size,
           enc->packet->data + vcl_off, enc->packet->size - vcl_off);
  }

  if (sei_bundle)
    bfree(sei_bundle);

  packet->data = enc->packet_buffer;
  packet->size = total_size;
  packet->type = OBS_ENCODER_VIDEO;
  packet->pts = enc->packet->pts;
  packet->dts = enc->packet->dts;
  packet->keyframe = keyframe;

  av_packet_unref(enc->packet);
  return true;
}

void vt_encoder_get_video_info_internal(void *data,
                                        struct video_scale_info *info) {
  vt_encoder_t *enc = (vt_encoder_t *)data;
  if (enc && !enc->is_hardware) {
    info->format = VIDEO_FORMAT_I420;
  } else {
    info->format = VIDEO_FORMAT_NV12;
  }
}

bool vt_encoder_get_extra_data_internal(void *data, uint8_t **extra_data,
                                        size_t *size) {
  vt_encoder_t *enc = (vt_encoder_t *)data;
  if (!enc || !enc->extra_data)
    return false;
  *extra_data = enc->extra_data;
  *size = enc->extra_data_size;
  return true;
}

#else

/* Stub when ENABLE_VIDEOTOOLBOX is not defined */

#endif /* ENABLE_VIDEOTOOLBOX */
