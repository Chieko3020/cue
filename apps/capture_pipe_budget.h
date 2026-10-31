// App-private pipe request shared by the real capture call site and process tests.
#pragma once

#include <algorithm>
#include <cstdint>

#include "cue/media/ffmpeg_args.h"

namespace cue::capture {

inline int video_pipe_bytes(cue::media::HostPlatform platform, cue::media::VideoEncodeMode encode,
                            int width, int height) {
  if (platform != cue::media::HostPlatform::Windows ||
      encode != cue::media::VideoEncodeMode::InProcess || width <= 0 || height <= 0) return 4096;
  // A 4 KiB pipe repeatedly empties before the bounded Windows polling read wakes.
  // Request at most one raw YUV420 frame, capped at 4 MiB rather than large kernel buffers.
  // CreatePipe treats this as a request; it is not a strict kernel-capacity guarantee.
  constexpr uint64_t kMaxRequest = 4 * 1024 * 1024;
  const uint64_t frame_bytes = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 3 / 2;
  return static_cast<int>((std::min)(frame_bytes, kMaxRequest));
}

}  // namespace cue::capture
