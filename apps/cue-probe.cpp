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
  std::vector<std::vector<uint8_t>> parts;
  std::vector<uint8_t> parity;
  bool fec_available = false;
  bool nack_queued = false;
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

void cleanup_stale(std::map<uint32_t, FrameAcc>& pending, int64_t now_us, uint64_t& lost) {
  for (auto it = pending.begin(); it != pending.end();) {
    if (now_us - it->second.first_seen_us > 2'000'000) {
      ++lost;
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
  int nack_interval_ms = 15;
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
  uint32_t max_completed = 0;
  udp::endpoint peer{};
  bool have_peer = false;
  int64_t last_nack_us = 0;
  const int64_t nack_interval_us = int64_t(nack_interval_ms) * 1000;
  const int64_t nack_delay_us = int64_t(nack_delay_ms) * 1000;
  uint64_t frames_prev = 0;
  int64_t t_last_report = cue::now_us();

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
      {
        bool fec_complete = true;
        for (const auto& pp : ffa.parts) {
          if (pp.empty()) {
            fec_complete = false;
            break;
          }
        }
        if (fec_complete) {
          ++frames;
          lat.push(cue::now_us() - static_cast<int64_t>(ffa.pts_us));
          if (base_fid > max_completed) max_completed = base_fid;
          pending.erase(base_fid);
        }
      }
      continue;
    }
    if (h->type != static_cast<uint8_t>(cue::proto::DownType::Video)) continue;

    bytes += n;
    // 迟到的重传分片不得为已完成的帧重建槽位（M0 幽灵帧教训）
    if (pending.find(h->frame_id) == pending.end() && h->frame_id <= max_completed) continue;
    auto& fa = pending[h->frame_id];
    if (fa.count == 0) {
      fa.count = h->frag_count;
      fa.pts_us = h->pts_us;
      fa.first_seen_us = cue::now_us();
      fa.parts.resize(fa.count);
    }
    if (h->frag_index < fa.count) {
      fa.parts[h->frag_index].assign(buf.data() + cue::proto::kDatagramHeaderSize, buf.data() + n);
    }

    bool complete = true;
    for (const auto& p : fa.parts) {
      if (p.empty()) {
        complete = false;
        break;
      }
    }
    if (complete) {
      ++frames;
      lat.push(cue::now_us() - static_cast<int64_t>(fa.pts_us));
      if (h->frame_id > max_completed) max_completed = h->frame_id;
      pending.erase(h->frame_id);
    }

    if (pending.size() > 256) cleanup_stale(pending, cue::now_us(), lost_frames);

    // NACK：对缺片且等待超过 nack_delay_ms 的帧请求重传（限速 nack_interval_ms 一包、单包 ≤64 条）
    const int64_t now2 = cue::now_us();
    if (nack_enabled && have_peer && now2 - last_nack_us >= nack_interval_us) {
      std::vector<uint8_t> entries;
      for (auto& [fid, fa] : pending) {
        if (fa.count == 0 || fa.nack_queued) continue;
        if (now2 - fa.first_seen_us < nack_delay_us) continue;
        for (uint16_t i = 0; i < fa.count; ++i) {
          if (!fa.parts[i].empty()) continue;
          uint32_t f = fid;
          uint16_t idx = i;
          entries.insert(entries.end(), reinterpret_cast<uint8_t*>(&f), reinterpret_cast<uint8_t*>(&f) + 4);
          entries.insert(entries.end(), reinterpret_cast<uint8_t*>(&idx), reinterpret_cast<uint8_t*>(&idx) + 2);
          if (entries.size() >= 64 * 6) break;
        }
        fa.nack_queued = true;
        if (entries.size() >= 64 * 6) break;
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

    const int64_t now = cue::now_us();
    if (now - t_last_report >= 1'000'000) {
      const double dt = double(now - t_last_report) / 1e6;
      const uint64_t df = frames - frames_prev;
      std::printf(
          "[probe] fps=%.1f frames=%llu lost=%llu pkts=%llu nack=%llu fec_rec=%llu MB=%.2f  "
          "lat p50=%.2fms p95=%.2fms p99=%.2fms max=%.2fms (n=%zu)\n",
          double(df) / dt, static_cast<unsigned long long>(frames),
          static_cast<unsigned long long>(lost_frames), static_cast<unsigned long long>(packets),
          static_cast<unsigned long long>(nack_sent), static_cast<unsigned long long>(fec_recovered),
          double(bytes) / (1024.0 * 1024.0), double(lat.percentile(0.50)) / 1000.0,
          double(lat.percentile(0.95)) / 1000.0, double(lat.percentile(0.99)) / 1000.0,
          double(lat.size() ? lat.percentile(1.0) : 0) / 1000.0, lat.size());
      std::fflush(stdout);
      frames_prev = frames;
      t_last_report = now;
    }
  }
}
