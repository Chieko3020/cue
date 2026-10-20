// Spike-C：UDP 接收端，负责分片重组、NACK 请求、XOR-FEC 修复与统计。
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "../common/bench_util.h"
#include "proto.h"

using namespace cue_spike;
using namespace cue_spike::proto;

namespace {

struct Config {
  uint16_t port{9300};
  int duration_s{30};
  bool nack{true};
  int nack_delay_ms{15};       // 发现缺片后等多久才发 NACK（留出乱序余量）
  int nack_interval_ms{15};    // NACK 发送最小间隔（SPEC 生产值 125ms ≈ 8 包/s）
  int frame_deadline_ms{120};  // 超过这个延迟的帧算「不可用」
  std::string out{"receiver.jsonl"};
  std::string label{"run"};
};

struct PendingFrame {
  uint16_t frag_count{0};
  uint32_t frame_bytes{0};
  uint16_t frag_payload{0};
  std::vector<uint8_t> data;
  std::vector<uint8_t> have;
  int received{0};
  bool fec_available{false};
  std::vector<uint8_t> parity;
  int64_t pts_us{0};
  int64_t first_seen_us{0};
  bool nack_queued{false};
  int nack_requests{0};
};

struct Counters {
  uint64_t frames_completed{0};
  uint64_t frames_failed{0};
  uint64_t frames_no_data{0};
  uint64_t dup_frags{0};
  uint64_t retransmit_frags{0};
  uint64_t fec_frags{0};
  uint64_t fec_recovered_frags{0};
  uint64_t nack_entries{0};
  uint64_t nack_packets{0};
  uint64_t bytes{0};
  Samples latency;
};

std::expected<int, std::string> make_socket(uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return std::unexpected(std::string("socket: ") + std::strerror(errno));
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  int rcvbuf = 8 * 1024 * 1024;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  timeval tv{0, 1000};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return std::unexpected(std::string("bind: ") + std::strerror(errno));
  }
  return fd;
}

void send_nack(int fd, const sockaddr_in& peer, uint32_t session_id,
               const std::vector<NackEntry>& entries, Counters& c) {
  std::size_t off = 0;
  while (off < entries.size()) {
    const std::size_t cnt = std::min<std::size_t>(64, entries.size() - off);
    uint8_t buf[24 + 64 * sizeof(NackEntry)];
    Header h{};
    h.magic = kMagicDown;
    h.type = static_cast<uint8_t>(Type::kNack);
    h.session_id = session_id;
    h.frame_id = 0;
    h.pts_us = static_cast<uint64_t>(now_ns() / 1000);
    std::memcpy(buf, &h, sizeof(h));
    std::memcpy(buf + sizeof(h), entries.data() + off, cnt * sizeof(NackEntry));
    const ssize_t sent = ::sendto(fd, buf, sizeof(h) + cnt * sizeof(NackEntry), 0,
                                  reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
    if (sent > 0) {
      c.nack_entries += cnt;
      c.nack_packets += 1;
    }
    off += cnt;
  }
}

uint32_t hash_bytes(const uint8_t* p, uint32_t n) {
  uint32_t h = 2166136261u;
  for (uint32_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

// 有冗余包且恰好缺 1 片时用 XOR 修复。
void try_fec_repair(PendingFrame& f, Counters& c) {
  if (!f.fec_available || f.frag_count == 0) return;
  if (f.received != int(f.frag_count) - 1) return;
  uint16_t missing = 0;
  for (uint16_t k = 0; k < f.frag_count; ++k) {
    if (!f.have[k]) {
      missing = k;
      break;
    }
  }
  const std::size_t plen = std::min<std::size_t>(kMaxPayload, f.parity.size());
  if (plen == 0) return;
  std::vector<uint8_t> rec(plen, 0);
  std::memcpy(rec.data(), f.parity.data(), plen);
  for (uint16_t k = 0; k < f.frag_count; ++k) {
    if (k == missing) continue;
    const std::size_t off = std::size_t(k) * f.frag_payload;
    for (std::size_t b = 0; b < plen; ++b) rec[b] ^= f.data[off + b];
  }
  const std::size_t off = std::size_t(missing) * f.frag_payload;
  std::memcpy(f.data.data() + off, rec.data(), plen);
  f.have[missing] = 1;
  f.received++;
  c.fec_recovered_frags++;
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  for (int i = 1; i < argc; ++i) {
    std::string_view a = argv[i];
    auto next = [&]() -> std::string { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(); };
    if (a == "--port") cfg.port = static_cast<uint16_t>(std::stoi(next()));
    else if (a == "--duration") cfg.duration_s = std::stoi(next());
    else if (a == "--nack") cfg.nack = next() == "on";
    else if (a == "--nack-delay-ms") cfg.nack_delay_ms = std::stoi(next());
    else if (a == "--nack-interval-ms") cfg.nack_interval_ms = std::stoi(next());
    else if (a == "--frame-deadline-ms") cfg.frame_deadline_ms = std::stoi(next());
    else if (a == "--out") cfg.out = next();
    else if (a == "--label") cfg.label = next();
    else {
      std::fprintf(stderr, "unknown arg %s\n", argv[i]);
      return 2;
    }
  }

  auto sock = make_socket(cfg.port);
  if (!sock) {
    std::fprintf(stderr, "%s\n", sock.error().c_str());
    return 1;
  }
  const int fd = *sock;

  std::map<uint32_t, PendingFrame> pending;
  Counters c;
  sockaddr_in peer{};
  socklen_t peer_len = sizeof(peer);
  bool have_peer = false;
  uint32_t session_id = 0;
  std::vector<NackEntry> nack_queue;
  int64_t last_nack_ns = 0;
  const int64_t t_end = now_ns() + int64_t(cfg.duration_s) * 1'000'000'000LL;
  const int64_t deadline_us = int64_t(cfg.frame_deadline_ms) * 1000;
  const int64_t nack_interval_ns = int64_t(cfg.nack_interval_ms) * 1'000'000;

  auto ensure_frame = [](PendingFrame& f, const Header& h, int64_t now_us) {
    if (f.frag_count != 0) return;
    f.frag_count = h.frag_count;
    f.frag_payload = static_cast<uint16_t>(kMaxPayload);
    f.frame_bytes = uint32_t(h.frag_count) * f.frag_payload;
    f.data.assign(f.frame_bytes, 0);
    f.have.assign(h.frag_count, 0);
    f.pts_us = static_cast<int64_t>(h.pts_us);
    f.first_seen_us = now_us;
  };

  uint8_t rbuf[4096];
  while (now_ns() < t_end) {
    sockaddr_in from{};
    socklen_t from_len = sizeof(from);
    const ssize_t n = ::recvfrom(fd, rbuf, sizeof(rbuf), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
    const int64_t now = now_ns();
    const int64_t now_us = now / 1000;
    if (n >= static_cast<ssize_t>(sizeof(Header))) {
      Header h{};
      std::memcpy(&h, rbuf, sizeof(h));
      if (h.magic != kMagicDown) continue;
      const auto type = static_cast<Type>(h.type);
      if (type == Type::kVideo || type == Type::kFec) {
        session_id = h.session_id;
        if (!have_peer) {
          peer = from;
          peer_len = from_len;
          have_peer = true;
        }
        // 已过期的帧槽位不因迟到重传/FEC 包而重建：
        //  · 重传包的 pts 是重传时刻，会污染延迟统计；
        //  · FEC 包通常在整帧数据之后到达，若此时该帧已判定完成并 erase，
        //    重建槽位会造出"幽灵帧"（零丢包时 no_data 正好等于 FEC 包数）。
        const bool late_capable = (h.flags & kFlagRetransmit) != 0 || type == Type::kFec;
        if (late_capable && pending.find(h.frame_id) == pending.end()) {
          continue;
        }
        auto& f = pending[h.frame_id];
        ensure_frame(f, h, now_us);
        if (type == Type::kVideo) {
          if (h.frag_index < f.frag_count) {
            if (f.have[h.frag_index]) {
              c.dup_frags++;
            } else {
              const std::size_t plen = std::min<std::size_t>(kMaxPayload, n - sizeof(Header));
              const std::size_t off = std::size_t(h.frag_index) * f.frag_payload;
              std::memcpy(f.data.data() + off, rbuf + sizeof(Header), plen);
              f.have[h.frag_index] = 1;
              f.received++;
            }
            if (h.flags & kFlagRetransmit) c.retransmit_frags++;
          }
          try_fec_repair(f, c);
        } else if (n - static_cast<ssize_t>(sizeof(Header)) >= static_cast<ssize_t>(sizeof(FecPayload))) {
          const std::size_t plen = n - sizeof(Header) - sizeof(FecPayload);
          if (!f.fec_available && plen <= kMaxPayload && plen > 0) {
            f.parity.assign(plen, 0);
            std::memcpy(f.parity.data(), rbuf + sizeof(Header) + sizeof(FecPayload), plen);
            f.fec_available = true;
            c.fec_frags++;
            try_fec_repair(f, c);
          }
        }
        c.bytes += uint64_t(n);
      }
    }

    // 帧完成/超期判定
    for (auto it = pending.begin(); it != pending.end();) {
      auto& f = it->second;
      const bool complete = f.frag_count > 0 && f.received == int(f.frag_count);
      const bool expired = (now_us - f.pts_us) > deadline_us;
      if (complete) {
        const uint32_t want = frame_hash(it->first, f.frame_bytes);
        const uint32_t got = hash_bytes(f.data.data(), f.frame_bytes);
        if (want == got) {
          c.frames_completed++;
          c.latency.add(now_us - f.pts_us);
        } else {
          c.frames_failed++;
          std::fprintf(stderr, "hash mismatch frame %u bytes=%u\n", it->first, f.frame_bytes);
        }
        it = pending.erase(it);
      } else if (expired) {
        if (f.received > 0) {
          c.frames_failed++;
        } else {
          c.frames_no_data++;
        }
        it = pending.erase(it);
      } else {
        ++it;
      }
    }

    // NACK 生成（限速为每 nack_interval 一个包，单包 ≤64 条）
    if (cfg.nack && have_peer && now - last_nack_ns >= nack_interval_ns) {
      nack_queue.clear();
      for (auto& [fid, f] : pending) {
        if (f.frag_count == 0 || f.nack_queued) continue;
        if (f.received == int(f.frag_count)) continue;
        if (now_us - f.first_seen_us < int64_t(cfg.nack_delay_ms) * 1000) continue;
        for (uint16_t k = 0; k < f.frag_count; ++k) {
          if (!f.have[k]) {
            nack_queue.push_back(NackEntry{fid, k});
            f.nack_requests++;
            if (nack_queue.size() >= 64) break;
          }
        }
        f.nack_queued = true;
        if (nack_queue.size() >= 64) break;
      }
      if (!nack_queue.empty()) {
        send_nack(fd, peer, session_id, nack_queue, c);
        last_nack_ns = now;
      }
    }
  }

  for (auto& [fid, f] : pending) {
    (void)fid;
    if (f.received > 0) c.frames_failed++;
    else c.frames_no_data++;
  }

  const uint64_t usable = c.frames_completed;
  const double dur = static_cast<double>(cfg.duration_s);
  nlohmann::json j = {
      {"label", cfg.label},
      {"side", "receiver"},
      {"nack", cfg.nack},
      {"nack_interval_ms", cfg.nack_interval_ms},
      {"nack_delay_ms", cfg.nack_delay_ms},
      {"frame_deadline_ms", cfg.frame_deadline_ms},
      {"duration_s", cfg.duration_s},
      {"frames_completed", c.frames_completed},
      {"frames_failed", c.frames_failed},
      {"frames_no_data", c.frames_no_data},
      {"usable_fps", static_cast<double>(usable) / dur},
      {"usable_ratio", static_cast<double>(usable) / static_cast<double>(std::max<uint64_t>(1, usable + c.frames_failed + c.frames_no_data))},
      {"dup_frags", c.dup_frags},
      {"retransmit_frags", c.retransmit_frags},
      {"fec_frags", c.fec_frags},
      {"fec_recovered_frags", c.fec_recovered_frags},
      {"nack_entries", c.nack_entries},
      {"nack_packets", c.nack_packets},
      {"lat_p50_ms", static_cast<double>(c.latency.pct(0.50)) / 1000.0},
      {"lat_p95_ms", static_cast<double>(c.latency.pct(0.95)) / 1000.0},
      {"lat_p99_ms", static_cast<double>(c.latency.pct(0.99)) / 1000.0},
      {"lat_max_ms", static_cast<double>(c.latency.max()) / 1000.0},
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
