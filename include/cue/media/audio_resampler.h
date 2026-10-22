// 音频重采样：源 PCM（s16 交错，常见 44.1 kHz 双声道）→ 48 kHz 单声道 s16。
//
// 为什么不用手写线性插值：线性插值没有抗混叠滤波器。44.1 kHz → 48 kHz 属于升采样，
// 线性插值会在 22.05 kHz 以上产生镜像，通带也有明显衰减（在 20 kHz 处不是一个
// 可忽略的量）。本项目的音频链路已经因为这个问题返工过一次，所以这里固定走
// libswresample 的多相 FIR（swr_convert），与浏览器侧把重采样交给 AudioContext 等价。
//
// 为什么统一到 s16：Opus 编码器的输入就是 s16le，ffmpeg 子进程的 PCM 输出也是 s16le，
// 中间不再转 fltp，少一次格式转换也少一类四舍五入误差。
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

struct SwrContext;

namespace cue::media {

struct ResampleSpec {
  int in_rate = 44100;
  int in_channels = 2;
  int out_rate = 48000;
  int out_channels = 1;
};

enum class ResampleError : uint8_t {
  InvalidSpec = 0,
  InitFailed,
  ConvertFailed,
};

inline const char* resample_error_name(ResampleError e) {
  switch (e) {
    case ResampleError::InvalidSpec: return "invalid_spec";
    case ResampleError::InitFailed: return "init_failed";
    case ResampleError::ConvertFailed: return "convert_failed";
  }
  return "unknown";
}

class AudioResampler {
 public:
  AudioResampler() = default;
  ~AudioResampler();
  AudioResampler(const AudioResampler&) = delete;
  AudioResampler& operator=(const AudioResampler&) = delete;

  bool open(const ResampleSpec& spec, std::string& err);
  void close();
  bool is_open() const { return swr_ != nullptr; }
  const ResampleSpec& spec() const { return spec_; }

  // in 是 interleaved s16，样本个数 = in_frames * in_channels。
  // out 被追加写入（不清空）：调用方通常把多次 convert 的输出拼成 960 样本的 Opus 帧。
  std::expected<void, ResampleError> convert(const int16_t* in, std::size_t in_frames,
                                             std::vector<int16_t>& out, std::string& err);

  // 排空重采样器内部的滤波器延迟（收流结束时调用一次，否则末尾几十个样本会丢）。
  std::expected<void, ResampleError> flush(std::vector<int16_t>& out, std::string& err);

  // 输入 in_frames 帧后，输出侧理论样本数（不含滤波器延迟）。四舍五入而不是截断：
  // 4410 帧 @44.1k→48k 恰好 4800；截断会让长跑慢慢少样本。
  static std::size_t output_frames_for(std::size_t in_frames, int in_rate, int out_rate);

 private:
  SwrContext* swr_ = nullptr;
  ResampleSpec spec_{};
};

}  // namespace cue::media
