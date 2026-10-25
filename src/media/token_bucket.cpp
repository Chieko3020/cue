// 令牌桶实现（SPEC §7.2 / FR-PIPE.8）：取值理由、口径与语义约束都在头文件注释里，
// 这里只放定义——头文件要能让人不跳文件就读明白"为什么是 512 包"。
//
// 语义（搬家时逐条保留，不许顺手优化）：
//   1) 非关键帧令牌不足即丢，不排队；
//   2) 关键帧允许透支到 -kBurstPackets，保证"先丢非关键帧"成立；
//   3) 时间回拨时 dt 取 0，令牌不会负增长；
//   4) rate_pkts_ 是 atomic<double>，控制线程写、媒体线程读；
//   5) 首次调用 last_us_ == 0 时把它设成 now_us，避免第一个包的 dt 是天文数字。
#include "cue/media/token_bucket.h"

#include <algorithm>

namespace cue::media {

TokenBucket::TokenBucket(double bits_per_sec) { set_rate(bits_per_sec); }

void TokenBucket::set_rate(double bits_per_sec) {
  rate_pkts_.store(bits_per_sec / 8.0 / kPacketBytes, std::memory_order_relaxed);
}

bool TokenBucket::take_nonkey(std::size_t pkts, int64_t now_us) {
  refill(now_us);
  if (tokens_ < double(pkts)) return false;
  tokens_ -= double(pkts);
  return true;
}

bool TokenBucket::take_key(std::size_t pkts, int64_t now_us) {
  refill(now_us);
  if (tokens_ - double(pkts) < -kBurstPackets) return false;
  tokens_ -= double(pkts);
  return true;
}

void TokenBucket::refill(int64_t now_us) {
  if (last_us_ == 0) last_us_ = now_us;
  // 时间回拨（NTP/CLOCK_MONOTONIC 不会，但参数注入的假时钟可能）不能让 dt 变负
  const double dt = now_us > last_us_ ? double(now_us - last_us_) / 1e6 : 0.0;
  last_us_ = now_us;
  tokens_ = std::min(kBurstPackets, tokens_ + dt * rate_pkts_.load(std::memory_order_relaxed));
}

}  // namespace cue::media
