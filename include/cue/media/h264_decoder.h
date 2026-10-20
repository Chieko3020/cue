// H.264 解码封装（libavcodec）
// 输入：Annex-B 编码帧（浏览器侧通过 avc.format='annexb' 产出）
// 输出：解码后的帧尺寸/格式信息（M2a 只做统计；M2b 起接 ProcessorChain）
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace cue::media {

struct DecodedFrameInfo {
  int width = 0;
  int height = 0;
  int pix_fmt = -1;
  int64_t pts = 0;
};

class H264Decoder {
 public:
  H264Decoder() = default;
  ~H264Decoder();
  H264Decoder(const H264Decoder&) = delete;
  H264Decoder& operator=(const H264Decoder&) = delete;

  bool open(std::string& err);
  void close();
  bool is_open() const { return ctx_ != nullptr; }

  // 喂一帧 Annex-B 数据；返回解码出的最后一张帧的信息（可能为 nullopt：需要更多数据或不产生输出）
  std::optional<DecodedFrameInfo> decode(std::span<const uint8_t> frame, std::string& err);

  // 累计统计
  struct Stats {
    uint64_t packets = 0;
    uint64_t frames = 0;
    uint64_t errors = 0;
    uint64_t dropped = 0;
  };
  const Stats& stats() const { return stats_; }

 private:
  AVCodecContext* ctx_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVPacket* pkt_ = nullptr;
  Stats stats_;
  bool flushed_ = false;
};

}  // namespace cue::media
