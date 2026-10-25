// 令牌桶（SPEC §7.2 / FR-PIPE.8）：默认 10 Mbps 速率 + 512 包突发，包大小按
// 「1200 B payload + 24 B 头」折算成包速率；不足时先丢非关键帧，关键帧可透支至 −512 包。
//
// 为什么放在 media 层（N-07）：这段实现原本内联在 apps/cued.cpp 的匿名命名空间里，
// 只有起 cued、观察 budget_drops 才能间接验证它。搬家是纯移动——语义、常量与浮点
// 运算顺序逐字节不变，目的只是让它能被 tests/test_token_bucket.cpp 直接实例化。
//
// 口径按**包**计费（包大小按 kMaxUdpPayload + 24 B 头折算成包速率），这样 24 B/包的
// 头开销不会被漏算；突发是一个常量包数，而不是"5% 窗口字节数"。
// 不足时先丢非关键帧：关键帧允许透支，但透支上限仍是有限的，关键帧风暴不能无限超发。
//
// 突发量为什么是 512 而不是规格最初写的 64：64 包 ≈ 76 KB，隐含假设"一帧的分片数
// 不超过 64"，而 1080p 的 IDR 有 100~200 个分片 —— 令牌一空整帧关键帧被丢，
// 每个 GOP 边界断一次参考链（实测 budget_drops 6 Mbps 时 11 次、12 Mbps 时 41 次，
// 码率越高丢得越多，因为 IDR 更大）。突发量是**容忍度**不是速率，取 512 仍是有界的。
// 为什么不按"码率 × 时长"推导：要覆盖的是**单个关键帧的大小**，而它与平均码率不是
// 线性关系（同一码率下 Keyframe 大小随场景剧烈变化），按时间算反而算不准。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "cue/proto/wire.h"

namespace cue::media {

class TokenBucket {
 public:
  // 关键帧允许透支的包数上限。
  //
  // 512 包 ≈ 600 KB，足以覆盖 1080p/4K 的单个 IDR；真正的限速靠 refill()，
  // 不是这个上限。上限本身的取值理由见类上方注释（那里也写了被丢的关键帧会造成什么）。
  static constexpr double kBurstPackets = 512.0;
  static constexpr double kPacketBytes =
      double(cue::proto::kMaxUdpPayload + cue::proto::kDatagramHeaderSize);

  explicit TokenBucket(double bits_per_sec);

  // 运行期改速率上限（fault.set kind=rate）：速率是令牌桶唯一的可变量，
  // 原地改不会丢令牌、也不打乱突发——所以这一步不需要重建任何东西。
  void set_rate(double bits_per_sec);

  // 非关键帧：令牌不够就丢（不排队）
  bool take_nonkey(std::size_t pkts, int64_t now_us);
  // 关键帧：允许透支到 -burst，保证"先丢非关键帧"这条语义真的成立
  bool take_key(std::size_t pkts, int64_t now_us);

 private:
  void refill(int64_t now_us);

  // 控制线程写、媒体线程读：atomic<double> 在 x86-64 上无锁，读到的要么是旧速率
  // 要么是新速率，不会出现"半个 double"（那会让令牌桶按乱码速率灌令牌）。
  std::atomic<double> rate_pkts_{0.0};
  double tokens_ = kBurstPackets;
  int64_t last_us_ = 0;
};

}  // namespace cue::media
