// 上下文预算单测：估算、裁剪边界、不可丢的消息
#include <algorithm>
#include <cstdio>
#include <limits>
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

  // ---- N-13：裁完之后是否仍在预算内（TrimResult::still_over / over_by）----

  // 1) 正常预算：不丢不截，也不算 overflow
  {
    std::vector<llm::ChatMessage> msgs;
    msgs.push_back(msg("system", 300, 's'));
    msgs.push_back(msg("user", 300, 'u'));
    msgs.push_back(msg("tool", 300, 'o'));
    agent::ContextBudget b;
    b.max_tokens = 5000;
    b.keep_recent = 2;
    const auto tr = agent::trim_report(msgs, b);
    check(tr.dropped == 0 && tr.truncated == 0, "N-13 fit: nothing trimmed");
    check(!tr.still_over, "N-13 fit: still_over=false");
    check(tr.over_by == 0, "N-13 fit: over_by=0");
    check(msgs.size() == 3, "N-13 fit: messages untouched");
  }

  // 2) 需要裁剪、但丢完就够：dropped>0 且 still_over=false
  {
    std::vector<llm::ChatMessage> msgs;
    msgs.push_back(msg("system", 300, 's'));
    msgs.push_back(msg("user", 300, 'u'));
    for (int i = 0; i < 18; ++i) msgs.push_back(msg("tool", 3000));
    agent::ContextBudget b;
    b.max_tokens = 10000;
    b.keep_recent = 6;
    const auto tr = agent::trim_report(msgs, b);
    check(tr.dropped > 0, "N-13 trim sufficient: dropped>0");
    check(!tr.still_over, "N-13 trim sufficient: still_over=false");
    check(tr.over_by == 0, "N-13 trim sufficient: over_by=0");
    check(agent::effective_tokens(msgs, b) <= b.max_tokens, "N-13 trim sufficient: within budget");
  }

  // 3) 裁完仍超限：极小预算 + 保护区里必留的大消息 → still_over=true，
  //    且 system 与最后一条 user（阶段提示）必须原样保留（不允许为了凑预算截断它们）
  {
    const std::string sys = std::string(300, 's');
    const std::string goal = std::string(900, 'g');
    const std::string hint = std::string(600, 'h');
    std::vector<llm::ChatMessage> msgs;
    msgs.push_back(msg("system", 300, 's'));
    msgs.push_back(msg("user", 900, 'g'));
    for (int i = 0; i < 8; ++i) msgs.push_back(msg("tool", 3000, 'o'));
    msgs.push_back(msg("user", 600, 'h'));  // 阶段提示：每轮追加在末尾
    agent::ContextBudget b;
    b.max_tokens = 50;
    b.keep_recent = 2;
    const auto tr = agent::trim_report(msgs, b);
    check(tr.dropped > 0, "N-13 over: dropped>0");
    check(tr.still_over, "N-13 over: still_over=true");
    check(tr.over_by > 0, "N-13 over: over_by>0");
    check(msgs.front().content == sys, "N-13 over: system prompt untouched");
    check(msgs[1].content == goal, "N-13 over: current goal untouched");
    check(msgs.back().content == hint, "N-13 over: stage hint untouched");
    check(msgs.size() == b.keep_recent + 2, "N-13 over: only middle rounds dropped");
  }

  // 4) 单条截断后仍超限：巨大 tool 观测 + 头部本身也很重 → truncated>0 且 still_over=true
  {
    const std::string sys = std::string(3000, 's');
    std::vector<llm::ChatMessage> msgs;
    msgs.push_back(msg("system", 3000, 's'));
    msgs.push_back(msg("user", 3000, 'u'));
    msgs.push_back(msg("tool", 20000, 'L'));
    agent::ContextBudget b;
    b.max_tokens = 2000;
    b.keep_recent = 2;
    b.single_msg_tokens = 500;  // 单条截到 1500 B
    const auto tr = agent::trim_report(msgs, b);
    check(tr.truncated == 1, "N-13 truncate then over: truncated==1");
    check(tr.still_over, "N-13 truncate then over: still_over=true");
    check(tr.over_by > 0, "N-13 truncate then over: over_by>0");
    check(msgs.front().content == sys, "N-13 truncate then over: system untouched");
    check(msgs[2].content.find("[truncated") != std::string::npos,
          "N-13 truncate then over: marker kept");
  }

  // 5) reserve_tokens 参与出口判定：同一份输入，reserve 加大后从 fit 变 over；
  //    reserve 是每轮固定开销，裁历史裁不掉它
  {
    auto build = [] {
      std::vector<llm::ChatMessage> msgs;
      msgs.push_back(msg("system", 300, 's'));
      msgs.push_back(msg("user", 300, 'u'));
      msgs.push_back(msg("tool", 300, 'o'));
      return msgs;
    };
    agent::ContextBudget b;
    b.max_tokens = 2000;
    b.keep_recent = 2;
    b.reserve_tokens = 0;
    auto msgs = build();
    const auto fit = agent::trim_report(msgs, b);
    check(!fit.still_over, "N-13 reserve: fit without reserve");
    check(fit.over_by == 0, "N-13 reserve: fit over_by=0");

    b.reserve_tokens = 5000;  // 仅 tools schema 就装不下
    msgs = build();
    const auto over = agent::trim_report(msgs, b);
    check(over.still_over, "N-13 reserve: over with large reserve");
    check(over.dropped == 0 && over.truncated == 0, "N-13 reserve: reserve cannot be trimmed away");
    check(over.over_by == agent::effective_tokens(msgs, b) - b.max_tokens,
          "N-13 reserve: over_by consistent with effective_tokens");
  }

  // C12: protected rounds survive, later eligible rounds are still reclaimed.
  {
    std::vector<llm::ChatMessage> history{msg("system", 30), msg("user", 30)};
    auto round = [&](const std::string& name, std::size_t bytes) {
      llm::ChatMessage a;
      a.role = "assistant";
      a.tool_calls.push_back({name, name, "{}"});
      history.push_back(a);
      auto t = msg("tool", bytes);
      t.name = name;
      t.tool_call_id = name;
      history.push_back(t);
    };
    round("query_metrics", 6000);
    round("stage_complete", 6000);
    round("other", 6000);
    history.push_back(msg("user", 30));
    agent::ContextBudget b;
    b.max_tokens = 6000;
    b.keep_recent = 1;
    b.single_msg_tokens = 1;
    b.protect_tool = [](const std::string& name) { return name != "other"; };
    const auto result = agent::trim_report(history, b);
    check(result.dropped == 2 && result.truncated == 0 && !result.still_over,
          "C12 skip protected oldest, delete later complete round");
    check(history[3].content == std::string(6000, 'x') && history[5].content == std::string(6000, 'x'),
          "C12 observation and diagnosis byte exact");
    b.keep_recent = std::numeric_limits<std::size_t>::max();
    b.max_tokens = 1;
    const auto saved = history;
    const auto tiny = agent::trim_report(history, b);
    check(tiny.dropped == 0 && tiny.truncated == 0 && tiny.still_over, "C12 huge keep_recent safe");
    check(history.size() == saved.size(), "C12 huge recent preserves complete rounds");
    b.k = std::numeric_limits<double>::quiet_NaN();
    b.max_tokens = std::numeric_limits<std::size_t>::max();
    check(agent::trim_report(history, b).still_over, "C12 NaN fails closed at maximum budget");
    b.k = std::numeric_limits<double>::max();
    check(agent::trim_report(history, b).still_over, "C12 scaled estimate overflow fails closed");
    b.k = 1;
    b.schema_tokens = std::numeric_limits<std::size_t>::max();
    b.output_tokens = 1;
    check(agent::trim_report(history, b).still_over, "C12 reserve arithmetic overflow fails closed");
    b.schema_tokens = 0;
    b.output_tokens = -1;
    check(agent::trim_report(history, b).still_over, "C12 negative output fails closed");
  }
  {
    std::vector<llm::ChatMessage> history{msg("system", 30), msg("user", 30)};
    agent::ContextBudget b;
    b.k = 2;
    const auto input = agent::effective_tokens(history, b);
    b.schema_tokens = 7;
    b.reserve_tokens = 11;
    b.output_tokens = 101;
    check(agent::effective_tokens(history, b) == input + 119, "C12 fixed reserves not calibrated");
    b.max_tokens = input + 119;
    check(!agent::trim_report(history, b).still_over, "C12 exact budget equality fits");
    --b.max_tokens;
    check(agent::trim_report(history, b).over_by == 1, "C12 one output token over boundary");
    b.single_msg_tokens = std::numeric_limits<std::size_t>::max();
    history.push_back(msg("tool", 300));
    b.keep_recent = 100;
    check(agent::trim_report(history, b).truncated == 0, "C12 huge single message limit cannot wrap");
  }

  {
    std::vector<llm::ChatMessage> history{msg("system", 30), msg("user", 30)};
    constexpr std::size_t rounds = 200;
    for (std::size_t i = 0; i < rounds; ++i) {
      llm::ChatMessage a;
      a.role = "assistant";
      a.tool_calls.push_back({"id" + std::to_string(i), "query_metrics", "{}"});
      history.push_back(a);
      auto t = msg("tool", 300);
      t.name = "query_metrics";
      t.tool_call_id = a.tool_calls[0].id;
      history.push_back(t);
    }
    agent::ContextBudget b;
    b.max_tokens = 1;
    std::size_t checks = 0;
    b.protect_tool = [&](const std::string&) { ++checks; return true; };
    const auto result = agent::trim_report(history, b);
    check(result.still_over && result.dropped == 0 && result.truncated == 0,
          "C12 all protected history fails closed");
    check(checks <= 2 * rounds, "C12 protected policy checks grow linearly with rounds");
  }

  if (failures == 0) {
    std::printf("test_context: OK (16 cases)\n");
    return 0;
  }
  std::printf("test_context: %d failure(s)\n", failures);
  return 1;
}
