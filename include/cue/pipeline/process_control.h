// ProcessPipelineControl：用「子进程 + UDS 控制通道」实现 IServicePipeline
//
// T-05 起的控制面（技术设计 §4.3）：
//  - set_param / inject_fault / observation / 主动查指标全部走 UDS（/tmp/cue/<pid>/control.sock），
//    **不再重启 cued**——这是 FR-PIPE.5「切换时不断流」与 FR-PIPE.10 的实现点；
//  - stdout 解析保留，但只用于人读日志与 [cue-event] 事件流（SSE/控制台依赖它）；
//  - 控制连接断开（cued 崩了）时，控制类调用返回明确错误，**不会**退回到
//    「重启 cued 让它重新读命令行参数」那条老路；
//  - UDS 建不起来时降级到 stdout 模式：set_param 会明确说「降级模式下改参数需要重启 cued」。
//
// 已知限制（写进 RECEIPT）：
//  - 指标缓存仍来自 cued 每秒的 METRICS 行（窗口约 1 s）；主动查询（refresh_metrics）
//    能拿到"此刻"的快照，但计数器仍是会话累计值，不是窗口值。
#pragma once

#include <sys/types.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "cue/pipeline/control_channel.h"
#include "cue/pipeline/metrics.h"
#include "cue/pipeline/service_pipeline.h"

namespace cue::pipeline {

// 观测数据模型（PipelineEvent / PipelineMetrics / kNoSample / has_sample）已搬到
// cue/pipeline/metrics.h：所有管线后端共用一份口径，本头文件继续把它们带进来，
// 所以原先只 include process_control.h 的代码不需要改动。

class ProcessPipelineControl : public IServicePipeline {
 public:
  // 端点描述的唯一来源是 PipelineEndpoint（M7 起 ProcessPipelineControl::Config 只是它的
  // 别名）：否则两个结构体字段各自演化，将来加字段必有一边漏掉。
  using Config = PipelineEndpoint;

  explicit ProcessPipelineControl(Config cfg);
  ~ProcessPipelineControl() override;

  // exec 握手失败（路径错/无权限）时返回 false 并带 errno 文案，不再"假装启动成功"（#10）
  bool start(std::string& err) override;
  void stop() override;
  // pid_ 由监督线程在子进程被回收时清零，所以对死进程不再返回 true（#11）
  bool running() const override { return pid_.load() != 0; }

  // 返回最近一次统计（无样本时 samples=0，alive 表示 cued 当下是否还活着）
  PipelineMetrics latest() const override;

  // 主动查一次指标（UDS metrics.query）：拿到的是"此刻"而不是 1 Hz 缓存。
  // 控制连接不可用 → false + err（调用方回退 latest()，age_ms 会如实说明新旧）。
  bool refresh_metrics(std::string& err) override;

  // 端到端观测：来自 harness 自己托管的 cue-probe（probe 的 stdout 事件流）。
  // 探针没起/起不来/还没上报时 alive=false 且带 unavailable_reason——消费方必须据此
  // 报 probe_unavailable，不能退回 cued 的 latest() 假装有端到端数据。
  ProbeMetrics probe_latest() const override;

  // 换事件回调（与 Config::on_event 等价，构造后也能改）。
  void set_event_callback(std::function<void(const PipelineEvent&)> cb) override;

  // 最近的事件（n=0 表示缓存里的全部；缓存上限见 kEventMax）。
  // 事件缓存与 on_event 是同一份数据：解析到就入缓存，再回调。
  // 默认实参只在 IServicePipeline 上声明一次，这里不能再给。
  std::vector<PipelineEvent> recent_events(std::size_t n) const override;

  // key ∈ bitrate_kbps / gop / preset / transcode / width / height / fps。
  // 走 UDS 的 pipeline.set_param，**不重启进程**；校验规则由 cued 侧（control_plane）
  // 统一给出，这边不重复一套（重复就会分叉）。width/height 必须成对，0/0 = 沿用解码尺寸。
  bool set_param(const std::string& key, const std::string& value, std::string& err) override;
  // kind ∈ drop / ingress_drop / rate / backlog；value=0 清除全部注入。同样不重启进程。
  bool inject_fault(const std::string& kind, double value, std::string& err) override;
  // 走 UDS 的 obs.fetch；控制连接不可用时退回本地缓存（stdout 解析的日志/事件/计数），
  // 并在文本第一行写明"这一份走的是 stdout"，不让旧数据冒充实时快照。
  std::string observation(const std::string& source, int tail, std::string& err) const override;

  // 不在 IServicePipeline 里：这是「子进程后端」的实现细节（HTTP/回放后端没有 cued 启动参数），
  // 工具层/评测层不得依赖它。按值返回：cfg_ 现在受 cfg_mu_ 保护（控制面会改它）。
  Config config() const;

  // 实际生效的控制通道："uds"（已连上）/ "stdout"（降级或未连接）。指标里同名。
  std::string control_channel() const;
  // 最近一次控制面错误（降级原因/断连原因/超时），空 = 没有异常
  std::string last_control_error() const;

 private:
  bool spawn(std::string& err);        // 对外：拿 spawn_mu_ 后调 spawn_locked
  bool spawn_locked(std::string& err); // 内部：调用方已持 spawn_mu_（probe + cued）
  bool spawn_cued_locked(std::string& err);
  void kill_cued_locked();
  // probe 是 cued 的下游：先起 probe 再起 cued，停的时候倒序。spawn_probe_locked
  // 失败不阻断 cued 启动（否则故障会被误报成 pipeline_start_failed），
  // 失败原因记在 probe_unavailable_ 里，由 probe_latest() 带出去。
  bool spawn_probe_locked(std::string& err);
  void kill_probe_locked();  // 调用方已持 spawn_mu_：SIGTERM → SIGKILL → waitpid
  void read_loop(int fd, std::atomic<bool>& stop_flag);
  void reader_loop();
  void probe_reader_loop();
  void parse_line(const std::string& line);
  void parse_event_line(const std::string& body);           // 已去掉事件标签的 JSON 体
  void parse_probe_metric(const nlohmann::json& data);      // probe.metric → probe_metrics_
  void emit_local_event(std::string type, nlohmann::json data);  // harness 合成的事件
  void record_event(PipelineEvent ev);  // 入缓存（最近 kEventMax 条）后回调
  void supervise_cued_once(pid_t pid);
  void supervise_probe_once(pid_t pid);
  void supervisor_loop();
  void push_log(std::string line);         // 内部持 mu_
  void push_log_locked(std::string line);  // 调用方已持 mu_

  // ---- 控制通道（UDS） ----
  // cued 的 socket 路径由 pid 决定：/tmp/cue/<pid>/control.sock（目录 0700，cued 自己建）
  static std::string control_socket_path(pid_t pid);
  // 调用方持 spawn_mu_：等 socket 出现 → 连上 → 一次性下发启动参数。
  // 失败**不阻断启动**（降级到 stdout），但会把原因记进 last_control_error_ 并写日志。
  bool connect_control_locked(std::string& err);
  void close_control_locked();
  // 发一条控制请求（内部持 ctl_mu_）。连接不可用时先尝试懒重连一次（**不重启 cued**）。
  bool control_call(const char* op, const nlohmann::json& args, nlohmann::json& result,
                    std::string& err) const;
  // METRICS 行与 UDS metrics.query 共用同一份解析（两处口径必须一致）。
  // from_uds=true 时只在"比缓存新"时写入，且不递增 samples（见 .cpp 的注释）。
  bool apply_metrics_json(const nlohmann::json& j, bool from_uds);
  // 控制通道不可用时的本地观测（数据源仍是 stdout 解析出来的缓存）
  std::string local_observation(const std::string& source, int tail) const;
  Config cfg_snapshot() const;

  mutable std::mutex cfg_mu_;    // 保护 cfg_（控制面会改它，spawn 时读它）
  Config cfg_;
  mutable std::mutex mu_;        // 保护 metrics_ / log_tail_
  mutable std::mutex spawn_mu_;  // 串行化 spawn/stop（工具调用 vs 监督线程重启）
  PipelineMetrics metrics_;
  std::deque<std::string> log_tail_;  // #28：deque 头删 O(1)
  std::deque<PipelineEvent> events_;   // 最近 kEventMax 条事件（observation/SSE 用）
  std::function<void(const PipelineEvent&)> on_event_;  // 受 mu_ 保护
  // codec_config（0x03）缓存：来自 cued 的 codec.config 事件，observation(frames) 作为
  // 媒体元信息回给 agent。不在 spawn 时清空——浏览器用 annexb，配置在 IDR 里重复出现，
  // 而 agent 恰恰在"重启修复后"最需要知道当前 SPS/PPS。受 mu_ 保护。
  std::string codec_name_ = "h264";
  std::string sps_hex_;
  std::string pps_hex_;
  std::string codec_raw_hex_;
  // 探针：与 cued 完全独立的一路子进程 + stdout 解析（同一套事件格式，见 wire.h）。
  ProbeMetrics probe_metrics_;       // 受 mu_ 保护
  std::string probe_unavailable_;    // 起不来/退出时留下的原因，受 mu_ 保护
  // probe 最后一行人读日志：起不来时最可能的原因是端口被占，把原文留在原因里
  // 比只写"probe 不可用"能让人直接定位（受 mu_ 保护）
  std::string probe_last_log_;
  bool probe_enabled_ = false;       // 构造时定：probe_bin 非空才托管探针（受 spawn_mu_）
  int log_fd_ = -1;                   // 受 spawn_mu_ 保护；reader 线程只读
  int probe_fd_ = -1;                 // 同上
  std::atomic<pid_t> pid_{0};         // #26：所有访问都走原子量，不再无锁读写
  std::atomic<pid_t> probe_pid_{0};
  std::atomic<bool> stop_reader_{false};
  std::atomic<bool> stop_probe_reader_{false};
  std::atomic<bool> stop_super_{false};
  std::atomic<bool> stopping_{false};  // stop() 正在进行：监督线程不要重启
  std::thread reader_;
  std::thread probe_reader_;
  std::thread supervisor_;
  std::atomic<int> restarts_{0};        // cued 已自动重启次数，防止崩溃循环（写受 spawn_mu_）
  // ---- 控制通道状态 ----
  // ctl_ 是**一条**长连接：控制请求严格一问一答，所以一个 fd 足够，也保证了
  // 服务端的连接线程不会因为"每请求一条连接"而频繁创建销毁。
  mutable std::mutex ctl_mu_;                // 保护 ctl_ / last_control_error_
  mutable std::unique_ptr<ControlClient> ctl_;
  mutable std::string last_control_error_;   // 受 ctl_mu_
  mutable std::atomic<ControlChannelKind> ctl_kind_{ControlChannelKind::None};
  std::atomic<int> probe_restarts_{0};  // probe 的重启预算单独算（互不牵连）
};

}  // namespace cue::pipeline
