// cue-probe — 下行接收端
// UDP 收包 → 分片重组 → 抖动缓冲 → FEC/NACK → H.264 软解 → 帧统计（帧率 / 卡顿 / 四段延迟 / 端到端分位）
//
// 延迟口径（SPEC §5：probe 通常跨机，与本机 cued 不是同一 CLOCK_MONOTONIC 基准）：
//   pts_us 是 cued 时钟域的帧时间戳，直接拿 probe 的 now 去减会得到毫无意义的差值。
//   所以 probe 周期性发 StatsReq（下行回环 ping），用最小 RTT 样本估
//   offset = cued_clock − probe_clock，端到端延迟按 (probe_recv + offset − pts_us) 计算；
//   从未拿到样本时不输出延迟分位（lat=unsynced）。同机部署时 offset ≈ 0，口径与旧实现一致。
//
// 指标行的窗口口径：每条 `probe.metric` 里的 fps 与四段/端到端分位都是**这一秒窗口**
// 的样本（分位环每秒清零），不是进程累计值。累计值没法回答"修复之后端到端是多少"——
// 故障期的 2 s 样本会一直留在环里。stalls / max_stall_us / lost 等计数仍是累计值，
// 由消费方按窗口差分（与 cued 的 dropped 同一口径）。
//
// 为什么 probe 要真的解码（而不是只数包）：
//   「收到多少帧」与「能解出多少帧」是两件事。收包数在花屏/参数集错位时照样是 30/s，
//   拿它当 fps 会把一条已经解不出画面的链路报成健康。所以 fps 一律按**解码出的帧**算，
//   解码失败与跳过（非 H264）分开计数，卡顿也只在有真实帧时统计。
#include <boost/asio/ip/udp.hpp>
#include <boost/system/error_code.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <poll.h>
#endif
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

extern "C" {
#include <libavutil/frame.h>
}

#include "probe_network.h"
#include "native_arguments.h"
using namespace cue::probe;

#include "cue/common/clock.h"
#include "cue/media/audio_receiver.h"
#include "cue/media/audio_rms.h"
#include "cue/media/h264_decoder.h"

#include "probe_render.h"
#include "cue/media/jitter_buffer.h"
#include "cue/proto/frame_guard.h"
#include "cue/proto/nack.h"
#include "cue/proto/reassembly_budget.h"
#include "cue/proto/wire.h"

namespace net = boost::asio;
using udp = net::ip::udp;
using json = nlohmann::json;

namespace {

// 事件的 t 是 unix 微秒（墙钟，给 SSE/报告展示）；延迟测量仍用 cue::now_us() 的
// 单调时钟，两者不混用。
int64_t unix_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// 单行结构化事件： [cue-event] {"ev":"<type>","t":<unix_us>,"data":{...}}
// 外层用 ordered_json：键序固定为 ev/t/data，不随字段名排序漂移。
std::string event_body(std::string_view ev, const json& data) {
  nlohmann::ordered_json body{{"ev", std::string(ev)}, {"t", unix_us()}, {"data", data}};
  return body.dump();
}

// ---------------------------------------------------------------- CLI 解析
// 不用 std::stoi/std::stod：非法值抛出的异常会穿透 main（SPEC §9 禁止异常穿透模块边界），
// 用 from_chars + 范围校验，失败打一行原因并以退出码 2 结束（review #22）。
bool parse_int(std::string_view s, long long lo, long long hi, long long& out) {
  if (s.empty()) return false;
  long long v = 0;
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || p != s.data() + s.size()) return false;
  if (v < lo || v > hi) return false;
  out = v;
  return true;
}

bool parse_double(std::string_view s, double lo, double hi, double& out) {
  if (s.empty()) return false;
  double v = 0;
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || p != s.data() + s.size()) return false;
  if (!(v >= lo && v <= hi)) return false;  // 同时挡掉 NaN
  out = v;
  return true;
}

struct FecGroupState {
  uint16_t offset = 0;
  uint16_t member_count = 0;
  std::vector<uint8_t> parity;
  bool available = false;
};

struct FrameAcc {
  uint16_t count = 0;
  uint64_t pts_us = 0;
  int64_t first_seen_us = 0;
  uint8_t codec = cue::proto::kDownCodecUnspecified;
  uint16_t recv_parts = 0;  // 真正收到的分片数（FEC 恢复的不计，用于丢包率）
  std::vector<std::vector<uint8_t>> parts;
  // FEC 组（FR-PIPE.9）：key = group_index。一帧可以有多个组（每 N 个数据包一组），
  // 旧的一帧一组格式只会产生一条（group_index=0，覆盖整帧）。
  std::map<uint16_t, FecGroupState> fec_groups;
  bool nack_queued = false;
  // 这个槽位当前占住的字节数（parts 里的分片 + fec_groups 里的校验数据，含 FEC 恢复出的片）。
  // 自己记一份而不是每次遍历求和：预算判据每收一个包都要问一次，遍历是 O(分片数)。
  // 它是 `pending_bytes` 总数器的分项，两者必须同步加减（见 main() 里的"重组表内存护栏"段）。
  uint64_t bytes = 0;
};

// 丢包率窗口：只在帧结算（完成或超时清理）时累加，样本包含「整帧丢失」的帧，无偏
struct LossWindow {
  uint64_t expected = 0;
  uint64_t received = 0;
};

inline void settle(LossWindow& w, const FrameAcc& fa) {
  w.expected += fa.count;
  w.received += fa.recv_parts;
}

// 端到端延迟的时钟域自检：offset 估计好之后仍有离谱值就说明样本本身有问题
inline bool lat_plausible(int64_t us) { return us > -1'000'000 && us < 10'000'000; }

// 时钟对齐：offset = cued_clock − probe_clock。
// SPEC §7.1 的口径（8 次往返取 RTT 最小者、每 30 s 重估、漂移 > 5 ms 重锚并记事件）
// 在这里落到 probe 侧：每 30 s 重新问一次，批内取最小 RTT，漂移超阈值记一次 reanchor。
struct ClockAlign {
  bool have = false;
  int64_t offset_us = 0;
  int64_t best_rtt_us = -1;
  uint64_t samples = 0;
  uint64_t reanchors = 0;
  int64_t last_drift_us = 0;

  void begin_batch() { best_rtt_us = -1; }

  // echo：我方发戳（probe 时钟）；cued_now：对端回包时的 cued 时钟。
  // 返回 true 表示这次样本触发了重锚（漂移 > 5 ms），调用方据此写 probe.reanchor 事件。
  bool observe(int64_t echo, int64_t cued_now, int64_t recv_now) {
    const int64_t rtt = recv_now - echo;
    if (rtt < 0 || rtt > 1'000'000) return false;  // 异常往返（排队/重放）当噪声丢掉
    const int64_t off = cued_now - (echo + rtt / 2);
    if (!have) {
      offset_us = off;
      best_rtt_us = rtt;
      have = true;
      ++samples;
      return false;
    }
    if (best_rtt_us < 0 || rtt < best_rtt_us) {  // 只认本批 RTT 最小的样本
      last_drift_us = off - offset_us;
      bool reanchored = false;
      if (std::llabs(last_drift_us) > 5'000) {  // 漂移 > 5 ms：重锚并记事件
        ++reanchors;
        reanchored = true;
      }
      offset_us = off;
      best_rtt_us = rtt;
      ++samples;
      return reanchored;
    }
    return false;
  }
};

// 逐组尝试 FEC 恢复：每组**恰好缺 1 片**时用 parity 补齐。返回本次恢复的片数。
// 一组丢 2 片是欠定方程（XOR 只能解一个未知数），不能恢复——这由 fec_recover_one 判定。
// pending_bytes 一并累加：恢复出来的片同样驻留在 pending 里（那是实打实的内存），
// 漏记就等于给预算凭空多发 1.2 MiB/帧的额度。调用方负责按需再守一次字节上限。
std::size_t try_fec_recover(FrameAcc& fa, uint64_t& pending_bytes) {
  std::size_t recovered = 0;
  std::vector<uint8_t> rec;
  for (auto& [gi, g] : fa.fec_groups) {
    if (!g.available || g.parity.empty()) continue;
    const std::size_t begin = g.offset;
    const std::size_t end = std::min<std::size_t>(fa.count, begin + g.member_count);
    if (end <= begin) continue;
    const std::span<const std::vector<uint8_t>> members(fa.parts.data() + begin, end - begin);
    std::size_t missing = 0;
    if (!cue::proto::fec_recover_one(members, g.parity, missing, rec)) continue;
    fa.parts[begin + missing] = rec;
    fa.bytes += rec.size();
    pending_bytes += rec.size();
    ++recovered;
  }
  return recovered;
}

// 帧结算时的 FEC 账：哪些组有 FEC 覆盖（收到过 parity）、哪些组到最后仍有 ≥2 片缺失
// （确定不可恢复）。只在帧完成或超时清理时调用一次，所以不会被重复计数。
// 注意：由 NACK 补回后完成的帧不会进不可恢复统计——那时候它确实恢复了。
struct FecSettle {
  uint64_t groups_rx = 0;
  uint64_t unrecoverable = 0;       // 组数
  uint64_t unrecoverable_pkts = 0;  // 这些组里到底缺了几片
};

void settle_fec(FrameAcc& fa, FecSettle& out) {
  for (const auto& [gi, g] : fa.fec_groups) {
    if (!g.available) continue;
    ++out.groups_rx;
    const std::size_t begin = g.offset;
    const std::size_t end = std::min<std::size_t>(fa.count, begin + g.member_count);
    std::size_t missing = 0;
    for (std::size_t i = begin; i < end; ++i) {
      if (fa.parts[i].empty()) ++missing;
    }
    if (missing >= 2) {
      ++out.unrecoverable;
      out.unrecoverable_pkts += missing;
    }
  }
}

class LatencyRing {
 public:
  // 按**时间**淘汰的滑动窗口。
  //
  // 为什么不是按帧数：故障期 12 fps 只产生 120 帧/10 s，而修复后 30 fps 会飞快补
  // 300 帧 —— 按帧数淘汰时环里仍是"故障期帧占多数"，p50 永远被故障期主导。
  // 实测 queue-backlog-12fps-tc：cued 侧已回 30 fps、dropped=0，probe 的
  // buffer p50 却仍是 99 ms（= 故障期 12 fps 的稳态），SLA（p50<=60）因此判失败。
  //
  // 5 s 的取法：settle（3 s）+ 重测先丢弃首条过渡窗口（1 s），环里剩下的
  // 基本全是修复后的帧。再短就会让 p99 在少数样本上抖动。
  static constexpr int64_t kWindowUs = 5'000'000;

  void push(int64_t us, int64_t now_us) {
    buf_.push_back({us, now_us});
    ++n_;
    while (!buf_.empty() && now_us - buf_.front().at > kWindowUs) buf_.pop_front();
    // 兜底：某些调用点没有稳定的 now（或回放时时间倒退），防止无界增长
    while (buf_.size() > kHardCap) buf_.pop_front();
  }
  int64_t percentile(double p) const {
    if (buf_.empty()) return 0;
    std::vector<int64_t> v;
    v.reserve(buf_.size());
    for (const auto& e : buf_) v.push_back(e.us);
    std::sort(v.begin(), v.end());
    std::size_t idx = static_cast<std::size_t>(p * double(v.size() - 1));
    return v[idx];
  }
  std::size_t size() const { return buf_.size(); }
  void clear() { buf_.clear(); }

 private:
  struct Entry {
    int64_t us;
    int64_t at;
  };
  static constexpr std::size_t kHardCap = 8192;
  std::deque<Entry> buf_;
  uint64_t n_ = 0;
};

// 分位行：没有样本时整段是 null。写成 0 会被下游读成「这一段花了 0 ms」——
// 对 buffer 段尤其致命（0 会看起来像"抖动缓冲没等待"，而它可能只是没样本）。
// p95 与 p50/p99 同源：probe 本地就有 LatencyRing，没有理由只发两位而让
// 报告/agent 侧读不到四段的 p95（N-05）。
json seg_or_null(const LatencyRing& r) {
  if (r.size() == 0) return nullptr;
  return json{{"p50", r.percentile(0.50)},
              {"p95", r.percentile(0.95)},
              {"p99", r.percentile(0.99)}};
}

// ---------------------------------------------------------------- Annex-B 参数集
// 解码器在 IDR 之前必须先见到 SPS/PPS。发送端有两种形态：内联在 IDR 前面的参数集
// （浏览器 / x264 的 annexb 输出），以及把参数集放在独立的 codec_config 包里。
// probe 只收到下行 Video 数据报，所以两种都要能接住：本帧内联了就现取；
// 本帧没有就补上之前缓存的两组参数，否则解码器会一直报「缺 SPS」并拒绝出帧。
struct ParamSets {
  std::vector<uint8_t> sps;
  std::vector<uint8_t> pps;

  bool complete() const { return !sps.empty() && !pps.empty(); }
};

// 扫描 Annex-B 起始码；顺手把本帧内联的 SPS/PPS 收进 cache。
// 返回 false = 这段数据里没有任何起始码（不是 Annex-B 帧的形态，喂进去只会浪费解码器）。
bool scan_param_sets(std::span<const uint8_t> in, ParamSets& cache, bool& inline_sps,
                     bool& inline_pps) {
  struct Nal {
    std::size_t sc_pos;  // 起始码首字节
    std::size_t begin;   // NAL 首字节
  };
  std::vector<Nal> nals;
  for (std::size_t i = 0; i + 3 <= in.size();) {
    // 4 字节起始码优先：00 00 00 01 不能被当成 00 00 01 + 一个多余 0
    if (i + 4 <= in.size() && in[i] == 0 && in[i + 1] == 0 && in[i + 2] == 0 && in[i + 3] == 1) {
      nals.push_back(Nal{i, i + 4});
      i += 4;
      continue;
    }
    if (in[i] == 0 && in[i + 1] == 0 && in[i + 2] == 1) {
      nals.push_back(Nal{i, i + 3});
      i += 3;
      continue;
    }
    ++i;
  }
  if (nals.empty()) return false;
  constexpr std::size_t kMaxParamNal = 1024;  // SPS/PPS 不可能更大，异常数据不当参数集
  for (std::size_t k = 0; k < nals.size(); ++k) {
    const std::size_t begin = nals[k].begin;
    const std::size_t end = (k + 1 < nals.size()) ? nals[k + 1].sc_pos : in.size();
    if (end <= begin || end - begin > kMaxParamNal) continue;
    switch (static_cast<uint8_t>(in[begin] & 0x1F)) {
      case 7:
        cache.sps.assign(in.data() + nals[k].sc_pos, in.data() + end);
        inline_sps = true;
        break;
      case 8:
        cache.pps.assign(in.data() + nals[k].sc_pos, in.data() + end);
        inline_pps = true;
        break;
      default:
        break;
    }
  }
  return true;
}

// ---------------------------------------------------------------- 解码 + 帧统计
// 单帧解码失败不是灾难（丢包/乱序都可能让一帧解不出），但**连续**失败说明参考帧或
// 参数集状态已经坏掉：继续硬解只会把每一帧都记成错误，却永远恢复不过来。
// 所以连续失败到阈值就重置解码器（重新 open，等于丢掉全部参考帧），并记一次事件。
constexpr int kMaxDecodeFailStreak = 30;

struct DecodeState {
  cue::media::H264Decoder dec;
  bool opened = false;
  bool params_ready = false;  // 解码器已经在有 SPS/PPS 的帧上成功出过帧
  int fail_streak = 0;
  uint64_t resets = 0;
  ParamSets params;
  std::vector<uint8_t> scratch;  // 补参数集时的拼帧缓冲（复用，避免每帧分配）
  // 见过哪些 codec：跳过事件按「每个 codec 值一条」记，否则 30 fps 的非 H264 流
  // 会在一秒内刷出 30 条同样的日志，把真正的事件冲掉。
  std::map<uint8_t, uint64_t> skipped_codecs;

  bool ensure_open() {
    if (opened) return true;
    std::string err;
    if (!dec.open(err)) {
      std::fprintf(stderr, "[probe] H264 解码器打不开：%s（此后只做收包统计）\n", err.c_str());
      return false;
    }
    opened = true;
    return true;
  }

  void reset() {
    dec.close();
    opened = false;
    params_ready = false;
    fail_streak = 0;
    ++resets;
  }
};

struct FrameStats {
  LatencyRing decode_us;      // 单帧解码耗时
  LatencyRing recv_us;        // 数据报到达 → 被读出
  LatencyRing reassembly_us;  // 首片到达 → 整帧完成
  LatencyRing buffer_us;      // 抖动缓冲等待（进队 → 放行）
  uint64_t decoded = 0;       // 解码出的帧数（fps 的分子）
  uint64_t decode_errors = 0;
  uint64_t decode_skips = 0;    // 非 H264（或未标注）跳过解码的帧
  // 「喂进解码器了，但既没出帧、也没报错」的帧数。这一格以前没有任何计数器：
  // H264Decoder::decode 在 avcodec_receive_frame 只回 EAGAIN 时返回 nullptr 且 err 为空
  // （解码器把包吞进 DPB、暂时不产帧），旧代码走的是 `else if (!derr.empty())`，
  // 于是这些帧**在账本上凭空消失**——frames 涨、decoded 不涨、errors 仍为 0，
  // 看上去像"帧没到"，其实帧到了。frames 与 decoded 对不上时的第一嫌疑人就是它。
  uint64_t decode_no_output = 0;
  uint64_t stalls = 0;
  uint64_t max_stall_us = 0;
  uint64_t recv_ts_missing = 0;  // 内核没给时间戳的次数（不伪造 0，只记账）
  int64_t last_pts_us = -1;
  int64_t last_dec_end_us = -1;

  // 卡顿判定：相邻两帧的 **PTS 间隔**超过 1.5 × 帧间隔即算一次（缺帧/被丢帧时
  // 媒体时间轴会一次性跳过好几个帧间隔，这正是"卡"的可观测量）。
  // PTS 不可用（重启后归零、乱序）时退回解码完成间隔——两条时间轴都有局限，
  // 但都不会把「正常 33 ms 一帧」误判成卡顿。
  void observe_gap(uint64_t pts_us, int64_t dec_end_us, int64_t threshold_us) {
    int64_t gap = -1;
    if (pts_us > 0 && last_pts_us >= 0 && static_cast<int64_t>(pts_us) > last_pts_us &&
        static_cast<int64_t>(pts_us) - last_pts_us < 5'000'000) {
      gap = static_cast<int64_t>(pts_us) - last_pts_us;
    } else if (last_dec_end_us >= 0) {
      gap = dec_end_us - last_dec_end_us;
    }
    if (gap > 0 && gap > threshold_us) {
      ++stalls;
      if (static_cast<uint64_t>(gap) > max_stall_us) max_stall_us = static_cast<uint64_t>(gap);
    }
    if (pts_us > 0) last_pts_us = static_cast<int64_t>(pts_us);
    last_dec_end_us = dec_end_us;
  }
};

// ---------------------------------------------------------------- 到达间隔分布
// 为什么需要它：`fps` 是**平均值**，`stalls` 是按 PTS 轴算的卡顿数——两样都看不出「帧成簇
// 到达」。一簇 0.4 s 的到达模式（0.4 s 完全静默，然后十几帧挤在同一瞬间）平均下来仍然是
// 30 fps，PTS 轴上一个洞都没有（所以 jb_skipped 也是 0），但它恰恰是「每 0.4 s 顿一下」的
// 直接原因：播放端每簇只被喂一次，簇内各帧的驻留时长天差地别。只有**间隔分布**能把它变成
// 数据，而不是靠"音频那边也这样"的类比去推。
//
// 度量点选在「整帧组装完成」时刻，也就是 jb.push 收到的 arrival_us：
// 抖动缓冲的时间轴就是它，指标必须和缓冲说同一件事。用首片到达会更接近网络层，但会把
// 「大 IDR 在途更久」混进来（见 JitterBasis 的注释），反而更难解释。代价是 IDR 那一帧的
// 间隔被抬高十几 ms——对「有没有 0.4 s 空档」这种量级的问题没有影响。
struct ArrivalGapMeter {
  // 桶上界（µs，含上界）：5 / 15 / 33 / 66 / 133 / 266 / 533 ms，最后一桶是更大的。
  // 取值理由：33 ms 是 30 fps 的帧栅格，每档是它的约 1/2、1、2、4、8、16 倍。
  // 一簇到达在直方图上的形状是「第 0 桶爆量 + 偶尔一个大桶」，连续到达则是「第 3 桶稳定」。
  static constexpr std::size_t kBuckets = 8;
  static constexpr int64_t kEdgesUs[kBuckets - 1] = {5'000,  15'000,  33'000,  66'000,
                                                     133'000, 266'000, 533'000};
  // 「空档」判据 > 50 ms：等于 33 ms 帧栅格的 1.5 倍，与卡顿判据（--stall-factor 1.5）同源，
  // 两个指标因此可以互相印证，而不是各说各话。
  static constexpr int64_t kGapAlertUs = 50'000;

  std::array<uint64_t, kBuckets> histogram{};
  uint64_t samples = 0;          // 本窗口的间隔样本数（到达事件数 − 1；帧级实例就是帧数 − 1）
  uint64_t gaps = 0;             // 本窗口 > kGapAlertUs 的间隔个数
  int64_t max_gap_us = 0;        // 本窗口最大空档
  int64_t max_gap_us_total = 0;  // 进程内最大空档（窗口清零也不丢，抓偶发的最坏值）
  int64_t last_us = 0;
  bool have_last = false;

  static std::size_t bucket_of(int64_t gap_us) {
    for (std::size_t i = 0; i < kBuckets - 1; ++i) {
      if (gap_us <= kEdgesUs[i]) return i;
    }
    return kBuckets - 1;
  }

  void observe(int64_t now_us) {
    if (have_last) {
      const int64_t gap = now_us - last_us;
      // 负间隔（时钟源回跳）当噪声丢掉，不记样本：记成 0 会把「没有观测」伪装成
      // 「观测到 0 ms」，那正好是"成簇"的读数，等于自己造假证据。
      if (gap >= 0) {
        ++samples;
        ++histogram[bucket_of(gap)];
        if (gap > kGapAlertUs) ++gaps;
        if (gap > max_gap_us) max_gap_us = gap;
        if (gap > max_gap_us_total) max_gap_us_total = gap;
      }
    }
    last_us = now_us;
    have_last = true;
  }

  // 指标行是「这一秒」的读数，不是累计值（与 fps / 四段分位同一口径）。
  // max_gap_us_total 故意不在这里清零：单秒最大值会被窗口边界切掉半个空档，
  // 而"全程最坏值"恰恰是报告里要引用的那个数。
  void clear_window() {
    histogram.fill(0);
    samples = 0;
    gaps = 0;
    max_gap_us = 0;
  }

  // 换代（新会话 / 换对端）时丢掉的只是「上一帧是什么时候」这个基准：两次换代之间的静默
  // 是进程在重连，不是链路的到达空档，留着它会在直方图里制造一个几秒宽的假空档——
  // 而那个假空档恰好会被读成"成簇"的证据，正是本次要查的那件事。
  // 全程最大值（max_gap_us_total）保留：它是进程级的最坏值，与哪一代会话无关。
  void rebase() {
    have_last = false;
    clear_window();
  }
};

// ---------------------------------------------------------------- 跨机上报
// 本地模式下 harness 直接读 probe 的 stdout（管道），不需要这条通道；跨机时 probe 与
// harness 不在同一台机器上，所以额外提供 TCP + JSON-line 上报（一行一个事件对象）。
// 约束：上报是**尽力而为**，不许影响收包主循环——连接非阻塞、失败只丢这一行并计数，
// 断线后按 1 s 节流重连。丢掉的行数会进指标行（report_drops），不静默。
// 解析 "host:port[,host:port]"；返回 false 表示有一项不合法（调用方按参数错误退出）
bool parse_report_addrs(std::string_view spec, std::vector<TcpReporters::Addr>& out) {
  std::size_t i = 0;
  while (i < spec.size()) {
    std::size_t comma = spec.find(',', i);
    if (comma == std::string_view::npos) comma = spec.size();
    const std::string_view item = spec.substr(i, comma - i);
    i = comma + 1;
    if (item.empty()) continue;
    const std::size_t colon = item.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= item.size()) return false;
    long long port = 0;
    if (!parse_int(item.substr(colon + 1), 1, 65535, port)) return false;
    out.push_back(TcpReporters::Addr{std::string(item.substr(0, colon)),
                                    static_cast<uint16_t>(port)});
  }
  return true;
}

// 超时清理（2 s TTL）。**只负责时间这一维**：帧数与字节上限现在由
// proto/reassembly_budget.h 的预算决策 + evict_oldest 负责（N-10），两者结算口径一致。
void cleanup_stale(std::map<uint32_t, FrameAcc>& pending, int64_t now_us, uint64_t& lost,
                   LossWindow& win, FecSettle& fec, uint64_t& pending_bytes) {
  for (auto it = pending.begin(); it != pending.end();) {
    if (now_us - it->second.first_seen_us > 2'000'000) {
      ++lost;  // 超时未被完成即计丢失（不再依赖 pending.size()>256 这条死条件）
      settle(win, it->second);
      settle_fec(it->second, fec);
      pending_bytes -= it->second.bytes;  // 槽位连同它占的字节一起消失
      it = pending.erase(it);
    } else {
      ++it;
    }
  }
}

// 把一帧的全部分片拼成一整段 Annex-B 字节流
std::vector<uint8_t> assemble(const FrameAcc& fa) {
  std::size_t total = 0;
  for (const auto& part : fa.parts) total += part.size();
  std::vector<uint8_t> frame;
  frame.reserve(total);
  for (const auto& part : fa.parts) frame.insert(frame.end(), part.begin(), part.end());
  return frame;
}


}  // namespace

int main(int argc, char** argv) {
  cue::app::NativeArguments arguments;
  if (const auto result = arguments.initialize(argc, argv); !result) {
    std::fprintf(stderr, "[probe] command line: %s\n", result.error().c_str());
    return 2;
  }
  argc = arguments.argc();
  argv = arguments.argv();
  SocketRuntime sockets;
  if (!sockets.ok()) {
    std::fprintf(stderr, "[probe] Winsock initialization failed\n");
    return 1;
  }

  uint16_t port = 9001;
  bool nack_enabled = true;
  long long nack_delay_ms = 15;
  // SPEC §7.2：NACK 限速 8 包/秒。默认间隔 125 ms 正好对应 8 包/秒；间隔更大只会更慢。
  // 间隔小于 125 ms 时由 NackRateLimiter 夹到 8 包/秒的上限（这是协议上限，不是可选项）。
  long long nack_interval_ms = 125;
  // 有重传在途时空洞的最长等待：要盖住「NACK 发出 → cued 重传 → 到达」一次往返。
  // 120 ms 的取法：本机往返 <1 ms、跨机局域网通常 <10 ms，留足余量但仍远小于
  // "追赶积压"造成的秒级延迟。设 0 = 关闭联动（退化成只等 budget）。
  long long nack_wait_ms = 120;
  // 抖动估计的基准：first（默认，首片到达）| done（组装完成，本改动前行为）。
  // 留 done 是为了能 A/B 量化"帧在途耗时污染抖动估计"的幅度。
  std::string jitter_basis = "first";
  // 渲染/播放（"能看能听"）：默认关。开着才需要 SDL2，无头环境不受影响。
  bool render_enabled = false;
  int render_w = 0;
  int render_h = 0;
  // 标称帧率只用于「卡顿阈值」的推断（阈值 = stall_factor × 1/fps），不是测量值。
  // 30 是这个项目的默认推流帧率（cued 的 --fps 默认 30）。
  long long nominal_fps = 30;
  double stall_factor = 1.5;
  // 对端切换的静默阈值。原实现是 3 s（review #20 的防串流考虑），但 harness 现在会
  // 主动重启 cued（改档/注入），3 s 静默意味着每次重启都要黑屏 3 s——而重测窗口只有
  // 约 1 s，端到端指标会整段缺失。500 ms 仍然要求"当前 peer 确实安静了很久"
  // （30 fps 下任何活着的 cued 间隔都 ≤33 ms），足以防串流，又不至于吞掉重测窗口。
  long long peer_timeout_ms = 500;
  // 抖动缓冲（FR-PROBE.4）：默认开、默认自适应。
  // 档位阶梯真实是 kDepthLadder = 1/2/4/8/16 帧（30 fps 下 0/33/100/233/500 ms 驻留）。
  // 默认最高只到 2 档（4 帧 ≈ 100 ms）：16 帧档在实时链路上就是半秒延迟，必须显式
  // --jitter-max-level 4 才打开。
  // 注意阶梯**加长并不改善"帧没到齐"**（见 jitter_buffer.h 文件头的三种丢帧）：
  // 它只对"帧都在、只是晚到/乱序"有用，对 PTS 轴上的洞一点办法都没有。
  bool jitter_enabled = true;
  long long jitter_budget_ms = 40;
  // 音频输出设备名（子串匹配）。空 = 系统默认。
  // 虚拟声卡环境里系统默认常常就是被采集的那一路，会让 probe 播出的
  // 声音回流进链路，听起来像回声——必须能指定。
  std::string audio_out;
  // 周期性截图（0 = 关）：把渲染结果存成 BMP，供远程检查画面质量。
  long long snap_ms = 0;
  // 视频呈现延迟（毫秒）。负数 = 自动跟随音频播放队列深度（两者相等才音画同步）。
  // 0 = 不延迟（旧行为）。
  long long av_delay_ms = 0;
  // 播放前预缓冲（毫秒）：公网有到达抖动，攒一小段再开播可避免设备饿着。
  long long audio_prebuffer_ms = 0;
  std::string snap_prefix;
  bool list_audio = false;
  long long jitter_max_level = 2;
  // 抖动缓冲的深度（帧）。**语义在 2026-10 修正过一次**，理由值得记下来：
  //   probe 的自适应是跟着 --jitter 一起开的（adaptive = enabled），而自适应时
  //   JitterBuffer::target_frames() 只认 kDepthLadder[level]，cfg.target_frames 被**完全忽略**。
  //   所以旧语义（"固定目标深度 2 帧"）在这里一直是空转：把 2 改成 4 实测毫无变化，
  //   因为 0 档的目标深度恒为 1。那种"调了没用、看起来却像调了"的参数比没有更糟。
  //   cued 侧不受影响：它不开自适应，target_frames 在那边仍然是字面深度。
  // 新语义：0（默认）= 完全跟随自适应，从 0 档起步（与旧默认的实际行为一致，延迟不变）；
  //         N > 0   = 自适应档位的**下限**，取阶梯上深度 <= N 的最深一档。
  //         例如 2 → 1 档（2 帧），4 → 2 档，8 → 3 档。
  // 这样 --jitter-max-level 与它一起构成"档位区间"，语义只有一个：档位决定深度。
  long long jitter_target_frames = 0;
  std::string bind_host = "0.0.0.0";
  std::vector<TcpReporters::Addr> report_addrs;
  // NAT 注册目标：周期性发 8 字节 "CUEHELLO"，让服务器学到我们经过 NAT 的外部地址。
  // 公网部署必需——客户端在 NAT 后面，不先发出去，服务器就没法把 UDP 回给我们。
  std::string hello_host;
  uint16_t hello_port = 0;
  // N-01：音频下行的 in-band FEC 恢复与 PLC。默认都开（弱网默认要能用），
  // 留 off 是为了能 A/B 量化"到底补回来多少帧"——scripts/n01-fec-ab.sh 的基线
  // 就是两个都关（那时全部缺口都只能计入 audio_unconcealed_frames）。
  bool audio_fec_enabled = true;
  bool audio_plc_enabled = true;

  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto next = [&]() -> std::string_view {
      return (i + 1 < argc) ? std::string_view(argv[++i]) : std::string_view();
    };
    auto bad = [&](std::string_view flag, std::string_view val) {
      std::fprintf(stderr, "[probe] 参数 %s 非法: '%.*s'\n", std::string(flag).c_str(),
                   static_cast<int>(val.size()), val.data());
      std::exit(2);
    };
    if (a == "--port") {
      long long v = 0;
      const auto val = next();
      if (!parse_int(val, 1, 65535, v)) bad(a, val);
      port = static_cast<uint16_t>(v);
    } else if (a == "--bind") {
      bind_host = std::string(next());
      if (bind_host.empty()) bad(a, bind_host);
    } else if (a == "--fps") {
      const auto val = next();
      if (!parse_int(val, 1, 240, nominal_fps)) bad(a, val);
    } else if (a == "--stall-factor") {
      const auto val = next();
      if (!parse_double(val, 1.0, 10.0, stall_factor)) bad(a, val);
    } else if (a == "--report-addrs") {
      const auto val = next();
      if (!parse_report_addrs(val, report_addrs)) bad(a, val);
    } else if (a == "--hello-to") {
      const auto val = next();
      const std::size_t colon = val.rfind(':');
      if (colon == std::string_view::npos) bad(a, val);
      hello_host = std::string(val.substr(0, colon));
      long long hp = 0;
      if (!parse_int(val.substr(colon + 1), 1, 65535, hp)) bad(a, val);
      hello_port = static_cast<uint16_t>(hp);
    } else if (a == "--jitter") {
      const auto val = next();
      if (val != "on" && val != "off") bad(a, val);
      jitter_enabled = (val == "on");
    } else if (a == "--jitter-budget-ms") {
      const auto val = next();
      if (!parse_int(val, 0, 2000, jitter_budget_ms)) bad(a, val);
    } else if (a == "--audio-out") {
      audio_out = std::string(next());
    } else if (a == "--audio-prebuffer-ms") {
      const auto val = next();
      if (!parse_int(val, 0, 2000, audio_prebuffer_ms)) bad(a, val);
    } else if (a == "--audio-fec") {
      // 只影响**恢复**路径（用后一包的 LBRR 救前一帧），不影响正常解码。
      const auto val = next();
      if (val != "on" && val != "off") bad(a, val);
      audio_fec_enabled = (val == "on");
    } else if (a == "--audio-plc") {
      // 只影响**缺口**（帧号前进跳出的洞）的隐藏帧生成，与空包/坏包无关。
      const auto val = next();
      if (val != "on" && val != "off") bad(a, val);
      audio_plc_enabled = (val == "on");
    } else if (a == "--av-delay-ms") {
      const auto val = next();
      if (!parse_int(val, -1, 5000, av_delay_ms)) bad(a, val);
    } else if (a == "--snap-ms") {
      const auto val = next();
      if (!parse_int(val, 0, 600000, snap_ms)) bad(a, val);
    } else if (a == "--snap-prefix") {
      snap_prefix = std::string(next());
    } else if (a == "--list-audio") {
      list_audio = true;
    } else if (a == "--jitter-target-frames") {
      const auto val = next();
      // 上界跟着阶梯走：阶梯最深 16 帧，再大的下限没有对应的档位。
      if (!parse_int(val, 0, 16, jitter_target_frames)) bad(a, val);
    } else if (a == "--jitter-max-level") {
      const auto val = next();
      // 0..4 对应 kDepthLadder 的五个档位。上界写成常量而不是字面 4：以后再加档位时
      // 这里少改一处就少一个「参数合法但档位到不了」的坑。
      if (!parse_int(val, 0, static_cast<long long>(cue::media::kDepthLadderSize) - 1,
                     jitter_max_level)) {
        bad(a, val);
      }
    } else if (a == "--peer-timeout-ms") {
      const auto val = next();
      if (!parse_int(val, 50, 60000, peer_timeout_ms)) bad(a, val);
    } else if (a == "--nack") {
      const auto val = next();
      if (val != "on" && val != "off") bad(a, val);
      nack_enabled = (val == "on");
    } else if (a == "--nack-delay-ms") {
      const auto val = next();
      if (!parse_int(val, 0, 5000, nack_delay_ms)) bad(a, val);
    } else if (a == "--render") {
      render_enabled = true;
    } else if (a == "--render-size") {
      const auto val = next();
      int rw = 0, rh = 0;
      if (std::sscanf(std::string(val).c_str(), "%dx%d", &rw, &rh) != 2 || rw <= 0 || rh <= 0) {
        bad(a, val);
      }
      render_w = rw;
      render_h = rh;
    } else if (a == "--jitter-basis") {
      const auto val = next();
      if (val != "first" && val != "done") bad(a, val);
      jitter_basis = std::string(val);
    } else if (a == "--nack-wait-ms") {
      const auto val = next();
      if (!parse_int(val, 0, 2000, nack_wait_ms)) bad(a, val);
    } else if (a == "--nack-interval-ms") {
      const auto val = next();
      if (!parse_int(val, 1, 10000, nack_interval_ms)) bad(a, val);
    } else if (a == "--help") {
      std::printf(
          "cue-probe --port 9001 [--bind 0.0.0.0] [--nack on|off] [--nack-delay-ms 15]\n"
          "          [--nack-interval-ms 125] [--nack-wait-ms 120] [--jitter-basis first|done]\n"
          "          [--fps 30] [--stall-factor 1.5]\n"
          "          [--peer-timeout-ms 500] [--jitter on|off] [--jitter-budget-ms 40]\n"
          "          [--jitter-max-level 0..4] [--jitter-target-frames 0..16]\n"
          "          [--audio-out NAME] [--av-delay-ms N] [--audio-prebuffer-ms N] [--list-audio]\n"
          "          [--audio-fec on|off] [--audio-plc on|off]\n"
          "          [--snap-ms N --snap-prefix PATH]\n"
          "          [--report-addrs host:port[,host:port]]\n"
          "  --nack-interval-ms 只决定「多长时间尝试一次」；实际发送上限是 8 包/秒\n"
          "  （SPEC §7.2），间隔 <125 ms 会被夹到该上限。冷却/单包探测不需要额外开关。\n"
          "  --audio-fec / --audio-plc 默认都开，只作用于**帧号缺口**（确知丢包）：\n"
          "  FEC 用后一包里冗余恢复前一帧，PLC 外推其余缺口（每次缺口最多 3 帧 = 60 ms）。\n"
          "  两个都关时全部缺口只计入 audio_unconcealed_frames（A/B 的基线）。\n"
          "  --jitter-max-level 与 --jitter-target-frames 共同定义自适应档位区间：\n"
          "  档位 0..4 = 驻留 1/2/4/8/16 帧（30 fps 下 0/33/100/233/500 ms）。\n"
          "  --jitter-target-frames 是区间下限（0 = 不设限，从 0 档起步）。加长区间只对\n"
          "  「帧晚到/乱序」有用，对「帧根本没到齐」（PTS 轴有洞）不会减少一个丢帧。\n"
          "  feedback（type=0x06）每 200 ms 自动发一条，无需参数。\n");
      return 0;
    } else {
      std::fprintf(stderr, "[probe] 未知参数: %.*s\n", static_cast<int>(a.size()), a.data());
      return 2;
    }
  }

  // 卡顿阈值：1.5 × 帧间隔（30 fps → 50 ms）。可配是因为「卡」在不同帧率下不是同一个
  // 秒数：把 30 fps 的阈值套到 10 fps 的流上，每帧都会被判成卡顿。
  const int64_t peer_timeout_us = peer_timeout_ms * 1000;
  const int64_t frame_interval_us = 1'000'000 / nominal_fps;
  const int64_t stall_threshold_us =
      static_cast<int64_t>(stall_factor * static_cast<double>(frame_interval_us));

  // 抖动缓冲配置：容量上限比最高档位多留 2 帧余量（留余量是为了"超过目标深度"能被观察到，
  // 而不是一超就挤掉——容量是硬上限，不是目标深度）。容量必须 >= 最深档位的深度，否则深处
  // 档位自己就会制造丢帧（队列被空洞挡住时，超容量的帧由 evict_one 挤掉）。
  cue::media::JitterBufferConfig jb_cfg;
  jb_cfg.enabled = jitter_enabled;
  jb_cfg.adaptive = jitter_enabled;
  jb_cfg.max_level = static_cast<std::size_t>(jitter_max_level);
  if (jb_cfg.max_level >= cue::media::kDepthLadderSize) {
    jb_cfg.max_level = cue::media::kDepthLadderSize - 1;  // 兜底：命令行已被 clamp 过
  }
  // --jitter-target-frames 现在表达「自适应的下限档位」，映射规则见该参数的注释。
  // 逐档比较而不是查表：阶梯可以加长，这条映射不需要跟着改。
  std::size_t floor_level = 0;
  if (jitter_target_frames > 0) {
    const auto want = static_cast<std::size_t>(jitter_target_frames);
    while (floor_level + 1 < cue::media::kDepthLadderSize &&
           cue::media::kDepthLadder[floor_level + 1] <= want) {
      ++floor_level;
    }
  }
  jb_cfg.min_level = std::min(floor_level, jb_cfg.max_level);
  // 显式设定而不是靠默认值：默认值改了不会有人知道，而 SLA 门限是按它算的。
  // 自适应时它不生效（深度由档位决定），留这一行只为把两处口径写在同一个地方。
  jb_cfg.target_frames = cue::media::kDepthLadder[jb_cfg.min_level];
  jb_cfg.max_frames = cue::media::kDepthLadder[jb_cfg.max_level] + 2;
  jb_cfg.nominal_interval_us = frame_interval_us;
  jb_cfg.budget_us = jitter_budget_ms * 1000;
  if (list_audio) {
    // 打印可用播放设备，方便挑 --audio-out 的字符串（虚拟声卡的名字很难手打对）
    for (const auto& nm : cue::probe::Renderer::audio_outputs()) {
      std::printf("%s\n", nm.c_str());
    }
    return 0;
  }
  jb_cfg.nack_wait_us = nack_wait_ms * 1000;  // 0 = 不联动（只有 budget 生效）
  jb_cfg.basis = (jitter_basis == "done") ? cue::media::JitterBasis::AssemblyDone
                                          : cue::media::JitterBasis::FirstFragment;  // 0 = 不联动（只有 budget 生效）

#if defined(_WIN32)
  // Windows 分支**不用 Boost.Asio 的 socket**：Asio 在 Windows 上会把 socket 关联到
  // IOCP，而 WSAPoll 是 select 语义，两者冲突时 poll 永远不触发。实测症状很隐蔽——
  // bind 成功、发包端确实发了（用 PowerShell 的 UdpClient 对照能收到），但 probe 的
  // pkts 恒 0、且不报任何错。所以这里直接用原生 winsock 句柄。
  const probe_socket_t udp_fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (udp_fd == kProbeInvalidSocket) {
    std::fprintf(stderr, "[probe] socket() 失败: %d\n", ::WSAGetLastError());
    return 1;
  }
  sockaddr_in baddr{};
  baddr.sin_family = AF_INET;
  baddr.sin_port = htons(static_cast<u_short>(port));
  if (::inet_pton(AF_INET, bind_host.c_str(), &baddr.sin_addr) != 1) {
    std::fprintf(stderr, "[probe] --bind 地址非法: %s\n", bind_host.c_str());
    return 2;
  }
  if (::bind(udp_fd, reinterpret_cast<sockaddr*>(&baddr), sizeof(baddr)) != 0) {
    std::fprintf(stderr, "[probe] bind %s:%u failed: %d\n", bind_host.c_str(), port,
                 ::WSAGetLastError());
    return 1;
  }
  {
    // 配 CUE_PROBE_NOPOLL 诊断用：没有 poll 时 recvfrom 靠这个超时返回
    int rcvto = 200;
    ::setsockopt(udp_fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&rcvto),
                 static_cast<int>(sizeof(rcvto)));
    int rcvbuf = 4 * 1024 * 1024;
    ::setsockopt(udp_fd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&rcvbuf),
                 static_cast<int>(sizeof(rcvbuf)));
  }
#else
  net::io_context ioc;
  boost::system::error_code ec;
  udp::socket sock(ioc);
  sock.open(udp::v4(), ec);
  boost::system::error_code bec;
  const net::ip::address_v4 bind_addr = net::ip::make_address_v4(bind_host, bec);
  if (bec) {
    std::fprintf(stderr, "[probe] --bind 地址非法: %s\n", bind_host.c_str());
    return 2;
  }
  const udp::endpoint bind_ep(bind_addr, port);
  sock.bind(bind_ep, ec);
  if (ec) {
    std::fprintf(stderr, "[probe] bind %s:%u failed: %s\n", bind_host.c_str(), port,
                 ec.message().c_str());
    return 1;
  }
  sock.set_option(udp::socket::receive_buffer_size(4 * 1024 * 1024), ec);
  const probe_socket_t udp_fd = static_cast<probe_socket_t>(sock.native_handle());
#endif
  // SO_TIMESTAMP：让内核在数据报上打到达时间戳，recv 段（到达 → 被读出）才有意义。
  // 没有它就只能测到 poll 之后的时间（恒 ≈0），那等于没测；取不到时间戳时宁可
  // 记 recv_ts_missing 并让该段无样本，也不写一个假的 0。
#if !defined(_WIN32)
  {
    const int on = 1;
    if (::setsockopt(udp_fd, SOL_SOCKET, SO_TIMESTAMP, &on, sizeof(on)) != 0) {
      std::fprintf(stderr, "[probe] SO_TIMESTAMP 打开失败（recv 段将无样本）：%s\n",
                   std::strerror(errno));
    }
  }
#endif
  std::fprintf(stderr, "[probe] listening udp %s:%u\n", bind_host.c_str(), port);

  TcpReporters reporters(report_addrs);
  // 每条事件既进 stdout（本地 harness 读管道），也进 TCP 上报（跨机）。
  // report 只有在 --report-addrs 非空时才真正发送。
  auto emit = [&](std::string_view ev, const json& data) {
    const std::string body = event_body(ev, data);
    cue::proto::emit_event_line(body);
    if (reporters.empty()) return;
    // 上报通道是"一行一个 JSON 对象"（JSON-line）：行尾的 \n 属于协议的一部分，
    // 少了它接收端只能靠猜边界（实测：三行黏成一行，按行解析直接失败）。
    const std::string line = body + "\n";
    reporters.publish(line, cue::now_us());
  };

  emit("probe.started",
       json{{"pid", ::getpid()},
            {"port", port},
            {"bind", bind_host},
            // 多设备会话（T-22）：一个探针实例只听**一路**（cued 的槽位 i → 端口
            // --probe-port+i）。这里把自己的端口写进事件流，会话号在首包到达后
            // 由 probe.metric 的 session_id 报出来，两边的映射因此可查。
            {"scope", "per_session_port"},
            {"nack", nack_enabled ? "on" : "off"},
            {"nack_interval_ms", nack_interval_ms},
            // 限速上限写进事件流：报告里说"8 包/秒"时这里能对得上，不必看命令行。
            {"nack_rate_pkt_s", std::min(8.0, 1000.0 / double(std::max<long long>(nack_interval_ms, 1)))},
            {"nack_cooldown_ms", 1000},
            {"feedback", "on"},
            {"feedback_interval_ms", 200},
            {"nominal_fps", nominal_fps},
            {"stall_threshold_us", stall_threshold_us},
            {"report_addrs", report_addrs.size()},
            {"jitter", jitter_enabled ? "on" : "off"},
            {"jitter_budget_ms", jitter_budget_ms},
            // 档位区间（不是"深度"）：min 来自 --jitter-target-frames 的映射，
            // max 来自 --jitter-max-level。报告里要说明"驻留多少毫秒"必须看这两个，
            // 而不是看 target_frames——自适应时它不生效。
            {"jitter_level_min", jb_cfg.min_level},
            {"jitter_max_level", jb_cfg.max_level},
            {"jitter_max_frames", jb_cfg.max_frames}});

  std::map<uint32_t, FrameAcc> pending;
  // pending 当前占住的总字节数 = 各槽位 `FrameAcc::bytes` 之和（维护在下面"重组表内存护栏"段）。
  // 单独一个总数器是为了让预算判据 O(1)：判据每收一个包都要问一次。
  uint64_t pending_bytes = 0;
  LatencyRing lat;
  ClockAlign clk;
  DecodeState dec;
  FrameStats fs;
  // 帧到达间隔分布（见 ArrivalGapMeter）：它是"成簇"唯一的直接证据。
  // 单独一个对象而不是塞进 FrameStats：它的窗口语义（每秒清零 + 全程最大值保留）
  // 与 FrameStats 里的分位环不一样，混在一起迟早会有人误清掉那个全程最大值。
  ArrivalGapMeter arrival_gaps;
  // 同一种度量的第二个采样点：数据报的内核到达时刻（纯到达侧，不受本进程掉链子的影响）。
  // 两者的对比才是"成簇"的归因证据，只有一个序列的话两种原因长得一模一样。
  ArrivalGapMeter datagram_gaps;
  // 与 NACK 联动（在 `pending` 声明之后接线）：抖动缓冲遇到 PTS 空洞时，如果那个缺失的
  // 帧正在被重传（`nack_queued`），就把等待预算放宽到 nack_wait_us。
  // 用 PTS 开区间查询而不是 frame_id：`FrameAcc` 本身就带 pts_us，且不依赖 frame_id 连续
  // （cued 重启、转码重建时它会重新计数）。
  // 单包探测模式**故意不参与**：探测的语义就是「少发、少等、能恢复就恢复」，
  // 给它放宽等待会把冷却期又变回高延迟期，与 FR-PROBE.2 的初衷相反。
  // 只在真的查到在途时放宽，查不到就退回原行为——这条差异是 A/B 对比的基准。
  if (nack_enabled) {
    jb_cfg.retransmit_pending = [&pending](uint64_t pts_lo, uint64_t pts_hi) {
      for (const auto& [fid, fa] : pending) {
        (void)fid;
        if (fa.nack_queued && fa.pts_us > pts_lo && fa.pts_us < pts_hi) return true;
      }
      return false;
    };
  }
  // 抖动缓冲：插在重组之后、解码之前。组件不带线程，队列与时间都由本循环驱动。
  cue::media::JitterBuffer jb(jb_cfg);
  uint64_t frames = 0, lost_frames = 0, bytes = 0, packets = 0;
  // 因抖动缓冲跳帧而 flush 解码器的次数：这是"参考链断过"唯一直接的证据，
  // 不加就只剩"画面偶尔冻一下"这种主观描述。
  // 参考链断裂次数。**只计数不修**，理由见 note_refchain_break 的注释。
  uint64_t refchain_breaks = 0;
  // 主动请求关键帧的次数与被冷却挡下的次数。没有这两个数，
  // "请求到底发没发出去、是不是被自己的限流吃掉了"就只能猜。
  uint64_t kf_req_sent = 0;
  uint64_t kf_req_suppressed = 0;
  bool kf_needed = false;  // 参考链断了、还没发出请求（主循环消费）
  // 上次请求时的 decode_errors 水位：只有它增长（真的解不出图）才值得要 IDR。
  uint64_t kf_err_baseline = 0;
  uint64_t rejected = 0, lat_invalid = 0, stats_reqs = 0, fb_sent = 0;
  // 下行音频数据报计数。cued 把上行 Opus 原样透传成 DownType::Audio（见 apps/cued.cpp
  // 的"音频下行"段），所以这个数会跟着音频流上涨；它同时也是 N-01 缺口率的分母
  // （audio_seq_gaps / audio_datagrams）。早期版本这里恒为 0（当时没有发送方），
  // 留着的初衷不变：让"下行到底有没有音频"成为可观测事实，而不是靠一句说明。
  uint64_t audio_datagrams = 0;
  // FEC 账（FR-PIPE.9）：恢复的片数、有 FEC 覆盖的组数、结算时仍缺 ≥2 片的组与片数。
  // groups_rx 与 recovered/unrecoverable 一起构成「恢复率」的可执行分母：
  //   rate = recovered / (recovered + unrecoverable_pkts)
  // 只拿 recovered 一个数讲不了"到底有多少丢了没救回来"。
  uint64_t fec_recovered = 0;
  FecSettle fec_totals;
  // 幽灵帧守卫的账（M0 教训 / N-02）：两条分支都会遇到"该帧已判定完成，却又有一个
  // 数据报要为它重建槽位"。重建槽位会让同一帧被计第二次完成数（零丢包时 no_data 恰等于
  // FEC 包数，完成帧数甚至超过发送帧数）。两条分支的**性质不同**，所以分开计：
  //   fec_ghost_discarded  = FEC 组的校验包在该帧已判完成后到达 → 不给它重建槽位。
  //     FEC 校验包本来就排在整帧数据之后，所以这是 FEC 开启后的**常态**：实测零丢包
  //     8 s / 241 帧 → 250 次（约每完成帧 1 次）；5% 丢包 8 s → 187~190 次（6 s
  //     实测 146 次），随完成帧数同比例下降。它是"守卫在跑、这条路径有账"的读数，
  //     **不是异常指标**；必须与 frames / fec_groups_rx 一起看比率才有意义。
  //   frag_ghost_discarded = 迟到的视频分片 / NACK 重传试图为已完成帧重建槽位。
  //     正常链路不该发生：实测零丢包 = 0；5% 丢包 = 2~3。**这个才是异常指示**。
  // 分开的理由：混成一个数会把"每完成帧一次的常态"读成故障，也会把真正需要关注的
  // 迟到分片淹没在常态里——两者的判读方式完全不同（比率 vs 是否为 0）。
  uint64_t fec_ghost_discarded = 0;
  uint64_t frag_ghost_discarded = 0;
  uint32_t max_completed = 0;
  // max_completed 的初值是 0，直接用 `fid <= max_completed` 会把**第一帧**（frame_id=0）
  // 当成"已完成帧的迟到分片"丢掉。丢了 IDR 的代价是解码器拿不到 SPS/PPS，接下来整整
  // 一个 GOP 都解不出帧（实测：转码链路 decoded=0、decode_errors 每秒 30）。
  bool have_completed = false;
  sockaddr_in peer{};
  bool have_peer = false;
  int64_t t_last_peer_us = 0;
  uint32_t session_seen = 0;
  bool warned_clock = false;
  const int64_t nack_interval_us = nack_interval_ms * 1000;
  const int64_t nack_delay_us = nack_delay_ms * 1000;
  // NACK 策略（限速 + 冷却 + 单包探测）：纯决策对象，时钟由本循环喂。
  // 速率由 --nack-interval-ms 推出来，但 NackRateLimiter 内部再夹一次 8 包/秒。
  cue::proto::NackController::Config nack_cfg;
  nack_cfg.packets_per_sec = 1000.0 / double(std::max<long long>(nack_interval_ms, 1));
  cue::proto::NackController nack_ctl(nack_cfg);
  int64_t last_nack_attempt_us = 0;
  // 单包探测的目标（fid, frag）：发出去后等这一片回来；回来 = 探测成功 → 恢复常规。
  bool nack_probe_pending = false;
  uint32_t nack_probe_fid = 0;
  uint16_t nack_probe_frag = 0;
  uint64_t frames_prev = 0;
  uint64_t decoded_prev = 0;
  int64_t t_last_report = cue::now_us();
  int64_t t_last_cleanup = cue::now_us();
  int64_t t_last_window = cue::now_us();
  // SPEC §7.1 的口径是「8 次往返取 RTT 最小者」。只发一次的话"最小"无从谈起：
  // cued 在编码繁忙时回包会被推迟，单样本估计直接把这个推迟算进 offset，
  // 端到端延迟就会出现一个恒定偏差（实测：4K/placebo 类 case 的 p99 永远停在 1.5~2.4 s，
  // 修复之后也不回落）。所以每次对齐发一个 8 样本的小批（间隔 20 ms），批内取最小 RTT。
  constexpr int kStatsBurst = 8;
  constexpr int64_t kStatsBurstGapUs = 20'000;
  int stats_burst_left = kStatsBurst;
  int64_t next_stats_us = 0;  // 首包到达后立刻问一次时钟
  LossWindow win;
  double win_loss = 0.0;
  bool warned_bad_frame = false;
  uint64_t bad_frame_events = 0;

  // ---- 重组表的内存护栏（N-10 / FR-PIPE.14「所有队列有硬上限，超限丢帧并计数」）----
  // pending 过去的唯一上限是 cleanup_stale 的 2 s TTL（外加单帧 kMaxFragPerFrame=1024），
  // 于是**帧数**无界：下行字节来自浏览器转发，只要每帧发 1 片、帧号递增，2 s 内就能攒出
  // 任意多个槽位，每个槽位自带 parts/fec_groups 两份容器。策略在 proto/reassembly_budget.h
  // （纯函数，单测 test_reassembly_budget），这里只负责"淘汰哪几帧、怎么结算"。
  // 两个计数**分开**记（判读方式不同，见 metrics.h 同名字段）：
  //   reassembly_pending_dropped = 因预算被淘汰的帧数（已计入 lost，是它的子集）；
  //   reassembly_rejected        = 因预算拒收的包数（新槽位连一个分片都装不下）。
  // 前者是接收端自保（链路可能完全正常、只是有人灌碎片），后者是病态单帧；
  // 与协议层的 `rejected`（坏包/越界/几何矛盾）也分开——那是"包本身不合法"。
  uint64_t reassembly_pending_dropped = 0;
  uint64_t reassembly_rejected = 0;

  // 按 first_seen_us 从最旧开始淘汰 want 帧，结算口径与 cleanup_stale **完全一致**：
  // ++lost、settle()（丢包率窗口的分子/分母都要算上它）、settle_fec()（FEC 组的账）。
  // 为什么必须结算而不是直接 erase：被预算挤掉的一帧同样"没完成"，如果不入账，
  // 内存是有界了，但 lost 与 win.expected 会同时少一帧——丢包率被低估、报告上的丢包
  // 看起来比实际好。指标不能因为护栏而开始撒谎，所以两处结算必须同一口径。
  // 淘汰顺序依据 first_seen_us 而**不是** frame_id：后者是 uint32_t 会回绕，回绕点前后
  // 大小关系翻转，按它取最小会先淘汰刚建的新帧（攻击者把帧号推到回绕点即可）。
  // keep：不淘汰这一帧（正在填的槽位，调用方还持有它的引用，删了就是 use-after-free）。
  auto evict_oldest = [&](std::size_t want, std::optional<uint32_t> keep) -> std::size_t {
    std::vector<std::pair<int64_t, uint32_t>> order;
    order.reserve(pending.size());
    for (const auto& [fid, fa] : pending) {
      if (keep && fid == *keep) continue;
      order.emplace_back(fa.first_seen_us, fid);
    }
    if (order.empty() || want == 0) return 0;
    want = std::min(want, order.size());
    // 定时器精度是微秒、而同一微秒内建的两个槽位会有相同的 first_seen_us：
    // std::pair 的字典序把 fid 当次级键，结果因此是确定的（不依赖 map 的遍历顺序）。
    std::sort(order.begin(), order.end());
    std::size_t evicted = 0;
    for (std::size_t i = 0; i < want; ++i) {
      const auto it = pending.find(order[i].second);
      if (it == pending.end()) continue;
      ++lost_frames;  // 与 cleanup_stale 同一口径：没完成的帧计丢失
      settle(win, it->second);
      settle_fec(it->second, fec_totals);
      pending_bytes -= it->second.bytes;
      pending.erase(it);
      ++reassembly_pending_dropped;
      ++evicted;
    }
    return evicted;
  };

  // 新建槽位前的预算判据。返回 false = 这一包被拒收（调用方 ++reassembly_rejected 后 continue）。
  // 已有槽位不走这里：那一帧已经在账上了，给它加片走下面的 grow_guard。
  auto reserve_slot = [&](uint32_t fid, std::size_t incoming_bytes) -> bool {
    if (pending.find(fid) != pending.end()) return true;
    const auto dec =
        cue::proto::plan_reassembly_budget(pending.size(), pending_bytes, incoming_bytes);
    if (dec.reject) return false;  // 病态单帧：拒收它，不拿存量队列陪葬
    if (dec.evict != 0) evict_oldest(dec.evict, std::nullopt);
    // 字节维度策略只给得出"至少 1 帧"的下界（它不知道每帧多大），这里继续按最旧优先
    // 逐帧淘汰直到装下。循环必然终止：reject 已挡住 incoming > 上限，队列淘汰空之后
    // 0 + incoming 一定装得下（此时 over 为假）。
    while (cue::proto::reassembly_over_bytes(pending_bytes, incoming_bytes) && !pending.empty()) {
      if (evict_oldest(1, std::nullopt) == 0) break;
    }
    return !cue::proto::reassembly_over_bytes(pending_bytes, incoming_bytes);
  };

  // 给某一帧加片（分片 / FEC 校验包 / FEC 恢复出的片）之后的字节守卫。
  // 光有帧数上限挡不住"少数几帧喂满 1024 片"：256 帧 × 1.2 MiB ≈ 300 MiB > 64 MiB。
  // 淘汰最旧的**其他**帧，绝不淘汰 keep_fid——它的引用还在调用方手里。
  // 收敛性：单帧最多 kMaxFragPerFrame × kMaxUdpPayload ≈ 1.2 MiB，远小于总预算，
  // 所以"只剩 keep_fid 一个槽位"时一定已经不超了。
  auto grow_guard = [&](uint32_t keep_fid) {
    while (pending_bytes > cue::proto::kMaxPendingBytes) {
      if (evict_oldest(1, keep_fid) == 0) break;
    }
  };

  // ---- feedback（技术设计 §4.2）：每 200 ms 一条，载荷 4 个 f32 ----
  // RTT 复用时钟对齐的回环请求：除了 30 s 一次的 8 样本批，另外每秒发 1 个单发回环，
  // 让 feedback 里的 rtt_ms 新鲜度 ≤1 s（30 s 批之间的旧值不能冒充"现在的 RTT"）。
  // 这个单发回环**不参与** offset 估计，避免打乱"批内取最小 RTT"的口径。
  int64_t next_fb_us = 0;
  int64_t fb_bytes_prev = 0;
  int64_t fb_t_prev = 0;
  int64_t next_rtt_ping_us = 0;
  int64_t rtt_ping_echo = 0;
  int64_t last_rtt_us = -1;      // -1 = 从未测到（feedback 里写 -1，不写 0）
  int64_t last_rtt_at_us = 0;
  // 帧间抖动：相邻帧「传输时延」差的 EWMA（RFC 3550 的 1/16 平滑）。
  // 帧间隔在相减时抵消，剩下的就是网络/调度抖动——正是抖动缓冲要吸收的量。
  // -1 = 还没对齐时钟，没有可靠值。
  double jitter_us = -1.0;
  int64_t last_transit_us = -1;

  // 记录「数据报到达 → 被读出」的耗时：内核时间戳是 CLOCK_REALTIME，而延迟测量
  // 全用 CLOCK_MONOTONIC，所以在 recv 前后各取一对 (realtime, monotonic) 样本，
  // 用两者的差把到达时刻换到单调时钟域（样本间隔微秒级，误差可忽略）。
  auto recv_timed = [&](uint8_t* buf, std::size_t cap, sockaddr_in& from,
                        int64_t& recv_us_out) -> ssize_t {
#if defined(_WIN32)
    // Windows 没有 SO_TIMESTAMP（内核打戳是 Linux/BSD 的设施），也没有 recvmsg 的
    // 控制消息——拿不到「数据报到达内核的时刻」，recv 段就**如实给 -1（无样本）**，
    // 而不是用"读出的时刻"冒充到达时刻（那会把 recv 段恒算成 0，等于伪造）。
    // 地址长度必须传有效指针：Windows 的 recvfrom 对 nullptr 返回 WSAEFAULT(10014)，
    // 而 POSIX 允许 nullptr。这个差异只在实机跑起来才暴露（编译期完全看不出来）。
    int from_len = static_cast<int>(sizeof(from));
    const int wn = ::recvfrom(udp_fd, reinterpret_cast<char*>(buf),
                              static_cast<int>(cap), 0,
                              reinterpret_cast<sockaddr*>(&from), &from_len);
    // CUE_PROBE_TRACE=1 时打前几条：Windows 上 recvfrom 的失败码（如 WSAEFAULT 10014）
    // 不会体现在任何指标里，只能靠它定位。
    if (const char* tr = std::getenv("CUE_PROBE_TRACE"); tr != nullptr && *tr != '\0') {
      static int n_tr = 0;
      if (n_tr++ < 5) {
        std::fprintf(stderr, "[trace] recvfrom n=%d err=%d sock=%lld\n", wn, ::WSAGetLastError(),
                     static_cast<long long>(udp_fd));
      }
    }
    recv_us_out = -1;
    return static_cast<ssize_t>(wn);
#else
    timespec rt0{};
    ::clock_gettime(CLOCK_REALTIME, &rt0);
    const int64_t mono0 = cue::now_us();
    const int64_t wall0 = int64_t(rt0.tv_sec) * 1'000'000 + rt0.tv_nsec / 1000;

    iovec iov{buf, cap};
    std::array<uint8_t, 256> ctrl{};
    msghdr msg{};
    msg.msg_name = &from;
    msg.msg_namelen = sizeof(from);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl.data();
    msg.msg_controllen = ctrl.size();
    const ssize_t n = ::recvmsg(udp_fd, &msg, 0);
    const int64_t mono1 = cue::now_us();
    if (n <= 0) return n;

    int64_t arrival_us = -1;
    for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
      if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMP) {
        timeval tv{};
        std::memcpy(&tv, CMSG_DATA(c), sizeof(tv));
        arrival_us = int64_t(tv.tv_sec) * 1'000'000 + tv.tv_usec;
        break;
      }
    }
    if (arrival_us < 0) {
      recv_us_out = -1;
      return n;
    }
    const int64_t arrival_mono = arrival_us - (wall0 - mono0);
    recv_us_out = mono1 - arrival_mono;
    if (recv_us_out < 0 || recv_us_out > 1'000'000) recv_us_out = -1;  // 时钟跳变当噪声丢掉
    return n;
#endif
  };

  // 渲染器必须声明在 decode_one **之前**：解码出帧的那一刻就要上屏——AVFrame* 是
  // 借用指针，下次 decode 就失效，攒着延后画只会画出旧帧或垃圾。
  // 渲染器：--render 才打开。打不开不致命（降级成"只算指标"），但要打出原因，
  // 否则"窗口没出现"会被误读成程序没跑。
  cue::probe::Renderer renderer;
  // render_live 才是"渲染真的开着"，render_enabled 只是"用户要求开"。
  // 两者必须分开：Renderer::pump_events() 在没打开时返回 false（语义是"没有窗口可以
  // 处理事件"），拿它当"用户关了窗口"会让 --render 打不开（无 DISPLAY、无 GPU）时
  // probe 立刻退出——而 open() 的约定恰恰是"失败就降级成只算指标"。
  bool render_live = false;
  if (render_enabled) {
    cue::probe::RenderConfig rcfg;
    rcfg.width = render_w;
    rcfg.audio_out = audio_out;
    rcfg.audio_prebuffer_ms = static_cast<int>(audio_prebuffer_ms);
    rcfg.height = render_h;
    std::string rerr;
    if (renderer.open(rcfg, rerr)) {
      render_live = true;
      // 必须在 open 成功**之后**启用：enable_snapshot 依赖 open 建好的 impl_，
      // 放在前面会静默变成空操作（实测就是这样：参数都对、文件一个都没生成）。
      if (snap_ms > 0 && !snap_prefix.empty()) {
        renderer.enable_snapshot(static_cast<int>(snap_ms), snap_prefix);
      }
    } else {
      std::fprintf(stderr, "[probe] --render 打开失败，降级为只算指标：%s\n", rerr.c_str());
      emit("probe.render_failed", json{{"error", rerr}, {"backend", cue::probe::Renderer::backend_name()}});
    }
  }

  // ---------------------------------------------------------------- 呈现延迟队列
  //
  // 音画同步：音频为了吸收"成簇到达"把播放队列垫到 400~600 ms，而视频原来解码完就
  // 立刻上屏（端到端只有 ~250 ms）⇒ 音频比视频晚 200~300 ms，听起来就是不同步。
  // 同步的正确方向不是砍音频缓冲（那会退回周期性咔咔），而是**给视频加同等延迟**。
  //
  // 为什么要克隆：decode() 返回的是借用指针，只在下次 decode 之前有效（见下面的注释），
  // 所以想延后画就必须自己持有一份引用。av_frame_clone 只增引用计数，不拷贝像素。
  struct Deferred { int64_t due_us; AVFrame* frame; };
  std::deque<Deferred> defer_q;
  double av_audio_ewma_ms = 0.0;   // 音频播放队列深度的 EWMA（毫秒）
  double av_video_ewma_ms = 0.0;   // 视频端到端延迟的 EWMA（毫秒）
  bool av_have_video_lat = false;  // 是否已经测到过视频延迟（需要时钟同步）

  auto take_av_delay_us = [&](int64_t now) -> int64_t {
    if (av_delay_ms > 0) return av_delay_ms * 1000;
    if (av_delay_ms == 0) return 0;
    // 自动模式：让**两端的总延迟相等**，而不是让"额外延迟"相等。
    //
    // 视频这边已经自带一段流水线延迟（采集+编码+传输+抖动缓冲，实测端到端约 250 ms），
    // 音频那边毛延迟就等于播放队列深度（实测 400~600 ms）。所以视频还需要补的是
    //     音频队列深度 − 视频已有延迟
    // 而不是直接等于音频队列深度——后者会把视频推到音频**后面**同样的量。
    // 用户的实际观测印证了这点：在 bilibili 里点暂停，画面先停、音频晚约 0.3 秒才停。
    //
    // 两个量都用 EWMA：瞬时值随每个包的进出跳动，会让视频延迟跟着抖，
    // 反而制造出新的、不断变化的唇音偏移。
    const int q = render_live ? renderer.queued_ms() : -1;
    if (q >= 0) av_audio_ewma_ms = av_audio_ewma_ms * 0.9 + double(q) * 0.1;
    double video_lat_ms = 0.0;
    if (lat.size() > 0) {
      video_lat_ms = double(lat.percentile(0.50)) / 1000.0;
      av_video_ewma_ms = av_video_ewma_ms * 0.9 + video_lat_ms * 0.1;
      av_have_video_lat = true;
    }
    (void)now;
    // 还没测出视频延迟（时钟未同步）时退化为"跟随音频队列"，至少方向是对的；
    // 一旦有了实测延迟就改用差值——两者相差正好一个流水线延迟。
    const double extra = av_have_video_lat ? (av_audio_ewma_ms - av_video_ewma_ms)
                                           : av_audio_ewma_ms;
    return static_cast<int64_t>((extra > 0.0 ? extra : 0.0) * 1000.0);
  };

  auto defer_or_draw = [&](AVFrame* out) {
    const int64_t now = cue::now_us();
    const int64_t delay_us = take_av_delay_us(now);
    if (delay_us <= 0) {
      renderer.draw_yuv(out);
      return;
    }
    AVFrame* clone = av_frame_clone(out);
    if (clone == nullptr) {  // 克隆失败就退化成立即上屏，绝不丢这一帧
      renderer.draw_yuv(out);
      return;
    }
    defer_q.push_back(Deferred{now + delay_us, clone});
    // 上限：延迟越大堆得越多，但必须有界。按 30 fps 留 2 倍余量即可。
    const std::size_t cap = static_cast<std::size_t>(delay_us / 33'333) * 2 + 8;
    while (defer_q.size() > cap) {
      av_frame_free(&defer_q.front().frame);
      defer_q.pop_front();
    }
  };

  // 每轮把到期的帧画出来。只画最后一帧到期的：中间那些已经过期，
  // 画出来只会变成"连着闪几下"，与"画面在某个时刻的样子"不符。
  auto drain_defer = [&](int64_t now) {
    if (defer_q.empty()) return;
    std::size_t drawn = 0;
    while (!defer_q.empty() && defer_q.front().due_us <= now) {
      AVFrame* f = defer_q.front().frame;
      defer_q.pop_front();
      if (!defer_q.empty() && defer_q.front().due_us <= now) {
        av_frame_free(&f);  // 已过期且后面还有更新的，直接丢
        continue;
      }
      renderer.draw_yuv(f);
      av_frame_free(&f);
      ++drawn;
    }
    (void)drawn;
  };

  // 一帧解码：补参数集 → 喂解码器 → 记耗时与失败计数。
  // 只借用 FrameAcc 的三个元信息（pts / codec / frag_count），不拷整帧分片：
  // 每帧都拷一遍分片向量，在 720p 关键帧上就是每帧几百 KB 的无谓分配。
  auto decode_one = [&](uint8_t codec, uint16_t frag_count,
                        std::span<const uint8_t> payload) {
    if (payload.empty()) return;
    if (codec != static_cast<uint8_t>(cue::proto::Codec::H264)) {
      // 只解 H264：别的 codec 的字节流喂进 H264 解码器只会产出错误帧，
      // 把「解不出来」算成解码错误更是误导（那不是故障，是这里不解这种码）。
      ++fs.decode_skips;
      auto& seen = dec.skipped_codecs[codec];
      if (seen++ == 0) {
        // 每个 codec 值只记一条：30 fps 的非 H264 流否则会刷满事件缓存
        emit("probe.decode_skipped",
             json{{"codec", static_cast<int>(codec)},
                  {"skipped_frames", fs.decode_skips},
                  {"bytes", payload.size()},
                  {"frag_count", frag_count}});
      }
      return;
    }
    if (!dec.ensure_open()) {
      ++fs.decode_errors;
      return;
    }
    bool inline_sps = false;
    bool inline_pps = false;
    const bool annexb = scan_param_sets(payload, dec.params, inline_sps, inline_pps);
    if (!annexb) {
      // 没有起始码就不是 Annex-B（可能是 AVCC 长度前缀形态）：这里不做转封装，
      // 直接记错并限流提示一次，不假装解过。
      ++fs.decode_errors;
      if (!warned_bad_frame) {
        std::fprintf(stderr, "[probe] 帧缺少 Annex-B 起始码，无法解码（只提示一次）\n");
        warned_bad_frame = true;
        ++bad_frame_events;
        emit("probe.decode_unsupported",
             json{{"reason", "no_annexb_start_code"}, {"bytes", payload.size()}});
      }
      return;
    }
    std::span<const uint8_t> feed = payload;
    if (!inline_sps && !inline_pps && !dec.params_ready && dec.params.complete()) {
      // 参数集在别的包里先到过：补在帧前，否则解码器会一直报「缺 SPS」
      dec.scratch.clear();
      dec.scratch.insert(dec.scratch.end(), dec.params.sps.begin(), dec.params.sps.end());
      dec.scratch.insert(dec.scratch.end(), dec.params.pps.begin(), dec.params.pps.end());
      dec.scratch.insert(dec.scratch.end(), payload.begin(), payload.end());
      feed = {dec.scratch.data(), dec.scratch.size()};
    }
    const int64_t t0 = cue::now_us();
    std::string derr;
    AVFrame* out = dec.dec.decode(feed, derr);
    const int64_t dec_us = cue::now_us() - t0;
    fs.decode_us.push(std::max<int64_t>(0, dec_us), cue::now_us());
    if (out != nullptr) {
      ++fs.decoded;
      dec.params_ready = true;
      dec.fail_streak = 0;
      // 借用指针只在下次 decode 之前有效，所以这里必须立刻上屏（不能存起来延后画）；
      // 用 render_live 而不是 render_enabled：没打开时 draw_yuv 本来就是空操作，
      // 分开只是为了读代码的人不必去翻实现确认这一条。
      if (render_live) defer_or_draw(out);
    } else if (!derr.empty()) {
      ++fs.decode_errors;
      ++dec.fail_streak;
      if (dec.fail_streak >= kMaxDecodeFailStreak) {
        // 连续失败到阈值：重置解码器（重新 open = 丢掉全部参考帧），并留一条事件，
        // 否则「一直解不出来」在指标里会和「一直没收到帧」长得一模一样。
        emit("probe.decode_reset",
             json{{"fail_streak", dec.fail_streak},
                  {"decode_errors", fs.decode_errors},
                  {"resets", dec.resets + 1},
                  {"last_error", derr}});
        dec.reset();
      }
    } else {
      // 第三个分支：decode 返回 nullptr 且**没有**错误信息 = 包被解码器吃了，只是这一拍
      // 还不产帧（avcodec_receive_frame 回 EAGAIN）。旧实现没有这一支，于是这类帧
      // 既不算 decoded 也不算 error，在账本上凭空消失——"frames 涨、decoded 不涨、
      // errors 恒 0"正是它的指纹。它不是故障，但也绝不是"没收到"，必须单独记。
      ++fs.decode_no_output;
    }
  };

  // 一帧从抖动缓冲放行之后的处理：采样端到端/缓冲段 → 解码 → 卡顿判定。
  // 端到端延迟的口径在这里，而不是在"重组完成"处：帧真正可交付的时刻才是链路延迟，
  // 接收端自己的抖动缓冲等待必须算进去。否则 buffer_us 涨了而 lat 不动，
  // 两段数字会互相矛盾，报告上看起来像抖动缓冲白等了。
  auto deliver = [&](const cue::media::JitterFrame& jf) {
    const int64_t now_rel = cue::now_us();
    const int64_t lat_us = now_rel + clk.offset_us - static_cast<int64_t>(jf.pts_us);
    if (clk.have && lat_plausible(lat_us)) {
      lat.push(lat_us, cue::now_us());
      // 抖动：相邻帧传输时延之差的 EWMA。帧间隔在相减中抵消，剩下的才是网络/调度抖动。
      // 用单调时钟 + offset，与端到端延迟同一口径；时钟未对齐时保持 -1（不编数）。
      if (last_transit_us >= 0) {
        const double d = std::fabs(double(lat_us - last_transit_us));
        jitter_us = jitter_us < 0.0 ? d : jitter_us + (d - jitter_us) / 16.0;
      }
      last_transit_us = lat_us;
    } else {
      ++lat_invalid;
      if (clk.have && !warned_clock) {
        std::fprintf(stderr,
                     "[probe] 延迟样本落在时钟域外（%lld us）：offset 估计可能失效，"
                     "延迟分位将被跳过。\n",
                     static_cast<long long>(lat_us));
        warned_clock = true;
      }
    }
    // 抖动缓冲段：进队 → 放行。这是 buffer_us 的唯一来源，不再是恒 null。
    fs.buffer_us.push(std::max<int64_t>(0, jf.wait_us), cue::now_us());
    decode_one(jf.codec, jf.frag_count, {jf.payload.data(), jf.payload.size()});
    // 卡顿按「帧与帧」的间隔算：与这一帧能否解出来无关（解不出来的帧同样占时间轴），
    // 所以放在解码之后统一观察，而不是塞进解码分支（跳过/失败会漏掉间隔）。
    fs.observe_gap(jf.pts_us, now_rel, stall_threshold_us);
  };

  // 抖动缓冲跳帧之后，参考链已经断了，但**解码层看不见这件事**：它收到的永远是
  // 连续的字节流，缺口只存在于抖动缓冲的 PTS 时间轴上。H.264 的间预测要用前序帧，
  // 缺一帧之后解出来的画面上每一帧都是错的（错的是**像素**，不是"少一帧"），
  // 而且这种错会一直挂到下一个 IDR 为止。H264Decoder::flush() 就是为此准备的入口。
  //
  // 什么算"跳帧"：jb 的 skipped（PTS 空洞 + 容量挤掉）和 late_dropped（入队即过期、
  // 根本无法按序输出）。后者在 jb 的计数口径里不进 skipped，但它同样意味着有一帧
  // 没进解码器，参考链一样断——所以两个计数器都要看，只看 skipped 会漏。
  //
  // 取舍（这里是明确的选择，不是"顺手"）：flush 之后画面会**冻**到下一个 IDR
  // （GOP=30 时 ≤1 s），而不是继续显示错误画面。
  //   · 选冻屏：错画面会让人去怀疑编码器/显卡/渲染器，而真相只是丢了一帧；
  //     冻屏只会让人怀疑丢包——而这时候确实就是丢包。且冻屏是自愈的（下个 IDR 即恢复）。
  //   · 不选"解错帧继续显示"：残影/花屏会持续整整一个 GOP，比冻 1 s 更难解释。
  //   · 条件放宽的唯一合理入口是"按需请求 IDR"（现在协议里没有），或者 GOP 缩短到
  //     一两帧——那时冻屏窗口小到无感，这条策略自然就无害了。
  // 已经在等 IDR 时不重复 flush：那时没有任何参考帧可丢，再调只会白记一次 flushes。
  auto note_refchain_break = [&](uint64_t skipped_before, uint64_t late_before) {
    const cue::media::JitterBufferStats js = jb.stats();
    const uint64_t ds = js.skipped > skipped_before ? js.skipped - skipped_before : 0;
    const uint64_t dl = js.late_dropped > late_before ? js.late_dropped - late_before : 0;
    const uint64_t delta = ds + dl;  // 两个计数器都不会回退；jb.reset 会清零，所以取差值前先挡掉
    if (delta == 0) return;
    // 这里**只报告，不修**。曾经试过在这一步 dec.dec.flush()（"宁可冻屏也不显示错画面"），
    // 实测代价不可接受：1080p/veryfast 下采集端编码吃满帧预算，30 秒内跳帧 69 次；
    // 每次 flush 都让解码器进入「等 IDR」，而 GOP=30 意味着最多等 1 秒 ——
    // 结果 30 秒里 flush 27 次、decode_gated 涨 627 帧，画面基本处于持续冻屏。
    // 把「偶发花屏」换成「持续冻屏」是净损失。
    //
    // 更根本的理由：检测工具的职责是留下证据，不是替被测链路做修复决策
    // （同一条理由也让我们去掉了渲染的 vsync）。真要在接收端修，正确位置是
    // 能主动请求 IDR —— 对应 RTP 的 PLI/FIR，现在下行协议里还没有这个类型。
    ++refchain_breaks;
    // 关键帧请求**只在真有丢包时**发起，不能只看"抖了一下"。
    //
    // 实测代价：OBS 源下 jb_skipped 每秒涨 2~4 帧，若据此请求 IDR，20 秒里就发出
    // 27 次（1.35 次/秒）。而 IDR 体积是 P 帧的 10~20 倍，等于持续往链路上插码率尖峰：
    // 撞上 VBV 之后后续 P 帧拿不到码率，正好在"视角大幅转动"这种最需要码率的时刻
    // 变成色块。与此同时包到达侧是干净的（pkt_arr_gap_max_ms = 0、jb_late = 0），
    // 说明这些 skipped 根本不是丢包造成的——为不存在的丢包付 IDR 的代价，是净亏。
    //
    // 判据改成"有真实损伤证据"：帧号空洞（late_dropped）或解码报错增长了才要。
    // 单纯 skipped（发送端 PTS 轴上的洞、采集端出帧不匀）不应该触发请求。
    if (dl > 0 || fs.decode_errors > kf_err_baseline) {
      kf_err_baseline = fs.decode_errors;
      kf_needed = true;
    }
    emit("probe.refchain_break",
         json{{"reason", "jitter_skip"},
              {"missing_delta", delta},
              {"skipped_total", js.skipped},
              {"late_total", js.late_dropped},
              {"breaks", refchain_breaks}});
  };


  // 抖动缓冲的时间驱动：主循环顶部、每个数据报之后、以及 poll 超时都走这里，
  // 保证"等待预算"是按时间生效的，而不是按"下一个包什么时候来"生效。
  auto drain_jb = [&](int64_t now) {
    const cue::media::JitterBufferStats before = jb.stats();
    auto ready = jb.poll(now);
    // 必须在 deliver 之前判定：本批里的帧全都是空洞之后的帧，先 flush 才能让它们
    // 走"等 IDR"的分支，而不是被当成连续的下一帧解出一张错画面。
    note_refchain_break(before.skipped, before.late_dropped);
    for (const auto& jf : ready) deliver(jf);
  };

  // 单包探测的判据：被探测的那一片真的回来了（原本到达或 FEC 恢复都算）。
  // 只有这一片被补回才算成功——不能拿"任意帧完成"冒充探测成功。
  auto mark_probe_if_recovered = [&](uint32_t fid) {
    if (!nack_probe_pending || fid != nack_probe_fid) return;
    const auto it = pending.find(fid);
    if (it == pending.end()) return;
    if (nack_probe_frag < it->second.parts.size() &&
        !it->second.parts[nack_probe_frag].empty()) {
      nack_probe_pending = false;
      nack_ctl.on_probe_success();
      emit("probe.nack_resume",
           json{{"frame_id", fid},
                {"frag_index", nack_probe_frag},
                {"reason", "recovered"},
                {"nack_sent", nack_ctl.sent()}});
    }
  };

  // 帧完成结算：所有分片到齐就计入帧数/重组分位/丢包窗口，然后**进抖动缓冲**。
  // 「收到多少帧」在这里结算（与旧口径一致）；「什么时候能交付」由抖动缓冲决定。
  auto finish_if_complete = [&](uint32_t fid, FrameAcc& fa) {
    for (const auto& pp : fa.parts) {
      if (pp.empty()) return false;
    }
    // 探测目标帧完整了 = 探测成功（可能在 FEC 包里被补齐，不一定经由数据包到达）。
    if (nack_probe_pending && fid == nack_probe_fid) {
      nack_probe_pending = false;
      nack_ctl.on_probe_success();
      emit("probe.nack_resume",
           json{{"frame_id", fid},
                {"frag_index", nack_probe_frag},
                {"reason", "recovered"},
                {"nack_sent", nack_ctl.sent()}});
    }
    const int64_t now_done = cue::now_us();
    ++frames;
    // 重组段：首片到达 → 整帧完成。这是 probe 自己能测的"重组开销"。
    fs.reassembly_us.push(std::max<int64_t>(0, now_done - fa.first_seen_us), cue::now_us());
    if (!have_completed || fid > max_completed) max_completed = fid;
    have_completed = true;
    settle(win, fa);
    settle_fec(fa, fec_totals);  // 完成的帧：组都齐了，只会累加 groups_rx
    cue::media::JitterFrame jf;
    jf.pts_us = fa.pts_us;
    jf.frame_id = fid;
    jf.frag_count = fa.count;
    jf.codec = fa.codec;
    // 抖动基准（见 JitterBasis）：首片到达时刻，不是 now_done。
    // now_done 含"整帧在途耗时 ≈ 帧字节数/带宽"，那是吞吐伪影；IDR 帧是 P 帧的十几倍大，
    // 用 now_done 会让每个 GOP 都把抖动估计顶穿。reassembly 段已经把这段单独度量了。
    jf.first_arrival_us = fa.first_seen_us;
    jf.payload = assemble(fa);
    pending_bytes -= fa.bytes;  // 槽位连同它占的字节一起出账（预算口径见"重组表内存护栏"段）
    pending.erase(fid);
    // 到达间隔必须在 push **之前**采样，而且用同一个 now_done：
    //  · push 之后采样的前提是这一帧没被 late_dropped 掉，而迟到帧同样是"到达"，
    //    漏掉它会把一次真实的到达从分布里抹掉，空档被人为拉长；
    //  · now_done 就是 jb.push 的 arrival_us，指标与缓冲必须同一条时间轴（理由见 ArrivalGapMeter）。
    arrival_gaps.observe(now_done);
    const cue::media::JitterBufferStats before_push = jb.stats();
    auto ready = jb.push(std::move(jf), now_done);
    note_refchain_break(before_push.skipped, before_push.late_dropped);
    for (const auto& rel : ready) deliver(rel);
    return true;
  };

  // SPEC §7.1 的时钟对齐：向 cued 发下行回环请求（载荷 = 我方发戳）
  auto send_udp = [&](const std::vector<uint8_t>& pkt) {
    if (!have_peer) return false;
    return ::sendto(udp_fd, reinterpret_cast<const char*>(pkt.data()), static_cast<int>(pkt.size()), 0,
                    reinterpret_cast<const sockaddr*>(&peer),
                    sizeof(peer)) == static_cast<ssize_t>(pkt.size());
  };
  // 请求一个关键帧（对应 RTCP 的 PLI）。
  //
  // 触发者只有丢帧：H.264 的 P/B 帧依赖前一帧，抖动缓冲跳过一帧之后参考链就断了，
  // 而解码器**看不出**这件事（它只收到一串连续的 Annex-B 字节，没有缺口标记）——
  // 所以"要不要请求"必须由这一层判断，这也正是它放在 probe 而不是解码器里的原因。
  //
  // 冷却 1500 ms：IDR 是 P 帧的 10~20 倍大，请求太密本身就是码率尖峰的来源
  // （实测 300 ms 冷却时达到 1.35 次/秒，反而放大了色块）。
  // 300 ms 比 cued→采集端那个 500 ms 冷却短一档，让"连续两次独立故障"仍然能各要一次。
  int64_t last_kf_req_us = 0;
  auto request_keyframe = [&](int64_t now, const char* reason) {
    if (!have_peer) return;
    if (kf_req_sent > 0 && now - last_kf_req_us < 1'500'000) {
      ++kf_req_suppressed;
      return;
    }
    last_kf_req_us = now;
    cue::proto::DatagramHeader h;
    h.type = static_cast<uint8_t>(cue::proto::DownType::KeyframeReq);
    h.session_id = session_seen;
    // 空载荷：只表达"我需要一个能重新起步的点"，怎么给由发送端决定
    const auto hdr = cue::proto::encode(h);
    std::vector<uint8_t> pkt(hdr.begin(), hdr.end());
    if (send_udp(pkt)) {
      ++kf_req_sent;
      emit("probe.keyframe_request", json{{"reason", reason}, {"total", kf_req_sent}});
    }
  };
  auto send_stats_request = [&](int64_t now) {
    if (!have_peer) return;
    cue::proto::DatagramHeader h;
    h.type = static_cast<uint8_t>(cue::proto::DownType::StatsReq);
    h.session_id = session_seen;
    const auto hdr = cue::proto::encode(h);
    std::vector<uint8_t> pkt(hdr.begin(), hdr.end());
    const std::size_t base = pkt.size();
    pkt.resize(base + 8, 0);
    cue::proto::detail::put_u64(pkt.data() + base, static_cast<uint64_t>(now));
    send_udp(pkt);
    ++stats_reqs;
  };

  // feedback（技术设计 §4.2）：每 200 ms 一条 type=0x06。
  // recv_bitrate_mbps 是「自从上一条 feedback 以来」的接收速率（200 ms 窗口），不是进程
  // 平均值——AIMD 要的是**当前**链路状况。窗口太短（<50 ms，例如刚刚对端切换）时写 -1。
  auto send_feedback = [&](int64_t now) {
    if (!have_peer) return;
    const double dt = fb_t_prev > 0 ? double(now - fb_t_prev) / 1e6 : 0.0;
    double mbps = -1.0;
    if (dt >= 0.05) {
      mbps = double(bytes - static_cast<uint64_t>(fb_bytes_prev)) * 8.0 / dt / 1e6;
    }
    fb_bytes_prev = static_cast<int64_t>(bytes);
    fb_t_prev = now;
    const double jitter_ms = jitter_us >= 0.0 ? jitter_us / 1000.0 : -1.0;
    // RTT 只在 **1 s 内** 有样本时才报：30 s 批之间隔得太久，旧值不能冒充"现在的 RTT"。
    const double rtt_ms = (last_rtt_us >= 0 && last_rtt_at_us > 0 &&
                           now - last_rtt_at_us <= 1'000'000)
                              ? double(last_rtt_us) / 1000.0
                              : -1.0;
    cue::proto::FeedbackPayload f;
    f.loss_rate = static_cast<float>(win_loss);
    f.recv_bitrate_mbps = static_cast<float>(mbps);
    f.jitter_ms = static_cast<float>(jitter_ms);
    f.rtt_ms = static_cast<float>(rtt_ms);
    cue::proto::DatagramHeader h;
    h.type = static_cast<uint8_t>(cue::proto::DownType::Feedback);
    h.session_id = session_seen;
    const auto hdr = cue::proto::encode(h);
    std::vector<uint8_t> pkt(hdr.begin(), hdr.end());
    const auto body = cue::proto::encode(f);
    pkt.insert(pkt.end(), body.begin(), body.end());
    send_udp(pkt);
    ++fb_sent;
  };

  // ---- 下行音频（FR-DEV.6 的另一半）----
  // cued 现在把上行 Opus 原样透传到下行（DownType::Audio），所以 probe 有了真的音频流可解。
  // 与视频共用一帧的 4096 字节栈缓冲无关：20 ms 的 Opus 包最多 1275 字节。
  cue::media::AudioReceiver audio_receiver(audio_fec_enabled, audio_plc_enabled);
  const auto& audio_stats = audio_receiver.stats();
  const auto& audio_decoded_frames = audio_stats.decoded_frames;
  const auto& audio_pcm_frames = audio_stats.pcm_samples;
  const auto& audio_decode_errors = audio_stats.decode_errors;
  const auto& audio_fec_recovered = audio_stats.fec_recovered;
  const auto& audio_plc_frames = audio_stats.plc_frames;
  const auto& audio_unconcealed_frames = audio_stats.unconcealed;
  const auto& audio_first_error = audio_receiver.first_error();
  cue::media::AudioRmsWindow audio_rms;
  const cue::media::AudioReceiver::Emit emit_audio =
      [&](uint32_t, cue::media::AudioOutputKind kind, std::span<const int16_t> pcm) {
        // PLC is extrapolation: queue it, but never call its energy a received signal.
        if (kind != cue::media::AudioOutputKind::Plc) audio_rms.push(pcm.data(), pcm.size());
        if (render_live) renderer.queue_audio(pcm.data(), static_cast<int>(pcm.size()));
      };

  std::array<uint8_t, 4096> buf{};

  // ---- NAT 注册（见 --hello-to）----
  // hello_fd 与收下行用同一个平台 socket；invalid sentinel 表示未配置。
  // 必须复用同一个 socket：换一个端口发出，NAT 会把映射建立在另一个外部端口上，
  // 服务器按那个地址回包时我们这边收不到（映射与 socket 是一对一的）。
  probe_socket_t hello_fd = kProbeInvalidSocket;
  sockaddr_in hello_addr{};
  int64_t last_hello_us = 0;
  if (!hello_host.empty() && hello_port != 0) {
    hello_addr.sin_family = AF_INET;
    hello_addr.sin_port = htons(hello_port);
    if (::inet_pton(AF_INET, hello_host.c_str(), &hello_addr.sin_addr) == 1) {
      hello_fd = udp_fd;
      std::fprintf(stderr, "[probe] NAT 注册：每 500 ms 向 %s:%u 发 HELLO\n", hello_host.c_str(),
                   static_cast<unsigned>(hello_port));
    } else {
      std::fprintf(stderr, "[probe] --hello-to 解析失败：%s\n", hello_host.c_str());
    }
  }

  for (;;) {
    // 窗口事件在循环顶部 pump：poll 的超时是 200 ms，所以最坏 200 ms 响应一次关闭/ESC。
    // 用户关了窗口就退出——留着一个看不见的进程继续收流只会让人以为"程序卡住了"。
    if (render_live && !renderer.pump_events()) {
      std::fprintf(stderr, "[probe] 窗口已关闭，退出\n");
      return 0;
    }
    // ---- 周期任务放在循环顶部：FEC 分支 / 完全没包时都不会被跳过 ----
    const int64_t now_tick = cue::now_us();
    // 抖动缓冲的到期帧先放行：后面的周期任务（清理/窗口/指标行）这一拍就该看到它们，
    // 否则 buffer_us 的采样与 jb_depth 会慢一拍。
    drain_jb(now_tick);
    drain_defer(now_tick);
    audio_receiver.advance(now_tick, emit_audio);
    // 关键帧请求必须在**每轮**都检查。一开始它被放在下面那个 stats 批量分支里，
    // 而那一支的间隔会退化到 30 秒——实测 52 次参考链断裂只发出了 1 次请求。
    // 限流由 request_keyframe 自己的 300 ms 冷却负责，这里不需要额外节拍。
    if (kf_needed) {
      kf_needed = false;
      request_keyframe(now_tick, "jitter_skip");
    }
    if (now_tick >= next_stats_us) {
      if (stats_burst_left == kStatsBurst) clk.begin_batch();  // 批首：批内取 RTT 最小者
      send_stats_request(now_tick);
      --stats_burst_left;
      // 批内 20 ms 一发；批发完回到 30 s 周期的重估（SPEC §7.1）
      next_stats_us = stats_burst_left > 0 ? now_tick + kStatsBurstGapUs
                                           : now_tick + 30'000'000;
    }
    // 1 Hz 单发回环：只为让 feedback 里的 rtt_ms 保持新鲜（≤1 s 前）。
    // echo 用 now+1 µs，与同一拍可能发出的对齐批请求区分开（两个请求不共用回显值，
    // 否则这一拍的回包会被当成 RTT 探测而漏喂一次 offset 估计）。
    if (now_tick >= next_rtt_ping_us && have_peer) {
      const int64_t echo = now_tick + 1;
      send_stats_request(echo);
      rtt_ping_echo = echo;
      next_rtt_ping_us = now_tick + 1'000'000;
    }
    if (now_tick >= next_fb_us && have_peer) {
      send_feedback(now_tick);
      next_fb_us = now_tick + 200'000;
    }
    if (!reporters.empty()) {
      // 连接推进放在周期任务里：pump 可能为跨机握手等上 200 ms，
      // 解码循环里做这件事会把收包节奏带歪。
      for (const auto& peer_addr : reporters.pump(now_tick)) {
        emit("probe.report_connected", json{{"peer", peer_addr}});
      }
    }
    if (now_tick - t_last_cleanup >= 500'000) {
      cleanup_stale(pending, now_tick, lost_frames, win, fec_totals, pending_bytes);
      t_last_cleanup = now_tick;
    }
    if (now_tick - t_last_window >= 1'000'000) {
      win_loss = win.expected ? double(win.expected - win.received) / double(win.expected) : 0.0;
      // NACK 策略的窗口：3 个连续 >10% 的窗口 → 暂停 + 冷却 1 s。
      // 冷却中 observe_loss_window 不会重复累计坏窗口（否则永远出不来）。
      // --nack off 时完全不推进策略：否则指标里会出现 paused/throttled，而实际上
      // 一条 NACK 都没打算发——那会把"关了 NACK"读成"被限速了"。
      if (nack_enabled) nack_ctl.observe_loss_window(win.expected, win.received, now_tick);
      win = LossWindow{};
      t_last_window = now_tick;
    }
    if (now_tick - t_last_report >= 1'000'000) {
      // 先快照本窗口的 NACK 状态，再清零窗口计数：
      //  - 清零必须发生在指标行这一拍，nack_sent 的每行增量才必然 ≤8（令牌桶只管
      //    长期平均，窗口边界上可能凑出 9）；
      //  - 但 mode 要按**刚结束的这一窗**来报：先清零再读，suppressed>0 的窗口会被
      //    报成 normal（"这一秒被限速了"看不出来）。
      const auto nack_mode = nack_ctl.mode(now_tick);
      const uint64_t nack_sent_now = nack_ctl.sent();
      const uint64_t nack_sup_now = nack_ctl.suppressed();
      const uint64_t nack_pm_now = nack_ctl.paused_ms();
      const uint64_t nack_pauses_now = nack_ctl.pauses();
      nack_ctl.begin_window(now_tick);
      // 渲染/播放侧的读数集中取一次：metrics 是纯读取，不该在 json 字面量里塞
      // 带副作用或可能失败的调用（写出来也不好看懂是"取值"还是"做事"）。
      const int audio_queue_ms_now = render_live ? renderer.queued_ms() : -1;
      // 1 Hz 的**瞬时**排队深度对音频没有意义：20 ms 一个包，采样点极大比例落在
      // 两包之间的空档上，读数长期是 0 —— 实测就是这样（中位 0、而 audio_starved 为 0、
      // dropped_drift 却有 64，三者互相矛盾）。保留瞬时值（aq_now）供排查，
      // 另给窗口分位（aq/edge 两个字段），判断"有没有余量"只看后者。
      static std::deque<int> aq_hist;
      if (audio_queue_ms_now >= 0) {
        aq_hist.push_back(audio_queue_ms_now);
        if (aq_hist.size() > 60) aq_hist.pop_front();  // 约 60 秒窗口
      }
      std::vector<int> aq_sorted(aq_hist.begin(), aq_hist.end());
      std::sort(aq_sorted.begin(), aq_sorted.end());
      auto aq_pct = [&](double p) -> int {
        if (aq_sorted.empty()) return -1;
        std::size_t i = static_cast<std::size_t>(p * double(aq_sorted.size() - 1));
        return aq_sorted[std::min(i, aq_sorted.size() - 1)];
      };
      const int audio_queue_ms = audio_queue_ms_now;
      const cue::probe::Renderer::AudioStats rstats = renderer.audio_stats();
      const double dt = double(now_tick - t_last_report) / 1e6;
      const uint64_t df = frames - frames_prev;
      const uint64_t dd = fs.decoded - decoded_prev;
      const double fps_rx = double(df) / dt;
      const double fps_dec = double(dd) / dt;
      // 重组表占预算的比例（人读行的 rpend=N/M%）。分母是字节上限 kMaxPendingBytes：
      // 正常链路长期在 1% 以下，持续上涨才是"有人在每帧塞碎片"的信号。
      // pend（槽位数）与 rpend（字节占用）分开看：碎片攻击先顶满 pend、放大攻击先顶满 rpend。
      const int rpend_pct = static_cast<int>(
          100.0 * double(pending_bytes) / double(cue::proto::kMaxPendingBytes));
      std::printf(
          "[probe] session=%u fps=%.1f frames=%llu lost=%llu pend=%zu pkts=%llu nack=%llu "
          "sup=%llu pm=%llums mode=%s pause=%llu fec_rec=%llu fec_unrec=%llu "
          "ghost=%llu/%llu rej=%llu rpend=%llu/%d%% loss=%.1f%% MB=%.2f fb=%llu  ",
          session_seen, fps_rx, static_cast<unsigned long long>(frames),
          static_cast<unsigned long long>(lost_frames), pending.size(),
          static_cast<unsigned long long>(packets),
          static_cast<unsigned long long>(nack_sent_now),
          static_cast<unsigned long long>(nack_sup_now),
          static_cast<unsigned long long>(nack_pm_now),
          cue::proto::NackController::mode_name(nack_mode),
          static_cast<unsigned long long>(nack_pauses_now),
          static_cast<unsigned long long>(fec_recovered),
          static_cast<unsigned long long>(fec_totals.unrecoverable),
          // ghost=fec/frag：人读行里同样分开写，免得人工核对时把两个源混成一个数
          static_cast<unsigned long long>(fec_ghost_discarded),
          static_cast<unsigned long long>(frag_ghost_discarded),
          static_cast<unsigned long long>(rejected),
          static_cast<unsigned long long>(pending_bytes), rpend_pct,
          win_loss * 100.0, double(bytes) / (1024.0 * 1024.0),
          static_cast<unsigned long long>(fb_sent));
      std::printf(
          "dec=%.1ffps dec=%llu dec_err=%llu skip=%llu stall=%llu max_stall=%.1fms ",
          fps_dec, static_cast<unsigned long long>(fs.decoded),
          static_cast<unsigned long long>(fs.decode_errors),
          static_cast<unsigned long long>(fs.decode_skips),
          static_cast<unsigned long long>(fs.stalls),
          double(fs.max_stall_us) / 1000.0);
      std::printf("jb depth=%zu lvl=%zu tgt=%zu sw=%llu skip=%llu late=%llu ",
                  jb.stats().depth, jb.stats().level, jb.stats().target_frames,
                  static_cast<unsigned long long>(jb.stats().switches),
                  static_cast<unsigned long long>(jb.stats().skipped),
                  static_cast<unsigned long long>(jb.stats().late_dropped));
      // 到达侧另起一组：gap_max 与 fps 一起读才答得了"是不是成簇"——fps=30 且
      // gap_max=400ms 就是成簇，两者都平稳才是真的连续到达（见 ArrivalGapMeter）。
      // pkt_gap_max 是同一度量的纯到达侧版本（内核时间戳），两个一起看才分得清
      // "网络成簇"与"我们自己读得慢"。
      std::printf("arr gap_max=%.1fms >50ms=%llu gap_max_all=%.1fms pkt_gap_max=%.1fms ",
                  double(arrival_gaps.max_gap_us) / 1000.0,
                  static_cast<unsigned long long>(arrival_gaps.gaps),
                  double(arrival_gaps.max_gap_us_total) / 1000.0,
                  double(datagram_gaps.max_gap_us) / 1000.0);
      if (!clk.have) {
        std::printf("lat=unsynced(no clock offset yet, stats_req=%llu)\n",
                    static_cast<unsigned long long>(stats_reqs));
      } else if (lat.size() == 0) {
        std::printf("lat=none(off=%+.2fms invalid=%llu)\n", double(clk.offset_us) / 1000.0,
                    static_cast<unsigned long long>(lat_invalid));
      } else {
        std::printf(
            "lat p50=%.2fms p95=%.2fms p99=%.2fms max=%.2fms (n=%zu off=%+.2fms "
            "drift=%.2fms reanchor=%llu)\n",
            double(lat.percentile(0.50)) / 1000.0, double(lat.percentile(0.95)) / 1000.0,
            double(lat.percentile(0.99)) / 1000.0,
            double(lat.size() ? lat.percentile(1.0) : 0) / 1000.0, lat.size(),
            double(clk.offset_us) / 1000.0, double(clk.last_drift_us) / 1000.0,
            static_cast<unsigned long long>(clk.reanchors));
      }
      std::fflush(stdout);

      // 同一份数字进事件流（harness 的指标回传 / M5 控制台 / 报告用）。
      // 没有样本的段一律写 null：null 是「没测到」，0 是「测到了 0」——buffer 段尤其
      // 不能写 0（cued 的抖动缓冲还没实现，接收端根本没有这一段可测）。
      const bool have_lat = clk.have && lat.size() > 0;
      auto pct_or_null = [&](double v) { return have_lat ? json(v) : json(nullptr); };
      // 到达间隔直方图（桶边界见 ArrivalGapMeter::kEdgesUs）：JSON 数组按桶顺序，
      // sums to arr_gap_samples。写成数组而不是九个扁平字段，是因为消费方要的是分布形状，
      // 九个独立的键会让"画一张直方图"变成九次手工拼装。
      json gap_hist = json::array();
      for (std::size_t i = 0; i < ArrivalGapMeter::kBuckets; ++i) {
        gap_hist.push_back(arrival_gaps.histogram[i]);
      }
      json metric{
          // 这一秒跟的是哪一路会话（服务端分配的会话号）。多设备部署时探针的端口与
          // cued 的 device.registered.probe_port 一一对应，这里再写一次会话号，
          // 两边的映射不必靠人脑记端口（T-22）。
          {"session_id", session_seen},
          {"fps", fps_dec},          // 解码帧率：分母是解码出的帧，不是收包数
          {"fps_rx", fps_rx},        // 收包侧的帧率，仅供对照
          {"frames", frames},
          {"decoded", fs.decoded},
          {"decode_errors", fs.decode_errors},
          {"decode_skips", fs.decode_skips},
          // 喂进解码器、既没出帧也没报错的帧数（见 FrameStats::decode_no_output）。
          // 没有它，「frames − decoded」这部分差额无账可查，只能靠猜。
          {"decode_no_output", fs.decode_no_output},
          {"stalls", fs.stalls},
          {"max_stall_us", fs.max_stall_us},
          {"stall_threshold_us", stall_threshold_us},
          {"lost", lost_frames},
          {"nack", nack_sent_now},
          // NACK 限速/冷却的可观测面（FR-PROBE.2）：被拦下的包数、累计暂停时长、
          // 当前模式。mode 是字符串而不是枚举数字：报告里直接可读，不必查表。
          {"nack_sent", nack_sent_now},
          {"nack_suppressed", nack_sup_now},
          {"nack_paused_ms", nack_pm_now},
          {"nack_pauses", nack_pauses_now},
          {"nack_mode", cue::proto::NackController::mode_name(nack_mode)},
          // FEC（FR-PIPE.9）：recovered = 靠 XOR 补回的片数；unrecoverable = 结算时仍缺
          // ≥2 片的**组数**，unrecoverable_pkts = 这些组一共缺了几片。
          // 恢复率 = fec_recovered / (fec_recovered + fec_unrecoverable_pkts)。
          {"fec_rec", fec_recovered},
          {"fec_recovered", fec_recovered},
          {"fec_groups_rx", fec_totals.groups_rx},
          {"fec_unrecoverable", fec_totals.unrecoverable},
          {"fec_unrecoverable_pkts", fec_totals.unrecoverable_pkts},
          // 幽灵帧守卫的计数（N-02）：被丢弃的"为已完成帧重建槽位"次数。
          // 两者口径不同，读法也不同（实测见变量定义处）：
          //   fec_ghost_discarded  = **常态**：FEC 校验包总在整帧之后到，零丢包实测
          //     8 s / 241 帧 → 250 次（约每帧 1 次），与完成帧数成正比、与丢包无关
          //     （5% 丢包反而降到 187~190，因为完成帧少了）。它只能当"守卫在跑"的
          //     读数，**不是故障指示**，不能拿来证明幽灵帧发生过。
          //   frag_ghost_discarded = **异常**：迟到分片/重传去重建已完成帧。
          //     零丢包实测 0、5% 丢包 2~3——这个才是幽灵帧的真证据。
          {"fec_ghost_discarded", fec_ghost_discarded},
          {"frag_ghost_discarded", frag_ghost_discarded},
          // 重组表的内存护栏（N-10 / FR-PIPE.14）：pending 此前唯一的数量上限是单帧
          // kMaxFragPerFrame=1024，**帧数**无界（每帧发 1 片、帧号递增，2 s TTL 内就能
          // 攒出任意多槽位）。现在多了帧数 256 + 字节 64 MiB 双上限，超限按最旧优先淘汰，
          // 淘汰的帧**照常结算成 lost**（++lost、settle、settle_fec）——所以 lost 不会因为
          // "内存变有界"而变小，丢包率也不会被护栏美化。
          //   reassembly_pending_dropped = 因预算淘汰的帧数（**是 lost 的子集**，单独记是为了
          //     分清"为什么丢"：超时 = 链路问题，预算淘汰 = 接收端自保，两者处置不同）；
          //   reassembly_pending_bytes   = 当前占用（**gauge**，不是累计；正常链路远小于 1%）；
          //   reassembly_rejected        = 因预算拒收的包数（病态单帧，与协议层的 rejected 分开）。
          {"reassembly_pending_dropped", reassembly_pending_dropped},
          {"reassembly_pending_bytes", pending_bytes},
          {"reassembly_rejected", reassembly_rejected},
          // feedback 发送计数；RTT/抖动本身是 cued 的消费面，probe 不重复一份。
          {"fb_sent", fb_sent},
          {"rtt_ms", (last_rtt_us >= 0 && last_rtt_at_us > 0 &&
                      now_tick - last_rtt_at_us <= 1'000'000)
                         ? json(double(last_rtt_us) / 1000.0)
                         : json(nullptr)},
          {"jitter_ms", jitter_us >= 0.0 ? json(jitter_us / 1000.0) : json(nullptr)},
          {"synced", clk.have},
          {"lat_p50", pct_or_null(double(lat.percentile(0.50)) / 1000.0)},
          {"lat_p95", pct_or_null(double(lat.percentile(0.95)) / 1000.0)},
          {"lat_p99", pct_or_null(double(lat.percentile(0.99)) / 1000.0)},
          {"offset_us", clk.have ? json(clk.offset_us) : json(nullptr)},
          // 四段分解（微秒分位）：没样本 → null
          {"recv_us", seg_or_null(fs.recv_us)},
          {"reassembly_us", seg_or_null(fs.reassembly_us)},
          // 抖动缓冲等待：帧在**接收端**抖动缓冲里真实等了多久（进队 → 放行）。
          // 没样本（这一秒没有帧被放行）仍然是 null——"没测到"不能写 0。
          {"buffer_us", seg_or_null(fs.buffer_us)},
          // 抖动缓冲档位（自适应）：jb_level 是当前档位，jb_switches 是切换次数，
          // jb_skipped/jb_late 是累计计数。深度本身进 jb_depth。
          {"jb_depth", jb.stats().depth},
          {"jb_level", jb.stats().level},
          // 抖动估计本身必须可观测：否则"基准点用错"这种问题没有证据可查。
          // 单位是帧间隔（1.0 = 半个档位），与 jitter_up_frames 同量纲。
          {"jb_jitter_frames", jb.stats().jitter_frames},
          {"jb_loss_rate", jb.stats().loss_rate},
          {"jb_basis", jitter_basis == "done" ? "assembly_done" : "first_fragment"},
          {"jb_switches", jb.stats().switches},
          {"jb_target_frames", jb.stats().target_frames},
          {"jb_skipped", jb.stats().skipped},
          {"jb_late", jb.stats().late_dropped},
          // 入队/放行两个累计计数：把"帧去哪了"变成可以对账的等式。
          //   jb_pushed − jb_late − jb_depth == jb_released（被容量挤掉的帧走 skipped 那条账）
          //   jb_released == decoded + decode_errors + decode_skips + decode_no_output
          // 两条等式不成立就是有分支没记账——这比任何单点读数都更能暴露口径漏洞。
          {"jb_pushed", jb.stats().pushed},
          {"jb_released", jb.stats().released},
          // 到达间隔分布（回答"帧是不是成簇到达"）：fps 是均值，看不出来；这一组能。
          // arr_gap_max_ms 与 fps 一起读：fps≈30 而 gap_max≈400 ms = 成簇；
          // 两者都平稳 = 真的连续到达。arr_gap_hist 的桶边界见 ArrivalGapMeter::kEdgesUs，
          // 各桶之和 = arr_gap_samples（本窗口），所以"空档 >50 ms 的次数/秒"=
          // arr_gaps_over_50ms ÷ (本窗口时长)。
          {"arr_gap_max_ms", double(arrival_gaps.max_gap_us) / 1000.0},
          {"arr_gap_max_ms_total", double(arrival_gaps.max_gap_us_total) / 1000.0},
          {"arr_gaps_over_50ms", arrival_gaps.gaps},
          {"arr_gap_samples", arrival_gaps.samples},
          {"arr_gap_hist", gap_hist},
          // 同一套读数的**纯到达侧**版本（内核时间戳，不受本进程解码/渲染卡顿影响）。
          // 判读规则：
          //   两条都成簇 → 数据本来就是成簇来的，抖动缓冲只能吸收、吸收不了就丢；
          //   只有 arr_* 成簇 → 读循环自己做出来的（消费不过来），加深缓冲等于固化延迟。
          // 没有内核时间戳时（pkt_arr_gap_samples=0，Windows）这一组没有意义，看 recv_ts_missing。
          {"pkt_arr_gap_max_ms", double(datagram_gaps.max_gap_us) / 1000.0},
          {"pkt_arr_gap_max_ms_total", double(datagram_gaps.max_gap_us_total) / 1000.0},
          {"pkt_arr_gaps_over_50ms", datagram_gaps.gaps},
          {"pkt_arr_gap_samples", datagram_gaps.samples},
          {"decode_us", seg_or_null(fs.decode_us)},
          {"decode_ms_p50", fs.decode_us.size() ? json(double(fs.decode_us.percentile(0.50)) / 1000.0)
                                                : json(nullptr)},
          {"decode_ms_p99", fs.decode_us.size() ? json(double(fs.decode_us.percentile(0.99)) / 1000.0)
                                                : json(nullptr)},
          {"recv_ts_missing", fs.recv_ts_missing},
          {"decode_resets", dec.resets},
          // 解码器"等 IDR"期间丢掉的包数（设计行为，不是失败）。不暴露它的话，
          // flush 之后的冻屏会表现成 fps=0 + decode_errors=0，看起来像"链路没在送帧"。
          {"decode_gated", dec.dec.stats().gated},
          {"waiting_idr", dec.dec.waiting_keyframe()},
          // 因抖动缓冲跳帧而 flush 解码器的次数（参考链断开的处置次数）
          {"refchain_breaks", refchain_breaks},
          {"kf_req_sent", kf_req_sent},
          {"kf_req_suppressed", kf_req_suppressed},
          {"bad_frame_events", bad_frame_events},
          {"report_sent", reporters.sent()},
          {"report_drops", reporters.dropped()},
          // 音频（FR-DEV.6 的另一半）：cued 现在把上行 Opus 原样透传到下行，
          // probe 侧解成 PCM 并算 RMS——与 cued 侧同一套口径（AudioRmsWindow 已移到公共头），
          // 两端 dBFS 之差就是"链路 + 一次 Opus 编解码"对音量的影响。
          {"audio_datagrams", audio_datagrams},
          {"audio_decoded", audio_decoded_frames > 0},
          {"audio_decoded_frames", audio_decoded_frames},
          {"audio_pcm_frames", audio_pcm_frames},
          {"audio_decode_errors", audio_decode_errors},
          // C03: gaps are finalized missing slots; reorder is accepted before commit.
          // All counters are process cumulative, including across stream state resets.
          {"audio_seq_gaps", audio_stats.gaps},
          {"audio_dups", audio_stats.duplicates},
          {"audio_reordered", audio_stats.reordered},
          {"audio_stale", audio_stats.stale},
          {"audio_fec_recovered", audio_fec_recovered},
          {"audio_plc_frames", audio_plc_frames},
          {"audio_unconcealed_frames", audio_unconcealed_frames},
          {"audio_committed_frames", audio_stats.committed},
          {"audio_failed_frames", audio_stats.failed_frames},
          {"audio_pending_packets", audio_receiver.pending_packets()},
          {"audio_pending_bytes", audio_receiver.pending_bytes()},
          {"audio_resyncs", audio_stats.resyncs},
          {"audio_deadline_flushes", audio_stats.deadline_flushes},
          {"audio_capacity_flushes", audio_stats.capacity_flushes},
          // 开关的**实际值**：否则"开了 FEC 但没恢复一帧"与"FEC 根本没开"在指标上
          // 长得一模一样（"参数写了不等于生效"这条旧坑）。
          {"audio_fec_enabled", audio_fec_enabled},
          {"audio_plc_enabled", audio_plc_enabled},
          // 播放队列深度（毫秒）：咔咔声最常来自这一侧——队列耗尽就欠载，
          // 超过上限就自己丢包，而此刻链路的丢包指标可能完全是 0。
          // 把这一项暴露出来，"是链路丢了还是自己丢了"才不用靠耳朵猜。
          // -1（设备没打开）写成 null：null 是"没有这个观测"，-1 会被当成一个读数。
          {"audio_queue_ms", audio_queue_ms >= 0 ? json(audio_queue_ms) : json(nullptr)},
          // 窗口分位（近 60 s）。判断播放侧余量看这两个，别看上面的瞬时值。
          {"audio_queue_ms_p50", aq_pct(0.50) >= 0 ? json(aq_pct(0.50)) : json(nullptr)},
          {"audio_queue_ms_p95", aq_pct(0.95) >= 0 ? json(aq_pct(0.95)) : json(nullptr)},
          // 音画同步的实际补偿量。没有这几个数，"补了多少、还差多少"只能靠耳朵判断。
          {"av_audio_ms", json(av_audio_ewma_ms)},
          {"av_video_ms", av_have_video_lat ? json(av_video_ewma_ms) : json(nullptr)},
          {"av_comp_ms",
           av_have_video_lat ? json(av_audio_ewma_ms > av_video_ewma_ms
                                        ? av_audio_ewma_ms - av_video_ewma_ms
                                        : 0.0)
                             : json(nullptr)},

          // 队列账：软上限命中 = 在积压（还放行），硬上限命中 = 清过一次（听感会跳）。
          // 只有这两个数能把"播放侧自己丢的"和"链路丢的"分开。
          {"audio_queue_over_soft", rstats.over_soft},
          // 因队列越上限而丢掉的 20 ms 包数：它是"声音断续"最直接的嫌疑人，
          // 不算进来就只能靠耳朵判断。
          {"audio_dropped_drift", rstats.dropped_drift},
          // 设备被饿着的次数：音频卡顿的直接证据（比听感更可靠）。
          {"audio_starved", rstats.starved},
          {"audio_queue_trimmed", rstats.trimmed},
          {"audio_queue_push_errors", rstats.push_errors},
          {"audio_queue_segments", rstats.queued},
          {"audio_rms_frames", audio_rms.frames()},
          {"audio_rms_dbfs",
           audio_rms.empty() ? json(nullptr) : json(cue::media::dbfs_for_metric(audio_rms.dbfs()))},
          {"audio_note", audio_datagrams == 0
                             ? "no audio datagrams in downlink (upstream had no audio, or cued passthrough is off)"
                             : (audio_decoded_frames == 0
                                    ? "audio datagrams received but none decoded: " + audio_first_error
                                    : "decoded")},
      };
      emit("probe.metric", metric);
      // 分位环按窗口清零：指标行是「这一秒」的读数，不是进程累计。不清零的话，
      // 故障期的 2 s 端到端延迟会一直留在环里，修复后的 p99 仍然报 2 s——
      // harness 的前后对比就永远看不出修复效果（实测踩到）。
      lat.clear();
      fs.decode_us.clear();
      fs.recv_us.clear();
      fs.reassembly_us.clear();
      fs.buffer_us.clear();
      // 到达间隔直方图/计数同样按窗口清零（全程最大空档 max_gap_us_total 不清，
      // 它就是要跨窗口活下来的那个"最坏值"）。
      arrival_gaps.clear_window();
      datagram_gaps.clear_window();
      frames_prev = frames;
      decoded_prev = fs.decoded;
      t_last_report = now_tick;
    }

    // ---- 等一个数据报：超时取 min(200 ms, 抖动缓冲队首到期时刻) ----
    // 200 ms 的上限保证周期任务（指标行/时钟对齐/NACK 节流）在无流时照常跑；
    // 队首到期时刻更小时以它为准，否则"等待预算"只能等下一个包到达才生效。
    int poll_timeout_ms = 200;
    if (const int64_t deadline = jb.next_deadline_us(); deadline > 0) {
      const int64_t wait_us = deadline - cue::now_us();
      poll_timeout_ms = static_cast<int>(std::clamp<int64_t>(wait_us / 1000, 0, 200));
    }
    if (const int64_t deadline = audio_receiver.next_deadline_us(); deadline > 0) {
      const int64_t wait_us = deadline - cue::now_us();
      poll_timeout_ms = std::min(poll_timeout_ms,
          static_cast<int>(std::clamp<int64_t>(wait_us / 1000, 0, 200)));
    }
    // NAT 注册心跳：每 500 ms 从**同一个 socket** 发一次 HELLO。
    // 必须用收下行的这个 socket 发，NAT 才会为它建立映射，服务器回包才能进来
    // （换个 socket 发出，映射落在另一个外部端口上，等于白做）。
    if (hello_fd != kProbeInvalidSocket) {
      const int64_t now_us = cue::now_us();
      if (now_us - last_hello_us >= 500'000) {
        last_hello_us = now_us;
        static const char kHello[8] = {'C', 'U', 'E', 'H', 'E', 'L', 'L', 'O'};
        ::sendto(hello_fd, kHello, sizeof(kHello), 0,
                 reinterpret_cast<const sockaddr*>(&hello_addr), sizeof(hello_addr));
      }
    }
    probe_pollfd pfd{static_cast<decltype(probe_pollfd::fd)>(udp_fd), POLLIN, 0};
    const int pr = probe_poll(&pfd, 1, poll_timeout_ms);
    if (getenv("CUE_PROBE_TRACE") != nullptr) {
      static int n_trace = 0;
      if (n_trace++ < 5) {
        std::fprintf(stderr, "[trace] poll pr=%d fd=%lld revents=0x%x\n", pr,
                     static_cast<long long>(pfd.fd), static_cast<int>(pfd.revents));
      }
    }
    if (pr <= 0) continue;
    sockaddr_in from{};
    int64_t recv_us_sample = -1;
    const ssize_t n = recv_timed(buf.data(), buf.size(), from, recv_us_sample);
    if (n < static_cast<ssize_t>(cue::proto::kDatagramHeaderSize)) continue;
    ++packets;
    {
      const int64_t now_recv = cue::now_us();
      if (recv_us_sample >= 0) {
        fs.recv_us.push(recv_us_sample, now_recv);
        // 纯网络侧（或纯内核侧）的到达时刻 = 被读出时刻 − 在内核缓冲里待的时间。
        // 它与帧级 arr_gap_*（读循环时间轴）**必须分开记**：
        //   · 两条序列都成簇  → 数据本来就是成簇送来的（发送端/采集端的问题）；
        //   · 只有帧级成簇    → 我们自己消费不过来（解码/渲染卡住读循环，包在 socket
        //                       缓冲里积成堆），换更深的抖动缓冲只会把这个自伤固化成延迟。
        // 误差量级是 now_recv 与 recv_timed 内那次 clock_gettime 之差（微秒级），
        // 对"有没有 0.4 s 空档"这种量级无影响。
        datagram_gaps.observe(now_recv - recv_us_sample);
      } else {
        // 没有内核时间戳（Windows / SO_TIMESTAMP 不可用）就不填这个序列：
        // 用"被读出时刻"冒充到达时刻会让它退化成读循环节奏，正是要区分的那个量，
        // 等于自己制造伪证据。缺样本这件事由 recv_ts_missing 如实记账。
        ++fs.recv_ts_missing;
      }
    }

    auto h = cue::proto::decode_datagram({buf.data(), static_cast<std::size_t>(n)});
    if (!h) continue;

    // 首包锁定对端：之后其它源发来的报文不许并进重组/时钟估计（review #20）。
    // 但 cued 的源端口是 ephemeral（它不 bind），重启后必变；原 peer 静默超过阈值
    // （默认 500 ms）就允许重新锁定，否则一次重启会让 probe 永久拒收新 cued（AC-2 直接失败）。
    // 还有一种更快、更准的换源信号：新 cued 的第一帧 frame_id 从 0 重新开始。
    // 只靠静默阈值的话，重启后的第一个 IDR 正好落在静默窗口里被丢掉——丢了参数集，
    // 接下来整个 GOP 都解不出帧（实测：decoded 掉到 0、decode_errors 每秒 30）。
    const bool same_peer = have_peer && from.sin_addr.s_addr == peer.sin_addr.s_addr &&
                           from.sin_port == peer.sin_port;
    // 换源信号：不同 peer + 视频帧 + frame_id 从 0 开始（新 cued 的第一帧）。
    // 两个 cued 的地址都是 127.0.0.1，区别在端口，所以这里必须比 (addr, port) 而不是只比地址。
    const bool new_stream = have_peer && !same_peer &&
                            h->type == static_cast<uint8_t>(cue::proto::DownType::Video) &&
                            h->frame_id == 0;
    // 换一代流（对端换了 / 同一端口上换了会话）要重置的东西**完全一样**：
    // 重组表、frame_id 水位、解码器参数集与参考帧、抖动缓冲、feedback 的链路读数。
    // 抽成一个 lambda，两处调用，避免"只改了一边"这种最典型的回归。
    const auto reset_stream = [&](const char* reason) {
      const uint32_t old_max = max_completed;
      pending.clear();
      pending_bytes = 0;  // 表清空 → 预算计数器必须同步归零，否则换代后预算恒为"已满"
      max_completed = 0;
      have_completed = false;
      dec.params = ParamSets{};
      dec.reset();
      // 抖动缓冲同样要清：上一代的 PTS 基线还在的话，新流前几帧会被判成"迟到"丢掉，
      // 那些帧里往往正好包含新编码器的 IDR/SPS。
      jb.reset();
      // 播放队列也要清。不清的话，上一轮已经解码进 SDL 队列的 PCM 会继续播完——
      // 用户听到的就是"重启之后重复播放的那段，其实是上一轮的旧数据"。
      // 这一条和 jb.reset() 同源：凡是"按时间轴排队"的状态，换代就必须一起清。
      if (render_live) renderer.reset_audio();
      // The previous generation's pending packets and Opus state cannot reach the sink.
      // Process counters remain cumulative, matching PCM/FEC/PLC counters.
      audio_receiver.reset_stream();
      audio_rms = cue::media::AudioRmsWindow{};
      // ---- 同源的其他跨代状态 ----
      // 卡顿判定的"上一帧"必须一起清：新旧两代的 PTS 属于不同基线（帧号与 PTS 都可能
      // 从头开始），留着旧值只会让换代后的**第一帧**被判成一次超大卡顿——
      // 而那次"卡"其实是一次会话切换，不是这条流的问题。
      fs.last_pts_us = -1;
      fs.last_dec_end_us = -1;
      // 端到端分位环：里面的样本是用**上一代**的 offset 算出来的。offset 在换对端后
      // 会重估，同一个环里混两种时钟基的样本，p50/p99 就变成了两个分布的平均数。
      lat.clear();
      // 到达间隔的基准同理：换代之间的静默（重启常常几秒）不是链路的到达空档，
      // 不 rebase 的话直方图里会出现一个几秒的假空档，而它会被读成"成簇"的证据。
      arrival_gaps.rebase();
      datagram_gaps.rebase();
      // 单包探测的目标帧号属于上一代（新会话的 frame_id 从 0 重新开始），不清会让
      // "新流里恰好同号的一帧"被当成探测成功，把 NACK 从探测模式里提前放出来。
      nack_probe_pending = false;
      // feedback 的速率/抖动/RTT 都是「针对上一代对端」的读数，不能带进新会话：
      // 否则新 cued 的第一个 200 ms 会收到一条基于旧链路的反馈。
      fb_bytes_prev = static_cast<int64_t>(bytes);
      fb_t_prev = 0;
      last_rtt_us = -1;
      last_rtt_at_us = 0;
      rtt_ping_echo = 0;
      jitter_us = -1.0;
      last_transit_us = -1;
      emit("probe.stream_reset",
           json{{"reason", reason},
                {"old_max_frame", old_max},
                {"frames", frames},
                {"decoded", fs.decoded},
                {"old_session", session_seen}});
    };
    if (!have_peer) {
      peer = from;
      have_peer = true;
      t_last_peer_us = now_tick;
      stats_burst_left = kStatsBurst;
      next_stats_us = 0;
    } else if (!same_peer) {
      if (new_stream || now_tick - t_last_peer_us > peer_timeout_us) {
        std::fprintf(stderr,
                     "[probe] 对端切换：原 peer 已静默 %lld ms，改认 %s:%u\n",
                     static_cast<long long>((now_tick - t_last_peer_us) / 1000),
                     ::inet_ntoa(from.sin_addr), ntohs(from.sin_port));
        peer = from;
        t_last_peer_us = now_tick;
        stats_burst_left = kStatsBurst;
        next_stats_us = 0;  // 立刻重开一批：新 cued 的回包延迟特性与旧代无关
        clk.begin_batch();
        reset_stream(new_stream ? "new_stream" : "peer_switch");
        session_seen = h->session_id;
      } else {
        ++rejected;
        continue;
      }
    } else {
      t_last_peer_us = now_tick;
    }

    // 会话换代（T-22 / FR-PIPE.12）：cued 的一个会话号对应**一路设备**，同一时刻只有
    // 一个会话占着一个下行端口。同一端口上会话号变了 = 上一路断了、槽位分配给了新设备
    // （或同一台设备重连）。不重置的话，新会话的 frame_id 从 0 开始，会被
    // `h->frame_id <= max_completed` 当成"迟到分片"整段丢掉——探针会瞎到 frame_id
    // 追上旧水位为止（旧实现只处理 cued 进程重启，不处理"同一 cued 换会话"）。
    if (h->session_id != 0 && session_seen != 0 && h->session_id != session_seen) {
      std::fprintf(stderr, "[probe] 会话切换：%u -> %u（同一 cued 的另一个会话）\n",
                   session_seen, h->session_id);
      reset_stream("session_switch");
      session_seen = h->session_id;
    }

    if (h->type == static_cast<uint8_t>(cue::proto::DownType::StatsReq)) {
      // 时钟对齐回包：u64 echo（我方发戳）| u64 cued_now（对端时钟）
      if (n < static_cast<ssize_t>(cue::proto::kDatagramHeaderSize + 16)) continue;
      const uint8_t* pl = buf.data() + cue::proto::kDatagramHeaderSize;
      const int64_t echo = static_cast<int64_t>(cue::proto::detail::get_u64(pl));
      const int64_t cued_now = static_cast<int64_t>(cue::proto::detail::get_u64(pl + 8));
      const int64_t recv_now = cue::now_us();
      const int64_t rtt = recv_now - echo;
      if (rtt >= 0 && rtt <= 1'000'000) {
        // feedback 的 rtt_ms 用**最近一次**单发回环（1 Hz），不用 30 s 批的最小值：
        // 最小值是给 offset 估计去抖用的，它代表"最好情况"而不是"现在"。
        last_rtt_us = rtt;
        last_rtt_at_us = recv_now;
      }
      if (rtt_ping_echo != 0 && echo == rtt_ping_echo) {
        // 1 Hz 单发回环：只喂 RTT，不参与 offset 估计（保持"批内取最小 RTT"的口径）。
        rtt_ping_echo = 0;
        continue;
      }
      if (clk.observe(echo, cued_now, recv_now)) {
        // SPEC §7.1：漂移 > 5 ms 重锚并记事件——这个"记事件"现在真的落到事件流里
        emit("probe.reanchor", json{{"drift_us", clk.last_drift_us},
                                    {"offset_us", clk.offset_us},
                                    {"rtt_us", clk.best_rtt_us},
                                    {"samples", clk.samples}});
      }
      continue;
    }

    if (h->type == static_cast<uint8_t>(cue::proto::DownType::Fec)) {
      // payload: base_frame_id(4) | group_index(2) | member_count(2) | parity（wire.h 统一编解码）
      const auto fph = cue::proto::decode_fec_payload_header(
          {buf.data() + cue::proto::kDatagramHeaderSize,
           static_cast<std::size_t>(n) - cue::proto::kDatagramHeaderSize});
      if (!fph) {
        ++rejected;
        continue;
      }
      const uint32_t base_fid = fph->base_frame_id;
      // FEC 校验包总在整帧数据**之后**到达：该帧若已判定完成，这里会给它重建一个
      // 空槽位，随后又被 finish_if_complete 计成"第二次完成"——M0 的幽灵帧。
      // 静默丢弃 + 计数（N-02）：这条路径以前只是 continue，于是"守卫每帧都在命中"
      // 这件事没有任何账可查（实测零丢包 8 s / 241 帧 → 250 次）。它是**常态读数**，
      // 不是异常；真正的异常指示是 frag_ghost_discarded（零丢包 0）。
      // 守卫条件见 proto/frame_guard.h（抽成纯函数，单测 test_frame_guard）；
      // 计数留在这里：这条是常态读数，与下面 frag 那条的判读不同。
      if (cue::proto::would_rebuild_completed_frame(base_fid, max_completed, have_completed,
                                                    pending.find(base_fid) != pending.end())) {
        ++fec_ghost_discarded;
        continue;
      }
      // 组的位置在数据报头里：frag_index = 组起始片号，frag_count = 整帧片数。
      // member_count 是**本组**成员数（不是整帧片数）。旧格式（frag_index=0、
      // member_count=frag_count）自然落进"整帧一组"，所以不需要协议版本。
      const uint16_t total = h->frag_count;
      const uint16_t offset = h->frag_index;
      const uint16_t members = fph->member_count;
      if (total == 0 || members == 0 || offset >= total ||
          static_cast<uint32_t>(offset) + members > total) {
        ++rejected;  // 组的范围越界：伪造/损坏包，不能拿它去 resize 别人的槽位
        continue;
      }
      // 新建槽位前的预算判据（N-10）：FEC 校验包与视频分片共用同一个 pending，也共用
      // 同一本账——否则 FEC 分支就是绕过帧数上限的后门（每帧一个伪造的 base_frame_id
      // 也不大，同样能刷出无限槽位）。这一包要占的字节 = 校验包载荷长度。
      const std::size_t parity_off =
          cue::proto::kDatagramHeaderSize + cue::proto::kFecPayloadHeaderSize;
      const std::size_t parity_bytes = static_cast<std::size_t>(n) - parity_off;
      if (!reserve_slot(base_fid, parity_bytes)) {
        ++reassembly_rejected;  // 与协议层的 rejected 分开计：这一包本身合法，是预算装不下
        continue;
      }
      auto& ffa = pending[base_fid];
      if (ffa.count == 0) {
        ffa.count = total;
        ffa.pts_us = h->pts_us;
        // 冗余包本身不带 codec（cued 显式置 0）：codec 由随后的视频分片补上，
        // 若整帧只靠 FEC 恢复、连一片视频分片都没到过，就按"未标注"跳过解码。
        ffa.first_seen_us = cue::now_us();
        ffa.parts.resize(total);
      } else if (ffa.count != total) {
        ++rejected;  // 同一 frame_id 的 FEC 与数据报对整帧片数说法不一致
        continue;
      }
      auto& grp = ffa.fec_groups[fph->group_index];
      if (grp.available && (grp.offset != offset || grp.member_count != members)) {
        ++rejected;  // 同一 group_index 的几何信息前后矛盾
        continue;
      }
      grp.offset = offset;
      grp.member_count = members;
      // 同一 group_index 的重复校验包是**覆盖写**：先把旧的那份从账上扣掉，否则重复包
      // 会让 pending_bytes 越加越大（assign 只替换内存、不新增），预算于是被凭空收紧。
      const std::size_t old_parity_bytes = grp.parity.size();
      pending_bytes -= old_parity_bytes;
      ffa.bytes -= old_parity_bytes;
      grp.parity.assign(buf.data() + parity_off, buf.data() + n);
      pending_bytes += parity_bytes;
      ffa.bytes += parity_bytes;
      grp.available = true;
      fec_recovered += try_fec_recover(ffa, pending_bytes);
      grow_guard(base_fid);  // 校验包 + 恢复出的片都进了账，再守一次字节上限
      // 恢复后必须重新判定完整性：否则被 FEC 修好的帧永远不会被计为完成（实测踩到的坑）
      mark_probe_if_recovered(base_fid);
      finish_if_complete(base_fid, ffa);
      continue;
    }
    if (h->type == static_cast<uint8_t>(cue::proto::DownType::Audio)) {
      ++audio_datagrams;
      if (session_seen == 0) session_seen = h->session_id;
      const std::span<const uint8_t> payload{
          buf.data() + cue::proto::kDatagramHeaderSize,
          static_cast<std::size_t>(n) - cue::proto::kDatagramHeaderSize};
      audio_receiver.insert(h->frame_id, payload, cue::now_us(), emit_audio);
      continue;
    }
    if (h->type != static_cast<uint8_t>(cue::proto::DownType::Video)) continue;

    session_seen = h->session_id;  // 视频包：与上面两处换代判定同源，幂等
    bytes += static_cast<std::size_t>(n);
    // 迟到的重传分片不得为已完成的帧重建槽位（M0 幽灵帧教训）。
    // 首帧（还没有任何已完成帧）不适用这条：frame_id 从 0 开始，初值 0 会把 IDR 挡掉。
    // 守卫条件见 proto/frame_guard.h：与 FEC 分支同一个纯函数，计数各自独立。
    if (cue::proto::would_rebuild_completed_frame(h->frame_id, max_completed, have_completed,
                                                 pending.find(h->frame_id) != pending.end())) {
      // 与 FEC 分支同一个幽灵帧守卫，但触发源是迟到的视频分片（含 NACK 重传）。
      // 这才是**异常**：正常链路不该出现（实测零丢包 = 0、5% 丢包 = 2~3）；
      // 与 fec 那条"每完成帧一次的常态"必须分开计，否则会被常态淹没
      // （见 fec_ghost_discarded 的定义处）。
      ++frag_ghost_discarded;
      continue;
    }
    // frag_count == 0 会让 parts 为空、完整性判定零次迭代 → 判为完成并污染 lat/fps
    if (h->frag_count == 0) {
      ++rejected;
      continue;
    }
    // 新建槽位前的预算判据（N-10 / FR-PIPE.14）：pending 是**无界**的攻击面——
    // 已有两道防线都不管帧数（kMaxFragPerFrame 只约束单帧、cleanup_stale 只管 2 s TTL），
    // 而上行字节来自浏览器：每帧只发 1 片、帧号递增就能在 2 s 内攒出任意多槽位。
    // 所以先按预算淘汰最旧的帧；连淘汰都装不下这个分片就拒收它（不动存量队列）。
    const std::size_t part_bytes = static_cast<std::size_t>(n) - cue::proto::kDatagramHeaderSize;
    if (!reserve_slot(h->frame_id, part_bytes)) {
      ++reassembly_rejected;  // 与协议层的 rejected 分开计：这一包合法，是预算装不下
      continue;
    }
    auto& fa = pending[h->frame_id];
    if (fa.count == 0) {
      fa.count = h->frag_count;
      fa.pts_us = h->pts_us;
      fa.codec = h->codec;
      fa.first_seen_us = cue::now_us();
      fa.parts.resize(fa.count);
    }
    if (fa.codec == cue::proto::kDownCodecUnspecified && h->codec != cue::proto::kDownCodecUnspecified) {
      fa.codec = h->codec;  // 首片没标注、后续片标注了：补一次（不覆盖已标注的）
    }
    if (h->frag_index < fa.count && fa.parts[h->frag_index].empty()) {
      fa.parts[h->frag_index].assign(buf.data() + cue::proto::kDatagramHeaderSize, buf.data() + n);
      // 字节账只在**真正写入**时加（上面那个 empty() 判断保证同一下标只计一次：
      // 重复分片 / NACK 重传补同一片都不再改内存，也就不能再记一遍预算）。
      fa.bytes += part_bytes;
      pending_bytes += part_bytes;
      // 丢包率只统计**原始传输**：重传包（flags bit1）补回来的是链路已经丢过的那一片，
      // 把它算成"收到"会让丢失率随 NACK 修复一起下降——那样 >10% 冷却阈值永远不会触发，
      // 指标也会把一条 30% 丢包的链路报成 9%。FEC 恢复的片同样不计（它没走过链路）。
      if ((h->flags & cue::proto::kDownFlagRetransmit) == 0) ++fa.recv_parts;
      grow_guard(h->frame_id);  // 分片把它喂大了：再守一次字节上限（不动这一帧自己）
    }

    // FEC 与 NACK 必须协同：FEC 包先到（当时缺 2 片无法恢复），NACK 补回 1 片后
    // 仍缺 1 片，此刻本可恢复 —— 旧实现只在 FEC 包到达那一刻试一次，白丢一帧。
    fec_recovered += try_fec_recover(fa, pending_bytes);
    grow_guard(h->frame_id);  // 恢复出的片也占内存
    mark_probe_if_recovered(h->frame_id);
    finish_if_complete(h->frame_id, fa);

    const int64_t now2 = cue::now_us();

    // ---- NACK（FR-PROBE.2）：令牌桶限速 8 包/秒、单包 ≤64 条、坏窗口冷却 ----
    // 策略本身在 proto/nack.h（纯决策，可单测）。这里只负责「收集缺口 → 问策略 → 发送」。
    if (nack_enabled && have_peer) {
      const bool probing = nack_ctl.probing(now2);
      // 探测模式下每个统计窗口只发 1 条：额度用完后不再收集缺口，也不重复计 suppressed。
      const bool probe_capped = probing && nack_ctl.probe_window_used();
      if (!probe_capped && now2 - last_nack_attempt_us >= nack_interval_us) {
        last_nack_attempt_us = now2;
        const std::size_t max_entries = probing ? 1 : cue::proto::kMaxNackEntries;
        std::vector<uint8_t> entries;
        entries.reserve(cue::proto::kMaxNackEntries * cue::proto::kNackEntrySize);
        // 一次 NACK 里整帧缺片都装下了，才把该帧标记成「已请求」；否则剩下的缺口
        // 下一拍继续（review #8：部分入包的帧不能标位，否则剩下的缺片永远不被请求）。
        std::vector<uint32_t> fully_covered;
        uint32_t probe_fid = 0;
        uint16_t probe_frag = 0;
        for (auto& [fid, fa2] : pending) {
          if (fa2.count == 0 || fa2.nack_queued) continue;
          if (now2 - fa2.first_seen_us < nack_delay_us) continue;
          bool all_queued = true;
          for (uint16_t i = 0; i < fa2.count; ++i) {
            if (!fa2.parts[i].empty()) continue;
            if (entries.size() / cue::proto::kNackEntrySize >= max_entries) {
              all_queued = false;
              break;
            }
            cue::proto::append_nack_entry(entries, cue::proto::NackEntry{fid, i});
            if (probing) probe_frag = i;
          }
          if (all_queued) fully_covered.push_back(fid);
          if (probing) {
            // 单包探测只问最老的一帧的一个缺口：多问就失去"探测"的语义。
            if (!entries.empty()) {
              probe_fid = fid;
              break;
            }
          } else if (entries.size() / cue::proto::kNackEntrySize >= max_entries) {
            break;
          }
        }
        if (!entries.empty()) {
          if (nack_ctl.try_acquire(now2, probing)) {
            cue::proto::DatagramHeader nh;
            nh.type = static_cast<uint8_t>(cue::proto::DownType::Nack);
            nh.session_id = session_seen;  // 回带会话号：cued 侧据此过滤（review #20）
            const auto hdr = cue::proto::encode(nh);
            std::vector<uint8_t> pkt(hdr.begin(), hdr.end());
            pkt.insert(pkt.end(), entries.begin(), entries.end());
            send_udp(pkt);
            if (probing) {
              // 记住探测目标：这一片回来才算探测成功（见 mark_probe_if_recovered）。
              nack_probe_pending = true;
              nack_probe_fid = probe_fid;
              nack_probe_frag = probe_frag;
            } else {
              for (const uint32_t fid : fully_covered) {
                const auto it = pending.find(fid);
                if (it != pending.end()) it->second.nack_queued = true;
              }
            }
          }
        } else if (probing) {
          // 探测模式下已经没有缺口 = 需要恢复的东西自己恢复了：直接回常规。
          nack_ctl.on_probe_success();
          nack_probe_pending = false;
          emit("probe.nack_resume",
               json{{"reason", "no_gap"}, {"nack_sent", nack_ctl.sent()}});
        }
      }
    }
  }
}
