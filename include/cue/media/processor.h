// Sequential, registered media processors. No scheduler or dynamic module loading.
#pragma once
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "cue/media/encoder.h"

namespace cue::media {
enum class MediaKind { Raw, Encoded };

struct ProcessorMedia {
  MediaKind kind = MediaKind::Encoded;
  // Borrowed raw input must live through process(). Owned raw outputs are immutable,
  // reference counted AVFrames and survive the next process(), close(), and chain destruction.
  const AVFrame* borrowed_frame = nullptr;
  std::shared_ptr<const AVFrame> owned_frame;
  std::vector<uint8_t> data;  // encoded results own their bytes
  uint8_t codec = 0;
  int64_t pts = 0;
  bool keyframe = false;
  const AVFrame* frame() const { return owned_frame ? owned_frame.get() : borrowed_frame; }
  static ProcessorMedia raw(const AVFrame* frame, int64_t pts);
  static ProcessorMedia encoded(std::span<const uint8_t> data, uint8_t codec,
                                int64_t pts, bool keyframe);
};

struct ProcessorTiming {
  int64_t elapsed_us = 0;
  int64_t op_us = 0;
  int64_t encode_us = 0;  // x264 input copy + encode, never counted twice in op
};
struct ProcessorError {
  std::string processor;
  size_t index = 0;
  EncodeError code = EncodeError::InvalidFrame;
  std::string message;
  ProcessorTiming timing;  // includes work performed before failure / EncoderDelay
};
class Processor {
 public:
  virtual ~Processor() = default;
  virtual MediaKind input_kind() const = 0;
  virtual MediaKind output_kind() const = 0;
  // Implementations return owning raw results when they create/retain a frame.
  // A raw identity processor may forward the input borrow for this synchronous call.
  // encode_us is accumulated even on failure; the chain measures total/op itself.
  virtual std::expected<ProcessorMedia, ProcessorError> process(ProcessorMedia input,
                                                              ProcessorTiming& timing) = 0;
};
struct ProcessorOptions {
  int width = 0, height = 0;
  int bitrate_kbps = 1500, fps = 30, gop = 60, threads = 1;
  std::string preset = "veryfast";
};
struct ProcessorSpec { std::string name; ProcessorOptions options; };
class ProcessorRegistry {
 public:
  using Factory = std::function<std::expected<std::unique_ptr<Processor>, std::string>(
      const ProcessorOptions&)>;
  // Reject empty name/factory and duplicates; never replace an existing registration.
  std::expected<void, std::string> add(std::string name, Factory factory);
  std::expected<std::unique_ptr<Processor>, std::string> create(
      std::string_view name, const ProcessorOptions& options) const;
  static ProcessorRegistry builtins();
 private:
  std::unordered_map<std::string, Factory> factories_;
};
class ProcessorChain {
 public:
  struct Stats {
    uint64_t processed = 0;  // process attempts, including failures and delays
    uint64_t succeeded = 0, failed = 0, delays = 0;
    ProcessorTiming timing;  // cumulative, including unsuccessful attempts
  };
  // Configure replaces/cleans the previous chain. Empty chains are rejected.
  // Stats are lifetime cumulative; close/reconfigure do not erase evidence.
  std::expected<void, ProcessorError> configure(const ProcessorRegistry& registry,
      const std::vector<ProcessorSpec>& specs, MediaKind input);
  void close();
  std::expected<ProcessorMedia, ProcessorError> process(ProcessorMedia input,
                                                       ProcessorTiming* timing = nullptr);
  const Stats& stats() const { return stats_; }
 private:
  std::vector<std::unique_ptr<Processor>> processors_;
  std::vector<std::string> names_;
  MediaKind input_ = MediaKind::Encoded;
  Stats stats_;
};
}  // namespace cue::media
