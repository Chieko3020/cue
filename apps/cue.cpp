// cue — Agent Harness CLI
//   demo：脚本化 LLM 跑通状态机（不联网、不花钱）
//   run ：真实 LLM（DeepSeek 兼容端点）+ 真实 cued 管线
#include <cerrno>
#include <csignal>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cue/agent/agent.h"
#include "cue/llm/cassette.h"
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
      "  cue demo [--http URL [--record DIR] | --replay DIR]\n"
      "      脚本化 LLM 跑一次完整诊断（默认不联网、不花钱）。\n"
      "      --http   改用 OpenAI 兼容端点（可指向假端点，用于录制 cassette）\n"
      "      --record 把 --http 下每次成功调用录进 DIR（cassette）\n"
      "      --replay 从 DIR 的 cassette 回放（不联网，miss 直接报错）\n"
      "  cue run [--goal TEXT] [--warmup S] [--model NAME] [--approval ask|auto|deny]\n"
      "          [--allow-dangerous NAMES|none] [--max-turns N] [--max-yuan Y] [--max-wall S]\n"
      "      接真实 LLM 对真实 cued 管线跑一次诊断（M3b 已接入）。\n"
      "      环境变量：CUE_API_KEY（必需）、CUE_BASE_URL、CUE_MODEL；\n"
      "      CUE_RECORD=1 时把成功调用录成 cassette（目录 CUE_CASSETTE_DIR，默认 cassettes/）\n"
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
      if (!parse_int(v, o.budget.max_turns) || o.budget.max_turns < 1) {
        std::fprintf(stderr, "--max-turns 取值非法（需 >= 1）：%s\n", v.c_str());
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

struct DemoOptions {
  std::string http_base;   // 非空 = 走 OpenAI 兼容端点（可指向假端点）
  std::string record_dir;  // 非空 = 把 --http 的成功调用录成 cassette
  std::string replay_dir;  // 非空 = 用 CassetteClient 回放（不联网）
};

// demo 的脚本序列。--http 时由同序列的假端点按序返回，因此录/放能与默认 demo 对照。
std::vector<llm::ChatResponse> demo_script() {
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
  std::vector<llm::ChatResponse> script;
  script.push_back(call("c1", "query_metrics", json{{"window_ms", 5000}}));
  script.push_back(call("c2", "stage_complete", json{{"next", "diagnose"}}));
  script.push_back(call("c3", "stage_complete",
                        json{{"next", "repair"},
                             {"root_cause", "network_loss"},
                             {"evidence", {"nack_retransmit=42"}},
                             {"confidence", 0.8}}));
  script.push_back(call("c4", "pipeline_set_param", json{{"key", "bitrate_kbps"}, {"value", "800"}}));
  script.push_back(call("c5", "stage_complete", json{{"next", "verify"}}));
  script.push_back(call("c6", "query_metrics", json{{"window_ms", 5000}}));
  script.push_back(call("c7", "stage_complete", json{{"next", "done"}}));
  return script;
}

int run_demo(const DemoOptions& opt = {}) {
  ToolState state;
  tool::Registry reg;
  state.build(reg);

  std::unique_ptr<llm::ILlmClient> client;
  llm::CassetteClient* replay = nullptr;
  if (!opt.replay_dir.empty()) {
    llm::CassetteClient::Config rc;
    rc.dir = opt.replay_dir;
    rc.default_model = "scripted";
    auto c = std::make_unique<llm::CassetteClient>(rc);
    replay = c.get();
    client = std::move(c);
  } else if (!opt.http_base.empty()) {
    llm::OpenAiCompatibleClient::Config cc;
    cc.base_url = opt.http_base;
    const char* env_key = std::getenv("CUE_API_KEY");
    cc.api_key = (env_key && *env_key) ? env_key : "test-key";
    cc.model = "scripted";
    cc.timeout_s = 15;
    cc.max_retries = 0;
    cc.record_dir = opt.record_dir;
    client = std::make_unique<llm::OpenAiCompatibleClient>(cc);
  } else {
    auto scripted = std::make_unique<llm::ScriptedClient>();
    for (auto& r : demo_script()) scripted->push(std::move(r));
    client = std::move(scripted);
  }

  agent::Runner::Config cfg;
  cfg.model = "scripted";
  cfg.system_prompt =
      "你是 Cue 的媒体管线运维 agent。你只能通过工具观测与调整管线；"
      "结论必须引用真实观测作为证据；每个阶段结束时调用 stage_complete。";
  cfg.ctx.mode = tool::ApprovalMode::Auto;

  agent::Runner runner(*client, reg, cfg);
  const auto res = runner.run("把下行端到端 p99 压到 120ms 以下", agent::Budget{}, print_event);

  print_result(res);
  std::printf("tools     : metrics=%d set_param=%d inject=%d\n", state.metrics_calls,
              state.set_param_calls, state.inject_calls);
  std::printf("verify    : %s\n", cfg.verify_sla ? "measured" : "model_claimed");
  if (replay) {
    const auto st = replay->stats();
    std::printf("cassette  : replay dir=%s hits=%zu misses=%zu\n", opt.replay_dir.c_str(), st.hits,
                st.misses);
  } else if (!opt.http_base.empty()) {
    std::printf("llm       : http=%s", opt.http_base.c_str());
    if (!opt.record_dir.empty()) std::printf(" record=%s", opt.record_dir.c_str());
    std::printf("\n");
  }
  return res.ok() ? 0 : 1;
}

// ---------------------------------------------------------------- run（真实管线）

// run 全程的取消标志（SIGINT/SIGTERM）：handler 里只做一次原子 store，不碰 printf/malloc，
// 因此是 async-signal-safe 的。阻塞点（warmup 睡眠、curl 传输、工具之间）各自轮询它，
// 保证「用户已经放弃」不会被执行到底。
std::atomic<bool> g_cancel{false};

void on_cancel_signal(int) { g_cancel.store(true, std::memory_order_relaxed); }

// 只在 run 路径安装：demo 是一次性脚本执行，不需要改信号语义。
bool install_cancel_handlers() {
  struct sigaction sa {};
  sa.sa_handler = on_cancel_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;  // 不设 SA_RESTART：被中断的阻塞调用应看到 EINTR
  return sigaction(SIGINT, &sa, nullptr) == 0 && sigaction(SIGTERM, &sa, nullptr) == 0;
}

// warmup 与 agent 阶段共用同一个标志：Ctrl-C 在 warmup 里是「跳过等待」，
// 在 agent 阶段是「取消本次 run」，两者都不应该留下孤儿 cued。
bool warmup_wait(int seconds) {
  if (seconds <= 0) return !g_cancel.load();
  std::fprintf(stderr, "[cue] warmup %d s：请在此期间开始推流（Ctrl-C 可跳过）\n", seconds);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (!g_cancel.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  return !g_cancel.load();
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

  // 信号接管覆盖整个 run 生命周期（warmup + agent）：退出前必须收回 cued（AC-8）
  if (!install_cancel_handlers()) {
    std::fprintf(stderr, "安装 SIGINT/SIGTERM 处理器失败: %s\n", std::strerror(errno));
    return 1;
  }

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
    std::fprintf(stderr, "[cue] 已取消（warmup 阶段）：回收 cued 后退出\n");
    pipe.stop();
    return 130;
  }

  tool::Registry reg;
  cue::pipeline::build_real_tools(reg, pipe, nullptr);

  llm::OpenAiCompatibleClient::Config cc;
  cc.base_url = base && *base ? base : "https://api.deepseek.com";
  cc.api_key = key;
  cc.model = model;
  cc.timeout_s = 60;
  // 与 cue-eval 同一开关：CUE_RECORD=1 时把成功调用录进 CUE_CASSETTE_DIR（默认 cassettes/）
  const char* rec = std::getenv("CUE_RECORD");
  if (rec && *rec && std::strcmp(rec, "0") != 0) {
    const char* dir = std::getenv("CUE_CASSETTE_DIR");
    cc.record_dir = (dir && *dir) ? dir : "cassettes";
  }
  llm::OpenAiCompatibleClient client(cc);

  agent::Runner::Config cfg;
  cfg.model = model;
  cfg.cancel = &g_cancel;  // 每轮开始 / 每次 LLM 调用 / 工具之间都会检查
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
  // 先收子进程再打印：取消路径上 cued 的存活时间不计入「结果输出」这段
  pipe.stop();

  const bool cancelled = g_cancel.load();
  if (cancelled) {
    // 明确一行「已取消」而不是让用户从 error=cancelled 里猜（也不打印幻觉结果）
    std::fprintf(stderr, "[cue] 已取消：LLM 调用/工具执行已中断，cued 已回收\n");
  }
  print_result(res);
  std::printf("dangerous : %s\n", o.allow_dangerous.empty() ? "(none)" : o.allow_dangerous.c_str());

  return cancelled ? 130 : (res.ok() ? 0 : 1);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string cmd = argc > 1 ? argv[1] : "demo";
  if (cmd == "demo") {
    DemoOptions opt;
    for (int i = 2; i < argc; ++i) {
      const std::string a = argv[i];
      if (a == "-h" || a == "--help") {
        print_usage();
        return 0;
      }
      if (a == "--http") {
        if (!take_value(argc, argv, i, a, opt.http_base)) return 2;
      } else if (a == "--record") {
        if (!take_value(argc, argv, i, a, opt.record_dir)) return 2;
      } else if (a == "--replay") {
        if (!take_value(argc, argv, i, a, opt.replay_dir)) return 2;
      } else {
        std::fprintf(stderr, "demo 未知参数：%s\n", a.c_str());
        print_usage();
        return 2;
      }
    }
    if (!opt.replay_dir.empty() && (!opt.http_base.empty() || !opt.record_dir.empty())) {
      std::fprintf(stderr, "--replay 不能与 --http/--record 同时使用\n");
      return 2;
    }
    if (!opt.record_dir.empty() && opt.http_base.empty()) {
      std::fprintf(stderr, "--record 需要 --http（ScriptedClient 不录）\n");
      return 2;
    }
    return run_demo(opt);
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
