// cue-probe — 下行接收端
// UDP 收包 → 分片重组 → FEC/NACK → H.264 软解 → 帧统计（帧率 / 卡顿 / 四段延迟 / 端到端分位）
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

struct FrameAcc {
  uint16_t count = 0;
  uint64_t pts_us = 0;
  int64_t first_seen_us = 0;
  uint8_t codec = cue::proto::kDownCodecUnspecified;
  uint16_t recv_parts = 0;  // 真正收到的分片数（FEC 恢复的不计，用于丢包率）
  std::vector<std::vector<uint8_t>> parts;
  std::vector<uint8_t> parity;
  bool fec_available = false;
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

// 用 FEC 冗余包恢复缺失的那一片（仅当恰好缺 1 片）
bool try_fec_recover(FrameAcc& fa, uint64_t& recovered) {
  if (!fa.fec_available || fa.parity.empty()) return false;
  int missing = -1;
  int n_missing = 0;
  for (uint16_t i = 0; i < fa.count; ++i) {
    if (fa.parts[i].empty()) {
      missing = i;
      ++n_missing;
    }
  }
  if (n_missing != 1) return false;
  std::vector<uint8_t> rec = fa.parity;
  for (uint16_t i = 0; i < fa.count; ++i) {
    if (i == static_cast<uint16_t>(missing)) continue;
    const auto& p = fa.parts[i];
    for (std::size_t k = 0; k < p.size() && k < rec.size(); ++k) rec[k] ^= p[k];
  }
  fa.parts[missing] = std::move(rec);
  ++recovered;
  return true;
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
                   LossWindow& win) {
  for (auto it = pending.begin(); it != pending.end();) {
    if (now_us - it->second.first_seen_us > 2'000'000) {
      ++lost;  // 超时未被完成即计丢失（不再依赖 pending.size()>256 这条死条件）
      settle(win, it->second);
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
  long long nack_interval_ms = 125;  // SPEC §7.2：限速 8 包/秒（旧默认 15 ms ≈ 66 包/秒）
  // 标称帧率只用于「卡顿阈值」的推断（阈值 = stall_factor × 1/fps），不是测量值。
  // 30 是这个项目的默认推流帧率（cued 的 --fps 默认 30）。
  long long nominal_fps = 30;
  double stall_factor = 1.5;
  // 对端切换的静默阈值。原实现是 3 s（review #20 的防串流考虑），但 harness 现在会
  // 主动重启 cued（改档/注入），3 s 静默意味着每次重启都要黑屏 3 s——而重测窗口只有
  // 约 1 s，端到端指标会整段缺失。500 ms 仍然要求"当前 peer 确实安静了很久"
  // （30 fps 下任何活着的 cued 间隔都 ≤33 ms），足以防串流，又不至于吞掉重测窗口。
  long long peer_timeout_ms = 500;
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
    } else if (a == "--nack-interval-ms") {
      const auto val = next();
      if (!parse_int(val, 1, 10000, nack_interval_ms)) bad(a, val);
    } else if (a == "--help") {
      std::printf(
          "cue-probe --port 9001 [--bind 0.0.0.0] [--nack on|off] [--nack-delay-ms 15]\n"
          "          [--nack-interval-ms 125] [--fps 30] [--stall-factor 1.5]\n"
          "          [--peer-timeout-ms 500]\n"
          "          [--report-addrs host:port[,host:port]]\n");
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
            {"nominal_fps", nominal_fps},
            {"stall_threshold_us", stall_threshold_us},
            {"report_addrs", report_addrs.size()}});

  std::map<uint32_t, FrameAcc> pending;
  LatencyRing lat;
  ClockAlign clk;
  DecodeState dec;
  FrameStats fs;
  uint64_t frames = 0, lost_frames = 0, bytes = 0, packets = 0, fec_recovered = 0, nack_sent = 0;
  uint64_t rejected = 0, lat_invalid = 0, nack_pauses = 0, stats_reqs = 0;
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
  int64_t last_nack_us = 0;
  const int64_t nack_interval_us = nack_interval_ms * 1000;
  const int64_t nack_delay_us = nack_delay_ms * 1000;
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
  int64_t nack_pause_until = 0;  // 丢包率持续过高时暂停 NACK 的截止时刻（冷却 1 s）
  LossWindow win;
  double win_loss = 0.0;
  int bad_windows = 0;
  bool warned_bad_frame = false;
  uint64_t bad_frame_events = 0;

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

  // 帧完成结算：所有分片到齐就计入帧数、延迟样本，然后解码
  auto finish_if_complete = [&](uint32_t fid, FrameAcc& fa) {
    for (const auto& pp : fa.parts) {
      if (pp.empty()) return false;
    }
    const int64_t now_done = cue::now_us();
    ++frames;
    // 延迟 = probe 收帧时刻（换算到 cued 时钟域）− 帧时间戳
    const int64_t lat_us = now_done + clk.offset_us - static_cast<int64_t>(fa.pts_us);
    if (clk.have && lat_plausible(lat_us)) {
      lat.push(lat_us);
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
    // 重组段：首片到达 → 整帧完成。这是 probe 自己能测的"重组开销"。
    fs.reassembly_us.push(std::max<int64_t>(0, now_done - fa.first_seen_us));
    if (!have_completed || fid > max_completed) max_completed = fid;
    have_completed = true;
    settle(win, fa);
    const uint64_t pts_us = fa.pts_us;
    const uint8_t codec = fa.codec;
    const uint16_t frag_count = fa.count;
    std::vector<uint8_t> frame = assemble(fa);
    pending.erase(fid);
    decode_one(codec, frag_count, {frame.data(), frame.size()});
    // 卡顿按「帧与帧」的间隔算：与这一帧能否解出来无关（解不出来的帧同样占时间轴），
    // 所以放在解码之后统一观察，而不是塞进解码分支（跳过/失败会漏掉间隔）。
    fs.observe_gap(pts_us, cue::now_us(), stall_threshold_us);
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

  std::array<uint8_t, 4096> buf{};
  for (;;) {
    // ---- 周期任务放在循环顶部：FEC 分支 / 完全没包时都不会被跳过 ----
    const int64_t now_tick = cue::now_us();
    if (now_tick >= next_stats_us) {
      if (stats_burst_left == kStatsBurst) clk.begin_batch();  // 批首：批内取 RTT 最小者
      send_stats_request(now_tick);
      --stats_burst_left;
      // 批内 20 ms 一发；批发完回到 30 s 周期的重估（SPEC §7.1）
      next_stats_us = stats_burst_left > 0 ? now_tick + kStatsBurstGapUs
                                           : now_tick + 30'000'000;
    }
    if (!reporters.empty()) {
      // 连接推进放在周期任务里：pump 可能为跨机握手等上 200 ms，
      // 解码循环里做这件事会把收包节奏带歪。
      for (const auto& peer_addr : reporters.pump(now_tick)) {
        emit("probe.report_connected", json{{"peer", peer_addr}});
      }
    }
    if (now_tick - t_last_cleanup >= 500'000) {
      cleanup_stale(pending, now_tick, lost_frames, win);
      t_last_cleanup = now_tick;
    }
    if (now_tick - t_last_window >= 1'000'000) {
      win_loss = win.expected ? double(win.expected - win.received) / double(win.expected) : 0.0;
      if (win.expected > 0 && win_loss > 0.10) {
        ++bad_windows;
        if (bad_windows >= 3) {
          nack_pause_until = now_tick + 1'000'000;
          bad_windows = 0;
          ++nack_pauses;
          std::fprintf(stderr, "[probe] loss=%.1f%% 连续 3 窗口 >10%%：暂停 NACK 1 s\n",
                       win_loss * 100.0);
        }
      } else if (win.expected > 0) {
        bad_windows = 0;
      }
      win = LossWindow{};
      t_last_window = now_tick;
    }
    if (now_tick - t_last_report >= 1'000'000) {
      const double dt = double(now_tick - t_last_report) / 1e6;
      const uint64_t df = frames - frames_prev;
      const uint64_t dd = fs.decoded - decoded_prev;
      const double fps_rx = double(df) / dt;
      const double fps_dec = double(dd) / dt;
      std::printf(
          "[probe] fps=%.1f frames=%llu lost=%llu pend=%zu pkts=%llu nack=%llu pause=%llu "
          "fec_rec=%llu rej=%llu loss=%.1f%% MB=%.2f  ",
          fps_rx, static_cast<unsigned long long>(frames),
          static_cast<unsigned long long>(lost_frames), pending.size(),
          static_cast<unsigned long long>(packets), static_cast<unsigned long long>(nack_sent),
          static_cast<unsigned long long>(nack_pauses), static_cast<unsigned long long>(fec_recovered),
          static_cast<unsigned long long>(rejected),
          win_loss * 100.0, double(bytes) / (1024.0 * 1024.0));
      std::printf(
          "dec=%.1ffps dec=%llu dec_err=%llu skip=%llu stall=%llu max_stall=%.1fms ",
          fps_dec, static_cast<unsigned long long>(fs.decoded),
          static_cast<unsigned long long>(fs.decode_errors),
          static_cast<unsigned long long>(fs.decode_skips),
          static_cast<unsigned long long>(fs.stalls),
          double(fs.max_stall_us) / 1000.0);
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
          {"nack", nack_sent},
          {"fec_rec", fec_recovered},
          {"synced", clk.have},
          {"lat_p50", pct_or_null(double(lat.percentile(0.50)) / 1000.0)},
          {"lat_p95", pct_or_null(double(lat.percentile(0.95)) / 1000.0)},
          {"lat_p99", pct_or_null(double(lat.percentile(0.99)) / 1000.0)},
          {"offset_us", clk.have ? json(clk.offset_us) : json(nullptr)},
          // 四段分解（微秒分位）：没样本 → null
          {"recv_us", seg_or_null(fs.recv_us)},
          {"reassembly_us", seg_or_null(fs.reassembly_us)},
          // 抖动缓冲等待：cued 侧抖动缓冲尚未实现（另一批任务在做），接收端没有这一段
          // 可测，所以这里恒为 null 并带上原因。等 cued 真的插入抖动缓冲后，
          // 等待时长会随下行帧一起带过来（协议加字段），这一段才可能有数。
          {"buffer_us", json(nullptr)},
          {"buffer_reason", "cued_jitter_buffer_unimplemented"},
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
      frames_prev = frames;
      decoded_prev = fs.decoded;
      t_last_report = now_tick;
    }

    // ---- 等一个数据报：200 ms 超时保证上面的周期任务准时跑（无流时也能报指标） ----
    pollfd pfd{udp_fd, POLLIN, 0};
    const int pr = ::poll(&pfd, 1, 200);
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
      if (clk.observe(echo, cued_now, cue::now_us())) {
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
      // member_count 与数据报头里的 frag_count 双写：以头为准，对不上就拒包（review #18）
      if (fph->member_count == 0 || fph->member_count != h->frag_count) {
        ++rejected;
        continue;
      }
      auto& ffa = pending[base_fid];
      if (ffa.count == 0) {
        ffa.count = fph->member_count;
        ffa.pts_us = h->pts_us;
        // 冗余包本身不带 codec（cued 显式置 0）：codec 由随后的视频分片补上，
        // 若整帧只靠 FEC 恢复、连一片视频分片都没到过，就按"未标注"跳过解码。
        ffa.first_seen_us = cue::now_us();
        ffa.parts.resize(fph->member_count);
      }
      const std::size_t parity_off =
          cue::proto::kDatagramHeaderSize + cue::proto::kFecPayloadHeaderSize;
      ffa.parity.assign(buf.data() + parity_off, buf.data() + n);
      ffa.fec_available = true;
      try_fec_recover(ffa, fec_recovered);
      // 恢复后必须重新判定完整性：否则被 FEC 修好的帧永远不会被计为完成（实测踩到的坑）
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
      ++fa.recv_parts;
    }

    // FEC 与 NACK 必须协同：FEC 包先到（当时缺 2 片无法恢复），NACK 补回 1 片后
    // 仍缺 1 片，此刻本可恢复 —— 旧实现只在 FEC 包到达那一刻试一次，白丢一帧。
    try_fec_recover(fa, fec_recovered);
    finish_if_complete(h->frame_id, fa);

    const int64_t now2 = cue::now_us();

    // NACK：限速 8 包/秒（间隔 ≥125 ms）、单包 ≤64 条；
    // 丢包率 >10% 连续 3 个窗口 → 暂停 + 冷却 1 s（弱网下不能放大拥塞）
    if (nack_enabled && have_peer && now2 >= nack_pause_until &&
        now2 - last_nack_us >= nack_interval_us) {
      std::vector<uint8_t> entries;
      entries.reserve(cue::proto::kMaxNackEntries * cue::proto::kNackEntrySize);
      for (auto& [fid, fa2] : pending) {
        if (fa2.count == 0 || fa2.nack_queued) continue;
        if (now2 - fa2.first_seen_us < nack_delay_us) continue;
        bool all_queued = true;
        for (uint16_t i = 0; i < fa2.count; ++i) {
          if (!fa2.parts[i].empty()) continue;
          if (entries.size() + cue::proto::kNackEntrySize >
              cue::proto::kMaxNackEntries * cue::proto::kNackEntrySize) {
            all_queued = false;  // 本帧剩余缺片进不来，下一轮再来
            break;
          }
          cue::proto::append_nack_entry(entries, cue::proto::NackEntry{fid, i});
        }
        // 只有整帧缺片都进过请求才置位；否则剩下的缺片永远不会被请求（review #8）
        if (all_queued) fa2.nack_queued = true;
        if (entries.size() + cue::proto::kNackEntrySize >
            cue::proto::kMaxNackEntries * cue::proto::kNackEntrySize) {
          break;
        }
      }
      if (!entries.empty()) {
        cue::proto::DatagramHeader nh;
        nh.type = static_cast<uint8_t>(cue::proto::DownType::Nack);
        nh.session_id = session_seen;  // 回带会话号：cued 侧据此过滤（review #20）
        const auto hdr = cue::proto::encode(nh);
        std::vector<uint8_t> pkt(hdr.begin(), hdr.end());
        pkt.insert(pkt.end(), entries.begin(), entries.end());
        send_udp(pkt);
        ++nack_sent;
      }
      last_nack_us = now2;
    }
  }
}
