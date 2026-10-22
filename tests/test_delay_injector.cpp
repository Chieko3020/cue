// DelayInjector（FR-PIPE.10 的 delay / jitter）的行为钉死：
// 同一 seed 同一序列、注入值变化即重置、单侧均匀分布、jitter=0 不消费随机数。
// 这些判据是「可复现的延迟注入」的全部承诺，且不依赖 FFmpeg/WS，所以放在 ctest 里。
#include <cstdint>
#include <cstdio>
#include <set>
#include <vector>

#include "cue/media/delay_injector.h"

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
  using cue::media::DelayInjector;

  // 1) 固定延迟：每个包都是同一个值，且与抖动无关
  {
    DelayInjector d;
    d.sync(42, 200.0, 0.0);
    check(d.active(), "delay=200 生效");
    bool all_same = true;
    for (int i = 0; i < 100; ++i) {
      if (d.extra_us() != 200'000) all_same = false;
    }
    check(all_same, "delay=200 且 jitter=0：每个包都压 200000 us");
    check(!(DelayInjector{}.active()), "默认值（0/0）不生效");
  }

  // 2) 同一 seed → 同一串决策（两次独立实例）
  {
    DelayInjector a, b;
    a.sync(7, 0.0, 40.0);
    b.sync(7, 0.0, 40.0);
    bool same = true;
    for (int i = 0; i < 500; ++i) {
      if (a.extra_us() != b.extra_us()) same = false;
    }
    check(same, "同 seed + 同注入值 → 逐项相同的抖动序列");
  }

  // 3) 不同 seed → 不同序列（不是"seed 被忽略"的假随机）
  {
    DelayInjector a, b;
    a.sync(1, 0.0, 40.0);
    b.sync(2, 0.0, 40.0);
    int diff = 0;
    for (int i = 0; i < 200; ++i) {
      if (a.extra_us() != b.extra_us()) ++diff;
    }
    check(diff > 100, "不同 seed 的序列显著不同");
  }

  // 4) 清零再注入同一个值 → 序列从头开始（可复现的「连续两次注入」）
  {
    DelayInjector d;
    d.sync(9, 0.0, 25.0);
    std::vector<std::int64_t> first;
    for (int i = 0; i < 64; ++i) first.push_back(d.extra_us());
    d.sync(9, 0.0, 0.0);  // value=0 清除
    check(!d.active(), "清空后不生效");
    d.sync(9, 0.0, 25.0);  // 再次注入同一个值
    bool same = true;
    for (int i = 0; i < 64; ++i) {
      if (d.extra_us() != first[static_cast<std::size_t>(i)]) same = false;
    }
    check(same, "清零再注入 → 抖动序列从头开始（可复现）");
  }

  // 5) 单侧分布：抖动 ∈ [0, value]，绝不出现负延迟
  {
    DelayInjector d;
    d.sync(3, 50.0, 30.0);
    bool in_range = true;
    std::int64_t lo = INT64_MAX;
    std::int64_t hi = INT64_MIN;
    std::set<std::int64_t> distinct;
    // 2 万次抽取：上下界各 30001 个取值，抽不到任何接近边界的概率 < e^-600，
    // 所以这条断言不会因为随机性偶发失败（不用"恰好等于 0"这种必然碰运气的写法）。
    for (int i = 0; i < 20000; ++i) {
      const auto v = d.extra_us();
      distinct.insert(v);
      if (v < lo) lo = v;
      if (v > hi) hi = v;
      if (v < 50'000 || v > 80'000) in_range = false;
    }
    check(in_range, "delay=50 jitter=30：额外延迟恒在 [50000, 80000] us");
    check(lo <= 51'000, "下界附近能被抽到（单侧分布，0 是可取的最小抖动）");
    check(hi >= 79'000, "上界附近能被抽到（不是常数）");
    check(distinct.size() > 10000, "抖动近似连续而不是几个固定档");
  }

  if (failures == 0) {
    std::printf("test_delay_injector: OK (5 cases)\n");
    return 0;
  }
  std::printf("test_delay_injector: %d failure(s)\n", failures);
  return 1;
}
