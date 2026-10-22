// 抖动缓冲（FR-PIPE.2 发送侧 / FR-PROBE.4 接收侧，六段埋点里 `buffer` 段的唯一来源）
//
// 为什么是纯函数式推进（push/poll 而不是带线程的黑盒）：
//   cued 是单线程 WS 读循环、probe 是单线程 UDP 收循环，两者都已有自己的时间基与退出路径；
//   在组件内部再起线程会引入两把锁、一个唤醒源和一个没法在单测里复现的调度依赖。
//   这里把「队列 + 排序 + 等待预算 + 档位」全做成显式状态，时间从参数进来，
//   调用方决定什么时候推进——单测可以拿假时钟把每条规则逐条钉死。
//
// 排序口径（双序）：
//   主序 PTS（播放/解码顺序），PTS 相同时按**到达顺序**。所以乱序到达的帧能回到正确位置，
//   而同一 PTS 的重复帧不会被排成随机次序。
//
// 什么时候放行一帧（这是"延迟"的全部来源）：
//   1) 正常情况：队列已经攒够目标深度（帧数就是档位），或者前端的等待时长 >= hold，
//      hold = (目标深度 - 1) × 帧间隔。目标深度 2 帧 = 1 个帧间隔 ≈ 30 fps 下 33 ms，
//      这就是抖动缓冲的固有延迟代价。
//      两条判据都要，是因为只有时间判据会卡在"等待 >= 一个帧间隔"的刀刃上：
//      实测到达间隔常比 PTS 间隔短几微秒，帧就会多等一个间隔（33 → 63 ms）；
//      而只有深度判据又救不了稀疏流（帧率远低于标称时永远攒不到目标深度）。
//   2) 前端前面有 PTS 空洞时：等满「等待预算」（budget）才放弃缺失帧继续往下走，
//      并把空洞记进 skipped。空洞还在预算内时**不放行**，这是乱序能被纠正的前提。
//   3) 空洞的下一个帧是 IDR 时不按 2) 等满预算：IDR 是解码重启点，它不依赖缺失的前序帧，
//      按 hold 放行即可（否则每次丢帧后都要白等一个预算，端到端延迟被无谓拉长）。
//   4) 队列超过 max_frames（FR-PIPE.14 的硬上限）时挤掉**最旧的一帧**：直播链路里旧帧
//      已经没有价值，继续留着只会把延迟越垫越高。关键帧不挤（见 evict_one）。
//
// 计数口径（两个计数器互不重复计同一帧）：
//   skipped      —— 放行时发现的 PTS 空洞数。缺失帧、被容量挤掉的帧最终都表现为空洞。
//   late_dropped —— 入队时就已经过期的帧（PTS <= 最近已放行的 PTS）。这种帧无法按序输出，
//                   只能丢；它不制造空洞（它自己的位置早就过去了），所以不进 skipped。
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace cue::media {

// 一帧媒体数据（不含分片语义：重组已经在调用方完成）
struct JitterFrame {
  uint64_t pts_us = 0;       // 帧时间戳（发送端时钟域，单调递增的媒体时间轴）
  uint32_t frame_id = 0;     // 供日志/诊断关联，不参与排序
  uint16_t frag_count = 0;   // 原始分片数（诊断用，不参与排序）
  bool keyframe = false;     // IDR：解码重启点，跳帧/挤队时的保护对象
  uint8_t codec = 0;         // 下行 codec 标记（probe 侧据此决定解不解）
  std::vector<uint8_t> payload;

  // 下面两个字段由 JitterBuffer 填写，调用方只读：
  int64_t arrival_us = 0;  // 进队时刻（调用方的单调时钟）
  int64_t wait_us = 0;     // 实际等待时长 = 放行时刻 − arrival_us（buffer 段埋点的值）
};

struct JitterBufferConfig {
  // 关掉时 push 原样立即放行（wait_us=0）。给 A/B 对比与"抖动缓冲是否值得"留出开关，
  // 不是把功能做成可选项：默认是开的。
  bool enabled = true;

  // 队列容量硬上限（帧）。超过就挤掉最旧的一帧，不允许无界增长。
  std::size_t max_frames = 8;

  // 固定目标深度（帧，非自适应时用）。2 帧 = 1 个帧间隔的驻留，是乱序纠正的最小代价。
  std::size_t target_frames = 2;

  // 单帧最长等待（µs）：前端被 PTS 空洞挡住时，最多等这么久就放弃缺失帧继续走。
  int64_t budget_us = 40'000;

  // 标称帧间隔（µs），hold 与空洞判定的起点；之后由 PTS 增量的 EWMA 慢慢跟踪真实帧率。
  int64_t nominal_interval_us = 33'333;

  // 深度自适应（探针侧用）：按「丢包率 + 到达抖动」在档位阶梯上上下移动。
  bool adaptive = false;
  std::size_t min_level = 0;
  std::size_t max_level = 0;
  // 评估窗口：每 window_frames 个入队帧结算一次。
  std::size_t window_frames = 60;
  // 升档门限：窗口丢包率 >= loss_up，或到达抖动 >= jitter_up_frames 个帧间隔。
  double loss_up = 0.02;
  double jitter_up_frames = 1.0;
  // 降档门限：窗口丢包率 <= loss_down 且抖动 <= jitter_down_frames，连续 clean_windows_needed 个窗口。
  // 降档比升档迟钝得多：档位来回抖会把延迟变成一条锯齿，比稳定在偏高的一档更糟。
  double loss_down = 0.005;
  double jitter_down_frames = 0.30;
  int clean_windows_needed = 3;
  // 快降：抖动 <= jitter_down_frames * 0.25 且零丢包时，一个窗口直接回 min_level。
  // 慢降 3 窗口 × 每窗口 60 帧，从 2 档降到 0 档要 6 个窗口（约 12 s@30fps），
  // 恢复期比"问题本身持续的时间"还长——延迟在抖动消失后仍然被收着。
  bool fast_down = true;
};

// 档位阶梯（帧）。索引即 jb_level。**0 档必须是 1 帧 = 零驻留**：
// hold_us() 在 target <= 1 时返回 0，release_ready() 于是不按深度拦，只在 PTS 空洞上
// 等预算——干净网络下这就是"立即放行"，额外延迟为 0。
// 早期版本从 2 帧起步，实测在 30 fps 干净链路上把端到端从 0.4 ms 抬到 41.6 ms
// （p99 65.8 ms），等于给实时流白送一帧延迟。乱序纠正靠的是 budget 窗口，不是恒定驻留。
// 30 fps 下四档驻留约 0 / 33 / 100 / 233 ms；探针默认最高 2 档，3 档要显式开。
inline constexpr std::size_t kDepthLadder[] = {1, 2, 4, 8};
inline constexpr std::size_t kDepthLadderSize = sizeof(kDepthLadder) / sizeof(kDepthLadder[0]);

struct JitterBufferStats {
  std::size_t depth = 0;        // 当前队列深度（帧）
  std::size_t capacity = 0;     // 容量上限（max_frames）
  std::size_t target_frames = 0;  // 当前档位对应的目标深度（帧）
  std::size_t level = 0;        // 当前档位（自适应时会被改；固定档恒为 0）
  uint64_t switches = 0;        // 档位切换次数（升+降）
  uint64_t pushed = 0;
  uint64_t released = 0;
  uint64_t skipped = 0;       // PTS 空洞（含被容量挤掉的帧）
  uint64_t late_dropped = 0;  // 入队即过期、无法按序输出的帧
  double loss_rate = 0.0;     // 最近一个评估窗口的丢包率（skipped/(pushed+skipped)）
  double jitter_frames = 0.0; // 最近一个评估窗口的到达抖动（以帧间隔为单位）
};

class JitterBuffer {
 public:
  explicit JitterBuffer(JitterBufferConfig cfg = JitterBufferConfig{});

  // 入队并按当前时间放行。返回本次可以按 PTS 升序输出的帧（可能为空）。
  std::vector<JitterFrame> push(JitterFrame frame, int64_t now_us);

  // 只按时间推进（没有新帧时由调用方的超时/指标 tick 驱动）。返回本次放行的帧。
  std::vector<JitterFrame> poll(int64_t now_us);

  // 队首下一次到期（放行）的绝对时刻；空队列返回 0。
  // 调用方据此把 poll 超时压到预算之内，否则"等待预算"只能等下一个包到达才生效。
  int64_t next_deadline_us() const;

  JitterBufferStats stats() const;
  std::size_t depth() const { return q_.size(); }
  std::size_t target_frames() const;
  const JitterBufferConfig& config() const { return cfg_; }

  // 新会话（新连接/新流）时清空：上一代的 PTS 基线不能延续，否则新流前几帧全被判成"迟到"。
  void reset();

 private:
  struct Entry {
    JitterFrame frame;
    uint64_t seq = 0;  // 到达序号：PTS 相同时的次序依据
  };

  void observe_arrival(uint64_t pts_us, int64_t now_us);
  bool gap_before(uint64_t pts_us) const;
  void count_hole(uint64_t pts_us);
  int64_t hold_us() const;
  std::vector<JitterFrame> release_ready(int64_t now_us);
  void evict_one();
  void maybe_adapt();

  JitterBufferConfig cfg_;
  std::deque<Entry> q_;  // 恒按 (pts, seq) 升序
  uint64_t next_seq_ = 0;

  bool have_last_released_ = false;
  uint64_t last_released_pts_ = 0;
  bool have_last_pushed_ = false;
  uint64_t last_pushed_pts_ = 0;
  int64_t last_pushed_arrival_us_ = 0;

  int64_t interval_est_us_ = 33'333;  // PTS 增量 EWMA（跟踪真实帧率）
  double jitter_ewma_us_ = 0.0;       // |到达增量 − PTS 增量| 的 EWMA

  std::size_t window_pushed_ = 0;
  std::size_t window_skipped_ = 0;
  int clean_windows_ = 0;

  JitterBufferStats stats_;
};

}  // namespace cue::media
