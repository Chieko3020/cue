// cue-eval — 评测 CLI
// 用法：
//   cue-eval --suite suites/m2-constraints.json --mode scripted --repeat 1
//   cue-eval --suite suites/m4-diagnosis.json --mode real --repeat 1   # 需并行推流
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "cue/eval/eval.h"

namespace eval = cue::eval;

namespace {

int usage() {
  std::printf(
      "cue-eval — Cue 评测 CLI\n"
      "\n"
      "  cue-eval --suite <path> [--mode scripted|real] [--repeat k] [--max-yuan N]\n"
      "           [--out dir] [--warmup s] [--model name] [--cued path]\n"
      "\n"
      "real 模式需环境变量 CUE_API_KEY / CUE_BASE_URL / CUE_MODEL，且需外部推流。\n");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  eval::RunConfig cfg;
  cfg.suite_path = "suites/m2-constraints.json";

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(); };
    if (a == "--suite") cfg.suite_path = next();
    else if (a == "--mode") cfg.agent_mode = next();
    else if (a == "--repeat") cfg.repeat = std::max(1, std::atoi(next().c_str()));
    else if (a == "--max-yuan") cfg.max_yuan = std::atof(next().c_str());
    else if (a == "--out") cfg.out_dir = next();
    else if (a == "--warmup") cfg.warmup_s = std::max(0, std::atoi(next().c_str()));
    else if (a == "--model") cfg.model = next();
    else if (a == "--cued") cfg.cued_bin = next();
    else if (a == "--help" || a == "-h") {
      return usage();
    } else {
      std::fprintf(stderr, "未知参数: %s\n", a.c_str());
      return 2;
    }
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
  return (report.root_rate >= 1.0 && report.evidence_rate >= 1.0) ? 0 : 1;
}
