// Exercise the actual pending-table owner, including shared concurrent reservations.
#include <atomic>
#include <barrier>
#include <cstdio>
#include <limits>
#include <thread>

#include "cue/proto/upstream_reassembly.h"

namespace {
int failures = 0;
void check(bool ok, const char* what) {
  if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}
using namespace cue::proto;
MediaHeader header(uint32_t id, uint32_t index = 0, uint32_t count = 2,
                   uint32_t size = 1) {
  MediaHeader h;
  h.frame_id = id;
  h.frag_index = index;
  h.frag_count = count;
  h.payload_len = size;
  h.pts_us = 100;
  h.codec = 1;
  return h;
}
const uint8_t one[] = {7};
const uint8_t two[] = {8, 9};
auto push(UpstreamReassembly& owner, MediaHeader h, int64_t now = 0) {
  return owner.push(h, one, now);
}
}

int main() {
  const auto slot = UpstreamReassembly::slot_charge(2);
  // Session frame and byte bounds are independent; exact limits remain usable.
  {
    UpstreamProcessBudget global({10, 100000});
    UpstreamReassembly owner(global, {2, 100000});
    check(bool(push(owner, header(1))), "first slot accepted");
    check(bool(push(owner, header(2))), "exact two slots accepted");
    auto denied = push(owner, header(3));
    check(!denied && denied.error() == UpstreamReject::SessionBudget, "third slot refused");
    check(owner.usage().frames == 2 && global.usage().frames == 2,
          "no allocation/reservation on refused new slot");
    check(owner.usage().budget_rejected == 1 && global.usage().budget_rejected == 1,
          "local/global refused-packet counters agree");
    owner.clear();
    check(global.usage().bytes == 0 && global.usage().frames == 0, "clear returns all credits");
    check(bool(push(owner, header(3))), "owner usable after clear");
  }
  {
    UpstreamProcessBudget global;
    UpstreamReassembly exact(global, {3, slot + 1});
    check(bool(push(exact, header(1))), "container + one byte exact limit accepted");
    check(exact.usage().bytes == slot + 1, "container charge included in live bytes");
    auto denied = push(exact, header(1, 1));
    check(!denied && denied.error() == UpstreamReject::SessionBudget,
          "existing slot cannot grow one byte beyond session byte cap");
    check(exact.usage().frames == 1 && exact.usage().bytes == slot + 1,
          "incremental refusal preserves old slot and payload");
    // A duplicate is neither charged nor replaced, even with a larger payload.
    auto dup = exact.push(header(1, 0, 2, 2), two, 1'900'000);
    check(dup && !*dup && exact.usage().bytes == slot + 1,
          "first-valid-wins duplicate consumes no credit");
    check(exact.expire(2'000'000) == 0, "TTL exact boundary is retained");
    check(exact.expire(2'000'001) == 1, "duplicate does not refresh first-seen TTL");
    check(global.usage().bytes == 0, "expiry returns global payload and metadata credit");
  }
  {
    UpstreamProcessBudget global;
    UpstreamReassembly too_small(global, {3, slot});
    check(!push(too_small, header(1)), "metadata alone leaves no room for payload");
    check(global.usage().frames == 0 && global.usage().bytes == 0,
          "metadata failure occurs before process reservation");
  }
  // Completed payload order, flags and original timing survive out-of-order input.
  {
    UpstreamProcessBudget global;
    UpstreamReassembly owner(global, {3, slot + 3});
    auto tail = header(5, 1, 2, 2);
    tail.flags = kUpFlagKeyframe;
    check(bool(owner.push(tail, two, 10)), "out-of-order tail accepted");
    auto dup = owner.push(tail, two, 11);
    check(dup && !*dup, "duplicate tail stays pending");
    auto complete = push(owner, header(5), 12);
    check(complete && *complete, "two unique fragments complete");
    if (complete && *complete) {
      check((*complete)->payload == std::vector<uint8_t>({7, 8, 9}), "ordered bytes preserved");
      check((*complete)->first_seen_us == 10 && (*complete)->keyframe &&
                (*complete)->pts_us == 100 && (*complete)->frag_count == 2,
            "original metadata and keyframe flag preserved");
    }
    check(owner.usage().frames == 0 && global.usage().bytes == 0,
          "completion returns exact reservation");
  }
  // Conflicts and malformed fragments leave both the frame and its credit intact.
  {
    UpstreamProcessBudget global;
    UpstreamReassembly owner(global);
    check(bool(push(owner, header(9))), "seed conflict case");
    auto conflict = push(owner, header(9, 1, 3));
    check(!conflict && conflict.error() == UpstreamReject::MetadataConflict, "count conflict refused");
    auto changed = header(9, 1); ++changed.pts_us;
    check(!push(owner, changed), "PTS conflict refused");
    changed = header(9, 1); ++changed.codec;
    check(!push(owner, changed), "codec conflict refused");
    check(!push(owner, header(10, 0, 0)), "zero count refused");
    check(!push(owner, header(10, 0, kMaxFragPerFrame + 1)), "oversized count refused");
    check(!push(owner, header(9, 2, 2)), "bad index refused");
    check(!owner.push(header(10, 0, 2, 0), {}, 0), "empty fragment refused");
    check(!owner.push(header(10, 0, 2, 2), one, 0), "declared length mismatch refused");
    check(owner.usage().frames == 1 && owner.usage().bytes == slot + 1 &&
              global.usage().budget_rejected == 0,
          "malformed input does not allocate or count as budget pressure");
    check(bool(push(owner, header(9, 1))) && owner.usage().frames == 0,
          "original slot still completes after conflicts");
  }
  // Global byte enforcement includes growth of existing entries and releases on scope exit.
  {
    UpstreamProcessBudget global({10, slot + 1});
    {
      UpstreamReassembly a(global), b(global);
      check(bool(push(a, header(1))), "exact global byte limit accepted");
      auto denied = push(a, header(1, 1));
      check(!denied && denied.error() == UpstreamReject::ProcessBudget,
            "global byte limit prevents existing slot growth");
      check(!push(b, header(2)), "another session cannot bypass shared byte cap");
      check(global.usage().budget_rejected == 2 && b.usage().frames == 0,
            "global rejection counted once and no empty slot left");
    }
    check(global.usage().bytes == 0 && global.usage().frames == 0,
          "disconnect/destructor returns global reservation");
    UpstreamReassembly next(global);
    check(bool(push(next, header(3))), "next session reuses returned credits");
  }
  // Frame IDs wrap; expiry depends on age and releases each old slot exactly once.
  {
    UpstreamProcessBudget global;
    UpstreamReassembly owner(global);
    check(bool(push(owner, header(std::numeric_limits<uint32_t>::max()), 0)), "pre-wrap slot");
    check(bool(push(owner, header(0), 1'000'000)), "post-wrap slot");
    check(owner.expire(2'000'001) == 1 && owner.usage().frames == 1,
          "age selects pre-wrap slot rather than numeric frame id");
    check(bool(push(owner, header(0, 1), 2'000'002)) && owner.usage().frames == 0,
          "post-wrap slot remains valid and completes");
  }
  // Shared check/reserve is atomic. Hold every successful entry through a barrier
  // so thread scheduling cannot disguise exceeding the five-slot global limit.
  {
    UpstreamProcessBudget global({5, 1000000});
    std::barrier ready(9);
    std::barrier release(9);
    std::atomic<int> granted{0};
    std::vector<std::jthread> threads;
    for (int t = 0; t < 8; ++t) {
      threads.emplace_back([&] {
        UpstreamReassembly owner(global, {4, 100000});
        for (uint32_t id = 0; id < 4; ++id) {
          if (push(owner, header(id))) granted.fetch_add(1);
        }
        ready.arrive_and_wait();
        release.arrive_and_wait();
      });
    }
    ready.arrive_and_wait();
    check(granted.load() == 5 && global.usage().frames == 5,
          "eight concurrent owners cannot exceed global frame cap");
    check(global.usage().bytes == 5 * (slot + 1) && global.usage().budget_rejected == 27,
          "shared concurrent charge and refused-packet ledger exact");
    release.arrive_and_wait();
    threads.clear();
    check(global.usage().frames == 0 && global.usage().bytes == 0,
          "all concurrent session destructors refund safely");
  }
  std::printf("upstream_reassembly: %d failures\n", failures);
  return failures ? 1 : 0;
}
