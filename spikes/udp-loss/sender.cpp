// Spike-C：UDP 发送端，负责分片、XOR-FEC、用户态丢包注入与 NACK 重传应答。
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "../common/bench_util.h"
#include "proto.h"

using namespace cue_spike;
using namespace cue_spike::proto;

namespace {

struct Config {
  std::string host{"127.0.0.1"};
  uint16_t port{9300};
  int fps{30};
  int duration_s{30};
  double loss{0.05};
  uint64_t seed{42};
  bool fec{true};
  uint16_t frag_count{8};
  std::string out{"sender.jsonl"};
  std::string label{"run"};
  std::string trace;  // 打开则记录被注入丢弃的 (frame,frag)
};

struct Counters {
  std::atomic<uint64_t> packets_attempted{0};
  std::atomic<uint64_t> packets_dropped{0};
  std::atomic<uint64_t> packets_sent{0};
  std::atomic<uint64_t> retransmit_attempted{0};
  std::atomic<uint64_t> retransmit_sent{0};
  std::atomic<uint64_t> retransmit_dropped{0};
  std::atomic<uint64_t> bytes_sent{0};
  std::atomic<uint64_t> nack_received{0};
  std::atomic<uint64_t> nack_entries{0};
};

std::expected<int, std::string> make_socket(const Config& cfg, sockaddr_in& peer) {
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return std::unexpected(std::string("socket: ") + std::strerror(errno));
  int sndbuf = 4 * 1024 * 1024;
  ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
  timeval tv{0, 5000};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  peer = {};
  peer.sin_family = AF_INET;
  peer.sin_port = htons(cfg.port);
  if (::inet_pton(AF_INET, cfg.host.c_str(), &peer.sin_addr) != 1) {
    ::close(fd);
    return std::unexpected("bad host: " + cfg.host);
  }
  if (::connect(fd, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) < 0) {
    ::close(fd);
    return std::unexpected(std::string("connect: ") + std::strerror(errno));
  }
  return fd;
}

// 一趟帧：数据分片 + 可选 XOR 冗余。分片定长 kMaxPayload，最后一片也补齐，
// 使接收端可以按 frag_index * kMaxPayload 直接定位。
void build_frame(uint32_t frame_id, uint16_t frag_count, std::vector<std::vector<uint8_t>>& out,
                 std::vector<uint8_t>* parity, uint32_t session_id, uint64_t pts_us) {
  out.assign(frag_count, std::vector<uint8_t>(24 + kMaxPayload, 0));
  for (uint16_t k = 0; k < frag_count; ++k) {
    Header h{};
    h.magic = kMagicDown;
    h.type = static_cast<uint8_t>(Type::kVideo);
    h.flags = (frame_id % 30 == 0) ? kFlagKeyframe : 0;
    h.session_id = session_id;
    h.frame_id = frame_id;
    h.frag_index = k;
    h.frag_count = frag_count;
    h.pts_us = pts_us;
    std::memcpy(out[k].data(), &h, sizeof(h));
    for (uint16_t b = 0; b < kMaxPayload; ++b) {
      out[k][24 + b] = payload_byte(frame_id, uint32_t(k) * kMaxPayload + b);
    }
  }
  if (parity != nullptr) {
    parity->assign(24 + sizeof(FecPayload) + kMaxPayload, 0);
    Header h{};
    h.magic = kMagicDown;
    h.type = static_cast<uint8_t>(Type::kFec);
    h.flags = 0;
    h.session_id = session_id;
    h.frame_id = frame_id;
    h.frag_index = 0;
    h.frag_count = frag_count;
    h.pts_us = pts_us;
    FecPayload fp{};
    fp.base_frame_id = frame_id;
    fp.group_index = 0;
    fp.member_count = frag_count;
    std::memcpy(parity->data(), &h, sizeof(h));
    std::memcpy(parity->data() + sizeof(h), &fp, sizeof(fp));
    for (uint16_t k = 0; k < frag_count; ++k) {
      for (uint16_t b = 0; b < kMaxPayload; ++b) {
        (*parity)[24 + sizeof(FecPayload) + b] ^= payload_byte(frame_id, uint32_t(k) * kMaxPayload + b);
      }
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  for (int i = 1; i < argc; ++i) {
    std::string_view a = argv[i];
    auto next = [&]() -> std::string { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(); };
    if (a == "--host") cfg.host = next();
    else if (a == "--port") cfg.port = static_cast<uint16_t>(std::stoi(next()));
    else if (a == "--fps") cfg.fps = std::stoi(next());
    else if (a == "--duration") cfg.duration_s = std::stoi(next());
    else if (a == "--loss") cfg.loss = std::stod(next());
    else if (a == "--seed") cfg.seed = std::stoull(next());
    else if (a == "--fec") cfg.fec = next() == "on";
    else if (a == "--frag-count") cfg.frag_count = static_cast<uint16_t>(std::stoi(next()));
    else if (a == "--out") cfg.out = next();
    else if (a == "--label") cfg.label = next();
    else if (a == "--trace") cfg.trace = next();
    else {
      std::fprintf(stderr, "unknown arg %s\n", argv[i]);
      return 2;
    }
  }

  sockaddr_in peer{};
  auto sock = make_socket(cfg, peer);
  if (!sock) {
    std::fprintf(stderr, "%s\n", sock.error().c_str());
    return 1;
  }
  const int fd = *sock;
  const uint32_t session_id = 0x00C0FFEEu;
  Counters c;

  std::ofstream trace;
  std::mutex trace_mu;
  if (!cfg.trace.empty()) {
    trace.open(cfg.trace, std::ios::trunc);
    trace << "# frame_id frag_index kind\n";
  }

  std::atomic<bool> stop{false};

  // 重传线程：应答 NACK。使用独立随机流，保证可复现。
  std::thread rexmit([&] {
    std::mt19937_64 rng(cfg.seed ^ 0x9E3779B97F4A7C15ull);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    std::vector<std::vector<uint8_t>> frags;
    uint8_t buf[4096];
    int64_t last_frame = -1;
    while (!stop.load(std::memory_order_relaxed)) {
      const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
      if (n < static_cast<ssize_t>(sizeof(Header))) continue;
      Header h{};
      std::memcpy(&h, buf, sizeof(h));
      if (h.magic != kMagicDown || h.type != static_cast<uint8_t>(Type::kNack)) continue;
      c.nack_received.fetch_add(1);
      const std::size_t cnt = (n - sizeof(Header)) / sizeof(NackEntry);
      std::vector<NackEntry> entries(cnt);
      std::memcpy(entries.data(), buf + sizeof(Header), cnt * sizeof(NackEntry));
      c.nack_entries.fetch_add(cnt);
      for (const auto& e : entries) {
        if (static_cast<int64_t>(e.frame_id) != last_frame) {
          // 重传包携带「重传时刻」作为 pts；接收端若已无该帧槽位会丢弃迟到重传
          build_frame(e.frame_id, cfg.frag_count, frags, nullptr, session_id,
                      static_cast<uint64_t>(now_ns() / 1000));
          last_frame = static_cast<int64_t>(e.frame_id);
        }
        if (e.frag_index >= frags.size()) continue;
        c.retransmit_attempted.fetch_add(1);
        if (uni(rng) < cfg.loss) {
          c.retransmit_dropped.fetch_add(1);
          continue;
        }
        Header rh{};
        std::memcpy(&rh, frags[e.frag_index].data(), sizeof(rh));
        rh.flags |= kFlagRetransmit;
        std::memcpy(frags[e.frag_index].data(), &rh, sizeof(rh));
        const ssize_t s = ::send(fd, frags[e.frag_index].data(), frags[e.frag_index].size(), 0);
        if (s > 0) {
          c.retransmit_sent.fetch_add(1);
          c.bytes_sent.fetch_add(uint64_t(s));
        }
      }
    }
  });

  // 主发送循环：绝对时钟，fps 精确
  const int64_t start = now_ns();
  const int64_t period_ns = 1'000'000'000LL / cfg.fps;
  const int64_t total = int64_t(cfg.fps) * cfg.duration_s;
  std::mt19937_64 rng(cfg.seed);
  std::uniform_real_distribution<double> uni(0.0, 1.0);
  std::vector<std::vector<uint8_t>> frags;
  std::vector<uint8_t> parity;
  uint64_t frames_sent = 0;

  for (int64_t i = 0; i < total; ++i) {
    sleep_until_ns(start + (i + 1) * period_ns);
    const int64_t now = now_ns();
    const uint64_t pts_us = static_cast<uint64_t>(now / 1000);
    build_frame(static_cast<uint32_t>(i), cfg.frag_count, frags, cfg.fec ? &parity : nullptr, session_id, pts_us);
    auto emit = [&](const std::vector<uint8_t>& pkt, const char* kind) {
      c.packets_attempted.fetch_add(1);
      if (cfg.loss > 0.0 && uni(rng) < cfg.loss) {
        c.packets_dropped.fetch_add(1);
        if (trace.is_open()) {
          std::lock_guard lk(trace_mu);
          Header h{};
          std::memcpy(&h, pkt.data(), sizeof(h));
          trace << h.frame_id << ' ' << h.frag_index << ' ' << kind << '\n';
        }
        return;
      }
      const ssize_t s = ::send(fd, pkt.data(), pkt.size(), 0);
      if (s > 0) {
        c.packets_sent.fetch_add(1);
        c.bytes_sent.fetch_add(uint64_t(s));
      }
    };
    for (const auto& f : frags) emit(f, "data");
    if (cfg.fec) emit(parity, "fec");
    frames_sent++;
  }
  stop.store(true);
  ::shutdown(fd, SHUT_RDWR);
  rexmit.join();

  nlohmann::json j = {
      {"label", cfg.label},
      {"side", "sender"},
      {"fps", cfg.fps},
      {"duration_s", cfg.duration_s},
      {"loss", cfg.loss},
      {"seed", cfg.seed},
      {"fec", cfg.fec},
      {"frag_count", cfg.frag_count},
      {"frag_payload", kMaxPayload},
      {"frame_bytes", uint32_t(cfg.frag_count) * kMaxPayload},
      {"frames_sent", frames_sent},
      {"packets_attempted", c.packets_attempted.load()},
      {"packets_dropped", c.packets_dropped.load()},
      {"packets_sent", c.packets_sent.load()},
      {"retransmit_attempted", c.retransmit_attempted.load()},
      {"retransmit_sent", c.retransmit_sent.load()},
      {"retransmit_dropped", c.retransmit_dropped.load()},
      {"nack_packets_received", c.nack_received.load()},
      {"nack_entries", c.nack_entries.load()},
      {"bytes_sent", c.bytes_sent.load()},
      {"rss_kb", rss_kb()},
  };
  std::ofstream out(cfg.out, std::ios::app);
  const std::string line = j.dump();
  out << line << '\n';
  out.flush();
  std::fprintf(stdout, "%s\n", line.c_str());
  ::close(fd);
  return 0;
}
