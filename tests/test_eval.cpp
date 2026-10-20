// M4 判分器负向测试：必须能把「答对」和「答错 / 伪造证据 / SLA 未达成」区分开
#include <cstdio>
#include <string>
#include <vector>

#include "cue/eval/eval.h"

namespace eval = cue::eval;
namespace agent = cue::agent;
namespace pipeline = cue::pipeline;

namespace {
int failures = 0;
void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

eval::Case make_case() {
  eval::Case c;
  c.id = "t1";
  c.expect_root_cause = "network_loss";
  c.sla_fps_min = 25.0;
  c.sla_dropped_max = 5;
  return c;
}

std::vector<eval::Observation> make_obs() {
  eval::Observation o;
  o.tool = "query_metrics";
  o.text = "frames_in=247 frames_out=247 dropped=0 injected=59 decoded=247 decode_errors=0 fps=30.3";
  o.data = nlohmann::json{{"injected", 59}, {"dropped", 0}, {"fps", 30.3}};
  return {o};
}

agent::RunResult good_result() {
  agent::RunResult r;
  r.final_stage = agent::Stage::Done;
  r.diagnosis.root_cause = "network_loss";
  r.diagnosis.evidence = {"injected=59"};
  return r;
}

pipeline::PipelineMetrics good_metrics() {
  pipeline::PipelineMetrics m;
  m.samples = 1;
  m.fps = 30.3;
  m.dropped = 0;
  return m;
}
}  // namespace

int main() {
  // 1) 全对
  {
    const auto s = eval::judge_case(make_case(), good_result(), make_obs(), good_metrics());
    check(s.root && s.evidence && s.sla, "case1 all pass");
  }

  // 2) 根因错（其余对）
  {
    auto r = good_result();
    r.diagnosis.root_cause = "encoder_too_slow";
    const auto s = eval::judge_case(make_case(), r, make_obs(), good_metrics());
    check(!s.root, "case2 wrong root detected");
    check(s.evidence, "case2 evidence still ok");
  }

  // 3) 伪造证据（观测里根本没有这个数字）
  {
    auto r = good_result();
    r.diagnosis.evidence = {"jitter_buffer_depth=9999"};
    const auto s = eval::judge_case(make_case(), r, make_obs(), good_metrics());
    check(!s.evidence, "case3 fabricated evidence rejected");
  }

  // 4) 空证据（不可成立）
  {
    auto r = good_result();
    r.diagnosis.evidence.clear();
    const auto s = eval::judge_case(make_case(), r, make_obs(), good_metrics());
    check(!s.evidence, "case4 empty evidence rejected");
  }

  // 5) SLA 未达成：fps 低于阈值
  {
    auto m = good_metrics();
    m.fps = 12.0;
    const auto s = eval::judge_case(make_case(), good_result(), make_obs(), m);
    check(!s.sla, "case5 low fps fails sla");
  }

  // 6) SLA 未达成：dropped 超阈值
  {
    auto m = good_metrics();
    m.dropped = 50;
    const auto s = eval::judge_case(make_case(), good_result(), make_obs(), m);
    check(!s.sla, "case6 high dropped fails sla");
  }

  // 7) 无采样（没跑起来）不能算达标
  {
    pipeline::PipelineMetrics m;
    m.samples = 0;
    const auto s = eval::judge_case(make_case(), good_result(), make_obs(), m);
    check(!s.sla, "case7 no samples fails sla");
  }

  // 8) evidence_matches 的三种命中方式
  {
    const auto obs = make_obs();
    check(eval::evidence_matches("injected=59", obs), "case8a exact substring");
    check(eval::evidence_matches("INJECTED=59", obs), "case8b case insensitive");
    check(eval::evidence_matches("59", obs), "case8c bare number");
    check(!eval::evidence_matches("injected=60", obs), "case8d wrong number rejected");
    check(!eval::evidence_matches("", obs), "case8e empty rejected");
  }

  // 8f–8i) 归一化匹配：模型把观测写成 "k": v 与 k=v 都要认，但数值必须真的对得上
  {
    const auto obs = make_obs();
    check(eval::evidence_matches("query_metrics(5000ms): injected=59, fps=30.3, dropped=0", obs),
          "case8f 带前缀与 key=value 的引用");
    check(eval::evidence_matches("fps=30.31", obs), "case8g 连续量 ±2% 容差");
    check(!eval::evidence_matches("injected=59, fps=12.0", obs), "case8h 同一引用里数值冲突被拒");
    check(!eval::evidence_matches("query_metrics: injected=60", obs), "case8i 计数值不容差");
    check(!eval::evidence_matches("jitter_buffer_depth=9999", obs), "case8j 编造字段被拒");
  }

  // 9) suite 解析
  {
    std::string err;
    const auto cases = eval::load_suite("suites/m2-constraints.json", err);
    check(err.empty(), "case9 suite loads");
    check(cases.size() == 2, "case9 two cases");
    if (cases.size() == 2) {  // 缺 suite 时不要越界（曾在 build/ 目录下跑崩过）
      check(cases[0].expect_root_cause == "resolution_too_high", "case9 first root cause");
      check(cases[1].expect_root_cause == "fec_ghost_frame", "case9 second root cause");
    }
  }

  if (failures == 0) {
    std::printf("test_eval: OK (9 cases)\n");
    return 0;
  }
  std::printf("test_eval: %d failure(s)\n", failures);
  return 1;
}
