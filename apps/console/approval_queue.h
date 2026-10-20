// ApprovalQueue：Ask 模式的待审批队列
//
// 语义（SPEC §7.4 + 任务 A.2）：
//  - 非只读工具在 Ask 模式下必须进队列，SSE 推 approval.request
//  - POST /api/approve {id, allow} 决定；等待方（工具调用所在线程）被唤醒
//  - 超时（默认 30s）自动拒绝——不能因为人不在就把危险动作挂住或放行
//  - 进程退出（shutdown）时所有等待者立即按拒绝返回，不阻塞 join
//
// 为什么用 shared_ptr<Slot>：decide() 与等待方不共享同一把锁的生存期，
// 等待方判断时只碰自己那份 slot（decided/allowed 都由 mu_ 保护）。
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "sse_hub.h"  // wall_us（同目录）

namespace cue::console {

class ApprovalQueue {
 public:
  enum class Decision { Allowed, Denied, Timeout, Shutdown };

  struct Item {
    uint64_t id = 0;
    std::string tool;
    std::string risk;
    nlohmann::json args = nlohmann::json::object();
    int64_t created_us = 0;
    int64_t timeout_ms = 0;
  };

  struct Outcome {
    Decision decision = Decision::Denied;
    Item item;
  };

  struct History {
    Item item;
    std::string outcome;  // allowed / denied / timeout / shutdown
    int64_t decided_us = 0;
  };

  // 阻塞等待审批。on_request 在"已入队、尚未决定"时调用（推 SSE），不持锁。
  Outcome request(Item item, std::chrono::milliseconds timeout,
                  const std::function<void(const Item&)>& on_request) {
    std::shared_ptr<Slot> slot;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (closed_) return Outcome{Decision::Shutdown, item};
      item.id = next_id_++;
      item.created_us = wall_us();
      item.timeout_ms = static_cast<int64_t>(timeout.count());
      slot = std::make_shared<Slot>();
      slot->item = item;
      waiting_.emplace(item.id, slot);
    }
    if (on_request) on_request(item);
    std::unique_lock<std::mutex> lk(mu_);
    const bool settled = cv_.wait_for(lk, timeout, [&] { return slot->decided || closed_; });
    Decision d;
    if (slot->decided) {
      d = slot->allowed ? Decision::Allowed : Decision::Denied;
    } else if (closed_) {
      d = Decision::Shutdown;
    } else {
      (void)settled;
      d = Decision::Timeout;  // 超时：没有决定就是拒绝
    }
    waiting_.erase(item.id);
    history_.push_back(History{slot->item, outcome_name(d), wall_us()});
    while (history_.size() > kHistoryMax) history_.pop_front();
    return Outcome{d, slot->item};
  }

  // 决定一条待审批；返回 false 表示 id 不存在或已决定（HTTP 侧回 404/409）
  bool decide(uint64_t id, bool allow) {
    std::shared_ptr<Slot> slot;
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto it = waiting_.find(id);
      if (it == waiting_.end()) return false;
      slot = it->second;
      if (slot->decided) return false;
      slot->decided = true;
      slot->allowed = allow;
    }
    cv_.notify_all();
    return true;
  }

  std::vector<Item> pending() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Item> out;
    for (const auto& [_, slot] : waiting_) {
      if (!slot->decided) out.push_back(slot->item);
    }
    return out;
  }

  std::vector<History> recent(std::size_t n = 20) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<History> out;
    const std::size_t start = n == 0 || n >= history_.size() ? 0 : history_.size() - n;
    for (std::size_t i = start; i < history_.size(); ++i) out.push_back(history_[i]);
    return out;
  }

  void shutdown() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (closed_) return;
      closed_ = true;
    }
    cv_.notify_all();
  }

  static const char* outcome_name(Decision d) {
    switch (d) {
      case Decision::Allowed: return "allowed";
      case Decision::Denied: return "denied";
      case Decision::Timeout: return "timeout";
      case Decision::Shutdown: return "shutdown";
    }
    return "unknown";
  }

 private:
  struct Slot {
    Item item;
    bool decided = false;
    bool allowed = false;
  };

  static constexpr std::size_t kHistoryMax = 64;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  uint64_t next_id_ = 1;
  bool closed_ = false;
  std::map<uint64_t, std::shared_ptr<Slot>> waiting_;
  std::deque<History> history_;
};

}  // namespace cue::console
