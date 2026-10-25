// 令牌桶单测（技术设计文档 §13.1「media/token_bucket」那行）：N-07 把实现从
// apps/cued.cpp 的匿名命名空间搬到 media 层之后补上的七条语义。
//
// 为什么全用假时钟：令牌桶的每条规则都是「时间 × 速率」的比较，用真时钟只能拿 sleep
// 逼近边界，既慢又不稳。TokenBucket 不读时钟、不起线程，时间从参数进来，所以
// 「1 s 补多少包」「回拨 1 s 会不会倒扣」这类断言可以逐条钉死。
//
// 七条锁定的是：
//   1) 速率生效（1 s 补 ~10e6/8/kPacketBytes 包，不是无限发）；
//   2) 非关键帧令牌不足即丢，且失败不扣令牌；
//   3) 关键帧可透支，总额度正好 -kBurstPackets；
//   4) 「先丢非关键帧」——透支后非关键帧先被拒，关键帧风暴也受 512 包兜底；
//   5) 时间回拨 dt 取 0，令牌不减、不变 NaN；
//   6) 首次调用 last_us_ == 0 被设成 now_us，不会补一笔"从 0 到现在"的巨量令牌；
//   7) set_rate 立刻按新速率补，且不重置突发、不丢已有令牌。
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "cue/media/token_bucket.h"

namespace {

int failures = 0;

void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

using cue::media::TokenBucket;

// 10 Mbps 下每秒能补出来的包数（kPacketBytes = 1200 + 24 = 1224）。
constexpr double kRate10Mbps = 10e6 / 8.0 / TokenBucket::kPacketBytes;
constexpr double kRate1Mbps = 1e6 / 8.0 / TokenBucket::kPacketBytes;

// 把桶耗空（返回成功取走的包数）。上限保护：逻辑写坏时不至于把 ctest 挂死。
std::size_t drain(TokenBucket& b, int64_t now_us) {
  std::size_t n = 0;
  while (b.take_nonkey(1, now_us) && n < 100'000) ++n;
  return n;
}

// 同一个时刻反复取 1 包，数到失败为止（上限保护同上）。
std::size_t count_take_nonkey(TokenBucket& b, int64_t now_us) {
  std::size_t n = 0;
  while (b.take_nonkey(1, now_us) && n < 100'000) ++n;
  return n;
}

// ---- 1. 速率生效 ----
// 先把满桶 512 包耗空，再数 1 s 的假时钟推进总共能补出多少包：应约等于
// 10e6/8/kPacketBytes ≈ 1021，而不是"无限发"。±1 包留给浮点边界。
//
// 注意 1 s 必须拆成小步推进：令牌桶的补令牌被 min(kBurstPackets) 封顶，一次推进
// 1 s 只会补到 512（0.5 s 就满了），测出来的是突发上限而不是速率。拆成 10 个 100 ms
// 步长、每步取空，累计值才等于 1 s 的真实产能（每步 ~102 包，远低于封顶）。
void test_rate_takes_effect() {
  TokenBucket b(10e6);
  const int64_t t0 = 1'000'000'000;
  check(drain(b, t0) == std::size_t(TokenBucket::kBurstPackets),
        "速率：满桶初始令牌恰好是 burst（512 包）");

  std::size_t n = 0;
  for (int step = 1; step <= 10; ++step) {
    n += count_take_nonkey(b, t0 + int64_t(step) * 100'000);
  }
  check(std::abs(double(n) - kRate10Mbps) <= 1.0,
        "速率：10 Mbps 下 1 s 累计只能补 ~10e6/8/kPacketBytes 包（±1），不是无限发");
  check(n < 2'000, "速率：1 s 的补包数远小于无限，限速真的生效");
}

// ---- 2. 非关键帧不足即丢，且失败不扣令牌 ----
// 令牌耗到 0 后推进 2 ms（≈2.04 包）：要 3 包必须失败；随后要 2 包必须成功。
// 后者正是"失败那次没有扣令牌"的证据——若失败也扣，这里会变成 -0.96 而继续失败。
void test_nonkey_drop_does_not_charge() {
  TokenBucket b(10e6);
  const int64_t t0 = 1'000'000'000;
  drain(b, t0);
  check(!b.take_nonkey(3, t0 + 2'000), "非关键帧：令牌不足一帧即丢（不排队）");
  check(b.take_nonkey(2, t0 + 2'000), "非关键帧：失败未扣令牌，2.04 包还能取 2 包");
}

// ---- 3. 关键帧可透支，总额度正好 -kBurstPackets ----
// 令牌为 0 时 take_key 仍成功，累计透支到 -512 包为止；再往后 take_key 也返回 false，
// 且失败不继续扣（桶停在 -512，不变 NaN）。
void test_key_can_overdraw() {
  TokenBucket b(10e6);
  const int64_t t0 = 1'000'000'000;
  drain(b, t0);
  check(b.take_key(1, t0), "关键帧：令牌为 0 时仍可透支发出");

  std::size_t key_ok = 0;
  while (b.take_key(1, t0) && key_ok < 10'000) ++key_ok;
  check(key_ok == std::size_t(TokenBucket::kBurstPackets) - 1,
        "关键帧：透支总额度正好是 kBurstPackets（首包之外还剩 511 包额度）");
  check(!b.take_key(1, t0), "关键帧：透支到 -burst 后也发不出去");
  check(!b.take_key(1, t0), "关键帧：失败不继续扣，再试仍失败");
}

// ---- 4. 「先丢非关键帧」这条语义 ----
// 同一时刻先用 take_key 透支一个关键帧（200 包），随后非关键帧全部被拒；关键帧自己
// 也只能继续透支到 -512 包为止。这正是"关键帧风暴不能无限超发"的证据。
void test_key_storm_is_bounded() {
  TokenBucket b(10e6);
  const int64_t t0 = 1'000'000'000;
  drain(b, t0);

  check(b.take_key(200, t0), "先丢非关键帧：关键帧可透支一个 IDR 的分片数（200 包）");
  for (int i = 0; i < 10; ++i) {
    check(!b.take_nonkey(1, t0), "先丢非关键帧：透支后非关键帧先被拒");
  }

  std::size_t more = 0;
  while (b.take_key(1, t0) && more < 10'000) ++more;
  check(more == std::size_t(TokenBucket::kBurstPackets) - 200,
        "先丢非关键帧：关键帧风暴的透支总额仍被 512 包兜住");
  check(!b.take_key(1, t0), "先丢非关键帧：透支额度用尽后关键帧也丢");
}

// ---- 5. 时间回拨不产生负增长 ----
// 补出来的令牌先被回拨"吃掉"是不允许的：回拨只让这一次 dt = 0，令牌保持原值；
// 而且令牌不能变成 NaN——NaN 与任何数比较都是 false，会让 take_nonkey 永远成功。
// 步长取 400 ms（~408.5 包）而不是 1 s：后者会被 burst 512 封顶，令牌数就看不出
// "有没有被倒扣"了。
void test_clock_rollback_no_negative_refill() {
  TokenBucket b(10e6);
  const int64_t t0 = 1'000'000'000;
  drain(b, t0);
  check(b.take_nonkey(1, t0 + 400'000), "回拨：正常前进 400 ms 能取到 1 包（剩 ~407.5）");

  // 回拨 200 ms：若 dt 变负，会倒扣 ~204 包，取 300 包必失败。
  check(b.take_nonkey(300, t0 + 200'000), "回拨：dt 取 0，令牌不被倒扣");
  // 再回拨 400 ms 到起点：剩 ~107.5 包，107 包仍应成功。
  check(b.take_nonkey(107, t0), "回拨：连续回拨后令牌仍是原值");
  check(!b.take_nonkey(1, t0 - 1), "回拨：令牌没有变成 NaN（NaN 会让这里永远通过）");

  // 回拨之后时钟恢复前进，补充应照常进行（说明只影响那一次 dt）。
  check(b.take_nonkey(1, t0 + 999), "回拨：时钟恢复前进后照常补令牌");
}

// ---- 6. 首次调用 ----
// last_us_ == 0 时若按"从 0 到现在"补令牌，now_us = 1e9 会补出天文数字的令牌，
// 且每次调用都重新补满（桶永远耗不空）。正确行为：首次调用只把 last_us_ 设成 now_us，
// 可用令牌上限就是 burst。
void test_first_call_does_not_backfill() {
  TokenBucket b(10e6);
  const int64_t t0 = 1'000'000'000;  // 假设进程"已经跑了 1000 s"
  const std::size_t n = count_take_nonkey(b, t0);
  check(n == std::size_t(TokenBucket::kBurstPackets),
        "首次调用：可用令牌恰好是 burst（512 包），没有按 0→now 补一大笔");
  check(!b.take_nonkey(1, t0), "首次调用：同一时刻不能反复补满（last_us_ 已被设成 now_us）");

  // 同一时刻一次取 burst 也应成功，再多 1 包失败——突发上限本身也是 512。
  TokenBucket b2(10e6);
  check(b2.take_nonkey(std::size_t(TokenBucket::kBurstPackets), t0),
        "首次调用：同一时刻一次可取满 burst 包");
  check(!b2.take_nonkey(1, t0), "首次调用：突发上限是 512 包，没有额外额度");
}

// ---- 7. set_rate ----
// (a) 令牌耗到 0 后改速率：不重置突发（同一时刻仍取不到包），1 s 后按新速率补包
//     （1 Mbps 的 102 包小于 512 封顶，所以这里可以用一整秒）；
// (b) 桶里还有令牌时改速率：已有令牌不被清掉。
void test_set_rate() {
  const int64_t t0 = 1'000'000'000;

  TokenBucket a(10e6);
  drain(a, t0);
  a.set_rate(1e6);
  check(!a.take_nonkey(1, t0), "改速率：不重置突发（零令牌原地改，没被清成满桶）");
  const std::size_t n = count_take_nonkey(a, t0 + 1'000'000);
  check(std::abs(double(n) - kRate1Mbps) <= 1.0,
        "改速率：1 s 后按 1 Mbps 补 ~102 包，而不是旧的 ~1021 包（±1）");

  TokenBucket b(10e6);
  drain(b, t0);
  // 前进 10 ms 补 ~10.2 包、取走 5 包（剩 ~5.2），然后改速率：这 5 包必须还在。
  check(b.take_nonkey(5, t0 + 10'000), "改速率：改前先攒下 5 包");
  b.set_rate(1e6);
  check(b.take_nonkey(5, t0 + 10'000), "改速率：已有令牌不被清掉（改后仍能取走 5 包）");
}

}  // namespace

int main() {
  test_rate_takes_effect();
  test_nonkey_drop_does_not_charge();
  test_key_can_overdraw();
  test_key_storm_is_bounded();
  test_clock_rollback_no_negative_refill();
  test_first_call_does_not_backfill();
  test_set_rate();

  if (failures == 0) {
    std::printf("test_token_bucket: OK\n");
    return 0;
  }
  std::printf("test_token_bucket: %d failure(s)\n", failures);
  return 1;
}
