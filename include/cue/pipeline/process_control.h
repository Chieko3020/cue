// ProcessPipelineControl：用「子进程 + stdout 解析」实现 IPipelineControl
// 这是 M3 的过渡实现：接口与 SPEC §7.3 一致，后续可整体替换成 UDS 后端而不动 agent/工具层。
//
// 已知限制（写进 RECEIPT）：
//  - set_param / inject_fault 通过「重启 cued」生效，会造成流中断（数百 ms）
//  - 指标来自 cued 每秒打印的六段分位，窗口约 1s
#pragma once

#include <atomic>
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
  // 六段 p50（ms）
  double recv_p50 = 0, buffer_p50 = 0, decode_p50 = 0, op_p50 = 0, encode_p50 = 0, send_p50 = 0;
  // 六段 p99（ms）
  double decode_p99 = 0, op_p99 = 0;
  int64_t updated_us = 0;
  uint64_t samples = 0;
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

  bool start(std::string& err);
  void stop();
  bool running() const { return pid_ != 0; }

  // 返回最近一次统计（无样本时 samples=0）
  PipelineMetrics latest() const;

  // key ∈ bitrate_kbps / gop / preset / transcode；通过重启生效
  bool set_param(const std::string& key, const std::string& value, std::string& err);
  bool inject_fault(const std::string& kind, double value, std::string& err);
  std::string observation(const std::string& source, int tail, std::string& err) const;

  const Config& config() const { return cfg_; }

 private:
  bool spawn(std::string& err);
  void reader_loop();
  void parse_line(const std::string& line);

  Config cfg_;
  mutable std::mutex mu_;
  PipelineMetrics metrics_;
  std::vector<std::string> log_tail_;
  int log_fd_ = -1;
  pid_t pid_ = 0;
  std::atomic<bool> stop_reader_{false};
  std::thread reader_;
};

}  // namespace cue::pipeline
