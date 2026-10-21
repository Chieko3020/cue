// 管线观测数据模型：指标快照与事件。
// 从 process_control.h 抽出（结构体字段、注释、语义一字未改），供所有管线后端
// （子进程 / HTTP 服务 / 录制回放）共用——观测口径只允许有一处定义。
#pragma once

#include <sys/types.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include <nlohmann/json.hpp>

namespace cue::pipeline {

// 分位缺失的哨兵：NaN 而不是 0（见 PipelineMetrics 的注释）
inline constexpr double kNoSample = std::numeric_limits<double>::quiet_NaN();

// 该分位是否真的采到了（NaN 表示没有样本）
inline bool has_sample(double v) { return std::isfinite(v); }

// 一条事件：cued stdout 里的 [cue-event] 行，或 harness 自己合成的控制面事件。
// data 原样保留（不同 ev 的字段不同，消费者按键取），t_us 是事件时间（unix us），
// seen_us 是本进程读到它的单调时钟时刻——两者口径不同，跨进程比较只能用 seen_us。
struct PipelineEvent {
  std::string type;
  nlohmann::json data = nlohmann::json::object();
  int64_t t_us = 0;
  int64_t seen_us = 0;
};

struct PipelineMetrics {
  double fps = 0;
  uint64_t frames_in = 0;
  uint64_t frames_out = 0;
  uint64_t dropped = 0;
  uint64_t injected = 0;
  uint64_t decoded = 0;
  uint64_t decode_errors = 0;
  uint64_t rejected = 0;  // 协议边界拒绝数（wire.h 的拒绝原因分类在 cued 侧汇总）
  // cued METRICS 行里原本就有、但过去没往 agent 侧暴露的计数（0 是真实读数，不是缺省）
  uint64_t send_errors = 0;    // sendto 失败（ENOBUFS/EMSGSIZE），不是"已发送"
  uint64_t encode_errors = 0;  // 编码失败（尺寸/格式/编码器错误）
  uint64_t decode_drops = 0;   // 解码器未消费的包（H264Decoder::stats().dropped）
  uint64_t budget_drops = 0;   // 令牌桶不足丢的帧
  // 六段 p50（ms）。NaN = 这一段没有样本（cued 的 METRICS 对应段是 null）：
  // "没采到"与"花了 0 ms"是两件事，消费者必须输出 null/n/a，不能用 0 冒充。
  double recv_p50 = kNoSample, buffer_p50 = kNoSample, decode_p50 = kNoSample,
         op_p50 = kNoSample, encode_p50 = kNoSample, send_p50 = kNoSample;
  // 六段 p99（ms，同上：只有 cued 真正打印了 p99 的段才有值）
  double decode_p99 = kNoSample, op_p99 = kNoSample, encode_p99 = kNoSample;
  int64_t updated_us = 0;
  uint64_t samples = 0;
  // 快照时的存活状态：cued 被 kill 后 latest() 不能继续撒谎（review #11）
  bool alive = false;
  // 指标属于哪个 cued 会话（同一个 cue 进程内可能重启过多代）；消费者据此判断跨进程拼接（#25）
  pid_t pid = 0;
};

}  // namespace cue::pipeline
