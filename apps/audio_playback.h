// Application-private queue policy; the SDL adapter and tests supply only device operations.
#pragma once

#include <cstdint>
#include <limits>

namespace cue::probe {

class AudioQueueDevice {
 public:
  virtual ~AudioQueueDevice() = default;
  virtual bool available() const = 0;
  virtual int queued_ms() const = 0;
  virtual void pause(bool paused) = 0;
  virtual void clear() = 0;
  virtual bool queue(const int16_t* pcm, uint32_t bytes) = 0;
};

struct PlaybackStats {
  uint64_t over_soft = 0;
  uint64_t dropped_drift = 0;
  uint64_t starved = 0;
  uint64_t trimmed = 0;
  uint64_t queued = 0;
  uint64_t push_errors = 0;
};

class AudioPlaybackQueue {
 public:
  explicit AudioPlaybackQueue(AudioQueueDevice& device) : device_(device) {}
  static bool valid_prebuffer(int ms) { return ms >= 0 && ms <= 2000; }
  bool configure(int prebuffer_ms) {
    if (!valid_prebuffer(prebuffer_ms)) return false;
    prebuffer_ms_ = prebuffer_ms;
    started_ = prebuffer_ms_ == 0;
    was_empty_ = false;
    if (device_.available()) device_.pause(!started_);
    return true;
  }
  void reset() {
    if (!device_.available()) return;
    device_.pause(true);
    device_.clear();
    started_ = prebuffer_ms_ == 0;
    was_empty_ = false;
    if (started_) device_.pause(false);
  }
  void push(const int16_t* pcm, int samples) {
    if (!device_.available() || !pcm || samples <= 0) return;
    const auto bytes = static_cast<uint64_t>(samples) * sizeof(int16_t);
    if (bytes > std::numeric_limits<uint32_t>::max()) return;
    const int depth = device_.queued_ms();
    if (depth > 2000) {
      reset();
      ++stats_.trimmed;
    } else if (started_ && depth > 250) {
      ++stats_.over_soft;
      ++stats_.dropped_drift;
      return;
    }
    if (!device_.queue(pcm, static_cast<uint32_t>(bytes))) {
      ++stats_.push_errors;
      return;
    }
    ++stats_.queued;
    if (!started_ && device_.queued_ms() >= prebuffer_ms_) {
      started_ = true;
      device_.pause(false);
    }
    // Preserve the existing arrival-time sample; idle starvation sampling belongs to C10.
    const bool empty = device_.queued_ms() <= 0;
    if (started_ && empty && !was_empty_) ++stats_.starved;
    was_empty_ = empty;
  }
  bool started() const { return started_; }
  const PlaybackStats& stats() const { return stats_; }

 private:
  AudioQueueDevice& device_;
  int prebuffer_ms_ = 0;
  bool started_ = false;
  bool was_empty_ = false;
  PlaybackStats stats_;
};

}  // namespace cue::probe
