// 延迟注入的决策状态（FR-PIPE.10 的 delay / jitter 两类）。
//
// 为什么单独抽一个类型，而不是塞进 cued 的 process_frame lambda：
// 抖动决策的正确性判据是**可复现**（同一 seed → 同一串额外延迟），而 cued 在 ctest 里
// 起不来（要 FFmpeg + WS 推流）。这里只做算术：不读时钟、不碰 socket、不分配网络
// 资源，单测可以把「同一 seed 两次决策逐项相等」钉死。
//
// 分布选择：**均匀 [0, jitter_ms]，单侧**。理由：
//  1) 物理上「负延迟」不存在——数据报只能被本地压住，不能被提前发出去。双侧 ±value
//     要实现就得先人为延迟 value 再往回抵消一部分，等于给所有包加一份固定延迟，
//     度量上会污染 delay 与 jitter 的区分（delay 不该被 jitter 偷偷加出来）。
//  2) 单侧抖动是「额外延迟的离散度」：注入它只涨 delayed_pkts / send 段分位，
//     与丢包（injected / budget_drops）在计数上完全不相交。
//
// 线程模型：只用一条线程（cued 的媒体线程按帧调用）。所以这里没有锁，也**不能**
// 跨线程共享；两条流各自一份实例，各自的序列互不影响。
#pragma once

#include <cstdint>
#include <random>

namespace cue::media {

class DelayInjector {
 public:
  // 参数同步。值没变就什么都不做（保留序列位置）；值一变就按 seed 重建引擎。
  //
  // 为什么是"变就重建"：运行期改注入（fault.set）不清空历史状态的话，
  // 「先 delay=200、清零、再 delay=200」两次注入会得到两段不同的抖动序列，
  // 可复现的承诺就只对进程第一次注入成立。重建让每次"注入生效的那一帧"成为
  // 序列的起点：同一 (seed, delay_ms, jitter_ms) 永远从第 0 个决策开始。
  void sync(std::uint64_t seed, double delay_ms, double jitter_ms) {
    if (inited_ && seed == seed_ && delay_ms == delay_ms_ && jitter_ms == jitter_ms_) return;
    seed_ = seed;
    delay_ms_ = delay_ms;
    jitter_ms_ = jitter_ms;
    // 与丢包判定的 splitmix64 用不同的域标签：两条链路共用同一次 --inject-seed，
    // 但丢弃序列与延迟序列必须互不相关（否则"该丢的包恰好被压得最久"会变成
    // 一个与网络无关的伪相关）。
    rng_.seed(seed ^ kDomainTag);
    inited_ = true;
  }

  // 注入是否生效（两个值都是 0 = 不压任何包，走原来的直发路径）。
  bool active() const { return delay_ms_ > 0.0 || jitter_ms_ > 0.0; }

  // 下一个数据报要额外压多少微秒 = 固定延迟 + 均匀 [0, jitter_ms]。
  // 每次调用恰好消费一个随机数（jitter_ms == 0 时不消费），因此「第几个数据报」
  // 与「第几次抽取」一一对应——trace 里落的就是这个序列。
  std::int64_t extra_us() {
    std::int64_t jitter = 0;
    if (jitter_ms_ > 0.0) {
      const auto max_us = static_cast<std::uint64_t>(jitter_ms_ * 1000.0 + 0.5);
      // 手写取模而不是 std::uniform_int_distribution：标准没有规定后者的算法
      // （同一 seed 在不同标准库上可以给出不同序列），而这里的承诺就是
      // 「同一 seed 同一序列」。max_us ≤ 2e6 远小于 2^64，取模偏差约 1e-13，可忽略。
      jitter = static_cast<std::int64_t>(rng_() % (max_us + 1));
    }
    return static_cast<std::int64_t>(delay_ms_ * 1000.0 + 0.5) + jitter;
  }

  double delay_ms() const { return delay_ms_; }
  double jitter_ms() const { return jitter_ms_; }

 private:
  // 任意固定常量：只要与 should_drop_pkt 的 splitmix64 派生方式不同即可。
  static constexpr std::uint64_t kDomainTag = 0x6A09E667F3BCC909ULL;

  std::mt19937_64 rng_{0};
  std::uint64_t seed_ = 0;
  double delay_ms_ = -1.0;
  double jitter_ms_ = -1.0;
  bool inited_ = false;
};

}  // namespace cue::media
