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
#include <poll.h>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

extern "C" {
#include <libavutil/frame.h>
}

#include "cue/common/clock.h"
#include "cue/media/h264_decoder.h"
#include "cue/media/jitter_buffer.h"
#include "cue/proto/nack.h"
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
std::size_t try_fec_recover(FrameAcc& fa) {
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
  void push(int64_t us) {
    if (buf_.size() < kCap) {
      buf_.push_back(us);
    } else {
      buf_[head_] = us;
      head_ = (head_ + 1) % kCap;
    }
    ++n_;
  }
  int64_t percentile(double p) const {
    if (buf_.empty()) return 0;
    std::vector<int64_t> v = buf_;
    std::sort(v.begin(), v.end());
    std::size_t idx = static_cast<std::size_t>(p * double(v.size() - 1));
    return v[idx];
  }
  std::size_t size() const { return buf_.size(); }
  void clear() {
    buf_.clear();
    head_ = 0;
  }

 private:
  static constexpr std::size_t kCap = 4096;
  std::vector<int64_t> buf_;
  std::size_t head_ = 0;
  uint64_t n_ = 0;
};

// 分位行：没有样本时整段是 null。写成 0 会被下游读成「这一段花了 0 ms」——
// 对 buffer 段尤其致命（cued 的抖动缓冲还没实现，0 就是伪造出来的数据）。
json seg_or_null(const LatencyRing& r) {
  if (r.size() == 0) return nullptr;
  return json{{"p50", r.percentile(0.50)}, {"p99", r.percentile(0.99)}};
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

// ---------------------------------------------------------------- 跨机上报
// 本地模式下 harness 直接读 probe 的 stdout（管道），不需要这条通道；跨机时 probe 与
// harness 不在同一台机器上，所以额外提供 TCP + JSON-line 上报（一行一个事件对象）。
// 约束：上报是**尽力而为**，不许影响收包主循环——连接非阻塞、失败只丢这一行并计数，
// 断线后按 1 s 节流重连。丢掉的行数会进指标行（report_drops），不静默。
class TcpReporters {
 public:
  struct Addr {
    std::string host;
    uint16_t port = 0;
  };

  explicit TcpReporters(std::vector<Addr> addrs) {
    for (auto& a : addrs) sinks_.push_back(Sink{std::move(a.host), a.port});
  }
  ~TcpReporters() {
    for (auto& s : sinks_) {
      if (s.fd >= 0) ::close(s.fd);
    }
  }
  TcpReporters(const TcpReporters&) = delete;
  TcpReporters& operator=(const TcpReporters&) = delete;

  bool empty() const { return sinks_.empty(); }

  // 尝试推进连接（非阻塞）；返回本轮新连上的地址（调用方据此记一条事件）。
  std::vector<std::string> pump(int64_t now_us) {
    std::vector<std::string> fresh;
    for (auto& s : sinks_) {
      if (s.up) continue;
      if (s.fd < 0) {
        if (now_us < s.next_try_us) continue;
        if (!begin_connect(s, now_us)) continue;
        if (s.up) fresh.push_back(s.host + ":" + std::to_string(s.port));
        continue;
      }
      // 连接进行中：用 poll(0) 看它是否已经完成
      pollfd pfd{s.fd, POLLOUT, 0};
      const int pr = ::poll(&pfd, 1, 0);
      if (pr <= 0) continue;
      int soerr = 0;
      socklen_t len = sizeof(soerr);
      if (::getsockopt(s.fd, SOL_SOCKET, SO_ERROR, &soerr, &len) != 0 || soerr != 0) {
        drop(s, now_us);
        continue;
      }
      s.up = true;
      fresh.push_back(s.host + ":" + std::to_string(s.port));
    }
    return fresh;
  }

  void publish(std::string_view line, int64_t now_us) {
    if (sinks_.empty()) return;
    for (auto& s : sinks_) {
      if (!s.up) {
        ++s.dropped;  // 未连上期间的指标行确实没送到，必须计数
        continue;
      }
      // MSG_NOSIGNAL：对端关闭时不能把整个 probe 用 SIGPIPE 打死
      const ssize_t n = ::send(s.fd, line.data(), line.size(), MSG_NOSIGNAL);
      if (n != static_cast<ssize_t>(line.size())) {
        ++s.dropped;
        drop(s, now_us);
        continue;
      }
      ++s.sent;
    }
  }

  uint64_t sent() const {
    uint64_t n = 0;
    for (const auto& s : sinks_) n += s.sent;
    return n;
  }
  uint64_t dropped() const {
    uint64_t n = 0;
    for (const auto& s : sinks_) n += s.dropped;
    return n;
  }

 private:
  struct Sink {
    std::string host;
    uint16_t port = 0;
    int fd = -1;
    bool up = false;
    int64_t next_try_us = 0;
    uint64_t sent = 0;
    uint64_t dropped = 0;
  };

  static constexpr int64_t kRetryUs = 1'000'000;
  static constexpr int64_t kConnectBudgetUs = 200'000;

  bool begin_connect(Sink& s, int64_t now_us) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(s.port);
    if (::inet_pton(AF_INET, s.host.c_str(), &addr.sin_addr) != 1) {
      s.next_try_us = now_us + kRetryUs;  // 解析不了就按域名失败处理，节流重试
      return false;
    }
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      s.next_try_us = now_us + kRetryUs;
      return false;
    }
    const int r = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (r == 0) {
      s.fd = fd;
      s.up = true;
      return true;
    }
    if (errno != EINPROGRESS) {
      ::close(fd);
      s.next_try_us = now_us + kRetryUs;
      return false;
    }
    // 给一点时间再判定：回环上通常立刻完成，跨机要等 SYN-ACK
    pollfd pfd{fd, POLLOUT, 0};
    if (::poll(&pfd, 1, static_cast<int>(kConnectBudgetUs / 1000)) > 0) {
      int soerr = 0;
      socklen_t len = sizeof(soerr);
      if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) == 0 && soerr == 0) {
        s.fd = fd;
        s.up = true;
        return true;
      }
      ::close(fd);
      s.next_try_us = now_us + kRetryUs;
      return false;
    }
    s.fd = fd;  // 仍在进行中：保留 fd，下一轮用 poll(0) 收尾
    return true;
  }

  void drop(Sink& s, int64_t now_us) {
    if (s.fd >= 0) ::close(s.fd);
    s.fd = -1;
    s.up = false;
    s.next_try_us = now_us + kRetryUs;
  }

  std::vector<Sink> sinks_;
};

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

void cleanup_stale(std::map<uint32_t, FrameAcc>& pending, int64_t now_us, uint64_t& lost,
                   LossWindow& win, FecSettle& fec) {
  for (auto it = pending.begin(); it != pending.end();) {
    if (now_us - it->second.first_seen_us > 2'000'000) {
      ++lost;  // 超时未被完成即计丢失（不再依赖 pending.size()>256 这条死条件）
      settle(win, it->second);
      settle_fec(it->second, fec);
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
  // 档位阶梯是 2/4/8/16 帧；默认只允许升到 2 档（8 帧），因为 16 帧档在 30 fps 下
  // 就是 500 ms 的驻留延迟——那个档位要显式 --jitter-max-level 3 才打开。
  bool jitter_enabled = true;
  long long jitter_budget_ms = 40;
  long long jitter_max_level = 2;
  std::string bind_host = "0.0.0.0";
  std::vector<TcpReporters::Addr> report_addrs;

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
    } else if (a == "--jitter") {
      const auto val = next();
      if (val != "on" && val != "off") bad(a, val);
      jitter_enabled = (val == "on");
    } else if (a == "--jitter-budget-ms") {
      const auto val = next();
      if (!parse_int(val, 0, 2000, jitter_budget_ms)) bad(a, val);
    } else if (a == "--jitter-max-level") {
      const auto val = next();
      if (!parse_int(val, 0, 3, jitter_max_level)) bad(a, val);
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
          "          [--jitter-max-level 0..3]\n"
          "          [--report-addrs host:port[,host:port]]\n"
          "  --nack-interval-ms 只决定「多长时间尝试一次」；实际发送上限是 8 包/秒\n"
          "  （SPEC §7.2），间隔 <125 ms 会被夹到该上限。冷却/单包探测不需要额外开关。\n"
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
  // 而不是一超就挤掉——容量是硬上限，不是目标深度）。
  cue::media::JitterBufferConfig jb_cfg;
  jb_cfg.enabled = jitter_enabled;
  jb_cfg.adaptive = jitter_enabled;
  jb_cfg.min_level = 0;
  jb_cfg.max_level = static_cast<std::size_t>(jitter_max_level);
  jb_cfg.max_frames = cue::media::kDepthLadder[jb_cfg.max_level] + 2;
  jb_cfg.nominal_interval_us = frame_interval_us;
  jb_cfg.budget_us = jitter_budget_ms * 1000;
  jb_cfg.nack_wait_us = nack_wait_ms * 1000;  // 0 = 不联动（只有 budget 生效）
  jb_cfg.basis = (jitter_basis == "done") ? cue::media::JitterBasis::AssemblyDone
                                          : cue::media::JitterBasis::FirstFragment;  // 0 = 不联动（只有 budget 生效）

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
  // SO_TIMESTAMP：让内核在数据报上打到达时间戳，recv 段（到达 → 被读出）才有意义。
  // 没有它就只能测到 poll 之后的时间（恒 ≈0），那等于没测；取不到时间戳时宁可
  // 记 recv_ts_missing 并让该段无样本，也不写一个假的 0。
  {
    const int on = 1;
    const int fd = sock.native_handle();
    if (::setsockopt(fd, SOL_SOCKET, SO_TIMESTAMP, &on, sizeof(on)) != 0) {
      std::fprintf(stderr, "[probe] SO_TIMESTAMP 打开失败（recv 段将无样本）：%s\n",
                   std::strerror(errno));
    }
  }
  const int udp_fd = sock.native_handle();

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
            {"jitter_max_level", jitter_max_level},
            {"jitter_max_frames", jb_cfg.max_frames}});

  std::map<uint32_t, FrameAcc> pending;
  LatencyRing lat;
  ClockAlign clk;
  DecodeState dec;
  FrameStats fs;
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
  uint64_t rejected = 0, lat_invalid = 0, stats_reqs = 0, fb_sent = 0;
  // FEC 账（FR-PIPE.9）：恢复的片数、有 FEC 覆盖的组数、结算时仍缺 ≥2 片的组与片数。
  // groups_rx 与 recovered/unrecoverable 一起构成「恢复率」的可执行分母：
  //   rate = recovered / (recovered + unrecoverable_pkts)
  // 只拿 recovered 一个数讲不了"到底有多少丢了没救回来"。
  uint64_t fec_recovered = 0;
  FecSettle fec_totals;
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
    fs.decode_us.push(std::max<int64_t>(0, dec_us));
    if (out != nullptr) {
      ++fs.decoded;
      dec.params_ready = true;
      dec.fail_streak = 0;
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
      lat.push(lat_us);
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
    fs.buffer_us.push(std::max<int64_t>(0, jf.wait_us));
    decode_one(jf.codec, jf.frag_count, {jf.payload.data(), jf.payload.size()});
    // 卡顿按「帧与帧」的间隔算：与这一帧能否解出来无关（解不出来的帧同样占时间轴），
    // 所以放在解码之后统一观察，而不是塞进解码分支（跳过/失败会漏掉间隔）。
    fs.observe_gap(jf.pts_us, now_rel, stall_threshold_us);
  };

  // 抖动缓冲的时间驱动：主循环顶部、每个数据报之后、以及 poll 超时都走这里，
  // 保证"等待预算"是按时间生效的，而不是按"下一个包什么时候来"生效。
  auto drain_jb = [&](int64_t now) {
    for (const auto& jf : jb.poll(now)) deliver(jf);
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
    fs.reassembly_us.push(std::max<int64_t>(0, now_done - fa.first_seen_us));
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
    pending.erase(fid);
    for (const auto& ready : jb.push(std::move(jf), now_done)) deliver(ready);
    return true;
  };

  // SPEC §7.1 的时钟对齐：向 cued 发下行回环请求（载荷 = 我方发戳）
  auto send_udp = [&](const std::vector<uint8_t>& pkt) {
    if (!have_peer) return false;
    return ::sendto(udp_fd, pkt.data(), pkt.size(), 0, reinterpret_cast<const sockaddr*>(&peer),
                    sizeof(peer)) == static_cast<ssize_t>(pkt.size());
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

  std::array<uint8_t, 4096> buf{};
  for (;;) {
    // ---- 周期任务放在循环顶部：FEC 分支 / 完全没包时都不会被跳过 ----
    const int64_t now_tick = cue::now_us();
    // 抖动缓冲的到期帧先放行：后面的周期任务（清理/窗口/指标行）这一拍就该看到它们，
    // 否则 buffer_us 的采样与 jb_depth 会慢一拍。
    drain_jb(now_tick);
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
      cleanup_stale(pending, now_tick, lost_frames, win, fec_totals);
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
      const double dt = double(now_tick - t_last_report) / 1e6;
      const uint64_t df = frames - frames_prev;
      const uint64_t dd = fs.decoded - decoded_prev;
      const double fps_rx = double(df) / dt;
      const double fps_dec = double(dd) / dt;
      std::printf(
          "[probe] fps=%.1f frames=%llu lost=%llu pend=%zu pkts=%llu nack=%llu sup=%llu "
          "pm=%llums mode=%s pause=%llu fec_rec=%llu fec_unrec=%llu rej=%llu loss=%.1f%% "
          "MB=%.2f fb=%llu  ",
          fps_rx, static_cast<unsigned long long>(frames),
          static_cast<unsigned long long>(lost_frames), pending.size(),
          static_cast<unsigned long long>(packets),
          static_cast<unsigned long long>(nack_sent_now),
          static_cast<unsigned long long>(nack_sup_now),
          static_cast<unsigned long long>(nack_pm_now),
          cue::proto::NackController::mode_name(nack_mode),
          static_cast<unsigned long long>(nack_pauses_now),
          static_cast<unsigned long long>(fec_recovered),
          static_cast<unsigned long long>(fec_totals.unrecoverable),
          static_cast<unsigned long long>(rejected),
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
      json metric{
          {"fps", fps_dec},          // 解码帧率：分母是解码出的帧，不是收包数
          {"fps_rx", fps_rx},        // 收包侧的帧率，仅供对照
          {"frames", frames},
          {"decoded", fs.decoded},
          {"decode_errors", fs.decode_errors},
          {"decode_skips", fs.decode_skips},
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
          {"decode_us", seg_or_null(fs.decode_us)},
          {"decode_ms_p50", fs.decode_us.size() ? json(double(fs.decode_us.percentile(0.50)) / 1000.0)
                                                : json(nullptr)},
          {"decode_ms_p99", fs.decode_us.size() ? json(double(fs.decode_us.percentile(0.99)) / 1000.0)
                                                : json(nullptr)},
          {"recv_ts_missing", fs.recv_ts_missing},
          {"decode_resets", dec.resets},
          {"bad_frame_events", bad_frame_events},
          {"report_sent", reporters.sent()},
          {"report_drops", reporters.dropped()},
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
    pollfd pfd{udp_fd, POLLIN, 0};
    const int pr = ::poll(&pfd, 1, poll_timeout_ms);
    if (pr <= 0) continue;
    sockaddr_in from{};
    int64_t recv_us_sample = -1;
    const ssize_t n = recv_timed(buf.data(), buf.size(), from, recv_us_sample);
    if (n < static_cast<ssize_t>(cue::proto::kDatagramHeaderSize)) continue;
    ++packets;
    if (recv_us_sample >= 0) {
      fs.recv_us.push(recv_us_sample);
    } else {
      ++fs.recv_ts_missing;
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
        // 新的一代 cued 从 frame_id=0 重新计数，而 max_completed 还是上一代的大值：
        // 不重置的话新流的前 max_completed 帧全被当成"迟到分片"，探针会瞎掉好几秒
        // （这正是"probe 不跟随 cued 重启"之后必须一起修的地方）。
        // 解码器同样要重置：新编码器的 SPS/PPS 可能变了，旧参数集不能沿用。
        const uint32_t old_max = max_completed;
        pending.clear();
        max_completed = 0;
        have_completed = false;
        dec.params = ParamSets{};
        dec.reset();
        // 抖动缓冲同样要清：上一代的 PTS 基线还在的话，新流前几帧会被判成"迟到"丢掉，
        // 那些帧里往往正好包含新编码器的 IDR/SPS。
        jb.reset();
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
             json{{"reason", new_stream ? "new_stream" : "peer_switch"},
                  {"old_max_frame", old_max},
                  {"frames", frames},
                  {"decoded", fs.decoded}});
      } else {
        ++rejected;
        continue;
      }
    } else {
      t_last_peer_us = now_tick;
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
      if (have_completed && base_fid <= max_completed && pending.find(base_fid) == pending.end()) {
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
      const std::size_t parity_off =
          cue::proto::kDatagramHeaderSize + cue::proto::kFecPayloadHeaderSize;
      auto& grp = ffa.fec_groups[fph->group_index];
      if (grp.available && (grp.offset != offset || grp.member_count != members)) {
        ++rejected;  // 同一 group_index 的几何信息前后矛盾
        continue;
      }
      grp.offset = offset;
      grp.member_count = members;
      grp.parity.assign(buf.data() + parity_off, buf.data() + n);
      grp.available = true;
      fec_recovered += try_fec_recover(ffa);
      // 恢复后必须重新判定完整性：否则被 FEC 修好的帧永远不会被计为完成（实测踩到的坑）
      mark_probe_if_recovered(base_fid);
      finish_if_complete(base_fid, ffa);
      continue;
    }
    if (h->type != static_cast<uint8_t>(cue::proto::DownType::Video)) continue;

    session_seen = h->session_id;
    bytes += static_cast<std::size_t>(n);
    // 迟到的重传分片不得为已完成的帧重建槽位（M0 幽灵帧教训）。
    // 首帧（还没有任何已完成帧）不适用这条：frame_id 从 0 开始，初值 0 会把 IDR 挡掉。
    if (have_completed && pending.find(h->frame_id) == pending.end() &&
        h->frame_id <= max_completed) {
      continue;
    }
    // frag_count == 0 会让 parts 为空、完整性判定零次迭代 → 判为完成并污染 lat/fps
    if (h->frag_count == 0) {
      ++rejected;
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
      // 丢包率只统计**原始传输**：重传包（flags bit1）补回来的是链路已经丢过的那一片，
      // 把它算成"收到"会让丢失率随 NACK 修复一起下降——那样 >10% 冷却阈值永远不会触发，
      // 指标也会把一条 30% 丢包的链路报成 9%。FEC 恢复的片同样不计（它没走过链路）。
      if ((h->flags & cue::proto::kDownFlagRetransmit) == 0) ++fa.recv_parts;
    }

    // FEC 与 NACK 必须协同：FEC 包先到（当时缺 2 片无法恢复），NACK 补回 1 片后
    // 仍缺 1 片，此刻本可恢复 —— 旧实现只在 FEC 包到达那一刻试一次，白丢一帧。
    fec_recovered += try_fec_recover(fa);
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
