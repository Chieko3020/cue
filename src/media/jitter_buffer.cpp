// 抖动缓冲实现：纯函数式推进（时间从参数进来，不读时钟、不起线程）
#include "cue/media/jitter_buffer.h"

#include <algorithm>

namespace cue::media {
namespace {

constexpr int64_t kMinIntervalUs = 1'000;   // 1000 fps 以上的 PTS 增量当噪声：不允许把间隔估到 0
constexpr int64_t kMaxIntervalUs = 1'000'000;  // 1 fps：再慢就不做空洞判定了，避免误判成大洞

int64_t clamp_interval(int64_t v) {
  if (v < kMinIntervalUs) return kMinIntervalUs;
  if (v > kMaxIntervalUs) return kMaxIntervalUs;
  return v;
}

}  // namespace

JitterBuffer::JitterBuffer(JitterBufferConfig cfg) : cfg_(cfg) {
  interval_est_us_ = clamp_interval(cfg_.nominal_interval_us);
  if (cfg_.max_frames < 1) cfg_.max_frames = 1;
  if (cfg_.adaptive) {
    if (cfg_.max_level >= kDepthLadderSize) cfg_.max_level = kDepthLadderSize - 1;
    if (cfg_.min_level > cfg_.max_level) cfg_.min_level = cfg_.max_level;
    stats_.level = cfg_.min_level;
  }
  if (cfg_.window_frames < 1) cfg_.window_frames = 1;
  if (cfg_.clean_windows_needed < 1) cfg_.clean_windows_needed = 1;
}

std::size_t JitterBuffer::target_frames() const {
  if (cfg_.adaptive) return kDepthLadder[stats_.level];
  return cfg_.target_frames < 1 ? 1 : cfg_.target_frames;
}

int64_t JitterBuffer::hold_us() const {
  if (!cfg_.enabled) return 0;
  const std::size_t target = target_frames();
  if (target <= 1) return 0;  // 深度 1 帧 = 不驻留（只剩空洞预算在起乱序纠正作用）
  return static_cast<int64_t>(target - 1) * interval_est_us_;
}

bool JitterBuffer::gap_before(uint64_t pts_us) const {
  if (!have_last_released_) return false;  // 还没有基线，第一帧无所谓空洞
  const int64_t delta = static_cast<int64_t>(pts_us) - static_cast<int64_t>(last_released_pts_);
  // 超过 1.5 个帧间隔才认为是空洞：PTS 本身带着发送端定时器的抖动，半间隔的余量给这点抖动。
  return delta > (interval_est_us_ * 3) / 2;
}

void JitterBuffer::count_hole(uint64_t pts_us) {
  const int64_t delta = static_cast<int64_t>(pts_us) - static_cast<int64_t>(last_released_pts_);
  int64_t holes = (delta + interval_est_us_ / 2) / interval_est_us_ - 1;
  if (holes < 1) holes = 1;  // 已经越过 1.5 间隔的判据，至少丢了一帧
  stats_.skipped += static_cast<uint64_t>(holes);
  window_skipped_ += static_cast<std::size_t>(holes);
}

void JitterBuffer::observe_arrival(uint64_t pts_us, int64_t now_us) {
  if (have_last_pushed_) {
    const int64_t dpts = static_cast<int64_t>(pts_us) - static_cast<int64_t>(last_pushed_pts_);
    // 只吃「按网格到达」的样本：空洞处（dpts 是间隔的整数倍）的增量会把间隔估计越推越大，
    // 那样真实丢包会越来越难被判定成空洞（自我掩盖）。
    if (dpts > 0 && dpts <= (interval_est_us_ * 3) / 2) {
      interval_est_us_ = clamp_interval(interval_est_us_ + (dpts - interval_est_us_) / 16);
      const int64_t darr = now_us - last_pushed_arrival_us_;
      // 只把「晚到」算成抖动，不看「早到」：
      //  · darr > dpts = 这一帧比上一帧更晚到（真实网络抖动/排队），缓冲能等后续帧对齐，有价值；
      //  · darr < dpts = 帧成批到达（发送端或上游积压后一次放出来），此时延迟已经发生且不可逆，
      //    缓冲它只会让延迟再叠一层。旧的 |darr-dpts| 把成批到达也算成抖动，实测在
      //    queue_backlog 恢复瞬间误升档到 2 档（+100 ms），把 lat_p50 顶过 SLA 阈值。
      const int64_t jit = darr > dpts ? darr - dpts : 0;
      // EWMA α=0.1：单帧的一次抖动不该立刻改变档位，持续抖动才该
      jitter_ewma_us_ += (static_cast<double>(jit) - jitter_ewma_us_) * 0.1;
    }
  }
  last_pushed_pts_ = pts_us;
  last_pushed_arrival_us_ = now_us;
  have_last_pushed_ = true;
}

void JitterBuffer::evict_one() {
  if (q_.empty()) return;
  // 优先挤掉最旧的一帧：直播链路里旧帧的价值随时间单调下降，留着它只会垫高延迟。
  // 但关键帧不挤——它是解码重启点，丢了它后面整个 GOP 的 P 帧全废。
  // 前端是 IDR 时退而挤掉它后面最旧的非关键帧（那一帧的位置会在放行时算成 skipped）。
  std::size_t victim = 0;
  if (q_.front().frame.keyframe) {
    for (std::size_t i = 1; i < q_.size(); ++i) {
      if (!q_[i].frame.keyframe) {
        victim = i;
        break;
      }
    }
  }
  q_.erase(q_.begin() + static_cast<std::ptrdiff_t>(victim));
}

std::vector<JitterFrame> JitterBuffer::release_ready(int64_t now_us) {
  std::vector<JitterFrame> out;
  const int64_t hold = hold_us();
  const std::size_t target = target_frames();
  while (!q_.empty()) {
    JitterFrame& front = q_.front().frame;
    const int64_t wait = now_us - front.arrival_us;
    const bool gap = gap_before(front.pts_us);
    if (gap) {
      // 前端前面有空洞：只有预算能放弃那个缺失帧。这正是乱序帧回到正确位置的机会窗口，
      // 提前放行等于自己把窗口关掉。
      // 关键帧例外——IDR 不依赖缺失的前序帧，白等一个预算只会拉高延迟。
      const int64_t need = front.keyframe ? hold : std::max(hold, cfg_.budget_us);
      if (wait < need) break;
      count_hole(front.pts_us);
    } else {
      // 没有空洞：只要已经攒够目标深度就放行（这正是「深度 2/4/8/16 帧」的字面含义）。
      // 只靠时间判据会让放行卡在「等待 >= 一个帧间隔」的刀刃上：实测算出 63 ms 的
      // 真实驻留（本该 33 ms），因为到达间隔常常比 PTS 间隔短几微秒。
      // 深度不足时仍按 hold 兜底：稀疏流（帧率远低于标称）永远攒不到目标深度，
      // 没有这条会一直积压到容量上限才挤出来。
      if (wait < hold && q_.size() < target) break;
    }
    front.wait_us = wait;
    last_released_pts_ = front.pts_us;
    have_last_released_ = true;
    ++stats_.released;
    out.push_back(std::move(front));
    q_.pop_front();
  }
  return out;
}

void JitterBuffer::maybe_adapt() {
  if (!cfg_.adaptive) return;
  if (window_pushed_ < cfg_.window_frames) return;

  const double denom = static_cast<double>(window_pushed_ + window_skipped_);
  const double loss = denom > 0.0 ? static_cast<double>(window_skipped_) / denom : 0.0;
  const double jitter_frames = static_cast<double>(jitter_ewma_us_) /
                               static_cast<double>(interval_est_us_);
  stats_.loss_rate = loss;
  stats_.jitter_frames = jitter_frames;

  const std::size_t level = stats_.level;
  if (loss >= cfg_.loss_up || jitter_frames >= cfg_.jitter_up_frames) {
    clean_windows_ = 0;
    if (level < cfg_.max_level) {
      stats_.level = level + 1;
      ++stats_.switches;
    }
  } else {
    if (loss <= cfg_.loss_down && jitter_frames <= cfg_.jitter_down_frames) {
      ++clean_windows_;
    } else {
      clean_windows_ = 0;  // 中性窗口：既不算干净也不算坏，连续干净窗口重新计数
    }
    if (clean_windows_ >= cfg_.clean_windows_needed && stats_.level > cfg_.min_level) {
      --stats_.level;
      ++stats_.switches;
      clean_windows_ = 0;
    } else if (cfg_.fast_down && stats_.level > cfg_.min_level && loss <= cfg_.loss_down &&
               jitter_frames <= cfg_.jitter_down_frames * 0.25) {
      // 快降：抖动低到降档门限的 1/4 以下且零丢包时，一个干净窗口就回到底。
      // 慢降是为了防档位锯齿（在门限边界来回切），但"抖动只剩门限的四分之一"
      // 说明问题已经消失，继续维持高档位只是在为不存在的问题付延迟。
      // 实测背景：queue_backlog 恢复后需要 6 个干净窗口才从 2 档回到 0 档，
      // MTTR 被拖到 2.5 s，端到端 p50 在 SLA 重测窗口里仍然超标。
      stats_.level = cfg_.min_level;
      ++stats_.switches;
      clean_windows_ = 0;
    }
  }
  window_pushed_ = 0;
  window_skipped_ = 0;
}

std::vector<JitterFrame> JitterBuffer::push(JitterFrame frame, int64_t now_us) {
  frame.arrival_us = now_us;
  frame.wait_us = 0;
  ++stats_.pushed;
  ++window_pushed_;

  if (!cfg_.enabled) {
    // 关掉时是纯透传：不建立 PTS 基线，否则乱序帧会被当成"迟到"丢掉，
    // 那样 --jitter off 就不再是"改动前的那条链路"，A/B 对比失去意义。
    ++stats_.released;
    maybe_adapt();
    return {std::move(frame)};
  }

  if (have_last_released_ && frame.pts_us <= last_released_pts_) {
    // 它的位置已经过去了（比最近放行的帧还旧，或同 PTS 的重复帧）：按序输出已经不可能，
    // 硬塞进来只会让解码顺序倒挂，所以丢掉并计数。
    ++stats_.late_dropped;
    maybe_adapt();
    return {};
  }

  observe_arrival(frame.pts_us, now_us);

  // 插入位置：PTS 主序、到达次序次之。seq 单调递增，所以「插在第一个 PTS 更大的元素之前」
  // 天然把同 PTS 的帧按到达顺序排在后面。
  Entry entry{std::move(frame), next_seq_++};
  const uint64_t pts = entry.frame.pts_us;
  const auto pos = std::upper_bound(q_.begin(), q_.end(), pts,
                                    [](uint64_t p, const Entry& e) { return p < e.frame.pts_us; });
  q_.insert(pos, std::move(entry));

  while (q_.size() > cfg_.max_frames) evict_one();

  auto out = release_ready(now_us);
  maybe_adapt();
  return out;
}

std::vector<JitterFrame> JitterBuffer::poll(int64_t now_us) {
  auto out = release_ready(now_us);
  maybe_adapt();
  return out;
}

int64_t JitterBuffer::next_deadline_us() const {
  if (q_.empty()) return 0;
  const JitterFrame& front = q_.front().frame;
  const bool gap = gap_before(front.pts_us);
  int64_t need = hold_us();
  if (gap && !front.keyframe) need = std::max(need, cfg_.budget_us);
  return front.arrival_us + need;
}

JitterBufferStats JitterBuffer::stats() const {
  JitterBufferStats s = stats_;
  s.depth = q_.size();
  s.capacity = cfg_.max_frames;
  s.target_frames = target_frames();
  return s;
}

void JitterBuffer::reset() {
  q_.clear();
  next_seq_ = 0;
  have_last_released_ = false;
  last_released_pts_ = 0;
  have_last_pushed_ = false;
  last_pushed_pts_ = 0;
  last_pushed_arrival_us_ = 0;
  interval_est_us_ = clamp_interval(cfg_.nominal_interval_us);
  jitter_ewma_us_ = 0.0;
  window_pushed_ = 0;
  window_skipped_ = 0;
  clean_windows_ = 0;
  stats_ = JitterBufferStats{};
  if (cfg_.adaptive) stats_.level = cfg_.min_level;
}

}  // namespace cue::media
