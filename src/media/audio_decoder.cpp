#include "cue/media/audio_decoder.h"

#include <cmath>
#include <limits>

#include <opus.h>

namespace cue::media {

double rms_dbfs(std::span<const int16_t> pcm) {
  // 没有样本 = 没有能量：与数字静音同样处理（-inf），调用方再用 null 区分
  // "根本没解出帧"和"解出了但没声"。
  if (pcm.empty()) return -std::numeric_limits<double>::infinity();
  double sum_sq = 0.0;
  for (const int16_t v : pcm) {
    const double d = static_cast<double>(v);
    sum_sq += d * d;
  }
  return dbfs_from_sum_sq(sum_sq, static_cast<std::uint64_t>(pcm.size()));
}

double dbfs_from_sum_sq(double sum_sq, std::uint64_t samples) {
  if (samples == 0 || sum_sq <= 0.0) return -std::numeric_limits<double>::infinity();
  const double mean_sq = sum_sq / static_cast<double>(samples);
  // 参考电平 int16 满量程 32768：rms == 32768 才是 0 dBFS，
  // 满幅正弦 (rms = 32768/√2) 读 -3.01 dBFS。
  return 10.0 * std::log10(mean_sq / (32768.0 * 32768.0));
}

double dbfs_for_metric(double dbfs) {
  if (std::isnan(dbfs)) return kSilenceFloorDbfs;  // 没有样本：调用方本该写 null
  return dbfs < kSilenceFloorDbfs ? kSilenceFloorDbfs : dbfs;
}

OpusAudioDecoder::~OpusAudioDecoder() { close(); }

void OpusAudioDecoder::close() {
  if (dec_ != nullptr) {
    opus_decoder_destroy(dec_);
    dec_ = nullptr;
  }
  // FEC 探针与主解码器同生共死：它只是用来判定"包里有没有 LBRR"，
  // 不参与播放，留着没有意义。
  if (fec_probe_ != nullptr) {
    opus_decoder_destroy(fec_probe_);
    fec_probe_ = nullptr;
  }
  out_.clear();
}

bool OpusAudioDecoder::open(std::string& err) {
  if (dec_ != nullptr) return true;
  int opus_err = 0;
  // 单声道解码器也能解立体声包（Opus 规定解码器可向下混音），
  // 所以上行如果哪天改成双声道，这里不用改。
  dec_ = opus_decoder_create(kAudioSampleRate, kAudioChannels, &opus_err);
  if (dec_ == nullptr || opus_err != OPUS_OK) {
    err = std::string("opus_decoder_create failed: ") + opus_strerror(opus_err);
    dec_ = nullptr;
    return false;
  }
  // 按规范单包上限预分配：一帧最多 120 ms（5760 样本），20 ms 的包只用到前 960 个。
  out_.assign(static_cast<std::size_t>(kAudioMaxFrameSamples), 0);
  return true;
}

std::expected<DecodedAudioFrame, AudioDecodeError> OpusAudioDecoder::decode(
    std::span<const uint8_t> packet, std::string& err, DecodeMode mode) {
  if (dec_ == nullptr) {
    err = "opus decoder not open";
    return std::unexpected(AudioDecodeError::NotOpen);
  }
  if (packet.empty()) {
    // 空包绝不能喂 opus_decode：libopus 把 (nullptr, 0) 当作"丢包"走 PLC，
    // 会凭空造出一帧隐藏音频，指标上看起来"解成功了"，其实是假的。
    // FecRecover 也不例外：这里先拒，再谈 FEC。
    err = "empty opus packet (PLC would fabricate a frame)";
    return std::unexpected(AudioDecodeError::EmptyPacket);
  }
  if (packet.size() > kMaxOpusPacketBytes) {
    err = "opus packet too large: " + std::to_string(packet.size()) + " > " +
          std::to_string(kMaxOpusPacketBytes) + " bytes (RFC 6716 §3.2.5)";
    return std::unexpected(AudioDecodeError::PacketTooLarge);
  }

  // 先问包声明了多少样本：既拿来做输出上限，也当作 TOC 合法性的第一道校验。
  // FEC 恢复同样按这个值申请输出：实测 frame_size 传 960 时 FEC 解码返回 960 个
  // **真实的上一帧样本**；传更大的值（例如 5760）它会按 PLC 继续外推补满，把"恢复的
  // 那一帧"和"外推的尾巴"混在一起，所以这里必须用包自己声明的时长。
  const int declared = opus_packet_get_nb_samples(packet.data(), static_cast<opus_int32>(packet.size()),
                                                  kAudioSampleRate);
  if (declared <= 0) {
    err = std::string("opus_packet_get_nb_samples failed: ") + opus_strerror(declared);
    return std::unexpected(AudioDecodeError::InvalidPacket);
  }
  if (declared > kAudioMaxFrameSamples) {
    // 规范上不该出现（单包 ≤ 120 ms），但输出缓冲按这个上限分配，越界必须先拒。
    err = "opus packet declares " + std::to_string(declared) + " samples > max " +
          std::to_string(kAudioMaxFrameSamples);
    return std::unexpected(AudioDecodeError::InvalidPacket);
  }

  // FEC 前置判定：包里没有 LBRR 时 opus_decode(decode_fec=1) 会**静默退化成 PLC**，
  // 仍然返回 declared 个样本。那条路径必须让调用方显式走 conceal() 并单独计数，
  // 否则"没有冗余可恢复"会被粉饰成"FEC 恢复成功"。判定发生在主解码器之前，
  // 所以失败时主解码器状态没有被这个包动过，调用方还能正常做 Normal 解码。
  if (mode == DecodeMode::FecRecover && !fec_payload_present(packet)) {
    err = "no in-band FEC (LBRR) in packet: previous frame is not recoverable";
    return std::unexpected(AudioDecodeError::FecUnavailable);
  }

  const int n = opus_decode(dec_, packet.data(), static_cast<opus_int32>(packet.size()), out_.data(),
                            declared,
                            /*decode_fec=*/mode == DecodeMode::FecRecover ? 1 : 0);
  if (n < 0) {
    err = std::string("opus_decode failed: ") + opus_strerror(n);
    // 失败后复用同一个解码器继续解下一包：libopus 在 OPUS_INVALID_PACKET 后是自同步的
    // （包边界由调用方按 20 ms 一包给出，不依赖解码器状态）。
    return std::unexpected(AudioDecodeError::DecodeFailed);
  }
  if (n == 0) {
    // 0 样本不是"解出了一帧空音频"：它会让 audio_decoded=true 而 audio_pcm_frames 不涨，
    // 正是"看起来通了其实没通"。按失败记，不粉饰。
    err = "opus_decode returned 0 samples";
    return std::unexpected(AudioDecodeError::DecodeFailed);
  }

  DecodedAudioFrame f;
  f.sample_rate = kAudioSampleRate;
  f.channels = kAudioChannels;
  f.samples.assign(out_.begin(), out_.begin() + n);
  return f;
}

std::expected<ConcealedFrame, AudioDecodeError> OpusAudioDecoder::conceal(std::string& err) {
  if (dec_ == nullptr) {
    err = "opus decoder not open";
    return std::unexpected(AudioDecodeError::NotOpen);
  }
  // PLC 的调用形状是 (data=nullptr, len=0)：libopus 据此隐藏"丢了一帧"，用上一帧的
  // 状态外推。frame_size 用 kAudioFrameSamples：PLC 会返回恰好这么多样本（实测 960），
  // 与上行 20 ms 一包的时间轴对齐。
  const int n = opus_decode(dec_, nullptr, 0, out_.data(), kAudioFrameSamples,
                            /*decode_fec=*/0);
  if (n < 0) {
    err = std::string("opus_decode PLC failed: ") + opus_strerror(n);
    return std::unexpected(AudioDecodeError::DecodeFailed);
  }
  if (n == 0) {
    // 与 decode() 同一口径：0 样本不当作"一帧静音"，否则 PLC 计数会虚高。
    err = "opus_decode PLC returned 0 samples";
    return std::unexpected(AudioDecodeError::DecodeFailed);
  }
  ConcealedFrame f;
  f.sample_rate = kAudioSampleRate;
  f.channels = kAudioChannels;
  f.samples.assign(out_.begin(), out_.begin() + n);
  return f;
}

bool OpusAudioDecoder::fec_payload_present(std::span<const uint8_t> packet) {
  if (packet.empty() || packet.size() > kMaxOpusPacketBytes) return false;
  if (fec_probe_ == nullptr) {
    int opus_err = 0;
    fec_probe_ = opus_decoder_create(kAudioSampleRate, kAudioChannels, &opus_err);
    if (fec_probe_ == nullptr || opus_err != OPUS_OK) {
      fec_probe_ = nullptr;
      return false;  // 探针建不起来 = 无法证明有冗余 → 当作没有，转 PLC
    }
  }
  const int declared = opus_packet_get_nb_samples(packet.data(), static_cast<opus_int32>(packet.size()),
                                                  kAudioSampleRate);
  if (declared <= 0 || declared > kAudioMaxFrameSamples) return false;
  // 每次判定前清零：有历史状态的解码器能从上一帧"预测"出内容，那样就分不清
  // "包里的冗余"和"解码器自己的外推"了。零状态下没有 LBRR 只能输出数字静音。
  opus_decoder_ctl(fec_probe_, OPUS_RESET_STATE);
  // 复用 out_：探索性解码的结果只用来算能量，随后 decode() 会把它整个覆盖掉。
  const int n = opus_decode(fec_probe_, packet.data(), static_cast<opus_int32>(packet.size()),
                            out_.data(), declared, /*decode_fec=*/1);
  if (n <= 0) return false;
  double sum_sq = 0.0;
  for (int i = 0; i < n; ++i) {
    const double v = static_cast<double>(out_[static_cast<std::size_t>(i)]);
    sum_sq += v * v;
  }
  // -100 dBFS 阈值：实测有 LBRR 时在 -19.6（双音）～-63.4（-60 dBFS 正弦）dBFS 之间，
  // 没有 LBRR 时是逐位全零（-120 dBFS 下限）。取两者中间、且比 16 bit 的 1 LSB
  // （-90.3 dBFS）还低 10 dB 的值，只用来区分"有冗余"与"全零"。
  return dbfs_from_sum_sq(sum_sq, static_cast<std::uint64_t>(n)) > -100.0;
}

}  // namespace cue::media
