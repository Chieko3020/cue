// ProcessPipelineControl：用「子进程 + stdout 解析」实现 IServicePipeline
// 这是 M3 的过渡实现：接口与 SPEC §7.3 一致，后续可整体替换成 UDS 后端而不动 agent/工具层。
//
// 已知限制（写进 RECEIPT）：
//  - set_param / inject_fault 通过「重启 cued」生效，会造成流中断（数百 ms）
//  - 指标来自 cued 每秒打印的 METRICS 行（JSON），窗口约 1s
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

  // 换事件回调（与 Config::on_event 等价，构造后也能改）。
  void set_event_callback(std::function<void(const PipelineEvent&)> cb) override;

  // 最近的事件（n=0 表示缓存里的全部；缓存上限见 kEventMax）。
  // 事件缓存与 on_event 是同一份数据：解析到就入缓存，再回调。
  // 默认实参只在 IServicePipeline 上声明一次，这里不能再给。
  std::vector<PipelineEvent> recent_events(std::size_t n) const override;

  // key ∈ bitrate_kbps / gop / preset / transcode / width / height；通过重启生效
  // width/height 必须成对给出且为偶数，0/0 表示沿用解码尺寸
  bool set_param(const std::string& key, const std::string& value, std::string& err) override;
  bool inject_fault(const std::string& kind, double value, std::string& err) override;
  std::string observation(const std::string& source, int tail, std::string& err) const override;

  // 不在 IServicePipeline 里：这是「子进程后端」的实现细节（HTTP/回放后端没有 cued 启动参数），
  // 工具层/评测层不得依赖它。
  const Config& config() const { return cfg_; }

 private:
  bool spawn(std::string& err);        // 对外：拿 spawn_mu_ 后调 spawn_locked
  bool spawn_locked(std::string& err); // 内部：调用方已持 spawn_mu_
  void reader_loop();
  void parse_line(const std::string& line);
  void parse_event_line(const std::string& body);           // 已去掉事件标签的 JSON 体
  void emit_local_event(std::string type, nlohmann::json data);  // harness 合成的事件
  void record_event(PipelineEvent ev);  // 入缓存（最近 kEventMax 条）后回调
  void supervisor_loop();
  void push_log(std::string line);         // 内部持 mu_
  void push_log_locked(std::string line);  // 调用方已持 mu_

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
  int log_fd_ = -1;                   // 受 spawn_mu_ 保护；reader 线程只读
  std::atomic<pid_t> pid_{0};         // #26：所有访问都走原子量，不再无锁读写
  std::atomic<bool> stop_reader_{false};
  std::atomic<bool> stop_super_{false};
  std::atomic<bool> stopping_{false};  // stop() 正在进行：监督线程不要重启
  std::thread reader_;
  std::thread supervisor_;
  std::atomic<int> restarts_{0};  // 本会话已自动重启次数，防止崩溃循环（写受 spawn_mu_）
};

}  // namespace cue::pipeline
