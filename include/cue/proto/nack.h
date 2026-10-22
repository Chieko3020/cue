// NACK 发送策略（FR-PROBE.2 / SPEC §7.2）：限速 + 冷却 + 单包探测。
//
// 为什么把策略从 cue-probe.cpp 的 main() 里抽出来：
//  - 限速边界（8 包/秒、窗口内 8 包）与冷却状态机（3 个窗口 >10% → 暂停 1 s →
//    单包探测 → 成功恢复常规）都是**纯决策**，不碰 socket，也不读时钟；
//  - 把它们放在 main() 里只能用「起进程 + 注入真丢包」来验证，边界条件（恰好第 8 包、
//    冷却结束的那一微秒、探测成功后立刻复位）几乎测不到。
//
// 语义约定：
//  - 限速用**令牌桶**（默认 8 令牌/秒、容量 1，即不允许突发），外加「每个统计窗口
//    最多 max_per_window 个 NACK 包」的硬上限。why 两个都要：令牌桶管长期平均速率，
//    窗口上限保证 1 s 指标行上的 nack_sent 增量不超过 8（令牌桶在窗口边界上可能
//    出现 9 的读数）。
//  - 被限速拦下的 NACK **不排队**：调用方只是这一拍没发，帧的缺片状态仍在 pending 里，
//    下一拍会重新算缺口再试。排队得拷贝条目，而缺口本身已经是状态，再存一份只会有
//    两份真相。
//  - 冷却期间完全不发 NACK（包括单包探测）；冷却结束后进入「单包探测」：每个统计窗口
//    只发 1 条 NACK（1 个条目）。探测成功（调用方确认缺片被补回）→ 恢复常规。
#pragma once

#include <algorithm>
#include <cstdint>

namespace cue::proto {

// 令牌桶：rate 个令牌/秒，容量 1（严格匀速，不允许突发）。
class NackRateLimiter {
 public:
  explicit NackRateLimiter(double packets_per_sec = 8.0) : rate_(packets_per_sec) {
    if (!(rate_ > 0.0)) rate_ = 8.0;
    rate_ = std::min(rate_, 8.0);  // SPEC §7.2 的上限是硬上限，不是默认值
  }

  bool take(int64_t now_us) {
    if (!started_) {
      started_ = true;
      last_us_ = now_us;
      tokens_ = 1.0;
    } else if (now_us > last_us_) {
      const double dt = double(now_us - last_us_) / 1e6;
      tokens_ = std::min(1.0, tokens_ + dt * rate_);
      last_us_ = now_us;
    }
    // 加一个极小 epsilon：dt 恰好等于 1/rate 时浮点可能算成 0.9999999
    if (tokens_ + 1e-9 < 1.0) return false;
    tokens_ = std::max(0.0, tokens_ - 1.0);
    return true;
  }

  double rate() const { return rate_; }

 private:
  double rate_ = 8.0;
  double tokens_ = 1.0;
  int64_t last_us_ = 0;
  bool started_ = false;
};

class NackController {
 public:
  enum class Mode { Normal, Throttled, Paused };

  static const char* mode_name(Mode m) {
    switch (m) {
      case Mode::Normal: return "normal";
      case Mode::Throttled: return "throttled";
      case Mode::Paused: return "paused";
    }
    return "normal";
  }

  struct Config {
    double packets_per_sec = 8.0;
    double bad_loss = 0.10;         // 丢包率高于它算「坏窗口」
    int bad_windows_to_pause = 3;   // 连续这么多个坏窗口 → 暂停
    int64_t cooldown_us = 1'000'000;
    int max_per_window = 8;         // 每个统计窗口最多发几个 NACK 包
  };

  // 两个构造函数：默认构造不能写成 `NackController(Config cfg = Config{})`——
  // C++ 规定默认实参不能在嵌套类的默认成员初始化器完成之前求值（GCC 直接报错）。
  NackController() = default;
  explicit NackController(const Config& cfg) : cfg_(cfg), limiter_(cfg.packets_per_sec) {
    if (cfg_.max_per_window < 1) cfg_.max_per_window = 1;
    if (cfg_.cooldown_us < 0) cfg_.cooldown_us = 0;
  }

  // 每个 1 s 统计窗口结束时调用一次：expected/received 是该窗口的包数。
  // 冷却中不累计坏窗口（否则冷却一结束就立刻再次暂停，永远出不来）。
  void observe_loss_window(uint64_t expected, uint64_t received, int64_t now_us) {
    last_loss_ = expected > 0 ? double(expected - std::min(expected, received)) / double(expected)
                              : 0.0;
    if (expected == 0) return;
    if (now_us < pause_until_us_) return;
    if (last_loss_ > cfg_.bad_loss) {
      if (++bad_windows_ >= cfg_.bad_windows_to_pause) {
        pause_until_us_ = now_us + cfg_.cooldown_us;
        paused_ms_ += static_cast<uint64_t>(cfg_.cooldown_us / 1000);
        ++pauses_;
        bad_windows_ = 0;
        probe_pending_ = true;
      }
    } else {
      bad_windows_ = 0;
    }
  }

  // 每个统计窗口（与指标行同一拍）开始时清零窗口内计数。
  void begin_window(int64_t /*now_us*/) {
    sent_this_window_ = 0;
    suppressed_this_window_ = 0;
    probe_window_used_ = false;
  }

  bool paused(int64_t now_us) const { return now_us < pause_until_us_; }
  // 冷却已过、且还没探测成功：进入单包探测模式。
  bool probing(int64_t now_us) const { return probe_pending_ && !paused(now_us); }
  bool probe_window_used() const { return probe_window_used_; }

  // 请求发送一个 NACK 包。probe=true 时按「每窗口 1 条」的探测额度。
  // 返回 false 表示这一拍不发（被限速/冷却/探测额度挡住）；调用方只需下一拍重试。
  bool try_acquire(int64_t now_us, bool probe) {
    if (paused(now_us)) {
      ++suppressed_;
      ++suppressed_this_window_;
      return false;
    }
    if (probe) {
      if (probe_window_used_) return false;  // 额度用完：不重复计 suppressed
      if (sent_this_window_ >= cfg_.max_per_window) {
        ++suppressed_;
        ++suppressed_this_window_;
        return false;
      }
      if (!limiter_.take(now_us)) {
        ++suppressed_;
        ++suppressed_this_window_;
        return false;
      }
      probe_window_used_ = true;
      ++sent_;
      ++sent_this_window_;
      return true;
    }
    if (sent_this_window_ >= cfg_.max_per_window || !limiter_.take(now_us)) {
      ++suppressed_;
      ++suppressed_this_window_;
      return false;
    }
    ++sent_;
    ++sent_this_window_;
    return true;
  }

  // 探测成功（缺片确实回来了）：退出探测模式，恢复常规限速。
  void on_probe_success() {
    probe_pending_ = false;
    bad_windows_ = 0;
  }

  Mode mode(int64_t now_us) const {
    if (paused(now_us)) return Mode::Paused;
    if (probing(now_us) || suppressed_this_window_ > 0) return Mode::Throttled;
    return Mode::Normal;
  }

  uint64_t sent() const { return sent_; }
  uint64_t suppressed() const { return suppressed_; }
  uint64_t paused_ms() const { return paused_ms_; }
  uint64_t pauses() const { return pauses_; }
  double last_window_loss() const { return last_loss_; }
  int bad_windows() const { return bad_windows_; }

 private:
  Config cfg_;
  NackRateLimiter limiter_;
  int64_t pause_until_us_ = 0;
  int bad_windows_ = 0;
  bool probe_pending_ = false;
  bool probe_window_used_ = false;
  int sent_this_window_ = 0;
  int suppressed_this_window_ = 0;
  uint64_t sent_ = 0;
  uint64_t suppressed_ = 0;
  uint64_t paused_ms_ = 0;
  uint64_t pauses_ = 0;
  double last_loss_ = 0.0;
};

}  // namespace cue::proto
