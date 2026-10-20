// cue — Agent Harness CLI（M3a：demo 用脚本化 LLM 跑通状态机；M3b 接真实 LLM 与 UDS 工具）
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "cue/agent/agent.h"
#include "cue/pipeline/process_control.h"
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

// ---- M3b：把 mock 换成真实管线（子进程 + stdout 指标）----
void build_real_tools(tool::Registry& reg, cue::pipeline::ProcessPipelineControl& pipe) {
  reg.add(tool::ToolSpec{.name = "query_metrics",
                         .desc = "查询媒体管线指标（六段分位 + 帧统计，预聚合）",
                         .risk = tool::Risk::ReadOnly,
                         .params = {{.name = "window_ms",
                                     .type = "integer",
                                     .desc = "窗口毫秒（当前实现固定 1s 采样）",
                                     .required = false,
                                     .enum_values = {},
                                     .has_range = true,
                                     .min = 200,
                                     .max = 60000}}},
          [&pipe](const json&) {
            const auto m = pipe.latest();
            if (m.samples == 0) {
              return tool::ToolResult{false, "尚无采样（cued 未收到媒体流）", json::object()};
            }
            json j{{"fps", m.fps},
                   {"frames_in", m.frames_in},
                   {"frames_out", m.frames_out},
                   {"dropped", m.dropped},
                   {"injected", m.injected},
                   {"decoded", m.decoded},
                   {"decode_errors", m.decode_errors},
                   {"recv_p50_ms", m.recv_p50},
                   {"buffer_p50_ms", m.buffer_p50},
                   {"decode_p50_ms", m.decode_p50},
                   {"decode_p99_ms", m.decode_p99},
                   {"op_p50_ms", m.op_p50},
                   {"op_p99_ms", m.op_p99},
                   {"send_p50_ms", m.send_p50}};
            return tool::ToolResult{true, "metrics=" + j.dump(), j};
          });

  reg.add(tool::ToolSpec{.name = "pipeline_set_param",
                         .desc = "修改媒体管线参数（生效方式为重启管线，会造成短暂断流）",
                         .risk = tool::Risk::Mutating,
                         .params = {{.name = "key",
                                     .type = "string",
                                     .desc = "参数名",
                                     .required = true,
                                     .enum_values = {"bitrate_kbps", "gop", "preset", "transcode"}},
                                    {.name = "value", .type = "string", .desc = "目标值"}}},
          [&pipe](const json& a) {
            std::string err;
            const std::string k = a["key"].get<std::string>();
            const std::string v = a["value"].get<std::string>();
            if (!pipe.set_param(k, v, err)) return tool::ToolResult{false, "设置失败: " + err, json::object()};
            return tool::ToolResult{true, "已设置 " + k + "=" + v + "（管线已重启）", a};
          });

  reg.add(tool::ToolSpec{.name = "inject_fault",
                         .desc = "注入网络损伤（危险动作；M3 仅支持 drop 百分比）",
                         .risk = tool::Risk::Dangerous,
                         .params = {{.name = "kind",
                                     .type = "string",
                                     .desc = "类型",
                                     .required = true,
                                     .enum_values = {"drop"}},
                                    {.name = "value",
                                     .type = "number",
                                     .desc = "丢包百分比 0-100",
                                     .required = true,
                                     .enum_values = {},
                                     .has_range = true,
                                     .min = 0,
                                     .max = 100}}},
          [&pipe](const json& a) {
            std::string err;
            const std::string k = a["kind"].get<std::string>();
            const double v = a["value"].get<double>();
            if (!pipe.inject_fault(k, v, err)) return tool::ToolResult{false, "注入失败: " + err, json::object()};
            return tool::ToolResult{true, "已注入 " + k + "=" + std::to_string(v) + "%", a};
          });

  reg.add(tool::ToolSpec{.name = "fetch_observation",
                         .desc = "获取观测（日志尾部 / 进程状态 / 帧统计）",
                         .risk = tool::Risk::ReadOnly,
                         .params = {{.name = "source",
                                     .type = "string",
                                     .desc = "来源",
                                     .required = true,
                                     .enum_values = {"logs", "procs", "frames"}},
                                    {.name = "tail",
                                     .type = "integer",
                                     .desc = "日志行数",
                                     .required = false,
                                     .enum_values = {},
                                     .has_range = true,
                                     .min = 1,
                                     .max = 200}}},
          [&pipe](const json& a) {
            std::string err;
            const std::string src = a["source"].get<std::string>();
            const int tail = a.contains("tail") ? a["tail"].get<int>() : 20;
            const std::string obs = pipe.observation(src, tail, err);
            if (!err.empty()) return tool::ToolResult{false, err, json::object()};
            return tool::ToolResult{true, obs, json{{"source", src}}};
          });
}

int run_real(std::string goal, int warmup_s) {
  const char* key = std::getenv("CUE_API_KEY");
  if (!key || !*key) {
    std::fprintf(stderr, "CUE_API_KEY 未设置（用环境变量传入，不要写进文件）\n");
    return 2;
  }
  const char* base = std::getenv("CUE_BASE_URL");
  const char* model_env = std::getenv("CUE_MODEL");
  const std::string model = model_env && *model_env ? model_env : "deepseek-chat";

  cue::pipeline::ProcessPipelineControl::Config pc;
  pc.cued_bin = "./build/cued";
  pc.inject_drop = 5.0;  // 演练场景：先注入 5% 丢包，让 agent 去发现
  cue::pipeline::ProcessPipelineControl pipe(pc);
  std::string err;
  if (!pipe.start(err)) {
    std::fprintf(stderr, "启动管线失败: %s\n", err.c_str());
    return 1;
  }
  std::fprintf(stderr, "[cue] cued 已启动（注入 5%% 丢包作为初始故障）\n");
  if (warmup_s > 0) {
    std::fprintf(stderr, "[cue] warmup %d s：请在此期间开始推流\n", warmup_s);
    for (int i = 0; i < warmup_s; ++i) {
      ::usleep(1000 * 1000);
    }
  }

  tool::Registry reg;
  build_real_tools(reg, pipe);

  llm::OpenAiCompatibleClient client({base && *base ? base : "https://api.deepseek.com", key, model, 60});

  agent::Runner::Config cfg;
  cfg.model = model;
  cfg.system_prompt =
      "你是 Cue 的媒体管线运维 agent。你只能通过工具观测与调整管线。"
      "每轮只能调用工具；结论必须引用真实观测作为证据；每个阶段结束调用 stage_complete。"
      "根因必须使用枚举值：network_loss / network_delay / bandwidth_insufficient / "
      "encoder_too_slow / resolution_too_high / queue_backlog / keyframe_storm / clock_drift。";
  cfg.ctx.mode = tool::ApprovalMode::Auto;
  cfg.ctx.allowed_dangerous = {"inject_fault"};
  cfg.yuan_per_mtok = 2.0;

  agent::Runner runner(client, reg, cfg);
  const auto res = runner.run(goal, agent::Budget{}, print_event);

  std::printf("\n=== result ===\n");
  std::printf("stage     : %s\n", agent::stage_name(res.final_stage));
  std::printf("turns     : %d\n", res.turns);
  std::printf("cost      : %.4f CNY\n", res.cost_yuan);
  std::printf("root_cause: %s\n", res.diagnosis.root_cause.c_str());
  for (const auto& e : res.diagnosis.evidence) std::printf("evidence  : %s\n", e.c_str());
  if (!res.error.empty()) std::printf("error     : %s\n", res.error.c_str());

  pipe.stop();
  return res.ok() ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string cmd = argc > 1 ? argv[1] : "demo";
  if (cmd == "demo") return run_demo();
  if (cmd == "run") {
    std::string goal = "定位当前下行管线的异常，把端到端延迟与丢帧恢复到正常水平";
    int warmup = 0;
    for (int i = 2; i < argc; ++i) {
      if (std::string(argv[i]) == "--goal" && i + 1 < argc) goal = argv[++i];
      else if (std::string(argv[i]) == "--warmup" && i + 1 < argc) warmup = std::atoi(argv[++i]);
    }
    return run_real(goal, warmup);
  }
  std::printf(
      "usage:\n"
      "  cue demo              # 用脚本化 LLM 跑一次完整诊断（不联网、不花钱）\n"
      "  cue run --goal ...    # M3b：接真实 LLM（尚未接入）\n");
  return 2;
}
