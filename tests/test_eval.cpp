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
  bool start(std::string&) override { return true; }
  void stop() override { ++stops; }
  bool running() const override { return true; }
  cue::pipeline::PipelineMetrics latest() const override {
    cue::pipeline::PipelineMetrics m;
    m.fps = 29.97;
    m.frames_in = 300;
    m.samples = 1;
    m.alive = true;
    m.pid = 4242;
    m.updated_us = 123456;
    m.decode_p50 = 1.24;
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
    p.recv = {0.12, 0.48};
    p.reassembly = {0.9, 4.2};
    p.decode = {1.5, 6.1};
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
  check(file_text.find("\"decode_p99_ms\":null") != std::string::npos, "case11 无样本的分位写 null");
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
    check(!cue::pipeline::has_sample(p.buffer.p50), "case11 回放 buffer 段仍是 NaN（不是 0）");
    check(p.buffer_reason == "cued_jitter_buffer_unimplemented", "case11 回放 buffer 原因保留");
    check(cue::pipeline::has_sample(m.decode_p50) && m.decode_p50 == 1.24, "case11 回放 p50 还原");
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

  // 负向：文件不存在 → 加载失败，不允许空实现
  {
    ReplayPipeline rp((dir / "missing.tools.jsonl").string());
    check(!rp.load_error().empty(), "case11 缺录制文件 → load_error");
    std::string err;
    check(!rp.start(err), "case11 缺录制文件时 start 失败");
  }
  fs::remove_all(dir);
}

  if (failures == 0) {
    std::printf("test_eval: OK (11 groups)\n");
    return 0;
  }
  std::printf("test_eval: %d failure(s)\n", failures);
  return 1;
}
