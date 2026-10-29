#include "cue/media/audio_receiver.h"

#include <algorithm>
#include <limits>
#include <opus/opus.h>

namespace cue::media {
namespace {
constexpr uint32_t kHalfSpace = 0x80000000u;
int64_t deadline_for(int64_t arrival) {
  return arrival > std::numeric_limits<int64_t>::max() - AudioReceiver::kReorderUs
             ? std::numeric_limits<int64_t>::max() : arrival + AudioReceiver::kReorderUs;
}
}  // namespace

AudioReceiver::Packet AudioReceiver::packet(uint32_t sequence, std::span<const uint8_t> payload,
                                           int64_t now_us) const {
  Packet out{sequence, now_us, {}, {}};
  if (payload.empty()) out.error = "empty opus packet";
  else if (payload.size() > kMaxOpusPacketBytes) out.error = "opus packet too large";
  else {
    unsigned char toc = 0;
    const unsigned char* frames[48]{};
    opus_int16 sizes[48]{};
    int offset = 0;
    const auto length = static_cast<opus_int32>(payload.size());
    if (opus_packet_parse(payload.data(), length, &toc, frames, sizes, &offset) < 0 ||
        opus_packet_get_nb_samples(payload.data(), length, kAudioSampleRate) != kAudioFrameSamples) {
      out.error = "invalid opus packet or duration (expected 20ms)";
    } else out.payload.assign(payload.begin(), payload.end());
  }
  return out;
}

void AudioReceiver::error(const std::string& reason) {
  ++stats_.decode_errors;
  if (first_error_.empty()) first_error_ = reason.substr(0, 400);
}

bool AudioReceiver::open_decoder() {
  if (decoder_.is_open()) return true;
  if (open_failed_) return false;
  std::string reason;
  if (decoder_.open(reason)) return true;
  open_failed_ = true;
  if (first_error_.empty()) first_error_ = reason.substr(0, 400);
  return false;
}

void AudioReceiver::reset_decoder() {
  decoder_.close();
  open_failed_ = false;
  ++stats_.resyncs;
}

void AudioReceiver::reset_stream() {
  have_ = false;
  expected_ = highest_received_ = 0;
  received_history_ = 0;
  pending_.clear();
  decoder_.close();
  open_failed_ = false;
  first_error_.clear();
}

std::size_t AudioReceiver::pending_bytes() const {
  std::size_t bytes = 0;
  for (const auto& p : pending_) bytes += p.payload.size();
  return bytes;
}

int64_t AudioReceiver::next_deadline_us() const {
  if (pending_.empty()) return 0;
  int64_t deadline = std::numeric_limits<int64_t>::max();
  for (const auto& p : pending_) deadline = std::min(deadline, deadline_for(p.arrival_us));
  return deadline;
}

std::size_t AudioReceiver::nearest() const {
  std::size_t best = 0;
  for (std::size_t i = 1; i < pending_.size(); ++i) {
    if (pending_[i].sequence - expected_ < pending_[best].sequence - expected_) best = i;
  }
  return best;
}

bool AudioReceiver::old(uint32_t sequence) {
  const uint32_t ahead = sequence - expected_;
  if (ahead < kHalfSpace) return false;
  const uint32_t behind = (expected_ - 1u) - sequence;
  if (ahead != kHalfSpace && behind < 64 && ((received_history_ >> behind) & 1u))
    ++stats_.duplicates;
  else ++stats_.stale;
  return true;
}

void AudioReceiver::consume_slots(uint32_t count) {
  received_history_ = count >= 64 ? 0 : received_history_ << count;
  expected_ += count;
  stats_.committed += count;
}

void AudioReceiver::emit_plc(uint32_t sequence, const Emit& emit) {
  std::string reason;
  if (open_decoder()) {
    auto pcm = decoder_.conceal(reason);
    if (pcm) {
      ++stats_.plc_frames;
      emit(sequence, AudioOutputKind::Plc, pcm->samples);
      return;
    }
  }
  ++stats_.unconcealed;
  reset_decoder();
}

void AudioReceiver::settle_gap(uint32_t count, const Packet& successor, const Emit& emit) {
  if (!count) return;
  stats_.gaps += count;
  if (count > 4 || !open_decoder()) {
    stats_.unconcealed += count;
    consume_slots(count);
    reset_decoder();
    return;
  }
  // Keep the last missing slot adjacent to this successor: only it can use its LBRR.
  const uint32_t earlier = count - 1;
  uint32_t plc_attempts = 0;
  if (!plc_) {
    if (earlier) {
      stats_.unconcealed += earlier;
      consume_slots(earlier);
      reset_decoder();
    }
  } else {
    for (uint32_t i = 0; i < earlier; ++i) {
      emit_plc(expected_, emit);
      ++plc_attempts;
      consume_slots(1);
    }
  }
  bool recovered = false;
  const bool try_fec = fec_ && successor.error.empty();
  bool fallback_plc = !try_fec;
  if (try_fec && open_decoder()) {
    std::string reason;
    auto pcm = decoder_.decode(successor.payload, reason, DecodeMode::FecRecover);
    if (pcm) {
      ++stats_.fec_recovered;
      emit(expected_, AudioOutputKind::Fec, pcm->samples);
      recovered = true;
    } else fallback_plc = pcm.error() == AudioDecodeError::FecUnavailable;
  }
  // FEC-unavailable fallback shares the same three-PLC budget as older missing slots.
  if (!recovered) {
    if (fallback_plc && plc_ && plc_attempts < 3) emit_plc(expected_, emit);
    else { ++stats_.unconcealed; reset_decoder(); }
  }
  consume_slots(1);
}

void AudioReceiver::commit(Packet&& p, const Emit& emit) {
  settle_gap(p.sequence - expected_, p, emit);
  bool decoded = false;
  if (!p.error.empty()) { error(p.error); reset_decoder(); }
  else if (!open_decoder()) error("opus decoder unavailable");
  else {
    std::string reason;
    auto pcm = decoder_.decode(p.payload, reason);
    if (!pcm || pcm->sample_count() != kAudioFrameSamples) {
      error(pcm ? "unexpected decoded opus duration" : reason);
      reset_decoder();
    } else {
      ++stats_.decoded_frames;
      stats_.pcm_samples += pcm->sample_count();
      emit(p.sequence, AudioOutputKind::Normal, pcm->samples);
      decoded = true;
    }
  }
  if (!decoded) ++stats_.failed_frames;
  consume_slots(1);
  received_history_ |= 1;
}

void AudioReceiver::release_nearest(const Emit& emit) {
  const auto index = nearest();
  Packet p = std::move(pending_[index]);
  pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(index));
  commit(std::move(p), emit);
}

void AudioReceiver::advance(int64_t now_us, const Emit& emit) {
  while (!pending_.empty()) {
    if (pending_[nearest()].sequence == expected_) { release_nearest(emit); continue; }
    if (now_us < next_deadline_us()) return;
    ++stats_.deadline_flushes;
    release_nearest(emit);
  }
}

void AudioReceiver::insert(uint32_t sequence, std::span<const uint8_t> payload, int64_t now_us,
                           const Emit& emit) {
  advance(now_us, emit);
  if (!have_) {
    auto first = packet(sequence, payload, now_us);
    if (!first.error.empty()) { error(first.error); return; }
    have_ = true;
    expected_ = highest_received_ = sequence;
    commit(std::move(first), emit);
    return;
  }
  if (old(sequence)) return;
  for (const auto& p : pending_) {
    if (p.sequence == sequence) { ++stats_.duplicates; return; }
  }
  const uint32_t high_delta = sequence - highest_received_;
  if (high_delta && high_delta < kHalfSpace) highest_received_ = sequence;
  else if (high_delta) ++stats_.reordered;
  auto p = packet(sequence, payload, now_us);
  if (sequence == expected_) { commit(std::move(p), emit); advance(now_us, emit); return; }
  if (pending_.size() == kMaxPending) {
    ++stats_.capacity_flushes;
    if (sequence - expected_ < pending_[nearest()].sequence - expected_) {
      commit(std::move(p), emit);
      advance(now_us, emit);
      return;
    }
    release_nearest(emit);
    advance(now_us, emit);
  }
  pending_.push_back(std::move(p));
  advance(now_us, emit);
}

}  // namespace cue::media
