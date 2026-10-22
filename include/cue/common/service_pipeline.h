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
//  - sessions_json 只对**能维护会话状态**的后端（Process → cued）有真实数据；
//    其余后端必须返回空表 + err 说明原因（FR-UI.1 的「没有设备」与「取不到」由此区分）。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cue/common/metrics.h"

namespace cue::pipeline {

// 管线服务端点描述：工具层/评测层只认识这个，不关心背后是子进程还是 HTTP 服务。
struct PipelineEndpoint {
  enum class Kind { Process, Http };
  Kind kind = Kind::Process;
  // Process 端（原有 ProcessPipelineControl::Config 的字段全部原样保留在这里）
  std::string cued_bin = "./build/cued";
  int ws_port = 9000;
  int probe_port = 9001;
  // 探针（cue-probe）由 harness 自己托管：probe 是 cued 的下游，先起 probe 再起 cued，
  // 停的时候倒序。probe_bin 为空 = 显式不启动本地探针（此时端到端指标必然不可用）。
  std::string probe_bin = "./build/cue-probe";
  std::string probe_host = "127.0.0.1";  // probe 的绑定地址 = cued 的下行目标
  // 跨机上报目标（"host:port,host:port"）：非空时 probe 除 stdout 外再用 TCP 上报
  // JSON-line；空 = 只打 stdout（本地模式，harness 直接读管道）。
  std::string probe_report_addrs;
  bool transcode = false;
  int width = 0, height = 0, bitrate_kbps = 1500, fps = 30, gop = 60;
  std::string preset = "veryfast";
  double inject_drop = 0.0;
  // 上行（浏览器 → cued）丢包百分比：与出向 inject_drop 分开，因为两者在指标上留下
  // 的是不同证据（见 ProcessPipelineControl::inject_fault 的注释）。
  double inject_ingress_drop = 0.0;
  // 发送侧排队节奏（帧/s）：>0 时不丢包，按这个节奏排空发送队列（queue_backlog）。
  double queue_fps = 0.0;
  // 延迟注入（FR-PIPE.10）：固定延迟与抖动的幅度，单位**毫秒**。
  // 与上面的百分比注入分开：它们压住的是「什么时候发」而不是「发不发」，
  // 所以两者的可观测证据不相交（injected 不涨 / send 段分位涨）。
  double inject_delay_ms = 0.0;
  double inject_jitter_ms = 0.0;
  uint64_t inject_seed = 42;
  bool fec = true;
  // FEC 组大小（FR-PIPE.9）：每 N 个数据包一组。运行期可通过 set_param fec_group 改；
  // 这个字段只是「下一次 spawn 的命令行初值」。
  int fec_group = 8;
  // AIMD（技术设计 §4.2）：仅 transcode=on 时生效。默认关（评测确定性：开启会在
  // 无丢包时持续升码率并重建编码器，实测抬高修复态 e2e 时延）。harness spawn 时
  // 显式下传该值，保证 cued 重启后与运行期一致。
  bool aimd = false;
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
  // 端到端观测（探针）。没有探针的后端（假域 / 回放）返回 alive=false、samples=0 并带
  // unavailable_reason，绝不"用 cued 的数字凑一份端到端"。
  // 单独一个接口而不是塞进 latest()：两者的样本源与可用性不同，混在一起时
  // 「cued 有数、probe 没数」会被读成「端到端正常」——那正是要防的假绿灯。
  virtual ProbeMetrics probe_latest() const = 0;
  virtual void set_event_callback(std::function<void(const PipelineEvent&)> cb) = 0;
  virtual std::vector<PipelineEvent> recent_events(std::size_t n = 0) const = 0;
  virtual bool set_param(const std::string& key, const std::string& value, std::string& err) = 0;
  virtual bool inject_fault(const std::string& kind, double value, std::string& err) = 0;
  virtual std::string observation(const std::string& source, int tail, std::string& err) const = 0;
  // 主动查一次指标快照（Process 后端走 UDS 的 metrics.query；其余后端默认不支持）。
  // 默认 false 而不是回退 latest()：调用方要能区分「这份数是刚查的」与「这是 1 Hz 缓存」，
  // 而 latest() 被监控线程/SSE 高频调用，不能让它变成一次可能超时的网络往返。
  virtual bool refresh_metrics(std::string& err);

  // 设备会话表（FR-UI.1 的数据源）。返回 SessionRegistry::sessions_json 的形状：
  // {"sessions":[{session_id, device_id, peer, caps, state, lease_ms, expires_in_ms, ...}],
  //  "count":N, "live":K, "max_sessions":M, "lease_ms":L}。
  //
  // 为什么带默认实现而不是纯虚：会话表是**新增**的观测口径，与 start/stop 那样的
  // 核心生命周期不是一回事。若改成纯虚，每一个后端（含测试里的假后端）都要为
  // 「我根本没有会话概念」这件事写一遍同样的空表，而漏掉一个就会编译不过；
  // 默认实现让「没有会话表」成为显式默认，需要真数据的后端（Process）覆盖它。
  // 默认实现**不假装成功**：返回空表的同时在 err 里写清「本后端没有会话表」，
  // 调用方据此把 devices 标成「取不到」而不是「一台设备都没有」。
  virtual nlohmann::json sessions_json(std::string& err) const;
};

// 空会话表的统一形状。三个「没有会话表」的后端共用它：形状不一致的话，
// 前端每个字段都要写三份兜底，而漏掉的那份会显示成「0 台设备在线」。
nlohmann::json empty_session_table();

// 没有会话表的后端的统一出口：空表 + err 里写清是哪个后端、为什么不支持。
// backend 用后端的短名（"http" / "recording" / "replay"），err 会被原样带到
// /api/state 的 devices_error 与工具输出里，所以文案要能直接给人看。
nlohmann::json no_session_table(const char* backend, std::string& err);

// 默认实现（定义在 service_pipeline.cpp）：后端不支持主动查询时返回 false 并写原因。
inline bool IServicePipeline::refresh_metrics(std::string& err) {
  err = "该后端不支持主动查询指标（用 latest() 的 1 Hz 缓存）";
  return false;
}

// 按端点类型造实现：Process → ProcessPipelineControl（子进程 + stdout 解析），
// Http → HttpServicePipeline（REST 控制协议，见 pipeline/http_pipeline.h）。
// 失败时返回 nullptr 并写 err（不静默返回空实现）。
std::unique_ptr<IServicePipeline> make_pipeline(const PipelineEndpoint& ep, std::string& err);

}  // namespace cue::pipeline
