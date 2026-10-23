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

// 覆盖当前抖动所需的最低档位。分界取「阶梯深度 − 0.5」，于是 0 档（1 帧驻留）能吃下
// 半帧以内的抖动，1 档（2 帧）吃 1.5 帧以内，依此类推。分界取半档而不是整档，是为了让
// "刚好在边界"的抖动不至于把档位顶上去。
// 用阶梯本身推导而不是写死 0.5/1.5/3.5/7.5：加长 kDepthLadder 时不必再改这里，否则
// 「阶梯加了 16 帧档、want_level 却永远返回 ≤3」这种漏改会让最深的档位永远用不上
// （而它看起来像是"没触发"，不是"没接线"）。
std::size_t want_level(double jitter_frames) {
  for (std::size_t i = 0; i < kDepthLadderSize; ++i) {
    if (jitter_frames < static_cast<double>(kDepthLadder[i]) - 0.5) return i;
  }
  return kDepthLadderSize - 1;
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

void JitterBuffer::observe_arrival(uint64_t pts_us, int64_t jitter_arrival_us) {
  if (have_last_pushed_) {
    const int64_t dpts = static_cast<int64_t>(pts_us) - static_cast<int64_t>(last_pushed_pts_);
    // 只吃「按网格到达」的样本：空洞处（dpts 是间隔的整数倍）的增量会把间隔估计越推越大，
    // 那样真实丢包会越来越难被判定成空洞（自我掩盖）。
    if (dpts > 0 && dpts <= (interval_est_us_ * 3) / 2) {
      interval_est_us_ = clamp_interval(interval_est_us_ + (dpts - interval_est_us_) / 16);
      const int64_t darr = jitter_arrival_us - last_pushed_arrival_us_;
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
  last_pushed_arrival_us_ = jitter_arrival_us;
  have_last_pushed_ = true;
}

void JitterBuffer::evict_one() {
  if (q_.empty()) return;
  // 优先挤掉最旧的一帧：直播链路里旧帧的价值随时间单调下降，留着它只会垫高延迟。
  // 但关键帧不挤——它是解码重启点，丢了它后面整个 GOP 的 P 帧全废。
  // 前端是 IDR 时退而挤掉它后面最旧的非关键帧（那一帧的位置会在放行时算成 skipped）。
  // 这是"到达成簇"唯一能造成丢帧的路径：稳态下队首一到目标深度就放行，队列长不起来；
  // 只有当队首被空洞挡住（在等 budget/nack_wait）时，簇里多出来的帧才会有机会把
  // max_frames 顶穿。容量因此必须 >= 最深档位，否则深处的档位自己就会制造丢帧。
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
      // 一簇 0.4 s 的到达空档只会在这里停留一次 budget：簇的头一帧到场时 wait 从 0 起算，
      // 等满 budget 后整簇按 PTS 顺序一次性放完（后面的帧之间没有空洞，走下面那一支）。
      // 所以"成簇"体现为**延迟**（每簇白等一个 budget + 放行时的突发），不是丢帧。
      // 关键帧例外——IDR 不依赖缺失的前序帧，白等一个预算只会拉高延迟。
      int64_t need = front.keyframe ? hold : std::max(hold, cfg_.budget_us);
      // 与 NACK 联动：缺失的那一段正在被重传时，把预算放宽到一次往返的量级。
      // 否则重传明明在跑，缓冲却按"纯丢包"在 40 ms 就放手——重传白跑，帧还是丢了。
      // 只在"确实查到在途"时放宽，查不到就退化成原行为（可 A/B 对比）。
      if (!front.keyframe && cfg_.retransmit_pending && cfg_.nack_wait_us > need &&
          have_last_released_ &&
          cfg_.retransmit_pending(last_released_pts_, front.pts_us)) {
        need = cfg_.nack_wait_us;
      }
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
    // 升档判据有两条，语义完全不同，别混着读：
    //   jitter_frames —— 到达抖动（只算「晚到」，见 observe_arrival）。这是缓冲真能吸收的量，
    //                    升档 = 用延迟换顺序，代价换得到东西。
    //   loss（window_skipped_）—— PTS 空洞，也就是「帧根本没到齐」。升档对它一点用都没有：
    //                    缺失的帧不会因为等得更久就出现，只是把放弃它之前白等的时间从 budget
    //                    拉长到更深档位的 hold。所以在真正丢包的链路上，档位会一路顶到
    //                    max_level、延迟涨满，而 skipped 依然按原速率增长——这条路径是
    //                    "延迟换不到任何质量"，排查时看到「档位一直最高 + skipped 稳定增长」
    //                    就该去查发送端，而不是继续调缓冲。
    clean_windows_ = 0;
    if (level < cfg_.max_level) {
      stats_.level = level + 1;
      ++stats_.switches;
    }
  } else if (level > cfg_.min_level) {
    // 降档目标 = 「覆盖当前抖动所需的最低档位」，而不是「等到固定门限再降一档」。
    // 旧判据用的是 jitter_down_frames（0.30 帧 ≈ 10 ms@30fps）这个绝对值，
    // 而真实链路常驻抖动就有 5~15 ms——门限永远够不到，档位就卡在 1 档不下来，
    // 端到端 p50 恒被 1 帧驻留顶到 33~40 ms。实测这就是 AC-15 里 queue_backlog
    // 偶尔 p50 超 30 ms 的原因（基线与本版都能复现，与延迟注入无关）。
    // want_level 用 kDepthLadder 的档位深度做分界，EWMA 已经把抖动平滑过，
    // 所以一个窗口（window_frames 帧）就够确认，不需要再多窗口滞回。
    const std::size_t want = want_level(jitter_frames);
    if (want < level) {
      ++clean_windows_;
      if (clean_windows_ >= 1) {
        stats_.level = want < cfg_.min_level ? cfg_.min_level : want;
        ++stats_.switches;
        clean_windows_ = 0;
      }
    } else {
      clean_windows_ = 0;  // 当前抖动还需要这个档位，不算干净窗口
    }
  } else {
    clean_windows_ = 0;  // 已在最低档
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
    // 注意：这一支**不会**被"到达空档"触发。空档期间没有帧可放行，last_released_pts_ 不会
    // 前进；空档结束后到达的帧 PTS 必然大于它（否则就不是空档，而是乱序）。走到这里说明
    // 发送端的 PTS 轴自己重复或倒退了——OBS 虚拟摄像头重复发同一帧就是最典型的一例，
    // 那不是网络成簇，也不是缓冲深度不够，加长阶梯对此一帧都救不回来。
    ++stats_.late_dropped;
    maybe_adapt();
    return {};
  }

  // 抖动估计用哪个时刻：见 JitterBasis 的注释。first_arrival_us 为 0（单分片帧、或调用方
  // 没提供）时退回组装完成时刻——两者在这种情况下本来就相等，不会引入偏差。
  const int64_t jitter_arrival =
      (cfg_.basis == JitterBasis::FirstFragment && frame.first_arrival_us > 0)
          ? frame.first_arrival_us
          : now_us;
  observe_arrival(frame.pts_us, jitter_arrival);

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
  if (gap && !front.keyframe) {
    need = std::max(need, cfg_.budget_us);
    // 与 release_ready 保持同一判据：否则 poll 会按 budget 提前醒来，
    // 而 release_ready 又要等到 nack_wait——调用方的超时会与真实到期时刻对不上。
    if (cfg_.retransmit_pending && cfg_.nack_wait_us > need && have_last_released_ &&
        cfg_.retransmit_pending(last_released_pts_, front.pts_us)) {
      need = cfg_.nack_wait_us;
    }
  }
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
