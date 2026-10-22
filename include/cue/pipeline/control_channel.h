// UDS 控制通道的**传输层**（技术设计文档 §4.3）：SOCK_SEQPACKET 优先，JSON-line 兜底。
//
// 为什么单独一层：报文形状（一条消息一个 JSON 对象 / 64 KiB 上限 / id 原样回填）与
// 「每个 op 做什么」是两件事。传输层要能用两种 socket 类型各测一遍往返，控制语义要能
// 在没有 cued 的情况下测；所以控制通道独立成库（cue_control），cued 与
// ProcessPipelineControl 共用同一份实现——两端各写一份分帧代码，迟早有一边漏掉上限。
//
// 约定（服务端与客户端都按这一份走）：
//  - SOCK_SEQPACKET：一条消息 = 一个 JSON 对象，不靠换行分帧（消息边界保留）；
//  - socket() 拿不到 SEQPACKET（容器/老内核/被 seccomp 拦）时退 SOCK_STREAM +
//    JSON-line（换行分帧），降级事实写进 ControlChannelKind，不假装还是 seqpacket；
//  - 单条消息上限 64 KiB：超限的请求一律 ok:false 拒绝，且**不断开连接**——
//    断开会让客户端在下一次请求时才失败，把「一条消息太大」误报成「控制通道挂了」。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace cue::pipeline {

// 单条控制消息上限（请求与响应各自适用）。64 KiB 足够装下 obs.fetch 的日志尾部，
// 又远小于任何会让人误以为「通道卡了」的尺寸。
inline constexpr std::size_t kMaxControlMessage = 64 * 1024;
// 一次请求-响应的默认超时。控制面卡住时必须失败得比运维动作的超时更早，
// 否则调用方看到的是「工具超时」而不是「控制通道不可用」。
inline constexpr int kControlIoTimeoutMs = 2000;

enum class ControlChannelKind : std::uint8_t { None = 0, SeqPacket, Stream };

// 指标里的控制通道名：uds / stdout（None 表示没有通道 → 只能用 stdout）。
const char* control_channel_tag(ControlChannelKind kind);
// 带实现细节的名字：uds:seqpacket / uds:stream / stdout。
// 降级到 STREAM 也要能看见——「协议一样、分帧方式不同」不能只活在注释里。
const char* control_channel_detail(ControlChannelKind kind);

// ---- 报文构造：两端共用，避免请求/响应形状各写一份 ----
nlohmann::json make_request(std::int64_t id, std::string_view op,
                            const nlohmann::json& args = nlohmann::json::object());
nlohmann::json make_ok(std::int64_t id, const nlohmann::json& result);
nlohmann::json make_error(std::int64_t id, std::string_view error);

// 解析请求头。缺 id / 缺 op / args 不是对象都算坏请求（写 err），但**不是**致命错误：
// 服务端要回一条 ok:false 然后继续服务下一条。
bool parse_request(const nlohmann::json& j, std::int64_t& id, std::string& op,
                   nlohmann::json& args, std::string& err);

// 每条连接暴露给 handler 的那一半能力：订阅 + 主动推送。
// handler 在连接自己的读线程里跑，所以这里的方法都不阻塞（推送用非阻塞写）。
class ControlSession {
 public:
  virtual ~ControlSession() = default;
  // events 为空 = 订阅全部事件（metric.sample 等）；非空 = 只推这些 ev 名。
  virtual void subscribe(std::vector<std::string> events) = 0;
  virtual void unsubscribe() = 0;
  virtual bool subscribed() const = 0;
  // 推一条消息（必须带 "event" 键；没有 id，客户端据此区分「推送」与「响应」）
  virtual bool push(const nlohmann::json& msg) = 0;
};

// UDS 控制服务端：监听一个路径，每条连接一个线程，handler 返回响应。
// 线程模型刻意简单：连接数是个位数（cued 只有 harness 一个客户端 + 少量调试），
// 每条连接一个线程换掉「一个事件循环里塞连接状态机」的复杂度是划算的。
class ControlServer {
 public:
  using Handler =
      std::function<nlohmann::json(const nlohmann::json& request, ControlSession& session)>;

  // preferred 只在测试与「强制走兜底路径」时需要：正常调用一律默认值。
  ControlServer(std::string socket_path, Handler handler,
                ControlChannelKind preferred = ControlChannelKind::SeqPacket);
  ~ControlServer();

  ControlServer(const ControlServer&) = delete;
  ControlServer& operator=(const ControlServer&) = delete;

  // 建目录（0700）+ bind + listen + 起 accept 线程。失败时 err 有可操作原因
  // （路径过长 / 目录属于别人 / bind 被拒），调用方据此降级到 stdout。
  bool start(std::string& err);
  void stop();
  bool running() const { return fd_ >= 0; }
  ControlChannelKind kind() const { return kind_; }
  const std::string& socket_path() const { return path_; }
  // socket() 退到 STREAM 的原因（空 = 没降级）。降级不能是无声的。
  const std::string& fallback_reason() const { return fallback_reason_; }

  // 事件广播：body 形如 {"event":"...","data":{...}}。
  // 调用方是媒体线程，所以这里绝不能阻塞在慢客户端上：每个连接一次非阻塞写，
  // 写不动就摘掉它的订阅（丢一次推送比卡住整条媒体管线便宜得多）。
  std::size_t publish(const nlohmann::json& body);

 private:
  struct Conn;
  void accept_loop();
  void serve(const std::shared_ptr<Conn>& conn);
  void handle_message(const std::shared_ptr<Conn>& conn, std::string_view raw);
  void detach_conn(const std::shared_ptr<Conn>& conn);
  bool write_reply(const std::shared_ptr<Conn>& conn, const nlohmann::json& j);

  std::string path_;
  Handler handler_;
  ControlChannelKind preferred_;
  ControlChannelKind kind_ = ControlChannelKind::None;
  std::string fallback_reason_;
  int fd_ = -1;
  std::atomic<bool> stop_{false};
  std::thread accept_thread_;

  // 全部活动连接：stop() 用它 shutdown，publish() 用它找订阅者。
  // 不另存一份「订阅者表」：连接数是个位数，过滤 subscribed() 比维护两张表少一类竞态。
  mutable std::mutex conns_mu_;
  std::vector<std::shared_ptr<Conn>> conns_;
  std::atomic<int> active_{0};
  std::mutex idle_mu_;
  std::condition_variable idle_cv_;
};

// UDS 控制客户端：一次一个请求（一个 fd 上不并发），请求-响应按 id 配对。
// 无 id 的消息是服务端主动推送的订阅事件，交给回调——不被当成响应。
class ControlClient {
 public:
  ControlClient() = default;
  ~ControlClient();

  ControlClient(const ControlClient&) = delete;
  ControlClient& operator=(const ControlClient&) = delete;

  // preferred 同服务端：测试里要显式指定；正常路径用默认值即可。
  bool connect(const std::string& path, int timeout_ms, std::string& err,
               ControlChannelKind preferred = ControlChannelKind::SeqPacket);
  void close();
  bool connected() const { return fd_ >= 0; }
  ControlChannelKind kind() const { return kind_; }

  // 订阅事件到达时的回调（在调用 request 的线程里跑）。空回调 = 丢弃推送。
  void set_event_callback(std::function<void(const nlohmann::json&)> cb);

  // 发一条请求并等回 id 相同的响应。服务端 ok:false → 返回 false 且 err 是它的 error 原文
  // （控制面的失败必须原样透出，不能改写成「内部错误」）。
  bool call(std::string_view op, const nlohmann::json& args, nlohmann::json& result,
            std::string& err, int timeout_ms = kControlIoTimeoutMs);
  bool request(const nlohmann::json& req, nlohmann::json& resp, std::string& err,
               int timeout_ms = kControlIoTimeoutMs);

  // 已发出的请求数（含失败）。用于指标 control_sent：与 cued 侧处理数对照，
  // 差得太多说明有请求死在通道上。
  std::uint64_t sent() const { return sent_.load(); }

 private:
  bool write_msg(const nlohmann::json& j, int timeout_ms, std::string& err);
  // 阻塞读一条完整消息（stream 模式下按换行累计）。超时/EOF 都返回 false。
  bool read_msg(std::string& out, int timeout_ms, std::string& err);
  void mark_broken(const std::string& why);

  int fd_ = -1;
  ControlChannelKind kind_ = ControlChannelKind::None;
  mutable std::mutex mu_;  // 串行化请求：一个 fd 一次一个在途请求
  std::string rbuf_;       // stream 模式的分帧残留
  std::int64_t next_id_ = 1;
  std::atomic<std::uint64_t> sent_{0};
  std::function<void(const nlohmann::json&)> on_event_;
  std::string broken_reason_;
};

}  // namespace cue::pipeline
