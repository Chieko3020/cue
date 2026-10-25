// Opus 编码：48 kHz 单声道、20 ms 一帧、48 kbps（与浏览器侧 AudioEncoder 的配置一致）。
//
// 为什么直接链 libopus 而不是走 libavcodec 的 libopus 封装：编码器是采集端的固定组件，
// 不需要 libavcodec 的 codec 查找/帧装填层；一拍 960 样本 → 一个 Opus 包正是上行协议
// 里 TYPE_AUDIO 的载荷。FFmpeg 里 libopus 默认也会把 20 ms 切成独立包。
//
// ---- 弱网：in-band FEC / DTX（PLC 在解码侧，见 audio_decoder.h）----
//
// in-band FEC：OPUS_SET_INBAND_FEC(1) + OPUS_SET_PACKET_LOSS_PERC(N)。语义是
// 「把**本帧**的 LBRR 冗余塞进**下一个包**」，所以接收端只能用后一个包恢复前一个丢包
// （见 audio_decoder.h 的 DecodeMode::FecRecover）。libopus 文档写得很清楚：loss perc
// 为 0 时不产生 LBRR；N 足够高时它会**自动切到 SILK**（哪怕码率很高）来让 FEC 生效
// （opus_defines.h 里 OPUS_SET_INBAND_FEC 的说明：1 = "will automatically switch to SILK"，
// 2 = "不因为音乐就切"）。FEC 不是免费的：它从同一个 48 kbps 预算里划走冗余，代价落在
// 主信号上（实测固定 48 kbps、连续正弦下平均包长 121.2 → 117.3 B，包变短就是主信号分到
// 的比特变少了）。
//
// N 默认 10 的依据（**实测出来的硬门槛**，不是"越大越好"的经验）：
//   同一份连续正弦、500 个 20 ms 帧，逐个包用"零状态探针"判定包里到底有没有 LBRR：
//     packet_loss_perc = 0  →   0/500 帧带 LBRR（FEC 完全不存在）
//     packet_loss_perc = 5  →  18/500 帧带 LBRR（4%！）
//     packet_loss_perc = 10 → 500/500 帧带 LBRR（100%）
//   也就是说默认填 5 的话，编码器只为 4% 的帧准备冗余，FEC 在 5% 丢包下只能救回
//   约 1/25 的缺口——真链路 A/B 也印证了这一点（14 个缺口只恢复 3 帧）。填 10 才能
//   覆盖到"每一帧丢了都有后一包可救"。代价是同一码率下约 4% 的比特让给冗余；实测
//   编码器仍报 FB（20 kHz）带宽，没有把音乐砍成窄带，"切 SILK"在本配置里没有发生。
//   N 大于 20 没有额外收益（LBRR 覆盖率已经 100%，多填只是让编码器更保守地分配码率），
//   所以默认取最低的有效值 10。
//
// 接收端兜底：真丢包率高于 N 时多出来的缺口由 PLC 补（每次缺口最多 3 帧，见
// cue-probe 的 kMaxPlcFrames），PLC 补不上的才计入 audio_unconcealed_frames。
//
// DTX（OPUS_SET_DTX）默认**开启**，依据是本机实测（20 ms 帧，不是推测）：
//   · 活动语音帧的码流**逐字节不变**：连续正弦 10 s（500 帧）在 DTX 开/关下平均包长
//     都是 121.24 B，min/max 也一致；
//   · 停顿段提前 100 ms 停发：正弦/静音 0.5 s 交替时，静音段在 DTX 下是 10 帧 121 B
//     + 15 帧 1 B，关掉 DTX 是 15 帧 121 B + 10 帧 3 B；解码后由 -53…-91 dBFS 的残留
//     衰减变成真正的数字静音（-120 dBFS）；
//   · 半占空比素材平均包长 99.8 B → 87.4 B（-12.4%）；-60 dBFS 低噪素材的包长分布
//     完全不变（VAD 判成有声，不误切），"轻声说话被吞掉"在本机没有复现；
//   · 已知代价：DTX 帧不带 LBRR，静音段丢包只能靠 PLC 兜（实测 20% 丢包时整体 RMS
//     -12.04 → -12.51 dBFS；5% 时 -12.04 → -12.05 dBFS，噪声量级）。
//   真机上若出现"轻声被吞"，用 cue-capture --opus-dtx off 关掉（kill switch）。
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

  // packet_loss_perc 的合法范围就是 0..100（opus_defines.h 的 OPUS_SET_PACKET_LOSS_PERC
  // 明文写着 "Loss percentage in the range 0-100, inclusive"）。越界一律**拒绝**而不是
  // 夹取：夹取会把 500% 静默变成 100%，让"参数写错"看起来像"参数生效了"。
  // 默认 10 不是"预期丢包率"，而是"让 libopus 真的生成 LBRR 的最小值"（实测：5 → 只有
  // 4% 的帧带冗余，10 → 100%），理由与实测数据见文件头。
  static constexpr int kDefaultPacketLossPerc = 10;
  static constexpr int kMinPacketLossPerc = 0;
  static constexpr int kMaxPacketLossPerc = 100;

  OpusAudioEncoder() = default;
  ~OpusAudioEncoder();
  OpusAudioEncoder(const OpusAudioEncoder&) = delete;
  OpusAudioEncoder& operator=(const OpusAudioEncoder&) = delete;

  bool open(std::string& err);
  void close();
  bool is_open() const { return enc_ != nullptr; }
  int frame_samples() const { return kFrameSamples; }
  int channels() const { return 1; }

  // ---- 弱网配置（open 前后都可以调；已经 open 时立即下发到 libopus）----

  // 预期的上行丢包率（%）。0 = 不带 LBRR（libopus 的行为：loss perc 为 0 时不产生 FEC
  // 冗余，此时 OPUS_SET_INBAND_FEC 写什么都不会有冗余可恢复）。越界返回 false 且不改
  // 当前值——调用方（CLI/配置）应当把 false 当参数错误处理。
  // 注意它不是"线性的旋钮"：libopus 用这个值决定**多少帧**准备 LBRR。实测连续正弦下
  // 5 → 只有 4% 的帧带冗余，10 → 100%。所以调低它不会"少一点冗余"，而是让 FEC 几乎失效。
  bool set_packet_loss_perc(int perc);
  int packet_loss_perc() const { return loss_perc_; }

  // in-band FEC 开关（与 loss perc 一起决定是否真的带 LBRR，见 fec_enabled()）。
  bool set_inband_fec(bool on);
  // 生效判据：开关打开 **且** loss perc > 0。libopus 在 loss perc == 0 时既不产生冗余，
  // 也不切 SILK，所以这里如实返回 false，而不是只回显开关状态。
  bool fec_enabled() const { return fec_ && loss_perc_ > 0; }

  // DTX（不连续发送）：静音段发 1 字节 DTX 帧而不是完整包。默认开，依据见文件头。
  bool set_dtx(bool on);
  bool dtx_enabled() const { return dtx_; }

  // pcm 必须是恰好 960 个 mono s16 样本（不足/超出都拒：Opus 不做跨帧缓冲，
  // 跨帧缓冲留给调用方，否则 pts 与帧边界会对不上）。
  std::expected<std::vector<uint8_t>, OpusError> encode(const int16_t* pcm, std::size_t samples,
                                                        std::string& err);

 private:
  // 把 loss_perc_/fec_/dtx_ 下发到 enc_。三个 ctl 都是幂等的，重复设置没有副作用。
  void apply_settings();

  ::OpusEncoder* enc_ = nullptr;
  int loss_perc_ = kDefaultPacketLossPerc;
  bool fec_ = true;
  bool dtx_ = true;
  std::vector<uint8_t> out_;
};

}  // namespace cue::media
