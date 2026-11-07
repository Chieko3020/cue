#include <cmath>
#include <cstring>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include "cue/media/audio_resampler.h"

namespace cue::media {

AudioResampler::~AudioResampler() { close(); }

void AudioResampler::close() {
  if (swr_) {
    swr_free(&swr_);
    swr_ = nullptr;
  }
}

bool AudioResampler::open(const ResampleSpec& spec, std::string& err) {
  close();
  if (spec.in_rate <= 0 || spec.out_rate <= 0 || spec.in_channels <= 0 || spec.out_channels <= 0) {
    err = "invalid resample spec: rate/channels must be positive";
    return false;
  }
  if (spec.in_channels > 8 || spec.out_channels > 8) {
    err = "unsupported channel count";
    return false;
  }
  spec_ = spec;
  AVChannelLayout in_layout{};
  AVChannelLayout out_layout{};
  av_channel_layout_default(&in_layout, spec.in_channels);
  av_channel_layout_default(&out_layout, spec.out_channels);
  const int rc = swr_alloc_set_opts2(&swr_, &out_layout, AV_SAMPLE_FMT_S16, spec.out_rate,
                                     &in_layout, AV_SAMPLE_FMT_S16, spec.in_rate, 0, nullptr);
  if (rc < 0 || swr_ == nullptr) {
    err = "swr_alloc_set_opts2 failed: " + std::to_string(rc);
    swr_ = nullptr;
    return false;
  }
  if (spec.force_resample && av_opt_set_int(swr_, "flags", SWR_FLAG_RESAMPLE, 0) < 0) {
    err = "cannot force resampling";
    close();
    return false;
  }
  // dither 关闭：s16 → s16 没有位深变化，开 dither 只会多一层噪声；
  // 重采样矩阵/相位由 swr 的默认多相滤波器决定，不用 soxr（不引入额外依赖）。
  if (swr_init(swr_) < 0) {
    err = "swr_init failed";
    close();
    return false;
  }
  return true;
}

std::expected<void, ResampleError> AudioResampler::compensate(int sample_delta, int distance,
                                                             std::string& err) {
  if (!swr_ || distance < 0 || (distance == 0 && sample_delta != 0)) {
    err = "invalid compensation state or distance";
    return std::unexpected(ResampleError::InvalidSpec);
  }
  const int rc = swr_set_compensation(swr_, sample_delta, distance);
  if (rc < 0) {
    err = "swr_set_compensation failed: " + std::to_string(rc);
    return std::unexpected(ResampleError::ConvertFailed);
  }
  return {};
}

std::size_t AudioResampler::output_frames_for(std::size_t in_frames, int in_rate, int out_rate) {
  if (in_rate <= 0 || out_rate <= 0) return 0;
  const double exact = static_cast<double>(in_frames) * static_cast<double>(out_rate) /
                       static_cast<double>(in_rate);
  return static_cast<std::size_t>(std::llround(exact));
}

std::expected<void, ResampleError> AudioResampler::convert(const int16_t* in, std::size_t in_frames,
                                                           std::vector<int16_t>& out,
                                                           std::string& err) {
  if (!swr_) {
    err = "resampler not open";
    return std::unexpected(ResampleError::InvalidSpec);
  }
  if (in_frames == 0) return {};

  // swr_get_out_samples 给的是上界（含内部延迟的余量），按上界分配后按返回值收缩，
  // 既不会截断也不会每帧多分配一整块。
  const int out_cap = swr_get_out_samples(swr_, static_cast<int>(in_frames));
  if (out_cap < 0) {
    err = "swr_get_out_samples failed";
    return std::unexpected(ResampleError::ConvertFailed);
  }
  const std::size_t base = out.size();
  out.resize(base + static_cast<std::size_t>(out_cap) * static_cast<std::size_t>(spec_.out_channels));
  uint8_t* out_ptr = reinterpret_cast<uint8_t*>(out.data() + base);
  const uint8_t* in_ptr = reinterpret_cast<const uint8_t*>(in);
  const int got = swr_convert(swr_, &out_ptr, out_cap, &in_ptr, static_cast<int>(in_frames));
  if (got < 0) {
    out.resize(base);
    err = "swr_convert failed: " + std::to_string(got);
    return std::unexpected(ResampleError::ConvertFailed);
  }
  out.resize(base + static_cast<std::size_t>(got) * static_cast<std::size_t>(spec_.out_channels));
  return {};
}

std::expected<void, ResampleError> AudioResampler::flush(std::vector<int16_t>& out,
                                                         std::string& err) {
  if (!swr_) {
    err = "resampler not open";
    return std::unexpected(ResampleError::InvalidSpec);
  }
  // 一次 flush 可能不够：滤波器长度以内可能还剩几帧，循环到 swr_convert 返回 0。
  for (int guard = 0; guard < 32; ++guard) {
    const int out_cap = swr_get_out_samples(swr_, 0);
    if (out_cap <= 0) return {};
    const std::size_t base = out.size();
    out.resize(base + static_cast<std::size_t>(out_cap) *
                          static_cast<std::size_t>(spec_.out_channels));
    uint8_t* out_ptr = reinterpret_cast<uint8_t*>(out.data() + base);
    const int got = swr_convert(swr_, &out_ptr, out_cap, nullptr, 0);
    if (got < 0) {
      out.resize(base);
      err = "swr_convert(flush) failed: " + std::to_string(got);
      return std::unexpected(ResampleError::ConvertFailed);
    }
    out.resize(base + static_cast<std::size_t>(got) * static_cast<std::size_t>(spec_.out_channels));
    if (got == 0) return {};
  }
  return {};
}

}  // namespace cue::media
