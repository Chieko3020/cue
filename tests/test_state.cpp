// M4b 测试：指纹 / state.jsonl 断点续跑 / 并发锁 / 回归对比 / real 模式配置推导 /
// suite schema（畸形输入必须返回错误，不能 SIGABRT）
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cue/common/sha256.h"
#include "cue/eval/eval.h"
#include "cue/eval/state.h"

namespace eval = cue::eval;
namespace agent = cue::agent;
namespace fs = std::filesystem;

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
  o.diagnosis.evidence = {"injected=59", "fps=28.4"};
  o.diagnosis.confidence = 0.8;
  o.final_stage = "done";
  o.final_fps = 28.4;
  o.final_dropped = 7;
  o.final_samples = 12;
  o.metrics.fps = 28.4;
  o.metrics.dropped = 7;
  o.metrics.samples = 3;
  o.metrics.decode.p50 = 3.1;
  o.metrics.decode.p99 = 9.2;
  o.metrics.op.p99 = 12.5;
  o.metrics.encode.p50 = eval::kNoValue;
  o.mttr_s = 12.5;
  eval::Observation ob;
  ob.tool = "query_metrics";
  ob.text = "metrics={\"injected\":59}";
  ob.data = nlohmann::json{{"injected", 59}};
  o.observations.push_back(ob);
  return o;
}

eval::Case make_case(const std::string& id) {
  eval::Case c;
  c.id = id;
  c.seed = 42;
  c.fault_kind = "drop";
  c.fault_value = 5.0;
  c.bitrate_kbps = 900;
  c.gop = 30;
  c.transcode = true;
  c.width = 640;
  c.height = 480;
  return c;
}

bool write_text(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary);
  if (!out) return false;
  out << text;
  return static_cast<bool>(out);
}

nlohmann::json base_case() {
  return nlohmann::json{
      {"id", "c1"},
      {"seed", 1},
      {"fault", {{"kind", "drop"}, {"params", {{"inject_drop_pct", 5}}}}},
      {"pipeline", {{"bitrate_kbps", 1500}, {"gop", 60}}},
      {"expect",
       {{"root_cause", "network_loss"},
        {"evidence_candidates", {"injected", "fps"}},
        {"allowed_actions", {"inject_fault", "query_metrics"}},
        {"sla", {{"fps_min", 25}, {"dropped_max", 30}}}}},
      {"budget", {{"max_turns", 8}, {"max_yuan", 0.2}, {"max_wall_s", 60}}}};
}

nlohmann::json suite_of(const std::vector<nlohmann::json>& cases) {
  return nlohmann::json{{"suite", "t"}, {"version", 1}, {"cases", cases}};
}

// 畸形 suite 必须返回错误（而不是让 nlohmann 类型异常穿透成 SIGABRT）
bool rejected(const fs::path& path, const nlohmann::json& suite) {
  if (!write_text(path.string(), suite.dump())) return false;
  return !eval::load_suite(path.string()).has_value();
}

}  // namespace

int main() {
  const fs::path tmp = fs::temp_directory_path() / "cue-test-state";
  std::error_code ec;
  fs::remove_all(tmp, ec);
  fs::create_directories(tmp, ec);

  // 1) sha256 已知向量（NIST 标准）
  {
    check(cue::sha256_hex("abc") ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "sha256('abc')");
    check(cue::sha256_hex("") ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "sha256('')");
    // 超过 64 字节的分块边界（55/56/64 是补位的三处边界）
    const std::string long_input(1000, 'a');
    check(cue::sha256_hex(long_input).size() == 64, "sha256 长输入长度");
    check(cue::sha256_file("/nonexistent/file") == "", "sha256_file 缺失文件返回空");
  }

  // 2) outcome JSON 往返（含分位与 MTTR；NaN 落成 null 再读回 NaN）
  {
    const auto o = make_outcome("c1", true, false, true);
    const auto back = eval::outcome_from_json(eval::outcome_to_json(o));
    check(back.has_value(), "outcome 反序列化成功");
    if (back) {
      check(back->id == o.id, "outcome id 往返");
      check(back->score.root && !back->score.evidence && back->score.sla, "outcome 三级判分往返");
      check(back->diagnosis.evidence.size() == 2, "outcome 证据往返");
      check(back->diagnosis.root_cause == "network_loss", "outcome 根因往返");
      check(back->cost_yuan > 0.049 && back->cost_yuan < 0.051, "outcome 成本往返");
      check(back->final_fps > 28.3 && back->final_fps < 28.5, "outcome 重测指标往返");
      check(back->metrics.decode.p99 > 9.1 && back->metrics.decode.p99 < 9.3, "分位往返");
      check(!eval::has_value(back->metrics.encode.p50), "取不到的分位仍为 NaN");
      check(back->mttr_s > 12.4 && back->mttr_s < 12.6, "MTTR 往返");
      check(back->observations.size() == 1 && back->observations[0].tool == "query_metrics",
            "outcome 观测留痕往返");
      check(eval::evidence_matches("injected=59", back->observations), "落盘后证据仍可复核");
    }
    nlohmann::json bad{{"scored", true}};
    check(!eval::outcome_from_json(bad).has_value(), "缺 id 的 outcome 被拒");
    nlohmann::json bad_type{{"id", "x"}, {"cost_yuan", "0.1"}};
    check(!eval::outcome_from_json(bad_type).has_value(), "类型错的 outcome 被拒（不抛异常）");
  }

  // 3) state.jsonl append + load
  {
    const std::string path = (tmp / "state.jsonl").string();
    check(eval::append_run_header(path, nlohmann::json{{"t", "run"}, {"run_id", "r1"}}).has_value(),
          "写入 header");
    check(eval::append_state(path, 0, 0, make_outcome("c1", true, true, true), "fp-aaa").has_value(),
          "写入 case0");
    check(eval::append_state(path, 0, 1, make_outcome("c1", true, false, true), "fp-aaa").has_value(),
          "写入 case0 第二次");
    check(eval::append_state(path, 1, 0, make_outcome("c2", false, true, true), "fp-bbb").has_value(),
          "写入 case1");

    auto m = eval::load_state(path);
    check(m.has_value(), "load_state 成功");
    if (m) {
      check(m->size() == 3, "load_state 三条 outcome");
      check(m->count({0, 0}) == 1 && m->count({0, 1}) == 1 && m->count({1, 0}) == 1, "load_state 键正确");
      check((*m)[{0, 0}].outcome.score.all(), "load_state 保留三级判分");
      check((*m)[{0, 0}].fingerprint == "fp-aaa", "load_state 保留指纹");
      check(!(*m)[{1, 0}].outcome.score.root, "load_state 保留失败判分");
    }

    const auto hdr = eval::read_state_header(path);
    check(hdr.has_value(), "read_state_header 成功");
    if (hdr) check(hdr->value("run_id", std::string()) == "r1", "header run_id 正确");
    check(!eval::read_state_header((tmp / "nope.jsonl").string()).has_value(), "缺失 header 报错");
    check(!eval::load_state((tmp / "nope.jsonl").string()).has_value(), "缺失 state 报错而非静默");
  }

  // 4) 崩溃截断的最后一行按无害丢弃；中间行坏了仍报错（P0#8）
  {
    const std::string truncated = (tmp / "truncated.jsonl").string();
    write_text(truncated, std::string(R"({"t":"run","run_id":"r2"})") + "\n" +
                              R"({"t":"case","ci":0,"k":0,"fp":"fp","outcome":{"id":"c1","scored":true}})" +
                              "\n" + R"({"t":"case","ci":1,"k":0,"fp":"fp","outc)");
    auto m = eval::load_state(truncated);
    check(m.has_value() && m->size() == 1, "截断的最后一行被丢弃，其余可续跑");

    const std::string middle_bad = (tmp / "middle-bad.jsonl").string();
    write_text(middle_bad, std::string(R"({"t":"run","run_id":"r3"})") + "\n" + "not json\n" +
                               R"({"t":"case","ci":0,"k":0,"fp":"fp","outcome":{"id":"c1","scored":true}})" + "\n");
    check(!eval::load_state(middle_bad).has_value(), "中间行不是 JSON 仍报错");

    const std::string bad_ci = (tmp / "bad-ci.jsonl").string();
    write_text(bad_ci, R"({"t":"case","ci":"0","k":0,"fp":"fp","outcome":{"id":"c1"}})" "\n");
    check(!eval::load_state(bad_ci).has_value(), "ci 类型错返回错误而不是 SIGABRT");
  }

  // 5) 并发锁：同一 run 第二个进程必须被拒（否则双份计费）
  {
    eval::RunLock a;
    check(a.acquire(tmp.string()).has_value(), "第一个进程拿到 run 锁");
    eval::RunLock b;
    auto second = b.acquire(tmp.string());
    check(!second.has_value(), "第二个进程被 run 锁拒绝");
    if (!second) check(second.error().find("并发") != std::string::npos, "锁错误说明是并发占用");
  }

  // 6) 指纹：稳定 + 随内容变化（改源码/改 suite/改配置必须失效）
  {
    const std::string suite_a = (tmp / "a.json").string();
    const std::string suite_b = (tmp / "b.json").string();
    write_text(suite_a, R"({"suite":"s","cases":[]})");
    write_text(suite_b, R"({"suite":"s","cases":[1]})");
    const auto c1 = make_case("c1");
    const auto c2 = make_case("c2");
    const std::string cfg_a = "cfg-a";
    const std::string cfg_b = "cfg-b";
    const std::string base = eval::base_fingerprint(suite_a, cfg_a);
    check(!base.empty(), "基线指纹非空");
    check(base == eval::base_fingerprint(suite_a, cfg_a), "基线指纹稳定");
    check(base != eval::base_fingerprint(suite_b, cfg_a), "suite 内容变化 → 指纹变化");
    check(eval::fingerprint_from_base(base, c1) == eval::fingerprint_for(suite_a, c1, cfg_a),
          "case 指纹 = 基线 ⊕ id ⊕ seed");
    check(eval::fingerprint_from_base(base, c1) == eval::fingerprint_from_base(base, c1), "case 指纹稳定");
    check(eval::fingerprint_from_base(base, c1) != eval::fingerprint_from_base(base, c2),
          "case id 变化 → 指纹变化");
    auto c3 = c1;
    c3.seed = 43;
    check(eval::fingerprint_from_base(base, c1) != eval::fingerprint_from_base(base, c3),
          "seed 变化 → 指纹变化");
    // 生效配置变化必须让指纹变化，否则续跑会跨 SUT/跨模型复用缓存（实测踩过）
    check(eval::fingerprint_for(suite_a, c1, cfg_a) != eval::fingerprint_for(suite_a, c1, cfg_b),
          "config 变化 → 指纹变化");
  }

  // 7) real 模式配置推导：注入档位只来自 case
  {
    eval::RunConfig cfg;
    cfg.cued_bin = "./build/cued";
    const auto c = make_case("netloss-5pct");
    const std::string run_dir = "runs/run-1";
    const auto pc = eval::pipeline_config_for(c, cfg, run_dir);
    check(pc.inject_drop == 5.0, "pipeline_config_for 注入来自 case.fault_value");
    check(pc.inject_seed == 42, "pipeline_config_for 种子来自 case.seed");
    check(pc.bitrate_kbps == 900 && pc.gop == 30, "pipeline_config_for 档位来自 case");
    check(pc.transcode && pc.width == 640 && pc.height == 480, "pipeline_config_for 转码来自 case");
    check(pc.cued_bin == "./build/cued", "pipeline_config_for cued 路径来自 config");

    auto no_fault = c;
    no_fault.fault_kind = "none";
    no_fault.fault_value = 5.0;
    check(eval::pipeline_config_for(no_fault, cfg, run_dir).inject_drop == 0.0,
          "kind != drop 时不注入");

    // N-11：注入 trace 按 run 目录 + case 名落点，同 seed 的两次 run 因此可 diff
    check(pc.inject_trace == run_dir + "/inject-trace/netloss-5pct.trace",
          "注入类 case 的 trace 落在 <run_dir>/inject-trace/<case>.trace");
    check(eval::pipeline_config_for(no_fault, cfg, run_dir).inject_trace.empty(),
          "非注入 case 不落 trace（不无条件给默认路径）");
    check(eval::pipeline_config_for(c, cfg, "").inject_trace.empty(),
          "没有 run 目录时不落 trace（不回退到 /tmp 或当前目录）");
    // cued 只有出向视频丢包与抖动注入两种逐包记录点：其余注入给 trace 只会得到空文件
    auto ingress = c;
    ingress.fault_kind = "ingress_drop";
    check(eval::pipeline_config_for(ingress, cfg, run_dir).inject_trace.empty(),
          "上行丢包在 cued 里没有逐包记录点，不落 trace");
    auto jitter = c;
    jitter.fault_kind = "jitter";
    check(!eval::pipeline_config_for(jitter, cfg, run_dir).inject_trace.empty(),
          "抖动注入有 j 行，落 trace");
  }

  // 8) run_error 分类：agent 没跑完 ≠ 答错
  {
    agent::RunResult r;
    check(eval::classify_run_error(r).empty(), "正常结束 → 计分");
    r.error = "wall clock budget exhausted";
    r.turns = 5;
    check(eval::classify_run_error(r) == "agent_timeout", "墙钟超时 → agent_timeout");
    r.error = "cost budget exhausted";
    check(eval::classify_run_error(r) == "agent_cost_budget", "成本耗尽 → agent_cost_budget");
    r.error = "turn budget exhausted";
    check(eval::classify_run_error(r) == "agent_turn_budget", "轮次耗尽 → agent_turn_budget");
    r.error = "too many guard rejects";
    check(eval::classify_run_error(r) == "agent_guard_loop", "守卫死循环 → agent_guard_loop");
    // 上下文裁完仍超限：必须优先于「零轮次」判定，否则首轮就超限的 run 会被误归成 llm_unavailable
    agent::RunResult ro;
    ro.error = "context_overflow";
    check(eval::classify_run_error(ro) == "context_overflow", "上下文裁完仍超限 → context_overflow");
    agent::RunResult r0;
    r0.error = "curl: timeout";
    check(eval::classify_run_error(r0) == "llm_unavailable", "零轮次失败 → llm_unavailable");
    agent::RunResult rg;
    rg.final_stage = agent::Stage::GiveUp;
    check(eval::classify_run_error(rg).empty(), "显式 give_up 仍计分（算失败）");
  }

  // 9) suite schema：畸形输入一律报错（静默降级会产出假绿灯）
  {
    const fs::path p = tmp / "suite-check.json";
    check(!rejected(p, suite_of({base_case()})), "合法 suite 能加载");
    auto c = base_case();
    c["seed"] = "42";  // 曾经这里 SIGABRT
    check(rejected(p, suite_of({c})), "seed 类型错 → 报错");
    c = base_case();
    c.erase("seed");
    check(rejected(p, suite_of({c})), "缺 seed → 报错");
    c = base_case();
    c["pipeline"]["profile"] = "p1080p30";
    check(rejected(p, suite_of({c})), "pipeline 未知字段 → 报错（不再静默丢弃）");
    c = base_case();
    c["fault"]["kind"] = "fec";
    check(rejected(p, suite_of({c})), "不支持的 fault.kind → 报错（不再静默变基线）");
    c = base_case();
    c["expect"].erase("sla");
    check(rejected(p, suite_of({c})), "缺 expect.sla → 报错（不允许 SLA 恒真）");
    c = base_case();
    c["expect"]["root_cause"] = "fec_ghost_frame";
    check(rejected(p, suite_of({c})), "枚举外根因且未标 manual_only → 报错");
    c = base_case();
    c["expect"]["root_cause"] = "fec_ghost_frame";
    c["manual_only"] = true;
    check(!rejected(p, suite_of({c})), "manual_only 豁免枚举限制");
    c = base_case();
    c["budget"]["max_yuan"] = 0;
    check(rejected(p, suite_of({c})), "max_yuan<=0 → 报错");
    c = base_case();
    c["expect"]["allowed_actions"] = nlohmann::json::array({"delete_everything"});
    check(rejected(p, suite_of({c})), "allowed_actions 含未知工具 → 报错");
    check(rejected(p, suite_of({base_case(), base_case()})), "重复 case id → 报错");
  }

  // 10) m4-diagnosis suite：根因必须在 8 类枚举内
  {
    auto cases = eval::load_suite("suites/m4-diagnosis.json");
    check(cases.has_value(), "m4-diagnosis 可加载");
    if (cases) {
      check(cases->size() >= 2, "m4-diagnosis 至少 2 个 case");
      const std::vector<std::string> kEnum = {"network_loss", "network_delay",
                                              "bandwidth_insufficient", "encoder_too_slow",
                                              "resolution_too_high", "queue_backlog",
                                              "keyframe_storm", "clock_drift"};
      bool has_netloss_5pct = false;
      for (const auto& c : *cases) {
        bool in_enum = false;
        for (const auto& e : kEnum) in_enum = in_enum || (c.expect_root_cause == e);
        check(in_enum, ("case 根因在枚举内: " + c.id).c_str());
        check(!c.prompt.empty(), ("case 有 prompt: " + c.id).c_str());
        check(c.max_turns > 0 && c.max_yuan > 0 && c.max_wall_s > 0, ("case 有预算: " + c.id).c_str());
        if (c.id == "netloss-5pct") {
          has_netloss_5pct = true;
          check(c.fault_kind == "drop" && c.fault_value == 5.0, "netloss-5pct 注入 5%");
          check(c.sla_fps_min >= 25.0, "netloss-5pct 有 fps SLA");
        }
      }
      check(has_netloss_5pct, "netloss-5pct 存在");
    }
  }

  fs::remove_all(tmp, ec);
  // C05: persistence retains raw scores; eligibility is derived on read.
  {
    auto raw = make_outcome("excluded", true, true, true);
    raw.injection = eval::judge_injection("rate", 0, 0);
    const auto encoded = eval::outcome_to_json(raw);
    const auto read = eval::outcome_from_json(encoded);
    check(read && read->scored && read->score.all() && !eval::rate_eligible(*read) &&
              read->injection.checked && !read->injection.effective,
          "C05 roundtrip retains judgement and injection exclusion");
    auto old = encoded;
    old.erase("injection");
    const auto legacy = eval::outcome_from_json(old);
    check(legacy && legacy->scored && !legacy->injection.checked && eval::rate_eligible(*legacy),
          "C05 legacy missing injection remains unknown and compatible");
    eval::RunReport report;
    report.cases = {*read};
    eval::summarize_scores(report);
    check(report.raw_scored == 1 && report.scored == 0 && report.injection_ineffective == 1,
          "C05 reloaded cached outcome keeps denominator exclusion");
  }

  if (failures == 0) {
    std::printf("test_state: OK\n");
    return 0;
  }
  std::printf("test_state: %d failure(s)\n", failures);
  return 1;
}
