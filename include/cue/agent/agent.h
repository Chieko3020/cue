// Agent 内核：阶段状态机（外层）+ ReAct 工具循环（内层）
// M3a：状态机 + 守卫 + 工具循环；M3b 接入真实 LLM 与审批界面
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cue/llm/client.h"
#include "cue/tool/tool.h"

namespace cue::agent {

using json = nlohmann::json;

enum class Stage { Collect, Diagnose, Repair, Verify, Done, GiveUp };
const char* stage_name(Stage s);

struct Budget {
  int max_turns = 12;
  double max_yuan = 0.3;
  int max_wall_s = 180;
};

struct Diagnosis {
  std::string root_cause;
  std::vector<std::string> evidence;
  double confidence = 0.0;
};

struct Event {
  std::string type;
  json data;
};

struct RunResult {
  Stage final_stage = Stage::GiveUp;
  int turns = 0;
  double cost_yuan = 0.0;
  Diagnosis diagnosis;
  std::string error;
  // "跑完了" 必须是 "到达 done 且没有 error"：只有 stage 会把「done without verify」
  // 这种自相矛盾的结果报成成功（eval 侧又按 error 记为 agent_error）。
  bool ok() const { return final_stage == Stage::Done && error.empty(); }
};

class Runner {
 public:
  struct Config {
    std::string model;
    std::string system_prompt;
    tool::CallContext ctx;
    // 每 1M prompt+completion token 的价格（元），用于成本预算；0 表示不计量
    double yuan_per_mtok = 2.0;
    // 上下文预算：超出后丢最旧观测（system 与最近若干条必须保留）
    std::size_t max_context_tokens = 24000;
    // 单轮输出上限。1024 会在多工具/长证据时被截断（SPEC §7.9）。
    int max_tokens = 2048;
    // verify → done 的 harness 重测（SPEC §7.5："harness 重新测量 10 s 窗口并满足 SLA"）。
    // 为空时保留"模型自称"的旧行为，但事件里标注 verify=model_claimed，
    // 避免评测/交互两条路径把"自报达标"当成"实测达标"。
    std::function<bool(const Diagnosis&)> verify_sla;
  };

  Runner(llm::ILlmClient& llm, const tool::Registry& tools, Config cfg);

  // 不抛异常：所有输入都来自模型（未受信），异常必须在这里收口（SPEC §9）。
  RunResult run(const std::string& goal, const Budget& budget,
                const std::function<void(const Event&)>& on_event = nullptr);

 private:
  RunResult run_impl(const std::string& goal, const Budget& budget,
                     const std::function<void(const Event&)>& on_event);

  llm::ILlmClient& llm_;
  const tool::Registry& tools_;
  Config cfg_;
};

}  // namespace cue::agent
