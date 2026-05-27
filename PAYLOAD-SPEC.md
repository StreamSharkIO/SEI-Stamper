# SEI Stamper Payload Specification

This document describes the SEI (Supplemental Enhancement Information) data that
the SEI Stamper plugin embeds into H.264 and H.265 video streams, so that
third-party tooling (receivers, server-side modules, VOD post-processors) can
consume it against a stable spec rather than reverse-engineering the bitstream.

The plugin emits two independent kinds of SEI:

1. A **custom NTP payload** carried in a `user_data_unregistered` SEI,
   identified by a fixed UUID. This is the plugin's own format and is the
   authoritative source of wall-clock timing for the SEI Stamper receiver.
2. A **standards-compliant timecode SEI** (H.264 `pic_timing`, H.265
   `time_code`) carrying the same instant as a SMPTE 12M timecode. This exists
   so stock tools (for example `ffprobe`, or any decoder that surfaces
   `AV_FRAME_DATA_S12M_TIMECODE`) can read the timecode with no custom parser.

Both are derived from the same NTP-disciplined clock, so they agree to within
one frame period (the SMPTE timecode is quantised to frame boundaries; the NTP
payload carries sub-frame precision).

---

## 1. Custom NTP payload (user_data_unregistered SEI)

### Identification

| Field | Value |
|-------|-------|
| SEI payload type | `5` (user_data_unregistered, per ITU-T H.264 D.1.1 / H.265 D.2.6) |
| UUID (`uuid_iso_iec_11578`) | `a5b3c2d1-e4f5-6789-abcd-ef0123456789` |
| Raw UUID bytes | `a5 b3 c2 d1 e4 f5 67 89 ab cd ef 01 23 45 67 89` |

A consumer identifies this payload by matching payload type 5 and the 16-byte
UUID prefix. Any other UUID is some other vendor's data and should be ignored.

### Payload layout

The SEI payload is exactly **32 bytes**: the 16-byte UUID followed by 16 bytes
of plugin data. All multi-byte integers are **big-endian (network byte order)**.

| Offset | Size | Field | Type | Description |
|--------|------|-------|------|-------------|
| 0  | 16 | `uuid` | bytes | Fixed identifier (above) |
| 16 | 8  | `pts` | int64 BE | Frame presentation timestamp, in the encoder's time base |
| 24 | 4  | `ntp_seconds` | uint32 BE | Seconds since the NTP epoch (1900-01-01 00:00:00 UTC) |
| 28 | 4  | `ntp_fraction` | uint32 BE | Fractional second, in units of 2^-32 s |

`payloadSize` in the SEI message is therefore `32`.

### Semantics

- **Cadence:** emitted on keyframes (IDR). The receiver re-syncs on keyframes
  and interpolates between them; the per-frame timecode SEI (section 2) covers
  frame-accurate needs.
- **Clock:** `ntp_seconds`/`ntp_fraction` are wall-clock UTC from the encoder's
  NTP-disciplined clock (NTP query when reachable, otherwise the
  OS system clock, which is itself NTP-disciplined). Convert to Unix time by
  subtracting the NTP epoch delta `2208988800`.
- **Sub-frame precision:** the fraction field gives roughly 0.23 ns resolution,
  far finer than a frame period; consumers needing wall-clock-to-frame mapping
  should use it rather than the SMPTE timecode's frame index.
- **PTS:** the encoder time base PTS for the frame the SEI is attached to, for
  correlating the wall-clock instant with the encoded stream's own timeline.

### Worked example

Reading the fraction as a Unix timestamp with sub-second precision:

```
unix_seconds = ntp_seconds - 2208988800
unix_subsec  = ntp_fraction / 2^32
utc_time     = unix_seconds + unix_subsec
```

---

## 2. Standards-compliant timecode SEI

Emitted on **every frame**, derived from the same NTP instant as section 1,
expressed as a SMPTE 12M wall-clock timecode (HH:MM:SS:FF, UTC time of day).

| Codec | SEI | Payload type | NAL unit type |
|-------|-----|--------------|---------------|
| H.264 | `pic_timing` | `1` | 6 (SEI) |
| H.265 | `time_code` | `136` | 39 (prefix SEI) |

- The frame index (`FF`) is computed from the NTP fractional second and the
  encoder frame rate. Non-drop-frame counting is used regardless of rate; the
  drop-frame flag is signalled for 29.97 / 59.94 modes so strict consumers can
  recompute.
- For H.264, the SPS VUI must have `pic_struct_present_flag = 1` for decoders to
  parse `pic_timing`; the plugin ensures this. When the SPS advertises HRD
  parameters, the `pic_timing` payload includes the required `cpb_removal_delay`
  and `dpb_output_delay` fields (zero-valued) ahead of the timecode.
- Stock `ffprobe` surfaces this as side data:

```
ffprobe -show_frames -show_entries frame_side_data=side_data_type,timecode <file>
# side_data_type=SMPTE 12-1 timecode
# timecode=HH:MM:SS:FF   (UTC)
```

---

## 3. Versioning

The current payload is **version 1**, identified implicitly by the UUID
`a5b3c2d1-e4f5-6789-abcd-ef0123456789` and the fixed 32-byte layout.

Any incompatible change to the custom NTP payload layout MUST use a **new UUID**,
so that consumers keyed on the version-1 UUID continue to parse version-1
streams unambiguously and can detect (and skip) formats they do not understand.
Additive, backward-compatible changes that keep the existing 32 bytes intact may
extend the payload beyond byte 32 under the same UUID; consumers MUST therefore
use the SEI `payloadSize`, not a hardcoded 32, when reading.

The standards-compliant timecode SEI (section 2) is not versioned by this
project; it follows the ITU-T H.264 / H.265 specifications directly.
