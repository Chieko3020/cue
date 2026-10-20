// M4b 测试：指纹 / state.jsonl 断点续跑 / 回归对比 / real 模式配置推导
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
namespace pipeline = cue::pipeline;
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

  // 2) outcome JSON 往返
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
      check(back->observations.size() == 1 && back->observations[0].tool == "query_metrics",
            "outcome 观测留痕往返");
      check(eval::evidence_matches("injected=59", back->observations), "落盘后证据仍可复核");
    }
    nlohmann::json bad{{"scored", true}};
    check(!eval::outcome_from_json(bad).has_value(), "缺 id 的 outcome 被拒");
  }

  // 3) state.jsonl append + load
  {
    const std::string path = (tmp / "state.jsonl").string();
    std::string err;
    check(eval::append_state_header(path, nlohmann::json{{"t", "run"}, {"run_id", "r1"}}, err),
          "写入 header");
    check(eval::append_state(path, 0, 0, make_outcome("c1", true, true, true), "fp-aaa", err),
          "写入 case0");
    check(eval::append_state(path, 0, 1, make_outcome("c1", true, false, true), "fp-aaa", err),
          "写入 case0 第二次");
    check(eval::append_state(path, 1, 0, make_outcome("c2", false, true, true), "fp-bbb", err),
          "写入 case1");

    eval::StateMap m;
    check(eval::load_state(path, m, err), "load_state 成功");
    check(m.size() == 3, "load_state 三条 outcome");
    check(m.count({0, 0}) == 1 && m.count({0, 1}) == 1 && m.count({1, 0}) == 1, "load_state 键正确");
    check(m[{0, 0}].outcome.score.all(), "load_state 保留三级判分");
    check(m[{0, 0}].fingerprint == "fp-aaa", "load_state 保留指纹");
    check(!m[{1, 0}].outcome.score.root, "load_state 保留失败判分");

    // 续跑靠 header 恢复 suite/mode/repeat（否则会拿 CLI 默认值去撞指纹）
    const auto hdr = eval::read_state_header(path);
    check(hdr.has_value(), "read_state_header 成功");
    if (hdr) {
      check(hdr->value("run_id", std::string()) == "r1", "header run_id 正确");
    }
    check(!eval::read_state_header((tmp / "nope.jsonl").string()).has_value(), "缺失 header 报错");

    eval::StateMap missing;
    check(!eval::load_state((tmp / "nope.jsonl").string(), missing, err), "缺失 state 报错而非静默");
  }

  // 4) 指纹：稳定 + 随内容变化（改源码/改 suite 必须失效）
  {
    const std::string suite_a = (tmp / "a.json").string();
    const std::string suite_b = (tmp / "b.json").string();
    write_text(suite_a, R"({"suite":"s","cases":[]})");
    write_text(suite_b, R"({"suite":"s","cases":[1]})");
    const auto c1 = make_case("c1");
    const auto c2 = make_case("c2");
    const std::string cfg_a = "cfg-a";
    const std::string cfg_b = "cfg-b";
    check(!eval::base_fingerprint(suite_a, cfg_a).empty(), "基线指纹非空");
    check(eval::base_fingerprint(suite_a, cfg_a) == eval::base_fingerprint(suite_a, cfg_a), "基线指纹稳定");
    check(eval::base_fingerprint(suite_a, cfg_a) != eval::base_fingerprint(suite_b, cfg_a), "suite 内容变化 → 指纹变化");
    check(eval::fingerprint_for(suite_a, c1, cfg_a) == eval::fingerprint_for(suite_a, c1, cfg_a), "case 指纹稳定");
    check(eval::fingerprint_for(suite_a, c1, cfg_a) != eval::fingerprint_for(suite_a, c2, cfg_a), "case id 变化 → 指纹变化");
    auto c3 = c1;
    c3.seed = 43;
    check(eval::fingerprint_for(suite_a, c1, cfg_a) != eval::fingerprint_for(suite_a, c3, cfg_a), "seed 变化 → 指纹变化");
    // 生效配置变化必须让指纹变化，否则续跑会跨 SUT/跨模型复用缓存（实测踩过）
    check(eval::fingerprint_for(suite_a, c1, cfg_a) != eval::fingerprint_for(suite_a, c1, cfg_b),
          "config 变化 → 指纹变化");
  }

  // 6) real 模式配置推导：注入档位只来自 case
  {
    eval::RunConfig cfg;
    cfg.cued_bin = "./build/cued";
    const auto c = make_case("netloss-5pct");
    const auto pc = eval::pipeline_config_for(c, cfg);
    check(pc.inject_drop == 5.0, "pipeline_config_for 注入来自 case.fault_value");
    check(pc.inject_seed == 42, "pipeline_config_for 种子来自 case.seed");
    check(pc.bitrate_kbps == 900 && pc.gop == 30, "pipeline_config_for 档位来自 case");
    check(pc.transcode && pc.width == 640 && pc.height == 480, "pipeline_config_for 转码来自 case");
    check(pc.cued_bin == "./build/cued", "pipeline_config_for cued 路径来自 config");

    auto no_fault = c;
    no_fault.fault_kind = "none";
    no_fault.fault_value = 5.0;
    check(eval::pipeline_config_for(no_fault, cfg).inject_drop == 0.0, "kind != drop 时不注入");
  }

  // 7) run_error 分类：agent 没跑完 ≠ 答错
  {
    agent::RunResult r;
    check(eval::classify_run_error(r).empty(), "正常结束 → 计分");
    r.error = "wall clock budget exhausted";
    r.turns = 5;
    check(eval::classify_run_error(r) == "agent_timeout", "墙钟超时 → agent_timeout");
    r.error = "cost budget exhausted";
    check(eval::classify_run_error(r) == "agent_cost_budget", "成本耗尽 → agent_cost_budget");
    r.error = "too many guard rejects";
    check(eval::classify_run_error(r) == "agent_guard_loop", "守卫死循环 → agent_guard_loop");
    agent::RunResult r0;
    r0.error = "curl: timeout";
    check(eval::classify_run_error(r0) == "llm_unavailable", "零轮次失败 → llm_unavailable");
    agent::RunResult rg;
    rg.final_stage = agent::Stage::GiveUp;
    check(eval::classify_run_error(rg).empty(), "显式 give_up 仍计分（算失败）");
  }

  // 8) m4-diagnosis suite：根因必须在 8 类枚举内（m2-constraints 的教训）
  {
    std::string err;
    const auto cases = eval::load_suite("suites/m4-diagnosis.json", err);
    check(err.empty(), "m4-diagnosis 可加载");
    check(cases.size() >= 2, "m4-diagnosis 至少 2 个 case");
    const std::vector<std::string> kEnum = {"network_loss", "network_delay",
                                            "bandwidth_insufficient", "encoder_too_slow",
                                            "resolution_too_high", "queue_backlog",
                                            "keyframe_storm", "clock_drift"};
    bool has_netloss_5pct = false;
    for (const auto& c : cases) {
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

  fs::remove_all(tmp, ec);
  if (failures == 0) {
    std::printf("test_state: OK\n");
    return 0;
  }
  std::printf("test_state: %d failure(s)\n", failures);
  return 1;
}
