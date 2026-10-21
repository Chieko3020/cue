// 管线服务接口：把「被 harness 驱动的媒体管线」抽成抽象后端。
//
// 存在的理由（M7）：工具层与评测层现在硬绑在 ProcessPipelineControl（子进程 + stdout 解析）上，
// 而同一套工具语义将来还要跑在「远端 HTTP 服务」与「录制回放」上。三者可互换的前提是
// 工具层只认识本接口 + PipelineEndpoint，不认识任何具体后端。
//
// 边界：
//  - start/stop 是生命周期；运行期的观测口径（latest/metrics_json）对所有后端一致。
//  - set_param/inject_fault 的**生效方式**允许后端不同（子进程重启 vs HTTP 请求），
//    但「成功/失败 + err 文案」必须如实反映真实结果，不能假装成功。
//  - observation 的 source 枚举与返回文本由各后端自己实现，工具层只透传。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cue/pipeline/metrics.h"

namespace cue::pipeline {

// 管线服务端点描述：工具层/评测层只认识这个，不关心背后是子进程还是 HTTP 服务。
struct PipelineEndpoint {
  enum class Kind { Process, Http };
  Kind kind = Kind::Process;
  // Process 端（原有 ProcessPipelineControl::Config 的字段全部原样保留在这里）
  std::string cued_bin = "./build/cued";
  int ws_port = 9000;
  int probe_port = 9001;
  bool transcode = false;
  int width = 0, height = 0, bitrate_kbps = 1500, fps = 30, gop = 60;
  std::string preset = "veryfast";
  double inject_drop = 0.0;
  uint64_t inject_seed = 42;
  bool fec = true;
  double send_mbps = 10.0;
  std::function<void(const PipelineEvent&)> on_event;
  // Http 端
  std::string http_base;        // 例如 http://127.0.0.1:18080
  int http_timeout_ms = 2000;
};

// 一条被 harness 驱动的媒体管线。工具层（src/pipeline/tools.cpp）只依赖本接口，
// 因此「真子进程 / 远端 HTTP 服务 / 录制回放」三种后端可以互换而不改工具语义。
class IServicePipeline {
 public:
  virtual ~IServicePipeline() = default;
  virtual bool start(std::string& err) = 0;
  virtual void stop() = 0;
  virtual bool running() const = 0;
  virtual PipelineMetrics latest() const = 0;
  virtual void set_event_callback(std::function<void(const PipelineEvent&)> cb) = 0;
  virtual std::vector<PipelineEvent> recent_events(std::size_t n = 0) const = 0;
  virtual bool set_param(const std::string& key, const std::string& value, std::string& err) = 0;
  virtual bool inject_fault(const std::string& kind, double value, std::string& err) = 0;
  virtual std::string observation(const std::string& source, int tail, std::string& err) const = 0;
};

// 按端点类型造实现。Process → ProcessPipelineControl；Http → 将来实现，现在返回
// 明确错误（不要静默返回空指针）。
std::unique_ptr<IServicePipeline> make_pipeline(const PipelineEndpoint& ep, std::string& err);

}  // namespace cue::pipeline
