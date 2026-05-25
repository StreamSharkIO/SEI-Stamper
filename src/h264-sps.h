/******************************************************************************
    H.264 SPS minimal patcher

    Provides just enough of an H.264 SPS parser to flip the
    pic_struct_present_flag bit in the SPS VUI of an Annex-B extradata
    buffer. Required because FFmpeg's h264_metadata bitstream filter
    does not expose that flag as an option (verified against trunk),
    and decoders ignore pic_timing SEI messages unless the flag is set —
    which means stock-ffmpeg does not surface AV_FRAME_DATA_S12M_TIMECODE
    for H.264.

    Bails out (returns false) on inputs that contain a seq_scaling_matrix
    or HRD parameters in the VUI, both of which would require a much
    larger parser. NVENC's default H.264 output does not include either,
    so this is a reasonable simplification.
******************************************************************************/

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Information extracted from the H.264 SPS that the encoder needs in order
 * to emit valid pic_timing SEI payloads.
 *
 * When CpbDpbDelaysPresentFlag (derived from nal_hrd or vcl_hrd presence)
 * is true, pic_timing SEI must prepend cpb_removal_delay and dpb_output_delay
 * fields of the lengths advertised here (in bits).
 */
typedef struct h264_sps_info {
  bool parsed_ok;                      /* false if the parser bailed */
  bool cpb_dpb_delays_present;
  uint8_t cpb_removal_delay_length;    /* number of bits, 1..32 */
  uint8_t dpb_output_delay_length;     /* number of bits, 1..32 */
} h264_sps_info_t;

/*
 * Parse the H.264 SPS in `in` and patch its VUI if pic_struct_present_flag
 * is currently 0. Whether or not patching happens, the SPS metadata needed
 * to build pic_timing SEI payloads is returned via `info_out`.
 *
 * Returns true only when a patched extradata buffer was produced. The
 * caller then owns *out (bmalloc'd, freed with bfree) and *out_size.
 * Returns false when:
 *   - the SPS parse failed (info_out->parsed_ok will be false)
 *   - the flag was already 1 (info_out->parsed_ok will be true; *out NULL)
 * In all "false" cases *out is NULL.
 */
bool h264_sps_patch_pic_struct_present(const uint8_t *in, size_t in_size,
                                       uint8_t **out, size_t *out_size,
                                       h264_sps_info_t *info_out);

#ifdef __cplusplus
}
#endif
