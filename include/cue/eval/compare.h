// 回归对比：两个 run 的 case 级 diff
//  新增失败 / 已修复 / 仍失败 / 仍通过 / 不可判定（run_error）/ 仅 A 有 / 仅 B 有
//
// 口径必须与评测报告一致（review-eval.md #15/#16）：
//  - run_error 不是"失败"，单列 Inconclusive，不计入 new_failures
//  - diff 前比对两边的指纹/suite/mode/repeat：判分器升级后的差异不能读成"已修复"
//  - case 集变动（仅 A/B 有）单独呈现，不静默通过
#pragma once

#include <map>
#include <string>
#include <vector>

#include "cue/eval/state.h"

namespace cue::eval {

// 单个 case 在若干次尝试上的聚合
struct CaseAgg {
  int attempts = 0;
  int passes = 0;       // 三级判分全过
  int run_errors = 0;   // 不计入分母
  std::string root_cause;
  int scored() const { return attempts - run_errors; }
  bool pass() const { return scored() > 0 && run_errors == 0 && passes == scored(); }
};

std::map<std::string, CaseAgg> aggregate(const StateMap& state);

enum class DiffClass {
  NewFailure,
  Fixed,
  StillFailing,
  StillPassing,
  Inconclusive,
  OnlyInA,
  OnlyInB
};
const char* diff_class_name(DiffClass c);

// 参与 diff 的 run 元信息（来自 state.jsonl 的 header）
struct RunInfo {
  std::string run_id;
  std::string fingerprint;
  std::string suite;
  std::string mode;
  int repeat = 1;
};

struct CaseDiff {
  std::string id;
  DiffClass cls = DiffClass::StillPassing;
  CaseAgg a;
  CaseAgg b;
};

struct DiffReport {
  RunInfo a;
  RunInfo b;
  std::vector<CaseDiff> cases;
  // false = 两边指纹/suite/mode 不一致，差异不可归因于被测对象
  bool comparable = true;
  std::vector<std::string> warnings;
  int new_failures = 0, fixed = 0, still_failing = 0, still_passing = 0;
  int inconclusive = 0, only_a = 0, only_b = 0;
};

DiffReport diff_runs(const RunInfo& a, const StateMap& a_state, const RunInfo& b,
                     const StateMap& b_state);
std::string render_diff_markdown(const DiffReport& d);

}  // namespace cue::eval
