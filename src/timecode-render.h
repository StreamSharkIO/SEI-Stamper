#pragma once

#include "ntp-client.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  TC_POS_TOP_LEFT = 0,
  TC_POS_TOP_CENTER,
  TC_POS_TOP_RIGHT,
  TC_POS_BOTTOM_LEFT,
  TC_POS_BOTTOM_CENTER,
  TC_POS_BOTTOM_RIGHT,
} timecode_position_t;

typedef enum {
  TC_PIX_NV12 = 0,
  TC_PIX_YUV420P,
} timecode_pixfmt_t;

typedef struct timecode_frame {
  uint8_t *data[3];
  int linesize[3];
  int width;
  int height;
  timecode_pixfmt_t pixfmt;
} timecode_frame_t;

void timecode_render_draw(const ntp_timestamp_t *ntp,
                          timecode_frame_t *frame,
                          timecode_position_t position);

#ifdef __cplusplus
}
#endif
