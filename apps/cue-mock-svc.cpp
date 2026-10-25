// cue-mock-svc — M7 假域服务（AC-16）
//
// 为什么需要它：AC-16 要证明「harness/工具/评测不改一行代码」也能驱动一个后端完成任务。
// 真 cued 需要 FFmpeg、摄像头/推流和大量内存；把它换成只实现同一套 REST 控制协议的
// 假域服务，评测链路（make_pipeline → build_real_tools → cue-eval）一行都不用改。
//
// 协议与 apps/cue-serve.cpp 对齐（同一个 HttpServicePipeline 两者都能驱动）：
//   GET  /api/health    {"ok":true,...}
//   GET  /api/state     全量快照：alive/pid/metrics/events（每次采样推进 1 秒）
//   POST /api/action    {"action":"start"|"stop"|"restart"|"metrics"|"set_param"|"inject"|"observe"}
//                       或 {"name":"pipeline_start"|"query_metrics"|...,"args":{...}}
//
// 模型是**确定性**的（不是随机仿真，评测才能据此判分）：
//   - 每个 /api/state（或 query_metrics）采样推进 1 秒 = 30 帧；
//   - 六段链路各自独立丢帧：completion = (1 - drop_pct/100)^6
//     （与 M6 实测 5% 丢包、FEC/NACK 下帧完成率 ~73.5% 对齐）；
//   - fps = 30 × completion × gop 因子 × delay 因子；dropped 按未完成帧累计；
//   - injected 直接反映当前注入值（真 cued 的 injected 是累计注入计数，假域取当前档位，
//     两者在 agent 眼里都是"链路被注入了多少"）。
//
// 线程模型：每连接一个线程（同步 Beast），连接之间不共享可变状态之外的东西；
// 状态用一把 mutex 保护，因此评测与浏览器可以同时打。
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cue/common/metrics.h"

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using json = nlohmann::json;
namespace pipeline = cue::pipeline;

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

int64_t wall_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// ---------------------------------------------------------------- 媒体模型

constexpr uint64_t kFramesPerTick = 30;  // 1 秒 30 帧口径
constexpr std::size_t kMaxEvents = 200;  // /api/state 返回最近 200 条
constexpr std::size_t kMaxLogLines = 200;

struct MockEvent {
  std::string type;
  json data = json::object();
  int64_t t_us = 0;
};

class MockPipeline {
 public:
  explicit MockPipeline(uint64_t seed) : seed_(seed) {
    log_locked("[mock-svc] started seed=" + std::to_string(seed_));
    emit_locked("pipeline.started", json{{"running", true}, {"pid", kPid}});
  }

  json health() const {
    std::lock_guard<std::mutex> lk(mu_);
    return json{{"ok", true},
                {"service", "cue-mock-svc"},
                {"version", "m7"},
                {"seed", seed_},
                {"alive", running_}};
  }

  json state() {
    std::lock_guard<std::mutex> lk(mu_);
    sample_locked();
    json events = json::array();
    for (const auto& e : events_) {
      events.push_back({{"type", e.type}, {"t_us", e.t_us}, {"data", e.data}});
    }
    return json{{"ok", true},
                {"service", "cue-mock-svc"},
                {"alive", running_},
                {"pid", kPid},
                {"running", running_},
                {"seed", seed_},
                {"metrics", metrics_locked()},
                {"pipeline",
                 {{"running", running_},
                  {"config",
                   {{"bitrate_kbps", bitrate_kbps_},
                    {"gop", gop_},
                    {"transcode", transcode_},
                    {"inject_drop", drop_pct_},
                    {"inject_delay_ms", delay_ms_}}}}},
                {"events", std::move(events)}};
  }

  // 控制动作：两种写法（cue-serve 的 action / 工具名 name+args）都认
  json action(const json& body, int& status_code) {
    status_code = 200;
    if (!body.is_object()) {
      status_code = 400;
      return reply(false, "failed", "", "", json::object(), "请求体必须是 JSON 对象");
    }
    std::string tool;
    json args = json::object();
    if (body.contains("name") && body["name"].is_string()) {
      tool = body["name"].get<std::string>();
      if (body.contains("args")) {
        if (!body["args"].is_object()) {
          status_code = 400;
          return reply(false, "failed", tool, "", json::object(), "args 必须是对象");
        }
        args = body["args"];
      }
    } else if (body.contains("action") && body["action"].is_string()) {
      std::string err;
      if (!map_action(body, tool, args, err)) {
        status_code = 400;
        return reply(false, "failed", "", "", json::object(), err);
      }
    } else {
      status_code = 400;
      return reply(false, "failed", "", "", json::object(), "缺少 name 或 action");
    }
    std::lock_guard<std::mutex> lk(mu_);
    return dispatch_locked(tool, args);
  }

 private:
  static constexpr int kPid = -1;  // 不是本机子进程：真 cued 的 pid>0，假域永远是 -1

  json reply(bool ok, const char* status, const std::string& tool, const std::string& text,
             const json& data, const std::string& error) const {
    // text 与 content 同时给：HttpServicePipeline 读 text，cue-serve 的工具回灌叫 content
    return json{{"ok", ok},     {"status", status}, {"tool", tool},
                {"text", text}, {"content", text},  {"data", data},
                {"error", error}};
  }

  // body 里的 {action, key, value, kind, source, tail} → 工具名 + args
  static bool map_action(const json& body, std::string& tool, json& args, std::string& err) {
    const std::string a = body["action"].get<std::string>();
    if (a == "start") {
      tool = "pipeline_start";
    } else if (a == "stop") {
      tool = "pipeline_stop";
    } else if (a == "restart") {
      tool = "pipeline_restart";
    } else if (a == "metrics") {
      tool = "query_metrics";
      args = body.contains("window_ms") ? json{{"window_ms", body["window_ms"]}} : json::object();
    } else if (a == "set_param") {
      tool = "pipeline_set_param";
      std::string key;
      if (body.contains("key") && body["key"].is_string()) key = body["key"].get<std::string>();
      std::string value;
      if (body.contains("value")) {
        const json& v = body["value"];
        if (v.is_string()) value = v.get<std::string>();
        else if (v.is_number_integer()) value = std::to_string(v.get<int64_t>());
        else if (v.is_number()) value = v.dump();
        else if (v.is_boolean()) value = v.get<bool>() ? "true" : "false";
      }
      if (key.empty() || value.empty()) {
        err = "set_param 需要非空的 key 与 value";
        return false;
      }
      args = {{"key", key}, {"value", value}};
    } else if (a == "inject") {
      tool = "inject_fault";
      std::string kind = "drop";
      if (body.contains("kind") && body["kind"].is_string()) kind = body["kind"].get<std::string>();
      double value = 0;
      if (body.contains("value")) {
        const json& v = body["value"];
        if (v.is_number()) {
          value = v.get<double>();
        } else if (v.is_string()) {
          char* end = nullptr;
          value = std::strtod(v.get<std::string>().c_str(), &end);
          if (end == nullptr || *end != '\0') {
            err = "inject 的 value 不是数字";
            return false;
          }
        } else {
          err = "inject 的 value 必须是数字或数字字符串";
          return false;
        }
      }
      args = {{"kind", kind}, {"value", value}};
    } else if (a == "observe") {
      tool = "fetch_observation";
      const std::string source =
          body.contains("source") && body["source"].is_string() ? body["source"].get<std::string>() : "procs";
      args = {{"source", source}};
      if (body.contains("tail")) args["tail"] = body["tail"];
    } else {
      err = "未知 action（可用：start/stop/restart/metrics/set_param/inject/observe）";
      return false;
    }
    return true;
  }

  json dispatch_locked(const std::string& tool, const json& args) {
    if (tool == "pipeline_start") {
      if (running_) {
        return reply(true, "executed", tool, "管线已在运行", json{{"running", true}}, "");
      }
      // 从"已停止"重新启动 = 服务按默认配置重新拉起（真 cued 由 harness 的启动参数决定，
      // 假域这里用默认值；随后 harness/agent 再通过 set_param/inject 施加本 case 的档位）。
      running_ = true;
      reset_all_locked();
      emit_locked("pipeline.started", json{{"running", true}, {"pid", kPid}, {"restart", false}});
      log_locked("[mock-svc] pipeline started");
      return reply(true, "executed", tool, "管线已启动", json{{"running", true}}, "");
    }
    if (tool == "pipeline_restart") {
      // restart = 同配置重新 spawn（真 cued 的 restart 不清 cfg）：计数从 0，注入档位保留
      running_ = true;
      reset_counters_locked();
      emit_locked("pipeline.started", json{{"running", true}, {"pid", kPid}, {"restart", true}});
      log_locked("[mock-svc] pipeline restarted");
      return reply(true, "executed", tool, "管线已重启（计数已清零）", json{{"running", true}}, "");
    }
    if (tool == "pipeline_stop") {
      if (running_) {
        running_ = false;
        emit_locked("pipeline.stopped", json{{"running", false}, {"pid", kPid}});
        log_locked("[mock-svc] pipeline stopped");
      }
      return reply(true, "executed", tool, "管线已停止", json{{"running", false}}, "");
    }
    if (tool == "query_metrics") {
      sample_locked();
      const json m = metrics_locked();
      return reply(true, "executed", tool, "metrics=" + m.dump(), m, "");
    }
    if (tool == "pipeline_set_param") {
      std::string key, value;
      if (args.contains("key") && args["key"].is_string()) key = args["key"].get<std::string>();
      if (args.contains("value")) {
        if (args["value"].is_string()) value = args["value"].get<std::string>();
        else if (args["value"].is_number_integer()) value = std::to_string(args["value"].get<int64_t>());
        else value = args["value"].dump();
      }
      if (key.empty() || value.empty()) {
        return reply(false, "failed", tool, "", json::object(), "set_param 需要非空的 key 与 value");
      }
      std::string err;
      if (!apply_param_locked(key, value, err)) {
        return reply(false, "failed", tool, "", json::object(), err);
      }
      emit_locked("param.changed", json{{"key", key}, {"value", value}, {"pid", kPid}});
      log_locked("[mock-svc] param " + key + "=" + value);
      // 不在这里采样：采样只发生在"读指标"（/api/state 与 query_metrics）时，
      // 否则写动作会把假域的时钟拨快，case 里的 dropped 预算会被动作次数污染。
      return reply(true, "executed", tool, "已设置 " + key + "=" + value, json::object(), "");
    }
    if (tool == "inject_fault") {
      const std::string kind =
          args.contains("kind") && args["kind"].is_string() ? args["kind"].get<std::string>() : "drop";
      double value = 0;
      if (args.contains("value") && args["value"].is_number()) value = args["value"].get<double>();
      std::string err;
      if (!apply_inject_locked(kind, value, err)) {
        return reply(false, "failed", tool, "", json::object(), err);
      }
      return reply(true, "executed", tool, "已注入 " + kind + "=" + num_text(value), json::object(), "");
    }
    if (tool == "fetch_observation") {
      if (!args.contains("source") || !args["source"].is_string()) {
        return reply(false, "failed", tool, "", json::object(), "observe 需要 source");
      }
      const std::string source = args["source"].get<std::string>();
      int tail = 20;
      if (args.contains("tail") && args["tail"].is_number_integer()) tail = args["tail"].get<int>();
      std::string err;
      const std::string text = observation_locked(source, tail, err);
      if (!err.empty()) return reply(false, "failed", tool, "", json::object(), err);
      return reply(true, "executed", tool, text, json::object(), "");
    }
    return reply(false, "failed", tool, "", json::object(), "未知工具：" + tool);
  }

  static std::string num_text(double v) {
    if (std::fabs(v - std::round(v)) < 1e-9) return std::to_string(static_cast<long long>(std::llround(v)));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    return buf;
  }

  bool apply_param_locked(const std::string& key, const std::string& value, std::string& err) {
    auto parse_int = [&value](int lo, int hi, int& out) {
      char* end = nullptr;
      errno = 0;
      const long v = std::strtol(value.c_str(), &end, 10);
      if (errno != 0 || end == nullptr || *end != '\0' || v < lo || v > hi) return false;
      out = static_cast<int>(v);
      return true;
    };
    if (key == "bitrate_kbps") {
      int v = 0;
      if (!parse_int(100, 20000, v)) {
        err = "bitrate_kbps 取值非法（100-20000）：" + value;
        return false;
      }
      bitrate_kbps_ = v;
      return true;
    }
    if (key == "gop") {
      int v = 0;
      if (!parse_int(1, 300, v)) {
        err = "gop 取值非法（1-300）：" + value;
        return false;
      }
      gop_ = v;
      return true;
    }
    if (key == "transcode") {
      if (value == "true" || value == "1" || value == "on") {
        transcode_ = true;
        return true;
      }
      if (value == "false" || value == "0" || value == "off") {
        transcode_ = false;
        return true;
      }
      err = "transcode 只接受 true/false/1/0/on/off：" + value;
      return false;
    }
    // 与真 cued 的 set_param 白名单对齐：假域也只支持它真的建模的参数
    err = "未知参数：" + key + "（可用：bitrate_kbps/gop/transcode）";
    return false;
  }

  bool apply_inject_locked(const std::string& kind, double value, std::string& err) {
    if (kind == "drop") {
      if (!(value >= 0.0 && value <= 100.0)) {
        err = "drop 注入值必须在 0-100（百分比）";
        return false;
      }
      drop_pct_ = value;
    } else if (kind == "delay") {
      if (!(value >= 0.0 && value <= 2000.0)) {
        err = "delay 注入值必须在 0-2000（毫秒）";
        return false;
      }
      delay_ms_ = value;
    } else {
      err = "未知注入类型：" + kind + "（可用：drop/delay）";
      return false;
    }
    if (value > 0.0) {
      emit_locked("fault.applied", json{{"kind", kind}, {"value", value}, {"pid", kPid}});
      log_locked("[mock-svc] fault applied " + kind + "=" + num_text(value));
    } else {
      emit_locked("fault.cleared", json{{"kind", kind}, {"value", 0.0}, {"pid", kPid}});
      log_locked("[mock-svc] fault cleared " + kind);
    }
    return true;
  }

  // ---- 确定性模型 ----

  double completion_locked() const { return std::pow(1.0 - drop_pct_ / 100.0, 6.0); }
  // gop 越小 fps 略降（1 个 I 帧 / 更多 I 帧 → 编码开销），限制在 ±10% 以内
  double gop_factor_locked() const {
    const double f = 1.0 - 0.02 * (60.0 / static_cast<double>(gop_) - 1.0);
    return std::clamp(f, 0.90, 1.05);
  }
  // 注入时延只影响 send 段与 fps 的小幅下降，不产生额外丢帧
  double delay_factor_locked() const { return 1.0 / (1.0 + delay_ms_ / 1000.0); }
  double compute_fps_locked() const {
    return 30.0 * completion_locked() * gop_factor_locked() * delay_factor_locked();
  }

  void reset_counters_locked() {
    frames_in_ = 0;
    frames_out_ = 0;
    dropped_ = 0;
    samples_ = 0;
    last_fps_ = 0.0;
  }

  // 全新启动 = 默认配置：不清掉的话，上一个使用者的注入/参数会污染下一条"新管线"
  // （实测：curl 先注入 5% 再跑评测，会让第一个 case 的 dropped 多算一个档位）。
  void reset_all_locked() {
    reset_counters_locked();
    drop_pct_ = 0.0;
    delay_ms_ = 0.0;
    bitrate_kbps_ = 1500;
    gop_ = 60;
    transcode_ = false;
  }

  void sample_locked() {
    if (!running_) return;
    ++samples_;
    last_fps_ = compute_fps_locked();
    frames_in_ += kFramesPerTick;
    const uint64_t completed = static_cast<uint64_t>(
        std::llround(static_cast<double>(kFramesPerTick) * completion_locked()));
    frames_out_ += completed;
    dropped_ += kFramesPerTick - completed;
  }

  // 字段名与 pipeline::metrics_json（tools.h）一一对应：harness 的口径只有一份
  json metrics_locked() const {
    const bool have = samples_ > 0;
    auto pct = [have](double v) { return have && pipeline::has_sample(v) ? json(v) : json(nullptr); };
    const double decode_p95 = have ? 2.8 : pipeline::kNoSample;
    const double decode_p99 = have ? 3.2 : pipeline::kNoSample;
    const double encode_p95 = have ? 3.9 : pipeline::kNoSample;
    const double encode_p99 = have ? 4.1 : pipeline::kNoSample;
    const double op_p95 = have ? 0.03 : pipeline::kNoSample;
    const double op_p99 = have ? 0.05 : pipeline::kNoSample;
    const double send_p95 = have ? 0.09 + delay_ms_ : pipeline::kNoSample;
    const double send_p99 = have ? 0.11 + delay_ms_ : pipeline::kNoSample;
    return json{
        {"fps", last_fps_},
        {"frames_in", frames_in_},
        {"frames_out", frames_out_},
        {"dropped", dropped_},
        {"injected", static_cast<uint64_t>(std::llround(drop_pct_))},
        {"decoded", frames_out_},
        {"decode_errors", uint64_t{0}},
        // 六段分位与真 cued 同形：每段 p50/p95/p99；没有样本时 null（不是 0）。
        {"recv_p50_ms", pct(0.01)},
        {"recv_p95_ms", pct(0.02)},
        {"recv_p99_ms", pct(0.03)},
        {"buffer_p50_ms", pct(0.01)},
        {"buffer_p95_ms", pct(0.02)},
        {"buffer_p99_ms", pct(0.03)},
        {"decode_p50_ms", pct(1.2)},
        {"decode_p95_ms", pct(decode_p95)},
        {"decode_p99_ms", pct(decode_p99)},
        {"op_p50_ms", pct(0.0)},
        {"op_p95_ms", pct(op_p95)},
        {"op_p99_ms", pct(op_p99)},
        {"encode_p50_ms", pct(3.4)},
        {"encode_p95_ms", pct(encode_p95)},
        {"encode_p99_ms", pct(encode_p99)},
        {"send_p50_ms", pct(0.08 + delay_ms_)},
        {"send_p95_ms", pct(send_p95)},
        {"send_p99_ms", pct(send_p99)},
        {"pid", kPid},
        {"alive", running_},
        {"rejected", uint64_t{0}},
        {"send_errors", uint64_t{0}},
        {"encode_errors", uint64_t{0}},
        {"decode_drops", uint64_t{0}},
        {"budget_drops", uint64_t{0}},
        {"samples", samples_},
        {"age_ms", have ? 0 : -1}};
  }

  // 观测文本：字段名与真 cued 的 observation 对齐（tools 层的 evidence 校验只看 key=value）
  std::string observation_locked(const std::string& source, int tail, std::string& err) const {
    const std::size_t want = static_cast<std::size_t>(std::max(1, tail <= 0 ? 20 : tail));
    if (source == "metrics") {
      char buf[512];
      std::snprintf(buf, sizeof(buf),
                    "fps=%.2f frames_in=%llu frames_out=%llu dropped=%llu injected=%llu "
                    "decoded=%llu decode_errors=0 recv_p50_ms=%.3f buffer_p50_ms=%.3f "
                    "decode_p50_ms=%.3f decode_p99_ms=%.3f encode_p50_ms=%.3f send_p50_ms=%.3f",
                    last_fps_, static_cast<unsigned long long>(frames_in_),
                    static_cast<unsigned long long>(frames_out_),
                    static_cast<unsigned long long>(dropped_),
                    static_cast<unsigned long long>(std::llround(drop_pct_)),
                    static_cast<unsigned long long>(frames_out_), 0.01, 0.01, 1.2, 3.2, 3.4,
                    0.08 + delay_ms_);
      return buf;
    }
    if (source == "frames") {
      char buf[256];
      std::snprintf(buf, sizeof(buf),
                    "frames_in=%llu frames_out=%llu dropped=%llu injected=%llu decoded=%llu "
                    "decode_errors=0",
                    static_cast<unsigned long long>(frames_in_),
                    static_cast<unsigned long long>(frames_out_),
                    static_cast<unsigned long long>(dropped_),
                    static_cast<unsigned long long>(std::llround(drop_pct_)),
                    static_cast<unsigned long long>(frames_out_));
      return buf;
    }
    if (source == "procs") {
      // 字段名与 ProcessPipelineControl::observation(procs) 对齐：pid/running/restarts/
      // transcode/bitrate/gop。pid=-1 表示"不是本机子进程"（假域没有真实 pid）。
      return std::string("mock-svc pid=") + std::to_string(kPid) + " running=" +
             (running_ ? "1" : "0") + " restarts=0 transcode=" + (transcode_ ? "on" : "off") +
             " bitrate=" + std::to_string(bitrate_kbps_) + "kbps gop=" + std::to_string(gop_);
    }
    if (source == "events") {
      std::string out;
      const std::size_t n = std::min(want, events_.size());
      for (std::size_t i = events_.size() - n; i < events_.size(); ++i) {
        const auto& e = events_[i];
        out += "ev=" + e.type + " t_us=" + std::to_string(e.t_us) + " data=" + e.data.dump() + '\n';
      }
      return out;
    }
    if (source == "logs") {
      std::string out;
      const std::size_t n = std::min(want, logs_.size());
      for (std::size_t i = logs_.size() - n; i < logs_.size(); ++i) out += logs_[i] + '\n';
      return out;
    }
    err = "unsupported source: " + source + "（可用：metrics/frames/logs/procs/events）";
    return {};
  }

  // ---- 事件/日志 ----

  void emit_locked(std::string type, json data) {
    int64_t t = wall_us();
    // 同一毫秒内多条事件也要有唯一 t_us：HttpServicePipeline 按 t_us 去重
    if (t <= last_event_t_) t = last_event_t_ + 1;
    last_event_t_ = t;
    events_.push_back(MockEvent{std::move(type), std::move(data), t});
    while (events_.size() > kMaxEvents) events_.pop_front();
  }

  void log_locked(std::string line) {
    logs_.push_back(std::move(line));
    while (logs_.size() > kMaxLogLines) logs_.pop_front();
  }

  uint64_t seed_ = 42;
  mutable std::mutex mu_;
  bool running_ = true;  // 服务起来即"管线在跑"；stop 后 alive=false（与真 cued 被 kill 一致）
  double drop_pct_ = 0.0;
  double delay_ms_ = 0.0;
  int bitrate_kbps_ = 1500;
  int gop_ = 60;
  bool transcode_ = false;
  uint64_t frames_in_ = 0;
  uint64_t frames_out_ = 0;
  uint64_t dropped_ = 0;
  uint64_t samples_ = 0;
  double last_fps_ = 0.0;  // 最近一次采样值：管线停掉后 latest() 仍返回"最后一次读数"（alive=false）
  int64_t last_event_t_ = 0;
  std::deque<MockEvent> events_;
  std::deque<std::string> logs_;
};

// ---------------------------------------------------------------- HTTP 服务

http::response<http::string_body> make_res(http::status st, std::string body,
                                           const http::request<http::string_body>& req,
                                           const char* ctype = "application/json; charset=utf-8") {
  http::response<http::string_body> res{st, req.version()};
  res.set(http::field::server, "cue-mock-svc");
  res.set(http::field::content_type, ctype);
  res.set(http::field::cache_control, "no-store");
  res.body() = std::move(body);
  res.keep_alive(req.keep_alive());
  res.prepare_payload();
  return res;
}

http::response<http::string_body> json_res(const json& j, const http::request<http::string_body>& req,
                                          http::status st = http::status::ok) {
  return make_res(st, j.dump(), req);
}

class Server {
 public:
  Server(MockPipeline& pipe, std::string bind_addr, int port)
      : pipe_(pipe), bind_addr_(std::move(bind_addr)), port_(port), ioc_(), acceptor_(ioc_) {}

  bool listen(std::string& err) {
    beast::error_code ec;
    const auto addr = net::ip::make_address(bind_addr_, ec);
    if (ec) {
      err = "监听地址非法：" + bind_addr_ + "（" + ec.message() + "）";
      return false;
    }
    const tcp::endpoint ep(addr, static_cast<unsigned short>(port_));
    acceptor_.open(ep.protocol(), ec);
    if (ec) {
      err = "open 失败：" + ec.message();
      return false;
    }
    acceptor_.set_option(net::socket_base::reuse_address(true), ec);
    acceptor_.bind(ep, ec);
    if (ec) {
      err = "绑定 " + bind_addr_ + ":" + std::to_string(port_) + " 失败：" + ec.message();
      return false;
    }
    acceptor_.listen(net::socket_base::max_listen_connections, ec);
    if (ec) {
      err = "listen 失败：" + ec.message();
      return false;
    }
    return true;
  }

  void run() {
    while (!g_stop.load()) {
      pollfd p{};
      p.fd = acceptor_.native_handle();
      p.events = POLLIN;
      const int n = ::poll(&p, 1, 200);
      if (n < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (n == 0) continue;
      tcp::socket sock(ioc_);
      beast::error_code ec;
      acceptor_.accept(sock, ec);
      if (ec) continue;
      auto sp = std::make_shared<tcp::socket>(std::move(sock));
      // 唤醒描述符：stop() 只 shutdown 自己 dup 的那一份，不跨线程碰工作线程的 fd
      // （与 cue-serve 的 M6 TSan 修复同一做法）
      const int wake_fd = ::dup(sp->native_handle());
      {
        std::lock_guard<std::mutex> lk(mu_);
        purge_finished_locked();
        auto done = std::make_shared<std::atomic<bool>>(false);
        threads_.push_back(ThreadRec{
            std::jthread([this, sp, done] {
              serve_conn(sp);
              done->store(true);
            }),
            done, wake_fd});
      }
    }
  }

  void stop() {
    std::vector<ThreadRec> threads;
    {
      std::lock_guard<std::mutex> lk(mu_);
      threads.swap(threads_);
    }
    beast::error_code ec;
    acceptor_.close(ec);
    for (auto& t : threads) {
      if (t.done->load() || t.wake_fd < 0) continue;
      ::shutdown(t.wake_fd, SHUT_RDWR);
    }
    for (auto& t : threads) {
      if (t.th.joinable()) t.th.join();
      if (t.wake_fd >= 0) ::close(t.wake_fd);
      t.wake_fd = -1;
    }
  }

 private:
  struct ThreadRec {
    std::jthread th;
    std::shared_ptr<std::atomic<bool>> done;
    int wake_fd = -1;
  };

  void purge_finished_locked() {
    for (auto it = threads_.begin(); it != threads_.end();) {
      if (it->done->load()) {
        if (it->th.joinable()) it->th.join();
        if (it->wake_fd >= 0) ::close(it->wake_fd);
        it = threads_.erase(it);
      } else {
        ++it;
      }
    }
  }

  void serve_conn(const std::shared_ptr<tcp::socket>& sp) {
    beast::flat_buffer buffer;
    for (;;) {
      http::request<http::string_body> req;
      beast::error_code ec;
      http::read(*sp, buffer, req, ec);
      if (ec) break;
      http::response<http::string_body> res;
      try {
        res = route(req);
      } catch (const std::exception& e) {
        // 请求体是外部输入：线程里抛异常 = std::terminate，这里必须兜住
        res = json_res({{"ok", false}, {"error", std::string("内部错误：") + e.what()}}, req,
                       http::status::internal_server_error);
      }
      const bool keep = res.keep_alive();
      http::write(*sp, res, ec);
      if (ec || !keep) break;
    }
    beast::error_code ec;
    sp->shutdown(tcp::socket::shutdown_both, ec);
    sp->close(ec);
  }

  http::response<http::string_body> route(const http::request<http::string_body>& req) {
    const std::string target(req.target());
    const bool is_get = req.method() == http::verb::get;
    const bool is_post = req.method() == http::verb::post;
    if (target == "/api/health") {
      if (!is_get) {
        return json_res({{"ok", false}, {"error", "method not allowed"}}, req,
                        http::status::method_not_allowed);
      }
      return json_res(pipe_.health(), req);
    }
    if (target == "/api/state") {
      if (!is_get) {
        return json_res({{"ok", false}, {"error", "method not allowed"}}, req,
                        http::status::method_not_allowed);
      }
      return json_res(pipe_.state(), req);
    }
    if (target == "/api/action") {
      if (!is_post) {
        return json_res({{"ok", false}, {"error", "method not allowed"}}, req,
                        http::status::method_not_allowed);
      }
      json body = json::parse(req.body(), nullptr, false);
      if (body.is_discarded()) {
        return json_res({{"ok", false}, {"error", "请求体不是合法 JSON"}}, req,
                        http::status::bad_request);
      }
      int code = 200;
      const json out = pipe_.action(body, code);
      return json_res(out, req, static_cast<http::status>(code));
    }
    return json_res({{"ok", false}, {"error", "not found"}}, req, http::status::not_found);
  }

  MockPipeline& pipe_;
  std::string bind_addr_;
  int port_;
  net::io_context ioc_;
  tcp::acceptor acceptor_;
  std::mutex mu_;
  std::vector<ThreadRec> threads_;
};

// ---------------------------------------------------------------- 参数

struct Options {
  std::string bind = "127.0.0.1";
  int port = 18080;
  uint64_t seed = 42;
  bool help = false;
};

void print_usage() {
  std::printf(
      "usage: cue-mock-svc [options]\n"
      "  --bind ADDR   监听地址（默认 127.0.0.1，仅回环）\n"
      "  --port N      监听端口（默认 18080）\n"
      "  --seed N      模型种子（默认 42；当前模型是确定性的，seed 仅回显）\n"
      "  --help        打印本帮助\n"
      "\n"
      "退出：Ctrl-C / SIGTERM。\n");
}

bool take_value(int argc, char** argv, int& i, const std::string& opt, std::string& out) {
  if (i + 1 >= argc) {
    std::fprintf(stderr, "参数 %s 缺少取值\n", opt.c_str());
    return false;
  }
  out = argv[++i];
  return true;
}

bool parse_int(const std::string& s, int lo, int hi, int& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  errno = 0;
  const long v = std::strtol(s.c_str(), &end, 10);
  if (errno != 0 || end == nullptr || *end != '\0' || v < lo || v > hi) return false;
  out = static_cast<int>(v);
  return true;
}

bool parse_args(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    std::string v;
    if (a == "--help" || a == "-h") {
      o.help = true;
      return true;
    } else if (a == "--bind") {
      if (!take_value(argc, argv, i, a, o.bind)) return false;
    } else if (a == "--port") {
      if (!take_value(argc, argv, i, a, v)) return false;
      if (!parse_int(v, 1, 65535, o.port)) {
        std::fprintf(stderr, "--port 取值非法：%s\n", v.c_str());
        return false;
      }
    } else if (a == "--seed") {
      if (!take_value(argc, argv, i, a, v)) return false;
      if (v.empty()) return false;
      char* end = nullptr;
      errno = 0;
      const unsigned long long s = std::strtoull(v.c_str(), &end, 10);
      if (errno != 0 || end == nullptr || *end != '\0') {
        std::fprintf(stderr, "--seed 取值非法：%s\n", v.c_str());
        return false;
      }
      o.seed = static_cast<uint64_t>(s);
    } else {
      std::fprintf(stderr, "未知参数：%s\n", a.c_str());
      print_usage();
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  Options opt;
  if (!parse_args(argc, argv, opt)) return 2;
  if (opt.help) {
    print_usage();
    return 0;
  }

  MockPipeline pipe(opt.seed);
  Server server(pipe, opt.bind, opt.port);
  std::string err;
  if (!server.listen(err)) {
    std::fprintf(stderr, "[cue-mock-svc] %s\n", err.c_str());
    return 1;
  }
  std::printf("[cue-mock-svc] listening on http://%s:%d/api/state (seed=%llu)\n", opt.bind.c_str(),
              opt.port, static_cast<unsigned long long>(opt.seed));
  std::fflush(stdout);

  server.run();
  server.stop();
  std::printf("[cue-mock-svc] 退出\n");
  std::fflush(stdout);
  return 0;
}
