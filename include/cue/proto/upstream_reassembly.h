// Own the cued upstream video pending table and reserve its storage before allocation.
// Session methods are thread-confined; only the process budget is shared. Charges
// cover payload + exact fragment-array storage + a conservative map-node allowance,
// not allocator arenas, completed frames, decoder queues or total process RSS.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "cue/proto/wire.h"

namespace cue::proto {

struct UpstreamLimits {
  std::size_t frames = 256;
  std::size_t bytes = std::size_t{64} * 1024 * 1024;
};
inline constexpr UpstreamLimits kUpstreamProcessLimits{1024, std::size_t{128} * 1024 * 1024};

struct UpstreamUsage {
  std::size_t frames = 0;
  std::size_t bytes = 0;
  uint64_t budget_rejected = 0;  // packets refused, not settled frames
};

enum class UpstreamReject { InvalidFragment, MetadataConflict, SessionBudget, ProcessBudget,
                            AllocationFailure };

class UpstreamProcessBudget {
 public:
  explicit UpstreamProcessBudget(UpstreamLimits limits = kUpstreamProcessLimits) : limits_(limits) {}
  UpstreamProcessBudget(const UpstreamProcessBudget&) = delete;
  UpstreamProcessBudget& operator=(const UpstreamProcessBudget&) = delete;

  // One lock makes check + reservation indivisible across session threads.
  [[nodiscard]] bool reserve(std::size_t frames, std::size_t bytes) {
    std::lock_guard lock(mu_);
    if (usage_.frames > limits_.frames || frames > limits_.frames - usage_.frames ||
        usage_.bytes > limits_.bytes || bytes > limits_.bytes - usage_.bytes) {
      ++usage_.budget_rejected;
      return false;
    }
    usage_.frames += frames;
    usage_.bytes += bytes;
    return true;
  }
  void release(std::size_t frames, std::size_t bytes) {
    std::lock_guard lock(mu_);
    usage_.frames -= frames;
    usage_.bytes -= bytes;
  }
  void session_rejected() {
    std::lock_guard lock(mu_);
    ++usage_.budget_rejected;
  }
  [[nodiscard]] UpstreamUsage usage() const {
    std::lock_guard lock(mu_);
    return usage_;
  }
  [[nodiscard]] UpstreamLimits limits() const { return limits_; }

 private:
  const UpstreamLimits limits_;
  mutable std::mutex mu_;
  UpstreamUsage usage_;
};

struct UpstreamFrame {
  uint32_t frame_id = 0;
  uint32_t frag_count = 0;
  uint64_t pts_us = 0;
  uint8_t codec = 0;
  bool keyframe = false;
  int64_t first_seen_us = 0;
  std::vector<uint8_t> payload;
};

class UpstreamReassembly {
  struct Part {
    std::unique_ptr<uint8_t[]> data;
    std::size_t size = 0;
  };
  struct Entry {
    uint32_t count = 0;
    uint32_t received = 0;
    uint64_t pts_us = 0;
    uint8_t codec = 0;
    bool keyframe = false;
    int64_t first_seen_us = 0;
    std::size_t charge = 0;
    std::size_t payload_bytes = 0;
    std::unique_ptr<Part[]> parts;
  };
  using Table = std::map<uint32_t, Entry>;

 public:
  // The referenced process budget must outlive every session owner.
  explicit UpstreamReassembly(UpstreamProcessBudget& process, UpstreamLimits limits = {})
      : process_(process), limits_(limits) {}
  ~UpstreamReassembly() { clear(); }
  UpstreamReassembly(const UpstreamReassembly&) = delete;
  UpstreamReassembly& operator=(const UpstreamReassembly&) = delete;

  // Four pointer-sized words conservatively cover the ordered-map tree links,
  // colour and padding in addition to its actual value. Two size_t words allow
  // for an array allocation cookie. Allocator overhead is not an RSS measure.
  // No payload is hidden in
  // vector capacity: fragment arrays and payloads use exact-sized unique_ptr[].
  [[nodiscard]] static constexpr std::size_t slot_charge(uint32_t count) {
    return sizeof(Table::value_type) + 4 * sizeof(void*) + 2 * sizeof(std::size_t) +
           std::size_t{count} * sizeof(Part);
  }
  [[nodiscard]] UpstreamUsage usage() const { return usage_; }
  [[nodiscard]] UpstreamLimits limits() const { return limits_; }

  [[nodiscard]] std::expected<std::optional<UpstreamFrame>, UpstreamReject> push(
      const MediaHeader& h, std::span<const uint8_t> payload, int64_t now_us) {
    if (h.frag_count == 0 || h.frag_count > kMaxFragPerFrame ||
        h.frag_index >= h.frag_count || payload.empty() || payload.size() > kMaxWsFrag ||
        payload.size() != h.payload_len) {
      return std::unexpected(UpstreamReject::InvalidFragment);
    }
    auto it = pending_.find(h.frame_id);
    const bool fresh = it == pending_.end();
    if (!fresh) {
      const auto& e = it->second;
      if (h.frag_count != e.count || h.pts_us != e.pts_us || h.codec != e.codec) {
        return std::unexpected(UpstreamReject::MetadataConflict);
      }
      // First valid fragment wins, including its flags. Repeats neither allocate
      // nor extend the TTL and cannot replace data with a larger payload.
      if (e.parts[h.frag_index].data) return std::optional<UpstreamFrame>{};
    }
    const std::size_t frames = fresh ? 1 : 0;
    const std::size_t bytes = (fresh ? slot_charge(h.frag_count) : 0) + payload.size();
    if (usage_.frames > limits_.frames || frames > limits_.frames - usage_.frames ||
        usage_.bytes > limits_.bytes || bytes > limits_.bytes - usage_.bytes) {
      ++usage_.budget_rejected;
      process_.session_rejected();
      return std::unexpected(UpstreamReject::SessionBudget);
    }
    if (!process_.reserve(frames, bytes)) {
      ++usage_.budget_rejected;
      return std::unexpected(UpstreamReject::ProcessBudget);
    }
    // Allocation failure must return the reservation, including map insertion.
    try {
      auto data = std::make_unique<uint8_t[]>(payload.size());
      std::copy(payload.begin(), payload.end(), data.get());
      if (fresh) {
        Entry e;
        e.count = h.frag_count;
        e.pts_us = h.pts_us;
        e.codec = h.codec;
        e.keyframe = (h.flags & kUpFlagKeyframe) != 0;
        e.first_seen_us = now_us;
        e.parts = std::make_unique<Part[]>(e.count);
        it = pending_.emplace(h.frame_id, std::move(e)).first;
      }
      auto& e = it->second;
      e.parts[h.frag_index] = Part{std::move(data), payload.size()};
      ++e.received;
      e.payload_bytes += payload.size();
      e.charge += bytes;
      e.keyframe = e.keyframe || (h.flags & kUpFlagKeyframe) != 0;
      usage_.frames += frames;
      usage_.bytes += bytes;
    } catch (const std::bad_alloc&) {
      process_.release(frames, bytes);
      return std::unexpected(UpstreamReject::AllocationFailure);
    }
    const auto& e = it->second;
    if (e.received != e.count) return std::optional<UpstreamFrame>{};
    try {
      UpstreamFrame frame{h.frame_id, e.count, e.pts_us, e.codec, e.keyframe,
                          e.first_seen_us, {}};
      frame.payload.reserve(e.payload_bytes);
      for (uint32_t i = 0; i < e.count; ++i) {
        const auto& part = e.parts[i];
        frame.payload.insert(frame.payload.end(), part.data.get(), part.data.get() + part.size);
      }
      erase(it);
      return std::optional<UpstreamFrame>{std::move(frame)};
    } catch (const std::bad_alloc&) {
      erase(it);  // no stranded complete frame on failed output allocation
      return std::unexpected(UpstreamReject::AllocationFailure);
    }
  }

  // Preserve cued's original strict >2s TTL. Age, not frame_id, controls expiry.
  std::size_t expire(int64_t now_us) {
    std::size_t expired = 0;
    for (auto it = pending_.begin(); it != pending_.end();) {
      if (now_us > it->second.first_seen_us && now_us - it->second.first_seen_us > 2'000'000) {
        it = erase(it);
        ++expired;
      } else {
        ++it;
      }
    }
    return expired;
  }
  void clear() {
    pending_.clear();
    process_.release(usage_.frames, usage_.bytes);
    usage_.frames = 0;
    usage_.bytes = 0;
  }

 private:
  Table::iterator erase(Table::iterator it) {
    const auto bytes = it->second.charge;
    auto next = pending_.erase(it);
    --usage_.frames;
    usage_.bytes -= bytes;
    process_.release(1, bytes);
    return next;
  }
  UpstreamProcessBudget& process_;
  const UpstreamLimits limits_;
  UpstreamUsage usage_;
  Table pending_;
};

}  // namespace cue::proto
