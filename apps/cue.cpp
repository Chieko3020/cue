// cue — Agent Harness CLI
//   demo：脚本化 LLM 跑通状态机（不联网、不花钱）
//   run ：真实 LLM（DeepSeek 兼容端点）+ 真实 cued 管线
#include <cerrno>
#include <csignal>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "cue/agent/agent.h"
#include "cue/pipeline/process_control.h"
#include "cue/pipeline/tools.h"
#include "cue/llm/client.h"
#include "cue/tool/tool.h"

using json = nlohmann::json;
namespace tool = cue::tool;
namespace llm = cue::llm;
namespace agent = cue::agent;

namespace {

// ---------------------------------------------------------------- 参数解析

void print_usage() {
  std::printf(
      "usage:\n"
      "  cue demo\n"
      "      用脚本化 LLM 跑一次完整诊断（不联网、不花钱）\n"
      "  cue run [--goal TEXT] [--warmup S] [--model NAME] [--approval ask|auto|deny]\n"
      "          [--allow-dangerous NAMES|none] [--max-turns N] [--max-yuan Y] [--max-wall S]\n"
      "      接真实 LLM 对真实 cued 管线跑一次诊断（M3b 已接入）。\n"
      "      环境变量：CUE_API_KEY（必需）、CUE_BASE_URL、CUE_MODEL\n"
      "  cue help\n"
      "\n"
      "  --allow-dangerous 逗号分隔的危险工具白名单，默认 inject_fault（演练场景要能取消注入）；\n"
      "                    none 表示全部禁止\n"
      "  --approval        auto（默认，自动批准+留痕）/ deny（禁一切非只读）/ ask（需审批回调，M5）\n"
      "\n"
      "错误：参数非法、缺值或未知参数一律报错退出（返回码 2），不静默忽略。\n");
}

bool take_value(int argc, char** argv, int& i, const std::string& opt, std::string& out) {
  if (i + 1 >= argc) {
    std::fprintf(stderr, "参数 %s 缺少取值\n", opt.c_str());
    return false;
  }
  out = argv[++i];
  return true;
}

bool parse_int(const std::string& s, int& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  errno = 0;
  const long v = std::strtol(s.c_str(), &end, 10);
  if (errno != 0 || end == nullptr || *end != '\0' || v < 0 || v > 1'000'000) return false;
  out = static_cast<int>(v);
  return true;
}

bool parse_double(const std::string& s, double& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  errno = 0;
  const double v = std::strtod(s.c_str(), &end);
  if (errno != 0 || end == nullptr || *end != '\0' || !(v >= 0.0)) return false;
  out = v;
  return true;
}

std::vector<std::string> split_commas(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (const char ch : s) {
    if (ch == ',') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else if (ch != ' ') {
      cur.push_back(ch);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

struct RunOptions {
  std::string goal = "定位当前下行管线的异常，把端到端延迟与丢帧恢复到正常水平";
  int warmup_s = 0;
  std::string model;
  std::string approval = "auto";
  std::string allow_dangerous = "inject_fault";
  agent::Budget budget;
  bool help = false;
};

bool parse_run_args(int argc, char** argv, RunOptions& o) {
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    std::string v;
    if (a == "-h" || a == "--help") {
      o.help = true;
      return true;
    } else if (a == "--goal") {
      if (!take_value(argc, argv, i, a, o.goal)) return false;
    } else if (a == "--warmup") {
      if (!take_value(argc, argv, i, a, v)) return false;
      if (!parse_int(v, o.warmup_s)) {
        std::fprintf(stderr, "--warmup 取值非法：%s\n", v.c_str());
        return false;
      }
    } else if (a == "--model") {
      if (!take_value(argc, argv, i, a, o.model)) return false;
    } else if (a == "--approval") {
      if (!take_value(argc, argv, i, a, o.approval)) return false;
      if (o.approval != "ask" && o.approval != "auto" && o.approval != "deny") {
        std::fprintf(stderr, "--approval 只能是 ask|auto|deny，收到：%s\n", o.approval.c_str());
        return false;
      }
    } else if (a == "--allow-dangerous") {
      if (!take_value(argc, argv, i, a, o.allow_dangerous)) return false;
      if (o.allow_dangerous == "none") o.allow_dangerous.clear();
    } else if (a == "--max-turns") {
      if (!take_value(argc, argv, i, a, v)) return false;
      if (!parse_int(v, o.budget.max_turns)) {
        std::fprintf(stderr, "--max-turns 取值非法：%s\n", v.c_str());
        return false;
      }
    } else if (a == "--max-yuan") {
      if (!take_value(argc, argv, i, a, v)) return false;
      if (!parse_double(v, o.budget.max_yuan)) {
        std::fprintf(stderr, "--max-yuan 取值非法：%s\n", v.c_str());
        return false;
      }
    } else if (a == "--max-wall") {
      if (!take_value(argc, argv, i, a, v)) return false;
      if (!parse_int(v, o.budget.max_wall_s)) {
        std::fprintf(stderr, "--max-wall 取值非法：%s\n", v.c_str());
        return false;
      }
    } else {
      std::fprintf(stderr, "未知参数：%s\n", a.c_str());
      print_usage();
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------- 事件与结果

void print_event(const agent::Event& e) {
  if (e.type == "llm.response") return;
  std::printf("[event] %-18s %s\n", e.type.c_str(), e.data.dump().c_str());
}

void print_result(const agent::RunResult& res) {
  std::printf("\n=== result ===\n");
  std::printf("stage     : %s\n", agent::stage_name(res.final_stage));
  std::printf("turns     : %d\n", res.turns);
  std::printf("cost      : %.4f CNY（估算，非账单）\n", res.cost_yuan);
  std::printf("root_cause: %s\n", res.diagnosis.root_cause.c_str());
  for (const auto& e : res.diagnosis.evidence) std::printf("evidence  : %s\n", e.c_str());
  if (!res.error.empty()) std::printf("error     : %s\n", res.error.c_str());
}

// ---------------------------------------------------------------- demo

// 脚本化 demo 用的 mock 工具。schema 必须与 src/pipeline/tools.cpp 的 build_real_tools
// 保持一致（枚举/范围/描述），否则 demo 证明的只是状态机，不是真实工具链。
struct ToolState {
  int metrics_calls = 0;
  int set_param_calls = 0;
  int inject_calls = 0;

  void build(tool::Registry& reg) {
    reg.add(tool::ToolSpec{.name = "query_metrics",
                           .desc = "查询媒体管线指标（六段分位 + 帧统计，预聚合）",
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
                           .desc = "修改媒体管线参数（生效方式为重启管线，会造成短暂断流）",
                           .risk = tool::Risk::Mutating,
                           .params = {{.name = "key",
                                       .type = "string",
                                       .desc = "参数名",
                                       .required = true,
                                       .enum_values = {"bitrate_kbps", "gop", "preset", "transcode"},
                                       .has_range = false,
                                       .min = 0,
                                       .max = 0},
                                      {.name = "value",
                                       .type = "string",
                                       .desc = "目标值",
                                       .required = true,
                                       .enum_values = {},
                                       .has_range = false,
                                       .min = 0,
                                       .max = 0}}},
            [this](const json& a) {
              ++set_param_calls;
              return tool::ToolResult{true, "ok", a};
            });

    reg.add(tool::ToolSpec{.name = "inject_fault",
                           .desc = "注入网络损伤（危险动作；取消注入用 value=0）",
                           .risk = tool::Risk::Dangerous,
                           .params = {{.name = "kind",
                                       .type = "string",
                                       .desc = "类型",
                                       .required = true,
                                       .enum_values = {"drop"},
                                       .has_range = false,
                                       .min = 0,
                                       .max = 0},
                                      {.name = "value",
                                       .type = "number",
                                       .desc = "丢包百分比 0-100",
                                       .required = true,
                                       .enum_values = {},
                                       .has_range = true,
                                       .min = 0,
                                       .max = 100}}},
            [this](const json& a) {
              ++inject_calls;
              return tool::ToolResult{true, "injected", a};
            });

    reg.add(tool::ToolSpec{.name = "fetch_observation",
                           .desc = "获取观测（日志尾部 / 进程状态 / 帧统计）",
                           .risk = tool::Risk::ReadOnly,
                           .params = {{.name = "source",
                                       .type = "string",
                                       .desc = "来源",
                                       .required = true,
                                       .enum_values = {"logs", "procs", "frames"},
                                       .has_range = false,
                                       .min = 0,
                                       .max = 0},
                                      {.name = "tail",
                                       .type = "integer",
                                       .desc = "日志行数",
                                       .required = false,
                                       .enum_values = {},
                                       .has_range = true,
                                       .min = 1,
                                       .max = 200}}},
            [](const json& a) {
              return tool::ToolResult{true, "observation", json{{"source", a["source"]}}};
            });
  }
};

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

  print_result(res);
  std::printf("tools     : metrics=%d set_param=%d inject=%d\n", state.metrics_calls,
              state.set_param_calls, state.inject_calls);
  std::printf("verify    : %s\n", cfg.verify_sla ? "measured" : "model_claimed");
  return res.ok() ? 0 : 1;
}

// ---------------------------------------------------------------- run（真实管线）

// warmup 期间的信号处理：只置标志，让用户 Ctrl-C 能干净收尾（不杀到一半留孤儿 cued）
volatile std::sig_atomic_t g_interrupted = 0;
void on_interrupt(int) { g_interrupted = 1; }

// 只在 warmup 期间接管 SIGINT/SIGTERM，之后恢复默认——agent 阶段的 Ctrl-C 行为不变
bool warmup_wait(int seconds) {
  if (seconds <= 0) return true;
  struct sigaction sa {};
  sa.sa_handler = on_interrupt;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  struct sigaction old_int {};
  struct sigaction old_term {};
  sigaction(SIGINT, &sa, &old_int);
  sigaction(SIGTERM, &sa, &old_term);
  g_interrupted = 0;

  std::fprintf(stderr, "[cue] warmup %d s：请在此期间开始推流（Ctrl-C 可跳过）\n", seconds);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (!g_interrupted && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  sigaction(SIGINT, &old_int, nullptr);
  sigaction(SIGTERM, &old_term, nullptr);
  return !g_interrupted;
}

int run_real(const RunOptions& o) {
  const char* key = std::getenv("CUE_API_KEY");
  if (!key || !*key) {
    std::fprintf(stderr, "CUE_API_KEY 未设置（用环境变量传入，不要写进文件）\n");
    return 2;
  }
  const char* base = std::getenv("CUE_BASE_URL");
  const char* model_env = std::getenv("CUE_MODEL");
  const std::string model =
      !o.model.empty() ? o.model : (model_env && *model_env ? model_env : "deepseek-chat");

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
  if (!warmup_wait(o.warmup_s)) {
    std::fprintf(stderr, "[cue] warmup 被中断，清理管线后退出\n");
    pipe.stop();
    return 130;
  }

  tool::Registry reg;
  cue::pipeline::build_real_tools(reg, pipe, nullptr);

  llm::OpenAiCompatibleClient client({base && *base ? base : "https://api.deepseek.com", key, model, 60});

  agent::Runner::Config cfg;
  cfg.model = model;
  cfg.system_prompt = cue::pipeline::real_ops_system_prompt();
  cfg.ctx.mode = o.approval == "deny"   ? tool::ApprovalMode::Deny
                 : o.approval == "ask" ? tool::ApprovalMode::Ask
                                       : tool::ApprovalMode::Auto;
  cfg.ctx.allowed_dangerous = split_commas(o.allow_dangerous);
  if (cfg.ctx.mode == tool::ApprovalMode::Ask) {
    std::fprintf(stderr, "[cue] --approval ask 需要审批回调（M5 审批 UI），当前未提供 → 非只读工具会被拒\n");
  }
  if (cfg.ctx.allowed_dangerous.empty()) {
    std::fprintf(stderr, "[cue] 未放行任何危险的工具（--allow-dangerous none）\n");
  }

  agent::Runner runner(client, reg, cfg);
  const auto res = runner.run(o.goal, o.budget, print_event);

  print_result(res);
  std::printf("dangerous : %s\n", o.allow_dangerous.empty() ? "(none)" : o.allow_dangerous.c_str());

  pipe.stop();
  return res.ok() ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string cmd = argc > 1 ? argv[1] : "demo";
  if (cmd == "demo") {
    if (argc > 2) {
      for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "-h" || std::string(argv[i]) == "--help") {
          print_usage();
          return 0;
        }
      }
      std::fprintf(stderr, "demo 不接受参数：%s\n", argv[2]);
      print_usage();
      return 2;
    }
    return run_demo();
  }
  if (cmd == "run") {
    RunOptions o;
    if (!parse_run_args(argc, argv, o)) return 2;
    if (o.help) {
      print_usage();
      return 0;
    }
    return run_real(o);
  }
  if (cmd == "help" || cmd == "-h" || cmd == "--help") {
    print_usage();
    return 0;
  }
  std::fprintf(stderr, "未知命令：%s\n", cmd.c_str());
  print_usage();
  return 2;
}
