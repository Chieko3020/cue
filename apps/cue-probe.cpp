// cue-probe — 下行接收端
// UDP 收包 → 分片重组 → FEC/NACK → 帧完成统计（帧率、端到端延迟分位、丢帧）
//
// 延迟口径（SPEC §5：probe 通常跨机，与本机 cued 不是同一 CLOCK_MONOTONIC 基准）：
//   pts_us 是 cued 时钟域的帧时间戳，直接拿 probe 的 now 去减会得到毫无意义的差值。
//   所以 probe 周期性发 StatsReq（下行回环 ping），用最小 RTT 样本估
//   offset = cued_clock − probe_clock，端到端延迟按 (probe_recv + offset − pts_us) 计算；
//   从未拿到样本时不输出延迟分位（lat=unsynced）。同机部署时 offset ≈ 0，口径与旧实现一致。
#include <boost/asio/ip/udp.hpp>
#include <boost/system/error_code.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
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
#include <unistd.h>
#include <vector>

#include "cue/common/clock.h"
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
void emit_event(std::string_view ev, const json& data) {
  nlohmann::ordered_json body{{"ev", std::string(ev)}, {"t", unix_us()}, {"data", data}};
  cue::proto::emit_event_line(body.dump());
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

struct FrameAcc {
  uint16_t count = 0;
  uint64_t pts_us = 0;
  int64_t first_seen_us = 0;
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

 private:
  static constexpr std::size_t kCap = 4096;
  std::vector<int64_t> buf_;
  std::size_t head_ = 0;
  uint64_t n_ = 0;
};

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

}  // namespace

int main(int argc, char** argv) {
  uint16_t port = 9001;
  bool nack_enabled = true;
  long long nack_delay_ms = 15;
  long long nack_interval_ms = 125;  // SPEC §7.2：限速 8 包/秒（旧默认 15 ms ≈ 66 包/秒）

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
          "cue-probe --port 9001 [--nack on|off] [--nack-delay-ms 15] [--nack-interval-ms 125]\n");
      return 0;
    } else {
      std::fprintf(stderr, "[probe] 未知参数: %.*s\n", static_cast<int>(a.size()), a.data());
      return 2;
    }
  }

  net::io_context ioc;
  boost::system::error_code ec;
  udp::socket sock(ioc);
  sock.open(udp::v4(), ec);
  sock.bind(udp::endpoint(udp::v4(), port), ec);
  if (ec) {
    std::fprintf(stderr, "[probe] bind %u failed: %s\n", port, ec.message().c_str());
    return 1;
  }
  sock.set_option(udp::socket::receive_buffer_size(4 * 1024 * 1024), ec);

  std::fprintf(stderr, "[probe] listening udp :%u\n", port);
  emit_event("probe.started",
             json{{"pid", ::getpid()}, {"port", port}, {"nack", nack_enabled ? "on" : "off"}});

  std::map<uint32_t, FrameAcc> pending;
  LatencyRing lat;
  ClockAlign clk;
  uint64_t frames = 0, lost_frames = 0, bytes = 0, packets = 0, fec_recovered = 0, nack_sent = 0;
  uint64_t rejected = 0, lat_invalid = 0, nack_pauses = 0, stats_reqs = 0;
  uint32_t max_completed = 0;
  udp::endpoint peer{};
  bool have_peer = false;
  int64_t t_last_peer_us = 0;
  uint32_t session_seen = 0;
  bool warned_clock = false;
  int64_t last_nack_us = 0;
  const int64_t nack_interval_us = nack_interval_ms * 1000;
  const int64_t nack_delay_us = nack_delay_ms * 1000;
  uint64_t frames_prev = 0;
  int64_t t_last_report = cue::now_us();
  int64_t t_last_cleanup = cue::now_us();
  int64_t t_last_window = cue::now_us();
  int64_t next_stats_us = 0;  // 首包到达后立刻问一次时钟
  int64_t nack_pause_until = 0;  // 丢包率持续过高时暂停 NACK 的截止时刻（冷却 1 s）
  LossWindow win;
  double win_loss = 0.0;
  int bad_windows = 0;

  // 帧完成结算：所有分片到齐就计入帧数与延迟样本
  auto finish_if_complete = [&](uint32_t fid, FrameAcc& fa) {
    for (const auto& pp : fa.parts) {
      if (pp.empty()) return false;
    }
    ++frames;
    // 延迟 = probe 收帧时刻（换算到 cued 时钟域）− 帧时间戳
    const int64_t lat_us = cue::now_us() + clk.offset_us - static_cast<int64_t>(fa.pts_us);
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
    if (fid > max_completed) max_completed = fid;
    settle(win, fa);
    pending.erase(fid);
    return true;
  };

  // SPEC §7.1 的时钟对齐：向 cued 发下行回环请求（载荷 = 我方发戳）
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
    sock.send_to(net::buffer(pkt), peer, 0, ec);
    ++stats_reqs;
    clk.begin_batch();  // 新一批：批内取 RTT 最小者
  };

  std::array<uint8_t, 4096> buf{};
  for (;;) {
    // ---- 周期任务放在循环顶部：FEC 分支 / 完全没包时都不会被跳过 ----
    const int64_t now_tick = cue::now_us();
    if (now_tick >= next_stats_us) {
      send_stats_request(now_tick);
      next_stats_us = now_tick + 30'000'000;  // 每 30 s 重估一次（SPEC §7.1）
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
      std::printf(
          "[probe] fps=%.1f frames=%llu lost=%llu pend=%zu pkts=%llu nack=%llu pause=%llu "
          "fec_rec=%llu rej=%llu loss=%.1f%% MB=%.2f  ",
          double(df) / dt, static_cast<unsigned long long>(frames),
          static_cast<unsigned long long>(lost_frames), pending.size(),
          static_cast<unsigned long long>(packets), static_cast<unsigned long long>(nack_sent),
          static_cast<unsigned long long>(nack_pauses), static_cast<unsigned long long>(fec_recovered),
          static_cast<unsigned long long>(rejected),
          win_loss * 100.0, double(bytes) / (1024.0 * 1024.0));
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
      // 同一份数字进事件流（M5 控制台 / agent 事件缓存用）。
      // 没有时钟样本时 offset_us 与延迟分位是 null：synced=false 时它们本来就不存在。
      const bool have_lat = clk.have && lat.size() > 0;
      emit_event("probe.metric",
                 json{{"fps", double(df) / dt},
                      {"frames", frames},
                      {"lost", lost_frames},
                      {"nack", nack_sent},
                      {"fec_rec", fec_recovered},
                      {"synced", clk.have},
                      {"lat_p50", have_lat ? json(double(lat.percentile(0.50)) / 1000.0)
                                           : json(nullptr)},
                      {"lat_p99", have_lat ? json(double(lat.percentile(0.99)) / 1000.0)
                                           : json(nullptr)},
                      {"offset_us", clk.have ? json(clk.offset_us) : json(nullptr)}});
      frames_prev = frames;
      t_last_report = now_tick;
    }

    // ---- 等一个数据报：200 ms 超时保证上面的周期任务准时跑（无流时也能报指标） ----
    pollfd pfd{sock.native_handle(), POLLIN, 0};
    const int pr = ::poll(&pfd, 1, 200);
    if (pr <= 0) continue;
    udp::endpoint from;
    const std::size_t n = sock.receive_from(net::buffer(buf), from, 0, ec);
    if (ec || n < cue::proto::kDatagramHeaderSize) continue;
    ++packets;

    // 首包锁定对端：之后其它源发来的报文不许并进重组/时钟估计（review #20）。
    // 但 cued 的源端口是 ephemeral（它不 bind），重启后必变；若原 peer 已静默 3 s 就
    // 允许重新锁定，否则一次重启会让 probe 永久拒收新 cued（AC-2 直接失败）。
    if (!have_peer) {
      peer = from;
      have_peer = true;
      t_last_peer_us = now_tick;
      next_stats_us = 0;
    } else if (from != peer) {
      if (now_tick - t_last_peer_us > 3'000'000) {
        std::fprintf(stderr,
                     "[probe] 对端切换：原 peer 已静默 %lld ms，改认 %s:%u\n",
                     static_cast<long long>((now_tick - t_last_peer_us) / 1000),
                     from.address().to_string().c_str(), from.port());
        peer = from;
        t_last_peer_us = now_tick;
        next_stats_us = 0;
        clk.begin_batch();  // 重新估一次 offset
      } else {
        ++rejected;
        continue;
      }
    } else {
      t_last_peer_us = now_tick;
    }

    auto h = cue::proto::decode_datagram({buf.data(), n});
    if (!h) continue;

    if (h->type == static_cast<uint8_t>(cue::proto::DownType::StatsReq)) {
      // 时钟对齐回包：u64 echo（我方发戳）| u64 cued_now（对端时钟）
      if (n < cue::proto::kDatagramHeaderSize + 16) continue;
      const uint8_t* pl = buf.data() + cue::proto::kDatagramHeaderSize;
      const int64_t echo = static_cast<int64_t>(cue::proto::detail::get_u64(pl));
      const int64_t cued_now = static_cast<int64_t>(cue::proto::detail::get_u64(pl + 8));
      if (clk.observe(echo, cued_now, cue::now_us())) {
        // SPEC §7.1：漂移 > 5 ms 重锚并记事件——这个"记事件"现在真的落到事件流里
        emit_event("probe.reanchor", json{{"drift_us", clk.last_drift_us},
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
           n - cue::proto::kDatagramHeaderSize});
      if (!fph) {
        ++rejected;
        continue;
      }
      const uint32_t base_fid = fph->base_frame_id;
      if (base_fid <= max_completed && pending.find(base_fid) == pending.end()) continue;
      // member_count 与数据报头里的 frag_count 双写：以头为准，对不上就拒包（review #18）
      if (fph->member_count == 0 || fph->member_count != h->frag_count) {
        ++rejected;
        continue;
      }
      auto& ffa = pending[base_fid];
      if (ffa.count == 0) {
        ffa.count = fph->member_count;
        ffa.pts_us = h->pts_us;
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
    bytes += n;
    // 迟到的重传分片不得为已完成的帧重建槽位（M0 幽灵帧教训）
    if (pending.find(h->frame_id) == pending.end() && h->frame_id <= max_completed) continue;
    // frag_count == 0 会让 parts 为空、完整性判定零次迭代 → 判为完成并污染 lat/fps
    if (h->frag_count == 0) {
      ++rejected;
      continue;
    }
    auto& fa = pending[h->frame_id];
    if (fa.count == 0) {
      fa.count = h->frag_count;
      fa.pts_us = h->pts_us;
      fa.first_seen_us = cue::now_us();
      fa.parts.resize(fa.count);
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
      for (auto& [fid, fa] : pending) {
        if (fa.count == 0 || fa.nack_queued) continue;
        if (now2 - fa.first_seen_us < nack_delay_us) continue;
        bool all_queued = true;
        for (uint16_t i = 0; i < fa.count; ++i) {
          if (!fa.parts[i].empty()) continue;
          if (entries.size() + cue::proto::kNackEntrySize >
              cue::proto::kMaxNackEntries * cue::proto::kNackEntrySize) {
            all_queued = false;  // 本帧剩余缺片进不来，下一轮再来
            break;
          }
          cue::proto::append_nack_entry(entries, cue::proto::NackEntry{fid, i});
        }
        // 只有整帧缺片都进过请求才置位；否则剩下的缺片永远不会被请求（review #8）
        if (all_queued) fa.nack_queued = true;
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
        sock.send_to(net::buffer(pkt), peer, 0, ec);
        ++nack_sent;
      }
      last_nack_us = now2;
    }
  }
}
