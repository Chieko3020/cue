// cue-serve — M5 控制台后端（HTTP + SSE + 静态托管 + 审批）
//
// 为什么是独立可执行文件而不是 `cue serve` 子命令：
//  - 控制台要长期驻留（SSE 长连接 + 子进程 supervise），而 `cue run` 是一次性 CLI；
//    混在一个 main 里，信号语义（serve 要优雅退出）和退出码会互相污染。
//  - serve 需要 Beast::http + 线程池 + 审批队列，链接 cue_pipeline/cue_agent 的全部工具层，
//    单独 target 能让 `cue`（交互）的启动时间与二进制不受影响。
//  - 两条入口共用同一套东西：ProcessPipelineControl（事件源）、Registry（工具与审批）、
//    metrics_json（指标口径），所以不会出现「控制台能做的 agent 不能做」的分叉。
//
// 路由：
//   GET  /               静态文件（默认 web/dist，回退 web/）
//   GET  /events         SSE；支持 Last-Event-ID 补齐（SPEC §6 M5）
//   GET  /api/state      全量快照：管线配置 + 指标 + 最近事件 + 待审批 + 工具表
//   GET  /api/runs       评测产物（runs/<id>/report.md）索引
//   POST /api/action     控制动作：必须走 Registry::call_guarded（risk + 审批），不直接碰管线
//   POST /api/approve    {id, allow} 决定一条待审批
//   GET  /api/health
//
// 线程模型：每个连接一个线程（同步 Beast）。SSE 用条件变量等事件，HTTP 请求在线程内同步处理；
// 因此审批等待（最长 approval_timeout_s）只阻塞那一条请求，不会把 SSE 卡住。
// 这是「单机开发控制台」的取舍：不用 io_context 线程池是为了让审批的同步语义直接可读。
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>

#include "console/approval_queue.h"
#include "console/report_index.h"
#include "console/sse_hub.h"
#include "cue/common/clock.h"
#include "cue/pipeline/process_control.h"
#include "cue/pipeline/tools.h"
#include "cue/tool/tool.h"

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using json = nlohmann::json;
namespace tool = cue::tool;
namespace pipeline = cue::pipeline;
namespace cow = cue::console;

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

// ---------------------------------------------------------------- 参数

struct Options {
  std::string bind = "127.0.0.1";
  int port = 8080;
  std::string web_dir;  // 空 = 自动探测 web/dist → web
  std::string cued_bin = "./build/cued";
  std::string runs_dir = "./runs";
  std::string approval = "ask";  // ask | auto | deny
  int ws_port = 9000;
  int probe_port = 9001;
  int approval_timeout_s = 30;   // 超时自动拒绝（任务 A.2）
  bool no_pipeline = false;
  int bitrate_kbps = 1500;
  int gop = 60;
  std::string preset = "veryfast";
  bool transcode = false;
  double inject_drop = 0.0;
  bool help = false;
};

void print_usage() {
  std::printf(
      "usage: cue-serve [options]\n"
      "  --bind ADDR            监听地址（默认 127.0.0.1，仅回环）\n"
      "  --port N               监听端口（默认 8080）\n"
      "  --web DIR              静态目录（默认自动：web/dist 存在则用它，否则 web/）\n"
      "  --cued BIN             cued 可执行文件（默认 ./build/cued）\n"
      "  --runs DIR             评测产物目录（默认 ./runs）\n"
      "  --approval MODE        ask（默认，审批 UI 决定）/ auto（自动批准）/ deny（拒绝非只读）\n"
      "  --ws-port N            cued 的 WS 端口（默认 9000）\n"
      "  --probe-port N         cued 的 UDP 目标端口（默认 9001）\n"
      "  --approval-timeout S   审批超时秒数（默认 30，超时自动拒绝）\n"
      "  --no-pipeline          不启动 cued（只用控制台看状态/评测）\n"
      "  --bitrate-kbps N       初始码率（默认 1500）\n"
      "  --gop N                初始 GOP（默认 60）\n"
      "  --preset NAME          初始 x264 preset（默认 veryfast）\n"
      "  --transcode            初始打开转码\n"
      "  --inject-drop PCT      启动时注入丢包百分比（默认 0）\n"
      "  --help                 打印本帮助\n"
      "\n"
      "退出：Ctrl-C / SIGTERM。会先停 cued 再关闭监听（不留孤儿进程）。\n");
}

bool take_value(int argc, char** argv, int& i, const std::string& opt, std::string& out) {
  if (i + 1 >= argc) {
    std::fprintf(stderr, "参数 %s 缺少取值\n", opt.c_str());
    return false;
  }
  out = argv[++i];
  return true;
}

bool parse_int(const std::string& s, int& out, int lo, int hi) {
  if (s.empty()) return false;
  char* end = nullptr;
  errno = 0;
  const long v = std::strtol(s.c_str(), &end, 10);
  if (errno != 0 || end == nullptr || *end != '\0' || v < lo || v > hi) return false;
  out = static_cast<int>(v);
  return true;
}

bool parse_double(const std::string& s, double& out, double lo, double hi) {
  if (s.empty()) return false;
  char* end = nullptr;
  errno = 0;
  const double v = std::strtod(s.c_str(), &end);
  if (errno != 0 || end == nullptr || *end != '\0' || !(v >= lo) || !(v <= hi)) return false;
  out = v;
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
      if (!parse_int(v, o.port, 1, 65535)) {
        std::fprintf(stderr, "--port 取值非法：%s\n", v.c_str());
        return false;
      }
    } else if (a == "--web") {
      if (!take_value(argc, argv, i, a, o.web_dir)) return false;
    } else if (a == "--cued") {
      if (!take_value(argc, argv, i, a, o.cued_bin)) return false;
    } else if (a == "--runs") {
      if (!take_value(argc, argv, i, a, o.runs_dir)) return false;
    } else if (a == "--approval") {
      if (!take_value(argc, argv, i, a, o.approval)) return false;
      if (o.approval != "ask" && o.approval != "auto" && o.approval != "deny") {
        std::fprintf(stderr, "--approval 只能是 ask|auto|deny，收到：%s\n", o.approval.c_str());
        return false;
      }
    } else if (a == "--ws-port") {
      if (!take_value(argc, argv, i, a, v)) return false;
      if (!parse_int(v, o.ws_port, 1, 65535)) return false;
    } else if (a == "--probe-port") {
      if (!take_value(argc, argv, i, a, v)) return false;
      if (!parse_int(v, o.probe_port, 1, 65535)) return false;
    } else if (a == "--approval-timeout") {
      if (!take_value(argc, argv, i, a, v)) return false;
      if (!parse_int(v, o.approval_timeout_s, 1, 600)) {
        std::fprintf(stderr, "--approval-timeout 取值非法：%s\n", v.c_str());
        return false;
      }
    } else if (a == "--no-pipeline") {
      o.no_pipeline = true;
    } else if (a == "--bitrate-kbps") {
      if (!take_value(argc, argv, i, a, v)) return false;
      if (!parse_int(v, o.bitrate_kbps, 100, 20000)) return false;
    } else if (a == "--gop") {
      if (!take_value(argc, argv, i, a, v)) return false;
      if (!parse_int(v, o.gop, 1, 300)) return false;
    } else if (a == "--preset") {
      if (!take_value(argc, argv, i, a, o.preset)) return false;
    } else if (a == "--transcode") {
      o.transcode = true;
    } else if (a == "--inject-drop") {
      if (!take_value(argc, argv, i, a, v)) return false;
      if (!parse_double(v, o.inject_drop, 0.0, 100.0)) return false;
    } else {
      std::fprintf(stderr, "未知参数：%s\n", a.c_str());
      print_usage();
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------- 控制台状态

class ConsoleApp {
 public:
  explicit ConsoleApp(Options o) : opt_(std::move(o)), hub_(512) {
    pipeline::ProcessPipelineControl::Config pc;
    pc.cued_bin = opt_.cued_bin;
    pc.ws_port = opt_.ws_port;
    pc.probe_port = opt_.probe_port;
    pc.bitrate_kbps = opt_.bitrate_kbps;
    pc.gop = opt_.gop;
    pc.preset = opt_.preset;
    pc.transcode = opt_.transcode;
    pc.inject_drop = opt_.inject_drop;
    // 事件源：cued 的 [cue-event] 行（pipeline.* / fault.* / metric.sample / codec.config）
    // 原样转发到 SSE——控制台不发明事件类型，前端按 ev 过滤。
    pc.on_event = [this](const pipeline::PipelineEvent& ev) {
      json data = ev.data.is_object() ? ev.data : json::object();
      data["t_us"] = ev.t_us;
      data["seen_us"] = ev.seen_us;
      hub_.publish(ev.type, std::move(data));
    };
    pipe_ = std::make_unique<pipeline::ProcessPipelineControl>(pc);

    mode_ = opt_.approval == "deny"   ? tool::ApprovalMode::Deny
            : opt_.approval == "auto" ? tool::ApprovalMode::Auto
                                      : tool::ApprovalMode::Ask;

    // 工具层与 agent 完全同源（build_real_tools），控制台不再另开一条"后门"。
    pipeline::build_real_tools(reg_, *pipe_, nullptr);
    add_pipeline_lifecycle_tools();
  }

  int port() const { return opt_.port; }
  const std::string& bind_addr() const { return opt_.bind; }
  std::string web_dir() const {
    namespace fs = std::filesystem;
    if (!opt_.web_dir.empty()) return opt_.web_dir;
    std::error_code ec;
    if (fs::is_regular_file(fs::path("web/dist/index.html"), ec)) return "web/dist";
    return "web";
  }
  const std::string& runs_dir() const { return opt_.runs_dir; }
  cow::SseHub& hub() { return hub_; }

  bool start_pipeline(std::string& err) {
    if (opt_.no_pipeline) return true;
    return pipe_->start(err);
  }

  // 进程退出：先停管线（它还会推 stopped 事件），再关 hub / 审批队列
  void shutdown() {
    pipe_->stop();
    hub_.shutdown();
    approvals_.shutdown();
  }

  json state() {
    const auto cfg = pipe_->config();
    json out = json::object();
    out["ok"] = true;
    out["server"] = {{"bind", opt_.bind},
                     {"port", opt_.port},
                     {"web_dir", web_dir()},
                     {"runs_dir", opt_.runs_dir},
                     {"approval_mode", opt_.approval},
                     {"approval_timeout_s", opt_.approval_timeout_s},
                     {"sse_last_id", hub_.last_id()}};
    out["pipeline"] = {{"running", pipe_->running()},
                       {"config",
                        {{"cued_bin", cfg.cued_bin},
                         {"ws_port", cfg.ws_port},
                         {"probe_port", cfg.probe_port},
                         {"bitrate_kbps", cfg.bitrate_kbps},
                         {"gop", cfg.gop},
                         {"preset", cfg.preset},
                         {"transcode", cfg.transcode},
                         {"inject_drop", cfg.inject_drop},
                         {"fec", cfg.fec},
                         {"width", cfg.width},
                         {"height", cfg.height},
                         {"send_mbps", cfg.send_mbps}}}};
    out["metrics"] = pipeline::metrics_json(pipe_->latest(), cue::now_us());
    out["approvals"] = {{"mode", opt_.approval},
                        {"timeout_s", opt_.approval_timeout_s},
                        {"pending", pending_json()},
                        {"recent", recent_approvals_json()}};
    out["events"] = hub_.recent(120);
    out["actions"] = recent_actions_json(20);
    out["tools"] = tools_json();
    return out;
  }

  // POST /api/action：唯一入口。所有动作都经过 Registry::call_guarded，
  // Ask 模式下非只读工具会阻塞等待审批（超时自动拒绝）。
  json handle_action(const json& body) {
    if (!body.is_object()) return action_reply("failed", "", json::object(), "请求体必须是 JSON 对象");

    std::string name;
    json args = json::object();
    if (body.contains("name") && body["name"].is_string()) {
      name = body["name"].get<std::string>();
      if (body.contains("args")) args = body["args"];
      if (!args.is_object()) return action_reply("failed", name, args, "args 必须是对象");
    } else if (body.contains("action") && body["action"].is_string()) {
      if (!map_action(body, name, args)) {
        return action_reply("failed", "", json::object(),
                            "未知 action（可用：start/stop/restart/set_param/inject/metrics）");
      }
    } else {
      return action_reply("failed", "", json::object(), "缺少 name 或 action");
    }

    tool::CallContext ctx;
    ctx.mode = mode_;
    if (mode_ == tool::ApprovalMode::Auto) {
      // auto = 无人值守控制台：dangerous 也放行，但仍然逐条留痕（hub 事件 + actions）
      ctx.allowed_dangerous = {"inject_fault"};
    }
    ctx.approve = [this](const tool::ToolSpec& spec, tool::Risk risk, const json& a) {
      return ask_approval(spec, risk, a);
    };

    const std::string tool_name = name;
    const json tool_args = args;
    auto gr = reg_.call_guarded(tool_name, tool_args, ctx, [this, &tool_name, &tool_args](tool::Risk risk) {
      hub_.publish("tool.call", {{"tool", tool_name},
                                 {"risk", tool::risk_name(risk)},
                                 {"args", tool_args}});
    });

    json reply;
    switch (gr.status) {
      case tool::GuardedCall::Status::Executed:
        reply = {{"ok", true},
                 {"status", "executed"},
                 {"tool", tool_name},
                 {"risk", tool::risk_name(gr.risk)},
                 {"content", gr.result.content},
                 {"data", gr.result.data}};
        break;
      case tool::GuardedCall::Status::ApprovalDenied:
        reply = {{"ok", false},
                 {"status", "denied"},
                 {"tool", tool_name},
                 {"risk", tool::risk_name(gr.risk)},
                 {"error", gr.error}};
        break;
      case tool::GuardedCall::Status::Failed:
        reply = {{"ok", false},
                 {"status", "failed"},
                 {"tool", tool_name},
                 {"risk", tool::risk_name(gr.risk)},
                 {"error", gr.error}};
        break;
    }
    const std::string status = reply["status"].get<std::string>();
    const bool ok = reply["ok"].get<bool>();
    std::string content = reply.value("content", std::string());
    if (content.size() > 2000) content = content.substr(0, 2000) + "…(截断)";
    hub_.publish("tool.result", {{"tool", tool_name},
                                 {"status", status},
                                 {"ok", ok},
                                 {"risk", tool::risk_name(gr.risk)},
                                 {"error", reply.value("error", std::string())},
                                 {"content", content}});
    record_action(tool_name, tool_args, status, ok, reply.value("error", std::string()));
    return reply;
  }

  // POST /api/approve
  json handle_approve(const json& body, int& status_code) {
    status_code = 200;
    const bool has_int_id =
        body.is_object() && body.contains("id") &&
        (body["id"].is_number_unsigned() || body["id"].is_number_integer());
    if (!has_int_id) {
      status_code = 400;
      return {{"ok", false}, {"error", "请求体需要整数 id 与布尔 allow"}};
    }
    const uint64_t id = body["id"].get<uint64_t>();
    // allow 必须是布尔：用 value() 在类型不符时会抛 type_error，把整个请求线程带走
    const bool allow = body.contains("allow") && body["allow"].is_boolean() && body["allow"].get<bool>();
    if (!approvals_.decide(id, allow)) {
      status_code = 409;
      return {{"ok", false}, {"id", id}, {"allow", allow},
              {"error", "没有该待审批项（可能已超时或已被决定）"}};
    }
    return {{"ok", true}, {"id", id}, {"allow", allow}};
  }

  json runs() const { return cow::scan_runs(opt_.runs_dir, 20); }

 private:
  // pipeline_start / stop / restart：SPEC §7.4 的三个 mutating 工具。
  // 它们不放进 build_real_tools 是有意的：agent 在评测里没有"启停"预算语义，
  // 控制台的操作员才有。风险级与审批规则完全一致（走同一个 Registry）。
  void add_pipeline_lifecycle_tools() {
    auto profile_param = tool::ParamSpec{.name = "profile",
                                         .type = "string",
                                         .desc = "档位名（当前版本仅记录，不改变参数）",
                                         .required = false,
                                         .enum_values = {},
                                         .has_range = false,
                                         .min = 0,
                                         .max = 0};

    reg_.add(tool::ToolSpec{.name = "pipeline_start",
                            .desc = "启动媒体管线（cued 子进程）",
                            .risk = tool::Risk::Mutating,
                            .params = {profile_param}},
             [this](const json&) {
               std::string err;
               if (pipe_->running()) {
                 return tool::ToolResult{true, "管线已在运行", json{{"running", true}}};
               }
               if (!pipe_->start(err)) {
                 return tool::ToolResult{false, "启动失败: " + err, json::object()};
               }
               return tool::ToolResult{true, "管线已启动", json{{"running", true}}};
             });

    reg_.add(tool::ToolSpec{.name = "pipeline_stop",
                            .desc = "停止媒体管线（cued 子进程）",
                            .risk = tool::Risk::Mutating,
                            .params = {profile_param}},
             [this](const json&) {
               pipe_->stop();
               return tool::ToolResult{true, "管线已停止", json{{"running", false}}};
             });

    reg_.add(tool::ToolSpec{.name = "pipeline_restart",
                            .desc = "重启媒体管线（会造成短暂断流）",
                            .risk = tool::Risk::Mutating,
                            .params = {profile_param}},
             [this](const json&) {
               pipe_->stop();
               std::string err;
               if (!pipe_->start(err)) {
                 return tool::ToolResult{false, "重启失败: " + err, json{{"running", false}}};
               }
               return tool::ToolResult{true, "管线已重启", json{{"running", true}}};
             });
  }

  // body 里的 {action, key, value, kind} → 工具名 + args
  bool map_action(const json& body, std::string& name, json& args) {
    const std::string a = body["action"].get<std::string>();
    if (a == "start") {
      name = "pipeline_start";
    } else if (a == "stop") {
      name = "pipeline_stop";
    } else if (a == "restart") {
      name = "pipeline_restart";
    } else if (a == "metrics") {
      name = "query_metrics";
      if (body.contains("window_ms")) args = {{"window_ms", body["window_ms"]}};
      else args = json::object();
    } else if (a == "set_param") {
      name = "pipeline_set_param";
      std::string key;
      if (body.contains("key") && body["key"].is_string()) key = body["key"].get<std::string>();
      // 工具层 value 是 string（枚举/范围在工具内校验）；数字在入口处转字符串，
      // 不接受"数字原样塞进 string 参数"这种类型偷懒。
      std::string value;
      if (body.contains("value")) {
        const json& v = body["value"];
        if (v.is_string()) value = v.get<std::string>();
        else if (v.is_number_integer()) value = std::to_string(v.get<int64_t>());
        else if (v.is_number()) value = v.dump();
        else if (v.is_boolean()) value = v.get<bool>() ? "true" : "false";
      }
      if (key.empty() || value.empty()) return false;
      args = {{"key", key}, {"value", value}};
    } else if (a == "inject") {
      name = "inject_fault";
      std::string kind = "drop";
      if (body.contains("kind") && body["kind"].is_string()) kind = body["kind"].get<std::string>();
      double value = 0;
      if (body.contains("value")) {
        const json& v = body["value"];
        if (v.is_number()) value = v.get<double>();
        else if (v.is_string()) {
          char* end = nullptr;
          value = std::strtod(v.get<std::string>().c_str(), &end);
          if (end == nullptr || *end != '\0') return false;
        } else {
          return false;
        }
      }
      args = {{"kind", kind}, {"value", value}};
    } else if (a == "observe") {
      name = "fetch_observation";
      const std::string source =
          body.contains("source") && body["source"].is_string() ? body["source"].get<std::string>() : "procs";
      args = {{"source", source}};
      if (body.contains("tail")) args["tail"] = body["tail"];
    } else {
      return false;
    }
    return true;
  }

  // Ask 模式的人机审批：入队 → SSE approval.request → 等 /api/approve 或超时拒绝
  bool ask_approval(const tool::ToolSpec& spec, tool::Risk risk, const json& args) {
    cow::ApprovalQueue::Item item;
    item.tool = spec.name;
    item.risk = tool::risk_name(risk);
    item.args = args;
    const auto outcome = approvals_.request(
        item, std::chrono::seconds(opt_.approval_timeout_s),
        [this](const cow::ApprovalQueue::Item& it) {
          hub_.publish("approval.request",
                       {{"id", it.id},
                        {"tool", it.tool},
                        {"risk", it.risk},
                        {"args", it.args},
                        {"timeout_ms", it.timeout_ms},
                        {"created_us", it.created_us}});
        });
    const bool allowed = outcome.decision == cow::ApprovalQueue::Decision::Allowed;
    hub_.publish("approval.decision",
                 {{"id", outcome.item.id},
                  {"tool", outcome.item.tool},
                  {"risk", outcome.item.risk},
                  {"outcome", cow::ApprovalQueue::outcome_name(outcome.decision)},
                  {"allow", allowed}});
    return allowed;
  }

  json pending_json() const {
    json arr = json::array();
    for (const auto& it : approvals_.pending()) {
      arr.push_back({{"id", it.id},
                     {"tool", it.tool},
                     {"risk", it.risk},
                     {"args", it.args},
                     {"created_us", it.created_us},
                     {"timeout_ms", it.timeout_ms}});
    }
    return arr;
  }

  json recent_approvals_json() const {
    json arr = json::array();
    for (const auto& h : approvals_.recent(20)) {
      arr.push_back({{"id", h.item.id},
                     {"tool", h.item.tool},
                     {"risk", h.item.risk},
                     {"args", h.item.args},
                     {"outcome", h.outcome},
                     {"decided_us", h.decided_us}});
    }
    return arr;
  }

  void record_action(const std::string& name, const json& args, const std::string& status, bool ok,
                     const std::string& error) {
    std::lock_guard<std::mutex> lk(stats_mu_);
    actions_.push_back({{"t_us", cow::wall_us()},
                        {"tool", name},
                        {"args", args},
                        {"status", status},
                        {"ok", ok},
                        {"error", error}});
    while (actions_.size() > 64) actions_.pop_front();
    if (ok) ++ok_actions_;
  }

  json recent_actions_json(std::size_t n) const {
    std::lock_guard<std::mutex> lk(stats_mu_);
    json arr = json::array();
    const std::size_t start = n == 0 || n >= actions_.size() ? 0 : actions_.size() - n;
    for (std::size_t i = start; i < actions_.size(); ++i) arr.push_back(actions_[i]);
    return arr;
  }

  json tools_json() const {
    json arr = json::array();
    for (const auto& spec : reg_.specs()) {
      json params = json::array();
      for (const auto& p : spec.params) {
        json pj{{"name", p.name}, {"type", p.type}, {"desc", p.desc}, {"required", p.required}};
        if (!p.enum_values.empty()) pj["enum"] = p.enum_values;
        if (p.has_range) {
          pj["min"] = p.min;
          pj["max"] = p.max;
        }
        params.push_back(std::move(pj));
      }
      arr.push_back({{"name", spec.name},
                     {"desc", spec.desc},
                     {"risk", tool::risk_name(spec.risk)},
                     {"params", std::move(params)}});
    }
    return arr;
  }

  json action_reply(const std::string& status, const std::string& tool_name, const json& args,
                    const std::string& error) {
    record_action(tool_name, args, status, false, error);
    hub_.publish("tool.result", {{"tool", tool_name},
                                 {"status", status},
                                 {"ok", false},
                                 {"error", error}});
    return {{"ok", false}, {"status", status}, {"tool", tool_name}, {"error", error}};
  }

  Options opt_;
  std::unique_ptr<pipeline::ProcessPipelineControl> pipe_;
  tool::Registry reg_;
  tool::ApprovalMode mode_ = tool::ApprovalMode::Ask;
  cow::SseHub hub_;
  cow::ApprovalQueue approvals_;
  mutable std::mutex stats_mu_;
  std::deque<json> actions_;
  uint64_t ok_actions_ = 0;
};

// ---------------------------------------------------------------- HTTP/SSE 服务

std::string mime_of(const std::string& path) {
  const auto pos = path.rfind('.');
  const std::string ext = pos == std::string::npos ? "" : path.substr(pos);
  static const std::map<std::string, const char*> kMap = {
      {".html", "text/html; charset=utf-8"}, {".js", "text/javascript; charset=utf-8"},
      {".mjs", "text/javascript; charset=utf-8"}, {".css", "text/css; charset=utf-8"},
      {".json", "application/json; charset=utf-8"}, {".svg", "image/svg+xml"},
      {".png", "image/png"}, {".jpg", "image/jpeg"}, {".ico", "image/x-icon"},
      {".woff2", "font/woff2"}, {".map", "application/json; charset=utf-8"},
      {".txt", "text/plain; charset=utf-8"}, {".md", "text/markdown; charset=utf-8"}};
  const auto it = kMap.find(ext);
  return it == kMap.end() ? "application/octet-stream" : it->second;
}

http::response<http::string_body> make_res(http::status st, std::string body,
                                           const http::request<http::string_body>& req,
                                           const char* ctype = "application/json; charset=utf-8") {
  http::response<http::string_body> res{st, req.version()};
  res.set(http::field::server, "cue-serve");
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

// chunked 帧（SSE 用一个 chunk 装一次写出的多帧）
bool write_chunk(tcp::socket& sock, const std::string& data) {
  if (data.empty()) return true;
  char head[32];
  std::snprintf(head, sizeof(head), "%zx\r\n", data.size());
  std::string out(head);
  out += data;
  out += "\r\n";
  beast::error_code ec;
  net::write(sock, net::buffer(out), ec);
  return !ec;
}

bool parse_u64(const std::string& s, uint64_t& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  errno = 0;
  const unsigned long long v = std::strtoull(s.c_str(), &end, 10);
  if (errno != 0 || end == nullptr || *end != '\0') return false;
  out = static_cast<uint64_t>(v);
  return true;
}

class Server {
 public:
  Server(ConsoleApp& app, const std::string& bind_addr, int port)
      : app_(app), bind_addr_(bind_addr), port_(port), ioc_(), acceptor_(ioc_) {}

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
      {
        std::lock_guard<std::mutex> lk(mu_);
        purge_finished_locked();
        live_.push_back(sp);
        auto done = std::make_shared<std::atomic<bool>>(false);
        threads_.push_back(ThreadRec{
            std::jthread([this, sp, done] { serve_conn(sp); done->store(true); }), done});
      }
    }
  }

  // 关闭所有连接的 socket（解除阻塞读），再 join
  void stop() {
    std::vector<ThreadRec> threads;
    std::vector<std::shared_ptr<tcp::socket>> live;
    {
      std::lock_guard<std::mutex> mu_lk(mu_);
      threads.swap(threads_);
      live.swap(live_);
    }
    beast::error_code ec;
    acceptor_.close(ec);
    for (auto& s : live) {
      if (!s->is_open()) continue;
      s->shutdown(tcp::socket::shutdown_both, ec);
      s->close(ec);
    }
    for (auto& t : threads) {
      if (t.th.joinable()) t.th.join();
    }
  }

 private:
  struct ThreadRec {
    std::jthread th;
    std::shared_ptr<std::atomic<bool>> done;
  };

  void purge_finished_locked() {
    for (auto it = threads_.begin(); it != threads_.end();) {
      if (it->done->load()) {
        if (it->th.joinable()) it->th.join();
        it = threads_.erase(it);
      } else {
        ++it;
      }
    }
    // 已关闭的连接不再需要保留 socket 引用
    for (auto it = live_.begin(); it != live_.end();) {
      if (!(*it)->is_open()) it = live_.erase(it);
      else ++it;
    }
  }

  void serve_conn(const std::shared_ptr<tcp::socket>& sp) {
    beast::flat_buffer buffer;
    for (;;) {
      http::request<http::string_body> req;
      beast::error_code ec;
      http::read(*sp, buffer, req, ec);
      if (ec) break;

      const std::string target(req.target());
      const bool is_get = req.method() == http::verb::get;
      if (is_get && target.rfind("/events", 0) == 0) {
        serve_sse(*sp, req);
        break;  // SSE 是长连接，结束即断开
      }
      // 请求体是外部输入：任何一格类型不符都不允许把进程带走（线程里抛 = std::terminate）
      http::response<http::string_body> res;
      try {
        res = route(req);
      } catch (const std::exception& e) {
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
      if (!is_get) return json_res({{"ok", false}, {"error", "method not allowed"}}, req,
                                   http::status::method_not_allowed);
      return json_res({{"ok", true}, {"service", "cue-serve"}, {"version", "m5"}}, req);
    }
    if (target == "/api/state") {
      if (!is_get) return json_res({{"ok", false}, {"error", "method not allowed"}}, req,
                                   http::status::method_not_allowed);
      return json_res(app_.state(), req);
    }
    if (target == "/api/runs") {
      if (!is_get) return json_res({{"ok", false}, {"error", "method not allowed"}}, req,
                                   http::status::method_not_allowed);
      return json_res(app_.runs(), req);
    }
    if (target == "/api/action") {
      if (!is_post) return json_res({{"ok", false}, {"error", "method not allowed"}}, req,
                                    http::status::method_not_allowed);
      // 请求体可能很大（观测原文），但控制台只发小对象；限制 1 MiB
      if (req.body().size() > (1u << 20)) {
        return json_res({{"ok", false}, {"error", "请求体过大"}}, req, http::status::payload_too_large);
      }
      json body = json::parse(req.body(), nullptr, false);
      if (body.is_discarded()) {
        return json_res({{"ok", false}, {"error", "请求体不是合法 JSON"}}, req, http::status::bad_request);
      }
      return json_res(app_.handle_action(body), req);
    }
    if (target == "/api/approve") {
      if (!is_post) return json_res({{"ok", false}, {"error", "method not allowed"}}, req,
                                    http::status::method_not_allowed);
      json body = json::parse(req.body(), nullptr, false);
      if (body.is_discarded()) {
        return json_res({{"ok", false}, {"error", "请求体不是合法 JSON"}}, req, http::status::bad_request);
      }
      int code = 200;
      json out = app_.handle_approve(body, code);
      return json_res(out, req, static_cast<http::status>(code));
    }
    if (is_get) return serve_static(req);
    return json_res({{"ok", false}, {"error", "not found"}}, req, http::status::not_found);
  }

  http::response<http::string_body> serve_static(const http::request<http::string_body>& req) {
    namespace fs = std::filesystem;
    std::string target(req.target());
    const auto q = target.find('?');
    if (q != std::string::npos) target = target.substr(0, q);
    if (target.find("..") != std::string::npos) {
      return json_res({{"ok", false}, {"error", "bad path"}}, req, http::status::bad_request);
    }
    const std::string web = app_.web_dir();
    auto open_file = [&](const std::string& rel) -> std::optional<std::string> {
      std::error_code ec;
      const fs::path p = fs::path(web) / rel;
      if (!fs::is_regular_file(p, ec)) return std::nullopt;
      std::ifstream in(p, std::ios::binary);
      if (!in) return std::nullopt;
      std::stringstream buf;
      buf << in.rdbuf();
      return buf.str();
    };

    std::string rel = target == "/" ? "index.html" : target.substr(1);
    if (!rel.empty() && rel.back() == '/') rel += "index.html";
    if (auto body = open_file(rel)) return make_res(http::status::ok, std::move(*body), req, mime_of(rel).c_str());
    // SPA 兜底：没有扩展名的路径交给 index.html（前端是五页单页切换）
    if (rel.find('.') == std::string::npos) {
      if (auto body = open_file("index.html")) {
        return make_res(http::status::ok, std::move(*body), req, "text/html; charset=utf-8");
      }
    }
    std::string msg = "404 " + target + " 不存在（静态目录：" + web + "，执行 cd web/console && npm run build 生成 web/dist）\n";
    return make_res(http::status::not_found, msg, req, "text/plain; charset=utf-8");
  }

  // ------------------------------------------------ SSE
  struct Sink {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::string> q;
    bool closed = false;
    std::size_t dropped = 0;
  };

  void serve_sse(tcp::socket& sock, const http::request<http::string_body>& req) {
    // Last-Event-ID：浏览器 EventSource 重连时自动带；也支持 ?last_event_id=N 便于脚本
    uint64_t last_id = 0;
    for (const auto& f : req.base()) {
      if (beast::string_view(f.name_string()) == "Last-Event-ID") {
        uint64_t v = 0;
        if (parse_u64(std::string(f.value()), v)) last_id = v;
      }
    }
    const std::string target(req.target());
    if (const auto p = target.find("last_event_id="); p != std::string::npos) {
      uint64_t v = 0;
      if (parse_u64(target.substr(p + 15), v)) last_id = v;
    }

    // 自己写响应头：SSE 不结束响应，不能用 beast::http::write（它会补 Content-Length）
    const std::string head =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream; charset=utf-8\r\n"
        "Cache-Control: no-cache, no-transform\r\n"
        "Connection: keep-alive\r\n"
        "Transfer-Encoding: chunked\r\n"
        "X-Accel-Buffering: no\r\n"
        "\r\n";
    beast::error_code ec;
    net::write(sock, net::buffer(head), ec);
    if (ec) {
      app_.hub().publish("console.sse", {{"client", "gone"}, {"phase", "handshake"}});
      return;
    }

    auto sink = std::make_shared<Sink>();
    const uint64_t sid = app_.hub().subscribe([sink](const std::string& text) {
      std::lock_guard<std::mutex> lk(sink->mu);
      if (text.empty()) {
        sink->closed = true;
      } else {
        sink->q.push_back(text);
        // 慢消费者：丢最旧的而不是无界增长（控制台可接受丢帧，不能吃掉服务端内存）
        while (sink->q.size() > 4096) {
          sink->q.pop_front();
          ++sink->dropped;
        }
      }
      sink->cv.notify_all();
    });

    bool ok = write_chunk(sock, "retry: 3000\n\n");
    if (ok) {
      for (const auto& frame : app_.hub().replay_after(last_id)) {
        // 补齐：一个 chunk 装一帧，便于浏览器逐帧 id 记账
        if (!write_chunk(sock, frame)) {
          ok = false;
          break;
        }
      }
    }
    // 告诉前端"当前是补齐后的实时流"，前端据此不必等第一条真实事件
    if (ok) {
      json ready{{"ev", "console.ready"}, {"id", app_.hub().last_id()}, {"t", cow::wall_us()},
                 {"data", {{"replayed_from", last_id}, {"last_id", app_.hub().last_id()}}}};
      ok = write_chunk(sock, "data: " + ready.dump() + "\n\n");
    }

    while (ok) {
      std::deque<std::string> batch;
      bool closed = false;
      {
        std::unique_lock<std::mutex> lk(sink->mu);
        sink->cv.wait_for(lk, std::chrono::seconds(10),
                          [&] { return !sink->q.empty() || sink->closed; });
        batch.swap(sink->q);
        closed = sink->closed;
      }
      if (!batch.empty()) {
        std::string data;
        for (const auto& f : batch) data += f;
        ok = write_chunk(sock, data);
      }
      if (closed) break;
      if (batch.empty()) ok = write_chunk(sock, ": ping\n\n");
    }
    app_.hub().unsubscribe(sid);
    net::write(sock, net::buffer(std::string("0\r\n\r\n")), ec);
  }

  ConsoleApp& app_;
  std::string bind_addr_;
  int port_;
  net::io_context ioc_;
  tcp::acceptor acceptor_;
  std::mutex mu_;
  std::vector<ThreadRec> threads_;
  std::vector<std::shared_ptr<tcp::socket>> live_;
};

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parse_args(argc, argv, opt)) return 2;
  if (opt.help) {
    print_usage();
    return 0;
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::signal(SIGPIPE, SIG_IGN);

  ConsoleApp app(opt);
  Server server(app, app.bind_addr(), app.port());
  std::string err;
  if (!server.listen(err)) {
    std::fprintf(stderr, "[cue-serve] %s\n", err.c_str());
    return 1;
  }
  std::printf("[cue-serve] http://%s:%d/ （静态目录：%s，审批模式：%s，审批超时 %d s）\n",
              app.bind_addr().c_str(), app.port(), app.web_dir().c_str(), opt.approval.c_str(),
              opt.approval_timeout_s);
  std::printf("[cue-serve] SSE: /events  API: /api/state /api/action /api/approve /api/runs\n");

  if (opt.no_pipeline) {
    std::printf("[cue-serve] --no-pipeline：不启动 cued（只读状态/评测）\n");
  } else if (!app.start_pipeline(err)) {
    std::fprintf(stderr, "[cue-serve] 管线启动失败：%s（控制台仍可用，可稍后点\"启动\"）\n", err.c_str());
  } else {
    std::printf("[cue-serve] cued 已启动（ws=%d，udp=%d，cued=%s）\n", opt.ws_port, opt.probe_port,
                opt.cued_bin.c_str());
  }

  server.run();
  std::printf("\n[cue-serve] 收到退出信号，正在停止管线与连接…\n");
  app.shutdown();
  server.stop();
  std::printf("[cue-serve] 已退出\n");
  return 0;
}
