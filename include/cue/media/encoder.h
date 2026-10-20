// 转码链：解码帧 →（可选缩放）→ x264 编码 → Annex-B
// M2b：这是 agent 唯一有音视频含量的可调对象（分辨率/码率/GOP/preset）
#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct AVFrame;

namespace cue::media {

struct EncodedResult {
  std::vector<uint8_t> data;  // Annex-B 码流
  bool keyframe = false;
  int64_t pts = 0;
};

// 编码失败的分类：旧实现把「未 open / 尺寸不符 / 格式不符 / 编码器延迟 / 编码失败」
// 全塞进 nullopt，调用方只能一律当丢帧，配置错误表现为"指标轻微变差"（review #31）。
// 区分之后 EncoderDelay 是正常路径（此刻没有可发数据），其余都是需要被看见的错误。
enum class EncodeError : uint8_t {
  NotOpen = 0,
  InvalidFrame,
  SizeMismatch,
  FormatMismatch,
  EncoderDelay,
  EncodeFailed,
  ScaleFailed,
};

inline const char* encode_error_name(EncodeError e) {
  switch (e) {
    case EncodeError::NotOpen: return "not_open";
    case EncodeError::InvalidFrame: return "invalid_frame";
    case EncodeError::SizeMismatch: return "size_mismatch";
    case EncodeError::FormatMismatch: return "format_mismatch";
    case EncodeError::EncoderDelay: return "encoder_delay";
    case EncodeError::EncodeFailed: return "encode_failed";
    case EncodeError::ScaleFailed: return "scale_failed";
  }
  return "unknown";
}

class X264Encoder {
 public:
  X264Encoder();
  ~X264Encoder();
  X264Encoder(const X264Encoder&) = delete;
  X264Encoder& operator=(const X264Encoder&) = delete;

  // threads ：x264 线程数（2 vCPU 下默认 1，给解码与网络留核）
  bool open(int w, int h, int bitrate_kbps, int fps, int gop, std::string_view preset,
            std::string& err, int threads = 1);
  void close();
  bool is_open() const;

  // encode_ns：非空时写回「拷入 + x264_encoder_encode」的耗时（微秒），供六段埋点把
  // encode 从 op 里拆出来；失败/无输出时也会写回已发生的耗时（0 表示未进入编码）。
  // 失败原因用 EncodeError 分类，err 只放人读细节。
  std::expected<EncodedResult, EncodeError> encode(const AVFrame* frame, int64_t pts,
                                                   std::string& err,
                                                   int64_t* encode_ns = nullptr);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class TranscodeChain {
 public:
  TranscodeChain();
  ~TranscodeChain();
  TranscodeChain(const TranscodeChain&) = delete;
  TranscodeChain& operator=(const TranscodeChain&) = delete;

  // dst_w/dst_h 为 0 表示不缩放（沿用解码尺寸）
  bool configure(int dst_w, int dst_h, int bitrate_kbps, int fps, int gop, std::string_view preset,
                 std::string& err, int threads = 1);
  void close();
  bool enabled() const { return enabled_; }
  int out_width() const { return dst_w_; }
  int out_height() const { return dst_h_; }

  // encode_ns：把编码段耗时单独带回（缩放仍计入调用方的 op 段）
  std::expected<EncodedResult, EncodeError> process(const AVFrame* in, int64_t pts,
                                                    std::string& err,
                                                    int64_t* encode_ns = nullptr);

 private:
  struct Scaler;
  std::unique_ptr<Scaler> scaler_;
  X264Encoder encoder_;
  int dst_w_ = 0;
  int dst_h_ = 0;
  bool enabled_ = false;
};

}  // namespace cue::media
