# Unreleased
- Fix buffer timestamping that corrupted downstream latency measurements. Buffers are now stamped
  with the running-time at which the frame actually arrived from the camera (recorded in the frame
  callback) instead of the time `create()` later dequeues them, so time spent waiting in the
  internal frame queue is no longer hidden. The redundant `do-timestamp` handling, which silently
  overwrote the timestamp with the dequeue time, has been disabled.
- Tag buffers with a DeepStream input-system-timestamp (when the optional DeepStream SDK is
  detected at build time) so this element becomes the origin for DeepStream latency measurement
  (`NVDS_ENABLE_LATENCY_MEASUREMENT`) instead of the first downstream NVIDIA element.

# 1.1.0
- Add `framebuffers` property to allow adjusting number of used buffers for frame transmission from
  device
- Implement zero-copy frame transmission. Incoming frames are no longer copied to `GstBuffer`s.
  Instead the memory that the frame already uses is reused for the `GstBuffer`
- Add `NVMM` capabilities on supported devices. This enables the use of accelerated pipeline
  elements provided by Nvidia.

# 1.0.0
- Initial release
