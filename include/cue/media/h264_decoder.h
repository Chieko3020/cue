// H.264 解码封装（libavcodec）
// 输入：Annex-B 编码帧（浏览器侧通过 avc.format='annexb' 产出）
// 输出：解码后的帧（**借用指针**，见 decode 的说明）
//
// 这个类有两条不变量，改之前先读：
//   1) 内存：上一张输出帧的引用必须在**下一次 send_packet 之前**放掉，否则解码器内部的
//      帧池永远回收不到那批缓冲（每帧漏一张完整 YUV，见 decode 里的注释与 tests/
//      test_h264_decoder.cpp 的 RSS 断言）。
//   2) 参考链：丢过帧、刚打开、刚 flush 之后，非 IDR 的画面在数学上一定是错的。
//      所以这里有一个"等 IDR"状态机：丢弃期间 decode() 返回 nullptr 且 err 为空
//      （那是策略，不是失败），调用方可以用 waiting_keyframe() 把"在等恢复"和
//      "没收到帧"区分开。
//
// 分辨率变化**不需要**重开解码器：SPS 变了以后 libavcodec 会自己重建内部状态，
// 输出的 AVFrame 就是新尺寸。要重建的是下游（sws 缩放上下文、SDL 纹理、转码链的
// 编码器尺寸），不是这一层。
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

struct AVCodecContext;
struct AVPacket;
struct AVFrame;  // FFmpeg 的 typedef struct AVFrame AVFrame，前向声明即可

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

  // 丢弃解码器内部状态（参考帧/延迟帧）并进入"等下一个 IDR"状态。
  // 新连接、换代、或调用方自己知道丢了帧时必须调用：旧参考帧会一直残留到下一个 IDR，
  // 表现为持续花屏，而不是"少一帧"。
  void flush();

  // 喂一帧 Annex-B 数据；返回解码出的最后一张帧（借用指针，下次 decode 前有效；
  // nullptr 表示这次没有输出）。两种"没有输出"必须由调用方按 err 区分：
  //   · err 空 + 返回 nullptr：正常无输出，或正在等 IDR（见 waiting_keyframe()）；
  //   · err 非空：真的解不出来（不是 Annex-B、缺参数集、send/receive 报错）。
  AVFrame* decode(std::span<const uint8_t> frame, std::string& err);

  // 是否正处在"等下一个 IDR"的恢复状态。丢弃期间 decode() 不产帧也不报错，
  // 只看 decode_errors 会把"解码器在等恢复"读成"链路健康但没帧"。
  bool waiting_keyframe() const { return waiting_keyframe_; }

  // 累计统计
  struct Stats {
    uint64_t packets = 0;
    uint64_t frames = 0;
    uint64_t errors = 0;
    uint64_t dropped = 0;  // 未被解码器消费的包（send_packet 失败 / 持续 EAGAIN / 等 IDR 主动丢）
    uint64_t flushes = 0;
    // 其中"等 IDR/参数集"而主动丢掉的包。它不是解码失败，但同样是"这一包没进解码器"，
    // 所以也计入 dropped；分开记是因为处置完全不同（一个要查丢包，一个是设计行为）。
    uint64_t gated = 0;
  };
  const Stats& stats() const { return stats_; }

 private:
  AVCodecContext* ctx_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVFrame* last_ = nullptr;
  AVPacket* pkt_ = nullptr;
  Stats stats_;
  // open()/flush() 之后必为 true：此时只有 IDR 能重建参考链。
  bool waiting_keyframe_ = true;
  // 见过 SPS/PPS 没有。没有它们时解码器对每个包都回 AVERROR_INVALIDDATA
  // （实测 -1094995529），而且**永不自愈**——只能等一个自带参数集的 IDR。
  bool saw_sps_ = false;
  bool saw_pps_ = false;
};

}  // namespace cue::media
