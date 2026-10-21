// cue-eval — 评测 CLI
// 用法：
//   cue-eval --suite suites/m4-diagnosis.json --mode scripted --repeat 1
//   cue-eval --suite ... --mode real --repeat 1 --warmup 6        # 需并行推流
//   cue-eval --resume run-<id>                                    # 断点续跑
//   cue-eval --diff run-<a> run-<b>                               # 回归对比
//
// 退出码：
//   0  全部通过（三级成功率与 pass^k 都是 100%，无 run_error）
//   1  有未通过（含 SLA 未达成、pass^k 不足、run_error）
//   2  用法错误 / 无法产出结果（state 写不进、run 被并发占用）
//   3  --diff 两边不可比（指纹/suite/mode/repeat 不同）
//   4  --diff case 集变动（仅 A 有 / 仅 B 有）
//   5  报告/state 写入失败
//   130 被 SIGINT/SIGTERM 取消
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "cue/eval/compare.h"
#include "cue/eval/eval.h"
#include "cue/eval/state.h"

namespace eval = cue::eval;

namespace {

std::atomic<bool> g_cancel{false};

void on_signal(int) { g_cancel.store(true); }

int usage() {
  std::printf(
      "cue-eval — Cue 评测 CLI\n"
      "\n"
      "  cue-eval --suite <path> [--mode scripted|real] [--repeat k] [--max-yuan N]\n"
      "           [--out dir] [--warmup s] [--model name] [--cued path]\n"
      "           [--endpoint URL]\n"
      "           [--replay|--live] [--record] [--cassette-dir dir]\n"
      "  cue-eval --resume <run_id> [--out dir]             # 从 state.jsonl 续跑\n"
      "  cue-eval --diff <run_a> <run_b> [--out dir]        # case 级回归对比\n"
      "\n"
      "  --endpoint URL 被测管线改为 HTTP 服务（实现 GET /api/state + POST /api/action），\n"
      "                 不再起 cued；warmup 等待也跳过（假域无需等外部推流）。\n"
      "  --record-tools DIR  把每个 case 的工具观测录到 DIR/<case_id>.tools.jsonl；\n"
  "  --replay-tools DIR  只回放该录制：不 spawn cued、不连 endpoint，miss 明确报错。\n"
  "                      两者互斥；--replay-tools 不能与 --endpoint 同时给。\n"
  "  --suite 缺省为 suites/m4-diagnosis.json；scripted 只是引擎自检，不判模型。\n"
      "  real 模式的 LLM 传输默认 replay：只读 cassette，零成本；缺录制直接报错\n"
      "  （run_error=cassette_miss，不静默走网络）。--live 才用真实网络（需 CUE_API_KEY /\n"
      "  CUE_BASE_URL），配 --record 把每次成功调用录进 --cassette-dir（默认 cassettes/）。\n"
      "  --replay / --live 隐含 --mode real，除非显式 --mode scripted。\n"
      "  CUE_RECORD=1 / CUE_CASSETTE_DIR 可替代对应开关。\n"
      "  单价可用 CUE_YUAN_PER_MTOK 覆盖（估算，不是账单）。\n"
      "\n"
      "  退出码：0 全过 / 1 有未通过 / 2 用法或无法产出 / 3 diff 不可比 /\n"
      "          4 diff case 集变动 / 5 写入失败 / 130 被取消\n");
  return 0;
}

std::string header_str(const nlohmann::json& h, const char* key) {
  if (!h.is_object() || !h.contains(key) || !h[key].is_string()) return {};
  return h[key].get<std::string>();
}

int header_int(const nlohmann::json& h, const char* key, int fallback) {
  if (!h.is_object() || !h.contains(key) || !h[key].is_number_integer()) return fallback;
  return h[key].get<int>();
}

int do_diff(const std::string& run_a, const std::string& run_b, const std::string& out_dir) {
  const std::string pa = out_dir + "/" + run_a + "/state.jsonl";
  const std::string pb = out_dir + "/" + run_b + "/state.jsonl";
  eval::RunInfo ia;
  eval::RunInfo ib;
  ia.run_id = run_a;
  ib.run_id = run_b;
  auto hdr_a = eval::read_state_header(pa);
  auto hdr_b = eval::read_state_header(pb);
  if (!hdr_a) {
    std::fprintf(stderr, "读取 A header 失败: %s\n", hdr_a.error().c_str());
    return 2;
  }
  if (!hdr_b) {
    std::fprintf(stderr, "读取 B header 失败: %s\n", hdr_b.error().c_str());
    return 2;
  }
  // 指纹/配置必须参与 diff：判分器升级后的差异不能被读成"已修复 N"（review-eval.md #16）
  ia.fingerprint = header_str(*hdr_a, "fingerprint");
  ib.fingerprint = header_str(*hdr_b, "fingerprint");
  ia.suite = header_str(*hdr_a, "suite");
  ib.suite = header_str(*hdr_b, "suite");
  ia.mode = header_str(*hdr_a, "mode");
  ib.mode = header_str(*hdr_b, "mode");
  ia.repeat = header_int(*hdr_a, "repeat", 1);
  ib.repeat = header_int(*hdr_b, "repeat", 1);

  auto a = eval::load_state(pa);
  if (!a) {
    std::fprintf(stderr, "读取 A 失败: %s\n", a.error().c_str());
    return 2;
  }
  auto b = eval::load_state(pb);
  if (!b) {
    std::fprintf(stderr, "读取 B 失败: %s\n", b.error().c_str());
    return 2;
  }
  const auto d = eval::diff_runs(ia, *a, ib, *b);
  const std::string md = eval::render_diff_markdown(d);
  const std::string path = out_dir + "/diff-" + run_a + "-" + run_b + ".md";
  std::ofstream out(path);
  if (!out) {
    std::fprintf(stderr, "无法写入 %s\n", path.c_str());
    return 5;
  }
  out << md;
  out.flush();
  if (!out) {
    std::fprintf(stderr, "写 diff 失败 %s\n", path.c_str());
    return 5;
  }
  std::printf("%s", md.c_str());
  std::printf("[eval] diff 已写入 %s\n", path.c_str());
  if (d.new_failures > 0) return 1;            // 有回归：CI 拦截
  if (!d.comparable) return 3;                 // 配置不同：差异不可归因
  if (d.only_a > 0 || d.only_b > 0) return 4;  // case 集变了：不能让 CI 静默通过
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  // Ctrl-C：置位取消标志，让评测把当前 case 的管线停掉再退出（AC-8：无残留子进程）
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  eval::RunConfig cfg;
  std::string diff_a, diff_b;
  bool suite_set = false;
  bool mode_set = false;
  bool repeat_set = false;
  bool replay_flag = false;
  bool live_flag = false;
  bool record_flag = false;
  bool record_cli = false;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(); };
    if (a == "--suite") {
      cfg.suite_path = next();
      suite_set = true;
    } else if (a == "--mode") {
      cfg.agent_mode = next();
      mode_set = true;
    } else if (a == "--replay") {
      replay_flag = true;
    } else if (a == "--live") {
      live_flag = true;
    } else if (a == "--record") {
      record_flag = true;
      record_cli = true;
    } else if (a == "--cassette-dir") {
      cfg.cassette_dir = next();
      if (cfg.cassette_dir.empty()) {
        std::fprintf(stderr, "--cassette-dir 需要目录名\n");
        return 2;
      }
    } else if (a == "--repeat") {
      cfg.repeat = std::max(1, std::atoi(next().c_str()));
      repeat_set = true;
    } else if (a == "--max-yuan") {
      cfg.max_yuan = std::atof(next().c_str());
    } else if (a == "--out") {
      cfg.out_dir = next();
    } else if (a == "--warmup") {
      cfg.warmup_s = std::max(0, std::atoi(next().c_str()));
    } else if (a == "--model") {
      cfg.model = next();
    } else if (a == "--cued") {
      cfg.cued_bin = next();
    } else if (a == "--endpoint") {
      cfg.endpoint = next();
      if (cfg.endpoint.rfind("http://", 0) != 0 && cfg.endpoint.rfind("https://", 0) != 0) {
        std::fprintf(stderr, "--endpoint 需要 http(s):// 开头的 URL（收到：%s）\n",
                     cfg.endpoint.empty() ? "(空)" : cfg.endpoint.c_str());
        return 2;
      }
    } else if (a == "--record-tools") {
      cfg.record_tools_dir = next();
      if (cfg.record_tools_dir.empty()) {
        std::fprintf(stderr, "--record-tools 需要目录\n");
        return 2;
      }
    } else if (a == "--replay-tools") {
      cfg.replay_tools_dir = next();
      if (cfg.replay_tools_dir.empty()) {
        std::fprintf(stderr, "--replay-tools 需要目录\n");
        return 2;
      }
    } else if (a == "--resume") {
      cfg.resume_run_id = next();
    } else if (a == "--diff") {
      diff_a = next();
      diff_b = next();
    } else if (a == "--help" || a == "-h") {
      return usage();
    } else {
      std::fprintf(stderr, "未知参数: %s\n", a.c_str());
      return 2;
    }
  }

  if (!diff_a.empty() || !diff_b.empty()) {
    if (diff_a.empty() || diff_b.empty()) {
      std::fprintf(stderr, "--diff 需要两个 run_id\n");
      return 2;
    }
    return do_diff(diff_a, diff_b, cfg.out_dir);
  }

  // AC-6：工具观测的录制/回放互斥；回放时没有要连的后端，与 --endpoint 同给会让
  // 「回放的是哪份观测」变得不可解释。
  if (!cfg.record_tools_dir.empty() && !cfg.replay_tools_dir.empty()) {
    std::fprintf(stderr, "--record-tools 与 --replay-tools 互斥\n");
    return 2;
  }
  if (!cfg.replay_tools_dir.empty() && !cfg.endpoint.empty()) {
    std::fprintf(stderr, "--replay-tools 不能与 --endpoint 同时使用（回放不连后端）\n");
    return 2;
  }

  // 单价：环境变量优先（报告里回显的单价必须是实际使用的那个）
  if (const char* price = std::getenv("CUE_YUAN_PER_MTOK"); price && *price) {
    const double v = std::atof(price);
    if (v > 0) cfg.yuan_per_mtok = v;
  }

  // ---- LLM 传输（SPEC §7.9）：real 默认 replay，--live 才联网 ----
  if (replay_flag && live_flag) {
    std::fprintf(stderr, "--replay 与 --live 互斥\n");
    return 2;
  }
  if (replay_flag && record_cli) {
    std::fprintf(stderr, "--replay 与 --record 不能同时使用（replay 只读 cassette）\n");
    return 2;
  }
  if (const char* dir = std::getenv("CUE_CASSETTE_DIR"); dir && *dir) cfg.cassette_dir = dir;
  if (const char* rec = std::getenv("CUE_RECORD"); rec && *rec && std::string(rec) != "0") {
    record_flag = true;
  }
  cfg.llm_mode = live_flag ? "live" : "replay";
  cfg.record = record_flag && !replay_flag;
  if (replay_flag && record_flag) {
    std::fprintf(stderr, "[eval] --replay 忽略 CUE_RECORD（回放不写 cassette）\n");
  }
  // 两个传输开关都隐含「跑 real agent」；显式 --mode scripted 时只有 replay 可降级为无操作。
  if (!mode_set && (live_flag || replay_flag)) cfg.agent_mode = "real";
  if (live_flag && cfg.agent_mode == "scripted") {
    std::fprintf(stderr, "--live 只能配 --mode real（scripted 不调模型）\n");
    return 2;
  }
  if (replay_flag && cfg.agent_mode == "scripted") {
    std::fprintf(stderr, "[eval] --replay 对 scripted 模式无影响（ScriptedClient 本就不联网）\n");
  }
  if (cfg.record && cfg.llm_mode != "live") {
    if (cfg.agent_mode == "real") {
      std::fprintf(stderr, "录制需要 --live：replay 只读 cassette，不会产生新录制\n");
      return 2;
    }
    // 环境变量 CUE_RECORD=1 可能是全局设置的：scripted 下静默降级并说明，不打乱自检
    std::fprintf(stderr, "[eval] %s 模式下不录制（ScriptedClient 不录）；要录制请用 --live --record\n",
                 cfg.agent_mode.c_str());
    cfg.record = false;
  }

  if (cfg.agent_mode != "scripted" && cfg.agent_mode != "real") {
    std::fprintf(stderr, "--mode 只能是 scripted 或 real（收到 %s）\n", cfg.agent_mode.c_str());
    return 2;
  }

  if (!cfg.resume_run_id.empty()) {
    const std::string state = cfg.out_dir + "/" + cfg.resume_run_id + "/state.jsonl";
    if (!std::filesystem::exists(state)) {
      std::fprintf(stderr, "续跑失败：%s 不存在（用新 run 请去掉 --resume）\n", state.c_str());
      return 2;
    }
    // 续跑必须沿用原 run 的 suite/mode/repeat：显式传入却与 header 不一致时直接拒绝，
    // 否则会拿新配置去撞旧指纹，缓存全废还报出"复用"（review-eval.md #20）
    auto hdr = eval::read_state_header(state);
    if (!hdr) {
      std::fprintf(stderr, "续跑失败：%s\n", hdr.error().c_str());
      return 2;
    }
    const std::string h_suite = header_str(*hdr, "suite");
    const std::string h_mode = header_str(*hdr, "mode");
    const int h_repeat = header_int(*hdr, "repeat", cfg.repeat);
    if (!h_suite.empty()) {
      if (suite_set && cfg.suite_path != h_suite) {
        std::fprintf(stderr, "续跑失败：--suite=%s 与原 run 的 %s 不一致\n", cfg.suite_path.c_str(),
                     h_suite.c_str());
        return 2;
      }
      cfg.suite_path = h_suite;
    }
    if (mode_set && cfg.agent_mode != h_mode) {
      std::fprintf(stderr, "续跑失败：--mode=%s 与原 run 的 %s 不一致\n", cfg.agent_mode.c_str(),
                   h_mode.c_str());
      return 2;
    }
    cfg.agent_mode = h_mode.empty() ? cfg.agent_mode : h_mode;
    // 原 run 的 LLM 传输必须一致：replay 续进 live run 会混入联网调用（重新计费），
    // live 续进 replay run 则会因为 cassette 不存在而整轮 miss。
    const std::string h_llm = header_str(*hdr, "llm");
    if (!h_llm.empty() && h_llm != cfg.llm_mode) {
      std::fprintf(stderr,
                   "续跑失败：原 run 的 LLM 模式为 %s，当前 %s（要联网请显式 --live，会计费）\n",
                   h_llm.c_str(), cfg.llm_mode.c_str());
      return 2;
    }
    if (repeat_set && cfg.repeat != h_repeat) {
      std::fprintf(stderr, "续跑失败：--repeat=%d 与原 run 的 %d 不一致\n", cfg.repeat, h_repeat);
      return 2;
    }
    cfg.repeat = std::max(1, h_repeat);
    std::printf("[eval] 续跑沿用原 run 配置：suite=%s mode=%s repeat=%d\n", cfg.suite_path.c_str(),
                cfg.agent_mode.c_str(), cfg.repeat);
  }

  if (!suite_set && cfg.suite_path.empty()) {
    // 默认给诊断集（不是 m2 的 manual_only 集）：仍会打 scripted banner，
    // 避免"没说跑哪个 suite"时拿一份不可判的集糊出 100%
    cfg.suite_path = "suites/m4-diagnosis.json";
    std::fprintf(stderr, "[eval] 未指定 --suite，使用默认 %s（scripted 只是引擎自检）\n",
                 cfg.suite_path.c_str());
  }

  auto cases = eval::load_suite(cfg.suite_path);
  if (!cases) {
    std::fprintf(stderr, "加载 suite 失败: %s\n", cases.error().c_str());
    return 1;
  }
  std::printf("[eval] suite=%s cases=%zu mode=%s repeat=%d\n", cfg.suite_path.c_str(), cases->size(),
              cfg.agent_mode.c_str(), cfg.repeat);
  if (cfg.agent_mode == "real") {
    std::printf("[eval] LLM=%s cassette=%s%s\n", cfg.llm_mode.c_str(), cfg.cassette_dir.c_str(),
                cfg.record ? "（录制中）" : "");
  }
  if (!cfg.record_tools_dir.empty()) {
    std::printf("[eval] 工具观测录制到 %s（miss 会报错）\n", cfg.record_tools_dir.c_str());
  }
  if (!cfg.replay_tools_dir.empty()) {
    std::printf("[eval] 工具观测回放 %s（不 spawn cued、不连网络）\n",
                cfg.replay_tools_dir.c_str());
  }

  cfg.cancel = &g_cancel;
  eval::Engine engine(cfg);
  const auto report = engine.run(*cases);
  if (!report.fatal_error.empty()) {
    // 不能写 report.md：resume 失败时会把原 run 的报告覆盖成 0 分（review-eval.md #8）
    std::fprintf(stderr, "[eval] 未产出结果：%s\n", report.fatal_error.c_str());
    return 2;
  }
  const std::string md = engine.render_markdown(report);

  std::error_code ec;
  std::filesystem::create_directories(cfg.out_dir + "/" + report.run_id, ec);
  const std::string report_path = cfg.out_dir + "/" + report.run_id + "/report.md";
  {
    std::ofstream out(report_path);
    if (!out) {
      std::fprintf(stderr, "无法写入 %s\n", report_path.c_str());
      return 5;
    }
    out << md;
    out.flush();
    if (!out) {
      std::fprintf(stderr, "写 %s 失败\n", report_path.c_str());
      return 5;
    }
  }
  std::printf("%s\n", md.c_str());
  if (g_cancel.load()) {
    std::fprintf(stderr, "[eval] 已按信号取消（已完成部分已落盘，可 --resume）\n");
    return 130;
  }
  if (report.io_errors > 0) return 5;
  if (report.scored == 0) return 1;
  // 截断（成本闸门/取消）不是"全过"：剩余 case 没跑，CI 不能绿灯
  if (report.truncated) return 1;
  // 退出码必须覆盖三级成功率与 pass^k：只看 root/evidence 时 SLA 全灭也会返回 0（假绿灯）
  return (report.run_errors == 0 && report.root_rate >= 1.0 && report.evidence_rate >= 1.0 &&
          report.sla_rate >= 1.0 && report.pass_k >= 1.0)
             ? 0
             : 1;
}
