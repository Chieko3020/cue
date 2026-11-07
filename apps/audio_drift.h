// Application-private playback clock adapter. FFmpeg generates all samples;
// this bounded queue feedback only selects a small compensation ratio.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include "cue/media/audio_resampler.h"
namespace cue::probe {
struct DriftStats {
  uint64_t updates = 0, errors = 0, input_samples = 0, output_samples = 0;
  int ppm = 0;
};
class AudioDrift {
 public:
  bool configure(int rate, int channels, int target_ms, bool enabled) {
    spec_ = {rate, channels, rate, channels, true};
    target_ms_ = std::max(20, target_ms);
    enabled_ = enabled;
    return reset();
  }
  bool reset() {
    next_update_us_ = 0;
    stats_.ppm = 0;
    resampler_.close();
    error_.clear();
    healthy_ = !enabled_ || resampler_.open(spec_, error_);
    if (!healthy_) ++stats_.errors;
    return healthy_;
  }
  void update(int depth_ms, int64_t now_us, bool playing) {
    if (!enabled_ || !healthy_ || !playing || depth_ms < 0) return;
    if (next_update_us_ != 0 && now_us < next_update_us_) return;
    next_update_us_ = now_us + 1'000'000;
    // Sample just before the next packet. A 20ms deadband absorbs packet/SDL
    // scheduling phase; proportional gain 25ppm/ms, limited to +/-1000ppm.
    // High depth -> fewer output samples (negative FFmpeg sample_delta).
    const int error_ms = depth_ms - target_ms_;
    const int desired = std::abs(error_ms) <= 20 ? 0 :
        -std::clamp(error_ms, -40, 40) * 25;
    const int delta = static_cast<int>(std::llround(double(desired) * spec_.out_rate / 1'000'000.0));
    if (!resampler_.compensate(delta, spec_.out_rate, error_)) {
      healthy_ = false;
      ++stats_.errors;
      stats_.ppm = 0;
      return;
    }
    ++stats_.updates;
    stats_.ppm = desired;
  }
  bool convert(const int16_t* pcm, int samples, std::vector<int16_t>& out) {
    out.clear();
    if (samples <= 0 || spec_.in_channels <= 0 || samples % spec_.in_channels != 0) {
      ++stats_.errors;
      return false;
    }
    if (!enabled_) out.assign(pcm, pcm + samples);
    else if (!healthy_ || !resampler_.convert(pcm, samples / spec_.in_channels, out, error_)) {
      if (healthy_) ++stats_.errors;
      healthy_ = false;
      return false;
    }
    stats_.input_samples += static_cast<uint64_t>(samples);
    stats_.output_samples += out.size();
    return true;
  }
  const DriftStats& stats() const { return stats_; }
  bool healthy() const { return healthy_; }
  const std::string& last_error() const { return error_; }
 private:
  cue::media::AudioResampler resampler_;
  cue::media::ResampleSpec spec_;
  DriftStats stats_;
  std::string error_;
  bool enabled_ = false, healthy_ = false;
  int target_ms_ = 250;
  int64_t next_update_us_ = 0;
};
} // namespace cue::probe
