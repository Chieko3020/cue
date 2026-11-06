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

// An observed counter is a nonnegative JSON integer, including an actual zero.
// Missing/null/wrong-type values stay unknown instead of becoming measured zero.
inline bool read_observed_counter(const nlohmann::json& j, const char* key, uint64_t& out) {
  out = 0;
  const auto it = j.find(key);
  if (it == j.end()) return false;
  if (it->is_number_unsigned()) { out = it->get<uint64_t>(); return true; }
  if (!it->is_number_integer() || it->get<int64_t>() < 0) return false;
  out = static_cast<uint64_t>(it->get<int64_t>());
  return true;
}

// 解析 cued 的单段分位对象 `{"p50","p95","p99"}`（METRICS 行顶层六段、
// metrics.query 响应里的同名段都用这一个口径）。
//
// 语义是 fail-soft，与旧 parse_line 的 stage() 完全一致：
//   · 整段键缺失 / 不是对象 → 三位都保持 NaN（旧 cued 不发这一段 = 没有样本）；
//   · 单个分位缺失 / null / 不是数字 → 只那一位保持 NaN，不把整条 METRICS 判坏，
//     更不补 0。
// 为什么不 fail-closed：这行来自子进程的 stdout，reader 线程不能被一个坏字段打断；
// 录制/回放的**坏文件**才需要 fail-closed（见 recorded_pipeline.cpp 的 metrics_from_json）。
// 为什么不把 p95 丢掉：生产端三处（summary / session_snapshot / metric.sample）
// 一直发 p50/p95/p99，消费端丢 p95 会让报告里的 p95 永远 n/a——那是解析问题，
// 不是"数据源没采"（N-05）。
inline void apply_stage_percentiles(const nlohmann::json& j, const char* key, double& p50,
                                    double& p95, double& p99) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_object()) return;
  auto num = [&it](const char* k, double& out) {
    const auto f = it->find(k);
    if (f == it->end() || !f->is_number()) return;
    out = f->get<double>();
  };
  num("p50", p50);
  num("p95", p95);
  num("p99", p99);
}

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
// p50/p95/p99 三位都在 probe 的 METRICS 与 probe.metric 事件里上报；NaN = 这一段
// 没有样本（整段 null），不是"耗时 0 微秒"。
struct ProbeLatencyUs {
  double p50 = kNoSample;
  double p95 = kNoSample;
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
  // 音频弱网恢复（N-01）：probe 下行音频的 FEC/PLC 账。三个都是**进程累计**，
  // 且口径互不重叠（见 apps/cue-probe.cpp 里 kMaxPlcFrames 上方的对账等式）：
  //   fec_recovered   = 用后一包的 in-band FEC 冗余恢复出的真实上一帧；
  //   plc_frames      = 帧号缺口处由 PLC 外推的隐藏帧（伪造，只用来填播放队列）；
  //   unconcealed     = 既没 FEC 也没 PLC 覆盖的缺口（诚实暴露）。
  // 旧录制里没有这三个字段：解析时缺省为 0（不是"解析失败"），见 probe_metrics_from_json。
  uint64_t audio_fec_recovered = 0;
  uint64_t audio_plc_frames = 0;
  uint64_t audio_unconcealed_frames = 0;
  // 下行 FEC / NACK / 音频帧号账（N-02）：这些数在 probe 的 METRICS 与 probe.metric
  // 事件里本来就有，但过去没进 agent 的观测面（query_metrics），于是审计说得出
  // "FEC 恢复了多少"，agent 却一个都引用不了。
  // **视频 FEC**（fec_*）与上面的**音频 FEC**（audio_fec_*）是两本账：
  // 前者按帧的片（fragment）算、XOR 组恢复；后者按 20 ms 音频帧算、LBRR 冗余恢复。
  uint64_t fec_recovered = 0;            // XOR-FEC 补回的视频片数
  uint64_t fec_groups_rx = 0;            // 收到过的 FEC 组数（恢复率的分母之一）
  uint64_t fec_unrecoverable = 0;        // 结算时仍缺 ≥2 片的组数
  uint64_t fec_unrecoverable_pkts = 0;   // 上面那些组一共缺了几片
  // 幽灵帧守卫（M0 教训 / N-02）：被丢弃的"为已完成帧重建槽位"次数。
  // 两个源的**判读方式不同**（实测见 apps/cue-probe.cpp 的定义处）：
  //   fec_ghost_discarded  = 常态（FEC 校验包总在整帧之后到；零丢包 8 s/241 帧 →
  //     250 次，约每完成帧 1 次），只能当"守卫在跑"的读数，不是故障指示；
  //   frag_ghost_discarded = 异常（迟到分片/重传；零丢包 0、5% 丢包 2~3）。
  // 所以证据层只能引用后者（以及 fec_recovered），不能拿前者证明幽灵帧。
  uint64_t fec_ghost_discarded = 0;
  uint64_t frag_ghost_discarded = 0;
  // 重组表的内存护栏（N-10 / FR-PIPE.14）：pending 是 `std::map<frame_id, FrameAcc>`，
  // 此前只受 2 s TTL 与单帧 kMaxFragPerFrame=1024 约束 → **帧数**无界（上行字节来自
  // 浏览器，每帧发 1 片、帧号递增，2 s 内就能攒出任意多槽位）。现在加了帧数 256 +
  // 总字节 64 MiB 双上限（include/cue/proto/reassembly_budget.h），超限按最旧优先淘汰：
  //   reassembly_pending_dropped = 因预算淘汰的帧数。**是 lost 的子集**（淘汰时照常
  //     ++lost、settle、settle_fec），单独记只为分清"为什么丢"：超时 = 链路问题，
  //     预算淘汰 = 接收端自保（链路可能完好，只是有人在灌碎片/放大帧）；
  //   reassembly_pending_bytes   = 当前占用字节数。这是**gauge**而不是累计值：
  //     它随收片上涨、随帧完成/淘汰回落，旧 cassette 里没有这个字段时按 0；
  //   reassembly_rejected        = 因预算拒收的包数（单个病态帧自己就超总预算）。
  //     与协议层的 `rejected`（坏包/几何矛盾）不同：那些包本身不合法。
  uint64_t reassembly_pending_dropped = 0;
  uint64_t reassembly_pending_bytes = 0;
  uint64_t reassembly_rejected = 0;
  uint64_t lost = 0;          // 重组阶段判定丢失的帧数（cleanup_stale 超时 + 预算淘汰）
  uint64_t nack_sent = 0;     // 发出的 NACK 条目数（累计；不是"重传成功"数）
  // 音频帧号账（见 probe 的 AudioSeqTracker）：gaps=真缺包，dups=重复，
  // reordered=迟到但在容差内（照播），stale=迟到太多（丢弃）。四者互不重叠。
  uint64_t audio_datagrams = 0;
  uint64_t audio_seq_gaps = 0;
  uint64_t audio_dups = 0;
  uint64_t audio_reordered = 0;
  uint64_t audio_stale = 0;
  uint64_t audio_decode_errors = 0;
  // 以上计数都是**进程累计**；旧录制里没有这些字段：解析时缺省为 0（不是"解析失败"），
  // 见 probe_metrics_from_json。
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
                          {"p95", has_sample(s.p95) ? nlohmann::json(s.p95)
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
      {"audio_fec_recovered", p.audio_fec_recovered},
      {"audio_plc_frames", p.audio_plc_frames},
      {"audio_unconcealed_frames", p.audio_unconcealed_frames},
      // N-02：FEC / NACK / 幽灵帧 / 音频帧号账。缺字段的旧录制回放时按 0（cnt 口径）。
      {"fec_recovered", p.fec_recovered},
      {"fec_groups_rx", p.fec_groups_rx},
      {"fec_unrecoverable", p.fec_unrecoverable},
      {"fec_unrecoverable_pkts", p.fec_unrecoverable_pkts},
      {"fec_ghost_discarded", p.fec_ghost_discarded},
      {"frag_ghost_discarded", p.frag_ghost_discarded},
      // N-10：重组表双上限的账（见结构体里的口径）。缺字段的旧录制回放时按 0（cnt 口径）。
      {"reassembly_pending_dropped", p.reassembly_pending_dropped},
      {"reassembly_pending_bytes", p.reassembly_pending_bytes},
      {"reassembly_rejected", p.reassembly_rejected},
      {"lost", p.lost},
      {"nack_sent", p.nack_sent},
      {"audio_datagrams", p.audio_datagrams},
      {"audio_seq_gaps", p.audio_seq_gaps},
      {"audio_dups", p.audio_dups},
      {"audio_reordered", p.audio_reordered},
      {"audio_stale", p.audio_stale},
      {"audio_decode_errors", p.audio_decode_errors},
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
    return inner("p50", dst.p50) && inner("p95", dst.p95) && inner("p99", dst.p99);
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
      !cnt("jb_late", out.jb_late) ||
      // N-01：这三个字段是后加的，旧 cassette 里没有。cnt() 的口径就是「缺了 → 0 且合法」，
      // 所以它们天然向后兼容（不像别处 fail-closed 那样缺字段就报错）；如果哪天这里要
      // 改成必填，旧录制会整体解析失败——那才是真回归。
      !cnt("audio_fec_recovered", out.audio_fec_recovered) ||
      !cnt("audio_plc_frames", out.audio_plc_frames) ||
      !cnt("audio_unconcealed_frames", out.audio_unconcealed_frames) ||
      // N-02：同一口径——缺字段 = 0 且合法。旧 cassette / 旧 probe（没有这些计数）回放
      // 时不能整体解析失败，否则"补观测"反而破坏历史录制。
      !cnt("fec_recovered", out.fec_recovered) || !cnt("fec_groups_rx", out.fec_groups_rx) ||
      !cnt("fec_unrecoverable", out.fec_unrecoverable) ||
      !cnt("fec_unrecoverable_pkts", out.fec_unrecoverable_pkts) ||
      !cnt("fec_ghost_discarded", out.fec_ghost_discarded) ||
      !cnt("frag_ghost_discarded", out.frag_ghost_discarded) ||
      // N-10：同一口径（缺字段 = 0 且合法）。这三个是后加的：旧 probe / 旧 cassette 里
      // 没有它们，不能被判成解析失败。
      !cnt("reassembly_pending_dropped", out.reassembly_pending_dropped) ||
      !cnt("reassembly_pending_bytes", out.reassembly_pending_bytes) ||
      !cnt("reassembly_rejected", out.reassembly_rejected) || !cnt("lost", out.lost) ||
      !cnt("nack_sent", out.nack_sent) || !cnt("audio_datagrams", out.audio_datagrams) ||
      !cnt("audio_seq_gaps", out.audio_seq_gaps) || !cnt("audio_dups", out.audio_dups) ||
      !cnt("audio_reordered", out.audio_reordered) || !cnt("audio_stale", out.audio_stale) ||
      !cnt("audio_decode_errors", out.audio_decode_errors) || !seg("recv_us", out.recv) ||
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
  bool injected_observed = false; // presence is preserved across backends/recordings
  uint64_t decoded = 0;
  uint64_t decode_errors = 0;
  uint64_t rejected = 0;  // 协议边界拒绝数（wire.h 的拒绝原因分类在 cued 侧汇总）
  // cued METRICS 行里原本就有、但过去没往 agent 侧暴露的计数（0 是真实读数，不是缺省）
  uint64_t send_errors = 0;    // sendto 失败（ENOBUFS/EMSGSIZE），不是"已发送"
  uint64_t encode_errors = 0;  // 编码失败（尺寸/格式/编码器错误）
  uint64_t decode_drops = 0;   // 解码器未消费的包（H264Decoder::stats().dropped）
  uint64_t budget_drops = 0;   // 令牌桶不足丢的帧
  bool budget_drops_observed = false;
  // 排队策略切换（fault.set kind=backlog 改了发送节奏）时主动丢弃的积压帧。
  // 与 dropped 分开：那一刻丢弃是控制面的策略决定，不是链路故障。
  uint64_t queue_dropped = 0;
  bool queue_dropped_observed = false;
  // 延迟注入（FR-PIPE.10 的 delay / jitter）的实际效果：被压进延迟队列的数据报数，
  // 以及单个数据报被压住的最长时间（微秒）。它们是「延迟而不是丢包」的正面证据：
  // 只有这两个数涨而 injected / budget_drops 不涨，才说明数据报最终都发出去了、只是晚了。
  uint64_t delayed_pkts = 0;
  bool delayed_pkts_observed = false;
  uint64_t delayed_max_us = 0;
  bool delayed_max_us_observed = false;
  // 迟到帧（相对最小延迟基线晚到超过 --max-ingress-lag-ms 而被丢弃）。
  // 它和延迟注入的关系必须能读出来：延迟注入只压下行数据报，**不**改变帧的年龄，
  // 所以注入 delay 时这个计数应该保持 0；一旦它涨，说明是真的积压/时钟问题，不是注入。
  uint64_t ingress_late = 0;
  bool ingress_late_observed = false;
  // 六段分位（ms）：recv/buffer/decode/op/encode/send 各 p50/p95/p99。
  // 生产端（cued 的 summary / session_snapshot / metric.sample）三处都发这三个分位，
  // 所以消费端六段的三分位**一律可读**：某一位是 NaN 只表示**那一位没有样本**
  // （cued 的 METRICS 对应段为 null），不是"花了 0 ms"。消费者必须输出 null/n/a，
  // 不能用 0 冒充——"没采到"与"测到 0"在判分上是相反的结论。
  double recv_p50 = kNoSample, buffer_p50 = kNoSample, decode_p50 = kNoSample,
         op_p50 = kNoSample, encode_p50 = kNoSample, send_p50 = kNoSample;
  double recv_p95 = kNoSample, buffer_p95 = kNoSample, decode_p95 = kNoSample,
         op_p95 = kNoSample, encode_p95 = kNoSample, send_p95 = kNoSample;
  // recv/buffer 两段的 p99 曾被整段丢掉（生产端在发、结构体没有字段），与 p95 同类。
  double recv_p99 = kNoSample, buffer_p99 = kNoSample;
  double decode_p99 = kNoSample, op_p99 = kNoSample, encode_p99 = kNoSample;
  // send 段 p99：延迟注入的主观测点（延迟只体现在发送侧尾部，不体现在计数上）
  double send_p99 = kNoSample;
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
  bool control_ops_observed = false;
  // 编码档位快照（N-02）：来自 cued 的 `metrics.query` 响应里的 `params`（ParamStore 的
  // 运行期真值，agent 改过的 set_param 会体现在这里）。过去这份数据只在控制面内部
  // 用来更新 spawn 参数，agent 的 query_metrics 里看不到——于是"AVC level 配错"这类
  // 根因只有一个报错字符串可讲，没有可引用的观测。
  //   encoder_known=false = 本后端/本次快照没有 params（例如控制通道降级到 stdout，
  //   或 HTTP 后端不返回这一段）：输出 null，绝不按 0 写。
  // transcode 沿用 cued 的写法 "on"/"off"（空 = 未知）。
  bool encoder_known = false;
  int enc_width = 0;   // 0 = 沿用解码尺寸（与 RuntimeParams 同义）
  int enc_height = 0;
  int enc_bitrate_kbps = 0;
  int enc_gop = 0;
  std::string enc_preset;
  std::string enc_transcode;
};

}  // namespace cue::pipeline
