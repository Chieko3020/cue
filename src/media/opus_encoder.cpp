#include <cstring>

extern "C" {
#include <opus.h>
}

#include "cue/media/opus_encoder.h"

namespace cue::media {

OpusAudioEncoder::~OpusAudioEncoder() { close(); }

void OpusAudioEncoder::close() {
  if (enc_) {
    opus_encoder_destroy(enc_);
    enc_ = nullptr;
  }
}

bool OpusAudioEncoder::open(std::string& err) {
  close();
  int opus_err = 0;
  // OPUS_APPLICATION_AUDIO：宽频带通用音频（sine/语音/音乐都不至于像 VOIP 那样被砍高频）
  enc_ = opus_encoder_create(kSampleRate, 1, OPUS_APPLICATION_AUDIO, &opus_err);
  if (!enc_ || opus_err != OPUS_OK) {
    err = std::string("opus_encoder_create failed: ") + opus_strerror(opus_err);
    enc_ = nullptr;
    return false;
  }
  opus_encoder_ctl(enc_, OPUS_SET_BITRATE(kBitrateBps));
  // VBR 打开（浏览器 AudioEncoder 的默认行为也是 VBR）：静音段自动降码率。
  opus_encoder_ctl(enc_, OPUS_SET_VBR(1));
  apply_settings();
  // 4000 B 足够 48 kbps × 20 ms（120 B）与任何 VBR 峰值；按 Opus 规范单包上限留余量。
  out_.resize(4000);
  return true;
}

void OpusAudioEncoder::apply_settings() {
  if (!enc_) return;
  // 顺序无所谓（三个 ctl 互不依赖），但 loss perc 先于 FEC 更贴合文档里的因果关系：
  // 「先声明预期丢包率，再打开 in-band FEC」。loss perc == 0 时显式关掉 FEC：libopus
  // 在这个组合下本来也不产生 LBRR，显式关闭只是让 GET 回来的状态与 fec_enabled() 一致。
  opus_encoder_ctl(enc_, OPUS_SET_PACKET_LOSS_PERC(loss_perc_));
  opus_encoder_ctl(enc_, OPUS_SET_INBAND_FEC(fec_enabled() ? 1 : 0));
  opus_encoder_ctl(enc_, OPUS_SET_DTX(dtx_ ? 1 : 0));
}

bool OpusAudioEncoder::set_packet_loss_perc(int perc) {
  if (perc < kMinPacketLossPerc || perc > kMaxPacketLossPerc) return false;
  loss_perc_ = perc;
  apply_settings();
  return true;
}

bool OpusAudioEncoder::set_inband_fec(bool on) {
  fec_ = on;
  apply_settings();
  return true;
}

bool OpusAudioEncoder::set_dtx(bool on) {
  dtx_ = on;
  apply_settings();
  return true;
}

std::expected<std::vector<uint8_t>, OpusError> OpusAudioEncoder::encode(const int16_t* pcm,
                                                                       std::size_t samples,
                                                                       std::string& err) {
  if (!enc_) {
    err = "opus encoder not open";
    return std::unexpected(OpusError::NotOpen);
  }
  if (!pcm || samples != static_cast<std::size_t>(kFrameSamples)) {
    err = "opus frame must be exactly " + std::to_string(kFrameSamples) + " samples, got " +
          std::to_string(samples);
    return std::unexpected(OpusError::InvalidFrame);
  }
  const int n = opus_encode(enc_, pcm, kFrameSamples, out_.data(),
                            static_cast<opus_int32>(out_.size()));
  if (n < 0) {
    err = std::string("opus_encode failed: ") + opus_strerror(n);
    return std::unexpected(OpusError::EncodeFailed);
  }
  // DTX 帧是 **1 字节**（实测 min=1），不是 0 字节：0 字节载荷在下游会被当成"空包"
  // 拒绝（audio_decoder.h 的 EmptyPacket）——TOC 必须有一个字节才存在。
  return std::vector<uint8_t>(out_.begin(), out_.begin() + n);
}

}  // namespace cue::media
