// 控制面：把 UDS 上的控制请求落到 cued 的**运行期状态**上（技术设计文档 §4.3）。
//
// 为什么需要这一层（而不是把 handler 写在 cued.cpp 里）：
//  1) 改参数必须不重启进程（FR-PIPE.5 / FR-PIPE.10）。要做到这一点，「参数」得先变成
//     一个有明确同步语义的对象（ParamStore：一把锁 + 每帧取快照），而不是 main() 里
//     一堆启动时定死的局部变量。参数对象与 op 语义放在库里，单测才能在没有媒体管线、
//     没有 FFmpeg 的情况下把 7 个 op 逐个钉死。
//  2) cued 与 ProcessPipelineControl 必须用**同一套**校验规则（范围、配对、幂等），
//     否则「工具层说成功、cued 说非法」这种分叉迟早出现。
//
// 线程模型：op 在连接自己的读线程里处理；写参数只改 ParamStore（内部加锁），
// 真正的编码器重建由媒体线程在下一帧按 generation 变化懒执行——控制线程永不触碰
// 解码器/编码器/会话，这是「不断流」的结构性保证。
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cue/pipeline/control_channel.h"

namespace cue::pipeline {

// 运行期可改的参数（对应 cued 的启动参数里那些**不涉及结构**的部分）。
// width/height = 0/0 表示沿用解码尺寸；inject_* 是故障注入档位。
//
// 不含 ws_port / bind / probe_* / jitter_* / fec：改它们要重建监听或抖动缓冲，
// 属于结构性参数——这类参数仍然必须重启 cued（控制面会明确拒绝，而不是假装改成功）。
struct RuntimeParams {
  bool transcode = false;
  int width = 0;
  int height = 0;
  int bitrate_kbps = 1500;
  int fps = 30;
  int gop = 60;
  int threads = 1;
  std::string preset = "veryfast";
  // 发送侧（故障注入的一种）：令牌桶速率与发送排队节奏
  double send_mbps = 10.0;
  double queue_fps = 0.0;
  // FEC 组大小（FR-PIPE.9）：每 N 个数据包一个 XOR 组，1..64。
  // 它不是结构性参数：发送侧每帧按它现切组，改完下一帧就生效，不重建编码器/不断流。
  int fec_group = 8;
  // AIMD（技术设计 §4.2）：仅 transcode=on 时按 feedback 调节码率。
  // **默认关闭**：开启后健康链路（loss<2%）每 3 个窗口升 5%，每升一次都要懒重建编码器；
  // 实测在 enc-placebo 的修复窗口里，每秒一次的重建会把探针抖动缓冲推高一档，
  // e2e p99 从 ~45 ms 抬到 108 ms（擦着 100 ms 的 SLA）。评测要确定性时默认值必须是它，
  // 需要拥塞控制就显式开：`--aimd on` 或 set_param aimd=on。
  bool aimd = false;
  // 注入档位（%）
  double inject_drop = 0.0;
  double inject_ingress_drop = 0.0;
  // 延迟注入（FR-PIPE.10）：固定延迟与抖动幅度，单位**毫秒**。
  // 与上面的百分比注入分开两个字段：两者在指标上留下的证据不同
  // （丢包只涨 injected，延迟只涨 delayed_pkts / send 段分位），混成一个"value"
  // 会让 agent 没法从读数反推是哪种根因。0 = 关闭。
  double inject_delay_ms = 0.0;
  double inject_jitter_ms = 0.0;
  uint64_t inject_seed = 42;
};

bool same_params(const RuntimeParams& a, const RuntimeParams& b);

// 观测里**可以**出现的编码档位：transcode/尺寸/码率/GOP/preset/fps。
// 注入档位（inject_drop / inject_ingress_drop / send_mbps / queue_fps）故意不在这里：
// agent 必须从指标推断根因，把「标准答案」写进观测等于给它抄（沿用旧 observation(procs)
// 的约定）。控制面自己仍持有全量参数（runtime_params_json）。
nlohmann::json encoder_params_json(const RuntimeParams& p);
nlohmann::json runtime_params_json(const RuntimeParams& p);

// 语义校验 + 写进 draft。失败返回 false 并写 err（文案就是工具层最终看到的那句）。
// 这是**唯一**一份校验：cued 用它，ProcessPipelineControl 不再自己重复一套。
bool apply_param(const std::string& key, const std::string& value, RuntimeParams& draft,
                 std::string& err);
bool apply_fault(const std::string& kind, double value, RuntimeParams& draft, std::string& err);
// 该键是否影响编码器（决定要不要重建编码器；注入类参数一律 false）
bool encoder_relevant_key(const std::string& key);

// 参数存储：一把锁 + 整份快照。
//
// 为什么不是一堆 std::atomic：媒体线程读的是**一组**参数（比如 width 与 height 必须
// 同时生效），逐个 atomic 会让它读到「width 改了、height 还没改」的半截状态。
// 快照是几十字节的拷贝，每帧一次的开销远小于一次 x264 调用。
//
// 两个 generation：
//  - generation：任何参数变化都 +1（媒体线程据此重读注入档位/令牌桶速率）；
//  - encoder_generation：只有影响编码器的键才 +1（据此决定要不要重建编码器）。
//    分开的理由：改注入丢包不该关掉编码器——那会让「注入故障」变成一次真实的断流。
class ParamStore {
 public:
  struct Snapshot {
    RuntimeParams params;
    uint64_t generation = 0;
    uint64_t encoder_generation = 0;
  };

  explicit ParamStore(RuntimeParams init = RuntimeParams{});

  Snapshot snapshot() const;
  // 单键设置。changed=false 表示值没变（幂等：不推进 generation，也就不会重建编码器）。
  bool set_param(const std::string& key, const std::string& value, bool& changed,
                 std::string& err);
  bool set_fault(const std::string& kind, double value, bool& changed, std::string& err);
  // pipeline.start 的批量下发：要么全应用，要么一个字段都不改。
  bool apply_batch(const nlohmann::json& args, bool& changed, std::string& err);

  nlohmann::json encoder_json() const;
  nlohmann::json full_json() const;

 private:
  mutable std::mutex mu_;
  RuntimeParams p_;
  uint64_t gen_ = 0;
  uint64_t enc_gen_ = 0;
};

// 控制面：socket 生命周期 + 8 个 op（7 个实现 + not implemented 的两个会话 op）
// + 事件广播 + 人读日志/事件环形缓冲。
class ControlPlane {
 public:
  struct Providers {
    // metrics.query：返回与 METRICS 行**同键名**的快照（含 t_us 与 samples）。
    // 没有样本时由提供方自己写 samples=0 + unavailable_reason——控制面不替它编数。
    std::function<nlohmann::json(int window_ms)> metrics;
    // obs.fetch 的两个数据源（frames 是计数器，procs 是进程自述）。
    // 用 ordered_json 是为了 observation 的字段顺序稳定可读（人读的文本要能一眼扫完）。
    std::function<nlohmann::ordered_json()> frames;
    std::function<nlohmann::ordered_json()> procs;
    // 事件出口：控制面产生的事件（fault.applied / pipeline.param …）必须与 cued 自己的
    // 事件走同一条出口（stdout + 订阅广播），否则 agent 看到的流里会缺一半。
    std::function<void(const std::string& ev, const nlohmann::json& data)> emit;
    // pipeline.stop：响应写出去之后才允许停（cued 用它优雅退出）
    std::function<void()> on_stop;
  };

  ControlPlane(ParamStore& store, Providers providers);
  ~ControlPlane();

  ControlPlane(const ControlPlane&) = delete;
  ControlPlane& operator=(const ControlPlane&) = delete;

  // 建 UDS 控制通道。失败 = 降级到 stdout 模式（cued 必须继续跑，但要在指标里
  // 如实写 control_channel=stdout，并把原因给出去）。
  bool start(const std::string& socket_path, std::string& err);
  void stop();

  ControlChannelKind channel() const { return channel_; }
  bool degraded() const { return channel_ == ControlChannelKind::None; }
  const char* channel_tag() const { return control_channel_tag(channel_); }
  const char* channel_detail() const { return control_channel_detail(channel_); }
  const std::string& unavailable_reason() const { return unavailable_reason_; }
  uint64_t ops() const { return ops_.load(); }

  // 事件广播（cued 的 emit_event 调用）：入事件环形 + 推给订阅者。
  // body 形如 {"ev":"...","t":<us>,"data":{...}}（与 stdout 的事件行同形）。
  void publish(const nlohmann::json& body);
  // 人读日志环形（obs.fetch logs 的数据源，受控上限，不能无限涨）
  void push_log(std::string line);

 private:
  nlohmann::json handle(const nlohmann::json& request, ControlSession& session);
  nlohmann::json op_pipeline_start(const nlohmann::json& args, std::int64_t id);
  nlohmann::json op_pipeline_stop(std::int64_t id);
  nlohmann::json op_set_param(const nlohmann::json& args, std::int64_t id);
  nlohmann::json op_metrics(const nlohmann::json& args, std::int64_t id);
  nlohmann::json op_obs(const nlohmann::json& args, std::int64_t id);
  nlohmann::json op_fault(const nlohmann::json& args, std::int64_t id);
  nlohmann::json op_subscribe(const nlohmann::json& args, ControlSession& session,
                              std::int64_t id);
  std::string tail_of_logs(int tail) const;
  std::string tail_of_events(int tail) const;

  ParamStore& store_;
  Providers prov_;
  std::unique_ptr<ControlServer> server_;  // 只有真的建起来才有；降级时为空
  ControlChannelKind channel_ = ControlChannelKind::None;
  std::string unavailable_reason_;
  std::atomic<uint64_t> ops_{0};

  mutable std::mutex ring_mu_;
  std::deque<nlohmann::json> events_;
  std::deque<std::string> logs_;
};

// 与 cued 的 obs.fetch 同口径的文本上限：响应必须装得进一条 64 KiB 消息。
inline constexpr std::size_t kMaxObservationText = 48 * 1024;
// 环形缓冲上限（与 harness 侧的 kEventMax / kLogTailMax 同量级）
inline constexpr std::size_t kControlEventMax = 256;
inline constexpr std::size_t kControlLogMax = 200;

}  // namespace cue::pipeline
