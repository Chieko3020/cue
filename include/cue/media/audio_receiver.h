// Bounded Opus receive timeline. Callers supply one monotonic clock and one PCM sink.
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "cue/media/audio_decoder.h"

namespace cue::media {

enum class AudioOutputKind { Normal, Fec, Plc };

struct AudioReceiveStats {
  uint64_t decoded_frames = 0;
  uint64_t pcm_samples = 0;  // Normal only; recovered/extrapolated frames have separate counters.
  uint64_t decode_errors = 0;
  uint64_t failed_frames = 0; // Committed current slots that failed; excludes unanchored rejects.
  uint64_t gaps = 0;
  uint64_t duplicates = 0;
  uint64_t reordered = 0;   // Accepted before commit, behind the highest received sequence.
  uint64_t stale = 0;       // Already committed (or ambiguous half-space), never replayed.
  uint64_t fec_recovered = 0;
  uint64_t plc_frames = 0;
  uint64_t unconcealed = 0;
  uint64_t committed = 0;   // Normal + failed current slots + all finalized missing slots.
  uint64_t resyncs = 0;
  uint64_t deadline_flushes = 0;
  uint64_t capacity_flushes = 0;
};

class AudioReceiver {
 public:
  static constexpr int64_t kReorderUs = 60'000;
  static constexpr std::size_t kMaxPending = 8;
  using Emit = std::function<void(uint32_t, AudioOutputKind, std::span<const int16_t>)>;

  explicit AudioReceiver(bool fec = true, bool plc = true) : fec_(fec), plc_(plc) {}
  // Expired slots are committed before considering a packet arriving exactly at the deadline.
  void insert(uint32_t sequence, std::span<const uint8_t> payload, int64_t now_us, const Emit& emit);
  void advance(int64_t now_us, const Emit& emit);
  int64_t next_deadline_us() const;  // 0 = no queued successor, so no evidence to invent a gap.
  void reset_stream();             // Clears stream state; process counters remain cumulative.
  const AudioReceiveStats& stats() const { return stats_; }
  const std::string& first_error() const { return first_error_; }
  std::size_t pending_packets() const { return pending_.size(); }
  std::size_t pending_bytes() const;

 private:
  struct Packet {
    uint32_t sequence = 0;
    int64_t arrival_us = 0;
    std::vector<uint8_t> payload;
    std::string error;
  };
  Packet packet(uint32_t sequence, std::span<const uint8_t> payload, int64_t now_us) const;
  std::size_t nearest() const;
  bool old(uint32_t sequence);
  bool open_decoder();
  void reset_decoder();
  void consume_slots(uint32_t count);
  void emit_plc(uint32_t sequence, const Emit& emit);
  void settle_gap(uint32_t count, const Packet& successor, const Emit& emit);
  void commit(Packet&& packet, const Emit& emit);
  void release_nearest(const Emit& emit);
  void error(const std::string& reason);

  bool fec_;
  bool plc_;
  bool have_ = false;
  uint32_t expected_ = 0;
  uint32_t highest_received_ = 0;
  uint64_t received_history_ = 0; // Actual received slots among the last 64 committed slots.
  bool open_failed_ = false;
  OpusAudioDecoder decoder_;
  std::vector<Packet> pending_;
  AudioReceiveStats stats_;
  std::string first_error_;
};

}  // namespace cue::media
