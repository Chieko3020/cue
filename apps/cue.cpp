// cue — Agent Harness CLI（M3a：demo 用脚本化 LLM 跑通状态机；M3b 接真实 LLM 与 UDS 工具）
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "cue/agent/agent.h"
#include "cue/llm/client.h"
#include "cue/tool/tool.h"

using json = nlohmann::json;
namespace tool = cue::tool;
namespace llm = cue::llm;
namespace agent = cue::agent;

namespace {

// M3a 的 mock 工具：行为与 M2 的真实指标同构；M3b 换成 UDS 客户端
struct ToolState {
  int metrics_calls = 0;
  int set_param_calls = 0;
  int inject_calls = 0;

  void build(tool::Registry& reg) {
    reg.add(tool::ToolSpec{.name = "query_metrics",
                           .desc = "查询管线指标（预聚合摘要，不是原始日志）",
                           .risk = tool::Risk::ReadOnly,
                           .params = {{.name = "window_ms",
                                       .type = "integer",
                                       .desc = "窗口毫秒",
                                       .required = false,
                                       .enum_values = {},
                                       .has_range = true,
                                       .min = 200,
                                       .max = 60000}}},
            [this](const json&) {
              ++metrics_calls;
              json m{{"p50_ms", 31.2}, {"p95_ms", 58.9}, {"p99_ms", 181.4}, {"drop_rate", 0.0},
                     {"queue_depth", 3}, {"nack_retransmit", 42}};
              return tool::ToolResult{true, "metrics=" + m.dump(), m};
            });

    reg.add(tool::ToolSpec{.name = "pipeline_set_param",
                           .desc = "修改媒体管线参数",
                           .risk = tool::Risk::Mutating,
                           .params = {{.name = "key",
                                       .type = "string",
                                       .desc = "参数名",
                                       .required = true,
                                       .enum_values = {"bitrate_kbps", "gop", "preset", "transcode"}},
                                      {.name = "value", .type = "string", .desc = "目标值"}}},
            [this](const json& a) {
              ++set_param_calls;
              return tool::ToolResult{true, "ok", a};
            });

    reg.add(tool::ToolSpec{.name = "inject_fault",
                           .desc = "注入网络损伤（危险动作）",
                           .risk = tool::Risk::Dangerous,
                           .params = {{.name = "kind",
                                       .type = "string",
                                       .desc = "类型",
                                       .required = true,
                                       .enum_values = {"drop", "delay", "jitter", "rate"}},
                                      {.name = "value", .type = "number", .desc = "数值"}}},
            [this](const json& a) {
              ++inject_calls;
              return tool::ToolResult{true, "injected", a};
            });

    reg.add(tool::ToolSpec{.name = "fetch_observation",
                           .desc = "获取观测（日志尾部/进程/帧统计）",
                           .risk = tool::Risk::ReadOnly,
                           .params = {{.name = "source",
                                       .type = "string",
                                       .desc = "来源",
                                       .required = true,
                                       .enum_values = {"logs", "procs", "frames"}},
                                      {.name = "tail",
                                       .type = "integer",
                                       .desc = "行数",
                                       .required = false,
                                       .enum_values = {},
                                       .has_range = true,
                                       .min = 1,
                                       .max = 500}}},
            [](const json& a) {
              return tool::ToolResult{true, "observation", json{{"source", a["source"]}}};
            });
  }
};

void print_event(const agent::Event& e) {
  if (e.type == "llm.response") return;
  std::printf("[event] %-18s %s\n", e.type.c_str(), e.data.dump().c_str());
}

int run_demo() {
  ToolState state;
  tool::Registry reg;
  state.build(reg);

  llm::ScriptedClient client;
  auto call = [](const char* id, const char* name, json args) {
    llm::ChatResponse r;
    llm::ToolCall tc;
    tc.id = id;
    tc.name = name;
    tc.arguments = args.dump();
    r.tool_calls.push_back(tc);
    r.usage = {120, 30};
    return r;
  };
  client.push(call("c1", "query_metrics", json{{"window_ms", 5000}}));
  client.push(call("c2", "stage_complete", json{{"next", "diagnose"}}));
  client.push(call("c3", "stage_complete",
                   json{{"next", "repair"},
                        {"root_cause", "network_loss"},
                        {"evidence", {"nack_retransmit=42"}},
                        {"confidence", 0.8}}));
  client.push(call("c4", "pipeline_set_param", json{{"key", "bitrate_kbps"}, {"value", "800"}}));
  client.push(call("c5", "stage_complete", json{{"next", "verify"}}));
  client.push(call("c6", "query_metrics", json{{"window_ms", 5000}}));
  client.push(call("c7", "stage_complete", json{{"next", "done"}}));

  agent::Runner::Config cfg;
  cfg.model = "scripted";
  cfg.system_prompt =
      "你是 Cue 的媒体管线运维 agent。你只能通过工具观测与调整管线；"
      "结论必须引用真实观测作为证据；每个阶段结束时调用 stage_complete。";
  cfg.ctx.mode = tool::ApprovalMode::Auto;

  agent::Runner runner(client, reg, cfg);
  const auto res = runner.run("把下行端到端 p99 压到 120ms 以下", agent::Budget{}, print_event);

  std::printf("\n=== result ===\n");
  std::printf("stage     : %s\n", agent::stage_name(res.final_stage));
  std::printf("turns     : %d\n", res.turns);
  std::printf("cost      : %.4f CNY\n", res.cost_yuan);
  std::printf("root_cause: %s\n", res.diagnosis.root_cause.c_str());
  std::printf("evidence  : %zu item(s)\n", res.diagnosis.evidence.size());
  if (!res.error.empty()) std::printf("error     : %s\n", res.error.c_str());
  std::printf("tools     : metrics=%d set_param=%d inject=%d\n", state.metrics_calls,
              state.set_param_calls, state.inject_calls);
  return res.ok() ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string cmd = argc > 1 ? argv[1] : "demo";
  if (cmd == "demo") return run_demo();
  std::printf(
      "usage:\n"
      "  cue demo              # 用脚本化 LLM 跑一次完整诊断（不联网、不花钱）\n"
      "  cue run --goal ...    # M3b：接真实 LLM（尚未接入）\n");
  return 2;
}
