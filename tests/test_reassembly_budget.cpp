// 重组表预算策略的单测（N-10 / FR-PIPE.14「所有队列有硬上限，超限丢帧并计数」）。
//
// 被测算的是 include/cue/proto/reassembly_budget.h：纯决策，不碰 map、不碰时钟，
// 所以这里毫秒级跑完、不需要起管线。
//
// 为什么值得单测：这两道上限是**安全护栏**，正常链路根本走不到（实测 pending 长期在
// 个位数、占用远不到 1 MiB），于是"淘汰了几帧"这条路径靠真跑管线测不到——只有直接喂
// 边界值才能钉死它。同时"单个病态帧不该清空整个队列"这条很容易在实现时写错成
// "先清空再判断"，也钉在这里。
//
// 五组锁定的是：
//   1) 帧数上限：填到 kMaxPendingFrames 恰好不淘汰，第 +1 帧淘汰 1 帧；
//   2) 字节上限：恰好等于 kMaxPendingBytes 不淘汰，多 1 字节就要求淘汰（至少 1 帧）；
//   3) 病态单帧：新帧自身 > 总预算 → reject，且**不要求**淘汰存量队列（evict == 0）；
//   4) 一次淘汰多帧：当前 300 帧、上限 256、新来 1 → 淘汰 45 帧（不是逐帧淘汰）；
//   5) 边界：0 帧 0 字节、恰好等于上限的任一侧、小上限下的双超限叠加。
#include <cstddef>
#include <cstdio>

#include "cue/proto/reassembly_budget.h"

namespace {

int failures = 0;

void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

using cue::proto::kMaxPendingBytes;
using cue::proto::kMaxPendingFrames;
using cue::proto::plan_reassembly_budget;
using cue::proto::reassembly_over_bytes;

}  // namespace

int main() {
  // 1) 帧数上限。语义是「这次要新建的槽位算进去之后」不越界：
  //    当前 255 帧 + 新来 1 帧 = 恰好 256，不淘汰；当前 256 帧 + 新来 1 帧 = 257，淘汰 1 帧。
  {
    const auto d = plan_reassembly_budget(kMaxPendingFrames - 1, 0, 1000);
    check(d.evict == 0 && !d.bytes_over && !d.reject,
          "1a 填到恰好 kMaxPendingFrames 帧 → 不淘汰");
  }
  {
    const auto d = plan_reassembly_budget(kMaxPendingFrames, 0, 1000);
    check(d.evict == 1 && !d.bytes_over && !d.reject, "1b 第 +1 帧 → 淘汰 1 帧");
  }

  // 2) 字节上限。入参与帧数案例同样理解：bytes + incoming 是插入后的总量。
  {
    const auto d = plan_reassembly_budget(0, kMaxPendingBytes - 100, 100);
    check(d.evict == 0 && !d.bytes_over, "2a 恰好等于 kMaxPendingBytes → 不淘汰");
  }
  {
    const auto d = plan_reassembly_budget(0, kMaxPendingBytes - 99, 100);
    check(d.bytes_over && d.evict >= 1, "2b 多 1 字节 → 要求淘汰（至少 1 帧）");
    // 策略给的是"至少 1 帧"的下界（它不知道每帧多大），所以调用方还得继续逐帧淘汰；
    // 这里同时锁定"下界确实不为 0"——为 0 的话调用方就会停在超限状态上。
    check(reassembly_over_bytes(kMaxPendingBytes - 99, 100), "2c 超限判据本身为真");
  }

  // 3) 病态单帧：伪造 frag_count / 单个超长分片让"这一帧自己"就超过总预算。
  //    答案是拒收它，而不是为了它把别人的在途帧全清空——否则一个坏包能让整队列陪葬。
  {
    const auto d = plan_reassembly_budget(0, 0, kMaxPendingBytes + 1);
    check(d.reject && d.evict == 0 && !d.bytes_over,
          "3a 单个新帧超总预算 → reject 且不淘汰任何存量帧");
  }
  {
    // 队列本来就装了 200 帧、40 MiB：病态帧不该把这份存量一起带走。
    const auto d = plan_reassembly_budget(200, std::size_t{40} * 1024 * 1024, kMaxPendingBytes + 1);
    check(d.reject && d.evict == 0, "3b 有存量时同样只拒收、不淘汰");
  }

  // 4) 一次算清该淘汰多少帧（不是每次只淘汰一帧）：300 帧 + 新来 1 → 301-256 = 45。
  {
    const auto d = plan_reassembly_budget(300, 0, 100);
    check(d.evict == 45 && !d.reject, "4a 300 帧 → 一次淘汰 45 帧");
  }
  {
    // 一条覆盖 0..400 的性质检查：淘汰之后必须恰好装下，且不会多淘汰。
    bool ok = true;
    for (std::size_t frames = 0; frames <= 400; ++frames) {
      const auto d = plan_reassembly_budget(frames, 0, 64);
      if (d.reject) {
        ok = false;
        break;
      }
      const std::size_t after = frames + 1 - d.evict;
      if (after > kMaxPendingFrames) ok = false;                     // 淘汰不够
      if (d.evict > 0 && after != kMaxPendingFrames) ok = false;      // 淘汰过头
      if (d.evict > frames + 1) ok = false;                          // 不可能淘汰比自己多的
    }
    check(ok, "4b 0..400 帧：淘汰后恰好回到上限，且不多淘汰");
  }

  // 5) 边界与两侧。
  {
    const auto d = plan_reassembly_budget(0, 0, 0);
    check(d.evict == 0 && !d.bytes_over && !d.reject, "5a 0 帧 0 字节 0 新字节 → 什么都不做");
  }
  {
    // 小上限下手算，两个维度同时超限：帧数 3+1>3 → 淘汰 1；字节 10+5>10 → bytes_over。
    const auto d = plan_reassembly_budget(3, 10, 5, /*max_frames=*/3, /*max_bytes=*/10);
    check(d.evict == 1 && d.bytes_over && !d.reject, "5b 双超限：淘汰数取帧数维度的硬下界");
  }
  {
    // 字节恰好等于上限的另一侧（等于 = 不超）。
    const auto d = plan_reassembly_budget(3, 5, 5, /*max_frames=*/3, /*max_bytes=*/10);
    check(d.evict == 1 && !d.bytes_over, "5c 字节恰好等于上限 → 不算超限");
  }
  {
    // 超限判据本身：等于不超、多 1 就超；入参已越界时也不能下溢成"没超"。
    check(!reassembly_over_bytes(kMaxPendingBytes, 0), "5d 恰好等于上限 → 不超");
    check(reassembly_over_bytes(kMaxPendingBytes, 1), "5e 上限 +1 字节 → 超");
    check(!reassembly_over_bytes(kMaxPendingBytes - 1, 1), "5f 上限 -1 +1 → 不超");
    check(reassembly_over_bytes(kMaxPendingBytes + 5, 0), "5g 已越界的入参 → 仍判超（不下溢）");
  }

  if (failures == 0) std::printf("test_reassembly_budget: 全部通过\n");
  return failures == 0 ? 0 : 1;
}
