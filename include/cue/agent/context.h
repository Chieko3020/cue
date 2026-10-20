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

// 返回被丢弃的消息条数。只从"中间"丢，永不丢 system[0] 与最后 keep_recent 条。
// 最小删除单位是「一整轮」：一条 assistant(tool_calls) 及其后连续的全部 tool 结果。
// 只删单条会把 assistant 删掉、把它的 tool 结果留在 keep_recent 里，产生
// "无前置 tool_calls 的 tool 消息"，OpenAI/DeepSeek 直接 400（实测踩过）。
inline std::size_t trim(std::vector<llm::ChatMessage>& msgs, const ContextBudget& b) {
  std::size_t dropped = 0;
  // 保护头部：system[0] 与首条 user（即当前目标）。丢它们等于丢失任务本身。
  const std::size_t protect_head = msgs.size() >= 2 ? 2 : msgs.size();
  while (estimate_tokens(msgs) > b.max_tokens &&
         msgs.size() > b.keep_recent + protect_head) {
    const std::size_t last_keep_start = msgs.size() - b.keep_recent;
    if (protect_head >= last_keep_start) break;
    // 一轮 = [assistant(tool_calls), tool, tool, ...]
    std::size_t end = protect_head + 1;
    if (msgs[protect_head].role == "assistant" && !msgs[protect_head].tool_calls.empty()) {
      while (end < last_keep_start && msgs[end].role == "tool") ++end;
    }
    // 整轮会侵入保护区时宁可不裁（裁一半同样是坏配对）
    if (end > last_keep_start) break;
    msgs.erase(msgs.begin() + static_cast<std::ptrdiff_t>(protect_head),
               msgs.begin() + static_cast<std::ptrdiff_t>(end));
    dropped += end - protect_head;
  }
  return dropped;
}

}  // namespace cue::agent
