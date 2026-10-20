// Spike-B：Boost.Beast WebSocket 客户端。
// 阻塞读循环 + 采样线程；按 seq 检测丢失，按 ts_ns 算单程延迟（同机 CLOCK_MONOTONIC 可比）。

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../common/bench_util.h"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using nlohmann::json;
using namespace cue_spike;

namespace {

struct Config {
  std::string host{"127.0.0.1"};
  uint16_t port{9101};
  int duration_s{600};
  std::string out{"client.jsonl"};
};

struct Shared {
  std::atomic<uint64_t> recv{0};
  std::atomic<uint64_t> lost{0};       // seq 缺口累计
  std::atomic<uint64_t> parse_ns{0};   // JSON 解析总耗时
  std::atomic<int64_t> last_seq{-1};
  std::atomic<int64_t> last_recv_ns{0};
  std::mutex mu;
  Samples latency;  // ns，本秒窗口
  std::atomic<size_t> lat_window_n{0};
};

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  for (int i = 1; i < argc; ++i) {
    std::string_view a = argv[i];
    auto next = [&]() -> std::string { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(); };
    if (a == "--host") cfg.host = next();
    else if (a == "--port") cfg.port = static_cast<uint16_t>(std::stoi(next()));
    else if (a == "--duration") cfg.duration_s = std::stoi(next());
    else if (a == "--out") cfg.out = next();
    else {
      std::fprintf(stderr, "unknown arg %s\n", argv[i]);
      return 2;
    }
  }

  asio::io_context ioc;
  tcp::resolver resolver{ioc};
  websocket::stream<beast::tcp_stream> ws{ioc};
  beast::error_code ec;

  auto results = resolver.resolve(cfg.host, std::to_string(cfg.port), ec);
  if (ec) {
    std::fprintf(stderr, "resolve: %s\n", ec.message().c_str());
    return 1;
  }
  beast::get_lowest_layer(ws).connect(results);
  beast::get_lowest_layer(ws).socket().set_option(tcp::no_delay(true));
  ws.handshake(cfg.host + ":" + std::to_string(cfg.port), "/", ec);
  if (ec) {
    std::fprintf(stderr, "handshake: %s\n", ec.message().c_str());
    return 1;
  }
  ws.read_message_max(256 * 1024);
  {
    const std::string hello = R"({"hello":"cue-spike-b"})";
    ws.text(true);
    ws.write(asio::buffer(hello), ec);
    if (ec) {
      std::fprintf(stderr, "hello write: %s\n", ec.message().c_str());
      return 1;
    }
  }

  Shared sh;
  std::ofstream out(cfg.out, std::ios::app);
  std::atomic<bool> done{false};
  const int64_t t_start = now_ns();

  // 采样线程：每秒一行
  std::thread sampler([&] {
    auto last_cpu = cpu_seconds();
    int64_t last_t = now_ns();
    uint64_t last_recv = 0;
    uint64_t last_lost = 0;
    uint64_t last_parse = 0;
    while (!done.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      const int64_t t = now_ns();
      const double dt = static_cast<double>(t - last_t) / 1e9;
      auto cpu = cpu_seconds();
      const double du = cpu.user - last_cpu.user;
      const double ds = cpu.sys - last_cpu.sys;
      last_cpu = cpu;
      const uint64_t r = sh.recv.load(std::memory_order_relaxed);
      const uint64_t l = sh.lost.load(std::memory_order_relaxed);
      const uint64_t p = sh.parse_ns.load(std::memory_order_relaxed);
      Samples lat;
      {
        std::lock_guard lk(sh.mu);
        lat = std::move(sh.latency);
        sh.latency = Samples{};
      }
      const uint64_t n = lat.size();
      json j = {
          {"t_s", static_cast<double>(t - t_start) / 1e9},
          {"rss_kb", rss_kb()},
          {"cpu_user_pct", 100.0 * du / dt},
          {"cpu_sys_pct", 100.0 * ds / dt},
          {"recv", r},
          {"rate", static_cast<double>(r - last_recv) / dt},
          {"lost", l},
          {"lost_delta", l - last_lost},
          {"parse_us_avg", (r != last_recv) ? static_cast<double>(p - last_parse) / 1000.0 / static_cast<double>(r - last_recv) : 0.0},
          {"lat_p50_ms", n ? static_cast<double>(lat.pct(0.50)) / 1e6 : 0.0},
          {"lat_p95_ms", n ? static_cast<double>(lat.pct(0.95)) / 1e6 : 0.0},
          {"lat_p99_ms", n ? static_cast<double>(lat.pct(0.99)) / 1e6 : 0.0},
          {"lat_max_ms", n ? static_cast<double>(lat.max()) / 1e6 : 0.0},
          {"lat_n", n},
      };
      const std::string line = j.dump();
      out << line << '\n';
      out.flush();
      std::fprintf(stdout, "%s\n", line.c_str());
      std::fflush(stdout);
      last_recv = r;
      last_lost = l;
      last_parse = p;
      last_t = t;
    }
  });

  beast::flat_buffer buf;
  std::vector<int64_t> window;
  window.reserve(4096);
  const int64_t deadline = t_start + int64_t(cfg.duration_s) * 1'000'000'000LL;
  while (now_ns() < deadline) {
    ws.read(buf, ec);
    if (ec) {
      // 对端关闭或本机结束
      std::fprintf(stderr, "read: %s\n", ec.message().c_str());
      break;
    }
    const int64_t t = now_ns();
    const auto d = buf.data();
    std::string_view sv(static_cast<const char*>(d.data()), d.size());
    const int64_t p0 = now_ns();
    json j = json::parse(sv, nullptr, false);
    const int64_t p1 = now_ns();
    sh.parse_ns.fetch_add(uint64_t(p1 - p0), std::memory_order_relaxed);
    if (j.is_discarded() || !j.is_object()) {
      buf.consume(buf.size());
      continue;
    }
    if (j.contains("pong")) {
      buf.consume(buf.size());
      continue;
    }
    const int64_t seq = j.value("seq", int64_t(-1));
    const int64_t ts = j.value("ts_ns", int64_t(0));
    if (seq >= 0) {
      const int64_t prev = sh.last_seq.load(std::memory_order_relaxed);
      if (prev >= 0 && seq > prev + 1) {
        sh.lost.fetch_add(uint64_t(seq - prev - 1), std::memory_order_relaxed);
      }
      sh.last_seq.store(seq, std::memory_order_relaxed);
      sh.recv.fetch_add(1, std::memory_order_relaxed);
      if (ts > 0) window.push_back(t - ts);
    }
    buf.consume(buf.size());
    // 每 200ms 把窗口交给采样线程
    if (window.size() >= 200) {
      std::lock_guard lk(sh.mu);
      for (int64_t v : window) sh.latency.add(v);
      window.clear();
    }
  }
  {
    std::lock_guard lk(sh.mu);
    for (int64_t v : window) sh.latency.add(v);
  }
  done.store(true);
  sampler.join();
  ws.close(websocket::close_code::normal, ec);

  auto cpu = cpu_seconds();
  json fin = {
      {"final", true},
      {"t_s", static_cast<double>(now_ns() - t_start) / 1e9},
      {"rss_kb", rss_kb()},
      {"cpu_user_s", cpu.user},
      {"cpu_sys_s", cpu.sys},
      {"recv", sh.recv.load()},
      {"lost", sh.lost.load()},
      {"last_seq", sh.last_seq.load()},
  };
  const std::string line = fin.dump();
  out << line << '\n';
  out.flush();
  std::fprintf(stdout, "%s\n", line.c_str());
  return 0;
}
