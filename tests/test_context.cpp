// 上下文预算单测：估算、裁剪边界、不可丢的消息
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "cue/agent/context.h"
#include "cue/llm/client.h"

namespace agent = cue::agent;
namespace llm = cue::llm;

namespace {
int failures = 0;
void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

llm::ChatMessage msg(const char* role, std::size_t bytes, char fill = 'x') {
  llm::ChatMessage m;
  m.role = role;
  m.content = std::string(bytes, fill);
  return m;
}
}  // namespace

int main() {
  // 估算：约 3 字节/token
  check(agent::estimate_tokens(std::string(300, 'a')) == 100, "estimate 300B -> 100 tokens");

  // 超限裁剪：20 条 3KB 消息（约 20*1000=20000 tokens），预算 5000
  {
    std::vector<llm::ChatMessage> msgs;
    msgs.push_back(msg("system", 300, 's'));
    msgs.push_back(msg("user", 300, 'u'));  // goal
    for (int i = 0; i < 18; ++i) msgs.push_back(msg("tool", 3000));

    const std::size_t before = agent::estimate_tokens(msgs);
    agent::ContextBudget b;
    b.max_tokens = 10000;
    b.keep_recent = 6;
    const std::size_t dropped = agent::trim(msgs, b);
    const std::size_t after = agent::estimate_tokens(msgs);

    check(dropped > 0, "trim dropped something");
    check(after <= 10000, "trim under budget");
    check(after < before, "trim reduced tokens");
    check(msgs.front().role == "system", "system message kept");
    check(msgs[1].role == "user", "goal kept");
    check(msgs.size() >= b.keep_recent + 2, "kept recent messages");
    // 最后 keep_recent 条必须原样保留
    check(msgs.back().role == "tool", "last message kept");
  }

  // 未超限时不动
  {
    std::vector<llm::ChatMessage> msgs;
    msgs.push_back(msg("system", 30));
    msgs.push_back(msg("user", 30));
    agent::ContextBudget b;
    b.max_tokens = 10000;
    const std::size_t before = msgs.size();
    check(agent::trim(msgs, b) == 0, "no trim when under budget");
    check(msgs.size() == before, "size unchanged");
  }

  // 极小预算：不应无限循环，也不会丢掉 system
  {
    std::vector<llm::ChatMessage> msgs;
    msgs.push_back(msg("system", 300));
    for (int i = 0; i < 10; ++i) msgs.push_back(msg("tool", 3000));
    agent::ContextBudget b;
    b.max_tokens = 100;  // 根本装不下
    b.keep_recent = 6;
    agent::trim(msgs, b);
    check(msgs.front().role == "system", "system survives tiny budget");
    check(msgs.size() >= 1, "not emptied");
  }

  // 裁剪必须保持 assistant(tool_calls) 与 tool 结果的配对：
  // 出现"无前置 tool_calls 的 tool 消息"或"有调用没结果的 assistant"，真实 API 直接 400
  {
    std::vector<llm::ChatMessage> msgs;
    msgs.push_back(msg("system", 300, 's'));
    msgs.push_back(msg("user", 300, 'u'));
    for (int round = 0; round < 12; ++round) {
      llm::ChatMessage a;
      a.role = "assistant";
      // 一轮两个工具调用：每条 tool 结果对应一个独立 tool_call_id
      for (int k = 0; k < 2; ++k) {
        llm::ToolCall tc;
        tc.id = "call" + std::to_string(round) + "-" + std::to_string(k);
        tc.name = "query_metrics";
        tc.arguments = "{}";
        a.tool_calls.push_back(tc);
      }
      msgs.push_back(a);
      for (const auto& tc : msgs.back().tool_calls) {
        llm::ChatMessage t;
        t.role = "tool";
        t.content = std::string(1500, 'o');
        t.tool_call_id = tc.id;
        t.name = "query_metrics";
        msgs.push_back(t);
      }
    }
    agent::ContextBudget b;
    b.max_tokens = 8000;
    b.keep_recent = 6;
    const auto tr = agent::trim_report(msgs, b);
    check(tr.dropped > 0, "pair trim dropped rounds");

    std::vector<std::string> open;
    bool orphan = false;
    for (const auto& m : msgs) {
      if (m.role == "assistant") {
        for (const auto& tc : m.tool_calls) open.push_back(tc.id);
      } else if (m.role == "tool") {
        const auto it = std::find(open.begin(), open.end(), m.tool_call_id);
        if (it == open.end()) {
          orphan = true;
          break;
        }
        open.erase(it);
      }
    }
    check(!orphan, "pair trim: no orphan tool message");
    check(open.empty(), "pair trim: no assistant(tool_calls) without result");
  }

  // tools schema 的固定开销（reserve）必须计入：同样的历史，reserve 越大越早开始裁
  {
    auto build = [] {
      std::vector<llm::ChatMessage> msgs;
      msgs.push_back(msg("system", 300));
      msgs.push_back(msg("user", 300));
      for (int i = 0; i < 10; ++i) msgs.push_back(msg("tool", 900));
      return msgs;
    };
    auto with_reserve = [&](std::size_t reserve) {
      auto msgs = build();
      agent::ContextBudget b;
      b.max_tokens = 4000;
      b.keep_recent = 2;
      b.reserve_tokens = reserve;
      return agent::trim_report(msgs, b).dropped;
    };
    check(with_reserve(0) < with_reserve(3000), "reserve tokens counted in budget");
  }

  // 最近 N 条全是大观测、裁不动时：单条截断 + 截断标记（SPEC §1 Observation）
  {
    std::vector<llm::ChatMessage> msgs;
    msgs.push_back(msg("system", 300));
    msgs.push_back(msg("user", 300));
    for (int i = 0; i < 6; ++i) {
      llm::ChatMessage t;
      t.role = "tool";
      t.content = std::string(20000, 'L');
      t.tool_call_id = "c" + std::to_string(i);
      t.name = "fetch_observation";
      msgs.push_back(t);
    }
    agent::ContextBudget b;
    b.max_tokens = 4000;
    b.keep_recent = 6;
    b.single_msg_tokens = 500;
    const auto tr = agent::trim_report(msgs, b);
    check(tr.truncated > 0, "oversized tool messages get truncated");
    bool marked = false;
    for (const auto& m : msgs) {
      if (m.content.find("[truncated") != std::string::npos) marked = true;
    }
    check(marked, "truncation leaves a marker");
    check(agent::effective_tokens(msgs, b) <= b.max_tokens, "truncation brings budget down");
  }

  // k 校准：effective 必须随 k 上升（usage 偏大时更早裁剪）
  {
    std::vector<llm::ChatMessage> msgs;
    msgs.push_back(msg("tool", 3000));
    agent::ContextBudget b;
    b.k = 1.0;
    const std::size_t t1 = agent::effective_tokens(msgs, b);
    b.k = 2.0;
    const std::size_t t2 = agent::effective_tokens(msgs, b);
    check(t2 > t1, "calibration factor k scales estimate");
    check(t1 >= agent::estimate_tokens(msgs), "effective >= raw estimate");
  }

  if (failures == 0) {
    std::printf("test_context: OK (8 cases)\n");
    return 0;
  }
  std::printf("test_context: %d failure(s)\n", failures);
  return 1;
}
