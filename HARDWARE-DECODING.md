# Narrow H264 hardware decoding

`HardwareDecoder` is a libavcodec-only helper used by file playback and available
to live-input consumers. It has no application-service dependencies.

## Selection and observation

- Default `IDX_FFMPEG_HW_DECODE=auto`: Linux VAAPI or Windows D3D11VA, **only**
  known 8-bit 4:2:0 H264 Baseline/Constrained Baseline/Main/High.
- `IDX_FFMPEG_HW_DECODE=software`: process-local software comparison override.
  Any other explicit value also selects software, rather than guessing.
- Linux uses `/dev/dri/renderD128` unless `IDX_FFMPEG_VAAPI_DEVICE` names another
  `/dev/dri/renderD<number>`. This does not install drivers, change permissions,
  or make device creation proof of decode. Intel VAAPI still needs its real
   driver and render-node access for the calling process.
- The decoder must advertise the selected device/configuration and successfully
  create it. There is no global "enable all accelerators", NVDEC/QSV/DXVA2,
  VideoToolbox or zero-copy renderer integration.
- All other codecs, unknown profiles/formats, high bit depth, 4:2:2, 4:4:4 and
  alpha-capable formats stay software. In particular, **all ProRes and CineForm
  stay software**, including alpha variants.
- Once-per-open/transition logs contain `path=file|live`, requested mode,
  selected decoder/device, a static reason code, and the **actual** active path
   after a usable frame. No media URL, key or per-frame log is added.
   `get_decode_path()` exposes the active path on playback objects;
  `unopened` means no usable frame yet.

## Frames and fallback

Each stream has its own state/device reference; codec contexts own those
references. The caller frees its context before destroying the callback owner.
Unknown hardware formats are never passed to swscale as ordinary pixels.

Real hardware frames are downloaded with `av_hwframe_transfer_data`.
`av_frame_copy_props` retains PTS/best-effort timestamp, duration, color and crop
properties. NV12 is losslessly deinterleaved into **owned YUV420P** planes before
the existing upload conversion. This matters: directly converting NV12 to RGB
can use different swscale chroma interpolation from software YUV420P. CPU crop
metadata is applied before upload. Size/format changes rebuild the cached scaler.
Buffers held by a consumer are never overwritten by the next transfer.

Hardware setup/open failures retry with a fresh software context. Decode,
format-negotiation or download failures demote only that stream, with no hardware
retry loop. File playback reopens video software and uses normal backward
keyframe seek/preroll at the last good timestamp; already published video is not
published twice. Live callers keep transport/audio/options and discard video
until a keyframe after reopening software. A genuinely new live URL can probe
again. ProRes/CineForm never enter the hardware/download path, preserving alpha.
File recovery retains already queued audio and skips its replayed prefix using
the last queued audio timestamp, rather than clearing that queue or mixing it twice.

Compatibility rendering keeps the normal CPU RGBA upload. A hardware-backed file
playback keeps that packed contract for its entire lifetime, even after fallback.
File seeks wait for the worker seek command, preventing stale EOF state from
stopping playback immediately after a backward seek.

## Tests and limits

Run `bash tests/run_hardware_decoder_test.sh <headers> <Linux runtime directory>`
against the exact vendored FFmpeg runtime. Tests inject only hardware boundaries:
selection exclusions, format negotiation, device/open failure, fresh/sticky
software fallback, ref release, download failure, NV12 plane values, crop,
timing/color metadata, odd-size changes and consumer buffer lifetime.
The existing packed-frame-scaler guards still cover exact-size packed uploads.

Synthetic unit transfers and software rendering are **not physical GPU evidence**.
Validate actual hardware decoding on the target device with its supported driver,
compare output with software, and separately test driver failure/recovery and
long-running playback. Passing the helper tests alone does not establish those
device-specific results.
