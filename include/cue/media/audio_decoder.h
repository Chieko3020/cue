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
  // FecRecover 模式下，这个包**没有携带上一帧的 in-band FEC 冗余**（LBRR）。
  // 它不是"包坏了"，也不是"解不出来"：libopus 在 decode_fec=1 且没有 LBRR 时不会报错，
  // 而是用 PLC 造一帧 —— 那正是本文件反复强调的"假成功"。所以这里主动把它判成
  // unavailable，调用方转去走 conceal()（PLC 要被**显式**调用，并且单独计数）。
  FecUnavailable,
};

inline const char* audio_decode_error_name(AudioDecodeError e) {
  switch (e) {
    case AudioDecodeError::NotOpen: return "not_open";
    case AudioDecodeError::EmptyPacket: return "empty_packet";
    case AudioDecodeError::PacketTooLarge: return "packet_too_large";
    case AudioDecodeError::InvalidPacket: return "invalid_packet";
    case AudioDecodeError::DecodeFailed: return "decode_failed";
    case AudioDecodeError::FecUnavailable: return "fec_unavailable";
  }
  return "unknown";
}

// 解码模式。**默认 Normal**，所有既有调用点行为不变。
//
// 为什么 FEC 恢复要单独一个模式而不是给 decode() 加个 bool：两种调用的语义完全不同——
// Normal 解的是"这个包自己"，FecRecover 解的是"**上一个**（丢掉的）包的 LBRR 冗余"，
// 同一段字节在两种模式下产出的是**两个不同时刻**的 20 ms。用一个 bool 很容易在调用点
// 写反，而且调用方必须知道"先 FEC、再 Normal"的顺序（libopus 只支持这个顺序：
// FEC 解码会把解码器状态推进到丢包那一帧，之后再 Normal 解当前包，状态才落在正确时刻；
// 反过来先 Normal 再 FEC，解出来的就没法用）。
enum class DecodeMode : uint8_t {
  Normal = 0,
  FecRecover,  // opus_decode(..., decode_fec=1)：恢复"后一包里冗余的**前一帧**"
};

struct DecodedAudioFrame {
  std::vector<int16_t> samples;  // 交错 s16；单声道时长度 == 每声道样本数
  int sample_rate = kAudioSampleRate;
  int channels = kAudioChannels;
  std::size_t sample_count() const { return samples.size(); }
};

// PLC（丢包隐藏）产出的一帧。**刻意不是 DecodedAudioFrame**：它没有"解码成功"的语义，
// 唯一合法的产生条件是「调用方确知丢了一帧」（帧号出现缺口），绝不能用来给空包/坏包
// 兜底（见本文件 decode() 对 EmptyPacket 的说明：那正是"假成功"的来源）。
// 它是伪造（外推）的音频，只能进 plc 计数，不能进 decoded/pcm 计数。
struct ConcealedFrame {
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
  //
  // mode == FecRecover 时它解的是「**上一个**包的 in-band FEC 冗余」（libopus 的语义：
  // 冗余存在后一个包里，用于恢复前一个丢包），产出的是**上一帧**的 20 ms 音频，而不是
  // 当前包的。调用方在拿到它之后**还必须**再以 Normal 解同一个包，把当前帧也吐出来——
  // 只做 FEC 恢复就等于"恢复了但没播"。顺序必须是先 FecRecover 再 Normal，理由见
  // DecodeMode 的注释。
  //
  // FecRecover 在包内没有 LBRR 时返回 FecUnavailable（**不会**用 PLC 顶替），
  // 判定方式见 fec_payload_present() 的注释。
  std::expected<DecodedAudioFrame, AudioDecodeError> decode(std::span<const uint8_t> packet,
                                                            std::string& err,
                                                            DecodeMode mode = DecodeMode::Normal);

  // PLC：用「丢包」语义让 libopus 外推一帧（opus_decode(dec_, nullptr, 0, ...)）。
  // **只有确知丢了一帧的调用方才能调它**（在 cue-probe 里唯一的来源是 AudioSeqTracker
  // 算出的帧号缺口）。绝不能用它给空载荷/损坏包兜底：那会把"没收到"粉饰成"收到并播了"，
  // 正是 decode() 对 EmptyPacket 那条注释里的事故形状。
  // 产出样本数是解码器上一帧的时长（本项目恒为 kAudioFrameSamples = 960）。
  std::expected<ConcealedFrame, AudioDecodeError> conceal(std::string& err);

  // 这个包里是否真的带了上一帧的 in-band FEC 冗余（LBRR）。
  //
  // 为什么需要它：libopus **没有公开的 LBRR 查询接口**，而 opus_decode(decode_fec=1) 在
  // 没有 LBRR 时既不报错也不返回 0，而是走 PLC 造一帧（实测：包不带冗余时它返回 960
  // 样本、幅度与真恢复帧不可区分）。如果直接把它当"恢复成功"，audio_fec_recovered 就会
  // 把伪造帧算成真恢复。
  // 判定手段（实测）：把**专用的探针解码器**重置到零状态后做同一次 FEC 解码——
  //   · 有 LBRR：输出的是包里的真实冗余波形（实测 -19.6 dBFS @ 双音；
  //     -25.9 @ 满幅正弦；-63.4 @ -60 dBFS 正弦）；
  //   · 无 LBRR：零状态没有任何历史可外推，输出恰好是数字静音（-120 dBFS）；
  //   · DTX 帧（1 字节）：同样没有 LBRR，输出 -120 dBFS。
  // 阈值取 -100 dBFS（比 16 bit 的 1 LSB -90.3 dBFS 还低 10 dB），只用来区分
  // "有冗余"与"逐位全零"。代价是极安静段落里的真冗余会被判成没有（那时 PLC 补静音，
  // 听感上没有区别）；探针解码器的状态在每次调用前被重置，不影响主解码器。
  bool fec_payload_present(std::span<const uint8_t> packet);

 private:
  ::OpusDecoder* dec_ = nullptr;
  // FEC 冗余探针（见 fec_payload_present）；懒创建，只在发生缺口时才用到。
  ::OpusDecoder* fec_probe_ = nullptr;
  // 复用输出缓冲：每帧最多 5760 个 s16。解码器由调用方长期持有（每会话一个），
  // 每 20 ms 一次分配没必要。
  std::vector<int16_t> out_;
};

}  // namespace cue::media
