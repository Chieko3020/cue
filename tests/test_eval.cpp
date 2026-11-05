// M4 判分器负向测试：必须能把「答对」和「答错 / 伪造证据 / SLA 未达成」区分开
//
// 对抗用例直接取自 docs/research/review-eval.md 第 3 节：改判分器后必须复跑这些构造，
// 任何一个被接受就是回归。
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include "cue/eval/eval.h"
#include "cue/common/recorded_pipeline.h"

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


// 11) 工具观测录制/回放（AC-6 的底层保证）：NaN 往返、参数不符报错、录制用尽报 miss
class FakePipeline : public cue::pipeline::IServicePipeline {
 public:
  bool counters_known = true;
  bool start(std::string&) override { return true; }
  void stop() override { ++stops; }
  bool running() const override { return true; }
  cue::pipeline::PipelineMetrics latest() const override {
    cue::pipeline::PipelineMetrics m;
    m.injected_observed = counters_known;
    m.budget_drops_observed = counters_known;
    m.fps = 29.97;
    m.frames_in = 300;
    m.samples = 1;
    m.alive = true;
    m.pid = 4242;
    m.updated_us = 123456;
    m.decode_p50 = 1.24;
    m.decode_p95 = 1.8;   // N-05：p95 必须进录制并能回放（过去结构体里根本没有这个字段）
    m.send_p95 = 0.11;
    // 没采到的分位：回放后必须仍是 NaN，不能变成 0
    m.decode_p99 = cue::pipeline::kNoSample;
    m.encode_p50 = cue::pipeline::kNoSample;
    return m;
  }
  // 端到端快照：这里造一份有样本的（NaN 段保持 NaN，验证录制回放不把"没采到"变 0）
  cue::pipeline::ProbeMetrics probe_latest() const override {
    cue::pipeline::ProbeMetrics p;
    p.alive = true;
    p.pid = 4242;
    p.samples = 1;
    p.fps = 29.5;
    p.frames = 300;
    p.decoded = 298;
    p.stalls = 2;
    p.max_stall_us = 66000;
    p.recv = {0.12, 0.31, 0.48};
    p.reassembly = {0.9, 2.4, 4.2};
    p.decode = {1.5, 3.7, 6.1};
    p.lat_p50_ms = 42.5;
    p.lat_p95_ms = 71.0;
    p.lat_p99_ms = 96.25;
    p.synced = true;
    // buffer 段没有样本：回放后必须仍是 NaN，不能变成 0
    p.buffer = {};
    p.buffer_reason = "cued_jitter_buffer_unimplemented";
    return p;
  }
  void set_event_callback(std::function<void(const cue::pipeline::PipelineEvent&)> cb) override {
    cb_ = std::move(cb);
  }
  std::vector<cue::pipeline::PipelineEvent> recent_events(std::size_t) const override {
    // 故意乱序：回调重放时必须按 t_us 排
    return {cue::pipeline::PipelineEvent{"late", nlohmann::json::object(), 20, 21},
            cue::pipeline::PipelineEvent{"early", nlohmann::json::object(), 10, 11}};
  }
  bool set_param(const std::string& key, const std::string& value, std::string& err) override {
    (void)value;
    if (key == "gop") {
      err = "gop out of range [1, 300]";
      return false;
    }
    return true;
  }
  bool inject_fault(const std::string& kind, double value, std::string&) override {
    (void)kind;
    (void)value;
    return true;
  }
  std::string observation(const std::string& source, int tail, std::string&) const override {
    return source + ":" + std::to_string(tail);
  }
  int stops = 0;

 private:
  std::function<void(const cue::pipeline::PipelineEvent&)> cb_;
};

{
  namespace fs = std::filesystem;
  using cue::pipeline::RecordingPipeline;
  using cue::pipeline::ReplayPipeline;
  const fs::path dir = fs::temp_directory_path() / ("cue-test-replay-" + std::to_string(::getpid()));
  fs::remove_all(dir);
  const std::string rec_path = cue::pipeline::tools_record_path(dir.string(), "a/b");
  check(rec_path.find("a-b.tools.jsonl") != std::string::npos,
        "case11 case id 里的 / 被净化为 - 后落盘");

  {
    RecordingPipeline rp(std::make_unique<FakePipeline>(), dir.string(), "a/b");
    check(rp.io_error().empty(), "case11 录制文件可写");
    std::string err;
    check(rp.start(err), "case11 录制 start");
    const auto m = rp.latest();
    check(m.samples == 1 && m.fps == 29.97, "case11 录制 latest 透传");
    // 端到端快照也必须进录制（否则回放时端到端 SLA 无从判定）
    const auto p = rp.probe_latest();
    check(p.samples == 1 && p.lat_p99_ms == 96.25, "case11 录制 probe_latest 透传");
    check(rp.set_param("bitrate_kbps", "800", err), "case11 录制 set_param 成功");
    check(!rp.set_param("gop", "61", err), "case11 录制 set_param 失败也照录");
    err.clear();
    check(rp.inject_fault("drop", 5.0, err), "case11 录制 inject_fault");
    err.clear();  // 调用方负责在每次调用前清空 err：录制按「err 是否为空」判成败
    check(rp.observation("procs", 20, err) == "procs:20", "case11 录制 observation");
    check(rp.recent_events(0).size() == 2, "case11 录制 recent_events");
    check(rp.running(), "case11 录制 running");
    rp.stop();
    rp.stop();
  }

  // 文件格式：NaN 必须是 null（贴一行最容易被 0 冒充的字段）
  std::string file_text;
  {
    std::ifstream in(rec_path);
    std::string line;
    while (std::getline(in, line)) file_text += line + "\n";
  }
  check(file_text.find("\"v\":1") != std::string::npos, "case11 录制带版本字段");
  check(file_text.find("\"decode_p50_ms\":1.24") != std::string::npos, "case11 有样本的分位写数字");
  check(file_text.find("\"decode_p95_ms\":1.8") != std::string::npos, "case11 有样本的 p95 也进录制");
  check(file_text.find("\"send_p95_ms\":0.11") != std::string::npos, "case11 send p95 进录制");
  check(file_text.find("\"decode_p99_ms\":null") != std::string::npos, "case11 无样本的分位写 null");
  check(file_text.find("\"encode_p95_ms\":null") != std::string::npos, "case11 无样本的 encode p95 写 null");
  check(file_text.find("\"encode_p50_ms\":null") != std::string::npos, "case11 无样本的 encode 写 null");
  check(file_text.find("\"decode_p99_ms\":0") == std::string::npos, "case11 无样本绝不写成 0");
  check(file_text.find("\"op\":\"probe_latest\"") != std::string::npos, "case11 端到端快照进录制");
  check(file_text.find("\"lat_p99_ms\":96.25") != std::string::npos, "case11 端到端 p99 写数字");
  check(file_text.find("\"buffer_us\":{\"p50\":null") != std::string::npos,
        "case11 buffer 段无样本写 null（不写 0）");

  {
    ReplayPipeline rp(rec_path);
    check(rp.load_error().empty(), "case11 回放加载成功");
    std::string err;
    check(rp.start(err), "case11 回放 start");
    const auto m = rp.latest();
    check(m.samples == 1 && m.fps == 29.97 && m.pid == 4242, "case11 回放指标一致");
    const auto p = rp.probe_latest();
    check(p.samples == 1 && p.lat_p99_ms == 96.25 && p.stalls == 2, "case11 回放端到端指标一致");
    check(cue::pipeline::has_sample(p.decode.p50) && p.decode.p50 == 1.5, "case11 回放 decode 段还原");
    check(cue::pipeline::has_sample(p.recv.p95) && p.recv.p95 == 0.31, "case11 回放 probe recv p95 还原");
    check(!cue::pipeline::has_sample(p.buffer.p50), "case11 回放 buffer 段仍是 NaN（不是 0）");
    check(!cue::pipeline::has_sample(p.buffer.p95), "case11 回放 buffer p95 仍是 NaN");
    check(p.buffer_reason == "cued_jitter_buffer_unimplemented", "case11 回放 buffer 原因保留");
    check(cue::pipeline::has_sample(m.decode_p50) && m.decode_p50 == 1.24, "case11 回放 p50 还原");
    check(cue::pipeline::has_sample(m.decode_p95) && m.decode_p95 == 1.8, "case11 回放 p95 还原（N-05）");
    check(cue::pipeline::has_sample(m.send_p95) && m.send_p95 == 0.11, "case11 回放 send p95 还原");
    check(!cue::pipeline::has_sample(m.decode_p99), "case11 回放 NaN 仍是 NaN（不是 0）");
    check(!cue::pipeline::has_sample(m.encode_p50), "case11 回放 encode NaN 仍是 NaN");

    check(rp.set_param("bitrate_kbps", "800", err), "case11 回放 set_param 命中");
    err.clear();
    check(!rp.set_param("gop", "61", err), "case11 回放录制中的失败也如实复现");
    check(err.find("out of range") != std::string::npos, "case11 回放失败文案来自录制");
    check(rp.inject_fault("drop", 5.0, err), "case11 回放 inject_fault 命中");
    // 先试与录制不符的参数：必须明确失败且不消费录制（回放不能「拿另一条顶上」）
    err.clear();
    check(rp.observation("procs", 21, err).empty(), "case11 回放参数不符必须失败");
    check(err.find("参数不符") != std::string::npos, "case11 参数不符的错误文案明确");
    err.clear();
    check(rp.observation("procs", 20, err) == "procs:20", "case11 回放 observation 命中");
    const auto evs = rp.recent_events(0);
    check(evs.size() == 2 && evs[0].type == "late" && evs[1].type == "early",
          "case11 回放 recent_events 原样返回");
    check(rp.running(), "case11 回放 running 返回录制值");
    rp.stop();
    rp.stop();  // 幂等空操作：不消费、不记 miss
    std::vector<std::string> order;
    rp.set_event_callback([&order](const cue::pipeline::PipelineEvent& e) { order.push_back(e.type); });
    check(order.size() == 2 && order[0] == "early" && order[1] == "late",
          "case11 事件回调按 t_us 重放");

    // 相同参数的 set_param 再要一条：录制里没有 → miss 必须报错
    const auto before = rp.stats().misses;
    err.clear();
    check(!rp.set_param("bitrate_kbps", "800", err), "case11 录制用尽 → 失败而不是给默认值");
    check(rp.stats().misses == before + 1, "case11 miss 计数递增");
    check(rp.last_error().find("miss") != std::string::npos, "case11 miss 有明确说明");
    // latest 用尽：返回 samples=0，绝不伪造
    err.clear();
    check(rp.latest().samples == 0, "case11 latest 用尽返回 samples=0");
    check(rp.stats().misses == before + 2, "case11 latest miss 计数递增");
  }

  // 负向：把录制里 set_param 的 key 改掉再回放 → 必须明确失败
  {
    const std::string bad = (dir / "bad.tools.jsonl").string();
    std::string text = file_text;
    const std::string from = "\"key\":\"bitrate_kbps\"";
    const std::string to = "\"key\":\"gop\"";
    const auto pos = text.find(from);
    check(pos != std::string::npos, "case11 负向用例能定位到 set_param 的 key");
    if (pos != std::string::npos) text.replace(pos, from.size(), to);
    {
      std::ofstream out(bad);
      out << text;
    }
    ReplayPipeline rp(bad);
    check(rp.load_error().empty(), "case11 改 key 后文件仍可加载");
    std::string err;
    check(rp.start(err), "case11 负向 start 命中");
    check(!rp.set_param("bitrate_kbps", "800", err), "case11 参数不符 → 回放明确失败");
    check(err.find("参数不符") != std::string::npos, "case11 参数不符不被静默接受");
    check(rp.stats().misses == 1, "case11 参数不符计 miss");
  }

  // 向后兼容：N-05 之前的旧录制没有 *_p95_ms（也没有 recv/buffer p99），
  // 回放必须能整体加载，缺失位按 kNoSample（NaN）处理——不能因为"新增观测"
  // 就把历史录制判成坏文件，也不能把缺失补成 0。
  {
    const std::string old_path = (dir / "old.tools.jsonl").string();
    {
      std::ofstream out(old_path);
      out << R"({"v":1,"op":"start","ret":{"ok":true}})" << "\n";
      out << R"({"v":1,"op":"latest","ret":{"fps":29.97,"frames_in":300,"frames_out":280,)"
             R"("dropped":20,"injected":0,"decoded":280,"decode_errors":0,"rejected":0,)"
             R"("send_errors":0,"encode_errors":0,"decode_drops":0,"budget_drops":0,)"
             R"("recv_p50_ms":0.01,"buffer_p50_ms":0.02,"decode_p50_ms":1.24,)"
             R"("op_p50_ms":0.0,"encode_p50_ms":null,"send_p50_ms":0.08,)"
             R"("decode_p99_ms":3.3,"op_p99_ms":0.05,"encode_p99_ms":null,)"
             R"("updated_us":123456,"samples":1,"alive":true,"pid":4242}})"
          << "\n";
      out << R"({"v":1,"op":"probe_latest","ret":{"fps":29.5,"frames":100,"decoded":98,)"
             R"("stalls":1,"samples":1,"recv_us":{"p50":0.12,"p99":0.48}}})"
          << "\n";
    }
    ReplayPipeline rp(old_path);
    if (!rp.load_error().empty()) {
      std::printf("旧录制裁剪失败原因: %s\n", rp.load_error().c_str());
    }
    check(rp.load_error().empty(), "旧录制（无 p95 键）仍能加载");
    std::string err;
    check(rp.start(err), "旧录制 start 命中");
    const auto m = rp.latest();
    check(m.samples == 1 && m.fps == 29.97, "旧录制 latest 可读");
    check(!cue::pipeline::has_sample(m.decode_p95), "旧录制缺 decode p95 → NaN（不是 0）");
    check(!cue::pipeline::has_sample(m.send_p95), "旧录制缺 send p95 → NaN");
    check(!cue::pipeline::has_sample(m.recv_p99), "旧录制缺 recv p99 → NaN");
    check(cue::pipeline::has_sample(m.decode_p50) && m.decode_p50 == 1.24,
          "旧录制已有的 p50 照常读出");
    const auto p = rp.probe_latest();
    check(p.samples == 1, "旧录制 probe_latest 可读");
    check(cue::pipeline::has_sample(p.recv.p50) && p.recv.p50 == 0.12, "旧录制 probe p50 照常读出");
    check(!cue::pipeline::has_sample(p.recv.p95), "旧录制 probe 段缺 p95 → NaN（不是 0）");
    check(!cue::pipeline::has_sample(p.buffer.p95), "旧录制 probe buffer 缺 p95 → NaN");
  }

  // 负向：文件不存在 → 加载失败，不允许空实现
  {
    ReplayPipeline rp((dir / "missing.tools.jsonl").string());
    check(!rp.load_error().empty(), "case11 缺录制文件 → load_error");
    std::string err;
    check(!rp.start(err), "case11 缺录制文件时 start 失败");
  }
  fs::remove_all(dir);
}

  // 12) 注入是否真的生效（N-23）：判据必须把「故障没发生」与「模型答错」分开。
  // 重点是 rate 的 budget_drops=0：那正是"源码率低于注入上限、令牌桶没被抽干"的形态，
  // 旧路径会把这种 case 判成 SLA 达标（fps 恒 30），这一层就是来堵这个假通过点的。
  {
    const auto rate_dead = eval::judge_injection("rate", 0, 0);
    check(rate_dead.checked, "case12 rate 有自动判据");
    check(!rate_dead.effective, "case12 rate budget_drops=0 → 注入未生效");
    check(rate_dead.detail.find("budget_drops=0") != std::string::npos,
          "case12 未生效说明里带出读数");

    const auto rate_ok = eval::judge_injection("rate", 60, 0);
    check(rate_ok.checked && rate_ok.effective, "case12 rate budget_drops=60 → 注入生效");

    const auto drop_dead = eval::judge_injection("drop", 0, 0);
    check(drop_dead.checked && !drop_dead.effective, "case12 drop injected=0 → 注入未生效");
    const auto drop_ok = eval::judge_injection("drop", 0, 59);
    check(drop_ok.checked && drop_ok.effective, "case12 drop injected=59 → 注入生效");
    const auto ingress_ok = eval::judge_injection("ingress_drop", 0, 7);
    check(ingress_ok.checked && ingress_ok.effective, "case12 ingress_drop injected=7 → 注入生效");

    // backlog / delay / jitter 没有单一计数判据：必须明确"未判定"，不能硬凑一个结论
    const auto backlog = eval::judge_injection("backlog", 0, 0);
    check(!backlog.checked, "case12 backlog 不做自动判定");
    check(backlog.detail.find("未做自动判定") != std::string::npos,
          "case12 backlog 的说明写明未做自动判定");
    const auto none = eval::judge_injection("none", 0, 0);
    check(!none.checked && none.detail.empty(), "case12 none 类没有判定也无说明");
  }

  // 13) 证据候选的取值谓词（N-22）
  // 核心回归锁：`budget_drops=0` 是真实观测里的真实字段，但取值支持不了 "带宽不足"，
  // 带 `>0` 谓词后必须判不成立——这正是旧口径放过两次的伪证。
  {
    // 13a) 解析：key OP value，容忍 key 里的 _ 与 .、容忍两侧空格
    {
      const auto a = eval::parse_evidence_candidate("budget_drops>0");
      check(a.key == "budget_drops" && a.op == ">" && a.value == "0" && a.has_predicate(),
            "case13a 解析 budget_drops>0");
      const auto b = eval::parse_evidence_candidate("encode_width==1920");
      check(b.key == "encode_width" && b.op == "==" && b.value == "1920", "case13a 解析 ==");
      const auto c = eval::parse_evidence_candidate("e2e_fec_recovered>=1");
      check(c.key == "e2e_fec_recovered" && c.op == ">=" && c.value == "1", "case13a 解析 >=");
      const auto d = eval::parse_evidence_candidate("key!=off");
      check(d.key == "key" && d.op == "!=" && d.value == "off", "case13a 解析 !=");
      const auto e = eval::parse_evidence_candidate("send_p99_ms <= 233.0");
      check(e.key == "send_p99_ms" && e.op == "<=" && e.value == "233.0",
            "case13a 解析带空格且 key 含 _");
      const auto f = eval::parse_evidence_candidate("metrics.fps>0");
      check(f.key == "metrics.fps" && f.op == ">" && f.value == "0", "case13a key 含 .");
      // 无谓词（回归）：整串就是 key，行为与旧候选一致
      const auto g = eval::parse_evidence_candidate("injected");
      check(g.key == "injected" && !g.has_predicate(), "case13a 无谓词候选解析");
      // 单个 '=' 不是合法运算符：按无谓词处理，旧的"字面 key"匹配行为不变
      const auto h = eval::parse_evidence_candidate("encode_width=1920");
      check(!h.has_predicate() && h.key == "encode_width=1920",
            "case13a 单个 = 不构成谓词（旧候选串仍是字面匹配）");
    }

    const auto pred_case = [](std::vector<std::string> cands) {
      eval::Case c;
      c.id = "pred";
      c.expect_root_cause = "bandwidth_insufficient";
      c.evidence_candidates = std::move(cands);
      return c;
    };
    const auto budget_obs = [](int budget_drops) {
      eval::Observation o;
      o.tool = "query_metrics";
      o.data = nlohmann::json{{"fps", 30.3}, {"budget_drops", budget_drops}};
      o.text = "metrics=" + o.data.dump();
      return std::vector<eval::Observation>{o};
    };
    const auto pred_evidence = [&](const std::vector<std::string>& cands,
                                   const std::vector<eval::Observation>& obs,
                                   const std::string& ev) {
      auto r = good_result();
      r.diagnosis.evidence = {ev};
      return eval::judge_case(pred_case(cands), r, obs, good_metrics()).evidence;
    };

    // 13b) 判定：取值不支持结论 → 证据不成立；真故障读数 → 成立
    check(!pred_evidence({"budget_drops>0"}, budget_obs(0), "budget_drops=0"),
          "case13b budget_drops=0 不满足 budget_drops>0（伪证回归锁）");
    check(pred_evidence({"budget_drops>0"}, budget_obs(82), "budget_drops=82"),
          "case13b budget_drops=82 满足 budget_drops>0");
    // 同一类伪证的另一个形态：encode_width=0（0 = 沿用解码尺寸）不是 1080p 尺寸证据
    {
      eval::Observation o;
      o.text = "metrics={\"encode_width\":0,\"fps\":30.3}";
      o.data = nlohmann::json{{"encode_width", 0}, {"fps", 30.3}};
      check(!pred_evidence({"encode_width==1920"}, {o}, "encode_width=0"),
            "case13b encode_width=0 不满足 ==1920");
    }
    // data 不是对象/没有该键时回退 text 解析（旧录制、纯文本观测）
    {
      eval::Observation o;  // data 保持空对象，只有 text
      o.text = "metrics: budget_drops=82, fps=30.3";
      check(pred_evidence({"budget_drops>0"}, {o}, "budget_drops=82"),
            "case13b 取值回退 text 的 key=VALUE");
    }

    // 13c) 字符串比较：== / != 按原样
    {
      const auto cand = eval::parse_evidence_candidate("encode_transcode==on");
      eval::Observation on_obs;
      on_obs.data = nlohmann::json{{"encode_transcode", "on"}, {"fps", 30.3}};
      on_obs.text = "metrics=" + on_obs.data.dump();
      eval::Observation off_obs;
      off_obs.data = nlohmann::json{{"encode_transcode", "off"}, {"fps", 30.3}};
      off_obs.text = "metrics=" + off_obs.data.dump();
      std::string why;
      check(eval::evidence_candidate_matches("encode_transcode=on", on_obs, cand, why),
            "case13c encode_transcode==on 在 on 时成立");
      check(!eval::evidence_candidate_matches("encode_transcode=on", off_obs, cand, why),
            "case13c encode_transcode==on 在 off 时不成立");
      const auto neq = eval::parse_evidence_candidate("encode_transcode!=off");
      check(eval::evidence_candidate_matches("encode_transcode=on", on_obs, neq, why),
            "case13c encode_transcode!=off 在 on 时成立");
      check(!eval::evidence_candidate_matches("encode_transcode=on", off_obs, neq, why),
            "case13c encode_transcode!=off 在 off 时不成立");
    }

    // 13d) 非数值无法比较：不成立、不崩，且原因写明
    {
      const auto gt = eval::parse_evidence_candidate("encode_transcode>0");
      eval::Observation on_obs;
      on_obs.data = nlohmann::json{{"encode_transcode", "on"}};
      on_obs.text = "metrics=" + on_obs.data.dump();
      std::string why;
      check(!eval::evidence_candidate_matches("encode_transcode=on", on_obs, gt, why),
            "case13d encode_transcode>0 对字符串判不成立");
      check(why.find("非数值") != std::string::npos, "case13d 原因写明非数值无法比较");
    }

    // 13e) 无谓词候选行为与之前一致：键出现即成立（值不在判据里）
    check(pred_evidence({"budget_drops"}, budget_obs(0), "budget_drops=0"),
          "case13e 无谓词候选 budget_drops 仍按键命中");
    check(pred_evidence({"injected"}, make_obs(), "injected=59"),
          "case13e 旧候选字符串（injected）行为不变");
    check(!pred_evidence({"injected"}, make_obs(), "send_p50_ms=0.09"),
          "case13e 无关字段仍不满足候选");
  }

  // C05: a judged result with ineffective injection is not a rate sample.
  {
    eval::CaseOutcome excluded;
    excluded.id = "excluded";
    excluded.scored = true;
    excluded.score = {true, true, true, {}};
    excluded.injection = eval::judge_injection("rate", 0, 0);
    eval::CaseOutcome valid;
    valid.id = "valid";
    valid.scored = true;
    valid.score = {false, false, false, {}};
    const auto mixed = eval::compute_stability({excluded, valid}, 1);
    check(mixed.root_rate.mean == 0 && mixed.evidence_rate.mean == 0 &&
              mixed.sla_rate.mean == 0 && mixed.pass_k.mean == 0,
          "C05 ineffective injection excluded from stability denominators");
    check(eval::compute_stability({excluded}, 3).rounds == 0,
          "C05 all ineffective injection yields no measured stability rounds");
  }

  // C05: counter presence across backend parsing and actual recording/replay.
  {
    cue::pipeline::PipelineMetrics m;
    m.samples = 1;
    const std::vector<nlohmann::json> values = {
        nullptr, "0", -1, 0.5, true, nlohmann::json::object()};
    for (const auto& value : values) {
      nlohmann::json j{{"budget_drops", value}, {"injected", value}};
      m.budget_drops_observed = cue::pipeline::read_observed_counter(j, "budget_drops", m.budget_drops);
      m.injected_observed = cue::pipeline::read_observed_counter(j, "injected", m.injected);
      check(!eval::judge_injection("rate", m).checked &&
                !eval::judge_injection("drop", m).checked,
            "C05 invalid/null counter is unknown, never measured zero");
    }
    m.budget_drops_observed = cue::pipeline::read_observed_counter({}, "budget_drops", m.budget_drops);
    check(!eval::judge_injection("rate", m).checked, "C05 absent counter is unknown");
    m.budget_drops_observed = cue::pipeline::read_observed_counter(
        nlohmann::json{{"budget_drops", 0}}, "budget_drops", m.budget_drops);
    check(eval::judge_injection("rate", m).checked && !eval::judge_injection("rate", m).effective,
          "C05 an actually observed zero is ineffective");
    m.samples = 0;
    check(!eval::judge_injection("rate", m).checked, "C05 absent sample cannot prove a zero");
    const auto dir = std::filesystem::temp_directory_path() /
        ("cue-eval-presence-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    for (const bool known : {false, true}) {
      const std::string id = known ? "known" : "unknown";
      {
        auto fake = std::make_unique<FakePipeline>();
        fake->counters_known = known;
        cue::pipeline::RecordingPipeline rec(std::move(fake), dir.string(), id);
        (void)rec.latest();
      }
      cue::pipeline::ReplayPipeline replay(cue::pipeline::tools_record_path(dir.string(), id));
      const auto read = replay.latest();
      check(read.samples == 1 && read.budget_drops_observed == known &&
                read.injected_observed == known && eval::judge_injection("rate", read).checked == known,
            "C05 recording/replay preserves unknown versus actual zero");
    }
    // A legacy recording lacking both keys stays unknown (not replay miss).
    const auto path = cue::pipeline::tools_record_path(dir.string(), "known");
    std::ifstream in(path);
    std::vector<nlohmann::json> rows;
    std::string line;
    while (std::getline(in, line)) {
      auto row = nlohmann::json::parse(line);
      if (row.contains("ret") && row["ret"].is_object()) {
        row["ret"].erase("budget_drops"); row["ret"].erase("injected");
      }
      rows.push_back(std::move(row));
    }
    in.close();
    { std::ofstream file(path); for (const auto& row : rows) file << row.dump() << "\n"; }
    cue::pipeline::ReplayPipeline legacy(path);
    const auto read = legacy.latest();
    check(read.samples == 1 && !read.budget_drops_observed && !read.injected_observed &&
              !eval::judge_injection("rate", read).checked && legacy.stats().misses == 0,
          "C05 legacy missing counters remain unknown without invented zeros");
    std::filesystem::remove_all(dir);
  }

  // C06: structured numeric predicates fail closed and keys are whole fields.
  {
    eval::Case c;
    c.id = "c06-red";
    c.expect_root_cause = "network_loss";
    c.evidence_candidates = {"budget_drops>0"};
    auto r = good_result();
    r.diagnosis.evidence = {"budget_drops=82"};
    eval::Observation o{"query_metrics", "budget_drops=82", nlohmann::json{{"budget_drops", "82"}}};
    check(!eval::judge_case(c, r, {o}, good_metrics()).evidence,
          "C06 structured numeric string cannot satisfy numeric predicate");
    o.data = nlohmann::json{{"fps", 30}};
    check(!eval::judge_case(c, r, {o}, good_metrics()).evidence,
          "C06 missing structured field cannot fall back to conflicting text");
    o.data = nlohmann::json{{"budget_drops", 82}, {"not_budget_drops", 82}};
    o.text = "not_budget_drops=82";
    r.diagnosis.evidence = {"not_budget_drops=82"};
    check(!eval::judge_case(c, r, {o}, good_metrics()).evidence,
          "C06 candidate cannot borrow prefixed field in evidence");
  }

  {
    eval::Case c;
    c.id = "c06-matrix"; c.expect_root_cause = "network_loss";
    c.evidence_candidates = {"budget_drops>0"};
    const auto judge = [&](nlohmann::json data, const std::string& text,
                           const std::string& quote, const std::string& candidate) {
      auto r = good_result(); r.diagnosis.evidence = {quote};
      auto copy = c; copy.evidence_candidates = {candidate};
      return eval::judge_case(copy, r, {{"query_metrics", text, data}}, good_metrics()).evidence;
    };
    for (const auto& value : {nlohmann::json(nullptr), nlohmann::json(true),
                              nlohmann::json::array({82}), nlohmann::json::object({{"n", 82}})})
      check(!judge({{"budget_drops", value}}, "budget_drops=82", "budget_drops=82", "budget_drops>0"),
            "C06 bad structured type never borrows text");
    for (const double value : {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
      check(!judge({{"budget_drops", value}}, "budget_drops=82", "budget_drops=82", "budget_drops!=off"),
            "C06 nonfinite cannot satisfy nonnumeric inequality");
    check(!judge({{"budget_drops", 82}}, "budget_drops=82", "budget_drops=82", "budget_drops!=1e999"),
          "C06 overflowing numeric RHS cannot fall back to string inequality");
    check(judge(nlohmann::json::object(), "budget_drops=82", "budget_drops=82", "budget_drops>0"),
          "C06 legacy text-only numeric record remains supported");
    std::string scalar_why;
    check(eval::evidence_candidate_matches("mode=on", {"query_metrics", "mode=on", {{"mode", "on"}}},
                                           eval::parse_evidence_candidate("mode==on"), scalar_why),
          "C06 scalar predicate retains structured string equality");
    check(eval::evidence_candidate_matches("enabled=true", {"query_metrics", "enabled=true", {{"enabled", true}}},
                                           eval::parse_evidence_candidate("enabled==true"), scalar_why),
          "C06 scalar predicate retains structured boolean equality");
    check(!judge({{"budget_drops", 82}, {"budget_drops_extra", 82}}, "budget_drops_extra=82",
                 "budget_drops_extra=82", "budget_drops>0"), "C06 candidate suffix cannot borrow field");
    auto quoted = good_result(); quoted.diagnosis.evidence = {"budget_drops=0"};
    check(!eval::judge_case(c, quoted, {{"query_metrics", "budget_drops=0", {{"budget_drops", 0}}},
                                      {"query_metrics", "budget_drops=82", {{"budget_drops", 82}}}}, good_metrics()).evidence,
          "C06 cited zero cannot borrow another observation positive count");
    check(!judge({{"budget_drops", 82}}, "budget_drops=82", "budget_drops=82 因此所有根因均已证明", "budget_drops>0"),
          "C06 authentic quote plus invented prose is rejected");
    for (const std::string path : {"suites/real-faults.json", "suites/m4-diagnosis.json", "suites/generated/fault-matrix.json"}) {
      const auto suite = eval::load_suite(path);
      check(bool(suite), "C06 maintained suite loads");
      if (!suite) continue;
      for (const auto& item : *suite) {
        for (const auto& candidate : item.evidence_candidates)
          check(eval::parse_evidence_candidate(candidate).has_predicate(), "C06 formal suite has no bare candidate bypass");
        auto r = good_result(); r.diagnosis.root_cause = item.expect_root_cause;
        r.diagnosis.evidence = {item.expect_root_cause == "bandwidth_insufficient" ? "budget_drops=0" : "frames_in=120"};
        const eval::Observation normal{"query_metrics", "frames_in=120 budget_drops=0 fps=30 encode_width=0",
                                       {{"frames_in", 120}, {"budget_drops", 0}, {"fps", 30}, {"encode_width", 0}}};
        check(!eval::judge_case(item, r, {normal}, good_metrics()).evidence,
              "C06 normal frames or zero budget do not prove suite fault");
      }
    }
  }

  {
    agent::RunResult expired;
    expired.error = "wall clock budget exhausted";
    check(eval::classify_run_error(expired) == "agent_timeout",
          "C07 first-turn wall deadline is timeout, not endpoint unavailable");
    expired.error = "http 400: wall clock limit configured by endpoint";
    check(eval::classify_run_error(expired) == "llm_unavailable",
          "C07 endpoint body cannot impersonate Runner wall-budget reason");
    expired.error = "curl: connection refused";
    check(eval::classify_run_error(expired) == "llm_unavailable",
          "C07 first-turn connection error remains unavailable");
  }

  if (failures == 0) {
    std::printf("test_eval: OK (13 groups)\n");
    return 0;
  }
  std::printf("test_eval: %d failure(s)\n", failures);
  return 1;
}
