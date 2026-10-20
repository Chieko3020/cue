// cued — 媒体管线（M1 版）
// 浏览器 --WS--> cued --UDP--> cue-probe
// M1 只做：WS 分片重组 → 帧拼接 → 按 UDP 协议分片下发（不做转码、NACK、FEC、损伤注入）
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "cue/common/clock.h"
#include "cue/proto/wire.h"

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using udp = net::ip::udp;
using json = nlohmann::json;

namespace {

struct FrameAcc {
  uint32_t count = 0;
  uint64_t pts_us = 0;
  bool keyframe = false;
  int64_t first_seen_us = 0;
  std::vector<std::vector<uint8_t>> parts;
};

void cleanup_stale(std::map<uint32_t, FrameAcc>& pending, int64_t now_us, uint64_t& dropped) {
  for (auto it = pending.begin(); it != pending.end();) {
    if (now_us - it->second.first_seen_us > 2'000'000) {
      ++dropped;
      it = pending.erase(it);
    } else {
      ++it;
    }
  }
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

  std::fprintf(stderr, "[cued] ws :%u  udp -> %s:%u\n", ws_port, probe_host.c_str(), probe_port);

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
    uint32_t session_id = 1;
    uint64_t frames_in = 0, frames_out = 0, frames_dropped = 0, bytes_in = 0;

    for (;;) {
      beast::flat_buffer buf;
      ws.read(buf, ec);
      if (ec) break;

      const auto* p = static_cast<const uint8_t*>(buf.data().data());
      const std::size_t n = buf.data().size();

      // 文本帧 = 控制消息（M1 只有 ping/pong 做时钟同步）
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
      bytes_in += n;
      if (h->frag_index == 0) ++frames_in;

      auto& fa = pending[h->frame_id];
      if (fa.count == 0) {
        fa.count = h->frag_count;
        fa.pts_us = h->pts_us;
        fa.keyframe = (h->flags & cue::proto::kUpFlagKeyframe) != 0;
        fa.first_seen_us = cue::now_us();
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

      std::vector<uint8_t> frame;
      std::size_t total = 0;
      for (const auto& part : fa.parts) total += part.size();
      frame.reserve(total);
      for (const auto& part : fa.parts) frame.insert(frame.end(), part.begin(), part.end());

      // 原样转发浏览器时间戳（已在客户端加过时钟偏移），probe 用它算端到端延迟
      auto datagrams = cue::proto::split_into_datagrams(frame, session_id, h->frame_id, fa.pts_us, fa.keyframe);
      if (datagrams.empty()) {
        ++frames_dropped;
      } else {
        for (auto& d : datagrams) {
          usock.send_to(net::buffer(d), probe_ep, 0, ec);
        }
        ++frames_out;
      }
      pending.erase(h->frame_id);
      if (pending.size() > 256) cleanup_stale(pending, cue::now_us(), frames_dropped);
    }

    std::fprintf(stderr,
                 "[cued] client disconnected  frames_in=%llu frames_out=%llu dropped=%llu bytes_in=%llu\n",
                 static_cast<unsigned long long>(frames_in), static_cast<unsigned long long>(frames_out),
                 static_cast<unsigned long long>(frames_dropped), static_cast<unsigned long long>(bytes_in));
  }
}
