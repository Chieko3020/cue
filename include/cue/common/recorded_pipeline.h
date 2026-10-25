// 录制/回放装饰器：把「工具看到的管线观测」也录下来，让整条评测链路可确定性重放。
//
// 为什么需要它（AC-6）：cassette 只录 LLM 一侧，cassette key 是整段 messages 的哈希。
// 真管线下工具观测每轮都不同（fps 抖动、累计计数递增），第二轮 messages 就变了 →
// cassette miss。要证明「同一份观测必然得到同一判定」，观测本身也必须可回放。
//
// 为什么按 op 分序列、而不是按全局调用序：不同 run 之间 latest() 与 observation() 的
// 交错顺序可能不同（工具循环的分支不同），但同一 op 的语义序列必须一致。全局调用序
// 回放会把「顺序不同」误判成「观测不同」，而按 op 分序列只要求每个 op 依次消费。
//
// 回放绝不触碰真实后端：录制条数用尽、参数与录制不符都返回明确错误并计 miss，
// 不会返回默认值或伪造数据——否则「回放失败」会被伪装成「管线正常」。
//
// 文件格式（<dir>/<case_id>.tools.jsonl，每行一条）：
//   {"v":1,"op":"latest","ret":{...}}
//   {"v":1,"op":"probe_latest","ret":{...}}   # 端到端（探针）快照，NaN 同样写 null
//   {"v":1,"op":"set_param","args":{"key":"bitrate_kbps","value":"800"},"ret":{"ok":true}}
//   {"v":1,"op":"inject_fault","args":{"kind":"drop","value":5},"ret":{"ok":true}}
//   {"v":1,"op":"observation","args":{"source":"procs","tail":20},"ret":{"text":"..."}}
//   {"v":1,"op":"recent_events","args":{"n":0},"ret":{"events":[...]}}
//   {"v":1,"op":"start","ret":{"ok":true}}
//   {"v":1,"op":"stop","ret":{}}
//   {"v":1,"op":"running","ret":{"value":true}}
//
// NaN 口径：PipelineMetrics / ProbeMetrics 用 NaN 表示「没采到样本」（kNoSample）。
// JSON 里写 null，回读时还原成 NaN，绝不变成 0——「没采到」被写成「花了 0 ms」
// 是最危险的一类假绿。
#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "cue/common/service_pipeline.h"

namespace cue::pipeline {

// 录制文件路径：<dir>/<净化后的 case_id>.tools.jsonl。
// case id 可能含 '/'（suite 里按层级命名），必须净化成文件名安全的形式；
// 同时也把其它路径分隔符/空白换成 '-'，避免录制文件跑到目录外面去。
std::string tools_record_path(const std::string& dir, const std::string& case_id);

// 回放统计：hit = 成功消费一条录制；miss = 录制用尽或参数不符（两者都必须报错，
// 且评测报告要把 miss 数打出来——静默 miss 等于「回放时悄悄换了答案」）。
struct ReplayStats {
  std::size_t records = 0;
  std::size_t hits = 0;
  std::size_t misses = 0;
};

// 录制装饰器：包住任意 IServicePipeline，按调用序把每次调用落盘。
// 内层失败也照录（ok=false + err），回放时如实复现失败，而不是把失败「洗成」成功。
class RecordingPipeline : public IServicePipeline {
 public:
  // 构造即建目录并截断录制文件：同一个 case 的多次重复（--repeat）只保留最后一次，
  // 回放文件与「一份观测」一一对应。
  RecordingPipeline(std::unique_ptr<IServicePipeline> inner, std::string dir, std::string case_id);
  ~RecordingPipeline() override;

  RecordingPipeline(const RecordingPipeline&) = delete;
  RecordingPipeline& operator=(const RecordingPipeline&) = delete;

  bool start(std::string& err) override;
  void stop() override;
  bool running() const override;
  PipelineMetrics latest() const override;
  // 主动查询必须透传到 inner_：默认实现（"该后端不支持主动查询指标"）会把
  // query_metrics 的「先 refresh 再 latest」变成只读 1 Hz 缓存，而**只有 UDS 的
  // metrics.query 响应带 params**（编码档位真值）→ 录制模式下观测面 encode_* 恒 null，
  // 所有编码/分辨率类 case 的证据层恒 ✗。录制模式本来是"能产出证据"的那条路，
  // 这个洞让证据面也瞎了（N-02b 实测：runs/n05-p95 的 avc 证据就是 ✗）。
  // 不写进录制格式：refresh 本身不是一条工具观测（工具输出是紧接着的 latest()），
  // 那份 latest 的录制值已经包含 refresh 之后的快照；回放重放一次 refresh 只是噪声。
  bool refresh_metrics(std::string& err) override;
  // 端到端指标也进录制：否则回放时 probe_latest() 恒为空，AC-6 的
  // 「同一份观测必然得到同一判定」在端到端 SLA 上就不成立。
  ProbeMetrics probe_latest() const override;
  void set_event_callback(std::function<void(const PipelineEvent&)> cb) override;
  std::vector<PipelineEvent> recent_events(std::size_t n = 0) const override;
  bool set_param(const std::string& key, const std::string& value, std::string& err) override;
  bool inject_fault(const std::string& kind, double value, std::string& err) override;
  std::string observation(const std::string& source, int tail, std::string& err) const override;
  // 会话表不进录制格式：设备注册/租约是**实时**事实，录制里存一份"当时的设备"会让
  // 回放把过期设备当成在线设备显示。所以录制与回放都如实报"没有会话表"。
  nlohmann::json sessions_json(std::string& err) const override;

  // 录制文件打不开/写失败时非空。调用方必须把它当 run_error，不能「录没录上都算过」。
  const std::string& io_error() const { return io_error_; }
  const std::string& path() const { return path_; }

 private:
  void write_line(const nlohmann::json& line) const;  // 调用方已持 write_mu_

  std::unique_ptr<IServicePipeline> inner_;
  std::string path_;
  mutable std::mutex write_mu_;
  mutable std::string io_error_;
  mutable bool opened_ = false;
  mutable std::ofstream out_;  // 懒打开：构造失败也要能通过 io_error() 报出来
};

// 回放装饰器：只读录制文件，绝不触碰真实后端，也不 spawn 任何进程。
class ReplayPipeline : public IServicePipeline {
 public:
  explicit ReplayPipeline(std::string path);
  ~ReplayPipeline() override = default;

  ReplayPipeline(const ReplayPipeline&) = delete;
  ReplayPipeline& operator=(const ReplayPipeline&) = delete;

  // 加载失败（文件缺失、坏行、未知 op、版本不符）时非空；此时任何调用都返回失败，
  // 不允许「加载失败 → 空实现 → 全部返回默认值」这种静默降级。
  const std::string& load_error() const { return load_error_; }
  const std::string& path() const { return path_; }
  ReplayStats stats() const;

  // 最近一次 miss/参数不符的说明（latest() 没有 err 出口，读它定位问题）
  std::string last_error() const;

  bool start(std::string& err) override;
  void stop() override;
  bool running() const override;
  PipelineMetrics latest() const override;
  ProbeMetrics probe_latest() const override;
  void set_event_callback(std::function<void(const PipelineEvent&)> cb) override;
  std::vector<PipelineEvent> recent_events(std::size_t n = 0) const override;
  bool set_param(const std::string& key, const std::string& value, std::string& err) override;
  bool inject_fault(const std::string& kind, double value, std::string& err) override;
  std::string observation(const std::string& source, int tail, std::string& err) const override;
  // 回放绝不触碰真实后端：会话表也不转问被包住的后端（那会让"回放"里冒出真实设备，
  // 而同一份录制在两台机器上会得到不同的设备列表——回放就不可复现了）。
  nlohmann::json sessions_json(std::string& err) const override;

 private:
  struct Record {
    nlohmann::json args = nlohmann::json::object();
    nlohmann::json ret = nlohmann::json::object();
  };
  struct EventsRecord {
    int n = 0;
    std::vector<PipelineEvent> events;
  };

  // 取该 op 的下一条录制：要么返回 true，要么写 err + 记 miss。
  bool take(const std::string& op, const nlohmann::json& args, bool check_args, Record& out,
            std::string& err) const;
  void fail(const std::string& why) const;

  std::string path_;
  std::string load_error_;
  std::vector<Record> start_, set_param_, inject_fault_, observation_, running_;
  std::vector<PipelineMetrics> latest_;
  std::vector<ProbeMetrics> probe_latest_;
  std::vector<EventsRecord> recent_events_;
  std::vector<PipelineEvent> events_for_callback_;
  mutable std::mutex mu_;
  mutable std::size_t i_start_ = 0, i_set_param_ = 0, i_inject_ = 0, i_obs_ = 0,
                       i_events_ = 0, i_running_ = 0, i_latest_ = 0, i_probe_ = 0;
  mutable ReplayStats stats_;
  mutable std::string error_;
  std::function<void(const PipelineEvent&)> cb_;
};

}  // namespace cue::pipeline
