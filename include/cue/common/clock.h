// 单调时钟工具：全项目统一用 steady_clock，禁止用 system_clock 做延迟测量。
#pragma once

#include <chrono>
#include <cstdint>

namespace cue {

inline int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline int64_t now_us() { return now_ns() / 1000; }

}  // namespace cue
