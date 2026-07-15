# Unreleased
- Add external hardware-trigger correlation. Trigger events (sequence number + CLOCK_MONOTONIC
  time) are fed in through the new `notify-trigger` action signal; the element correlates each
  delivered frame to the trigger that produced it by timestamp proximity (robust to dropped
  frames / over-triggering) and attaches a `GstVmbSrcTriggerMeta` carrying the trigger sequence,
  trigger time, approximate transfer latency and the camera frame id/timestamp. This is a
  `GstCustomMeta` (GStreamer >= 1.20), so it is readable from Python without custom bindings via
  `buffer.get_custom_meta("GstVmbSrcTriggerMeta").get_structure()`. The buffer PTS is
  set to the trigger instant, and (with the new nvstreammux) a `GstReferenceTimestampMeta` anchors
  DeepStream latency measurement at the trigger. New properties `triggerlatency`,
  `triggerlatencytolerance` and `triggerlatencymeta`. See EXAMPLES.md for usage and a DeepStream
  bridge probe.
- When the optional DeepStream SDK is detected at build time (`HAVE_DEEPSTREAM`), additionally
  attach the trigger record (`trigger_seq`, `trigger_time`, `camera_frame_id`, `correlated`) as a
  DeepStream `NvDsMeta`. `nvstreammux` transforms it into an `NvDsUserMeta` on the matching
  `NvDsFrameMeta` (user-meta type `"VMBSRC.TRIGGER.USERMETA"`), so a DeepStream consumer
  reads it straight off the frame - correctly paired per source - with no bridging pad probe. When
  the SDK is absent this compiles out and the custom-meta bridge probe (see EXAMPLES.md) remains the
  way to reach `NvDsFrameMeta`.
- Fix buffer timestamping that corrupted downstream latency measurements. Buffers are now stamped
  with the running-time at which the frame actually arrived from the camera (recorded in the frame
  callback) instead of the time `create()` later dequeues them, so time spent waiting in the
  internal frame queue is no longer hidden. The redundant `do-timestamp` handling, which silently
  overwrote the timestamp with the dequeue time, has been disabled.
- NOTE for DeepStream frame-latency measurement: `nvds_measure_buffer_latency()` hardcodes that only
  components whose name starts with `nvv4l2decode` or `audiodecoder` provide the overall
  "Frame latency" baseline (`comp_in_timestamp`); metas under any other name are printed as
  per-component lines but ignored for the frame total. To anchor DeepStream's frame latency at the
  hardware-trigger instant, name the vmbsrc element accordingly (e.g. `nvv4l2decoder_cam_<ID>`) so
  the `<element-name>-trigger` meta emitted via `triggerlatencymeta` passes that check. Frames
  without a correlated trigger (including pipelines that never emit `notify-trigger`) fall back to
  the frame arrival time as the anchor, so the DeepStream frame counter and overall latency keep
  working without external trigger events. Calling
  `nvds_set_input_system_timestamp()` inside the source does not work as an alternative - it
  requires `NvDsBatchMeta`, which only exists downstream of `nvstreammux` - so no DeepStream SDK
  dependency is needed at build time.

# 1.1.0
- Add `framebuffers` property to allow adjusting number of used buffers for frame transmission from
  device
- Implement zero-copy frame transmission. Incoming frames are no longer copied to `GstBuffer`s.
  Instead the memory that the frame already uses is reused for the `GstBuffer`
- Add `NVMM` capabilities on supported devices. This enables the use of accelerated pipeline
  elements provided by Nvidia.

# 1.0.0
- Initial release
