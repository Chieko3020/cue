// M4b 回归对比测试：case 级 diff 分类 + run_error 单列 + 指纹不可比告警
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

eval::CaseOutcome make_error(const std::string& id) {
  eval::CaseOutcome o;
  o.id = id;
  o.scored = false;
  o.error_kind = "agent_timeout";
  return o;
}

eval::RunInfo info(const std::string& id, const std::string& fp) {
  eval::RunInfo r;
  r.run_id = id;
  r.fingerprint = fp;
  r.suite = "suites/m4-diagnosis.json";
  r.mode = "real";
  r.repeat = 1;
  return r;
}

}  // namespace

int main() {
  // 1) 基本分类：新增失败 / 已修复 / 仍失败 / 仍通过 / 仅 B 有
  {
    eval::StateMap a;
    eval::StateMap b;
    a[{0, 0}] = eval::CachedOutcome{make_outcome("c1", true, true, true), "fp"};
    a[{1, 0}] = eval::CachedOutcome{make_outcome("c2", false, true, true), "fp"};
    a[{2, 0}] = eval::CachedOutcome{make_outcome("c3", false, true, true), "fp"};
    a[{3, 0}] = eval::CachedOutcome{make_outcome("c4", true, true, true), "fp"};
    b[{0, 0}] = eval::CachedOutcome{make_outcome("c1", true, true, false), "fp"};
    b[{1, 0}] = eval::CachedOutcome{make_outcome("c2", true, true, true), "fp"};
    b[{2, 0}] = eval::CachedOutcome{make_outcome("c3", false, true, true), "fp"};
    b[{4, 0}] = eval::CachedOutcome{make_outcome("c5", true, true, true), "fp"};

    const auto d = eval::diff_runs(info("runA", "fp1"), a, info("runB", "fp1"), b);
    check(d.comparable, "同指纹 → 可比");
    check(d.new_failures == 1, "diff 新增失败 = 1");
    check(d.fixed == 1, "diff 已修复 = 1");
    check(d.still_failing == 1, "diff 仍失败 = 1");
    check(d.still_passing == 0, "diff 仍通过 = 0");
    check(d.only_a == 1 && d.only_b == 1, "diff 仅 A/仅 B 各有 1");
    const auto md = eval::render_diff_markdown(d);
    check(md.find("新增失败") != std::string::npos && md.find("已修复") != std::string::npos,
          "diff markdown 含分类");
    check(md.find("c1") != std::string::npos && md.find("c5") != std::string::npos, "diff markdown 含 case");
    check(md.find("case 集发生变化") != std::string::npos, "case 集变动有醒目提示");
  }

  // 2) run_error 单列 Inconclusive：不能报成"新增失败"（CI 会误红）
  {
    eval::StateMap a;
    eval::StateMap b;
    a[{0, 0}] = eval::CachedOutcome{make_outcome("c1", true, true, true), "fp"};
    b[{0, 0}] = eval::CachedOutcome{make_error("c1"), "fp"};
    eval::StateMap ea;  // A 侧也是 run_error
    ea[{0, 0}] = eval::CachedOutcome{make_error("c1"), "fp"};
    b[{1, 0}] = eval::CachedOutcome{make_outcome("c2", true, true, true), "fp"};
    ea[{1, 0}] = eval::CachedOutcome{make_error("c2"), "fp"};

    const auto d = eval::diff_runs(info("A", "fp"), a, info("B", "fp"), b);
    check(d.new_failures == 0, "run_error 不算新增失败");
    check(d.inconclusive == 1, "run_error 计入不可判定");
    const auto md = eval::render_diff_markdown(d);
    check(md.find("不可判定") != std::string::npos, "diff markdown 有不可判定分组");
    const auto agg = eval::aggregate(ea);
    check(agg.at("c1").run_errors == 1 && !agg.at("c1").pass(), "run_error 不计为通过");
  }

  // 3) 不可比：指纹/suite/mode/repeat 不一致时必须显式告警（不能被读成"已修复 N"）
  {
    eval::StateMap a;
    eval::StateMap b;
    a[{0, 0}] = eval::CachedOutcome{make_outcome("c1", false, true, true), "fp"};
    b[{0, 0}] = eval::CachedOutcome{make_outcome("c1", true, true, true), "fp"};
    auto ib = info("B", "fp2");
    ib.suite = "suites/other.json";
    ib.mode = "scripted";
    ib.repeat = 3;
    const auto d = eval::diff_runs(info("A", "fp1"), a, ib, b);
    check(!d.comparable, "指纹/配置不同 → 不可比");
    check(d.warnings.size() >= 4, "指纹/suite/mode/repeat 各有一条告警");
    const auto md = eval::render_diff_markdown(d);
    check(md.find("两 run 不可比") != std::string::npos, "diff markdown 有不可比 banner");
  }

  // C05: known ineffective injection is inconclusive, never a fix/failure.
  {
    auto excluded = make_outcome("x", true, true, true);
    excluded.injection = eval::judge_injection("rate", 0, 0);
    eval::StateMap a, b;
    a[{0, 0}] = eval::CachedOutcome{excluded, "fp"};
    b[{0, 0}] = eval::CachedOutcome{make_outcome("x", false, false, false), "fp"};
    const auto d = eval::diff_runs(info("A", "fp"), a, info("B", "fp"), b);
    check(d.inconclusive == 1 && d.new_failures == 0 && d.fixed == 0,
          "C05 ineffective injection is not a scored regression");
  }

  if (failures == 0) {
    std::printf("test_compare: OK\n");
    return 0;
  }
  std::printf("test_compare: %d failure(s)\n", failures);
  return 1;
}
