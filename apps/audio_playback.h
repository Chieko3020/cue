// Application-private queue policy; the SDL adapter and tests supply only device operations.
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace cue::probe {

enum class AudioQueueResult { Failed, Buffered, Queued };

class AudioQueueDevice {
 public:
  virtual ~AudioQueueDevice() = default;
  virtual bool available() const = 0;
  virtual int queued_ms() const = 0;
  virtual bool empty() const { return queued_ms() == 0; }
  virtual void reset_processing() {}
  virtual void pause(bool paused) = 0;
  virtual void clear() = 0;
  virtual AudioQueueResult queue(const int16_t* pcm, uint32_t bytes) = 0;
};

struct PlaybackStats {
  uint64_t over_soft = 0;
  uint64_t dropped_drift = 0;
  uint64_t starved = 0;
  uint64_t trimmed = 0;
  uint64_t queued = 0;
  uint64_t push_errors = 0;
  uint64_t rebuffered = 0;
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
    have_pcm_ = false;
    if (device_.available()) device_.pause(!started_);
    return true;
  }
  void reset() {
    if (!device_.available()) return;
    device_.pause(true);
    device_.clear();
    device_.reset_processing();
    started_ = prebuffer_ms_ == 0;
    was_empty_ = false;
    have_pcm_ = false;
    if (started_) device_.pause(false);
  }
  int soft_limit_ms() const { return std::max(250, prebuffer_ms_ + 100); }
  int hard_limit_ms() const { return std::max(2000, prebuffer_ms_ + 250); }
  // Main-loop ticks and pre-arrival samples share one episode latch. Queue
  // depletion is observable even when packets stop; it is not a sound-card xrun.
  void observe() {
    if (!device_.available() || !started_ || !have_pcm_) return;
    const bool empty = device_.empty();
    if (empty && !was_empty_) {
      ++stats_.starved;
      if (prebuffer_ms_ > 0) {
        started_ = false;
        device_.pause(true);
        device_.reset_processing();
        ++stats_.rebuffered;
      }
    }
    was_empty_ = empty;
  }
  void push(const int16_t* pcm, int samples) {
    if (!device_.available() || !pcm || samples <= 0) return;
    const auto bytes = static_cast<uint64_t>(samples) * sizeof(int16_t);
    if (bytes > std::numeric_limits<uint32_t>::max()) return;
    observe();
    const int depth = device_.queued_ms();
    if (depth > hard_limit_ms()) {
      reset();
      ++stats_.trimmed;
    } else if (started_ && depth > soft_limit_ms()) {
      ++stats_.over_soft;
      ++stats_.dropped_drift;
      return;
    }
    const auto result = device_.queue(pcm, static_cast<uint32_t>(bytes));
    if (result == AudioQueueResult::Failed) {
      ++stats_.push_errors;
      return;
    }
    if (result == AudioQueueResult::Buffered) return;
    ++stats_.queued;
    have_pcm_ = true;
    was_empty_ = false;
    if (!started_ && device_.queued_ms() >= prebuffer_ms_) {
      started_ = true;
      device_.pause(false);
    }
    observe();
  }
  bool started() const { return started_; }
  const PlaybackStats& stats() const { return stats_; }

 private:
  AudioQueueDevice& device_;
  int prebuffer_ms_ = 0;
  bool started_ = false;
  bool was_empty_ = false;
  bool have_pcm_ = false;
  PlaybackStats stats_;
};

}  // namespace cue::probe
