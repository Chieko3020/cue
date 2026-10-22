// HttpServicePipeline：用「HTTP 控制协议」实现 IServicePipeline。
//
// 存在的理由（M7 / AC-16）：同一套工具语义要能驱动两种后端——
//  1) 本机子进程（ProcessPipelineControl，stdout 解析）
//  2) 远端/假域 HTTP 服务（本文件，REST 控制）
// 协议与 apps/cue-serve.cpp 对齐（GET /api/health、GET /api/state、POST /api/action），
// 所以同一个后端既能驱动 M5 控制台，也能驱动只实现这套 REST 的假域服务。
//
// 契约：
//  - start/stop 失败必须返回 false + 真实 err（连接失败、HTTP 非 200、{"ok":false} 都算失败），
//    绝不假装成功。
//  - latest()/running() 每次请求都带 http_timeout_ms 超时；超时/坏响应返回 NaN 分位 +
//    alive=false，不抛异常（评测循环里一条链路抖动不该带走整轮评测）。
//  - recent_events 只读本地缓存（收到 state 里的 events 就入缓存并触发回调，按 t_us 去重），
//    不再额外发请求——事件是"已经看到过的观测"，不是每次查询都该重放的东西。
//  - latest()/recent_events() 是 const 且被监控线程与 agent 线程并发调用：缓存用 mutex 保护。
#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "cue/common/service_pipeline.h"

namespace cue::pipeline {

class HttpServicePipeline : public IServicePipeline {
 public:
  explicit HttpServicePipeline(const PipelineEndpoint& ep);

  bool start(std::string& err) override;
  void stop() override;
  bool running() const override;
  PipelineMetrics latest() const override;
  // 假域 / 远端 REST 服务没有探针进程：端到端指标不可用。这里如实返回
  // alive=false、samples=0 并带上原因，绝不拿 latest() 的服务端自报凑一份"端到端"。
  ProbeMetrics probe_latest() const override;
  void set_event_callback(std::function<void(const PipelineEvent&)> cb) override;
  std::vector<PipelineEvent> recent_events(std::size_t n = 0) const override;
  bool set_param(const std::string& key, const std::string& value, std::string& err) override;
  bool inject_fault(const std::string& kind, double value, std::string& err) override;
  std::string observation(const std::string& source, int tail, std::string& err) const override;

 private:
  // 一次 HTTP 调用的结果：传输层错误与业务层 ok=false 分开表达（err 非空即失败）
  struct Reply {
    bool ok = false;
    long http_status = 0;
    nlohmann::json body = nlohmann::json::object();
    std::string err;
  };

  Reply request(const char* method, const std::string& path, const nlohmann::json* body) const;
  // POST /api/action（两种 body 写法都由服务端认，这里统一用 action 写法）
  Reply action(const nlohmann::json& body) const;
  // 拉一次 /api/state 刷新本地缓存；失败时把缓存标成 alive=false 并返回 false
  bool refresh() const;

  std::string base_;
  int timeout_ms_ = 2000;
  mutable std::mutex mu_;
  mutable PipelineMetrics metrics_;
  mutable bool running_ = false;
  mutable std::deque<PipelineEvent> events_;
  // 去重键集合（按 t_us + type；没有时间戳的事件退回 data）。淘汰时按键重算，见 key_of。
  mutable std::unordered_set<std::string> event_keys_;
  std::function<void(const PipelineEvent&)> on_event_;  // 受 mu_ 保护
};

}  // namespace cue::pipeline
