# 1.1.0
- Add `framebuffers` property to allow adjusting number of used buffers for frame transmission from
  device
- Implement zero-copy frame transmission. Incoming frames are no longer copied to `GstBuffer`s.
  Instead the memory that the frame already uses is reused for the `GstBuffer`
- Add `NVMM` capabilities on supported devices. This enables the use of accelerated pipeline
  elements provided by Nvidia.

# 1.0.0
- Initial release
