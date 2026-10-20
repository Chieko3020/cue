// 转码链：解码帧 →（可选缩放）→ x264 编码 → Annex-B
// M2b：这是 agent 唯一有音视频含量的可调对象（分辨率/码率/GOP/preset）
#pragma once

#include <cstdint>
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

class X264Encoder {
 public:
  X264Encoder();
  ~X264Encoder();
  X264Encoder(const X264Encoder&) = delete;
  X264Encoder& operator=(const X264Encoder&) = delete;

  bool open(int w, int h, int bitrate_kbps, int fps, int gop, std::string_view preset, std::string& err);
  void close();
  bool is_open() const;

  std::optional<EncodedResult> encode(const AVFrame* frame, int64_t pts, std::string& err);

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
                 std::string& err);
  void close();
  bool enabled() const { return enabled_; }
  int out_width() const { return dst_w_; }
  int out_height() const { return dst_h_; }

  std::optional<EncodedResult> process(const AVFrame* in, int64_t pts, std::string& err);

 private:
  struct Scaler;
  std::unique_ptr<Scaler> scaler_;
  X264Encoder encoder_;
  int dst_w_ = 0;
  int dst_h_ = 0;
  bool enabled_ = false;
};

}  // namespace cue::media
