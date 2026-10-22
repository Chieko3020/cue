// cue-eval — 评测 CLI
// 用法：
//   cue-eval --suite suites/m4-diagnosis.json --mode scripted --repeat 1
//   cue-eval --suite ... --mode real --repeat 1 --warmup 6        # 需并行推流
//   cue-eval --gen suites/matrices/fault-matrix.json              # 矩阵生成 case（FR-EVAL.5）
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
#include <vector>

#include "cue/eval/compare.h"
#include "cue/eval/eval.h"
#include "cue/eval/generator.h"
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
      "           [--out dir] [--warmup s] [--model name] [--cued path] [--probe path]\n"
      "           [--sla-settle-ms N] [--endpoint URL]\n"
      "           [--replay|--live] [--record] [--cassette-dir dir]\n"
      "  cue-eval --gen <matrix.json> [--out dir]           # 矩阵批量生成 case（默认写到\n"
      "                  suites/generated/；拒绝覆盖非生成物）\n"
      "  cue-eval --resume <run_id> [--out dir]             # 从 state.jsonl 续跑\n"
      "  cue-eval --diff <run_a> <run_b> [--out dir]        # case 级回归对比\n"
      "\n"
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

// 回归清单里一条 case 的文字口径（与 compare.cpp 的 agg_text 同一套语义）
std::string case_agg_text(const eval::CaseAgg& g) {
  if (g.attempts == 0) return "—";
  std::string s = std::to_string(g.passes) + "/" + std::to_string(g.attempts) + " 过";
  if (g.run_errors > 0) s += "，run_error " + std::to_string(g.run_errors) + "（不计分母）";
  return s;
}

// 自动 baseline：--out 下同 suite / mode / repeat 的最近一个 run（按 header 的 created_us）。
// 为什么要对齐这三项：suite 换了比的就不是同一组 case；mode 换了（scripted vs real）
// 成功率没有可比性；repeat 换了 pass^k 口径不同——三种情况拿来比都会得出错误结论。
std::string find_previous_run(const std::string& out_dir, const std::string& cur_run_id,
                              const std::string& suite, const std::string& mode, int repeat) {
  namespace fs = std::filesystem;
  std::error_code ec;
  std::string best;
  std::int64_t best_created = -1;
  for (const auto& de :
       fs::directory_iterator(out_dir, fs::directory_options::skip_permission_denied, ec)) {
    std::error_code ec2;
    if (!de.is_directory(ec2)) continue;
    const std::string id = de.path().filename().string();
    if (id == cur_run_id || id.rfind("run-", 0) != 0) continue;
    auto hdr = eval::read_state_header((de.path() / "state.jsonl").string());
    if (!hdr) continue;
    if (header_str(*hdr, "suite") != suite) continue;
    if (header_str(*hdr, "mode") != mode) continue;
    if (header_int(*hdr, "repeat", 1) != repeat) continue;
    std::int64_t created = 0;
    if (hdr->contains("created_us") && (*hdr)["created_us"].is_number_integer()) {
      created = (*hdr)["created_us"].get<std::int64_t>();
    }
    if (created > best_created) {
      best_created = created;
      best = id;
    }
  }
  return best;
}

// 把两个 run 的 case 级差异投影成报告里的回归清单。判定复用 compare.h 的 diff_runs，
// 与 --diff 的输出同一套口径（run_error 单列、不可比先说明），不另写一套判据。
// present=false 表示读不到 baseline 的 state/header——调用方决定是报错还是当作没有 baseline。
eval::RegressionList build_regression(const std::string& out_dir, const eval::RunReport& cur,
                                      const std::string& baseline_id,
                                      const std::string& source) {
  eval::RegressionList reg;
  reg.baseline_run_id = baseline_id;
  reg.source = source;
  const std::string pa = out_dir + "/" + baseline_id + "/state.jsonl";
  const std::string pb = out_dir + "/" + cur.run_id + "/state.jsonl";
  auto hdr_a = eval::read_state_header(pa);
  auto hdr_b = eval::read_state_header(pb);
  if (!hdr_a || !hdr_b) return reg;
  auto state_a = eval::load_state(pa);
  auto state_b = eval::load_state(pb);
  if (!state_a || !state_b) return reg;

  eval::RunInfo ia;
  eval::RunInfo ib;
  ia.run_id = baseline_id;
  ib.run_id = cur.run_id;
  ia.fingerprint = header_str(*hdr_a, "fingerprint");
  ib.fingerprint = header_str(*hdr_b, "fingerprint");
  ia.suite = header_str(*hdr_a, "suite");
  ib.suite = header_str(*hdr_b, "suite");
  ia.mode = header_str(*hdr_a, "mode");
  ib.mode = header_str(*hdr_b, "mode");
  ia.repeat = header_int(*hdr_a, "repeat", 1);
  ib.repeat = header_int(*hdr_b, "repeat", 1);

  const auto d = eval::diff_runs(ia, *state_a, ib, *state_b);
  reg.present = true;
  reg.comparable = d.comparable;
  reg.warnings = d.warnings;
  reg.still_passing = d.still_passing;
  reg.inconclusive = d.inconclusive;
  reg.only_a = d.only_a;
  reg.only_b = d.only_b;
  for (const auto& c : d.cases) {
    switch (c.cls) {
      case eval::DiffClass::NewFailure:
        reg.new_failures.push_back(
            eval::RegressionEntry{c.id, "A " + case_agg_text(c.a) + " → B " + case_agg_text(c.b)});
        break;
      case eval::DiffClass::Fixed:
        reg.fixed.push_back(
            eval::RegressionEntry{c.id, "A " + case_agg_text(c.a) + " → B " + case_agg_text(c.b)});
        break;
      case eval::DiffClass::StillFailing:
        reg.still_failing.push_back(eval::RegressionEntry{c.id, case_agg_text(c.b)});
        break;
      default: break;
    }
  }
  return reg;
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
  std::string gen_path;
  std::string baseline_id;  // --baseline：显式对照 run
  bool no_baseline = false;
  bool out_set = false;
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
    } else if (a == "--baseline") {
      baseline_id = next();
      if (baseline_id.empty()) {
        std::fprintf(stderr, "--baseline 需要 run_id\n");
        return 2;
      }
    } else if (a == "--no-baseline") {
      no_baseline = true;
    } else if (a == "--max-yuan") {
      cfg.max_yuan = std::atof(next().c_str());
    } else if (a == "--gen") {
      gen_path = next();
      if (gen_path.empty()) {
        std::fprintf(stderr, "--gen 需要矩阵文件路径\n");
        return 2;
      }
    } else if (a == "--out") {
      cfg.out_dir = next();
      out_set = true;
    } else if (a == "--warmup") {
      cfg.warmup_s = std::max(0, std::atoi(next().c_str()));
    } else if (a == "--sla-settle-ms") {
      // SLA 重测前的收敛期。0 = 立即采样（复现"恢复期尖峰"这种问题时会用到）。
      cfg.sla_settle_ms = std::max(0, std::atoi(next().c_str()));
    } else if (a == "--model") {
      cfg.model = next();
    } else if (a == "--cued") {
      cfg.cued_bin = next();
    } else if (a == "--probe") {
      cfg.probe_bin = next();
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

  // FR-EVAL.5：--gen 是独立模式（只展开矩阵、写 suite，不跑 case）。
  // 与 --diff/--resume 同给会让"这次到底产出了什么"不可解释，直接拒绝。
  if (!baseline_id.empty() && no_baseline) {
    std::fprintf(stderr, "--baseline 与 --no-baseline 互斥\n");
    return 2;
  }
  if (!baseline_id.empty() || no_baseline) {
    if (!diff_a.empty() || !diff_b.empty() || !gen_path.empty()) {
      std::fprintf(stderr, "--baseline / --no-baseline 只影响评测报告的回归清单，"
                           "不能与 --diff / --gen 同时使用\n");
      return 2;
    }
  }
  // 显式 baseline 在跑之前就要校验：real 模式每个 case 约 11 s，跑完一轮才发现
  // "对照 run 写错了"是纯浪费（而且此时报告已经写不出来，终端只看到一条错误）。
  if (!baseline_id.empty()) {
    const std::string bp = cfg.out_dir + "/" + baseline_id + "/state.jsonl";
    auto hdr = eval::read_state_header(bp);
    if (!hdr) {
      std::fprintf(stderr, "--baseline %s：读不出 %s 的 header（%s）\n", baseline_id.c_str(),
                   bp.c_str(), hdr.error().c_str());
      return 2;
    }
  }
  if (!gen_path.empty()) {
    if (!diff_a.empty() || !diff_b.empty() || !cfg.resume_run_id.empty()) {
      std::fprintf(stderr, "--gen 不能与 --diff / --resume 同时使用\n");
      return 2;
    }
    auto gen = eval::generate_from_files(gen_path);
    if (!gen) {
      std::fprintf(stderr, "[gen] 生成失败: %s\n", gen.error().c_str());
      return 2;
    }
    const std::string dir = out_set ? cfg.out_dir : std::string("suites/generated");
    const std::string name = gen->suite.value("suite", std::string("generated"));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
      std::fprintf(stderr, "[gen] 无法创建输出目录 %s: %s\n", dir.c_str(), ec.message().c_str());
      return 5;
    }
    const std::string path = dir + "/" + name + ".json";
    // 只允许覆盖 --gen 自己的产物：手写 suite 的 note 里没有这个标记，防止
    // 「--out suites」把 real-faults.json 这类手写集冲掉（生成器不该改手写资产）。
    if (std::filesystem::exists(path)) {
      std::ifstream old(path);
      std::string text((std::istreambuf_iterator<char>(old)), std::istreambuf_iterator<char>());
      if (text.find("cue-eval --gen") == std::string::npos) {
        std::fprintf(stderr,
                     "[gen] 拒绝覆盖 %s：它不是 --gen 生成的 suite（手写 suite 不允许被生成器改写）\n",
                     path.c_str());
        return 2;
      }
    }
    {
      std::ofstream out(path);
      if (!out) {
        std::fprintf(stderr, "[gen] 无法写入 %s\n", path.c_str());
        return 5;
      }
      out << gen->suite.dump(2) << "\n";
      out.flush();
      if (!out) {
        std::fprintf(stderr, "[gen] 写 %s 失败\n", path.c_str());
        return 5;
      }
    }
    const auto& st = gen->stats;
    std::vector<std::string> dist;
    for (const auto& [root, n] : st.by_root_cause) {
      dist.push_back(root + "=" + std::to_string(n));
    }
    std::vector<std::string> rejections;
    for (const auto& [why, n] : st.rejected_reasons) {
      rejections.push_back(why + "×" + std::to_string(n));
    }
    std::string joined;
    for (std::size_t i = 0; i < dist.size(); ++i) {
      joined += (i == 0 ? "" : ", ") + dist[i];
    }
    std::string why_joined;
    for (std::size_t i = 0; i < rejections.size(); ++i) {
      why_joined += (i == 0 ? "" : "；") + rejections[i];
    }
    const std::string why_text = why_joined.empty() ? "" : "（" + why_joined + "）";
    std::printf("[gen] 生成 %zu 个 case → %s（%s）；组合 %zu 个、排除 %zu 个非法%s\n",
                st.generated, path.c_str(), joined.c_str(), st.combinations, st.rejected,
                why_text.c_str());
    return 0;
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
  auto report = engine.run(*cases);
  if (!report.fatal_error.empty()) {
    // 不能写 report.md：resume 失败时会把原 run 的报告覆盖成 0 分（review-eval.md #8）
    std::fprintf(stderr, "[eval] 未产出结果：%s\n", report.fatal_error.c_str());
    // 终端摘要照样给（走 fatal 分支）：读者要知道"这次没有结果"，而不是只看到一行 stderr
    std::printf("%s", engine.render_terminal_summary(report).c_str());
    return 2;
  }

  std::error_code ec;
  std::filesystem::create_directories(cfg.out_dir + "/" + report.run_id, ec);
  const std::string report_path = cfg.out_dir + "/" + report.run_id + "/report.md";
  const std::string json_path = cfg.out_dir + "/" + report.run_id + "/report.json";
  report.report_path = report_path;
  report.report_json_path = json_path;

  // 回归清单（§8.9 ④）：显式 --baseline 优先，否则自动找同 suite / mode / repeat 的上一次 run。
  // 找不到时 present 保持 false —— 报告写"本次没有 baseline，跳过"，而不是留空小节，
  // 更不能拿别的 suite 的 run 冒充（那会把无关差异读成回归）。
  if (!no_baseline) {
    std::string bid = baseline_id;
    std::string src = "explicit";
    if (bid.empty()) {
      bid = find_previous_run(cfg.out_dir, report.run_id, cfg.suite_path, cfg.agent_mode,
                              cfg.repeat);
      src = "auto";
    }
    if (!bid.empty()) {
      auto reg = build_regression(cfg.out_dir, report, bid, src);
      if (reg.present) {
        report.regression = std::move(reg);
      } else if (!baseline_id.empty()) {
        // 显式指定却读不出来：不能静默当作"没有 baseline"（那会看起来像比过了）
        std::fprintf(stderr, "--baseline %s：读不出 %s/%s/state.jsonl 的 header 或行\n",
                     baseline_id.c_str(), cfg.out_dir.c_str(), baseline_id.c_str());
        return 2;
      }
    }
  }

  const std::string md = engine.render_markdown(report);
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
  // report.json：与 report.md 同一份数据（字段名稳定，给回归脚本/看板用）。
  // 端到端字段没有样本时写 null + null 原因，不写 0。
  {
    std::ofstream out(json_path);
    if (!out) {
      std::fprintf(stderr, "无法写入 %s\n", json_path.c_str());
      return 5;
    }
    out << engine.render_json(report);
    out.flush();
    if (!out) {
      std::fprintf(stderr, "写 %s 失败\n", json_path.c_str());
      return 5;
    }
  }
  std::printf("%s\n", md.c_str());
  // 终端摘要（FR-EVAL.10）：走 stdout（报告正文也在 stdout），stderr 只留告警与错误。
  std::printf("%s", engine.render_terminal_summary(report).c_str());
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
