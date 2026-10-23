// Opus 解码（cued 侧上行音频）：48 kHz 单声道，一个上行包 = 一个 Opus 包 = 一帧。
//
// 为什么直接链 libopus 而不是 libavcodec 的 opus 解码器：上行载荷就是**裸 Opus 包**
// （见 proto/upstream_pack.h：32 字节 MediaHeader 之后只有编码器吐出的字节，没有容器、
// 没有时间基），libavcodec 的帧装填层在这里没有东西可装；libopus 直接吃字节数组，
// 与采集端 OpusAudioEncoder 用同一份库，编码/解码的口径（48 kHz / mono / 20 ms）
// 在类型上就对齐。
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

struct OpusDecoder;  // 全局命名空间里的 opus.h 类型

namespace cue::media {

inline constexpr int kAudioSampleRate = 48000;
inline constexpr int kAudioChannels = 1;
inline constexpr int kAudioFrameMs = 20;
// 20 ms @ 48 kHz = 960 样本：与采集端 OpusAudioEncoder::kFrameSamples、
// 浏览器侧 AUDIO_FRAME_SAMPLES 是同一个数。
inline constexpr int kAudioFrameSamples = kAudioSampleRate * kAudioFrameMs / 1000;

// 一个 Opus 包最长 120 ms（RFC 6716 §3.2.5），48 kHz 下是 5760 样本。
// 解码输出必须按这个上限分配：伪造/损坏的 TOC 可以声明 120 ms 帧，
// 按"20 ms 一帧"预分配 960 会直接越界写。这是上限保护，不是 20 ms 的期望值。
inline constexpr int kAudioMaxFrameSamples = kAudioSampleRate * 120 / 1000;

// 一个 Opus 包最大 1275 字节（RFC 6716 §3.2.5）。超过的载荷按协议错误拒绝，
// 不喂给解码器：libopus 自己只读 TOC，对超长包也照解（实测 1276 字节返回 120 样本），
// 那等于让畸形输入悄悄进解码路径。
inline constexpr std::size_t kMaxOpusPacketBytes = 1275;

// dBFS 下限。JSON 没有 -inf，而数字静音（全零 PCM）的 RMS 就是 0 → -inf。
// -120 dBFS 比 16 bit 的 1 LSB（-90.3 dBFS）还低 30 dB，读作"静音"不会歧义；
// 这是显式下限（真值只会更低），不是把读数改好看。
inline constexpr double kSilenceFloorDbfs = -120.0;

enum class AudioDecodeError : uint8_t {
  NotOpen = 0,     // 解码器没打开（open 失败或未调用）
  EmptyPacket,     // 0 字节载荷：不喂给 libopus，否则它走 PLC 会"凭空造出"一帧
  PacketTooLarge,  // > kMaxOpusPacketBytes：不是一个合法的 Opus 包
  InvalidPacket,   // TOC/帧结构损坏（opus_packet_get_nb_samples 拒绝）
  DecodeFailed,    // opus_decode 返回负值（OPUS_INVALID_PACKET / OPUS_BUFFER_TOO_SMALL 等）
};

inline const char* audio_decode_error_name(AudioDecodeError e) {
  switch (e) {
    case AudioDecodeError::NotOpen: return "not_open";
    case AudioDecodeError::EmptyPacket: return "empty_packet";
    case AudioDecodeError::PacketTooLarge: return "packet_too_large";
    case AudioDecodeError::InvalidPacket: return "invalid_packet";
    case AudioDecodeError::DecodeFailed: return "decode_failed";
  }
  return "unknown";
}

struct DecodedAudioFrame {
  std::vector<int16_t> samples;  // 交错 s16；单声道时长度 == 每声道样本数
  int sample_rate = kAudioSampleRate;
  int channels = kAudioChannels;
  std::size_t sample_count() const { return samples.size(); }
};

// RMS → dBFS：参考电平是 int16 满量程 32768（0 dBFS）。
// 因此满幅正弦的理论读数是 20·log10(23170/32768) = -3.01 dBFS，不是 0。
// 全零输入返回 -infinity；写进指标前用 dbfs_for_metric() 夹到下限。
double rms_dbfs(std::span<const int16_t> pcm);

// 由 Σx² 与样本数直接算 dBFS。窗口聚合（最近 1 s）不需要留着原始 PCM，
// 只要这两个量；口径与 rms_dbfs() 完全一致（同一个参考电平）。
// samples == 0 或 sum_sq == 0 返回 -infinity。
double dbfs_from_sum_sq(double sum_sq, std::uint64_t samples);

// 把 -inf 夹到 kSilenceFloorDbfs（JSON 放不下 -inf）。其他取值原样返回——
// 16 bit 的 1 LSB 对应约 -90 dBFS，正常流不可能真的低于 -120。
double dbfs_for_metric(double dbfs);

class OpusAudioDecoder {
 public:
  OpusAudioDecoder() = default;
  ~OpusAudioDecoder();
  OpusAudioDecoder(const OpusAudioDecoder&) = delete;
  OpusAudioDecoder& operator=(const OpusAudioDecoder&) = delete;

  bool open(std::string& err);
  void close();
  bool is_open() const { return dec_ != nullptr; }

  // packet 是上行包里 MediaHeader 之后的那段字节（裸 Opus 包）。
  // 失败时 err 是一句人读原因（带 opus_strerror），调用方必须计入 audio_decode_errors；
  // 这个函数不吞错误、也不做 concealment（PLC）——解不出来就是解不出来。
  std::expected<DecodedAudioFrame, AudioDecodeError> decode(std::span<const uint8_t> packet,
                                                            std::string& err);

 private:
  ::OpusDecoder* dec_ = nullptr;
  // 复用输出缓冲：每帧最多 5760 个 s16。解码器由调用方长期持有（每会话一个），
  // 每 20 ms 一次分配没必要。
  std::vector<int16_t> out_;
};

}  // namespace cue::media
