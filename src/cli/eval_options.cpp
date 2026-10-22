// cue-eval 选项解析：配置文件 → 命令行覆盖（FR-OBS.5）
#include "cue/cli/eval_options.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>

namespace cue::cli {
namespace {

constexpr const char* kProg = "cue-eval";

config::Error cli_error(std::string message) { return config::Error{kProg, 0, 0, std::move(message)}; }

bool take_value(int argc, char** argv, int& i, std::string& out) {
  const std::string a = argv[i];
  const auto eq = a.find('=');
  if (eq != std::string::npos) {
    out = a.substr(eq + 1);
    return true;
  }
  if (i + 1 >= argc) return false;
  out = argv[++i];
  return true;
}

std::string_view opt_name(const char* arg) {
  std::string_view a(arg);
  const auto eq = a.find('=');
  return eq == std::string_view::npos ? a : a.substr(0, eq);
}

int atoi_or(std::string_view s, int fallback) {
  if (s.empty()) return fallback;
  std::string tmp(s);
  return std::atoi(tmp.c_str());
}

double atof_or(std::string_view s, double fallback) {
  if (s.empty()) return fallback;
  std::string tmp(s);
  return std::atof(tmp.c_str());
}

// ---- 配置文件里的 eval 段 ----

std::expected<void, config::Error> apply_eval_config(const config::Doc& doc, EvalOptions& o) {
  static constexpr std::string_view kKnown[] = {
      "suite_path",   "agent_mode",      "repeat",       "max_yuan",
      "out_dir",      "warmup_s",        "cued_bin",     "probe_bin",
      "endpoint",     "model",           "yuan_per_mtok", "llm_mode",
      "cassette_dir", "record",          "record_tools_dir",
      "replay_tools_dir", "sla_settle_ms"};
  if (auto ok = doc.reject_unknown_keys("eval", kKnown); !ok) return std::unexpected(ok.error());

  auto& cfg = o.cfg;
  if (doc.contains("eval.suite_path")) {
    auto v = doc.get_existing_file("eval.suite_path");
    if (!v) return std::unexpected(v.error());
    cfg.suite_path = *v;
    o.suite_set = true;
  }
  if (doc.contains("eval.agent_mode")) {
    static constexpr std::string_view kModes[] = {"scripted", "real"};
    auto v = doc.get_enum("eval.agent_mode", kModes);
    if (!v) return std::unexpected(v.error());
    cfg.agent_mode = *v;
    o.mode_set = true;
  }
  if (doc.contains("eval.repeat")) {
    auto v = doc.get_int("eval.repeat", 1, 100);
    if (!v) return std::unexpected(v.error());
    cfg.repeat = static_cast<int>(*v);
    o.repeat_set = true;
  }
  if (doc.contains("eval.max_yuan")) {
    auto v = doc.get_double("eval.max_yuan", 0.0, 1e9);
    if (!v) return std::unexpected(v.error());
    cfg.max_yuan = *v;
  }
  if (doc.contains("eval.out_dir")) {
    auto v = doc.get_string("eval.out_dir");
    if (!v) return std::unexpected(v.error());
    cfg.out_dir = *v;  // 目录可以先不存在（评测会创建）
    o.out_set = true;
  }
  if (doc.contains("eval.warmup_s")) {
    auto v = doc.get_int("eval.warmup_s", 0, 3600);
    if (!v) return std::unexpected(v.error());
    cfg.warmup_s = static_cast<int>(*v);
  }
  if (doc.contains("eval.cued_bin")) {
    auto v = doc.get_existing_file("eval.cued_bin");
    if (!v) return std::unexpected(v.error());
    cfg.cued_bin = *v;
  }
  if (doc.contains("eval.probe_bin")) {
    auto v = doc.get_string("eval.probe_bin");
    if (!v) return std::unexpected(v.error());
    if (!v->empty()) {
      std::error_code ec;
      if (!std::filesystem::is_regular_file(*v, ec)) {
        return std::unexpected(doc.error("eval.probe_bin",
                                         "eval.probe_bin 指向的文件不存在：" + *v));
      }
    }
    cfg.probe_bin = *v;
  }
  if (doc.contains("eval.endpoint")) {
    auto v = doc.get_string("eval.endpoint");
    if (!v) return std::unexpected(v.error());
    if (!v->empty() && v->rfind("http://", 0) != 0 && v->rfind("https://", 0) != 0) {
      return std::unexpected(doc.error("eval.endpoint",
                                       "eval.endpoint 需要 http(s):// 开头的 URL，收到 " + *v));
    }
    cfg.endpoint = *v;
  }
  if (doc.contains("eval.model")) {
    auto v = doc.get_string("eval.model");
    if (!v) return std::unexpected(v.error());
    cfg.model = *v;
  }
  if (doc.contains("eval.yuan_per_mtok")) {
    auto v = doc.get_double("eval.yuan_per_mtok", 0.0, 1e6);
    if (!v) return std::unexpected(v.error());
    cfg.yuan_per_mtok = *v;
  }
  if (doc.contains("eval.llm_mode")) {
    static constexpr std::string_view kLlms[] = {"replay", "live"};
    auto v = doc.get_enum("eval.llm_mode", kLlms);
    if (!v) return std::unexpected(v.error());
    cfg.llm_mode = *v;
  }
  if (doc.contains("eval.cassette_dir")) {
    auto v = doc.get_string("eval.cassette_dir");
    if (!v) return std::unexpected(v.error());
    cfg.cassette_dir = *v;
  }
  if (doc.contains("eval.record")) {
    auto v = doc.get_bool("eval.record");
    if (!v) return std::unexpected(v.error());
    cfg.record = *v;
    o.record_flag = *v;
  }
  if (doc.contains("eval.record_tools_dir")) {
    auto v = doc.get_string("eval.record_tools_dir");
    if (!v) return std::unexpected(v.error());
    cfg.record_tools_dir = *v;
  }
  if (doc.contains("eval.replay_tools_dir")) {
    auto v = doc.get_string("eval.replay_tools_dir");
    if (!v) return std::unexpected(v.error());
    cfg.replay_tools_dir = *v;
  }
  if (doc.contains("eval.sla_settle_ms")) {
    auto v = doc.get_int("eval.sla_settle_ms", 0, 600000);
    if (!v) return std::unexpected(v.error());
    cfg.sla_settle_ms = static_cast<int>(*v);
  }
  // 配置文件自身内部的互斥项：在行号还能对上原文时先拒（FR-OBS.5 要的是行号）。
  if (auto ok = doc.forbid_together("eval.record_tools_dir", "eval.replay_tools_dir"); !ok) {
    return std::unexpected(ok.error());
  }
  return {};
}

}  // namespace

void print_eval_usage() {
  std::printf(
      "cue-eval — Cue 评测 CLI\n"
      "\n"
      "  cue-eval --suite <path> [--mode scripted|real] [--repeat k] [--max-yuan N]\n"
      "           [--out dir] [--warmup s] [--model name] [--cued path] [--probe path]\n"
      "           [--sla-settle-ms N] [--endpoint URL]\n"
      "           [--replay|--live] [--record] [--cassette-dir dir]\n"
      "  cue-eval --gen <matrix.json> [--out dir]           # 矩阵批量生成 case（默认写到\n"
      "                  suites/generated/；拒绝覆盖非生成物）\n"
      "  cue-eval --resume <run_id> [--out dir]             # 从 state.jsonl 续跑\n"
      "  cue-eval --diff <run_a> <run_b> [--out dir]        # case 级回归对比\n"
      "\n"
      "  --config PATH  读 JSON 配置文件（先应用，命令行随后覆盖；非法配置带行号拒绝启动）\n"
      "  --endpoint URL 被测管线改为 HTTP 服务（实现 GET /api/state + POST /api/action），\n"
      "                 不再起 cued；warmup 等待也跳过（假域无需等外部推流）。\n"
      "  --baseline <run_id>  指定回归清单的对照 run（report.md 的「回归清单」节）。\n"
      "                 默认自动找 --out 下同 suite / mode / repeat 的最近一个 run；\n"
      "                 找不到就写「本次没有 baseline，跳过」，绝不拿别的 run 冒充。\n"
      "  --no-baseline  关闭自动 baseline 发现（只跑本次、不做对比）。\n"
      "  --record-tools DIR  把每个 case 的工具观测录到 DIR/<case_id>.tools.jsonl；\n"
      "  --replay-tools DIR  只回放该录制：不 spawn cued、不连 endpoint，miss 明确报错。\n"
      "                      两者互斥；--replay-tools 不能与 --endpoint 同时给。\n"
      "  --probe path    cue-probe 二进制（默认与 --cued 同目录的 cue-probe）。探针由\n"
      "                  harness 托管：起不来/没有样本时该 case 记 run_error=probe_unavailable，\n"
      "                  端到端 SLA 绝不退回 cued 指标判分。\n"
      "  --suite 缺省为 suites/m4-diagnosis.json；scripted 只是引擎自检，不判模型。\n"
      "  real 模式的 LLM 传输默认 replay：只读 cassette，零成本；缺录制直接报错\n"
      "  （run_error=cassette_miss，不静默走网络）。--live 才用真实网络（需 CUE_API_KEY /\n"
      "  CUE_BASE_URL），配 --record 把每次成功调用录进 --cassette-dir（默认 cassettes/）。\n"
      "  --replay / --live 隐含 --mode real，除非显式 --mode scripted。\n"
      "  CUE_RECORD=1 / CUE_CASSETTE_DIR 可替代对应开关。\n"
      "  单价可用 CUE_YUAN_PER_MTOK 覆盖（估算，不是账单）。\n"
      "  --log-file PATH / --log-level LEVEL / --log-text / --log-json：结构化日志开关\n"
      "                  （默认 JSON-line 写 stderr；--log-file 额外写轮转文件）。\n"
      "\n"
      "  退出码：0 全过 / 1 有未通过 / 2 用法或无法产出 / 3 diff 不可比 /\n"
      "          4 diff case 集变动 / 5 写入失败 / 130 被取消\n");
}

std::expected<EvalOptions, config::Error> parse_eval_options(int argc, char** argv) {
  EvalOptions o;
  std::optional<config::Doc> doc;

  // 容错：不带任何参数时走默认 suite（原行为）；--config 先读、命令行再覆盖。
  for (int i = 1; i < argc; ++i) {
    if (opt_name(argv[i]) == "--config") {
      std::string v;
      if (!take_value(argc, argv, i, v)) return std::unexpected(cli_error("--config 缺少取值"));
      o.config_path = v;
    }
  }
  if (!o.config_path.empty()) {
    auto loaded = config::Doc::load(o.config_path);
    if (!loaded) return std::unexpected(loaded.error());
    doc = std::move(*loaded);
    if (auto ok = apply_eval_config(*doc, o); !ok) return std::unexpected(ok.error());
    if (auto ok = config::apply_log_options(*doc, o.log); !ok) return std::unexpected(ok.error());
  }

  auto& cfg = o.cfg;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const std::string_view name = opt_name(argv[i]);
    std::string v;
    if (name == "--suite") {
      if (!take_value(argc, argv, i, cfg.suite_path)) {
        return std::unexpected(cli_error("--suite 缺少取值"));
      }
      o.suite_set = true;
    } else if (name == "--mode") {
      if (!take_value(argc, argv, i, cfg.agent_mode)) {
        return std::unexpected(cli_error("--mode 缺少取值"));
      }
      o.mode_set = true;
    } else if (name == "--replay") {
      o.replay_flag = true;
    } else if (name == "--live") {
      o.live_flag = true;
    } else if (name == "--record") {
      o.record_flag = true;
      o.record_cli = true;
    } else if (name == "--cassette-dir") {
      if (!take_value(argc, argv, i, cfg.cassette_dir) || cfg.cassette_dir.empty()) {
        return std::unexpected(cli_error("--cassette-dir 需要目录名"));
      }
    } else if (name == "--repeat") {
      if (!take_value(argc, argv, i, v)) return std::unexpected(cli_error("--repeat 缺少取值"));
      cfg.repeat = std::max(1, atoi_or(v, 0));
      o.repeat_set = true;
    } else if (name == "--baseline") {
      if (!take_value(argc, argv, i, o.baseline_id) || o.baseline_id.empty()) {
        return std::unexpected(cli_error("--baseline 需要 run_id"));
      }
    } else if (name == "--no-baseline") {
      o.no_baseline = true;
    } else if (name == "--max-yuan") {
      if (!take_value(argc, argv, i, v)) return std::unexpected(cli_error("--max-yuan 缺少取值"));
      cfg.max_yuan = atof_or(v, 0.0);
    } else if (name == "--gen") {
      if (!take_value(argc, argv, i, o.gen_path) || o.gen_path.empty()) {
        return std::unexpected(cli_error("--gen 需要矩阵文件路径"));
      }
    } else if (name == "--out") {
      if (!take_value(argc, argv, i, cfg.out_dir)) return std::unexpected(cli_error("--out 缺少取值"));
      o.out_set = true;
    } else if (name == "--warmup") {
      if (!take_value(argc, argv, i, v)) return std::unexpected(cli_error("--warmup 缺少取值"));
      cfg.warmup_s = std::max(0, atoi_or(v, 0));
    } else if (name == "--sla-settle-ms") {
      if (!take_value(argc, argv, i, v)) {
        return std::unexpected(cli_error("--sla-settle-ms 缺少取值"));
      }
      cfg.sla_settle_ms = std::max(0, atoi_or(v, 0));
    } else if (name == "--model") {
      if (!take_value(argc, argv, i, cfg.model)) return std::unexpected(cli_error("--model 缺少取值"));
    } else if (name == "--cued") {
      if (!take_value(argc, argv, i, cfg.cued_bin)) return std::unexpected(cli_error("--cued 缺少取值"));
    } else if (name == "--probe") {
      if (!take_value(argc, argv, i, cfg.probe_bin)) return std::unexpected(cli_error("--probe 缺少取值"));
    } else if (name == "--endpoint") {
      if (!take_value(argc, argv, i, cfg.endpoint)) {
        return std::unexpected(cli_error("--endpoint 缺少取值"));
      }
      if (cfg.endpoint.rfind("http://", 0) != 0 && cfg.endpoint.rfind("https://", 0) != 0) {
        return std::unexpected(cli_error("--endpoint 需要 http(s):// 开头的 URL（收到：" +
                                         (cfg.endpoint.empty() ? std::string("(空)") : cfg.endpoint) +
                                         "）"));
      }
    } else if (name == "--record-tools") {
      if (!take_value(argc, argv, i, cfg.record_tools_dir) || cfg.record_tools_dir.empty()) {
        return std::unexpected(cli_error("--record-tools 需要目录"));
      }
    } else if (name == "--replay-tools") {
      if (!take_value(argc, argv, i, cfg.replay_tools_dir) || cfg.replay_tools_dir.empty()) {
        return std::unexpected(cli_error("--replay-tools 需要目录"));
      }
    } else if (name == "--resume") {
      if (!take_value(argc, argv, i, cfg.resume_run_id)) {
        return std::unexpected(cli_error("--resume 缺少取值"));
      }
    } else if (name == "--diff") {
      if (!take_value(argc, argv, i, o.diff_a)) return std::unexpected(cli_error("--diff 缺少取值"));
      if (!take_value(argc, argv, i, o.diff_b)) return std::unexpected(cli_error("--diff 缺少取值"));
    } else if (name == "--config") {
      if (!take_value(argc, argv, i, v)) return std::unexpected(cli_error("--config 缺少取值"));
    } else if (name == "--log-file") {
      if (!take_value(argc, argv, i, v)) return std::unexpected(cli_error("--log-file 缺少取值"));
      o.log.file = v;
    } else if (name == "--log-level") {
      if (!take_value(argc, argv, i, v)) return std::unexpected(cli_error("--log-level 缺少取值"));
      cue::log::Level lvl{};
      if (!cue::log::parse_level(v, lvl)) {
        return std::unexpected(cli_error("--log-level 只能是 debug|info|warn|error，收到：" + v));
      }
      o.log.level = v;
    } else if (name == "--log-json") {
      o.log.json = true;
    } else if (name == "--log-text") {
      o.log.json = false;
    } else if (name == "--help" || name == "-h") {
      o.help = true;
      return o;
    } else {
      return std::unexpected(cli_error("未知参数: " + a));
    }
  }

  // 合并之后才判互斥：配置文件给了 A、命令行给了 B 也要被拒。
  if (!cfg.record_tools_dir.empty() && !cfg.replay_tools_dir.empty()) {
    if (doc && doc->contains("eval.record_tools_dir") && doc->contains("eval.replay_tools_dir")) {
      auto ok = doc->forbid_together("eval.record_tools_dir", "eval.replay_tools_dir");
      if (!ok) return std::unexpected(ok.error());
    }
    return std::unexpected(cli_error("--record-tools 与 --replay-tools 互斥"));
  }
  return o;
}

}  // namespace cue::cli
