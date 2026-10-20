// M4b 回归对比测试：case 级 diff 分类（新增失败 / 已修复 / 仍失败 / 仍通过）+ run_error 口径
#include <cstdio>
#include <string>

#include "cue/eval/compare.h"
#include "cue/eval/state.h"

namespace eval = cue::eval;

namespace {

int failures = 0;
void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

eval::CaseOutcome make_outcome(const std::string& id, bool root, bool evidence, bool sla) {
  eval::CaseOutcome o;
  o.id = id;
  o.scored = true;
  o.score.root = root;
  o.score.evidence = evidence;
  o.score.sla = sla;
  o.turns = 7;
  o.cost_yuan = 0.05;
  o.diagnosis.root_cause = "network_loss";
  o.final_stage = "done";
  return o;
}

}  // namespace

int main() {
  // 回归对比分类
  {
    // A：c1 过，c2 失败，c3 过；B：c1 失败（新增失败），c2 过（已修复），c3 过，c4 仅 B 有
    eval::StateMap a;
    eval::StateMap b;
    a[{0, 0}] = eval::CachedOutcome{make_outcome("c1", true, true, true), "fp"};
    a[{1, 0}] = eval::CachedOutcome{make_outcome("c2", false, true, true), "fp"};
    a[{2, 0}] = eval::CachedOutcome{make_outcome("c3", true, true, true), "fp"};
    b[{0, 0}] = eval::CachedOutcome{make_outcome("c1", true, true, false), "fp"};
    b[{1, 0}] = eval::CachedOutcome{make_outcome("c2", true, true, true), "fp"};
    b[{2, 0}] = eval::CachedOutcome{make_outcome("c3", true, true, true), "fp"};
    b[{3, 0}] = eval::CachedOutcome{make_outcome("c4", true, true, true), "fp"};

    const auto d = eval::diff_runs("runA", a, "runB", b);
    check(d.new_failures == 1, "diff 新增失败 = 1");
    check(d.fixed == 1, "diff 已修复 = 1");
    check(d.still_passing == 1, "diff 仍通过 = 1");
    check(d.still_failing == 0, "diff 仍失败 = 0");
    check(d.only_b == 1, "diff 仅 B 有 = 1");
    const auto md = eval::render_diff_markdown(d);
    check(md.find("新增失败") != std::string::npos && md.find("已修复") != std::string::npos,
          "diff markdown 含分类");
    check(md.find("c1") != std::string::npos && md.find("c4") != std::string::npos, "diff markdown 含 case");

    // run_error 不算通过
    eval::StateMap er;
    eval::CaseOutcome o = make_outcome("c5", true, true, true);
    o.scored = false;
    o.error_kind = "agent_timeout";
    er[{0, 0}] = eval::CachedOutcome{o, "fp"};
    const auto agg = eval::aggregate(er);
    check(agg.at("c5").run_errors == 1 && !agg.at("c5").pass(), "run_error 不计为通过");
  }
  if (failures == 0) {
    std::printf("test_compare: OK\n");
    return 0;
  }
  std::printf("test_compare: %d failure(s)\n", failures);
  return 1;
}
