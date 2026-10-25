// 重组队列的内存预算（N-10 / FR-PIPE.14「所有队列有硬上限，超限丢帧并计数」）。
//
// **防的是什么**：probe 的下行重组表是 `std::map<uint32_t, FrameAcc>`，键是帧号，
// 每个槽位自带 `parts`（`vector<vector<uint8_t>>`，resize 到 frag_count）与
// `fec_groups`（map）两份容器。原有的两道防线都不管**帧数**：
//   - `kMaxFragPerFrame = 1024` 只约束**单帧**分片数（wire.h）；
//   - `cleanup_stale` 的 2 s TTL 只约束**时间**：2 s 内攒多少帧都算合法。
// 攻击面很具体：下行字节来自浏览器转发（任何页面都能发），只要每帧发**一个**分片、
// 帧号递增，2 s 窗口内就能攒出任意多个槽位，每个槽位都带一份容器开销 → pending 无界。
// 所以还要两道**数量**上限：帧数与总字节。
//
// 本头只放策略（纯标准库、无副作用、可单测）。槽位本体（FrameAcc / map / 时钟）留在
// apps/cue-probe.cpp：这里不该知道帧长什么样，否则就没法在不起管线的情况下单测。
#pragma once

#include <cstddef>

namespace cue::proto {

// 帧数上限 256 的依据：正常链路上 pending 的深度由 TTL 与帧率共同决定——
// 2 s 窗口 × 30 fps ≈ 60 帧（零丢包的稳态远小于它：帧一到齐就 erase，实测 pending
// 停在个位数）。256 = 60 的 4 倍余量：够容忍 4 倍的乱序 / 重传迟滞，同时把最坏情况的
// 槽位数钉成常数。再大就不划算了——每个空槽位本身就有千字节级的容器开销
// （`parts` 即使全空也占 frag_count × sizeof(vector)）。
inline constexpr std::size_t kMaxPendingFrames = 256;

// 字节上限 64 MiB 的依据：单帧最坏 = kMaxFragPerFrame(1024) × kMaxUdpPayload(1200 B)
// ≈ 1.2 MiB，64 MiB ≈ 53 个"最坏帧"；而常规 1080p 帧只有几十~几百 KB，能装上千个。
// 它与帧数上限是**双保险**、谁也替不了谁：
//   - 帧数上限挡住"每帧 1 片"的碎片轰炸（槽位容器开销 >> 载荷，实测这类攻击每帧
//     只有 1 个 1200 B 的分片，却占住一整个槽位）；
//   - 字节上限挡住"少数几帧但每帧喂满 1024 片"的放大（256 × 1.2 MiB ≈ 300 MiB，
//     光有帧数上限会漏掉这一半）。
inline constexpr std::size_t kMaxPendingBytes = std::size_t{64} * 1024 * 1024;

// 预算决策。三个字段的语义：
//   evict      需要从**最旧**开始淘汰的帧数，由帧数上限精确算出（帧数维度是全序的）。
//              字节维度超限时至少为 1——策略不知道每帧多大，只能给这个下界，
//              调用方按最旧优先继续逐帧淘汰，直到 reassembly_over_bytes() 为假。
//   bytes_over 字节维度超限：仅淘汰 evict 帧不一定够（同上）。
//   reject     新来的这一帧**自己**就超过总预算 → 直接拒收。不要为了它把整个队列清空：
//              病态输入（伪造的 frag_count）不该有能力把别人的在途帧一起带走。
struct BudgetDecision {
  std::size_t evict = 0;
  bool bytes_over = false;
  bool reject = false;
};

// 当前占用 + 新来这一帧（或这一片）是否把字节预算顶穿。
// 先比 bytes > max 再算差，避免 bytes 已在预算外时 `max - bytes` 下溢。
[[nodiscard]] inline bool reassembly_over_bytes(std::size_t bytes, std::size_t incoming_bytes,
                                                std::size_t max_bytes = kMaxPendingBytes) {
  return bytes > max_bytes || incoming_bytes > max_bytes - bytes;
}

// 决策入口。调用点只有一个语义：**新建槽位之前**问一次（已有槽位加片不走这里，
// 那条路走 reassembly_over_bytes 的增量守卫）。
//   frames / bytes  当前 pending 的槽位数与已占字节数
//   incoming_bytes  新来的一帧（首片）要占的字节数
[[nodiscard]] inline BudgetDecision plan_reassembly_budget(
    std::size_t frames, std::size_t bytes, std::size_t incoming_bytes,
    std::size_t max_frames = kMaxPendingFrames, std::size_t max_bytes = kMaxPendingBytes) {
  BudgetDecision d;
  if (incoming_bytes > max_bytes) {  // 病态单帧：拒收，不动存量队列
    d.reject = true;
    return d;
  }
  const std::size_t want_frames = frames + 1;  // 这次要新建的那个槽位
  if (want_frames > max_frames) d.evict = want_frames - max_frames;
  d.bytes_over = reassembly_over_bytes(bytes, incoming_bytes, max_bytes);
  if (d.bytes_over && d.evict == 0) d.evict = 1;  // 字节超限至少淘汰一帧（下界，见上）
  return d;
}

// 淘汰顺序**不能**按 frame_id 取最小：frame_id 是 uint32_t、会回绕，回绕点前后的大小
// 关系翻转，按它排序会把刚建的新帧当成最旧帧先淘汰掉（攻击者只要把帧号推到回绕点就能
// 让守卫淘汰"正确的帧"、留下自己的）。所以调用方按 `first_seen_us` 排序——它是单调
// 时钟，与帧号无关，回绕不影响顺序。本函数不参与该排序，只负责"要不要淘汰、几帧"。
}  // namespace cue::proto
