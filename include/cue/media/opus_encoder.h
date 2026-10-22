// Opus 编码：48 kHz 单声道、20 ms 一帧、48 kbps（与浏览器侧 AudioEncoder 的配置一致）。
//
// 为什么直接链 libopus 而不是走 libavcodec 的 libopus 封装：编码器是采集端的固定组件，
// 不需要 libavcodec 的 codec 查找/帧装填层；一拍 960 样本 → 一个 Opus 包正是上行协议
// 里 TYPE_AUDIO 的载荷。FFmpeg 里 libopus 默认也会把 20 ms 切成独立包。
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

struct OpusEncoder;  // 全局命名空间里的 opus.h 类型

namespace cue::media {

enum class OpusError : uint8_t {
  NotOpen = 0,
  InvalidFrame,  // 采样数与 frame_ms 不匹配（Opus 要求 2.5/5/10/20/40/60 ms）
  EncodeFailed,
};

inline const char* opus_error_name(OpusError e) {
  switch (e) {
    case OpusError::NotOpen: return "not_open";
    case OpusError::InvalidFrame: return "invalid_frame";
    case OpusError::EncodeFailed: return "encode_failed";
  }
  return "unknown";
}

class OpusAudioEncoder {
 public:
  static constexpr int kSampleRate = 48000;  // Opus 只支持 8/12/16/24/48 kHz；上行固定 48k
  static constexpr int kFrameMs = 20;
  static constexpr int kBitrateBps = 48000;
  static constexpr int kFrameSamples = kSampleRate * kFrameMs / 1000;  // 960

  OpusAudioEncoder() = default;
  ~OpusAudioEncoder();
  OpusAudioEncoder(const OpusAudioEncoder&) = delete;
  OpusAudioEncoder& operator=(const OpusAudioEncoder&) = delete;

  bool open(std::string& err);
  void close();
  bool is_open() const { return enc_ != nullptr; }
  int frame_samples() const { return kFrameSamples; }
  int channels() const { return 1; }

  // pcm 必须是恰好 960 个 mono s16 样本（不足/超出都拒：Opus 不做跨帧缓冲，
  // 跨帧缓冲留给调用方，否则 pts 与帧边界会对不上）。
  std::expected<std::vector<uint8_t>, OpusError> encode(const int16_t* pcm, std::size_t samples,
                                                        std::string& err);

 private:
  ::OpusEncoder* enc_ = nullptr;
  std::vector<uint8_t> out_;
};

}  // namespace cue::media
