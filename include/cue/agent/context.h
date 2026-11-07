// 上下文预算：估算 + 裁剪
// Drop eligible complete rounds; preserve system, goal, recent and explicitly protected evidence/control.
#pragma once

#include <cstddef>
#include <cmath>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "cue/llm/client.h"

namespace cue::agent {

struct ContextBudget {
  std::size_t max_tokens = 24000;
  // 至少保留最近这么多条非 system 消息（含当前阶段提示）
  std::size_t keep_recent = 6;
  // tools schema 每轮都要发、但不随历史增长，必须单独计入（审查发现漏算会系统性低估）
  std::size_t reserve_tokens = 0;
  // 在线校准系数 k：上一轮 usage.prompt_tokens / 本次估算，SPEC §7.9 的三层估算
  double k = 1.0;
  // Separate fixed schema and output reserves; neither is multiplied by k.
  std::size_t schema_tokens = 0;
  int output_tokens = 0;
  // Execution-only policy, reconstructed from tool names on each request/resume.
  // A matching call or result protects the entire contiguous tool round.
  std::function<bool(const std::string&)> protect_tool;
  // 单条消息的硬预算（token）：裁不动时对最旧的 tool 消息做头部保留 + 截断标记
  std::size_t single_msg_tokens = 4000;
};

// 中英混合的保守估算：约 3 字节/token（报告里必须标注"估算"）
inline std::size_t token_add(std::size_t a, std::size_t b) {
  const auto max = std::numeric_limits<std::size_t>::max();
  return b > max - a ? max : a + b;
}
inline std::size_t estimate_tokens(const std::string& s) {
  return s.size() / 3 + (s.size() % 3 != 0);
}

inline std::size_t estimate_tokens(const llm::ChatMessage& m) {
  std::size_t n = token_add(token_add(estimate_tokens(m.content), estimate_tokens(m.name)), 4);
  for (const auto& tc : m.tool_calls) n = token_add(n, token_add(token_add(estimate_tokens(tc.name), estimate_tokens(tc.arguments)), 8));
  return n;
}

inline std::size_t estimate_tokens(const std::vector<llm::ChatMessage>& msgs) {
  std::size_t n = 0;
  for (const auto& m : msgs) n = token_add(n, estimate_tokens(m));
  return n;
}

// tools schema 的估算（JSON 原文 / 3）
inline std::size_t estimate_tokens(const nlohmann::json& j) {
  return j.is_null() ? 0 : estimate_tokens(j.dump());
}

// SPEC §7.9 三层估算：bytes/3.0 × k × 1.2 + reserve。宁多算不少算（报告标注"估算"）。
struct ContextEstimate {
  std::size_t tokens = 0;
  bool invalid = false;
};
inline ContextEstimate context_estimate(const std::vector<llm::ChatMessage>& msgs, const ContextBudget& b) {
  const auto max = std::numeric_limits<std::size_t>::max();
  if (!std::isfinite(b.k) || b.output_tokens < 0) return {max, true};
  const double k = b.k > 0.0 ? b.k : 1.0;
  const long double scaled = static_cast<long double>(estimate_tokens(msgs)) * k * 1.2L;
  if (!std::isfinite(scaled) || scaled >= static_cast<long double>(max)) return {max, true};
  std::size_t total = static_cast<std::size_t>(scaled);
  for (const auto reserve : {b.reserve_tokens, b.schema_tokens, static_cast<std::size_t>(b.output_tokens)}) {
    if (reserve > max - total) return {max, true};
    total += reserve;
  }
  return {total, false};
}
inline std::size_t effective_tokens(const std::vector<llm::ChatMessage>& msgs, const ContextBudget& b) {
  return context_estimate(msgs, b).tokens;
}

struct TrimResult {
  std::size_t dropped = 0;    // 删掉的消息条数（按整轮）
  std::size_t protected_messages = 0;
  bool invalid_budget = false;
  std::size_t truncated = 0;  // 被单条截断的消息条数
  // 出口判定：裁完之后是否仍超预算。true 表示调用方**不能**再拿这份消息去请求，
  // 必须终止本轮 run（runner 侧发 context.overflow，stop_reason=context_overflow）。
  // 判定口径与裁剪判据完全一致：同一次 effective_tokens（estimate × k × 1.2 + reserve）。
  bool still_over = false;
  // 仍超出多少 token（同一估算口径）；still_over=false 时为 0。
  std::size_t over_by = 0;
};

// 只从"中间"丢，永不丢 system[0] 与最后 keep_recent 条。
// 最小删除单位是「一整轮」：一条 assistant(tool_calls) 及其后连续的全部 tool 结果。
// 只删单条会把 assistant 删掉、把它的 tool 结果留在 keep_recent 里，产生
// "无前置 tool_calls 的 tool 消息"，OpenAI/DeepSeek 直接 400（实测踩过）。
// 裁不动时只对未受证据保护的 tool 消息做单条截断，
// 并留下 "…[truncated N bytes]" 标记（SPEC §1 Observation 要求带截断标记）。
inline TrimResult trim_report(std::vector<llm::ChatMessage>& msgs, const ContextBudget& b) {
  TrimResult out;
  // 保护头部：system[0] 与首条 user（即当前目标）。丢它们等于丢失任务本身。
  const std::size_t protect_head = msgs.size() >= 2 ? 2 : msgs.size();
  auto round_end = [&](std::size_t begin) {
    std::size_t end = begin + 1;
    if (msgs[begin].role == "assistant" && !msgs[begin].tool_calls.empty())
      while (end < msgs.size() && msgs[end].role == "tool") ++end;
    return end;
  };
  auto protected_round = [&](std::size_t begin, std::size_t end) {
    for (std::size_t j = begin; j < end; ++j) {
      if (msgs[j].role == "system") return true;
      if (b.protect_tool) {
        if (msgs[j].role == "tool" && b.protect_tool(msgs[j].name)) return true;
        for (const auto& tc : msgs[j].tool_calls)
          if (b.protect_tool(tc.name)) return true;
      }
    }
    return false;
  };
  // A protected candidate changes nothing: reuse its estimate, and evaluate
  // only that round's policy. Recompute with the same estimator after mutation.
  auto estimate = context_estimate(msgs, b);
  auto over = [&] { return estimate.invalid || estimate.tokens > b.max_tokens; };
  for (std::size_t i = protect_head; i < msgs.size() && over();) {
    const std::size_t last_keep_start = msgs.size() > b.keep_recent ? msgs.size() - b.keep_recent : 0;
    const std::size_t end = round_end(i);
    if (end > last_keep_start || protected_round(i, end)) { i = end; continue; }
    msgs.erase(msgs.begin() + static_cast<std::ptrdiff_t>(i),
               msgs.begin() + static_cast<std::ptrdiff_t>(end));
    out.dropped += end - i;
    estimate = context_estimate(msgs, b);
  }

  const auto max = std::numeric_limits<std::size_t>::max();
  const std::size_t limit_bytes = b.single_msg_tokens > max / 3 ? max : b.single_msg_tokens * 3;
  for (std::size_t i = 0; i < msgs.size();) {
    const std::size_t end = round_end(i);
    if (protected_round(i, end)) {
      out.protected_messages += end - i;
    } else {
      for (std::size_t j = i < protect_head ? protect_head : i; j < end && over(); ++j) {
        auto& m = msgs[j];
        if (m.role != "tool" || m.content.size() <= limit_bytes) continue;
        const std::size_t cut = m.content.size() - limit_bytes;
        const std::string marker = "\n…[truncated " + std::to_string(cut) + " bytes]";
        if (marker.size() >= cut) continue;  // Never grow a message while trimming.
        m.content.resize(limit_bytes);
        m.content += marker;
        ++out.truncated;
        estimate = context_estimate(msgs, b);
      }
    }
    i = end;
  }

  // 出口（N-13）：裁完再按**同一个** over() 口径算一次，而不是另立一套。
  // 注意这里只报告、不再继续裁：protect_head（system + 当前目标）与最近 keep_recent 条
  // 是硬保护，为了凑预算去截断它们等于丢失任务本身；单条截断也只针对 tool 观测。
  // 因此 still_over 时唯一的正确动作是终止 run，由调用方决定怎么收口。
  const std::size_t est = estimate.tokens;
  out.invalid_budget = estimate.invalid;
  out.still_over = over();
  out.over_by = est > b.max_tokens ? est - b.max_tokens : 0;
  return out;
}

inline std::size_t trim(std::vector<llm::ChatMessage>& msgs, const ContextBudget& b) {
  return trim_report(msgs, b).dropped;
}

}  // namespace cue::agent
