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
#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "cue/common/clock.h"
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
  std::printf("[cued] fps=%.1f in=%llu out=%llu drop=%llu dec=%llu dec_err=%llu MB=%.2f",
              double(df) / dt, static_cast<unsigned long long>(c.frames_in),
              static_cast<unsigned long long>(c.frames_out), static_cast<unsigned long long>(c.frames_dropped),
              static_cast<unsigned long long>(c.frames_decoded),
              static_cast<unsigned long long>(c.decode_errors),
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

  for (int i = 1; i < argc; ++i) {
    std::string_view a = argv[i];
    auto next = [&]() -> std::string {
      return (i + 1 < argc) ? std::string(argv[++i]) : std::string();
    };
    if (a == "--ws-port") ws_port = static_cast<uint16_t>(std::stoi(next()));
    else if (a == "--probe-host") probe_host = next();
    else if (a == "--probe-port") probe_port = static_cast<uint16_t>(std::stoi(next()));
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
  udp::socket usock(ioc, udp::v4());
  const udp::endpoint probe_ep(net::ip::make_address(probe_host), probe_port);

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
    uint64_t frames_prev = 0;

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
      if (!h) continue;
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
      auto info = decoder.decode(frame, derr);
      rings[2].push(cue::now_us() - t_dec0);
      if (info) {
        ++c.frames_decoded;
      } else if (!derr.empty()) {
        ++c.decode_errors;
      }

      const int64_t t_op0 = cue::now_us();
      rings[3].push(cue::now_us() - t_op0);

      rings[4].push(0);

      const int64_t t_send0 = cue::now_us();
      auto datagrams =
          cue::proto::split_into_datagrams(frame, session_id, h->frame_id, fa.pts_us, fa.keyframe);
      if (datagrams.empty()) {
        ++c.frames_dropped;
      } else {
        for (auto& d : datagrams) usock.send_to(net::buffer(d), probe_ep, 0, ec);
        ++c.frames_out;
      }
      rings[5].push(cue::now_us() - t_send0);

      pending.erase(h->frame_id);
      if (pending.size() > 256) cleanup_stale(pending, cue::now_us(), c);

      if (cue::now_us() - t_last_report >= 1'000'000) {
        report(rings, c, cue::now_us(), t_last_report, frames_prev);
      }
    }
    std::fprintf(stderr, "[cued] client disconnected\n");
  }
}
