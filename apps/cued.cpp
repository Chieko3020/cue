// cued — 媒体管线（M2a：解码 + 六段埋点，转码仍为 passthrough）
// 浏览器 --WS--> cued --UDP--> cue-probe
//
// 六段埋点（SPEC §5.3）：
//   recv   首片到达 → 本帧重组完成（帧内首片到齐时间）
//   buffer 帧重组耗时（M2a 与 recv 同口径，M2b 拆出抖动缓冲后分离）
//   decode libavcodec 解码耗时
//   op     ProcessorChain 耗时（M2a 为 passthrough，恒 ~0）
//   encode 编码耗时（M2b 转码后才非 0）
//   send   UDP 分片与发送耗时
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <span>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <map>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

#include "cue/common/clock.h"
#include "cue/media/encoder.h"
#include "cue/media/h264_decoder.h"
#include "cue/proto/wire.h"

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using udp = net::ip::udp;
using json = nlohmann::json;

namespace {

class Ring {
 public:
  void push(int64_t v) {
    if (buf_.size() < kCap) {
      buf_.push_back(v);
    } else {
      buf_[head_] = v;
      head_ = (head_ + 1) % kCap;
    }
  }
  int64_t pct(double p) const {
    if (buf_.empty()) return 0;
    std::vector<int64_t> v = buf_;
    std::sort(v.begin(), v.end());
    return v[static_cast<std::size_t>(p * double(v.size() - 1))];
  }
  std::size_t size() const { return buf_.size(); }

 private:
  static constexpr std::size_t kCap = 4096;
  std::vector<int64_t> buf_;
  std::size_t head_ = 0;
};

// splitmix64：由包序号直接决定是否丢包，保证同一 (seed, seq) 结果完全一致
inline uint64_t splitmix64(uint64_t x) {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

class Injector {
 public:
  Injector(uint64_t seed, double drop_pct)
      : seed_(seed), permille_(static_cast<uint64_t>(drop_pct * 10.0)) {}
  bool drop(uint64_t seq) const {
    if (permille_ == 0) return false;
    return (splitmix64(seed_ * 0x9E3779B97F4A7C15ULL + seq) % 1000) < permille_;
  }
  uint64_t permille() const { return permille_; }

 private:
  uint64_t seed_;
  uint64_t permille_;
};

// 最近 N 帧的分片缓存，供 NACK 重传
class RetransmitCache {
 public:
  void put(uint32_t frame_id, const std::vector<std::vector<uint8_t>>& dg) {
    std::lock_guard<std::mutex> lk(mu_);
    frames_.emplace_back(frame_id, dg);
    while (frames_.size() > 256) frames_.pop_front();
  }
  std::optional<std::vector<uint8_t>> find(uint32_t frame_id, uint16_t frag_index) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
      if (it->first != frame_id) continue;
      if (frag_index < it->second.size()) return it->second[frag_index];
      return std::nullopt;
    }
    return std::nullopt;
  }

 private:
  std::mutex mu_;
  std::deque<std::pair<uint32_t, std::vector<std::vector<uint8_t>>>> frames_;
};

// 令牌桶：按字节计费（SPEC §7.2 的每会话限速）
class TokenBucket {
 public:
  TokenBucket(double bits_per_sec, double burst_bytes)
      : rate_bytes_(bits_per_sec / 8.0), burst_(burst_bytes), tokens_(burst_bytes) {}

  bool take(std::size_t bytes, int64_t now_us) {
    if (last_us_ == 0) last_us_ = now_us;
    const double dt = double(now_us - last_us_) / 1e6;
    last_us_ = now_us;
    tokens_ = std::min(burst_, tokens_ + dt * rate_bytes_);
    if (tokens_ < double(bytes)) return false;
    tokens_ -= double(bytes);
    return true;
  }

 private:
  double rate_bytes_;
  double burst_;
  double tokens_;
  int64_t last_us_ = 0;
};

struct FrameAcc {
  uint32_t count = 0;
  uint64_t pts_us = 0;
  bool keyframe = false;
  int64_t first_seen_us = 0;
  std::vector<std::vector<uint8_t>> parts;
};

struct Counters {
  uint64_t frames_in = 0;
  uint64_t frames_out = 0;
  uint64_t frames_dropped = 0;
  uint64_t bytes_in = 0;
  uint64_t frames_decoded = 0;
  uint64_t decode_errors = 0;
  uint64_t injected_drops = 0;
  uint64_t rejected = 0;  // 协议边界拒绝（wire.h 的 validate_media 给出具体原因）
};

void cleanup_stale(std::map<uint32_t, FrameAcc>& pending, int64_t now_us, Counters& c) {
  for (auto it = pending.begin(); it != pending.end();) {
    if (now_us - it->second.first_seen_us > 2'000'000) {
      ++c.frames_dropped;
      it = pending.erase(it);
    } else {
      ++it;
    }
  }
}

const char* kStageNames[6] = {"recv", "buffer", "decode", "op", "encode", "send"};

void report(const Ring rings[6], Counters& c, int64_t now_us, int64_t& t_last, uint64_t& frames_prev) {
  const double dt = double(now_us - t_last) / 1e6;
  const uint64_t df = c.frames_out - frames_prev;
  std::printf("[cued] fps=%.1f in=%llu out=%llu drop=%llu inj=%llu dec=%llu dec_err=%llu rej=%llu MB=%.2f",
              double(df) / dt, static_cast<unsigned long long>(c.frames_in),
              static_cast<unsigned long long>(c.frames_out), static_cast<unsigned long long>(c.frames_dropped),
              static_cast<unsigned long long>(c.injected_drops),
              static_cast<unsigned long long>(c.frames_decoded),
              static_cast<unsigned long long>(c.decode_errors),
              static_cast<unsigned long long>(c.rejected),
              double(c.bytes_in) / (1024.0 * 1024.0));
  for (int i = 0; i < 6; ++i) {
    if (rings[i].size() == 0) {
      std::printf("  %s=-", kStageNames[i]);
    } else {
      std::printf("  %s=%.3f/%.3f/%.3f", kStageNames[i],
                  double(rings[i].pct(0.50)) / 1000.0, double(rings[i].pct(0.95)) / 1000.0,
                  double(rings[i].pct(0.99)) / 1000.0);
    }
  }
  std::printf(" ms(p50/p95/p99)\n");
  std::fflush(stdout);
  frames_prev = c.frames_out;
  t_last = now_us;
}

}  // namespace

int main(int argc, char** argv) {
  uint16_t ws_port = 9000;
  std::string probe_host = "127.0.0.1";
  uint16_t probe_port = 9001;

  bool transcode_enabled = false;
  int out_w = 0;
  int out_h = 0;
  int out_kbps = 1500;
  int out_fps = 30;
  int out_gop = 60;
  std::string preset = "veryfast";
  double send_mbps = 10.0;
  double inject_drop = 0.0;
  uint64_t inject_seed = 42;
  std::string inject_trace;
  bool fec_enabled = true;

  for (int i = 1; i < argc; ++i) {
    std::string_view a = argv[i];
    auto next = [&]() -> std::string {
      return (i + 1 < argc) ? std::string(argv[++i]) : std::string();
    };
    if (a == "--ws-port") ws_port = static_cast<uint16_t>(std::stoi(next()));
    else if (a == "--probe-host") probe_host = next();
    else if (a == "--probe-port") probe_port = static_cast<uint16_t>(std::stoi(next()));
    else if (a == "--transcode") transcode_enabled = (next() == "on");
    else if (a == "--width") out_w = std::stoi(next());
    else if (a == "--height") out_h = std::stoi(next());
    else if (a == "--bitrate") out_kbps = std::stoi(next());
    else if (a == "--fps") out_fps = std::stoi(next());
    else if (a == "--gop") out_gop = std::stoi(next());
    else if (a == "--preset") preset = next();
    else if (a == "--send-mbps") send_mbps = std::stod(next());
    else if (a == "--inject-drop") inject_drop = std::stod(next());
    else if (a == "--inject-seed") inject_seed = std::stoull(next());
    else if (a == "--inject-trace") inject_trace = next();
    else if (a == "--fec") fec_enabled = (next() == "on");
    else if (a == "--help") {
      std::printf("cued --ws-port 9000 --probe-host 127.0.0.1 --probe-port 9001\n");
      return 0;
    }
  }

  cue::media::H264Decoder decoder;
  {
    std::string err;
    if (!decoder.open(err)) {
      std::fprintf(stderr, "[cued] decoder open failed: %s\n", err.c_str());
      return 1;
    }
  }

  cue::media::TranscodeChain chain;
  bool chain_ready = false;
  TokenBucket bucket(send_mbps * 1e6, send_mbps * 1e6 / 8.0 * 0.05);

  net::io_context ioc;
  beast::error_code ec;
  tcp::acceptor acc(ioc);
  acc.open(tcp::v4(), ec);
  acc.set_option(net::socket_base::reuse_address(true), ec);
  acc.bind(tcp::endpoint(net::ip::make_address("0.0.0.0"), ws_port), ec);
  acc.listen(net::socket_base::max_listen_connections, ec);
  if (ec) {
    std::fprintf(stderr, "[cued] listen %u failed: %s\n", ws_port, ec.message().c_str());
    return 1;
  }
  // 用原生 fd：主线程 sendto，NACK 线程 recvfrom（同一个 UDP socket 全双工）
  const int usock = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (usock < 0) {
    std::fprintf(stderr, "[cued] udp socket failed\n");
    return 1;
  }
  sockaddr_in probe_addr{};
  probe_addr.sin_family = AF_INET;
  probe_addr.sin_port = htons(probe_port);
  ::inet_pton(AF_INET, probe_host.c_str(), &probe_addr.sin_addr);
  const Injector injector(inject_seed, inject_drop);
  FILE* trace_fp = nullptr;
  if (!inject_trace.empty()) {
    trace_fp = std::fopen(inject_trace.c_str(), "w");
    if (trace_fp) {
      std::setvbuf(trace_fp, nullptr, _IONBF, 0);  // 进程被 kill 时也要保证 trace 已落盘
    } else {
      std::fprintf(stderr, "[cued] cannot open trace %s\n", inject_trace.c_str());
    }
  }
  RetransmitCache xmit_cache;

  std::atomic<bool> stop_nack{false};
  std::thread nack_thread([&] {
    std::vector<uint8_t> nbuf(2048);
    while (!stop_nack.load(std::memory_order_relaxed)) {
      sockaddr_in from{};
      socklen_t fl = sizeof(from);
      const ssize_t n = ::recvfrom(usock, nbuf.data(), nbuf.size(), 0,
                                   reinterpret_cast<sockaddr*>(&from), &fl);
      if (n < static_cast<ssize_t>(cue::proto::kDatagramHeaderSize)) continue;
      auto nh = cue::proto::decode_datagram({nbuf.data(), static_cast<size_t>(n)});
      if (!nh || nh->type != static_cast<uint8_t>(cue::proto::DownType::Nack)) continue;
      const uint8_t* p = nbuf.data() + cue::proto::kDatagramHeaderSize;
      std::size_t remain = static_cast<std::size_t>(n) - cue::proto::kDatagramHeaderSize;
      while (remain >= 6) {
        uint32_t fid = 0;
        uint16_t idx = 0;
        std::memcpy(&fid, p, 4);
        std::memcpy(&idx, p + 4, 2);
        p += 6;
        remain -= 6;
        if (auto pkt = xmit_cache.find(fid, idx)) {
          ::sendto(usock, pkt->data(), pkt->size(), 0, reinterpret_cast<const sockaddr*>(&from), fl);
        }
      }
    }
  });

  std::fprintf(stderr, "[cued] ws :%u  udp -> %s:%u (M2a: decode + 6-stage profile)\n", ws_port,
               probe_host.c_str(), probe_port);

  for (;;) {
    tcp::socket sock(ioc);
    acc.accept(sock, ec);
    if (ec) continue;

    websocket::stream<tcp::socket> ws(std::move(sock));
    ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
    ws.read_message_max(8 * 1024 * 1024);
    ws.accept(ec);
    if (ec) {
      std::fprintf(stderr, "[cued] handshake failed: %s\n", ec.message().c_str());
      continue;
    }
    std::fprintf(stderr, "[cued] client connected\n");

    std::map<uint32_t, FrameAcc> pending;
    Counters c;
    Ring rings[6];
    uint32_t session_id = 1;
    int64_t t_last_report = cue::now_us();
    int64_t t_last_cleanup = cue::now_us();
    uint64_t frames_prev = 0;
    // 注入丢包判定用的包序号：每连接一个、跨帧单调递增。
    // 旧口径 frame_id*64+pk 在 1080p 关键帧（>63 分片）时会与 FEC 包 / 下一帧撞号，
    // 丢包不再独立同分布；FEC 冗余包与数据包共用这一序号空间。
    uint64_t pkt_seq = 0;

    for (;;) {
      beast::flat_buffer buf;
      ws.read(buf, ec);
      if (ec) break;

      const auto* p = static_cast<const uint8_t*>(buf.data().data());
      const std::size_t n = buf.data().size();

      if (ws.got_text()) {
        auto j = json::parse(std::string_view(reinterpret_cast<const char*>(p), n), nullptr, false);
        if (j.is_object() && j.value("op", "") == "ping") {
          json pong{{"op", "pong"}, {"t", j.value("t", static_cast<int64_t>(0))}, {"server_us", cue::now_us()}};
          ws.text(true);
          ws.write(net::buffer(pong.dump()), ec);
          ws.binary(true);
          if (ec) break;
        }
        continue;
      }

      auto h = cue::proto::decode_media({p, n});
      if (!h) {
        ++c.rejected;  // 协议边界拒绝（wire.h 的 validate_media 决定具体原因）
        continue;
      }
      if (h->type != static_cast<uint8_t>(cue::proto::MediaType::Video)) continue;

      session_id = h->stream_id;
      c.bytes_in += n;
      if (h->frag_index == 0) ++c.frames_in;

      const int64_t now = cue::now_us();
      auto& fa = pending[h->frame_id];
      if (fa.count == 0) {
        fa.count = h->frag_count;
        fa.pts_us = h->pts_us;
        fa.keyframe = (h->flags & cue::proto::kUpFlagKeyframe) != 0;
        fa.first_seen_us = now;
        fa.parts.resize(fa.count);
      }
      if (h->frag_index < fa.count) {
        fa.parts[h->frag_index].assign(p + cue::proto::kMediaHeaderSize, p + n);
        if (h->flags & cue::proto::kUpFlagKeyframe) fa.keyframe = true;
      }

      bool complete = true;
      for (const auto& part : fa.parts) {
        if (part.empty()) {
          complete = false;
          break;
        }
      }
      if (!complete) continue;

      const int64_t t_buffer_done = cue::now_us();
      rings[0].push(t_buffer_done - fa.first_seen_us);
      rings[1].push(t_buffer_done - fa.first_seen_us);

      std::vector<uint8_t> frame;
      std::size_t total = 0;
      for (const auto& part : fa.parts) total += part.size();
      frame.reserve(total);
      for (const auto& part : fa.parts) frame.insert(frame.end(), part.begin(), part.end());

      const int64_t t_dec0 = cue::now_us();
      std::string derr;
      AVFrame* decoded = decoder.decode(frame, derr);
      rings[2].push(cue::now_us() - t_dec0);
      if (decoded) {
        ++c.frames_decoded;
      } else if (!derr.empty()) {
        ++c.decode_errors;
      }

      const int64_t t_op0 = cue::now_us();
      std::vector<uint8_t> encoded_buf;
      std::span<const uint8_t> payload{frame};
      bool keyframe = fa.keyframe;
      bool skip_send = false;

      if (transcode_enabled && decoded) {
        if (!chain_ready) {
          const int dw = out_w > 0 ? out_w : decoded->width;
          const int dh = out_h > 0 ? out_h : decoded->height;
          std::string cerr;
          if (chain.configure(dw, dh, out_kbps, out_fps, out_gop, preset, cerr)) {
            chain_ready = true;
            std::fprintf(stderr, "[cued] transcode on: %dx%d %dkbps gop=%d preset=%s\n", dw, dh,
                         out_kbps, out_gop, preset.c_str());
          } else {
            transcode_enabled = false;
            std::fprintf(stderr, "[cued] transcode configure failed: %s\n", cerr.c_str());
          }
        }
        if (chain_ready) {
          std::string eerr;
          auto enc = chain.process(decoded, fa.pts_us, eerr);
          if (enc) {
            encoded_buf = std::move(enc->data);
            payload = encoded_buf;
            keyframe = enc->keyframe;
          } else {
            skip_send = true;
          }
        }
      }
      rings[3].push(cue::now_us() - t_op0);
      rings[4].push(0);

      const int64_t t_send0 = cue::now_us();
      if (skip_send || !bucket.take(payload.size(), t_send0)) {
        ++c.frames_dropped;
      } else {
        auto datagrams =
            cue::proto::split_into_datagrams(payload, session_id, h->frame_id, fa.pts_us, keyframe);
        if (datagrams.empty()) {
          ++c.frames_dropped;
        } else {
          xmit_cache.put(h->frame_id, datagrams);

          // 注入丢包：序号必须全局唯一。若每帧从 0 重新计数，固定几个序号会被反复判定
          // （实测踩过：seed=42 时 seq 0..9 恰好全不丢，导致注入率恒为 0）
          std::size_t pk = 0;
          for (auto& d : datagrams) {
            const std::size_t cur_pk = pk++;
            if (injector.drop(uint64_t(h->frame_id) * 64ULL + cur_pk)) {
              ++c.injected_drops;
              if (trace_fp) std::fprintf(trace_fp, "%u %zu\n", h->frame_id, cur_pk);
              continue;
            }
            ::sendto(usock, d.data(), d.size(), 0,
                     reinterpret_cast<const sockaddr*>(&probe_addr), sizeof(probe_addr));
          }

          // XOR-FEC：一帧的所有分片为一组，生成 1 个冗余包
          if (fec_enabled && datagrams.size() > 1) {
            std::size_t maxlen = 0;
            for (auto& d : datagrams) {
              maxlen = std::max(maxlen, d.size() - cue::proto::kDatagramHeaderSize);
            }
            std::vector<uint8_t> parity(maxlen, 0);
            for (auto& d : datagrams) {
              const std::size_t plen = d.size() - cue::proto::kDatagramHeaderSize;
              for (std::size_t i = 0; i < plen; ++i) {
                parity[i] ^= d[cue::proto::kDatagramHeaderSize + i];
              }
            }
            cue::proto::DatagramHeader fh;
            fh.type = static_cast<uint8_t>(cue::proto::DownType::Fec);
            fh.session_id = session_id;
            fh.frame_id = h->frame_id;
            fh.frag_index = 0;
            fh.frag_count = static_cast<uint16_t>(datagrams.size());
            fh.pts_us = fa.pts_us;
            auto hdr = cue::proto::encode(fh);
            std::vector<uint8_t> fpkt(hdr.begin(), hdr.end());
            const std::size_t base = fpkt.size();
            fpkt.resize(base + 8 + maxlen, 0);
            std::memcpy(fpkt.data() + base, &h->frame_id, 4);
            const uint16_t mc = static_cast<uint16_t>(datagrams.size());
            std::memcpy(fpkt.data() + base + 6, &mc, 2);
            std::memcpy(fpkt.data() + base + 8, parity.data(), maxlen);
            if (!injector.drop(pkt_seq++)) {
              ::sendto(usock, fpkt.data(), fpkt.size(), 0,
                       reinterpret_cast<const sockaddr*>(&probe_addr), sizeof(probe_addr));
            }
          }
          ++c.frames_out;
        }
      }
      rings[5].push(cue::now_us() - t_send0);

      pending.erase(h->frame_id);
      // 未完成帧必须周期性清理：只挂在"完成路径"上时，丢了尾片的帧永远不会过期
      if (cue::now_us() - t_last_cleanup >= 1'000'000) {
        cleanup_stale(pending, cue::now_us(), c);
        t_last_cleanup = cue::now_us();
      }

      if (cue::now_us() - t_last_report >= 1'000'000) {
        report(rings, c, cue::now_us(), t_last_report, frames_prev);
      }
    }
    std::fprintf(stderr, "[cued] client disconnected\n");
  }
}
