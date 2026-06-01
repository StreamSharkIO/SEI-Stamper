# Timecode Burn-in Design

## Problem

The SEI Stamper plugin embeds NTP-derived timecodes as SEI metadata in the H.264 bitstream, but there's no visual confirmation of the timecode on the video itself. Users need a way to verify the timecodes match the video content during testing and QA, and some workflows require a visible timecode overlay in the output.

## Solution

Add a "Burn in NTP Timecode" checkbox to the encoder settings. When enabled, the plugin renders the NTP timestamp as white monospace text on a black background directly onto each video frame before encoding.

## Format

ISO 8601 with milliseconds: `2026-06-01T04:09:42.233Z`

## Rendering

- Embedded 8x16 monospace bitmap font (ASCII 0x20-0x7E)
- White text on black background with padding
- YUV color values: white Y=235 U=128 V=128, black Y=16 U=128 V=128
- Scale: 1x for <720p, 2x for 720p-1079p, 3x for 1080p+
- Handles both NV12 (VideoToolbox) and YUV420P (x264) pixel formats

## Position

Configurable via dropdown: Top Left, Top Center, Top Right, Bottom Left, Bottom Center, Bottom Right.

## New files

- `src/timecode-render.h` — public API
- `src/timecode-render.c` — font table, NTP-to-string formatting, YUV drawing

## Modified files

- `src/videotoolbox-encoder.h` — add `burn_in_timecode` and `timecode_position` fields to `vt_encoder_t`
- `src/videotoolbox-encoder.c` — read settings, move NTP capture before frame submission, call render function
- `src/unified-encoder.c` — add checkbox and position dropdown to properties UI, add defaults
- `CMakeLists.txt` — add `timecode-render.c` to source list

## Data flow

1. `vt_encoder_encode_internal` captures NTP time (moved before pixel access)
2. If `burn_in_timecode` enabled, calls `timecode_render_draw()` on the raw frame data
3. Frame with burned-in text is sent to `avcodec_send_frame()`
4. SEI bundle is built from the same NTP timestamp (timecodes match exactly)

## UI

In the Encoder Settings panel, after "NTP Sync Interval (ms)":

```
[x] Burn in NTP Timecode
Timecode Position: [Top Left v]
```

Settings keys: `burn_in_timecode` (bool, default false), `timecode_position` (int, default 0).

## Scope

Wired into the macOS VideoToolbox and x264 encoder paths (both use `vt_encoder_encode_internal`). The render module is encoder-agnostic — Windows encoders can call the same functions in a future change.
