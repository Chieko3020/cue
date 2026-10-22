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
  // 4000 B 足够 48 kbps × 20 ms（120 B）与任何 VBR 峰值；按 Opus 规范单包上限留余量。
  out_.resize(4000);
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
  return std::vector<uint8_t>(out_.begin(), out_.begin() + n);
}

}  // namespace cue::media
