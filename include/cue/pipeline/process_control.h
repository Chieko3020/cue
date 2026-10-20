// ProcessPipelineControl：用「子进程 + stdout 解析」实现 IPipelineControl
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
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cue::pipeline {

struct PipelineMetrics {
  double fps = 0;
  uint64_t frames_in = 0;
  uint64_t frames_out = 0;
  uint64_t dropped = 0;
  uint64_t injected = 0;
  uint64_t decoded = 0;
  uint64_t decode_errors = 0;
  uint64_t rejected = 0;  // 协议边界拒绝数（wire.h 的拒绝原因分类在 cued 侧汇总）
  // 六段 p50（ms）
  double recv_p50 = 0, buffer_p50 = 0, decode_p50 = 0, op_p50 = 0, encode_p50 = 0, send_p50 = 0;
  // 六段 p99（ms，仅 cued 打印了的段有值）
  double decode_p99 = 0, op_p99 = 0, encode_p99 = 0;
  int64_t updated_us = 0;
  uint64_t samples = 0;
  // 快照时的存活状态：cued 被 kill 后 latest() 不能继续撒谎（review #11）
  bool alive = false;
  // 指标属于哪个 cued 会话（同一个 cue 进程内可能重启过多代）；消费者据此判断跨进程拼接（#25）
  pid_t pid = 0;
};

class ProcessPipelineControl {
 public:
  struct Config {
    std::string cued_bin = "./build/cued";
    int ws_port = 9000;
    int probe_port = 9001;
    // 初始参数
    bool transcode = false;
    int width = 0;
    int height = 0;
    int bitrate_kbps = 1500;
    int fps = 30;
    int gop = 60;
    std::string preset = "veryfast";
    double inject_drop = 0.0;
    uint64_t inject_seed = 42;
    bool fec = true;
    double send_mbps = 10.0;
  };

  explicit ProcessPipelineControl(Config cfg);
  ~ProcessPipelineControl();

  // exec 握手失败（路径错/无权限）时返回 false 并带 errno 文案，不再"假装启动成功"（#10）
  bool start(std::string& err);
  void stop();
  // pid_ 由监督线程在子进程被回收时清零，所以对死进程不再返回 true（#11）
  bool running() const { return pid_.load() != 0; }

  // 返回最近一次统计（无样本时 samples=0，alive 表示 cued 当下是否还活着）
  PipelineMetrics latest() const;

  // key ∈ bitrate_kbps / gop / preset / transcode / width / height；通过重启生效
  // width/height 必须成对给出且为偶数，0/0 表示沿用解码尺寸
  bool set_param(const std::string& key, const std::string& value, std::string& err);
  bool inject_fault(const std::string& kind, double value, std::string& err);
  std::string observation(const std::string& source, int tail, std::string& err) const;

  const Config& config() const { return cfg_; }

 private:
  bool spawn(std::string& err);        // 对外：拿 spawn_mu_ 后调 spawn_locked
  bool spawn_locked(std::string& err); // 内部：调用方已持 spawn_mu_
  void reader_loop();
  void parse_line(const std::string& line);
  void supervisor_loop();
  void push_log(std::string line);         // 内部持 mu_
  void push_log_locked(std::string line);  // 调用方已持 mu_

  Config cfg_;
  mutable std::mutex mu_;        // 保护 metrics_ / log_tail_
  mutable std::mutex spawn_mu_;  // 串行化 spawn/stop（工具调用 vs 监督线程重启）
  PipelineMetrics metrics_;
  std::deque<std::string> log_tail_;  // #28：deque 头删 O(1)
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
