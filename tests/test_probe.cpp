// T-02/T-03 的判据测试：
//  1) ProbeMetrics 的序列化口径——「没采到」（NaN）必须写成 null，回读仍是 NaN，
//     绝不能在任一方向上变成 0；有样本时数值原样往返。
//  2) case.expect.sla 的端到端字段解析与向后兼容：旧 suite（只有 fps_min 等）行为不变，
//     新字段可选、负数拒绝、未知字段 fail-closed。
//  3) judge_case 的端到端判分：探针没有样本时端到端 SLA 一律不达标（不许拿 cued 数据顶替）。
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cue/eval/eval.h"
#include "cue/common/metrics.h"

namespace eval = cue::eval;
namespace pipeline = cue::pipeline;
namespace agent = cue::agent;
using nlohmann::json;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

// 最小可解析 case：只给必填项（expect 必须显式给 sla）
json minimal_case(const json& sla) {
  return json{{"id", "t1"},
              {"seed", 42},
              {"expect",
               {{"root_cause", "network_loss"}, {"evidence_candidates", json::array()},
                {"allowed_actions", {"query_metrics"}}, {"sla", sla}}}};
}

}  // namespace

int main() {
  // ---------------------------------------------------------------- 1) ProbeMetrics 序列化
  {
    // 无样本：所有分位必须落成 null，且回读是 NaN（不是 0）
    pipeline::ProbeMetrics empty;
    empty.buffer_reason = "cued_jitter_buffer_unimplemented";
    empty.unavailable_reason = "probe 进程未运行";
    const json j = pipeline::probe_metrics_to_json(empty);
    check(j["samples"] == 0, "无样本：samples=0");
    check(j["lat_p99_ms"].is_null(), "无样本：端到端 p99 落成 null 而不是 0");
    check(j["lat_p50_ms"].is_null(), "无样本：端到端 p50 落成 null");
    check(j["recv_us"]["p50"].is_null(), "无样本：recv 段 p50 落成 null");
    check(j["decode_us"]["p99"].is_null(), "无样本：decode 段 p99 落成 null");
    check(j["buffer_us"]["p50"].is_null(), "buffer 段恒为 null（cued 抖动缓冲未实现）");
    check(j["buffer_reason"] == "cued_jitter_buffer_unimplemented", "buffer 段带原因");
    check(j["unavailable_reason"] == "probe 进程未运行", "不可用原因往返");

    pipeline::ProbeMetrics back;
    std::string err;
    check(pipeline::probe_metrics_from_json(j, back, err), "无样本 JSON 可回读");
    check(err.empty(), "回读无错误");
    check(back.samples == 0, "回读 samples=0");
    check(!pipeline::has_sample(back.lat_p99_ms), "回读端到端 p99 仍是 NaN（不是 0）");
    check(back.lat_p99_ms != 0.0, "回读端到端 p99 绝不是 0");
    check(!pipeline::has_sample(back.decode.p99), "回读 decode p99 仍是 NaN");
    check(!pipeline::has_sample(back.buffer.p50), "回读 buffer p50 仍是 NaN");
    check(back.buffer_reason == "cued_jitter_buffer_unimplemented", "回读 buffer 原因");
  }
  {
    // 有样本：数值原样往返，未采到的段继续是 NaN
    pipeline::ProbeMetrics p;
    p.alive = true;
    p.pid = 4321;
    p.fps = 29.7;
    p.frames = 300;
    p.decoded = 298;
    p.decode_errors = 2;
    p.stalls = 3;
    p.max_stall_us = 88000;
    p.recv = {120.0, 480.0};
    p.reassembly = {900.0, 4200.0};
    p.buffer = {};  // 没有样本
    p.decode = {1500.0, 6100.0};
    p.lat_p50_ms = 42.5;
    p.lat_p95_ms = 71.0;
    p.lat_p99_ms = 96.25;
    p.synced = true;
    p.offset_us = -1234;
    p.samples = 7;
    const json j = pipeline::probe_metrics_to_json(p);
    check(j["samples"] == 7, "有样本：samples 往返");
    check(j["lat_p99_ms"] == 96.25, "有样本：端到端 p99 往返");
    check(j["buffer_us"]["p50"].is_null(), "有样本但 buffer 无样本：仍是 null");
    check(j["max_stall_us"] == 88000, "有样本：max_stall_us 往返");

    pipeline::ProbeMetrics back;
    std::string err;
    check(pipeline::probe_metrics_from_json(j, back, err), "有样本 JSON 可回读");
    check(back.fps == 29.7, "回读 fps");
    check(back.stalls == 3, "回读 stalls");
    check(pipeline::has_sample(back.lat_p99_ms) && back.lat_p99_ms == 96.25, "回读 p99 有样本");
    check(back.recv.p99 == 480.0, "回读 recv p99");
    check(back.decode.p50 == 1500.0, "回读 decode p50");
    check(!pipeline::has_sample(back.buffer.p50), "回读 buffer 仍无样本");
    check(back.offset_us == -1234, "回读 offset_us（负数）");
  }
  {
    // 坏形态必须报错，不能被当成「没采到」静默放过
    pipeline::ProbeMetrics out;
    std::string err;
    check(!pipeline::probe_metrics_from_json(json{{"lat_p99_ms", "fast"}}, out, err),
          "lat_p99_ms 是字符串 → 报错");
    err.clear();
    check(!pipeline::probe_metrics_from_json(json{{"recv_us", 5}}, out, err),
          "recv_us 不是对象 → 报错");
    err.clear();
    check(!pipeline::probe_metrics_from_json(json::array(), out, err), "非对象 → 报错");
  }

  // ---------------------------------------------------------------- 2) SLA 字段解析
  {
    // 旧字段：行为必须完全不变
    auto c = eval::parse_case(minimal_case(json{{"fps_min", 25}, {"dropped_max", 3}}), "s");
    check(c.has_value(), "旧 SLA 字段可解析");
    if (c) {
      check(c->sla_fps_min == 25.0, "旧字段 fps_min 解析正确");
      check(c->sla_dropped_max == 3, "旧字段 dropped_max 解析正确");
      check(!eval::has_value(c->sla_lat_p99_ms), "未写端到端字段 → NaN（不检查）");
      check(!eval::has_value(c->sla_max_stall_ms), "未写 max_stall_ms → NaN");
      check(c->sla_stalls_max == UINT64_MAX, "未写 stalls → 不检查哨兵");
    }
  }
  {
    // 新旧混用：端到端字段与旧字段同时可用
    auto c = eval::parse_case(minimal_case(json{{"fps_min", 25},
                                                {"lat_p99_ms", 80},
                                                {"lat_p50_ms", 40},
                                                {"max_stall_ms", 120},
                                                {"stalls", 5}}),
                              "s");
    check(c.has_value(), "新旧 SLA 字段可同时解析");
    if (c) {
      check(c->sla_fps_min == 25.0, "混用时旧字段仍生效");
      check(c->sla_lat_p99_ms == 80.0, "lat_p99_ms 解析");
      check(c->sla_lat_p50_ms == 40.0, "lat_p50_ms 解析");
      check(c->sla_max_stall_ms == 120.0, "max_stall_ms 解析");
      check(c->sla_stalls_max == 5, "stalls 解析");
    }
  }
  {
    auto bad = eval::parse_case(minimal_case(json{{"fps_min", 25}, {"lat_p999_ms", 80}}), "s");
    check(!bad.has_value(), "未知 SLA 字段 fail-closed");
    auto neg = eval::parse_case(minimal_case(json{{"lat_p99_ms", -1}}), "s");
    check(!neg.has_value(), "端到端阈值为负 → 拒绝（否则任何读数都达标）");
    auto str = eval::parse_case(minimal_case(json{{"max_stall_ms", "1s"}}), "s");
    check(!str.has_value(), "max_stall_ms 类型错误 → 拒绝");
  }

  // ---------------------------------------------------------------- 3) 端到端判分
  {
    const auto c = eval::parse_case(minimal_case(json{{"fps_min", 25}, {"lat_p99_ms", 80}}), "s");
    check(c.has_value(), "判分用例可解析");
    if (c) {
      eval::Metrics m;
      m.samples = 1;
      m.fps = 30.0;
      agent::RunResult res;
      res.diagnosis.root_cause = "network_loss";
      const std::vector<eval::Observation> obs;

      // 探针没有样本：cued 侧 fps 再好也不能证明端到端达标
      m.probe.samples = 0;
      check(!eval::judge_case(*c, res, obs, m).sla, "探针无样本 → 端到端 SLA 不达标");

      // 有样本且低于阈值 → 达标
      m.probe.samples = 3;
      m.probe.e2e.p99 = 42.0;
      check(eval::judge_case(*c, res, obs, m).sla, "端到端 p99 达标 → SLA 通过");

      // 有样本但超阈值 → 不达标
      m.probe.e2e.p99 = 120.0;
      check(!eval::judge_case(*c, res, obs, m).sla, "端到端 p99 超阈值 → SLA 不达标");

      // 卡顿阈值
      auto cs = eval::parse_case(minimal_case(json{{"fps_min", 25}, {"stalls", 2}}), "s");
      check(cs.has_value(), "stalls 判分用例可解析");
      if (cs) {
        m.probe.samples = 3;
        m.probe.e2e.p99 = eval::kNoValue;
        m.probe.stalls = 1;
        check(eval::judge_case(*cs, res, obs, m).sla, "窗口卡顿 1 次 ≤ 2 → 通过");
        m.probe.stalls = 9;
        check(!eval::judge_case(*cs, res, obs, m).sla, "窗口卡顿 9 次 > 2 → 不达标");
        m.probe.samples = 0;
        check(!eval::judge_case(*cs, res, obs, m).sla, "无探针样本时卡顿阈值不可证明 → 不达标");
      }
    }
  }

  if (failures == 0) {
    std::printf("test_probe: OK\n");
    return 0;
  }
  std::printf("test_probe: %d failure(s)\n", failures);
  return 1;
}
