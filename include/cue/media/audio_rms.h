// 音频 RMS 的窗口口径（cued 与 cue-probe 共用）。
//
// 为什么放在公共头而不各写一份：两端必须用**同一个口径**才有可比性——cued 侧算的是
// 「上行进来的音频有多响」，probe 侧算的是「下行收到的音频有多响」，两者之差
// 就是链路（含一次 Opus 编解码）对音量的影响。口径一旦分叉，这个差值就没有意义。
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>

#include "cue/media/audio_decoder.h"  // dbfs_from_sum_sq

namespace cue::media {

// 最近 kFrames 个**已解码帧**（50 × 20 ms = 1 s）。
// 为什么按帧数而不是按挂钟：解码是"来一包解一包"的流水，按时间窗截断会在丢包/抖动把
// 到达节奏拉稀时算出"窗口里只有 2 个样本"，读数抖动比音频本身还大；按帧数则只要流还在，
// 窗口就总是 1 s 的量。只存每帧的 Σx² 与样本数：算 RMS 只需要这两个量，
// 不留原始 PCM（1 s 96 KB 的无用内存）。
struct AudioRmsWindow {
  static constexpr std::size_t kFrames = 50;  // 50 × 20 ms = 1 s

  void push(const int16_t* pcm, std::size_t n) {
    if (pcm == nullptr || n == 0) return;
    double sum_sq = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      const double v = static_cast<double>(pcm[i]);
      sum_sq += v * v;
    }
    sums_.push_back(sum_sq);
    counts_.push_back(n);
    if (sums_.size() > kFrames) {
      sums_.pop_front();
      counts_.pop_front();
    }
  }

  bool empty() const { return sums_.empty(); }
  std::size_t frames() const { return sums_.size(); }

  // 窗口内**全部样本**的 RMS，不是各帧 dBFS 的平均（那会让静音帧和响帧按帧等权，
  // 读出来"比实际响"）。口径与 cue::media::rms_dbfs 同一个参考电平。
  double dbfs() const {
    double sum_sq = 0.0;
    uint64_t n = 0;
    for (std::size_t i = 0; i < sums_.size(); ++i) {
      sum_sq += sums_[i];
      n += counts_[i];
    }
    return dbfs_from_sum_sq(sum_sq, n);
  }

 private:
  std::deque<double> sums_;
  std::deque<std::size_t> counts_;
};

}  // namespace cue::media
