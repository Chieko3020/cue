// T-06：FR-TOOL.1 的 8 个工具必须真的注册在 agent 侧（过去 lifecycle 三个只在
// cue-serve 里，agent 根本调不到）。这里用假管线把「注册了什么」钉死：
//  - 名字集合恰好 8 个（多一个少一个都失败）；
//  - lifecycle 三个的风险级是 Mutating（pipeline_stop 不该被当成只读）；
//  - profile 真的映射成 set_param（不是"仅记录"）；
//  - inject_fault 的 kind 覆盖 delay / jitter，单位在描述里写清（毫秒 vs 百分比）。
#include <algorithm>
#include <cstdio>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cue/tool/tools.h"
#include "cue/tool/tool.h"

using json = nlohmann::json;
namespace tool = cue::tool;
namespace pipeline = cue::pipeline;

namespace {

int failures = 0;
void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

// 只记录"被要求做什么"的管线后端：工具层的行为（注册、参数、profile 映射）
// 不需要真 cued，也就不需要 FFmpeg/WS。
class FakePipeline : public pipeline::IServicePipeline {
 public:
  bool start(std::string&) override {
    ++starts;
    running_ = true;
    return start_ok;
  }
  void stop() override {
    ++stops;
    running_ = false;
  }
  bool running() const override { return running_; }
  pipeline::PipelineMetrics latest() const override { return {}; }
  pipeline::ProbeMetrics probe_latest() const override { return {}; }
  void set_event_callback(std::function<void(const pipeline::PipelineEvent&)>) override {}
  std::vector<pipeline::PipelineEvent> recent_events(std::size_t) const override { return {}; }
  bool set_param(const std::string& key, const std::string& value, std::string& err) override {
    if (reject_key == key) {
      err = "假后端拒绝 " + key;
      return false;
    }
    set_calls.push_back(key + "=" + value);
    return true;
  }
  bool inject_fault(const std::string& kind, double value, std::string& err) override {
    (void)err;
    faults.push_back(kind + "=" + std::to_string(value));
    return true;
  }
  std::string observation(const std::string&, int, std::string&) const override { return {}; }

  bool running_ = false;
  bool start_ok = true;
  std::string reject_key;
  int starts = 0;
  int stops = 0;
  std::vector<std::string> set_calls;
  std::vector<std::string> faults;
};

std::set<std::string> names_of(const tool::Registry& reg) {
  std::set<std::string> out;
  for (const auto& s : reg.specs()) out.insert(s.name);
  return out;
}

const tool::ToolSpec* spec_of(const tool::Registry& reg, const std::string& name) {
  return reg.find_spec(name);
}

bool has_value(const std::vector<std::string>& v, const std::string& x) {
  return std::find(v.begin(), v.end(), x) != v.end();
}

// N-02：agent 的观测面到底长什么样，只有真调一次 query_metrics 才算证明。
// suite 的 evidence_candidates 写在 JSON 里，但"能不能摘到"取决于这里的输出键名；
// 过去 m2-constraints 引用的 frames_no_data 就是在这条路径上根本不存在（证据恒 0%）。
class MetricsPipeline : public FakePipeline {
 public:
  pipeline::PipelineMetrics latest() const override {
    pipeline::PipelineMetrics m;
    m.samples = 3;
    m.alive = true;
    m.fps = 30.0;
    m.encoder_known = true;
    m.enc_width = 1920;
    m.enc_height = 1080;
    m.enc_bitrate_kbps = 6000;
    m.enc_gop = 60;
    m.enc_preset = "veryfast";
    m.enc_transcode = "on";
    // N-05：六段分位（含 p95 与 recv/buffer p99）必须真的出现在 agent 观测面
    m.decode_p50 = 1.2;
    m.decode_p95 = 2.8;
    m.decode_p99 = 3.3;
    m.send_p95 = 0.12;
    m.send_p99 = 0.2;
    m.recv_p95 = 0.02;
    m.recv_p99 = 0.03;
    m.buffer_p95 = 0.04;
    m.buffer_p99 = 0.05;
    m.op_p95 = 0.01;
    m.encode_p95 = 3.9;
    return m;
  }
  pipeline::ProbeMetrics probe_latest() const override {
    pipeline::ProbeMetrics p;
    p.alive = true;
    p.samples = 5;
    p.fps = 30.0;
    p.frames = 100;
    p.decoded = 99;
    p.fec_recovered = 37;
    p.fec_groups_rx = 118;
    p.fec_unrecoverable = 1;
    p.fec_unrecoverable_pkts = 3;
    p.fec_ghost_discarded = 9;
    p.frag_ghost_discarded = 2;
    p.lost = 4;
    p.nack_sent = 11;
    p.audio_datagrams = 1500;
    p.audio_seq_gaps = 12;
    p.audio_dups = 1;
    p.audio_reordered = 2;
    p.audio_stale = 3;
    p.audio_decode_errors = 0;
    p.audio_fec_recovered = 4;
    p.audio_plc_frames = 2;
    p.audio_unconcealed_frames = 1;
    // N-05：probe 四段 p95 也要进 e2e_*_p95_us
    p.recv = {10.0, 20.0, 30.0};
    p.reassembly = {40.0, 50.0, 60.0};
    p.buffer = {70.0, 80.0, 90.0};
    p.decode = {100.0, 110.0, 120.0};
    return p;
  }
};

}  // namespace

int main() {
  FakePipeline pipe;
  pipe.running_ = true;  // 避免 pipeline_start 真的去 start（各用例自己控制）
  tool::Registry reg;
  pipeline::build_real_tools(reg, pipe, nullptr);

  // ---------- 1) 恰好 8 个工具，名字与 FR-TOOL.1 一致 ----------
  {
    const std::set<std::string> expected = {
        "query_metrics",  "pipeline_set_param", "inject_fault", "fetch_observation",
        "run_shell",      "pipeline_start",     "pipeline_stop", "pipeline_restart"};
    const auto got = names_of(reg);
    check(reg.specs().size() == 8, "build_real_tools 产出恰好 8 个工具");
    check(got == expected, "8 个工具名与 FR-TOOL.1 的清单一致");
  }

  // ---------- 2) 风险级：lifecycle 三个是 Mutating ----------
  {
    for (const char* n : {"pipeline_start", "pipeline_stop", "pipeline_restart"}) {
      const auto* s = spec_of(reg, n);
      check(s != nullptr, "lifecycle 工具存在");
      if (s != nullptr) check(s->risk == tool::Risk::Mutating, "lifecycle 工具 risk == Mutating");
    }
    // 观测工具必须仍是 ReadOnly：把只读升成写会让 collect 阶段的守卫失去意义
    check(reg.risk_of("query_metrics") == tool::Risk::ReadOnly, "query_metrics 仍是只读");
    check(reg.risk_of("fetch_observation") == tool::Risk::ReadOnly, "fetch_observation 仍是只读");
    check(reg.risk_of("inject_fault") == tool::Risk::Dangerous, "inject_fault 仍是 dangerous");
  }

  // ---------- 3) profile 真的生效（映射成 set_param），且 stop 不带这个参数 ----------
  {
    FakePipeline p;
    p.running_ = false;
    tool::Registry r;
    pipeline::add_lifecycle_tools(r, p);

    const auto res = r.call("pipeline_start", json{{"profile", "std-1500k"}});
    check(res.has_value(), "pipeline_start(profile=std-1500k) 成功");
    check(p.starts == 1, "pipeline_start 真的启动了管线");
    check(has_value(p.set_calls, "transcode=on") && has_value(p.set_calls, "bitrate_kbps=1500") &&
              has_value(p.set_calls, "fps=30") && has_value(p.set_calls, "gop=60") &&
              has_value(p.set_calls, "preset=veryfast"),
          "profile=std-1500k 映射成 5 个 set_param（不是仅记录）");
    check(res.has_value() && res->content.find("profile=std-1500k") != std::string::npos,
          "返回文本如实说明应用了哪个档位");

    // passthrough 只关转码：档位之间互不残留
    p.set_calls.clear();
    const auto res2 = r.call("pipeline_start", json{{"profile", "passthrough"}});
    check(res2.has_value() && p.set_calls.size() == 1 && p.set_calls[0] == "transcode=off",
          "profile=passthrough 只关转码");

    // 已在运行：不重复 start，但档位仍然应用
    p.set_calls.clear();
    const auto res3 = r.call("pipeline_start", json{{"profile", "low-600k"}});
    check(p.starts == 1, "已在运行时 pipeline_start 不再 spawn");
    check(res3.has_value() && p.set_calls.size() == 5, "已在运行时仍然套用 profile");

    // profile 失败必须如实报失败（不能因为 start 成功就整体报成功）
    p.reject_key = "bitrate_kbps";
    const auto res4 = r.call("pipeline_restart", json{{"profile", "high-4000k"}});
    check(!res4.has_value(), "profile 某一步被拒 → 工具返回失败");
    check(!res4.has_value() &&
              res4.error().message.find("未生效") != std::string::npos,
          "失败文本指明档位未生效");
    // 档位应用失败属于"执行失败"（不是参数错，也不是沙箱拒绝）
    check(!res4.has_value() &&
              res4.error().kind == tool::ToolError::Kind::Execution,
          "档位未生效分类为 Execution");

    // pipeline_stop 没有 profile 参数（否则就是个"看起来能改、实际不动"的旋钮）
    const auto* stop = r.find_spec("pipeline_stop");
    check(stop != nullptr && stop->params.empty(), "pipeline_stop 不带 profile 参数");
  }

  // ---------- 4) profile 是闭集：未知档位在 schema 层就被拒 ----------
  {
    check(!reg.call("pipeline_start", json{{"profile", "8k-ultra"}}).has_value(),
          "未知 profile 被 schema 拒绝");
  }

  // ---------- 5) inject_fault 覆盖 delay / jitter，单位写清楚 ----------
  {
    const auto* s = spec_of(reg, "inject_fault");
    check(s != nullptr, "inject_fault 存在");
    if (s != nullptr) {
      const auto& kind = s->params.at(0);
      const bool has_delay = has_value(kind.enum_values, "delay");
      const bool has_jitter = has_value(kind.enum_values, "jitter");
      check(has_delay && has_jitter, "inject_fault.kind 含 delay / jitter");
      check(s->params.at(0).desc.find("delay") != std::string::npos, "kind 描述列出 delay");
      const auto& value = s->params.at(1);
      check(value.desc.find("毫秒") != std::string::npos, "value 描述写明 delay/jitter 是毫秒");
      check(value.desc.find("2000") != std::string::npos, "value 描述写明延迟上界 2000");
      check(!value.has_range, "混合单位下不给单一 [min,max]（校验只有 cued 侧一份）");
    }

    FakePipeline p;
    tool::Registry r;
    pipeline::build_real_tools(r, p, nullptr);
    (void)r.call("inject_fault", json{{"kind", "delay"}, {"value", 200}});
    (void)r.call("inject_fault", json{{"kind", "jitter"}, {"value", 25}});
    check(has_value(p.faults, "delay=200.000000"), "delay 注入透传到后端（毫秒原样）");
    check(has_value(p.faults, "jitter=25.000000"), "jitter 注入透传到后端");
    check(!r.call("inject_fault", json{{"kind", "latency"}, {"value", 1}}).has_value(),
          "未知 kind 在 schema 层就被拒");
  }

  // ---------- 7b) run_shell 的失败分类（FR-TOOL.8 的核心价值） ----------
  // 三类失败在模型看来该做的事完全不同，分类错了就会陷进重试死循环。
  {
    FakePipeline p;
    tool::Registry r;
    pipeline::build_real_tools(r, p, nullptr);

    // 白名单外的命令 → Sandbox：换命令，重试同一命令没用
    const auto denied = r.call("run_shell", json{{"cmd", "rm"}, {"args", json::array()}});
    check(!denied.has_value(), "白名单外的命令被拒");
    check(!denied.has_value() && denied.error().kind == tool::ToolError::Kind::Sandbox,
          "白名单拒绝分类为 Sandbox（不是 Execution）");

    // Timeout 分类无法在这套沙箱里端到端复现，如实说明而不硬造：
    // ① timeout_ms 的 schema 范围是 [200, 30000]，1 ms 在参数层就被拒（Argument）；
    // ② 白名单的 25 个命令全是只读诊断（cat/ls/ps/df/ss…），都在 200 ms 内跑完——
    //    这正是当初选它们做白名单的原因，不该为了测试往生产实现里塞慢命令。
    // 所以 Timeout 只有代码路径（tools.cpp 的 sr.timed_out 分支），没有实测断言。
    const auto too_small =
        r.call("run_shell", json{{"cmd", "ps"}, {"args", json::array()}, {"timeout_ms", 1}});
    check(!too_small.has_value() &&
              too_small.error().kind == tool::ToolError::Kind::Argument,
          "超时参数越界分类为 Argument（schema 层拦下，不是 Timeout）");
  }

  // ---------- 6) 写白名单只影响 Auto；空列表 = 全部放行（旧行为不变） ----------
  {
    tool::CallContext open;
    open.mode = tool::ApprovalMode::Auto;
    check(reg.call_guarded("pipeline_stop", json::object(), open).status ==
              tool::GuardedCall::Status::Executed,
          "Auto + 空写白名单：mutating 照旧放行");
    check(reg.risk_of("pipeline_stop") == tool::Risk::Mutating,
          "pipeline_stop 没有升成 dangerous（审批口径不变）");

    tool::CallContext restricted;
    restricted.mode = tool::ApprovalMode::Auto;
    restricted.allowed_mutating = {"pipeline_set_param"};
    check(reg.call_guarded("pipeline_stop", json::object(), restricted).status ==
              tool::GuardedCall::Status::ApprovalDenied,
          "Auto + case 写白名单不含 pipeline_stop：拒绝（评测里不能停被测管线）");
    check(reg.call_guarded("pipeline_set_param", json{{"key", "gop"}, {"value", "30"}}, restricted)
                  .status == tool::GuardedCall::Status::Executed,
          "白名单内的写工具照常放行");

    tool::CallContext deny;
    deny.mode = tool::ApprovalMode::Deny;
    check(reg.call_guarded("pipeline_start", json::object(), deny).status ==
              tool::GuardedCall::Status::ApprovalDenied,
          "Deny 模式仍然拒绝 lifecycle");
  }

  // ---------- 8) query_metrics 的观测面：N-02 的证据候选真实存在 ----------
  {
    MetricsPipeline mp;
    mp.running_ = true;
    tool::Registry r;
    pipeline::build_real_tools(r, mp, nullptr);
    const auto res = r.call("query_metrics", json::object());
    check(res.has_value(), "query_metrics 在有样本时成功");
    if (res) {
      const json& d = res->data;
      // 编码档位（AVC level 类 case 的唯一可引用观测）
      check(d.value("encode_width", 0) == 1920, "观测面带 encode_width");
      check(d.value("encode_height", 0) == 1080, "观测面带 encode_height");
      check(d.value("encode_bitrate_kbps", 0) == 6000, "观测面带 encode_bitrate_kbps");
      check(d.value("encode_gop", 0) == 60, "观测面带 encode_gop");
      check(d.value("encode_preset", std::string()) == "veryfast", "观测面带 encode_preset");
      // FEC / 幽灵帧 / 音频帧号账（m2-constraints 的候选）
      check(d.value("e2e_fec_recovered", 0) == 37, "观测面带 e2e_fec_recovered");
      check(d.value("e2e_fec_ghost_discarded", 0) == 9, "观测面带 e2e_fec_ghost_discarded");
      check(d.value("e2e_frag_ghost_discarded", 0) == 2, "观测面带 e2e_frag_ghost_discarded");
      check(d.value("e2e_fec_unrecoverable", 0) == 1, "观测面带 e2e_fec_unrecoverable");
      check(d.value("e2e_fec_unrecoverable_pkts", 0) == 3,
            "观测面带 e2e_fec_unrecoverable_pkts");
      check(d.value("e2e_lost", 0) == 4, "观测面带 e2e_lost");
      check(d.value("e2e_audio_datagrams", 0) == 1500, "观测面带 e2e_audio_datagrams");
      check(d.value("e2e_audio_seq_gaps", 0) == 12, "观测面带 e2e_audio_seq_gaps");
      check(d.value("e2e_audio_dups", 0) == 1, "观测面带 e2e_audio_dups");
      check(d.value("e2e_audio_reordered", 0) == 2, "观测面带 e2e_audio_reordered");
      check(d.value("e2e_audio_stale", 0) == 3, "观测面带 e2e_audio_stale");
      // 六段分位（N-05）：p95 与 recv/buffer p99 必须在 agent 观测面里可读
      check(d.value("decode_p95_ms", 0.0) == 2.8, "观测面带 decode_p95_ms");
      check(d.value("send_p95_ms", 0.0) == 0.12, "观测面带 send_p95_ms");
      check(d.value("recv_p95_ms", 0.0) == 0.02, "观测面带 recv_p95_ms");
      check(d.value("recv_p99_ms", 0.0) == 0.03, "观测面带 recv_p99_ms（过去连字段都没有）");
      check(d.value("buffer_p95_ms", 0.0) == 0.04, "观测面带 buffer_p95_ms");
      check(d.value("buffer_p99_ms", 0.0) == 0.05, "观测面带 buffer_p99_ms");
      check(d.value("op_p95_ms", 0.0) == 0.01, "观测面带 op_p95_ms");
      check(d.value("encode_p95_ms", 0.0) == 3.9, "观测面带 encode_p95_ms");
      check(d.value("e2e_recv_p95_us", 0.0) == 20.0, "观测面带 e2e_recv_p95_us");
      check(d.value("e2e_reassembly_p95_us", 0.0) == 50.0, "观测面带 e2e_reassembly_p95_us");
      check(d.value("e2e_buffer_p95_us", 0.0) == 80.0, "观测面带 e2e_buffer_p95_us");
      check(d.value("e2e_decode_p95_us", 0.0) == 110.0, "观测面带 e2e_decode_p95_us");
      // 候选键必须能从观测原文里逐字摘出（scripted 假 agent 走的正是这条路）
      const std::string text = "metrics=" + d.dump();
      check(text.find("\"e2e_fec_ghost_discarded\":9") != std::string::npos,
            "e2e_fec_ghost_discarded=9 可从观测原文摘出");
      check(text.find("\"decode_p95_ms\":2.8") != std::string::npos,
            "decode_p95_ms=2.8 可从观测原文摘出");
      check(text.find("\"send_p95_ms\":0.12") != std::string::npos,
            "send_p95_ms=0.12 可从观测原文摘出");
      check(text.find("\"encode_width\":1920") != std::string::npos,
            "encode_width=1920 可从观测原文摘出");
    }
    // 编码档位未知时必须是 null，而不是 0（0 会被读成"尺寸为 0"）
    FakePipeline bare;
    tool::Registry r2;
    pipeline::build_real_tools(r2, bare, nullptr);
    check(!r2.call("query_metrics", json::object()).has_value(),
          "没有采样时 query_metrics 报错（不返回一份空观测）");
  }

  if (failures == 0) {
    std::printf("test_tools: OK (7 groups)\n");
    return 0;
  }
  std::printf("test_tools: %d failure(s)\n", failures);
  return 1;
}
