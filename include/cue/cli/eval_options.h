// cue-eval 的启动选项：命令行 > 配置文件 > 内置默认值
//
// 与 serve_options.h 同一套规则（FR-OBS.5）。区别是评测的选项更多、还有互斥项
// （--record-tools / --replay-tools），所以配置校验必须覆盖"配置文件给一个、命令行给
// 另一个"的情况：合并之后才判定互斥，报错时才可能带出配置文件那一行的行号。
#pragma once

#include <expected>
#include <string>

#include "cue/common/config.h"
#include "cue/common/log.h"
#include "cue/eval/eval.h"

namespace cue::cli {

struct EvalOptions {
  cue::eval::RunConfig cfg;

  bool help = false;
  // "这个字段是显式给的"标记：默认 suite / 默认 out 的逻辑依赖它们。
  bool suite_set = false;
  bool mode_set = false;
  bool repeat_set = false;
  bool out_set = false;
  bool no_baseline = false;
  bool replay_flag = false;
  bool live_flag = false;
  bool record_flag = false;
  bool record_cli = false;
  std::string baseline_id;
  std::string gen_path;
  std::string diff_a;
  std::string diff_b;

  cue::log::Options log;
  std::string config_path;  // 非空 = 用了 --config
};

std::expected<EvalOptions, config::Error> parse_eval_options(int argc, char** argv);

void print_eval_usage();

}  // namespace cue::cli
