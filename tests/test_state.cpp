// M4b 测试：指纹 / state.jsonl 断点续跑 / 并发锁 / 回归对比 / real 模式配置推导 /
// suite schema（畸形输入必须返回错误，不能 SIGABRT）
#include <cstdio>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "cue/common/sha256.h"
#include "cue/eval/eval.h"
#include "cue/eval/state.h"

namespace eval = cue::eval;
namespace agent = cue::agent;
namespace fs = std::filesystem;

namespace {

int failures = 0;
volatile std::sig_atomic_t fixture_stop = 0;
void stop_fixture(int) { fixture_stop = 1; }

// Invoked by tiny, replaceable shebang entry files. The interpreter is this test
// binary, already part of base_fingerprint; no Python/shell/FFmpeg dependency.
int run_entry_fixture(const std::string& entry) {
  std::signal(SIGTERM, stop_fixture);
  std::signal(SIGINT, stop_fixture);
  const auto role = fs::path(entry).filename().string();
  const bool cued = role == "cued";
  const bool capture = role == "capture";
  int listener = -1, peer = -1;
  const auto directory = fs::path("/tmp/cue") / std::to_string(::getpid());
  if (cued) {
    fs::create_directories(directory);
    listener = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto path = (directory / "control.sock").string();
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (listener < 0 || ::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ||
        ::listen(listener, 1)) return 2;
  }
  const nlohmann::json metrics{{"fps", 30}, {"frames_in", 99}, {"frames_out", 99},
                               {"injected", 0}, {"dropped", 0}};
  while (!fixture_stop) {
    if (cued) {
      if (peer < 0) peer = ::accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
      char buffer[65536];
      const auto n = peer < 0 ? -1 : ::recv(peer, buffer, sizeof(buffer), MSG_DONTWAIT);
      if (n > 0) {
        const auto request = nlohmann::json::parse(buffer, buffer + n, nullptr, false);
        if (!request.is_object()) return 3;
        const auto result = request.value("op", std::string()) == "metrics.query"
                                ? metrics : nlohmann::json::object();
        const auto reply = nlohmann::json{{"id", request.at("id")}, {"ok", true},
                                          {"result", result}}.dump();
        if (::send(peer, reply.data(), reply.size(), MSG_NOSIGNAL) < 0) break;
      }
      std::printf("METRICS %s\n", metrics.dump().c_str());
    } else if (!capture) {
      std::puts("[cue-event] {\"ev\":\"probe.metric\",\"data\":{\"fps\":30,\"frames\":99,\"decoded\":99,\"synced\":true}}");
    }
    std::fflush(stdout);
    ::usleep(20000);
  }
  if (peer >= 0) ::close(peer);
  if (listener >= 0) { ::close(listener); fs::remove_all(directory); }
  return 0;
}
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

int main(int argc, char** argv) {
  if (argc > 2 && std::string(argv[1]) == "--c17-fixture") return run_entry_fixture(argv[2]);
  const fs::path tmp = fs::temp_directory_path() / ("cue-test-state-" + std::to_string(::getpid()));
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

  // C17: actual entry bytes, including default probe resolution, drive Engine/state resume.
  {
    const auto entries = tmp / "entries";
    fs::create_directories(entries);
    eval::RunConfig cfg;
    cfg.agent_mode = "real";
    cfg.out_dir = (tmp / "identity-runs").string();
    cfg.suite_path = (tmp / "identity-suite.json").string();
    cfg.cued_bin = (entries / "cued").string();
    cfg.capture_bin = (entries / "capture").string();
    const auto probe = entries / "cue-probe";
    const auto explicit_probe = entries / "explicit-probe";
    write_text(cfg.suite_path, "suite bytes");
    write_text(cfg.cued_bin, "cued-v1");
    write_text(probe.string(), "probe-1");
    write_text(explicit_probe.string(), "other-1");
    write_text(cfg.capture_bin, "capture-1");
    auto hash = [](const eval::RunConfig& config) { return eval::Engine(config).config_hash(); };
    const auto initial = hash(cfg);
    check(!initial.empty() && initial == hash(cfg), "C17 unchanged entry identity is stable");
    check(eval::pipeline_config_for(make_case("default"), cfg, "").probe_bin == probe.string(),
          "C17 launch and hash use the same default sibling probe");
    const auto stamp = fs::last_write_time(probe);
    write_text(probe.string(), "probe-2");
    fs::last_write_time(probe, stamp);
    check(initial != hash(cfg), "C17 default probe same size/mtime byte replacement invalidates");
    write_text(probe.string(), "probe-1");
    check(initial == hash(cfg), "C17 identical bytes ignore mtime");
    write_text(cfg.cued_bin, "cued-v2");
    check(initial != hash(cfg), "C17 cued entry bytes remain part of identity");
    write_text(cfg.cued_bin, "cued-v1");
    const auto old_cwd = fs::current_path();
    fs::current_path(entries);
    auto bare = cfg;
    bare.cued_bin = "cued";
    check(eval::pipeline_config_for(make_case("bare"), bare, "").probe_bin == "cue-probe" &&
              !hash(bare).empty(), "C17 bare execv paths resolve in cwd without PATH lookup");
    fs::current_path(old_cwd);
    cfg.probe_bin = explicit_probe.string();
    const auto explicit_hash = hash(cfg);
    const auto same_bytes_path = entries / "same-probe";
    write_text(same_bytes_path.string(), "other-1");
    auto moved = cfg;
    moved.probe_bin = same_bytes_path.string();
    check(explicit_hash != hash(moved), "C17 equal bytes at a different entry path invalidate");
    const auto bytepath_a = entries / (std::string("probe-") + char(0xff));
    const auto bytepath_b = entries / (std::string("probe-") + char(0xfe));
    write_text(bytepath_a.string(), "other-1");
    write_text(bytepath_b.string(), "other-1");
    moved.probe_bin = bytepath_a.string();
    const auto bytepath_hash = hash(moved);
    moved.probe_bin = bytepath_b.string();
    check(!bytepath_hash.empty() && bytepath_hash != hash(moved),
          "C17 distinct non-UTF8 path bytes serialize losslessly without exceptions");
    check(eval::pipeline_config_for(make_case("identity"), cfg, "").probe_bin == cfg.probe_bin,
          "C17 launch honors explicit probe override");
    write_text(explicit_probe.string(), "other-2");
    check(explicit_hash != hash(cfg), "C17 explicit probe bytes invalidate");
    write_text(probe.string(), "unused!");
    const auto explicit_changed = hash(cfg);
    write_text(probe.string(), "ignored");
    check(explicit_changed == hash(cfg), "C17 overridden default probe is unused");
    const auto disabled = hash(cfg);
    write_text(cfg.capture_bin, "capture-2");
    check(disabled == hash(cfg), "C17 producer none ignores capture bytes");
    cfg.producer = "lavfi";
    const auto enabled = hash(cfg);
    write_text(cfg.capture_bin, "capture-3");
    check(enabled != hash(cfg), "C17 enabled capture bytes invalidate");
    const auto target = entries / "probe-target";
    write_text(target.string(), "target-1");
    fs::remove(explicit_probe);
    fs::create_symlink(target, explicit_probe);
    const auto symlink_hash = hash(cfg);
    const auto replacement = entries / "replacement";
    write_text(replacement.string(), "target-2");
    fs::rename(replacement, target);
    check(symlink_hash != hash(cfg), "C17 symlink target bytes invalidate");

    // Controlled executable fixtures exercise the real Process backend. They emit
    // synthetic metrics only: this is not media/device or model acceptance.
    const std::string cued_fixture = "#!" + fs::read_symlink("/proc/self/exe").string() +
                                    " --c17-fixture\n# version-1\n";
    const std::string probe_fixture = cued_fixture;
    const std::string capture_fixture = cued_fixture;
    auto executable = [](const std::string& path, const std::string& bytes) {
      check(write_text(path, bytes), "C17 write executable fixture");
      fs::permissions(path, fs::perms::owner_all);
    };
    auto version2 = [](std::string bytes) {
      bytes.replace(bytes.find("version-1"), 9, "version-2");
      return bytes;
    };
    executable(cfg.cued_bin, cued_fixture);
    executable(target.string(), probe_fixture);
    executable(cfg.capture_bin, capture_fixture);
    fs::remove(bytepath_a);
    fs::create_symlink(target, bytepath_a);
    cfg.probe_bin = bytepath_a.string();
    cfg.agent_mode = "scripted";
    cfg.record_tools_dir = (tmp / "process-recordings").string();
    cfg.sla_settle_ms = 0;

    // Successful Engine outcomes really pass through append/load/reuse. File length proves
    // reuse without relying on a private cache helper or a claimed fingerprint alone.
    eval::Case c;
    c.id = "identity";
    c.fault_kind = "none";
    c.expect_root_cause = "network_loss";
    c.evidence_candidates = {"fps"};
    c.sla_fps_min = 1;
    cfg.producer = "none";
    const auto first = eval::Engine(cfg).run({c});
    check(first.fatal_error.empty() && first.cases.size() == 1 && first.cases[0].score.all(),
          "C17 Engine creates successful Process cache with non-UTF8 entry path");
    cfg.resume_run_id = first.run_id;
    const auto state = fs::path(cfg.out_dir) / first.run_id / "state.jsonl";
    const auto size = fs::file_size(state);
    const auto resumed = eval::Engine(cfg).run({c});
    check(resumed.fatal_error.empty() && resumed.executed == 1 && fs::file_size(state) == size,
          "C17 unchanged Engine resume reuses successful case without append");
    auto gated_cfg = cfg;
    gated_cfg.max_yuan = 0.001;
    const auto gated = eval::Engine(gated_cfg).run({c});
    check(gated.fatal_error.empty() && gated.gated && gated.fingerprint == first.fingerprint &&
              fs::file_size(state) == size, "C17 lowered cost gate still applies to reused outcomes");
    const auto runtime_stamp = fs::last_write_time(target);
    executable(target.string(), version2(probe_fixture));
    fs::last_write_time(target, runtime_stamp);
    const auto changed = eval::Engine(cfg).run({c});
    check(changed.fatal_error.empty() && changed.cases.size() == 1 && changed.cases[0].score.all() &&
              changed.fingerprint != first.fingerprint &&
              fs::file_size(state) > size, "C17 probe replacement reruns Engine cached case");
    cfg.producer = "lavfi";
    const auto capture_first = eval::Engine(cfg).run({c});
    const auto capture_size = fs::file_size(state);
    executable(cfg.capture_bin, version2(capture_fixture));
    const auto capture_next = eval::Engine(cfg).run({c});
    check(capture_first.fatal_error.empty() && capture_first.cases.size() == 1 &&
              capture_first.cases[0].score.all() && capture_next.fatal_error.empty() &&
              capture_next.cases.size() == 1 && capture_next.cases[0].score.all() &&
              capture_first.fingerprint != capture_next.fingerprint && fs::file_size(state) > capture_size,
          "C17 capture replacement reruns Engine cached case");
    const auto valid_size = fs::file_size(state);
    fs::remove(target);
    const auto missing = eval::Engine(cfg).run({c});
    check(missing.fatal_error == "process entry identity unreadable: probe" &&
              missing.executed == 0 && fs::file_size(state) == valid_size,
          "C17 missing required probe rejects successful cache before reuse/write");
    fs::create_directory(target);
    const auto directory = eval::Engine(cfg).run({c});
    check(!directory.fatal_error.empty() && directory.executed == 0 && fs::file_size(state) == valid_size,
          "C17 directory required probe rejects successful cache before reuse/write");
    fs::remove(target);
    executable(target.string(), version2(probe_fixture));
    fs::permissions(target, fs::perms::none);
    const auto unreadable = eval::Engine(cfg).run({c});
    check(!unreadable.fatal_error.empty() && unreadable.executed == 0 &&
              fs::file_size(state) == valid_size,
          "C17 permission-denied probe refuses successful cache before reuse/write");
    fs::permissions(target, fs::perms::owner_all);
    for (const auto& required : {cfg.cued_bin, cfg.capture_bin}) {
      const auto saved = required + ".saved";
      fs::rename(required, saved);
      const auto absent = eval::Engine(cfg).run({c});
      check(!absent.fatal_error.empty() && absent.executed == 0 && fs::file_size(state) == valid_size,
            "C17 missing cued/capture also refuses successful cache");
      fs::rename(saved, required);
    }
    auto playback = cfg;
    playback.producer = "none";
    playback.record_tools_dir.clear();
    playback.replay_tools_dir = cfg.record_tools_dir;
    playback.resume_run_id.clear();
    const auto replay_case = eval::Engine(playback).run({c});
    playback.cued_bin = "/missing-cued";
    playback.probe_bin = "/missing-probe";
    playback.capture_bin = "/missing-capture";
    const auto replay_with_endpoint = eval::Engine(playback).run({c});
    check(replay_case.fatal_error.empty() && replay_case.cases.size() == 1 &&
              replay_case.cases[0].score.all() && replay_case.cases[0].tool_misses == 0 &&
              replay_with_endpoint.fatal_error.empty() && replay_with_endpoint.cases.size() == 1 &&
              replay_with_endpoint.cases[0].score.all() && replay_with_endpoint.cases[0].tool_misses == 0 &&
              replay_case.fingerprint == replay_with_endpoint.fingerprint,
          "C17 actual recorded Engine replay ignores unused local entry paths");
    cfg.producer = "none";
    const auto uncapped = hash(cfg);
    cfg.max_yuan = 0.001;
    cfg.out_dir += "-other";
    check(uncapped == hash(cfg), "C17 cost gate and output directory remain outside identity");
    cfg.endpoint = "http://127.0.0.1:1";
    const auto http = hash(cfg);
    auto http_config = cfg;
    http_config.resume_run_id.clear();
    check(!http.empty() && eval::Engine(http_config).run({}).fatal_error.empty(),
          "C17 HTTP fingerprint/run needs no Process entries");
    write_text(cfg.cued_bin, "cued-v2");
    write_text(target.string(), "target-4");
    fs::remove(cfg.capture_bin);
    check(http == hash(cfg), "C17 HTTP ignores all local entry contents and missing capture");
    const auto recordings = tmp / "identity-recordings";
    fs::create_directory(recordings);
    write_text((recordings / "identity.jsonl").string(), "record-1");
    cfg.replay_tools_dir = recordings.string();
    const auto replay = hash(cfg);
    cfg.endpoint = "http://127.0.0.1:2";
    cfg.cued_bin = "/missing-cued";
    cfg.probe_bin = "/missing-probe";
    cfg.capture_bin = "/missing-capture";
    check(replay == hash(cfg), "C17 tools replay precedence ignores endpoint and local entries");
    const auto replay_http_policy = hash(cfg);
    cfg.endpoint.clear();
    check(replay_http_policy != hash(cfg), "C17 replay retains effective HTTP/Process sampling policy");
    cfg.endpoint = "http://127.0.0.1:2";
    write_text((recordings / "identity.jsonl").string(), "record-2");
    check(replay != hash(cfg), "C17 tools recording bytes still invalidate");
    cfg.agent_mode = "real";
    cfg.llm_mode = "replay";
    cfg.cassette_dir = recordings.string();
    const auto cassette = hash(cfg);
    write_text((recordings / "llm.jsonl").string(), "cassette-1");
    check(cassette != hash(cfg), "C17 real LLM cassette contents remain part of identity");
    cfg.agent_mode = "scripted";
    cfg.replay_tools_dir.clear();
    cfg.record_tools_dir.clear();
    cfg.endpoint.clear();
    cfg.resume_run_id.clear();
    const auto selftest = eval::Engine(cfg).run({c});
    check(selftest.fatal_error.empty() && selftest.executed == 1 && selftest.cases[0].score.all(),
          "C17 plain scripted selftest uses no local executable identities");
    cfg.agent_mode = "real";
    cfg.producer = "none";
    auto skipped_case = c;
    skipped_case.manual_only = true;
    const auto skipped = eval::Engine(cfg).run({skipped_case});
    check(skipped.fatal_error.empty() && skipped.executed == 0 && skipped.cases.empty() &&
              !skipped.run_notes.empty(), "C17 all-manual-only real suite skips without unused entries");
    check(hash(cfg).empty(), "C17 public config-only hash still assumes runnable real cases");
    cfg.resume_run_id = skipped.run_id;
    const auto skip_state = fs::path(cfg.out_dir) / skipped.run_id / "state.jsonl";
    const auto skip_size = fs::file_size(skip_state);
    auto runnable_case = c;
    runnable_case.id = "runnable";
    const auto mixed = eval::Engine(cfg).run({skipped_case, runnable_case});
    check(!mixed.fatal_error.empty() && mixed.executed == 0 && fs::file_size(skip_state) == skip_size,
          "C17 mixed real suite still rejects required missing entry before resume/write");
  }

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

  fs::remove_all(tmp, ec);
  if (failures == 0) {
    std::printf("test_state: OK\n");
    return 0;
  }
  std::printf("test_state: %d failure(s)\n", failures);
  return 1;
}
