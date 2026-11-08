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
  // level_idc：AVC level ×10（30 = L3.0、40 = L4.0）；0 = 交给 x264 自动推导。
  //   cued 的转码链一直用自动值（默认参数不变）；采集端按浏览器侧 pickCodec 的
  //   「coded area」规则显式指定，否则同一分辨率在两条产线上会得到不同的 level，
  //   probe 侧无法对照「编码参数变了没」。
  bool open(int w, int h, int bitrate_kbps, int fps, int gop, std::string_view preset,
            std::string& err, int threads = 1, int level_idc = 0);
  void close();
  bool is_open() const;
  // 让**下一帧**编码成 IDR。接收端丢帧后参考链就断了，它只能等下一个 IDR 才恢复；
  // 与其等满一个 GOP（默认 1 秒），不如由它主动请求、这里立刻插一个关键帧。
  // 用"置位等下一帧"而不是立刻编码：编码必须在编码线程里做，而且中间帧不能跳过
  // （跳过会让时间戳与帧序错位）。
  void force_keyframe() { force_keyframe_ = true; }

  // encode_ns：非空时写回「拷入 + x264_encoder_encode」的耗时（微秒），供六段埋点把
  // encode 从 op 里拆出来；失败/无输出时也会写回已发生的耗时（0 表示未进入编码）。
  // 失败原因用 EncodeError 分类，err 只放人读细节。
  std::expected<EncodedResult, EncodeError> encode(const AVFrame* frame, int64_t pts,
                                                   std::string& err,
                                                   int64_t* encode_ns = nullptr);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  // 下一次 encode 是否强制 IDR（见 force_keyframe）
  bool force_keyframe_ = false;
};

class ProcessorChain;

class TranscodeChain {
 public:
  TranscodeChain();
  ~TranscodeChain();
  TranscodeChain(const TranscodeChain&) = delete;
  TranscodeChain& operator=(const TranscodeChain&) = delete;

  // dst_w/dst_h must be positive even dimensions (same contract as X264Encoder).
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
  std::unique_ptr<ProcessorChain> chain_;
  int dst_w_ = 0;
  int dst_h_ = 0;
  bool enabled_ = false;
};

}  // namespace cue::media
