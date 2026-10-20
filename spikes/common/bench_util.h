// Spike 共用小工具：单调时钟、RSS、CPU、分位数采样。
// 仅用于 spikes/，不属于产品代码。
#pragma once

#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

namespace cue_spike {

// steady_clock 在 Linux 上等于 CLOCK_MONOTONIC，跨进程可直接比较（同机测量）。
inline int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline int64_t rss_kb() {
  std::ifstream f("/proc/self/statm");
  int64_t size_pages = 0;
  int64_t res_pages = 0;
  if (f >> size_pages >> res_pages) {
    return res_pages * static_cast<int64_t>(::sysconf(_SC_PAGESIZE)) / 1024;
  }
  return -1;
}

// utime + stime，秒。返回 (user, sys) 两段分开统计。
struct CpuSample {
  double user;
  double sys;
};

inline CpuSample cpu_seconds() {
  rusage ru{};
  ::getrusage(RUSAGE_SELF, &ru);
  auto s = [](const timeval& tv) {
    return static_cast<double>(tv.tv_sec) + static_cast<double>(tv.tv_usec) / 1e6;
  };
  return {s(ru.ru_utime), s(ru.ru_stime)};
}

// 绝对睡眠到 CLOCK_MONOTONIC 的 abs_ns（避免累积漂移）。
inline void sleep_until_ns(int64_t abs_ns) {
  timespec ts{};
  ts.tv_sec = static_cast<time_t>(abs_ns / 1'000'000'000);
  ts.tv_nsec = static_cast<long>(abs_ns % 1'000'000'000);
  while (::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
  }
}

// 一段样本的 p50/p99/max（单位由调用方决定）。
class Samples {
 public:
  void add(int64_t v) { v_.push_back(v); }
  void clear() { v_.clear(); }
  size_t size() const { return v_.size(); }
  int64_t sum() const {
    return std::accumulate(v_.begin(), v_.end(), int64_t{0});
  }
  int64_t pct(double p) {
    if (v_.empty()) return 0;
    std::sort(v_.begin(), v_.end());
    size_t idx = static_cast<size_t>(p * static_cast<double>(v_.size() - 1));
    return v_[idx];
  }
  int64_t max() {
    if (v_.empty()) return 0;
    return *std::max_element(v_.begin(), v_.end());
  }

 private:
  std::vector<int64_t> v_;
};

}  // namespace cue_spike
