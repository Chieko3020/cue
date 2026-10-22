// 管线观测数据模型：指标快照与事件。
// 从 process_control.h 抽出（结构体字段、注释、语义一字未改），供所有管线后端
// （子进程 / HTTP 服务 / 录制回放）共用——观测口径只允许有一处定义。
#pragma once

#include <sys/types.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include <nlohmann/json.hpp>

namespace cue::pipeline {

// 分位缺失的哨兵：NaN 而不是 0（见 PipelineMetrics 的注释）
inline constexpr double kNoSample = std::numeric_limits<double>::quiet_NaN();

// 该分位是否真的采到了（NaN 表示没有样本）
inline bool has_sample(double v) { return std::isfinite(v); }

// 一条事件：cued stdout 里的 [cue-event] 行，或 harness 自己合成的控制面事件。
// data 原样保留（不同 ev 的字段不同，消费者按键取），t_us 是事件时间（unix us），
// seen_us 是本进程读到它的单调时钟时刻——两者口径不同，跨进程比较只能用 seen_us。
struct PipelineEvent {
  std::string type;
  nlohmann::json data = nlohmann::json::object();
  int64_t t_us = 0;
  int64_t seen_us = 0;
};

// 探针（cue-probe）的一段微秒分位。与 PipelineMetrics 的六段（ms）分开：这里的样本
// 来自接收端自己的时钟，且 probe 可能根本不在（假域 / 回放），可用性必须单独表达。
struct ProbeLatencyUs {
  double p50 = kNoSample;
  double p99 = kNoSample;
};

// 端到端观测快照：字段与 probe 的 `probe.metric` 事件一一对应。
//
// 与 PipelineMetrics 为什么必须分开（review 的核心教训）：cued 的 METRICS 是「发出去多少、
// 编码花了多久」，probe 的是「收到时已经过了多久、卡了多久」。用前者冒充后者，就是拿
// 服务端的自报当端到端证据——所以 SLA 一旦声明端到端阈值，只能用本结构里的数判。
struct ProbeMetrics {
  // 快照时的存活状态：探针进程没了就不能继续用最后一条指标撒谎（与 PipelineMetrics 同口径）
  bool alive = false;
  pid_t pid = 0;
  double fps = 0;           // 解码帧率（按解码出的帧算，不是收包数）
  uint64_t frames = 0;      // 重组完成的帧数
  uint64_t decoded = 0;     // 解码出的帧数
  uint64_t decode_errors = 0;
  uint64_t decode_skips = 0;  // codec 不是 H264、按协议跳过解码的帧数
  uint64_t stalls = 0;        // 卡顿次数（进程累计；评测侧取窗口增量）
  double max_stall_us = 0;    // 最长单次卡顿（0 = 没发生过卡顿；有无样本看 samples）
  // 四段分解（微秒）：recv=数据报到达→被读出，reassembly=首片→整帧完成，
  // buffer=抖动缓冲等待，decode=解码。NaN = 这一段没有样本（输出 null，不写 0）。
  ProbeLatencyUs recv, reassembly, buffer, decode;
  // 端到端（ms）：probe 收帧时刻（换算到 cued 时钟域）− 帧 PTS。时钟未对齐时全 NaN。
  double lat_p50_ms = kNoSample, lat_p95_ms = kNoSample, lat_p99_ms = kNoSample;
  bool synced = false;     // 是否已估出时钟偏移（false → 端到端分位必然无样本）
  int64_t offset_us = 0;   // cued_clock − probe_clock
  int64_t updated_us = 0;  // 最近一条指标的本进程单调时刻
  uint64_t samples = 0;    // 收到过多少条 probe.metric 行（0 = 还没有任何样本）
  // buffer 段无样本的**原因**（可选）：不是"测出来是 0"，而是这一段这次确实没有样本。
  // T-04 之后 probe 自己就有抖动缓冲，正常情况下不再带原因；字段保留是为了旧录制
  // （里面写着 cued_jitter_buffer_unimplemented）仍能原样回放，不被当成"0 样本"。
  std::string buffer_reason;
  // 抖动缓冲（FR-PROBE.4）：档位、切换次数、目标深度、跳帧与迟到（累计）。
  // 与 buffer_us 一起回答"抖动缓冲在干什么"：buffer 段涨 + jb_level 涨 = 升档换平滑；
  // buffer 段涨 + jb_skipped 涨 = 帧真的丢了，等也等不回来。
  uint64_t jb_level = 0;
  uint64_t jb_switches = 0;
  uint64_t jb_target_frames = 0;
  uint64_t jb_depth = 0;
  uint64_t jb_skipped = 0;
  uint64_t jb_late = 0;
  // 不可用原因（进程没起 / 起不来 / 起来了但一条指标都没上报）。空 = 没有异常。
  // 评测侧据此报 probe_unavailable，而不是退回 cued 指标假装有端到端数据。
  std::string unavailable_reason;
};

// NaN → null、null/缺字段 → NaN：录制回放与报告都靠这一对函数保住「没采到 ≠ 0」。
inline nlohmann::json probe_metrics_to_json(const ProbeMetrics& p) {
  auto pct = [](double v) { return has_sample(v) ? nlohmann::json(v) : nlohmann::json(nullptr); };
  auto seg = [](const ProbeLatencyUs& s) {
    return nlohmann::json{{"p50", has_sample(s.p50) ? nlohmann::json(s.p50)
                                                   : nlohmann::json(nullptr)},
                          {"p99", has_sample(s.p99) ? nlohmann::json(s.p99)
                                                    : nlohmann::json(nullptr)}};
  };
  return nlohmann::json{
      {"alive", p.alive},
      {"pid", p.pid == 0 ? nlohmann::json(nullptr) : nlohmann::json(static_cast<int64_t>(p.pid))},
      {"fps", p.fps},
      {"frames", p.frames},
      {"decoded", p.decoded},
      {"decode_errors", p.decode_errors},
      {"decode_skips", p.decode_skips},
      {"stalls", p.stalls},
      {"max_stall_us", p.max_stall_us},
      {"recv_us", seg(p.recv)},
      {"reassembly_us", seg(p.reassembly)},
      {"buffer_us", seg(p.buffer)},
      {"decode_us", seg(p.decode)},
      {"jb_level", p.jb_level},
      {"jb_switches", p.jb_switches},
      {"jb_target_frames", p.jb_target_frames},
      {"jb_depth", p.jb_depth},
      {"jb_skipped", p.jb_skipped},
      {"jb_late", p.jb_late},
      {"lat_p50_ms", pct(p.lat_p50_ms)},
      {"lat_p95_ms", pct(p.lat_p95_ms)},
      {"lat_p99_ms", pct(p.lat_p99_ms)},
      {"synced", p.synced},
      {"offset_us", p.offset_us},
      {"samples", p.samples},
      {"buffer_reason", p.buffer_reason},
      {"unavailable_reason", p.unavailable_reason}};
}

// 只认「数字或 null」，其它形态一律报错：坏录制不能被当成"没采到"静默放过。
inline bool probe_metrics_from_json(const nlohmann::json& j, ProbeMetrics& out,
                                    std::string& err) {
  if (!j.is_object()) {
    err = "probe_metrics 不是对象";
    return false;
  }
  auto num = [&j, &err](const char* key, double& dst) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) {
      dst = kNoSample;
      return true;
    }
    if (!it->is_number()) {
      err = std::string("probe_metrics 字段 ") + key + " 既不是 null 也不是数字";
      return false;
    }
    dst = it->get<double>();
    return true;
  };
  auto cnt = [&j, &err](const char* key, uint64_t& dst) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) {
      dst = 0;
      return true;
    }
    if (!it->is_number_integer()) {
      err = std::string("probe_metrics 字段 ") + key + " 不是整数";
      return false;
    }
    const int64_t v = it->get<int64_t>();
    dst = v > 0 ? static_cast<uint64_t>(v) : 0;
    return true;
  };
  // 端到端分位在 probe 的 `probe.metric` 事件里沿用旧字段名（lat_p50 / lat_p95 / lat_p99，
  // 单位 ms），录制格式用带单位的 *_ms。两种拼写都认：同一个快照不该有两套解析。
  auto num_alias = [&j, &err](const char* key_ms, const char* key_plain, double& dst) {
    const char* keys[2] = {key_ms, key_plain};
    for (const char* k : keys) {
      const auto it = j.find(k);
      if (it == j.end()) continue;
      if (it->is_null()) {
        dst = kNoSample;
        return true;
      }
      if (!it->is_number()) {
        err = std::string("probe_metrics 字段 ") + k + " 既不是 null 也不是数字";
        return false;
      }
      dst = it->get<double>();
      return true;
    }
    dst = kNoSample;
    return true;
  };
  auto seg = [&j, &err](const char* key, ProbeLatencyUs& dst) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) {
      dst = ProbeLatencyUs{};
      return true;
    }
    if (!it->is_object()) {
      err = std::string("probe_metrics 字段 ") + key + " 不是对象";
      return false;
    }
    auto inner = [&it, &err, key](const char* k, double& d) {
      const auto f = it->find(k);
      if (f == it->end() || f->is_null()) {
        d = kNoSample;
        return true;
      }
      if (!f->is_number()) {
        err = std::string("probe_metrics.") + key + "." + k + " 既不是 null 也不是数字";
        return false;
      }
      d = f->get<double>();
      return true;
    };
    return inner("p50", dst.p50) && inner("p99", dst.p99);
  };
  auto flag = [&j](const char* key, bool& dst) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_boolean()) return;
    dst = it->get<bool>();
  };
  auto str = [&j](const char* key, std::string& dst) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return;
    dst = it->get<std::string>();
  };

  flag("alive", out.alive);
  flag("synced", out.synced);
  if (!num("fps", out.fps) || !num("max_stall_us", out.max_stall_us) ||
      !num_alias("lat_p50_ms", "lat_p50", out.lat_p50_ms) ||
      !num_alias("lat_p95_ms", "lat_p95", out.lat_p95_ms) ||
      !num_alias("lat_p99_ms", "lat_p99", out.lat_p99_ms) || !cnt("frames", out.frames) ||
      !cnt("decoded", out.decoded) || !cnt("decode_errors", out.decode_errors) ||
      !cnt("decode_skips", out.decode_skips) || !cnt("stalls", out.stalls) ||
      !cnt("samples", out.samples) || !cnt("jb_level", out.jb_level) ||
      !cnt("jb_switches", out.jb_switches) || !cnt("jb_target_frames", out.jb_target_frames) ||
      !cnt("jb_depth", out.jb_depth) || !cnt("jb_skipped", out.jb_skipped) ||
      !cnt("jb_late", out.jb_late) || !seg("recv_us", out.recv) ||
      !seg("reassembly_us", out.reassembly) || !seg("buffer_us", out.buffer) ||
      !seg("decode_us", out.decode)) {
    return false;
  }
  const auto pid_it = j.find("pid");
  out.pid = (pid_it != j.end() && pid_it->is_number_integer())
                ? static_cast<pid_t>(pid_it->get<int64_t>())
                : 0;
  const auto off = j.find("offset_us");
  out.offset_us = (off != j.end() && off->is_number_integer()) ? off->get<int64_t>() : 0;
  str("buffer_reason", out.buffer_reason);
  str("unavailable_reason", out.unavailable_reason);
  return true;
}

struct PipelineMetrics {
  double fps = 0;
  uint64_t frames_in = 0;
  uint64_t frames_out = 0;
  uint64_t dropped = 0;
  uint64_t injected = 0;
  uint64_t decoded = 0;
  uint64_t decode_errors = 0;
  uint64_t rejected = 0;  // 协议边界拒绝数（wire.h 的拒绝原因分类在 cued 侧汇总）
  // cued METRICS 行里原本就有、但过去没往 agent 侧暴露的计数（0 是真实读数，不是缺省）
  uint64_t send_errors = 0;    // sendto 失败（ENOBUFS/EMSGSIZE），不是"已发送"
  uint64_t encode_errors = 0;  // 编码失败（尺寸/格式/编码器错误）
  uint64_t decode_drops = 0;   // 解码器未消费的包（H264Decoder::stats().dropped）
  uint64_t budget_drops = 0;   // 令牌桶不足丢的帧
  // 排队策略切换（fault.set kind=backlog 改了发送节奏）时主动丢弃的积压帧。
  // 与 dropped 分开：那一刻丢弃是控制面的策略决定，不是链路故障。
  uint64_t queue_dropped = 0;
  // 六段 p50（ms）。NaN = 这一段没有样本（cued 的 METRICS 对应段是 null）：
  // "没采到"与"花了 0 ms"是两件事，消费者必须输出 null/n/a，不能用 0 冒充。
  double recv_p50 = kNoSample, buffer_p50 = kNoSample, decode_p50 = kNoSample,
         op_p50 = kNoSample, encode_p50 = kNoSample, send_p50 = kNoSample;
  // 六段 p99（ms，同上：只有 cued 真正打印了 p99 的段才有值）
  double decode_p99 = kNoSample, op_p99 = kNoSample, encode_p99 = kNoSample;
  int64_t updated_us = 0;
  uint64_t samples = 0;
  // 快照时的存活状态：cued 被 kill 后 latest() 不能继续撒谎（review #11）
  bool alive = false;
  // 指标属于哪个 cued 会话（同一个 cue 进程内可能重启过多代）；消费者据此判断跨进程拼接（#25）
  pid_t pid = 0;
  // 控制通道（T-05）：cued 侧实际生效的通道 "uds" / "stdout"，以及它累计处理的控制
  // 请求数。空字符串 = 这行 METRICS 来自没有该字段的旧 cued（不要读成"没有控制面"）。
  std::string control_channel;
  uint64_t control_ops = 0;
};

}  // namespace cue::pipeline
