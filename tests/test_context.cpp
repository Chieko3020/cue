// 上下文预算单测：估算、裁剪边界、不可丢的消息
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

  if (failures == 0) {
    std::printf("test_context: OK (4 cases)\n");
    return 0;
  }
  std::printf("test_context: %d failure(s)\n", failures);
  return 1;
}
