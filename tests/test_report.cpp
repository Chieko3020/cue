// T-08 报告字段的单测：失败模式分布（§8.4）/ 运行状态分离 / 稳定性 / report.md 两节 /
// 终端摘要（FR-EVAL.10）。全部是纯函数，毫秒级，不读磁盘、不起管线。
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "cue/eval/eval.h"

namespace eval = cue::eval;

namespace {

int failures = 0;
void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

eval::Case mk_case(const std::string& id, const std::string& root) {
  eval::Case c;
  c.id = id;
  c.expect_root_cause = root;
  c.sla_fps_min = 25.0;
  c.sla_dropped_max = 5;
  return c;
}

eval::CaseOutcome mk_outcome(const std::string& id, int attempt, bool root, bool evidence,
                             bool sla) {
  eval::CaseOutcome o;
  o.id = id;
  o.attempt = attempt;
  o.scored = true;
  o.score.root = root;
  o.score.evidence = evidence;
  o.score.sla = sla;
  o.metrics.samples = 1;
  o.metrics.fps = 30.0;
  return o;
}

eval::CaseOutcome mk_run_error(const std::string& id, int attempt) {
  eval::CaseOutcome o;
  o.id = id;
  o.attempt = attempt;
  o.scored = false;
  o.error_kind = "agent_timeout";
  return o;
}

bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

// ---------------------------------------------------------------- 失败模式分布

void test_failure_modes() {
  const std::vector<eval::Case> cases = {mk_case("a", "network_loss"),
                                         mk_case("b", "bandwidth_insufficient"),
                                         mk_case("c", "network_loss")};
  std::vector<eval::CaseOutcome> outcomes = {
      mk_outcome("a", 0, false, true, true),   // root 未命中
      mk_outcome("b", 0, false, false, false), // 三维全未命中 → 仍只计 1 次
      mk_outcome("c", 0, true, false, false),  // evidence + sla
      mk_run_error("c", 0),                     // 没判分 → __run__ 组
  };
  const auto fm = eval::summarize_failure_modes(cases, outcomes);
  check(fm.size() == 3, "fm: 两个根因组 + __run__ 组");
  if (fm.size() != 3) return;
  // 按 miss_count 降序；并列按名字升序
  check(fm[0].root_cause == "network_loss" && fm[0].miss_count == 2,
        "fm: network_loss 计 2 次（a 的 root + c 的 evidence/sla 各算一次尝试）");
  // miss 并列时按名字升序：'_' < 'b'，所以 __run__ 排在 bandwidth_insufficient 之前
  check(fm[1].root_cause == eval::kRunErrorGroup && fm[1].miss_count == 1,
        "fm: run_error 进 __run__ 组而不是混进某个根因");
  check(fm[2].root_cause == "bandwidth_insufficient" && fm[2].miss_count == 1,
        "fm: 三维全灭只计 1 次（不按层计 3 次）");
  const auto& net = fm[0];
  check(net.examples.size() == 2, "fm: 两类未命中各留一个 example");
  check(net.examples[0] == "a(root)", "fm: example 带未命中层（root）");
  check(net.examples[1] == "c(evidence,sla)", "fm: example 带多层且按固定顺序");
  check(fm[2].examples.size() == 1 && fm[2].examples[0] == "b(root,evidence,sla)",
        "fm: 三维全未命中的 example 写明三层");
  check(fm[1].examples.size() == 1 && fm[1].examples[0] == "c(run_error)",
        "fm: __run__ 的 example 明确标注 run_error");

  // 全过 → 空分布（不是"0 次失败"的一行占位）
  const auto none = eval::summarize_failure_modes({mk_case("x", "network_loss")},
                                                  {mk_outcome("x", 0, true, true, true)});
  check(none.empty(), "fm: 全过时分布为空");

  // examples 最多 3 个
  std::vector<eval::CaseOutcome> many;
  for (int i = 0; i < 5; ++i) {
    many.push_back(mk_outcome("n" + std::to_string(i), 0, false, true, true));
  }
  std::vector<eval::Case> many_cases;
  for (int i = 0; i < 5; ++i) {
    many_cases.push_back(mk_case("n" + std::to_string(i), "network_loss"));
  }
  const auto capped = eval::summarize_failure_modes(many_cases, many);
  check(capped.size() == 1 && capped[0].miss_count == 5 && capped[0].examples.size() == 3,
        "fm: miss_count 全量计数、examples 截到 3 个");
}

// ---------------------------------------------------------------- 稳定性

void test_stability() {
  // repeat=1：stddev 必须是 0（无跨轮信息），且报告里要能看出"没测稳定性"
  {
    const auto st = eval::compute_stability({mk_outcome("x", 0, true, false, true)}, 1);
    check(!st.informative(), "st: repeat=1 时 informative=false");
    check(st.repeat == 1 && st.rounds == 1, "st: repeat 与 rounds 回填");
    check(near(st.root_rate.mean, 1.0) && near(st.root_rate.stddev, 0.0),
          "st: repeat=1 的 stddev 为 0（不是 NaN）");
    check(near(st.evidence_rate.mean, 0.0), "st: repeat=1 的 evidence_rate 取该轮值");
    check(near(st.evidence_rate.min, 0.0) && near(st.evidence_rate.max, 0.0),
          "st: repeat=1 的 min=max=mean");
  }

  // repeat=3，构造已知序列：每轮 2 个 scored case
  //   轮0：root/ev/sla 全过 ×2 → 1.0 / 1.0 / 1.0 / pass 1.0
  //   轮1：1 过、1 个 root=false   → 0.5 / 1.0 / 1.0 / pass 0.5
  //   轮2：1 过、1 个三维全 false  → 0.5 / 0.5 / 0.5 / pass 0.5
  // 另加一个 run_error（run_error 不进分母、不占轮次样本）
  std::vector<eval::CaseOutcome> outcomes = {
      mk_outcome("a", 0, true, true, true),   mk_outcome("b", 0, true, true, true),
      mk_outcome("a", 1, true, true, true),   mk_outcome("b", 1, false, true, true),
      mk_outcome("a", 2, true, true, true),   mk_outcome("b", 2, false, false, false),
      mk_run_error("c", 2),
  };
  const auto st = eval::compute_stability(outcomes, 3);
  check(st.informative(), "st: repeat=3 时 informative=true");
  // root = [1.0, 0.5, 0.5]：mean=2/3，总体 stddev=sqrt(((1/3)^2+(1/6)^2+(1/6)^2)/3)=0.235702...
  check(near(st.root_rate.mean, 2.0 / 3.0), "st: root mean = 2/3");
  check(near(st.root_rate.stddev, std::sqrt(2.0 / 36.0)), "st: root 用总体标准差（除以 n）而不是样本标准差");
  check(near(st.root_rate.min, 0.5) && near(st.root_rate.max, 1.0), "st: root min/max");
  // evidence = [1.0, 1.0, 0.5]：mean=5/6，stddev 同 0.235702...
  check(near(st.evidence_rate.mean, 5.0 / 6.0), "st: evidence mean = 5/6");
  check(near(st.evidence_rate.min, 0.5) && near(st.evidence_rate.max, 1.0), "st: evidence min/max");
  // pass_k（逐轮）：[1.0, 0.5, 0.5] 与 root 同
  check(near(st.pass_k.mean, 2.0 / 3.0), "st: 逐轮通过率 mean = 2/3");

  // 某轮全是 run_error：该轮没有 scored 样本 → 不计入（不能当成 0% 通过）
  {
    const auto st2 = eval::compute_stability(
        {mk_outcome("a", 0, true, true, true), mk_run_error("a", 1), mk_run_error("a", 2)}, 3);
    check(near(st2.root_rate.mean, 1.0) && near(st2.root_rate.stddev, 0.0),
          "st: 只有 1 轮有样本时 stddev 为 0、不把整轮 run_error 当 0%");
    check(st2.rounds == 1 && !st2.informative(),
          "st: repeat=3 但只有 1 轮样本时 informative=false（截断不能装成稳定）");
  }

  // 完全没有 scored 样本：NaN（报告写 null），不是 0
  {
    const auto st3 = eval::compute_stability({mk_run_error("a", 0)}, 1);
    check(!eval::has_value(st3.root_rate.mean) && !eval::has_value(st3.root_rate.stddev),
          "st: 无样本写 NaN（不拿 0 冒充）");
  }
}

// ---------------------------------------------------------------- 报告渲染

eval::RunReport base_report() {
  eval::RunReport r;
  r.run_id = "run-abc";
  r.fingerprint = "0123456789abcdef0123456789abcdef";
  r.planned = 2;
  r.executed = 2;
  r.scored = 2;
  r.repeat = 3;
  r.root_rate = 0.75;
  r.evidence_rate = 0.5;
  r.sla_rate = 1.0;
  r.pass_k = 0.5;
  r.cost_yuan = 0.5;
  r.mttr_avg_s = 2.5;
  // 3 轮 × 2 case：a 每轮全过、b 每轮 root+evidence 未命中，用来验证「按轮聚合」
  r.cases.clear();
  for (int k = 0; k < 3; ++k) {
    r.cases.push_back(mk_outcome("a", k, true, true, true));
    r.cases.push_back(mk_outcome("b", k, false, false, true));
  }
  r.failure_modes = eval::summarize_failure_modes({mk_case("a", "network_loss"),
                                                   mk_case("b", "bandwidth_insufficient")},
                                                  r.cases);
  r.stability = eval::compute_stability(r.cases, 3);
  r.report_path = "runs/run-abc/report.md";
  r.report_json_path = "runs/run-abc/report.json";
  return r;
}

namespace {
bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}
}  // namespace

void test_markdown_sections() {
  eval::RunConfig cfg;
  cfg.suite_path = "suites/m4-diagnosis.json";
  cfg.agent_mode = "scripted";
  cfg.max_yuan = 5.0;
  const eval::Engine engine(cfg);

  auto r = base_report();
  // 没有 baseline：必须明说跳过，不能留空小节
  const std::string md = engine.render_markdown(r);
  check(has(md, "## 失败模式 TOP3"), "md: 有失败模式 TOP3 小节");
  check(has(md, "## 回归清单"), "md: 有回归清单小节");
  check(has(md, "本次没有 baseline，跳过"), "md: 无 baseline 时写跳过而不是空小节");
  check(has(md, "| 1 | bandwidth_insufficient | 3 | `b(root,evidence)` |"),
        "md: TOP3 表给出例子（只标真正未命中的层，miss 跨 3 轮累计）");
  check(has(md, "稳定性: repeat=3"), "md: 分层成功率区标注稳定性口径");
  check(has(md, "总体标准差"), "md: 写明标准差用总体标准差");
  check(has(md, "运行状态备注") == false, "md: 没有 run_notes 时不渲染空的运行状态段");

  // N-11：注入 trace 的落点要能在报告里找到（没有 trace 的 run 不渲染这一行）
  check(!has(md, "注入 trace"), "md: 没有 trace 的 run 不渲染注入 trace 段");
  {
    eval::RunReport with_trace = r;
    with_trace.cases.at(0).inject_trace = "runs/run-abc/inject-trace/a.trace";
    const std::string md_trace = engine.render_markdown(with_trace);
    check(has(md_trace, "注入 trace（1 个注入类 case 的逐包判定"),
          "md: 有 trace 时给出注入 trace 段与计数");
    check(has(md_trace, "runs/run-abc/inject-trace/a.trace"), "md: 注入 trace 段列出具体路径");
  }

  // N-05：六段延迟表里的 p95 必须是数字（不再是恒 n/a）。这里给每个 case 灌同一组分位，
  // 表里取中位后应逐位显示；同时确认脚注不再说"p95 写 n/a"。
  {
    eval::RunReport with_p95 = r;
    for (auto& c : with_p95.cases) {
      c.metrics.decode.p50 = 1.0;
      c.metrics.decode.p95 = 2.0;
      c.metrics.decode.p99 = 3.0;
      c.metrics.send.p95 = 12.5;
    }
    const std::string md5 = engine.render_markdown(with_p95);
    check(has(md5, "| decode | 1.00 | 2.00 | 3.00 |"), "md: decode 段 p95 渲染成数字");
    check(has(md5, "| send | n/a | 12.50 | n/a |"), "md: send 段 p95 渲染成数字");
    check(!has(md5, "p95 与 encode 段写 n/a"), "md: 脚注不再声称 p95 恒写 n/a");
  }

  // run_notes 与 failure_modes 分开渲染
  r.run_notes = {"truncated: 计划 6 次，实际执行 2 次"};
  r.failure_modes = {eval::FailureMode{"network_loss", 2, {"a(root)", "c(sla)"}}};
  const std::string md2 = engine.render_markdown(r);
  check(has(md2, "运行状态备注（不是失败模式"), "md: 运行状态单独一段");
  const auto top3_pos = md2.find("## 失败模式 TOP3");
  const auto notes_pos = md2.find("运行状态备注");
  const auto reg_pos = md2.find("## 回归清单");
  check(top3_pos < notes_pos && notes_pos < reg_pos, "md: 顺序 = 失败模式 → 运行状态 → 回归清单");
  const auto table_end = md2.find("## 回归清单");
  check(md2.substr(0, table_end).find("| truncated") == std::string::npos,
        "md: 运行状态不出现在失败模式的表格里");

  // 有 baseline：给出计数与条目
  r.regression.present = true;
  r.regression.baseline_run_id = "run-old";
  r.regression.source = "auto";
  r.regression.new_failures = {eval::RegressionEntry{"c", "A 3/3 过 → B 0/3 过"}};
  r.regression.fixed = {eval::RegressionEntry{"d", "A 0/3 过 → B 3/3 过"}};
  r.regression.still_failing = {eval::RegressionEntry{"e", "0/3 过"}};
  r.regression.comparable = false;
  r.regression.warnings = {"A/B 指纹不同"};
  const std::string md3 = engine.render_markdown(r);
  check(has(md3, "baseline: `run-old`"), "md: 回归清单写出 baseline run_id");
  check(has(md3, "新增失败 1，已修复 1，仍失败 1"), "md: 回归清单计数");
  check(has(md3, "### 新增失败（回归）") && has(md3, "`c`：A 3/3 过 → B 0/3 过"),
        "md: 新失败条目带 A→B");
  check(has(md3, "两次 run 不可比") && has(md3, "A/B 指纹不同"), "md: 不可比时给出警告");
}

// ---------------------------------------------------------------- JSON

void test_json_fields() {
  eval::RunConfig cfg;
  cfg.suite_path = "suites/m4-diagnosis.json";
  cfg.agent_mode = "scripted";
  const eval::Engine engine(cfg);
  auto r = base_report();
  r.run_notes = {"cost gate triggered at 6.0"};
  const auto j = nlohmann::json::parse(engine.render_json(r));

  check(j["failure_modes"].is_array(), "json: failure_modes 是数组");
  check(j["failure_modes"][0].is_object(), "json: failure_modes 元素是对象（不再是字符串）");
  check(j["failure_modes"][0].contains("root_cause") &&
            j["failure_modes"][0].contains("miss_count") &&
            j["failure_modes"][0].contains("examples"),
        "json: 对象含 root_cause / miss_count / examples");
  check(j["run_notes"].is_array() && j["run_notes"][0] == "cost gate triggered at 6.0",
        "json: run_notes 是独立的字符串数组");
  // N-11：没有 trace 的 case 写 null（"没有这个文件"不是空路径串）
  check(j["cases"][0].contains("inject_trace") && j["cases"][0]["inject_trace"].is_null(),
        "json: 无 trace 的 case 写 inject_trace=null");
  check(j["cases"][1].contains("inject_trace"), "json: 每个 case 都有 inject_trace 字段");
  check(j["stability"]["repeat"] == 3, "json: stability.repeat");
  check(j["stability"]["rounds"] == 3, "json: stability.rounds = 有样本的轮数");
  check(j["stability"]["informative"] == true, "json: repeat=3 时 informative=true");
  check(j["stability"]["stddev_kind"] == "population", "json: 标准差口径写进报告");
  check(j["stability"]["root_rate"]["mean"].is_number(), "json: stability.root_rate.mean 是数字");
  check(j["stability"]["root_rate"]["min"].is_number() &&
            j["stability"]["root_rate"]["max"].is_number(),
        "json: stability.root_rate 有 min/max");
  check(j["regression"]["present"] == false, "json: 无 baseline 时 regression.present=false");

  // repeat=1 / 无样本：null 而不是 0
  eval::RunReport r1;
  r1.run_id = "run-1";
  r1.repeat = 1;
  r1.stability = eval::compute_stability({}, 1);
  const auto j1 = nlohmann::json::parse(engine.render_json(r1));
  check(j1["stability"]["informative"] == false, "json: repeat=1 时 informative=false");
  check(j1["stability"]["root_rate"]["mean"].is_null(), "json: 无样本的 mean 写 null");
}

// ---------------------------------------------------------------- 终端摘要（快照）

void test_terminal_summary() {
  eval::RunConfig cfg;
  cfg.suite_path = "suites/m4-diagnosis.json";
  cfg.agent_mode = "scripted";
  cfg.max_yuan = 5.0;
  cfg.yuan_per_mtok = 2.0;
  const eval::Engine engine(cfg);

  auto r = base_report();
  const std::string s = engine.render_terminal_summary(r);
  check(has(s, "run-abc"), "summary: 有 run_id");
  check(has(s, "suites/m4-diagnosis.json"), "summary: 有 suite");
  check(has(s, "scripted"), "summary: 有模式");
  check(has(s, "执行 2 / 计划 2"), "summary: 有 executed/planned");
  check(has(s, "计分 2") && has(s, "run_error 0"), "summary: 有 scored/run_error");
  check(has(s, "根因 75.0%") && has(s, "证据 50.0%") && has(s, "SLA 100.0%"),
        "summary: 三级成功率");
  check(has(s, "pass^3"), "summary: 有 pass^k 且带 k");
  check(has(s, "pass^3  : 50.0%"), "summary: pass^k 数值");
  check(has(s, "¥0.5000"), "summary: 有成本");
  check(has(s, "MTTR    : 2.50 s"), "summary: 有 MTTR");
  check(has(s, "0123456789ab") && !has(s, "0123456789abc"), "summary: 指纹只给前 12 位");
  check(has(s, "runs/run-abc/report.md") && has(s, "runs/run-abc/report.json"),
        "summary: 有报告路径（md + json）");
  check(has(s, "±"), "summary: repeat>1 时给均值和离散度");
  int lines = 1;
  for (char c : s) {
    if (c == '\n') ++lines;
  }
  check(lines >= 11 && lines <= 16, "summary: 约 10-15 行（含收尾分隔线）");

  // repeat=1：必须明说"无稳定性信息"，不能把 stddev=0 读成稳定
  auto r1 = base_report();
  r1.repeat = 1;
  r1.cases = {mk_outcome("a", 0, true, true, true), mk_outcome("b", 0, true, true, true)};
  r1.stability = eval::compute_stability(r1.cases, 1);
  const std::string s1 = engine.render_terminal_summary(r1);
  check(has(s1, "repeat=1，只有 1 轮有样本，无稳定性信息"),
        "summary: repeat=1 标注无稳定性信息");

  // 未产出结果：摘要也要给出结论行
  eval::RunReport fatal;
  fatal.fatal_error = "state 写入失败：磁盘满";
  const std::string sf = engine.render_terminal_summary(fatal);
  check(has(sf, "未产出") && has(sf, "磁盘满"), "summary: fatal_error 走「未产出」分支");
}

}  // namespace

int main() {
  test_failure_modes();
  test_stability();
  test_markdown_sections();
  test_json_fields();
  test_terminal_summary();
  if (failures == 0) {
    std::printf("test_report: OK (5 groups)\n");
    return 0;
  }
  std::printf("test_report: %d failure(s)\n", failures);
  return 1;
}
