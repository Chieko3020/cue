// Spike-B：Boost.Beast WebSocket 服务端压测。
// 单 io 线程 + 单生产者线程，按固定速率（默认 1000 事件/秒）向所有连接广播 JSON 事件。
// 测量：RSS 曲线、CPU、JSON 序列化耗时、入队→写完成耗时、写队列深度、丢弃数。

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <format>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
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
  std::string bind_addr{"127.0.0.1"};
  uint16_t port{9101};
  int rate{1000};          // 事件/秒
  int duration_s{600};     // 运行时长
  int stats_interval_s{30};
  size_t queue_cap{20000}; // 单连接写队列上限（= 20 s 的 1000 evt/s）
  std::string out{"server.jsonl"};
};

struct Counters {
  std::atomic<uint64_t> evt_generated{0};
  std::atomic<uint64_t> evt_enqueued{0};
  std::atomic<uint64_t> evt_dropped{0};
  std::atomic<uint64_t> bytes_sent{0};
  std::atomic<uint64_t> write_calls{0};
  std::atomic<uint64_t> sessions_accepted{0};
  std::atomic<std::size_t> queue_max{0};
  // 以下只在 io 线程写，采样线程读，用 mutex 保护
  std::mutex mu;
  Samples write_latency;  // 入队 → async_write 完成，ns
};

class Session;

class Broadcaster {
 public:
  void add(const std::shared_ptr<Session>& s) {
    std::lock_guard lk(mu_);
    sessions_.push_back(s);
  }
  void remove(Session* s) {
    std::lock_guard lk(mu_);
    std::erase_if(sessions_, [s](const std::weak_ptr<Session>& w) {
      auto sp = w.lock();
      return !sp || sp.get() == s;
    });
  }
  // 在 io 线程上执行
  void broadcast(const std::shared_ptr<const std::string>& msg, int64_t t_enqueue);

  size_t alive() {
    std::lock_guard lk(mu_);
    size_t n = 0;
    for (auto& w : sessions_) {
      if (!w.expired()) ++n;
    }
    return n;
  }

 private:
  std::mutex mu_;
  std::vector<std::weak_ptr<Session>> sessions_;
};

class Session : public std::enable_shared_from_this<Session> {
 public:
  Session(tcp::socket&& sock, Broadcaster& bc, Counters& c) : ws_(std::move(sock)), bc_(bc), c_(c) {}

  ~Session() { bc_.remove(this); }

  void run() {
    beast::get_lowest_layer(ws_).socket().set_option(tcp::no_delay(true));
    ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
    ws_.read_message_max(64 * 1024);
    // 必须完成 WebSocket 握手，否则 client 会在 handshake 阶段收到 "end of stream"。
    // 注意：注册到 Broadcaster 由 accept 处负责（bc.add），这里不可重复 add，否则事件会被广播两次。
    auto self = shared_from_this();
    ws_.async_accept([self](beast::error_code ec) {
      if (ec) return;
      self->do_read();
    });
  }

  // 只为「拥塞丢事件」与「写队列深度」计数
  void push(const std::shared_ptr<const std::string>& msg, int64_t t_enqueue) {
    if (q_.size() >= cfg_queue_cap_) {
      c_.evt_dropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    q_.push_back(Out{msg, t_enqueue});
    c_.evt_enqueued.fetch_add(1, std::memory_order_relaxed);
    const size_t depth = q_.size();
    std::size_t prev = c_.queue_max.load(std::memory_order_relaxed);
    while (depth > prev && !c_.queue_max.compare_exchange_weak(prev, depth, std::memory_order_relaxed)) {
    }
    if (!writing_) do_write();
  }

  static void set_queue_cap(size_t cap) { cfg_queue_cap_ = cap; }

 private:
  struct Out {
    std::shared_ptr<const std::string> msg;
    int64_t t_enqueue;
  };

  void do_read() {
    auto self = shared_from_this();
    ws_.async_read(rbuf_, [self](beast::error_code ec, size_t n) {
      if (ec) {
        self->closing_ = true;
        return;
      }
      self->handle_control(self->rbuf_, n);
      self->rbuf_.consume(self->rbuf_.size());
      self->do_read();
    });
  }

  void handle_control(beast::flat_buffer& buf, size_t n) {
    if (n == 0 || n > 8192) return;
    auto data = buf.data();
    std::string_view sv(static_cast<const char*>(data.data()), n);
    json j = json::parse(sv, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return;
    if (j.contains("ping")) {
      std::string pong = std::format(R"({{"pong":{}}})", j["ping"].dump());
      push(std::make_shared<const std::string>(std::move(pong)), now_ns());
    }
  }

  void do_write() {
    if (q_.empty()) {
      writing_ = false;
      return;
    }
    writing_ = true;
    auto self = shared_from_this();
    auto& front = q_.front();
    current_t_ = front.t_enqueue;
    ws_.text(true);
    ws_.async_write(asio::buffer(*front.msg), [self](beast::error_code ec, size_t n) {
      if (ec) {
        self->closing_ = true;
        self->q_.clear();
        self->writing_ = false;
        return;
      }
      const int64_t lat = now_ns() - self->current_t_;
      {
        std::lock_guard lk(self->c_.mu);
        self->c_.write_latency.add(lat);
      }
      self->c_.bytes_sent.fetch_add(n, std::memory_order_relaxed);
      self->c_.write_calls.fetch_add(1, std::memory_order_relaxed);
      self->q_.pop_front();
      self->do_write();
    });
  }

  inline static size_t cfg_queue_cap_ = 20000;

  websocket::stream<beast::tcp_stream> ws_;
  beast::flat_buffer rbuf_;
  std::deque<Out> q_;
  bool writing_{false};
  bool closing_{false};
  int64_t current_t_{0};
  Broadcaster& bc_;
  Counters& c_;
};

void Broadcaster::broadcast(const std::shared_ptr<const std::string>& msg, int64_t t_enqueue) {
  std::lock_guard lk(mu_);
  for (auto& w : sessions_) {
    if (auto sp = w.lock()) sp->push(msg, t_enqueue);
  }
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  for (int i = 1; i < argc; ++i) {
    std::string_view a = argv[i];
    auto next = [&]() -> std::string { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(); };
    if (a == "--port") cfg.port = static_cast<uint16_t>(std::stoi(next()));
    else if (a == "--rate") cfg.rate = std::stoi(next());
    else if (a == "--duration") cfg.duration_s = std::stoi(next());
    else if (a == "--stats-interval") cfg.stats_interval_s = std::stoi(next());
    else if (a == "--queue-cap") cfg.queue_cap = static_cast<size_t>(std::stoul(next()));
    else if (a == "--out") cfg.out = next();
    else if (a == "--bind") cfg.bind_addr = next();
    else {
      std::fprintf(stderr, "unknown arg %s\n", argv[i]);
      return 2;
    }
  }
  Session::set_queue_cap(cfg.queue_cap);

  asio::io_context ioc{1};
  Broadcaster bc;
  Counters counters;
  std::atomic<bool> stop{false};

  tcp::endpoint ep{asio::ip::make_address(cfg.bind_addr), cfg.port};
  tcp::acceptor acceptor{ioc, ep};
  acceptor.set_option(asio::socket_base::reuse_address(true));

  std::function<void()> do_accept = [&] {
    acceptor.async_accept([&](beast::error_code ec, tcp::socket sock) {
      if (ec) {
        if (ec != asio::error::operation_aborted) do_accept();
        return;
      }
      auto s = std::make_shared<Session>(std::move(sock), bc, counters);
      bc.add(s);
      counters.sessions_accepted.fetch_add(1);
      s->run();
      do_accept();
    });
  };
  do_accept();

  std::ofstream out(cfg.out, std::ios::app);

  // 采样定时器（跑在 io 线程上，读计数不使用额外线程）
  Samples ser;  // 生产者线程独占写；采样时快照由生产者每 stats_interval_s 交换
  std::mutex ser_mu;
  Samples ser_pending;
  std::atomic<bool> swap_requested{false};

  auto last_cpu = cpu_seconds();
  int64_t t_start = now_ns();
  auto timer = std::make_shared<asio::steady_timer>(ioc);
  std::function<void()> sample = [&] {
    timer->expires_after(std::chrono::seconds(cfg.stats_interval_s));
    timer->async_wait([&](beast::error_code ec) {
      if (ec) return;
      // 取序列化样本
      Samples local_ser;
      {
        std::lock_guard lk(ser_mu);
        local_ser = std::move(ser_pending);
        ser_pending = Samples{};
      }
      Samples wl;
      {
        std::lock_guard lk(counters.mu);
        wl = std::move(counters.write_latency);
        counters.write_latency = Samples{};
      }
      auto cpu = cpu_seconds();
      const int64_t t = now_ns();
      const double dt = static_cast<double>(t - t_start) / 1e9;
      const double du = cpu.user - last_cpu.user;
      const double ds = cpu.sys - last_cpu.sys;
      last_cpu = cpu;
      json j = {
          {"t_s", dt},
          {"rss_kb", rss_kb()},
          {"cpu_user_pct", 100.0 * du / static_cast<double>(cfg.stats_interval_s)},
          {"cpu_sys_pct", 100.0 * ds / static_cast<double>(cfg.stats_interval_s)},
          {"evt_generated", counters.evt_generated.load()},
          {"evt_enqueued", counters.evt_enqueued.load()},
          {"evt_dropped", counters.evt_dropped.load()},
          {"bytes_sent", counters.bytes_sent.load()},
          {"write_calls", counters.write_calls.load()},
          {"queue_max", counters.queue_max.load()},
          {"sessions", bc.alive()},
          {"sessions_accepted", counters.sessions_accepted.load()},
          {"ser_p50_us", local_ser.size() ? static_cast<double>(local_ser.pct(0.50)) / 1000.0 : 0.0},
          {"ser_p99_us", local_ser.size() ? static_cast<double>(local_ser.pct(0.99)) / 1000.0 : 0.0},
          {"ser_n", local_ser.size()},
          {"wr_p50_us", wl.size() ? static_cast<double>(wl.pct(0.50)) / 1000.0 : 0.0},
          {"wr_p99_us", wl.size() ? static_cast<double>(wl.pct(0.99)) / 1000.0 : 0.0},
          {"wr_max_us", wl.size() ? static_cast<double>(wl.max()) / 1000.0 : 0.0},
          {"wr_n", wl.size()},
      };
      const std::string line = j.dump();
      out << line << '\n';
      out.flush();
      std::fprintf(stdout, "%s\n", line.c_str());
      std::fflush(stdout);
      sample();
    });
  };
  sample();

  // 生产者线程
  std::thread prod([&] {
    const int64_t start = now_ns();
    const int64_t period_ns = 1'000'000'000LL / cfg.rate;
    const int64_t total = static_cast<int64_t>(cfg.rate) * cfg.duration_s;
    int64_t last_swap = start;
    for (int64_t i = 0; i < total && !stop.load(std::memory_order_relaxed); ++i) {
      sleep_until_ns(start + (i + 1) * period_ns);
      const int64_t t0 = now_ns();
      const double ts_ms = static_cast<double>(t0 - start) / 1e6;
      std::string s = std::format(
          R"({{"seq":{},"ts_ns":{},"kind":"metric","src":"cam0","w":1280,"h":720,"fps":30.0,)"
          R"("frames":{},"box_cpu_pct":{:.2f},"queue_ms":{:.2f},"note":"cue spike-b synthetic event"}})",
          i, t0, i, 12.5 + static_cast<double>(i % 7) * 0.1, ts_ms);
      const int64_t t1 = now_ns();
      ser.add(t1 - t0);
      counters.evt_generated.fetch_add(1, std::memory_order_relaxed);
      auto sp = std::make_shared<const std::string>(std::move(s));
      asio::post(ioc, [&bc, sp, t1] { bc.broadcast(sp, t1); });
      if (t1 - last_swap >= int64_t(cfg.stats_interval_s) * 1'000'000'000LL) {
        std::lock_guard lk(ser_mu);
        ser_pending = std::move(ser);
        ser = Samples{};
        last_swap = t1;
      }
    }
    // 收尾：把最后一段样本交给采样器
    {
      std::lock_guard lk(ser_mu);
      ser_pending = std::move(ser);
      ser = Samples{};
    }
    stop.store(true);
  });

  int64_t deadline = now_ns() + int64_t(cfg.duration_s + 3) * 1'000'000'000LL;
  while (now_ns() < deadline) {
    ioc.run_for(std::chrono::milliseconds(500));
  }
  stop.store(true);
  if (prod.joinable()) prod.join();

  // 最终摘要
  {
    Samples local_ser;
    {
      std::lock_guard lk(ser_mu);
      local_ser = std::move(ser_pending);
    }
    Samples wl;
    {
      std::lock_guard lk(counters.mu);
      wl = std::move(counters.write_latency);
    }
    auto cpu = cpu_seconds();
    json j = {
        {"final", true},
        {"t_s", static_cast<double>(now_ns() - t_start) / 1e9},
        {"rss_kb", rss_kb()},
        {"cpu_user_s", cpu.user},
        {"cpu_sys_s", cpu.sys},
        {"evt_generated", counters.evt_generated.load()},
        {"evt_enqueued", counters.evt_enqueued.load()},
        {"evt_dropped", counters.evt_dropped.load()},
        {"bytes_sent", counters.bytes_sent.load()},
        {"write_calls", counters.write_calls.load()},
        {"queue_max", counters.queue_max.load()},
        {"sessions_accepted", counters.sessions_accepted.load()},
        {"ser_p50_us", local_ser.size() ? static_cast<double>(local_ser.pct(0.50)) / 1000.0 : 0.0},
        {"ser_p99_us", local_ser.size() ? static_cast<double>(local_ser.pct(0.99)) / 1000.0 : 0.0},
        {"wr_p50_us", wl.size() ? static_cast<double>(wl.pct(0.50)) / 1000.0 : 0.0},
        {"wr_p99_us", wl.size() ? static_cast<double>(wl.pct(0.99)) / 1000.0 : 0.0},
    };
    const std::string line = j.dump();
    out << line << '\n';
    out.flush();
    std::fprintf(stdout, "%s\n", line.c_str());
  }
  return 0;
}
