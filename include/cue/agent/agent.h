// Agent 内核：阶段状态机（外层）+ ReAct 工具循环（内层）
// M3a：状态机 + 守卫 + 工具循环；M3b 接入真实 LLM 与审批界面
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cue/llm/client.h"
#include "cue/tool/tool.h"

namespace cue::agent {

using json = nlohmann::json;

// 会话持久化（FR-AGENT.5）：只在这里前向声明，避免 agent.h ⇄ session.h 互相包含
class SessionLog;
struct LoadedSession;

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
  // 会话写盘失败（磁盘满/权限/锁丢失…）。**不让它把已经跑完的诊断判成失败**，
  // 但必须能被调用方看到：静默失败等于"用户以为有存档、崩溃后什么都没有"。
  std::string session_error;
  // "跑完了" 必须是 "到达 done 且没有 error"：只有 stage 会把「done without verify」
  // 这种自相矛盾的结果报成成功（eval 侧又按 error 记为 agent_error）。
  bool ok() const { return final_stage == Stage::Done && error.empty() && session_error.empty(); }
};

class Runner {
 public:
  struct Config {
    std::string model;
    std::string system_prompt;
    tool::CallContext ctx;
    // 成本估算单价（元 / 1M token）。报告里的 cost 是**估算，不是账单**：
    // output 单价通常显著高于 input，cache hit 又更低。
    // 兼容字段：prompt/completion 未单独配置（=0）时用它统一计价。
    double yuan_per_mtok = 2.0;
    double prompt_yuan_per_mtok = 0.0;      // 0 = 回落到 yuan_per_mtok
    double completion_yuan_per_mtok = 0.0;  // 0 = 回落到 yuan_per_mtok
    // 上下文预算：超出后丢最旧观测（system 与最近若干条必须保留）
    std::size_t max_context_tokens = 24000;
    // 单轮输出上限。1024 会在多工具/长证据时被截断（SPEC §7.9）。
    int max_tokens = 2048;
    // verify → done 的 harness 重测（SPEC §7.5："harness 重新测量 10 s 窗口并满足 SLA"）。
    // 为空时保留"模型自称"的旧行为，但事件里标注 verify=model_claimed，
    // 避免评测/交互两条路径把"自报达标"当成"实测达标"。
    std::function<bool(const Diagnosis&)> verify_sla;
    // 非空且被置位 = 中止本次 run（SIGTERM / 外部取消）。每轮开始与每次 LLM 调用前检查；
    // 它不参与成本/轮次预算，也不写 done 阶段：取消的 run 一律以 error="cancelled" 失败。
    const std::atomic<bool>* cancel = nullptr;

    // ---- 会话持久化（FR-AGENT.5）----
    // 非空：把本次 run 的完整轨迹（含**当时那份 system prompt**）逐条追加进 session。
    // 每条状态变化都落盘，进程被 kill -9 之后文件里是到那一刻为止的完整快照。
    SessionLog* session = nullptr;
    // 非空：从已加载的 session 继续——阶段复位到中断时的阶段、预算从累计值接着算、
    // 对话逐条重放（system prompt 用 session 里记的，不用 cfg_.system_prompt）。
    // 通常与 session 一起给：resume 决定起点，session 决定往哪写。
    const LoadedSession* resume = nullptr;
    // 新会话 meta 里的 mode（"run" / "demo" / "eval"）；恢复时不使用。
    std::string session_mode = "run";
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
  // 上下文 token 估算的在线校准系数 k（SPEC §7.9：事后以 usage 为真值）
  double ctx_k_ = 1.0;
};

}  // namespace cue::agent
