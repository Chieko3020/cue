// M4 判分器负向测试：必须能把「答对」和「答错 / 伪造证据 / SLA 未达成」区分开
//
// 对抗用例直接取自 docs/research/review-eval.md 第 3 节：改判分器后必须复跑这些构造，
// 任何一个被接受就是回归。
#include <cstdio>
#include <string>
#include <vector>

#include "cue/eval/eval.h"

namespace eval = cue::eval;
namespace agent = cue::agent;

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
  c.evidence_candidates = {"injected", "dropped", "fps"};
  c.sla_fps_min = 25.0;
  c.sla_dropped_max = 5;
  return c;
}

// 真实 query_metrics 返回的形态（字段名与 cued 统计行一致）
const char* kMetricsText =
    "metrics={\"fps\":30.3,\"frames_in\":247,\"frames_out\":247,\"dropped\":0,\"injected\":59,"
    "\"decoded\":247,\"decode_errors\":0,\"decode_p99_ms\":3.36,\"send_p50_ms\":0.09}";

std::vector<eval::Observation> make_obs() {
  eval::Observation o;
  o.tool = "query_metrics";
  o.text = kMetricsText;
  o.data = nlohmann::json{{"fps", 30.3},        {"frames_in", 247}, {"frames_out", 247},
                          {"dropped", 0},       {"injected", 59},  {"decoded", 247},
                          {"decode_errors", 0}, {"decode_p99_ms", 3.36}, {"send_p50_ms", 0.09}};
  return {o};
}

agent::RunResult good_result() {
  agent::RunResult r;
  r.final_stage = agent::Stage::Done;
  r.diagnosis.root_cause = "network_loss";
  r.diagnosis.evidence = {"injected=59"};
  return r;
}

eval::Metrics good_metrics() {
  eval::Metrics m;
  m.samples = 1;
  m.fps = 30.3;
  m.dropped = 0;
  return m;
}

bool evidence_of(const std::string& ev) {
  auto r = good_result();
  r.diagnosis.evidence = {ev};
  return eval::judge_case(make_case(), r, make_obs(), good_metrics()).evidence;
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
    eval::Metrics m;
    m.samples = 0;
    const auto s = eval::judge_case(make_case(), good_result(), make_obs(), m);
    check(!s.sla, "case7 no samples fails sla");
  }

  // 8) evidence_matches 的命中方式
  {
    const auto obs = make_obs();
    check(eval::evidence_matches("injected=59", obs), "case8a exact substring");
    check(eval::evidence_matches("INJECTED=59", obs), "case8b case insensitive");
    check(!eval::evidence_matches("59", obs), "case8c bare number rejected");
    check(!eval::evidence_matches("injected=60", obs), "case8d wrong number rejected");
    check(!eval::evidence_matches("", obs), "case8e empty rejected");

    // 对抗：空观测 / 纯中文观测不能成为万能通行证
    std::vector<eval::Observation> empty_obs;
    eval::Observation eo;
    empty_obs.push_back(eo);
    check(!eval::evidence_matches("anything at all here", empty_obs), "case8j empty observation not a pass");
    std::vector<eval::Observation> cn_obs;
    eval::Observation co;
    co.text = "观测为空";
    cn_obs.push_back(co);
    check(!eval::evidence_matches("nothing useful here", cn_obs), "case8k short observation not a pass");
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

  // 9) 五个必须判伪的伪造构造（review-eval.md 第 3 节）
  {
    // (1) 整段观测 + 编造结论：引用全部命中，但多了解释性文字 → 判伪
    check(!evidence_of(std::string(kMetricsText) + "，根因是编码器过慢且 CPU 打满"),
          "case9a 整段观测 + 编造结论被判伪");
    // (2) 空观测：任何 ≥8 字符的证据都不能成立
    {
      eval::Observation empty;
      const std::vector<eval::Observation> obs{empty};
      check(!eval::evidence_matches("injected=999 fps=9999", obs), "case9b 空观测不能当通行证");
    }
    // (3) 计数用小数逃逸
    check(!evidence_of("injected=59.4"), "case9c injected=59.4 被判伪");
    // (4) 计数必须精确
    check(!evidence_of("injected=60"), "case9d injected=60 被判伪");
    // (5) 无关数字（真实存在但与根因无关）：靠 evidence_candidates 拦截
    check(eval::evidence_matches("send_p50_ms=0.09", make_obs()), "case9e send_p50_ms 确实是真实观测");
    check(!evidence_of("send_p50_ms=0.09"), "case9e 无关数字不满足 evidence_candidates → 判伪");
  }

  // 10) suite 解析（显式 schema：未知字段/不支持的注入器都报错）
  {
    auto cases = eval::load_suite("suites/m2-constraints.json");
    check(cases.has_value(), "case10 suite loads");
    if (cases) {
      check(cases->size() == 2, "case10 two cases");
      if (cases->size() == 2) {
        check((*cases)[0].expect_root_cause == "resolution_too_high", "case10 first root cause");
        check((*cases)[1].expect_root_cause == "fec_ghost_frame", "case10 second root cause");
        check((*cases)[0].manual_only && (*cases)[1].manual_only, "case10 m2 集只能人工复现");
        check((*cases)[0].sla_fps_min > 0.0, "case10 usable_fps_min 被解析（不再丢字段）");
      }
    }
    auto m4 = eval::load_suite("suites/m4-diagnosis.json");
    check(m4.has_value() && m4->size() == 2, "case10 m4-diagnosis 可加载");
  }

  if (failures == 0) {
    std::printf("test_eval: OK (10 groups)\n");
    return 0;
  }
  std::printf("test_eval: %d failure(s)\n", failures);
  return 1;
}
