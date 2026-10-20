// 上下文预算：估算 + 裁剪
// 原则（SPEC §7.9）：超限时丢最旧的观测，但 system、当前目标、最近 N 条必须保留。
#pragma once

#include <cstddef>
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
  // 单条消息的硬预算（token）：裁不动时对最旧的 tool 消息做头部保留 + 截断标记
  std::size_t single_msg_tokens = 4000;
};

// 中英混合的保守估算：约 3 字节/token（报告里必须标注"估算"）
inline std::size_t estimate_tokens(const std::string& s) { return (s.size() + 2) / 3; }

inline std::size_t estimate_tokens(const llm::ChatMessage& m) {
  std::size_t n = estimate_tokens(m.content) + estimate_tokens(m.name) + 4;
  for (const auto& tc : m.tool_calls) n += estimate_tokens(tc.name) + estimate_tokens(tc.arguments) + 8;
  return n;
}

inline std::size_t estimate_tokens(const std::vector<llm::ChatMessage>& msgs) {
  std::size_t n = 0;
  for (const auto& m : msgs) n += estimate_tokens(m);
  return n;
}

// tools schema 的估算（JSON 原文 / 3）
inline std::size_t estimate_tokens(const nlohmann::json& j) {
  return j.is_null() ? 0 : (j.dump().size() + 2) / 3;
}

// SPEC §7.9 三层估算：bytes/3.0 × k × 1.2 + reserve。宁多算不少算（报告标注"估算"）。
inline std::size_t effective_tokens(const std::vector<llm::ChatMessage>& msgs, const ContextBudget& b) {
  const double k = b.k > 0.0 ? b.k : 1.0;
  return std::size_t(double(estimate_tokens(msgs)) * k * 1.2) + b.reserve_tokens;
}

struct TrimResult {
  std::size_t dropped = 0;    // 删掉的消息条数（按整轮）
  std::size_t truncated = 0;  // 被单条截断的消息条数
};

// 只从"中间"丢，永不丢 system[0] 与最后 keep_recent 条。
// 最小删除单位是「一整轮」：一条 assistant(tool_calls) 及其后连续的全部 tool 结果。
// 只删单条会把 assistant 删掉、把它的 tool 结果留在 keep_recent 里，产生
// "无前置 tool_calls 的 tool 消息"，OpenAI/DeepSeek 直接 400（实测踩过）。
// 裁不动时（keep_recent 全是大观测）退化为对最旧 tool 消息做单条截断，
// 并留下 "…[truncated N bytes]" 标记（SPEC §1 Observation 要求带截断标记）。
inline TrimResult trim_report(std::vector<llm::ChatMessage>& msgs, const ContextBudget& b) {
  TrimResult out;
  // 保护头部：system[0] 与首条 user（即当前目标）。丢它们等于丢失任务本身。
  const std::size_t protect_head = msgs.size() >= 2 ? 2 : msgs.size();
  auto over = [&] { return effective_tokens(msgs, b) > b.max_tokens; };
  while (over() && msgs.size() > b.keep_recent + protect_head) {
    const std::size_t last_keep_start = msgs.size() - b.keep_recent;
    if (protect_head >= last_keep_start) break;
    // 一轮 = [assistant(tool_calls), tool, tool, ...]
    // end 的扫描不受保护区限制：先数完这一轮，再判断它是否侵入保护区。
    // 若把扫描截在 last_keep_start，会只删掉 assistant 而留下它的 tool 结果 → 孤立 tool 消息。
    std::size_t end = protect_head + 1;
    if (msgs[protect_head].role == "assistant" && !msgs[protect_head].tool_calls.empty()) {
      while (end < msgs.size() && msgs[end].role == "tool") ++end;
    }
    // 整轮会侵入保护区时宁可不裁（裁一半同样是坏配对）
    if (end > last_keep_start) break;
    msgs.erase(msgs.begin() + static_cast<std::ptrdiff_t>(protect_head),
               msgs.begin() + static_cast<std::ptrdiff_t>(end));
    out.dropped += end - protect_head;
  }

  // 兜底：仍然超预算时，截断最旧的 tool 消息（保留头部 + 标记，配对不破坏）
  const std::size_t limit_bytes = b.single_msg_tokens * 3;
  for (std::size_t i = protect_head; i < msgs.size() && over(); ++i) {
    auto& m = msgs[i];
    if (m.role != "tool" || m.content.size() <= limit_bytes) continue;
    const std::size_t cut = m.content.size() - limit_bytes;
    m.content.resize(limit_bytes);
    m.content += "\n…[truncated " + std::to_string(cut) + " bytes]";
    ++out.truncated;
  }
  return out;
}

inline std::size_t trim(std::vector<llm::ChatMessage>& msgs, const ContextBudget& b) {
  return trim_report(msgs, b).dropped;
}

}  // namespace cue::agent
