#ifndef VIDEOTOOLBOX_ENCODER_H
#define VIDEOTOOLBOX_ENCODER_H

#include <obs-module.h>

#ifdef ENABLE_VIDEOTOOLBOX

#include "ntp-client.h"
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>

typedef struct vt_encoder {
  obs_encoder_t *encoder;

  /* FFmpeg encoder */
  const AVCodec *codec;
  AVCodecContext *codec_context;
  AVFrame *frame;
  AVPacket *packet;

  /* Hardware device context (VideoToolbox only, NULL for x264) */
  AVBufferRef *hw_device_ctx;

  /* Configuration */
  int width;
  int height;
  int fps_num;
  int fps_den;
  int bitrate;
  int keyint;
  int bframes;
  char *profile;
  char *preset;

  /* true = h264_videotoolbox, false = libx264 */
  bool is_hardware;

  /* Extra Data (SPS/PPS) returned to OBS via get_extra_data */
  uint8_t *extra_data;
  size_t extra_data_size;

  /* Inline parameter sets in Annex-B form for keyframe prepending */
  uint8_t *inline_params;
  size_t inline_params_size;

  /* H.264 SPS-derived info for pic_timing SEI */
  bool h264_cpb_dpb_delays_present;
  uint8_t h264_cpb_removal_delay_length;
  uint8_t h264_dpb_output_delay_length;

  /* NTP sync */
  struct ntp_client ntp_client;
  uint64_t last_ntp_sync_time;
  ntp_timestamp_t current_ntp_time;
  bool ntp_enabled;
  uint32_t ntp_sync_interval_ms;

  /* Timecode burn-in */
  bool burn_in_timecode;
  int timecode_position;

  /* Packet buffer */
  uint8_t *packet_buffer;
  size_t packet_buffer_size;
} vt_encoder_t;

/* Public API for unified encoder dispatch */
void *vt_encoder_create_internal(obs_data_t *settings, obs_encoder_t *encoder,
                                 bool is_hardware);
bool vt_encoder_encode_internal(void *data, struct encoder_frame *frame,
                                struct encoder_packet *packet,
                                bool *received_packet);
void vt_encoder_get_video_info_internal(void *data,
                                        struct video_scale_info *info);
bool vt_encoder_get_extra_data_internal(void *data, uint8_t **extra_data,
                                        size_t *size);
void vt_encoder_destroy(vt_encoder_t *enc);

#endif /* ENABLE_VIDEOTOOLBOX */

#endif /* VIDEOTOOLBOX_ENCODER_H */
