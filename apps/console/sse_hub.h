// SseHub：控制台的 SSE 事件广播
//
// 三件事：
//  1. 给每条事件分配单调 id（浏览器断线重连时按 Last-Event-ID 补齐，SPEC §6 M5）
//  2. 保留环形缓冲（cap 默认 512 条），既供补齐也供 /api/state 的"最近事件"
//  3. 广播给所有订阅者——回调必须非阻塞（订阅方只做入队），否则会把
//     cued reader 线程拖住（publish 的调用方之一就是它）
//
// 关闭哨兵：shutdown() 后向订阅者回调空串，SSE 连接线程据此退出写循环。
#pragma once

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace cue::console {

// 事件时间用墙钟微秒（给人看/给前端排序）；延迟测量仍只准用 cue::now_us()
inline int64_t wall_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// SSE 帧：id 字段 + 单行 JSON data。浏览器 EventSource 以空行分隔事件。
inline std::string sse_frame(const std::string& cursor, const std::string& payload) {
  return "id: " + cursor + "\ndata: " + payload + "\n\n";
}

class SseHub {
 public:
  explicit SseHub(std::size_t cap = 512) : cap_(cap == 0 ? 1 : cap) {}

  const std::string& generation() const { return generation_; }
  std::string cursor(uint64_t id) const { return generation_ + ":" + std::to_string(id); }

  // A foreign generation always starts at the retained ring. Legacy numeric cursors
  // remain supported, but cannot identify a restart when their id is still in range.
  uint64_t resume_id(const std::string& cursor) const {
    std::string_view number(cursor);
    const auto colon = number.find(':');
    if (colon != std::string_view::npos) {
      if (number.substr(0, colon) != generation_) return 0;
      number.remove_prefix(colon + 1);
    }
    uint64_t id = 0;
    const auto [end, ec] = std::from_chars(number.data(), number.data() + number.size(), id);
    if (ec != std::errc{} || end != number.data() + number.size()) return 0;
    return id > last_id() ? 0 : id;
  }

  // 发布：入缓冲 + 广播。返回事件 id（关闭后仍然编号，避免前端把 id 当序号用错）。
  uint64_t publish(const std::string& ev, nlohmann::json data) {
    if (!data.is_object()) data = nlohmann::json::object();
    nlohmann::json payload{{"ev", ev}, {"t", wall_us()}, {"data", std::move(data)}};
    std::vector<std::function<void(const std::string&)>> cbs;
    uint64_t id = 0;
    std::string text;
    {
      std::lock_guard<std::mutex> lk(mu_);
      id = next_id_++;
      payload["id"] = id;
      payload["generation"] = generation_;
      text = payload.dump();
      ring_.push_back(Frame{id, text, sse_frame(cursor(id), text)});
      while (ring_.size() > cap_) ring_.pop_front();
      if (closed_) return id;  // 关闭后只留档，不广播
      cbs.reserve(subs_.size());
      for (const auto& [_, cb] : subs_) cbs.push_back(cb);
    }
    for (const auto& cb : cbs) cb(text);
    return id;
  }

  uint64_t subscribe(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> lk(mu_);
    const uint64_t sid = next_sub_++;
    subs_.emplace(sid, std::move(cb));
    if (closed_) subs_[sid](std::string());
    return sid;
  }

  void unsubscribe(uint64_t sid) {
    std::lock_guard<std::mutex> lk(mu_);
    subs_.erase(sid);
  }

  // Last-Event-ID 补齐：返回 id > last_id 的完整 SSE 帧（按时间序）。
  // last_id 太旧（已被环形缓冲淘汰）时只能给现存最旧的——前端必须能从"不连续"恢复，
  // 所以补偿帧之外还会得到 /api/state 的全量快照。
  std::vector<std::string> replay_after(uint64_t last_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    for (const auto& f : ring_) {
      if (f.id > last_id) out.push_back(f.text);
    }
    return out;
  }

  std::vector<nlohmann::json> recent(std::size_t n) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<nlohmann::json> out;
    const std::size_t start = n == 0 || n >= ring_.size() ? 0 : ring_.size() - n;
    for (std::size_t i = start; i < ring_.size(); ++i) out.push_back(nlohmann::json::parse(ring_[i].payload, nullptr, false));
    return out;
  }

  uint64_t last_id() const {
    std::lock_guard<std::mutex> lk(mu_);
    return next_id_ - 1;
  }

  // 当前在线订阅者数（/metrics 的 cue_serve_sse_clients）。慢消费者被丢弃的帧数不在这里：
  // 那是每条连接自己的 deque（Server::Sink::dropped），口径不同，不能混成一个数。
  std::size_t subscriber_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return subs_.size();
  }

  // 关闭：先清订阅（等 SSE 线程退订/退出），再通知所有还挂着的订阅者收到空串
  void shutdown() {
    std::vector<std::function<void(const std::string&)>> cbs;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (closed_) return;
      closed_ = true;
      for (const auto& [_, cb] : subs_) cbs.push_back(cb);
      subs_.clear();
    }
    for (const auto& cb : cbs) cb(std::string());
  }

  bool closed() const {
    std::lock_guard<std::mutex> lk(mu_);
    return closed_;
  }

 private:
  static std::string make_generation() {
    static std::atomic<uint64_t> serial{0};
    // Wall and monotonic nanoseconds distinguish process lifetimes; serial also
    // distinguishes multiple hubs constructed in the same process/clock tick.
    return std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) +
           "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
           "-" + std::to_string(serial.fetch_add(1));
  }
  const std::string generation_ = make_generation();
  struct Frame {
    uint64_t id = 0;
    std::string payload;  // JSON 原文（recent 用）
    std::string text;     // 完整 SSE 帧（replay 用）
  };

  mutable std::mutex mu_;
  std::size_t cap_;
  uint64_t next_id_ = 1;   // 从 1 开始：Last-Event-ID 缺省是 uint64，0 表示"没有"
  uint64_t next_sub_ = 1;
  bool closed_ = false;
  std::deque<Frame> ring_;
  std::map<uint64_t, std::function<void(const std::string&)>> subs_;
};

}  // namespace cue::console
