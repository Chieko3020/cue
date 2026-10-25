// UDS `metrics.query` 响应的去重水位：**只**回答「这一份响应比上一次采纳的是不是更新」。
//
// 为什么必须和 PipelineMetrics::updated_us 分开（这是真踩出来的坑，别合回去）：
//   · updated_us 是**本机时钟**口径：tools 层用 `now_us() - updated_us` 算 age_ms，
//     所以它只能是「本进程解析/写入这条指标的时刻」；
//   · remote_t 是 cued 侧 report_once 的采样时刻（响应里的 t_us，来自它的 now_us()）。
//     两个数虽然都是单调微秒，语义却不同：一个是「数据产生于何时」，另一个是
//     「我何时收到它」。拿它们直接比大小，问的其实是「这份数据是不是比我上次收到
//     那份更晚产生」——而 stdout 每秒那行 METRICS 已经把 updated_us 顶到「刚刚」，
//     于是 `remote_t <= metrics_.updated_us` 结构性成立，每一份 UDS 响应都被判成
//     「不比缓存新」整份丢弃：metrics.query 只剩 stdout 也有的字段，params（cued
//     ParamStore 的编码档位真值）永远进不了缓存 → 观测面 encode_width/height 恒 null。
//   · 一旦 cued 跑在另一台机器、换用进程相对时钟，或哪天 stdout 行开始自带 t_us，
//     这两个数就**真的**不可比了。把去重绑在 updated_us 上，这两种情况下还会以
//     同样的方式悄悄错——所以判据只能是「上一次采纳的 remote_t」，而不是任何
//     本机时间戳。
//   · 本条判断是 fail-soft 的：remote_t == 0（旧版 cued 不带 t_us）没有可比的水位，
//     一律接受，也不推进水位（否则一个 0 会把水位打回原点）。
//
// 水位的生命周期属于「一代 cued」：cued 换代（spawn / 自动重启）时必须清零，
// 见 process_control.cpp 的 spawn_cued_locked。清零点放在换代处而不是每次查询前，
// 因为「同一代之内响应单调递增」正是这条判据成立的前提。
//
// 线程安全：本头文件不自己加锁。ProcessPipelineControl 用 mu_ 把水位与 metrics_
// 放在同一个临界区里更新（判定 + 推进 + 写缓存必须原子，否则两份响应能交错通过）。
#pragma once

#include <cstdint>

namespace cue::pipeline {

// 这一份 UDS 响应是否比上一次**采纳**的更新？
//   remote_t          本次响应里的 t_us（0 = 旧版 cued 没带时间戳）
//   last_remote_t_us  上一次采纳的 remote_t（0 = 这一代还没采纳过）
// 判据是严格大于：同一份响应（t_us 相同）重复应用不刷新 age_ms，也不改写缓存。
inline bool uds_metrics_is_newer(int64_t remote_t, int64_t last_remote_t_us) {
  if (remote_t == 0) return true;  // 没有可比的水位：接受
  return remote_t > last_remote_t_us;
}

// 去重水位：把「判定」与「推进」绑在一起，调用方不可能忘记推进（忘了就会让
// 同一份响应反复通过，age_ms 永远新鲜）。
class UdsMetricsWatermark {
 public:
  // true = 这份响应应当被采纳（调用方随后写缓存，水位已推进）
  bool accept(int64_t remote_t) {
    if (!uds_metrics_is_newer(remote_t, last_us_)) return false;
    if (remote_t != 0) last_us_ = remote_t;
    return true;
  }
  // cued 换代时调用（见 process_control.cpp 的 spawn_cued_locked）
  void reset() { last_us_ = 0; }
  int64_t last() const { return last_us_; }

 private:
  int64_t last_us_ = 0;
};

}  // namespace cue::pipeline
