// 抖动缓冲单测（技术设计文档 §13.1 的四条：乱序重排 / 缺帧等待预算 / 超时跳帧 / 深度自适应），
// 外加关键帧保护与两个「不能悄悄坏掉」的口径：关掉时纯透传、迟到帧计数。
//
// 为什么这里全用假时钟：抖动缓冲的每条规则都是「时间 + PTS」的比较，用真时钟写这些断言
// 只能靠 sleep 去逼近边界，既慢又不稳。JitterBuffer 不读时钟、不起线程，所以能把
// 「恰好差 1 µs 不到期 / 到 1 µs 到期」这种边界逐条钉死。
#include <cstdint>
#include <cstdio>
#include <vector>

#include "cue/media/jitter_buffer.h"

namespace {

int failures = 0;

void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

using cue::media::JitterBuffer;
using cue::media::JitterBufferConfig;
using cue::media::JitterFrame;

JitterFrame frame(uint64_t pts_us, bool keyframe = false) {
  JitterFrame f;
  f.pts_us = pts_us;
  f.frame_id = static_cast<uint32_t>(pts_us / 1000);
  f.keyframe = keyframe;
  f.payload = {static_cast<uint8_t>(pts_us / 1000)};
  return f;
}

std::vector<uint64_t> pts_list(const std::vector<JitterFrame>& v) {
  std::vector<uint64_t> out;
  out.reserve(v.size());
  for (const auto& f : v) out.push_back(f.pts_us);
  return out;
}

// 帧间隔 10 ms 的固定配置：所有断言里的时刻都是它的整数倍，边界一眼能算
constexpr int64_t kInterval = 10'000;

JitterBufferConfig base_cfg() {
  JitterBufferConfig c;
  c.max_frames = 8;
  c.target_frames = 2;  // hold = (2-1) × 10 ms = 10 ms
  c.budget_us = 50'000;
  c.nominal_interval_us = kInterval;
  return c;
}

}  // namespace

int main() {
  using namespace cue::media;

  // ---------------------------------------------------------------- 1) 乱序重排
  // PTS 3,1,2 乱序到达：输出必须按 PTS 升序（1,2,3），而不是到达顺序（3,1,2）。
  // 第一帧单独到达时不会立刻放行——目标深度 2 帧意味着"再等一帧，看有没有更小的 PTS"，
  // 这正是乱序能被纠正的原因，也是抖动缓冲固有的一帧延迟。
  {
    JitterBuffer jb(base_cfg());  // 目标深度 2 帧、hold=10 ms
    std::vector<JitterFrame> out;
    auto append = [&out](std::vector<JitterFrame> v) {
      for (auto& f : v) out.push_back(std::move(f));
    };
    check(jb.push(frame(30'000), 1'000'000).empty(),
          "乱序重排：单帧不立刻放行（深度 1 < 目标深度 2）");
    append(jb.push(frame(10'000), 1'000'000));  // 攒够 2 帧 → 放行更小的 PTS=1
    append(jb.push(frame(20'000), 1'000'000));
    const auto tail = jb.poll(1'010'000);  // 剩下的 PTS=3 等满 hold 放行
    append(tail);

    check(pts_list(out) == std::vector<uint64_t>({10'000, 20'000, 30'000}),
          "乱序重排：3,1,2 乱序入队 → 按 1,2,3 输出（PTS 主序压过到达顺序）");
    check(!out.empty() && out[0].pts_us == 10'000,
          "乱序重排：先到的是 PTS=3，但第一个出去的必须是 PTS=1");
    check(jb.stats().skipped == 0, "乱序重排：没有丢帧，skipped=0");
    check(jb.stats().late_dropped == 0, "乱序重排：没有迟到帧");
    check(jb.stats().released == 3, "乱序重排：三帧都出来了");
    check(tail.size() == 1 && tail[0].wait_us == 10'000,
          "乱序重排：末帧 wait_us 是真实等待时长（10 ms）");
    check(jb.depth() == 0, "乱序重排：队列清空");
  }
  // PTS 相同的两帧：按到达顺序输出，而不是按内容/随机次序
  {
    JitterBuffer jb(base_cfg());
    JitterFrame first = frame(50'000);
    first.frame_id = 1;  // 用 frame_id 区分「谁先到」，只靠 PTS 分不出来
    JitterFrame second = frame(50'000);
    second.frame_id = 2;
    check(jb.push(std::move(first), 2'000'000).empty(), "同 PTS：单帧不立刻放行");
    auto out = jb.push(std::move(second), 2'000'000);  // 攒够 2 帧 → 放行先到的那帧
    check(out.size() == 1 && out[0].frame_id == 1, "同 PTS：先到的先出队");
    auto more = jb.poll(2'010'000);
    check(more.size() == 1 && more[0].frame_id == 2, "同 PTS：后到的随后出队");
    check(jb.stats().skipped == 0, "同 PTS：不制造空洞");
    check(jb.stats().late_dropped == 0, "同 PTS：不把同 PTS 的帧当迟到丢掉");
  }

  // ---------------------------------------------------------------- 2) 缺帧等待预算
  // PTS=2 没到：前端（PTS=3）必须等满预算才放弃它，并且 skipped 只记 1。
  {
    JitterBuffer jb(base_cfg());  // hold=10 ms，budget=50 ms
    jb.push(frame(10'000), 0);
    auto out = jb.push(frame(30'000), 10'000);  // 到 hold：PTS=1 放行
    check(pts_list(out) == std::vector<uint64_t>({10'000}), "缺帧：前端按 hold 正常放行");
    check(jb.depth() == 1, "缺帧：PTS=3 被空洞挡住，留在队列里");

    out = jb.push(frame(40'000), 20'000);
    check(out.empty(), "缺帧：空洞未满预算，PTS=3/4 都不放行");
    out = jb.poll(59'999);  // 距 PTS=3 到达 49 999 µs，差 1 µs 不到期
    check(out.empty(), "缺帧：差 1 µs 到预算 → 仍不放行（边界不能提前）");
    out = jb.poll(60'000);  // 距 PTS=3 到达恰好 50 000 µs
    check(pts_list(out) == std::vector<uint64_t>({30'000, 40'000}),
          "缺帧：等满预算后跳过 PTS=2，继续输出 3,4");
    check(jb.stats().skipped == 1, "缺帧：skipped 记 1 个空洞");
    check(jb.stats().late_dropped == 0, "缺帧：不是迟到帧，late_dropped=0");
    check(jb.stats().depth == 0, "缺帧：队列已排空");
  }

  // ---------------------------------------------------------------- 3) 超时跳帧（整段空闲后清空）
  // 整段空闲时没有新帧推进入队，深度判据永远不会满足，只能靠调用方按
  // next_deadline_us() 驱动 poll。这里用目标深度 4 帧（hold=30 ms）让三帧都还压着，
  // 验证「到期时刻」与「整段空闲后按 hold 全部放行」。
  {
    JitterBufferConfig c = base_cfg();
    c.target_frames = 4;  // hold = 3 × 10 ms = 30 ms
    JitterBuffer jb(c);
    check(jb.push(frame(0), 0).empty(), "空闲清空：深度不足时不放行");
    check(jb.push(frame(kInterval), 1'000).empty(), "空闲清空：依然不足目标深度");
    check(jb.push(frame(2 * kInterval), 2'000).empty(), "空闲清空：三帧都压着");
    check(jb.depth() == 3, "空闲清空：三帧积压");
    check(jb.next_deadline_us() == 30'000, "空闲清空：队首到期时刻 = 到达 + hold");
    check(jb.poll(29'999).empty(), "空闲清空：差 1 µs 到 hold → 不放行");

    const auto out = jb.poll(1'000'000);  // 整段空闲后
    check(pts_list(out) == std::vector<uint64_t>({0, kInterval, 2 * kInterval}),
          "空闲清空：所有帧按 PTS 顺序全部放行");
    check(jb.stats().late_dropped == 0 && jb.stats().skipped == 0,
          "空闲清空：超时是放行而不是丢弃（丢帧会白扔可解码的数据）");
    check(jb.depth() == 0 && jb.next_deadline_us() == 0, "空闲清空：队列空，没有下一次到期");
  }

  // ---------------------------------------------------------------- 4) 深度自适应
  // 脏序列（每 3 帧丢 1 帧）→ 档位升到上限；干净序列 → 连续干净窗口后回落。
  {
    JitterBufferConfig c = base_cfg();
    c.adaptive = true;
    c.min_level = 0;
    c.max_level = 2;  // 1/2/4 帧
    c.window_frames = 8;
    c.clean_windows_needed = 2;
    c.budget_us = 30'000;
    JitterBuffer jb(c);

    auto feed = [&jb](uint64_t pts, int64_t& now) {
      now += kInterval;
      auto out = jb.push(frame(pts), now);
      // 每一帧之后用「远未来」的 poll 把队列排空：这样每个窗口的 skipped 都在窗口内结算，
      // 不会被积压推迟到下一个窗口（否则升档判定会被推迟，断言就变成了在测时序巧合）。
      auto more = jb.poll(now + 200'000);
      for (auto& f : more) out.push_back(std::move(f));
      return out;
    };

    check(jb.stats().level == 0, "自适应：起始档位为 0（1 帧 = 零驻留）");
    check(jb.target_frames() == 1, "自适应：0 档 = 1 帧，干净网络不额外驻留");

    int64_t now = 0;
    uint64_t pts = 0;
    size_t released = 0;
    for (int i = 0; i < 48; ++i) {
      const bool dropped = (i % 3) == 2;  // 每 3 帧丢 1 帧 → 丢包率 1/3
      if (!dropped) {
        released += feed(pts, now).size();
      }
      pts += static_cast<uint64_t>(kInterval);
    }
    const auto dirty = jb.stats();
    check(dirty.level == 2, "自适应：高丢包序列后档位升到上限（2 档 = 4 帧）");
    check(dirty.switches >= 2, "自适应：升档被计数（switches>=2）");
    check(dirty.skipped > 0, "自适应：空洞被计数");
    check(released > 0, "自适应：脏序列也在出帧（不会因为丢包卡死）");
    check(jb.target_frames() == 4, "自适应：2 档 = 4 帧");

    // 干净序列：PTS 与到达时间都严格对齐，丢包率 0、抖动 0
    const int64_t up_switches = static_cast<int64_t>(dirty.switches);
    for (int i = 0; i < 80; ++i) {
      feed(pts, now);
      pts += static_cast<uint64_t>(kInterval);
    }
    const auto clean = jb.stats();
    check(clean.level == 0, "自适应：干净序列后档位回落到最低档");
    // 快降（fast_down，默认开）在"抖动低到降档门限 1/4 且零丢包"时一步回到底，
    // 所以这里只保证"至少发生了一次降档"；降档被计数由 switches 增加证明。
    check(static_cast<int64_t>(clean.switches) >= up_switches + 1, "自适应：降档也被计数");
    check(clean.loss_rate == 0.0, "自适应：干净窗口的丢包率为 0");
  }

  // ---------------------------------------------------------------- 4b) 与 NACK 联动
  // 空洞时若「缺失的那一段正在被重传」，等待预算要从 budget 放宽到 nack_wait；
  // 查不到在途就退回 budget —— 这条差异是 A/B 对比的基准，不能悄悄失效。
  // 用 target_frames=1（零驻留）让"首帧立即放行、后续帧形成空洞"这件事可预期；
  // 深度 2 的配置下首帧本来就要等一帧才放行（见用例 1），那会把时序搅乱。
  {
    JitterBufferConfig c;
    c.max_frames = 8;
    c.target_frames = 1;         // hold = 0
    c.budget_us = 50'000;
    c.nominal_interval_us = kInterval;
    c.nack_wait_us = 200'000;
    uint64_t asked_lo = 0, asked_hi = 0;
    int asks = 0;
    bool inflight = true;
    c.retransmit_pending = [&](uint64_t lo, uint64_t hi) {
      ++asks;
      asked_lo = lo;
      asked_hi = hi;
      return inflight;
    };

    {
      JitterBuffer jb(c);
      // PTS=0 建立基线并立即放行（无空洞）
      auto first = jb.push(frame(0), 0);
      check(first.size() == 1, "NACK 联动：零驻留时首帧立即放行并建立基线");
      // PTS=2×interval：与基线之间空出 PTS=1×interval，构成空洞
      auto got = jb.push(frame(2 * kInterval), 20'000);
      check(got.empty(), "NACK 联动：有空洞且未到期时不放行");
      check(asks > 0, "NACK 联动：空洞时确实查询了重传状态");
      check(asked_lo == 0 && asked_hi == 2 * kInterval,
            "NACK 联动：查询区间是 (最近已放行, 队首) 的开区间");
      // budget(50 ms) 已过，但 nack_wait(200 ms) 未到：在途 → 继续等
      check(jb.poll(20'000 + 60'000).empty(), "NACK 联动：重传在途时超过 budget 仍不放行");
      // 越过 nack_wait → 放弃缺失帧、放行队首
      check(!jb.poll(20'000 + 200'000).empty(), "NACK 联动：重传在途时等满 nack_wait 后放行");
    }

    {
      // 同样时序但查不到在途：按 budget 放行，不被 nack_wait 拖住
      inflight = false;
      JitterBuffer jb2(c);
      (void)jb2.push(frame(0), 0);
      check(jb2.push(frame(2 * kInterval), 20'000).empty(), "NACK 联动：无在途时未到期不放行");
      check(!jb2.poll(20'000 + 60'000).empty(),
            "NACK 联动：无在途时按 budget 放行（不被 nack_wait 拖住）");
      inflight = true;
    }

    {
      // 没装回调 = 完全退回旧行为（nack_wait 配了也不起作用）
      JitterBufferConfig c2 = c;
      c2.retransmit_pending = nullptr;
      JitterBuffer jb3(c2);
      (void)jb3.push(frame(0), 0);
      (void)jb3.push(frame(2 * kInterval), 20'000);
      check(!jb3.poll(20'000 + 60'000).empty(),
            "NACK 联动：没装回调时按 budget 放行（与改动前一致）");
    }
  }

  // ---------------------------------------------------------------- 4c) 抖动基准
  // 组装完成时刻里混着"整帧在途耗时 ≈ 帧字节数/带宽"。IDR 帧是 P 帧的十几倍大，
  // 于是每次 IDR 都会让组装完成时刻突然后移一大截——那是吞吐伪影，不是网络抖动。
  // 这里用「首片准点、组装完成在第 4 帧阶跃 200 ms」复现它：
  //  · AssemblyDone：这次阶跃被算成抖动（jitter_frames 抬起来）
  //  · FirstFragment：首片一直准点，抖动估计应该接近 0
  {
    auto run = [](cue::media::JitterBasis basis) {
      JitterBufferConfig c;
      c.target_frames = 1;      // 零驻留，放行不干扰观察
      c.adaptive = true;        // jitter_frames 只在 adaptive 下结算
      c.min_level = 0;
      c.max_level = 0;          // 档位锁死，只看抖动统计
      c.window_frames = 3;
      c.nominal_interval_us = kInterval;
      c.basis = basis;
      JitterBuffer jb(c);
      for (int i = 0; i < 9; ++i) {
        JitterFrame f;
        f.pts_us = static_cast<uint64_t>(i) * kInterval;
        f.first_arrival_us = static_cast<int64_t>(i) * kInterval;  // 首片始终准点
        const int64_t assembly_delay = (i >= 4) ? 200'000 : 0;     // 第 4 帧起组装晚 200 ms
        jb.push(std::move(f), static_cast<int64_t>(i) * kInterval + assembly_delay);
      }
      return jb.stats().jitter_frames;
    };

    const double j_first = run(cue::media::JitterBasis::FirstFragment);
    const double j_done = run(cue::media::JitterBasis::AssemblyDone);
    check(j_first < 0.1, "抖动基准：首片准点时，组装阶跃不该被算成抖动");
    check(j_done > 1.0, "抖动基准：用组装完成时刻时，同一次阶跃被算成抖动（这就是污染）");
    check(j_done > j_first, "抖动基准：两种基准的结论必须不同，否则开关没接线");
  }

  // ---------------------------------------------------------------- 5) 关键帧保护
  // 队列超容量时必须挤掉非关键帧，而不是把 IDR 扔掉——IDR 一丢，后面整个 GOP 的 P 帧全废。
  {
    JitterBufferConfig c = base_cfg();
    c.max_frames = 2;
    c.target_frames = 4;  // hold = 3 × 10 ms = 30 ms
    JitterBuffer jb(c);

    jb.push(frame(0, /*keyframe=*/true), 0);
    jb.push(frame(10'000), 1'000);
    jb.push(frame(20'000), 2'000);  // 超容量：前端是 IDR → 挤掉 PTS=10 000 的 P 帧
    check(jb.depth() == 2, "关键帧保护：容量上限仍然成立");
    check(jb.stats().late_dropped == 0, "关键帧保护：挤队不记迟到帧");

    const auto out = jb.poll(100'000);
    check(pts_list(out) == std::vector<uint64_t>({0, 20'000}),
          "关键帧保护：IDR 没有被挤掉，跟着它的帧一起输出");
    check(jb.stats().skipped == 1, "关键帧保护：被挤掉的 P 帧按 PTS 空洞计入 skipped（不重复计 late_dropped）");

    // 迟到帧：PTS 已经落在已放行帧之后 → 无法按序输出，计入 late_dropped（不制造空洞）
    const auto late = jb.push(frame(10'000), 200'000);
    check(late.empty(), "迟到帧：不放行");
    check(jb.stats().late_dropped == 1, "迟到帧：late_dropped 计数");
    check(jb.stats().skipped == 1, "迟到帧：late_dropped 不重复计进 skipped");
  }

  // ---------------------------------------------------------------- 6) 关掉时纯透传
  // --jitter off 必须等于「改动前那条链路」：不驻留、不丢迟到帧，wait_us=0。
  {
    JitterBufferConfig c = base_cfg();
    c.enabled = false;
    JitterBuffer jb(c);
    auto out = jb.push(frame(20'000), 5'000);
    check(pts_list(out) == std::vector<uint64_t>({20'000}), "关闭：立即放行");
    check(out.size() == 1 && out[0].wait_us == 0, "关闭：等待时长为 0");
    out = jb.push(frame(10'000), 5'001);  // 乱序也原样透传
    check(pts_list(out) == std::vector<uint64_t>({10'000}), "关闭：乱序帧也原样透传（不许当成迟到丢掉）");
    check(jb.stats().late_dropped == 0, "关闭：不产生迟到计数");
    check(jb.next_deadline_us() == 0, "关闭：没有驻留，也就没有到期时刻");
  }

  if (failures == 0) {
    std::printf("test_jitter_buffer: OK\n");
    return 0;
  }
  std::printf("test_jitter_buffer: %d failure(s)\n", failures);
  return 1;
}
