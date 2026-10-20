// cue-probe — 下行接收端（M1 版）
// UDP 收包 → 分片重组 → 帧完成统计（帧率、端到端延迟分位、丢帧）
// M1 不做 NACK / FEC / 抖动缓冲，只做"能看见指标"。
#include <boost/asio/ip/udp.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cue/common/clock.h"
#include "cue/proto/wire.h"

namespace net = boost::asio;
using udp = net::ip::udp;

namespace {

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

// 端到端延迟的时钟域自检：pts_us 来自浏览器按握手 offset 换算的服务端时钟，
// 而 probe 通常跨机。两条 monotonic 不同源时算出来的差值是任意数（负值或巨大），
// 打印出来的分位毫无意义。超出合理区间就判为「时钟未对齐」，报告里明确标注。
inline bool lat_plausible(int64_t us) { return us > -1'000'000 && us < 10'000'000; }

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
  int nack_delay_ms = 15;
  int nack_interval_ms = 125;  // SPEC §7.2：限速 8 包/秒（旧默认 15 ms ≈ 66 包/秒）
  for (int i = 1; i < argc; ++i) {
    std::string_view a = argv[i];
    auto next = [&]() -> std::string {
      return (i + 1 < argc) ? std::string(argv[++i]) : std::string();
    };
    if (a == "--port") port = static_cast<uint16_t>(std::stoi(next()));
    else if (a == "--nack") nack_enabled = (next() == "on");
    else if (a == "--nack-delay-ms") nack_delay_ms = std::stoi(next());
    else if (a == "--nack-interval-ms") nack_interval_ms = std::stoi(next());
    else if (a == "--help") {
      std::printf("cue-probe --port 9001\n");
      return 0;
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

  std::map<uint32_t, FrameAcc> pending;
  LatencyRing lat;
  uint64_t frames = 0, lost_frames = 0, bytes = 0, packets = 0, fec_recovered = 0, nack_sent = 0;
  uint64_t rejected = 0, lat_invalid = 0, nack_pauses = 0;
  uint32_t max_completed = 0;
  udp::endpoint peer{};
  bool have_peer = false;
  bool warned_clock = false;
  int64_t last_nack_us = 0;
  const int64_t nack_interval_us = int64_t(nack_interval_ms) * 1000;
  const int64_t nack_delay_us = int64_t(nack_delay_ms) * 1000;
  uint64_t frames_prev = 0;
  int64_t t_last_report = cue::now_us();
  int64_t t_last_cleanup = cue::now_us();
  int64_t t_last_window = cue::now_us();
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
    const int64_t lat_us = cue::now_us() - static_cast<int64_t>(fa.pts_us);
    if (lat_plausible(lat_us)) {
      lat.push(lat_us);
    } else {
      ++lat_invalid;
      if (!warned_clock) {
        std::fprintf(stderr,
                     "[probe] 延迟样本落在时钟域外（%lld us）：probe 与 cued 不同机/未对齐时钟，"
                     "延迟分位不会输出。跨机部署请先做时钟对齐，或同机运行 probe。\n",
                     static_cast<long long>(lat_us));
        warned_clock = true;
      }
    }
    if (fid > max_completed) max_completed = fid;
    settle(win, fa);
    pending.erase(fid);
    return true;
  };

  std::array<uint8_t, 4096> buf{};
  for (;;) {
    udp::endpoint from;
    const std::size_t n = sock.receive_from(net::buffer(buf), from, 0, ec);
    if (ec || n < cue::proto::kDatagramHeaderSize) continue;
    ++packets;

    auto h = cue::proto::decode_datagram({buf.data(), n});
    if (!h) continue;
    if (!have_peer) {
      peer = from;
      have_peer = true;
    }
    if (h->type == static_cast<uint8_t>(cue::proto::DownType::Fec)) {
      // payload: base_frame_id(4) | group_index(2) | member_count(2) | parity
      if (n < cue::proto::kDatagramHeaderSize + 8) continue;
      const uint8_t* pl = buf.data() + cue::proto::kDatagramHeaderSize;
      uint32_t base_fid = 0;
      uint16_t member_count = 0;
      std::memcpy(&base_fid, pl, 4);
      std::memcpy(&member_count, pl + 6, 2);
      if (base_fid <= max_completed && pending.find(base_fid) == pending.end()) continue;
      // member_count == 0 会让 parts 为空、完整性循环零次迭代 → 凭空推一帧（review #19）
      if (member_count == 0) {
        ++rejected;
        continue;
      }
      auto& ffa = pending[base_fid];
      if (ffa.count == 0) {
        ffa.count = member_count;
        ffa.pts_us = h->pts_us;
        ffa.first_seen_us = cue::now_us();
        ffa.parts.resize(member_count);
      }
      const uint8_t* base_ptr = buf.data();
      ffa.parity.assign(base_ptr + cue::proto::kDatagramHeaderSize + 8, base_ptr + n);
      ffa.fec_available = true;
      try_fec_recover(ffa, fec_recovered);
      // 恢复后必须重新判定完整性：否则被 FEC 修好的帧永远不会被计为完成（实测踩到的坑）
      finish_if_complete(base_fid, ffa);
      // FEC 分支不得 continue 跳过周期清理（review #7）：清理已统一放到循环尾部的 tick
      continue;
    }
    if (h->type != static_cast<uint8_t>(cue::proto::DownType::Video)) continue;

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
      for (auto& [fid, fa] : pending) {
        if (fa.count == 0 || fa.nack_queued) continue;
        if (now2 - fa.first_seen_us < nack_delay_us) continue;
        bool all_queued = true;
        for (uint16_t i = 0; i < fa.count; ++i) {
          if (!fa.parts[i].empty()) continue;
          if (entries.size() + 6 > 64 * 6) {  // 本帧剩余缺片进不来，下一轮再来
            all_queued = false;
            break;
          }
          uint32_t f = fid;
          uint16_t idx = i;
          entries.insert(entries.end(), reinterpret_cast<uint8_t*>(&f), reinterpret_cast<uint8_t*>(&f) + 4);
          entries.insert(entries.end(), reinterpret_cast<uint8_t*>(&idx), reinterpret_cast<uint8_t*>(&idx) + 2);
        }
        // 只有整帧缺片都进过请求才置位；否则剩下的缺片永远不会被请求（review #8）
        if (all_queued) fa.nack_queued = true;
        if (entries.size() + 6 > 64 * 6) break;
      }
      if (!entries.empty()) {
        cue::proto::DatagramHeader nh;
        nh.type = static_cast<uint8_t>(cue::proto::DownType::Nack);
        nh.session_id = 1;
        auto hdr = cue::proto::encode(nh);
        std::vector<uint8_t> pkt(hdr.begin(), hdr.end());
        pkt.insert(pkt.end(), entries.begin(), entries.end());
        sock.send_to(net::buffer(pkt), peer, 0, ec);
        ++nack_sent;
      }
      last_nack_us = now2;
    }

    // ---- 周期任务：与「帧是否完成」完全解耦（review #7） ----
    const int64_t now = cue::now_us();
    if (now - t_last_cleanup >= 500'000) {
      cleanup_stale(pending, now, lost_frames, win);
      t_last_cleanup = now;
    }
    if (now - t_last_window >= 1'000'000) {
      win_loss = win.expected ? double(win.expected - win.received) / double(win.expected) : 0.0;
      if (win.expected > 0 && win_loss > 0.10) {
        ++bad_windows;
        if (bad_windows >= 3) {
          nack_pause_until = now + 1'000'000;
          bad_windows = 0;
          ++nack_pauses;
          std::fprintf(stderr, "[probe] loss=%.1f%% 连续 3 窗口 >10%%：暂停 NACK 1 s\n",
                       win_loss * 100.0);
        }
      } else if (win.expected > 0) {
        bad_windows = 0;
      }
      win = LossWindow{};
      t_last_window = now;
    }
    if (now - t_last_report >= 1'000'000) {
      const double dt = double(now - t_last_report) / 1e6;
      const uint64_t df = frames - frames_prev;
      std::printf(
          "[probe] fps=%.1f frames=%llu lost=%llu pend=%zu pkts=%llu nack=%llu pause=%llu "
          "fec_rec=%llu loss=%.1f%% MB=%.2f  ",
          double(df) / dt, static_cast<unsigned long long>(frames),
          static_cast<unsigned long long>(lost_frames), pending.size(),
          static_cast<unsigned long long>(packets), static_cast<unsigned long long>(nack_sent),
          static_cast<unsigned long long>(nack_pauses), static_cast<unsigned long long>(fec_recovered),
          win_loss * 100.0, double(bytes) / (1024.0 * 1024.0));
      if (lat.size() == 0) {
        std::printf("lat=unsynced(invalid=%llu)\n", static_cast<unsigned long long>(lat_invalid));
      } else {
        std::printf("lat p50=%.2fms p95=%.2fms p99=%.2fms max=%.2fms (n=%zu)\n",
                    double(lat.percentile(0.50)) / 1000.0, double(lat.percentile(0.95)) / 1000.0,
                    double(lat.percentile(0.99)) / 1000.0,
                    double(lat.size() ? lat.percentile(1.0) : 0) / 1000.0, lat.size());
      }
      std::fflush(stdout);
      frames_prev = frames;
      t_last_report = now;
    }
  }
}
