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
#include <charconv>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <span>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
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

// 令牌桶：SPEC §7.2「10 Mbps / 64 包突发」。
// 口径按**包**计费（包大小按 kMaxUdpPayload + 24 B 头折算成包速率），这样 24 B/包的
// 头开销不会被漏算；突发是 64 包常量而不是"5% 窗口字节数"。
// 不足时先丢非关键帧：关键帧允许透支，但透支上限也是 64 包，关键帧风暴不能无限超发。
class TokenBucket {
 public:
  static constexpr double kBurstPackets = 64.0;
  static constexpr double kPacketBytes =
      double(cue::proto::kMaxUdpPayload + cue::proto::kDatagramHeaderSize);

  explicit TokenBucket(double bits_per_sec) : rate_pkts_(bits_per_sec / 8.0 / kPacketBytes) {}

  // 非关键帧：令牌不够就丢（不排队）
  bool take_nonkey(std::size_t pkts, int64_t now_us) {
    refill(now_us);
    if (tokens_ < double(pkts)) return false;
    tokens_ -= double(pkts);
    return true;
  }
  // 关键帧：允许透支到 -burst，保证"先丢非关键帧"这条语义真的成立
  bool take_key(std::size_t pkts, int64_t now_us) {
    refill(now_us);
    if (tokens_ - double(pkts) < -kBurstPackets) return false;
    tokens_ -= double(pkts);
    return true;
  }

 private:
  void refill(int64_t now_us) {
    if (last_us_ == 0) last_us_ = now_us;
    // 时间回拨（NTP/CLOCK_MONOTONIC 不会，但参数注入的假时钟可能）不能让 dt 变负
    const double dt = now_us > last_us_ ? double(now_us - last_us_) / 1e6 : 0.0;
    last_us_ = now_us;
    tokens_ = std::min(kBurstPackets, tokens_ + dt * rate_pkts_);
  }

  double rate_pkts_;
  double tokens_ = kBurstPackets;
  int64_t last_us_ = 0;
};

// 限流日志：同一类错误 1 s 内只打一行（弱网/配置错误不能把 stderr 刷爆）
struct RateLimitedLog {
  void log(int64_t now_us, std::string msg) {
    if (msg == last && now_us - t_last_us < 1'000'000) return;
    std::fprintf(stderr, "[cued] %s\n", msg.c_str());
    last = std::move(msg);
    t_last_us = now_us;
  }
  int64_t t_last_us = 0;
  std::string last;
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
  uint64_t rejected = 0;      // 协议边界拒绝（wire.h 的 validate_media 给出具体原因）
  uint64_t budget_drops = 0;  // 令牌桶不足丢的帧（先丢非关键帧）
  uint64_t send_errors = 0;   // sendto 失败的包（ENOBUFS/EMSGSIZE 不再被当成"已发送"）
};

void cleanup_stale(std::map<uint32_t, FrameAcc>& pending, int64_t now_us, Counters& c) {
  for (auto it = pending.begin(); it != pending.end();) {
    if (now_us - it->second.first_seen_us > 2'000'000) {
      ++c.frames_in;  // 帧在"完成或超时"处结算，重复首片/缺首片都不会再算错
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
  std::printf("[cued] fps=%.1f in=%llu out=%llu drop=%llu inj=%llu bkt=%llu serr=%llu dec=%llu "
              "dec_err=%llu rej=%llu MB=%.2f",
              double(df) / dt, static_cast<unsigned long long>(c.frames_in),
              static_cast<unsigned long long>(c.frames_out), static_cast<unsigned long long>(c.frames_dropped),
              static_cast<unsigned long long>(c.injected_drops),
              static_cast<unsigned long long>(c.budget_drops),
              static_cast<unsigned long long>(c.send_errors),
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
  // 结构化指标行：process_control 只认这一行（严格整行前缀 "METRICS "），不再靠裸子串
  // 匹配 "in="/"dec="（cued 多打一个 frames_in= 就会串味，review #29）。
  // 六段里未被采样的段写 null：报告里显示 n/a，不能被 0 冒充实测。
  json m{{"fps", double(df) / dt},
         {"frames_in", c.frames_in},
         {"frames_out", c.frames_out},
         {"dropped", c.frames_dropped},
         {"injected", c.injected_drops},
         {"decoded", c.frames_decoded},
         {"decode_errors", c.decode_errors},
         {"rejected", c.rejected},
         {"budget_drops", c.budget_drops},
         {"send_errors", c.send_errors}};
  for (int i = 0; i < 6; ++i) {
    if (rings[i].size() == 0) {
      m[kStageNames[i]] = nullptr;
    } else {
      m[kStageNames[i]] = json{{"p50", double(rings[i].pct(0.50)) / 1000.0},
                               {"p95", double(rings[i].pct(0.95)) / 1000.0},
                               {"p99", double(rings[i].pct(0.99)) / 1000.0}};
    }
  }
  const std::string line = "METRICS " + m.dump();
  std::fwrite(line.data(), 1, line.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
  frames_prev = c.frames_out;
  t_last = now_us;
}

// CLI 解析：不用 std::stoi/std::stod —— 非法值抛出的异常会穿透 main（SPEC §9 禁止
// 异常穿透模块边界）。from_chars + 范围校验，失败打一行原因并以退出码 2 结束（review #22）。
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
    const std::string_view a = argv[i];
    auto next = [&]() -> std::string_view {
      return (i + 1 < argc) ? std::string_view(argv[++i]) : std::string_view();
    };
    auto bad = [&](std::string_view val) {
      std::fprintf(stderr, "[cued] 参数 %.*s 非法: '%.*s'\n", static_cast<int>(a.size()), a.data(),
                   static_cast<int>(val.size()), val.data());
      std::exit(2);
    };
    auto want_int = [&](long long lo, long long hi) {
      long long v = 0;
      const auto val = next();
      if (!parse_int(val, lo, hi, v)) bad(val);
      return v;
    };
    if (a == "--ws-port") {
      ws_port = static_cast<uint16_t>(want_int(1, 65535));
    } else if (a == "--probe-host") {
      const auto val = next();
      if (val.empty()) bad(val);
      probe_host = std::string(val);
    } else if (a == "--probe-port") {
      probe_port = static_cast<uint16_t>(want_int(1, 65535));
    } else if (a == "--transcode") {
      const auto val = next();
      if (val != "on" && val != "off") bad(val);
      transcode_enabled = (val == "on");
    } else if (a == "--width") {
      out_w = static_cast<int>(want_int(0, 4096));
    } else if (a == "--height") {
      out_h = static_cast<int>(want_int(0, 4096));
    } else if (a == "--bitrate") {
      out_kbps = static_cast<int>(want_int(1, 20000));
    } else if (a == "--fps") {
      out_fps = static_cast<int>(want_int(1, 240));
    } else if (a == "--gop") {
      out_gop = static_cast<int>(want_int(1, 600));
    } else if (a == "--preset") {
      const auto val = next();
      if (val.empty()) bad(val);
      preset = std::string(val);
    } else if (a == "--send-mbps") {
      const auto val = next();
      if (!parse_double(val, 0.1, 10000.0, send_mbps)) bad(val);
    } else if (a == "--inject-drop") {
      // 旧实现用 stod + static_cast<uint64_t>，负数会变成巨大值（UB）
      const auto val = next();
      if (!parse_double(val, 0.0, 100.0, inject_drop)) bad(val);
    } else if (a == "--inject-seed") {
      const auto val = next();
      if (val.empty()) bad(val);
      unsigned long long v = 0;
      const auto [pe, ec] = std::from_chars(val.data(), val.data() + val.size(), v);
      if (ec != std::errc() || pe != val.data() + val.size()) bad(val);
      inject_seed = static_cast<uint64_t>(v);
    } else if (a == "--inject-trace") {
      const auto val = next();
      if (val.empty()) bad(val);
      inject_trace = std::string(val);
    } else if (a == "--fec") {
      const auto val = next();
      if (val != "on" && val != "off") bad(val);
      fec_enabled = (val == "on");
    } else if (a == "--help") {
      std::printf(
          "cued --ws-port 9000 --probe-host 127.0.0.1 --probe-port 9001\n"
          "     [--transcode on|off] [--width N --height N] [--bitrate kbps] [--fps N]\n"
          "     [--gop N] [--preset NAME] [--send-mbps F]\n"
          "     [--inject-drop 0..100] [--inject-seed N] [--inject-trace FILE] [--fec on|off]\n");
      return 0;
    } else {
      std::fprintf(stderr, "[cued] 未知参数: %.*s\n", static_cast<int>(a.size()), a.data());
      return 2;
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
  // 10 Mbps / 64 包突发（SPEC §7.2）：包速率按 1224 B/包折算，头开销一并计入
  TokenBucket bucket(send_mbps * 1e6);

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

  // NACK 重传线程：阻塞在 poll(200ms) 上而不是裸 recvfrom——退出路径才能
  // stop_nack=true 后 join()；旧实现从不 join，主循环哪天能正常退出就会被
  // std::thread 析构里的 std::terminate 打死（review #21）。
  std::atomic<bool> stop_nack{false};
  std::atomic<uint64_t> nack_resends{0};
  std::atomic<uint32_t> cur_session{0};  // 当前连接的会话号：NACK 线程据此过滤（review #20）
  std::thread nack_thread([&] {
    std::vector<uint8_t> nbuf(2048);
    RateLimitedLog err_log;
    while (!stop_nack.load(std::memory_order_relaxed)) {
      pollfd pfd{usock, POLLIN, 0};
      const int pr = ::poll(&pfd, 1, 200);
      if (pr < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (pr == 0) continue;  // 超时：回到循环头检查停止位
      sockaddr_in from{};
      socklen_t fl = sizeof(from);
      const ssize_t n = ::recvfrom(usock, nbuf.data(), nbuf.size(), MSG_DONTWAIT,
                                   reinterpret_cast<sockaddr*>(&from), &fl);
      if (n < static_cast<ssize_t>(cue::proto::kDatagramHeaderSize)) continue;
      // 只服务本会话的 probe：任意源都能触发重传就是一个反射放大器（review #20）
      if (from.sin_addr.s_addr != probe_addr.sin_addr.s_addr ||
          from.sin_port != probe_addr.sin_port) {
        continue;
      }
      auto nh = cue::proto::decode_datagram({nbuf.data(), static_cast<size_t>(n)});
      if (!nh) continue;
      if (nh->type == static_cast<uint8_t>(cue::proto::DownType::StatsReq)) {
        // SPEC §7.1 时钟对齐：probe 发下行回环请求（载荷 = 它的发戳），这里原样回显发戳
        // 并附上自己的时钟；跨机部署的端到端延迟由此有确定口径（review #9）。
        if (n < static_cast<ssize_t>(cue::proto::kDatagramHeaderSize + 8)) continue;
        cue::proto::DatagramHeader sh;
        sh.type = static_cast<uint8_t>(cue::proto::DownType::StatsReq);
        sh.session_id = nh->session_id;
        const auto shb = cue::proto::encode(sh);
        std::vector<uint8_t> rep(shb.begin(), shb.end());
        const std::size_t base = rep.size();
        rep.resize(base + 16, 0);
        std::memcpy(rep.data() + base, nbuf.data() + cue::proto::kDatagramHeaderSize, 8);
        cue::proto::detail::put_u64(rep.data() + base + 8,
                                    static_cast<uint64_t>(cue::now_us()));
        if (::sendto(usock, rep.data(), rep.size(), 0, reinterpret_cast<const sockaddr*>(&from),
                     fl) < 0) {
          err_log.log(cue::now_us(), std::string("stats 回包 sendto 失败: ") +
                                         std::strerror(errno));
        }
        continue;
      }
      if (nh->type != static_cast<uint8_t>(cue::proto::DownType::Nack)) continue;
      // 会话号校验（review #20）：probe 回带下行头里的 session_id
      const uint32_t sid = nh->session_id;
      const uint32_t cur = cur_session.load(std::memory_order_relaxed);
      if (sid != 0 && cur != 0 && sid != cur) continue;
      const auto entries = cue::proto::decode_nack_entries(
          {nbuf.data() + cue::proto::kDatagramHeaderSize,
           static_cast<std::size_t>(n) - cue::proto::kDatagramHeaderSize});
      for (const auto& e : entries) {
        auto pkt = xmit_cache.find(e.frame_id, e.frag_index);
        if (!pkt) continue;
        // SPEC §7.2 下行 flags bit1：重传包必须能被接收端识别成重传
        if (pkt->size() > 2) {
          (*pkt)[2] = static_cast<uint8_t>((*pkt)[2] | cue::proto::kDownFlagRetransmit);
        }
        const ssize_t sn = ::sendto(usock, pkt->data(), pkt->size(), 0,
                                    reinterpret_cast<const sockaddr*>(&from), fl);
        if (sn < 0) {
          err_log.log(cue::now_us(), std::string("nack 重传 sendto 失败: ") + std::strerror(errno));
          continue;
        }
        nack_resends.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });
  // 退出路径的 RAII：即使将来给主循环加了正常退出条件，也不会漏 join
  struct NackGuard {
    std::atomic<bool>& stop;
    std::thread& th;
    ~NackGuard() {
      stop.store(true);
      if (th.joinable()) th.join();
    }
  } nack_guard{stop_nack, nack_thread};

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
    RateLimitedLog err_log;  // 每连接一份：重启 cued 后限流状态跟着新会话走

    for (;;) {
      beast::flat_buffer buf;
      ws.read(buf, ec);
      if (ec) break;

      // 周期任务必须与「帧是否完成」解耦：丢尾片的帧走不到完成路径，若把清理挂在
      // 完成之后，pending 只增不减、2 s 阈值形同虚设（review #5）。这里每包都检查，
      // 500 ms 一次真正清理。
      {
        const int64_t now_tick = cue::now_us();
        if (now_tick - t_last_cleanup >= 500'000) {
          cleanup_stale(pending, now_tick, c);
          t_last_cleanup = now_tick;
        }
        // 报告同样不能挂在「帧完成」上：整帧持续丢尾片时 fps/丢帧仍要可见
        if (now_tick - t_last_report >= 1'000'000) {
          report(rings, c, now_tick, t_last_report, frames_prev);
        }
      }

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
      cur_session.store(session_id, std::memory_order_relaxed);
      c.bytes_in += n;
      // frames_in 不再按 "frag_index==0" 计：首片丢失会少计、重复首片会多计。
      // 改成「帧完成或超时」时结算（review #35），与 probe 侧的 frames 口径一致。

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
        // 只吃 payload_len 字节：整段剩余字节可能带尾随垃圾（review #17）；
        // validate_media 已保证 payload_len <= n - 32
        const uint8_t* frag = p + cue::proto::kMediaHeaderSize;
        fa.parts[h->frag_index].assign(frag, frag + h->payload_len);
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
      ++c.frames_in;  // 帧在「完成或超时」处结算（#35）

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
      int64_t enc_us = 0;      // 编码段真实耗时（review #3：不再无条件记 0）
      bool enc_sampled = false;  // transcode 关闭时该段不采样，报告里显示 "-"

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
          auto enc = chain.process(decoded, fa.pts_us, eerr, &enc_us);
          enc_sampled = enc_us > 0;
          if (enc) {
            encoded_buf = std::move(enc->data);
            payload = encoded_buf;
            keyframe = enc->keyframe;
          } else {
            skip_send = true;
          }
        }
      }
      // op 段只算 ProcessorChain 里除编码以外的部分（M2a 为 passthrough ≈0），
      // encode 段单列 —— 两段互不重叠，否则「op 含编码」会让六段口径名不副实。
      const int64_t t_op_end = cue::now_us();
      rings[3].push(std::max<int64_t>(0, t_op_end - t_op0 - enc_us));
      if (enc_sampled) rings[4].push(enc_us);

      const int64_t t_send0 = cue::now_us();
      std::vector<std::vector<uint8_t>> datagrams;
      cue::proto::SplitError split_err = cue::proto::SplitError::None;
      bool budget_ok = false;
      if (!skip_send) {
        datagrams = cue::proto::split_into_datagrams(payload, session_id, h->frame_id, fa.pts_us,
                                                     keyframe, &split_err);
        if (!datagrams.empty()) {
          // SPEC §7.2 令牌桶：按包计费（含 24 B 头）、64 包突发、不足时先丢非关键帧
          budget_ok = keyframe ? bucket.take_key(datagrams.size(), t_send0)
                               : bucket.take_nonkey(datagrams.size(), t_send0);
        }
      }
      if (skip_send) {
        ++c.frames_dropped;
      } else if (split_err != cue::proto::SplitError::None) {
        // 空帧 / 超过单帧 1024 分片：协议上限内发不出去，计 rejected 而不是静默丢（#34）
        ++c.rejected;
        ++c.frames_dropped;
        err_log.log(cue::now_us(),
                    std::string("帧无法切片: ") + cue::proto::split_error_reason(split_err));
      } else if (!budget_ok) {
        ++c.budget_drops;
        ++c.frames_dropped;
      } else {
        xmit_cache.put(h->frame_id, datagrams);

        // sendto 的返回值与 errno 不能丢：ENOBUFS/EMSGSIZE 会被算成「已发送」（review #16）
        auto send_dgram = [&](const std::vector<uint8_t>& d) -> bool {
          const ssize_t sn = ::sendto(usock, d.data(), d.size(), 0,
                                      reinterpret_cast<const sockaddr*>(&probe_addr),
                                      sizeof(probe_addr));
          if (sn < 0) {
            ++c.send_errors;
            err_log.log(cue::now_us(), std::string("sendto 失败: ") + std::strerror(errno));
            return false;
          }
          return true;
        };

        // 注入丢包：判定序号必须是整条连接全局单调、且与 FEC 包共用一个序号空间。
        // 旧口径 frame_id*64+pk 在分片数 >64（720p 关键帧）时会与 FEC 包撞号，
        // 且 frame_id*64+64 == (frame_id+1)*64 会跨帧撞号 → 丢包不再独立同分布。
        bool all_sent = true;
        std::size_t pk = 0;
        for (auto& d : datagrams) {
          const std::size_t cur_pk = pk++;
          if (injector.drop(pkt_seq++)) {
            ++c.injected_drops;
            if (trace_fp) std::fprintf(trace_fp, "%u %zu\n", h->frame_id, cur_pk);
            continue;
          }
          if (!send_dgram(d)) all_sent = false;
        }

        // XOR-FEC：一帧的所有分片为一组，生成 1 个冗余包。
        // 载荷的字节序/字段顺序收在 wire.h（两端不再各写一份 memcpy，review #18）。
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
          const auto hdr = cue::proto::encode(fh);
          std::vector<uint8_t> fpkt(hdr.begin(), hdr.end());
          cue::proto::FecPayloadHeader ph;
          ph.base_frame_id = h->frame_id;
          ph.group_index = 0;  // 一帧一组：显式写 0，不再靠"零填充"隐式约定
          ph.member_count = static_cast<uint16_t>(datagrams.size());
          const auto phb = cue::proto::encode(ph);
          fpkt.insert(fpkt.end(), phb.begin(), phb.end());
          fpkt.insert(fpkt.end(), parity.begin(), parity.end());
          if (!injector.drop(pkt_seq++)) {
            if (!send_dgram(fpkt)) all_sent = false;
          }
        }
        // 真实上线率：只要有一包没被内核接受，这一帧就不算发出去（review #16）
        if (all_sent) ++c.frames_out;
      }
      rings[5].push(cue::now_us() - t_send0);

      pending.erase(h->frame_id);
    }
    std::fprintf(stderr, "[cued] client disconnected\n");
  }
}
