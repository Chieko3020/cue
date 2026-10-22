// NACK 策略（FR-PROBE.2）：令牌桶限速边界、每窗口上限、坏窗口冷却与单包探测。
//
// 为什么单独测：这些边界在真链路上几乎不可复现（"恰好第 8 包"、"冷却结束那一微秒"、
// "探测成功之后立刻复位"），而它们恰恰是"高丢包时 probe 会不会把反向链路打满"的全部
// 判据。纯决策对象不碰 socket/时钟，所以可以逐微秒地钉死。
#include <cstdint>
#include <cstdio>

#include "cue/proto/nack.h"

namespace {

int failures = 0;

void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

}  // namespace

int main() {
  using cue::proto::NackController;
  using cue::proto::NackRateLimiter;

  // ---------------------------------------------------------------- 令牌桶
  {
    NackRateLimiter limiter(8.0);
    check(limiter.take(0), "令牌桶：第一包立即允许（容量 1）");
    check(!limiter.take(0), "令牌桶：同一微秒第二包被拒（不允许突发）");
    check(limiter.take(125'000), "令牌桶：125 ms 后恢复 1 个令牌（8 包/秒）");
    check(!limiter.take(200'000), "令牌桶：不足一个令牌间隔仍被拒");
    check(limiter.take(250'000), "令牌桶：下一枚令牌按 125 ms 节奏到达");
    check(limiter.take(1'000'000), "令牌桶：长时间空闲不会攒出多个令牌（容量 1）");

    NackRateLimiter fast(100.0);
    check(fast.rate() == 8.0, "令牌桶：速率被夹到 8 包/秒（协议上限）");
    NackRateLimiter bad(-1.0);
    check(bad.rate() == 8.0, "令牌桶：非法速率回落到 8 包/秒");
  }

  // ---------------------------------------------------------------- 每窗口硬上限
  {
    NackController c;
    c.begin_window(0);
    int ok = 0;
    for (int i = 0; i < 8; ++i) {
      if (c.try_acquire(int64_t(i) * 130'000, false)) ++ok;
    }
    check(ok == 8, "窗口内最多 8 个 NACK 包（第 1..8 个都发）");
    check(c.sent() == 8, "sent 计数 = 8");
    check(!c.try_acquire(1'040'000, false), "第 9 个包在同一窗口被拒（即使令牌已满）");
    check(c.suppressed() == 1, "第 9 个包计入 suppressed");
    check(c.mode(1'040'000) == NackController::Mode::Throttled,
          "本窗口有 suppressed → mode=throttled");
    c.begin_window(1'040'000);
    check(c.try_acquire(1'040'000, false), "下一个统计窗口立刻恢复发送");
  }

  // ---------------------------------------------------------------- 坏窗口 → 冷却 → 单包探测 → 恢复
  {
    NackController c;
    check(c.mode(0) == NackController::Mode::Normal, "初始 mode=normal");
    c.observe_loss_window(1000, 800, 1'000'000);  // 20%
    c.observe_loss_window(1000, 800, 2'000'000);
    check(!c.paused(2'000'000), "连续 2 个坏窗口还不暂停");
    c.observe_loss_window(1000, 800, 3'000'000);
    check(c.paused(3'000'000), "连续 3 个 >10% 窗口 → 暂停");
    check(c.paused_ms() == 1000, "暂停时长按冷却 1 s 累计");
    check(c.pauses() == 1, "暂停次数 = 1");
    check(c.mode(3'000'000) == NackController::Mode::Paused, "冷却中 mode=paused");
    check(!c.try_acquire(3'000'000, false), "冷却中不发 NACK");
    check(c.probing(3'500'000) == false, "冷却未结束不做单包探测");
    // 冷却中的坏窗口不重复累计（否则永远出不来）
    c.observe_loss_window(1000, 800, 3'500'000);
    check(c.pauses() == 1, "冷却期间的坏窗口不重复触发暂停");

    c.begin_window(4'000'000);
    check(c.probing(4'000'000), "冷却结束 → 进入单包探测");
    check(c.mode(4'000'000) == NackController::Mode::Throttled, "探测期 mode=throttled");
    check(c.try_acquire(4'000'000, true), "探测许可：本窗口第 1 条");
    check(!c.try_acquire(4'100'000, true), "探测额度：本窗口第 2 条被拒");
    c.on_probe_success();
    check(c.mode(4'200'000) == NackController::Mode::Normal, "探测成功 → 恢复 normal");
    check(!c.probing(4'200'000), "探测成功后不再处于探测模式");
  }

  // ---------------------------------------------------------------- 好窗口打断坏窗口计数
  {
    NackController c;
    c.observe_loss_window(1000, 800, 1'000'000);
    c.observe_loss_window(1000, 800, 2'000'000);
    c.observe_loss_window(1000, 995, 3'000'000);  // 0.5%：好窗口清零
    check(c.bad_windows() == 0, "好窗口把连续坏窗口计数清零");
    c.observe_loss_window(1000, 800, 4'000'000);
    c.observe_loss_window(1000, 800, 5'000'000);
    check(!c.paused(5'000'000), "清零之后只累计到 2 个坏窗口，不暂停");
    c.observe_loss_window(1000, 800, 6'000'000);
    check(c.paused(6'000'000), "再连续 3 个坏窗口 → 暂停");
    // 5% 不算坏窗口（阈值是 10%）
    NackController c2;
    for (int i = 0; i < 3; ++i) c2.observe_loss_window(1000, 950, int64_t(i + 1) * 1'000'000);
    check(!c2.paused(3'500'000), "5% 丢包不触发冷却");
    check(c2.last_window_loss() > 0.049 && c2.last_window_loss() < 0.051,
          "窗口丢包率读数正确");
  }

  if (failures == 0) {
    std::printf("test_nack: OK\n");
    return 0;
  }
  std::printf("test_nack: %d failure(s)\n", failures);
  return 1;
}
