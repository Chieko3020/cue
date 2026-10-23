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
    std::span<const uint8_t> packet, std::string& err) {
  if (dec_ == nullptr) {
    err = "opus decoder not open";
    return std::unexpected(AudioDecodeError::NotOpen);
  }
  if (packet.empty()) {
    // 空包绝不能喂 opus_decode：libopus 把 (nullptr, 0) 当作"丢包"走 PLC，
    // 会凭空造出一帧隐藏音频，指标上看起来"解成功了"，其实是假的。
    err = "empty opus packet (PLC would fabricate a frame)";
    return std::unexpected(AudioDecodeError::EmptyPacket);
  }
  if (packet.size() > kMaxOpusPacketBytes) {
    err = "opus packet too large: " + std::to_string(packet.size()) + " > " +
          std::to_string(kMaxOpusPacketBytes) + " bytes (RFC 6716 §3.2.5)";
    return std::unexpected(AudioDecodeError::PacketTooLarge);
  }

  // 先问包声明了多少样本：既拿来做输出上限，也当作 TOC 合法性的第一道校验。
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

  const int n = opus_decode(dec_, packet.data(), static_cast<opus_int32>(packet.size()), out_.data(),
                            declared, /*decode_fec=*/0);
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

}  // namespace cue::media
