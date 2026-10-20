// cue-eval — 评测 CLI
// 用法：
//   cue-eval --suite suites/m4-diagnosis.json --mode scripted --repeat 1
//   cue-eval --suite ... --mode real --repeat 1 --warmup 6        # 需并行推流
//   cue-eval --resume run-<id>                                    # 断点续跑
//   cue-eval --diff run-<a> run-<b>                               # 回归对比
#include <algorithm>
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

int usage() {
  std::printf(
      "cue-eval — Cue 评测 CLI\n"
      "\n"
      "  cue-eval --suite <path> [--mode scripted|real] [--repeat k] [--max-yuan N]\n"
      "           [--out dir] [--warmup s] [--model name] [--cued path]\n"
      "  cue-eval --resume <run_id> [--out dir]             # 从 state.jsonl 续跑\n"
      "  cue-eval --diff <run_a> <run_b> [--out dir]        # case 级回归对比\n"

      "\n"
      "real 模式需环境变量 CUE_API_KEY / CUE_BASE_URL / CUE_MODEL，且需外部推流。\n");
  return 0;
}

int do_diff(const std::string& run_a, const std::string& run_b, const std::string& out_dir) {
  eval::StateMap a;
  eval::StateMap b;
  std::string err;
  if (!eval::load_state(out_dir + "/" + run_a + "/state.jsonl", a, err)) {
    std::fprintf(stderr, "读取 A 失败: %s\n", err.c_str());
    return 1;
  }
  if (!eval::load_state(out_dir + "/" + run_b + "/state.jsonl", b, err)) {
    std::fprintf(stderr, "读取 B 失败: %s\n", err.c_str());
    return 1;
  }
  const auto d = eval::diff_runs(run_a, a, run_b, b);
  const std::string md = eval::render_diff_markdown(d);
  const std::string path = out_dir + "/diff-" + run_a + "-" + run_b + ".md";
  std::ofstream out(path);
  if (!out) {
    std::fprintf(stderr, "无法写入 %s\n", path.c_str());
  } else {
    out << md;
    std::printf("[eval] diff 已写入 %s\n", path.c_str());
  }
  std::printf("%s", md.c_str());
  // 有回归（新增失败）时以非零退出码提示 CI
  return d.new_failures > 0 ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  eval::RunConfig cfg;
  cfg.suite_path = "suites/m2-constraints.json";
  std::string diff_a, diff_b;
  bool suite_set = false;
  bool mode_set = false;
  bool repeat_set = false;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(); };
    if (a == "--suite") {
      cfg.suite_path = next();
      suite_set = true;
    } else if (a == "--mode") {
      cfg.agent_mode = next();
      mode_set = true;
    } else if (a == "--repeat") {
      cfg.repeat = std::max(1, std::atoi(next().c_str()));
      repeat_set = true;
    }
    else if (a == "--max-yuan") cfg.max_yuan = std::atof(next().c_str());
    else if (a == "--out") cfg.out_dir = next();
    else if (a == "--warmup") cfg.warmup_s = std::max(0, std::atoi(next().c_str()));
    else if (a == "--model") cfg.model = next();
    else if (a == "--cued") cfg.cued_bin = next();
    else if (a == "--resume") cfg.resume_run_id = next();
    else if (a == "--diff") {
      diff_a = next();
      diff_b = next();
    }
    else if (a == "--help" || a == "-h") {
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

  if (!cfg.resume_run_id.empty()) {
    const std::string state = cfg.out_dir + "/" + cfg.resume_run_id + "/state.jsonl";
    if (!std::filesystem::exists(state)) {
      std::fprintf(stderr, "续跑失败：%s 不存在（用新 run 请去掉 --resume）\n", state.c_str());
      return 2;
    }
    // 续跑必须沿用原 run 的 suite/mode/repeat：否则指纹全变，缓存全废
    auto hdr = eval::read_state_header(state);
    if (!hdr) {
      std::fprintf(stderr, "续跑失败：%s\n", hdr.error().c_str());
      return 2;
    }
    const std::string h_suite = hdr->value("suite", std::string());
    const std::string h_mode = hdr->value("mode", cfg.agent_mode);
    const int h_repeat = hdr->value("repeat", cfg.repeat);
    if (!h_suite.empty()) {
      if (suite_set && cfg.suite_path != h_suite) {
        std::fprintf(stderr, "续跑失败：--suite=%s 与原 run 的 %s 不一致\n", cfg.suite_path.c_str(),
                     h_suite.c_str());
        return 2;
      }
      cfg.suite_path = h_suite;
    }
    if (!mode_set) cfg.agent_mode = h_mode;
    if (!repeat_set) cfg.repeat = std::max(1, h_repeat);
    std::printf("[eval] 续跑沿用原 run 配置：suite=%s mode=%s repeat=%d\n", cfg.suite_path.c_str(),
                cfg.agent_mode.c_str(), cfg.repeat);
  }

  std::string err;
  const auto cases = eval::load_suite(cfg.suite_path, err);
  if (!err.empty()) {
    std::fprintf(stderr, "加载 suite 失败: %s\n", err.c_str());
    return 1;
  }
  std::printf("[eval] suite=%s cases=%zu mode=%s repeat=%d\n", cfg.suite_path.c_str(), cases.size(),
              cfg.agent_mode.c_str(), cfg.repeat);

  eval::Engine engine(cfg);
  const auto report = engine.run(cases);
  const std::string md = engine.render_markdown(report);

  std::error_code ec;
  std::filesystem::create_directories(cfg.out_dir + "/" + report.run_id, ec);
  {
    std::ofstream out(cfg.out_dir + "/" + report.run_id + "/report.md");
    out << md;
  }
  std::printf("%s\n", md.c_str());
  if (report.scored == 0) return 1;
  // 退出码必须覆盖三级成功率与 pass^k：只看 root/evidence 时 SLA 全灭也会返回 0（假绿灯）
  return (report.run_errors == 0 && report.root_rate >= 1.0 && report.evidence_rate >= 1.0 &&
          report.sla_rate >= 1.0 && report.pass_k >= 1.0)
             ? 0
             : 1;
}
