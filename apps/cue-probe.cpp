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
};

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
  for (int i = 1; i < argc; ++i) {
    std::string_view a = argv[i];
    auto next = [&]() -> std::string {
      return (i + 1 < argc) ? std::string(argv[++i]) : std::string();
    };
    if (a == "--port") port = static_cast<uint16_t>(std::stoi(next()));
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
  uint64_t frames = 0, lost_frames = 0, bytes = 0, packets = 0;
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
    if (h->type != static_cast<uint8_t>(cue::proto::DownType::Video)) continue;

    bytes += n;
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
      pending.erase(h->frame_id);
    }

    if (pending.size() > 256) cleanup_stale(pending, cue::now_us(), lost_frames);

    const int64_t now = cue::now_us();
    if (now - t_last_report >= 1'000'000) {
      const double dt = double(now - t_last_report) / 1e6;
      const uint64_t df = frames - frames_prev;
      std::printf(
          "[probe] fps=%.1f frames=%llu lost=%llu pkts=%llu MB=%.2f  "
          "lat p50=%.2fms p95=%.2fms p99=%.2fms max=%.2fms (n=%zu)\n",
          double(df) / dt, static_cast<unsigned long long>(frames),
          static_cast<unsigned long long>(lost_frames), static_cast<unsigned long long>(packets),
          double(bytes) / (1024.0 * 1024.0), double(lat.percentile(0.50)) / 1000.0,
          double(lat.percentile(0.95)) / 1000.0, double(lat.percentile(0.99)) / 1000.0,
          double(lat.size() ? lat.percentile(1.0) : 0) / 1000.0, lat.size());
      std::fflush(stdout);
      frames_prev = frames;
      t_last_report = now;
    }
  }
}
