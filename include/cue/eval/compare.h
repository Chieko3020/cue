// 回归对比：两个 run 的 case 级 diff（新增失败 / 已修复 / 仍失败 / 仍通过）
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
  int run_errors = 0;
  std::string root_cause;
  bool pass() const { return attempts > 0 && passes == attempts && run_errors == 0; }
};

std::map<std::string, CaseAgg> aggregate(const StateMap& state);

enum class DiffClass { NewFailure, Fixed, StillFailing, StillPassing, OnlyInA, OnlyInB };
const char* diff_class_name(DiffClass c);

struct CaseDiff {
  std::string id;
  DiffClass cls = DiffClass::StillPassing;
  CaseAgg a;
  CaseAgg b;
};

struct DiffReport {
  std::string run_a;
  std::string run_b;
  std::vector<CaseDiff> cases;
  int new_failures = 0, fixed = 0, still_failing = 0, still_passing = 0;
  int only_a = 0, only_b = 0;
};

DiffReport diff_runs(const std::string& run_a, const StateMap& a, const std::string& run_b,
                     const StateMap& b);
std::string render_diff_markdown(const DiffReport& d);

}  // namespace cue::eval
