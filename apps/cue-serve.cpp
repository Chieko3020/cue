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
//   GET  /api/state      全量快照：管线配置 + 指标 + 设备会话表 + 最近事件 + 待审批 + 工具表
//   GET  /api/runs       评测产物（runs/<id>/report.md）索引
//   GET  /api/sessions   agent 会话索引（runs/*/sessions/*.jsonl）
//   GET  /api/session    单个会话的逐类分组记录（?id=<sid> 或 ?id=<run>/<sid>）
//   GET  /api/diff       两个 run 的 case 级回归对比（?a=<run>&b=<run>，语义同 cue-eval --diff）
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
#include <array>
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
#include <utility>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "console/approval_queue.h"
#include "console/metrics_report.h"
#include "console/report_index.h"
#include "console/session_index.h"
#include "console/sse_hub.h"
#include "cue/eval/compare.h"
#include "cue/eval/state.h"
#include "cue/cli/serve_options.h"
#include "cue/common/clock.h"
#include "cue/common/log.h"
#include "cue/pipeline/process_control.h"
#include "cue/tool/tools.h"
#include "cue/tool/tool.h"

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using json = nlohmann::json;
namespace tool = cue::tool;
namespace pipeline = cue::pipeline;
namespace cow = cue::console;
namespace eval = cue::eval;

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

// ---------------------------------------------------------------- 参数
//
// 选项与优先级（命令行 > 配置文件 > 内置默认值）在 cue/cli/serve_options.h 里，
// 这样"配置文件写 8080、命令行写 9999 → 9999"这条规则能被单测直接驱动，
// 不必起一个真进程去验。
using Options = cue::cli::ServeOptions;

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
    // 控制台也是交互入口：run_shell 一样要落审计（AC-11），关停信号 g_stop 兼作
    // 取消指针——否则一个正在跑的诊断命令会把 shutdown 拖到工具超时。
    // lifecycle 三个工具也由 build_real_tools 注册（同一个 Registry、同一份 schema），
    // 控制台与 agent 看到的是同样 8 个工具。
    pipeline::build_real_tools(
        reg_, *pipe_, nullptr,
        pipeline::RealToolsOptions{.audit_log_path = "runs/audit.jsonl", .cancel = &g_stop});
  }

  int port() const { return opt_.port; }
  const std::string& bind_addr() const { return opt_.bind; }
  // 推流页（web/index.html + web/app.js）是**原生 JS、不参与 Vite 构建**，
  // 所以它永远不在 web/dist 里。而 web_dir() 在 dist 存在时只指向 dist ——
  // 结果就是"构建过控制台的人再也打不开推流页"，干净 clone 与构建后行为相反。
  // 这里给它一个独立的根，并允许 --stream-dir 覆盖。
  std::string stream_dir() const {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!opt_.stream_dir.empty()) return opt_.stream_dir;
    // 默认取 web_dir 的父目录：web/dist → web；web → web（干净 clone 时两者同根）
    const fs::path w(web_dir());
    if (w.filename() == "dist") return w.parent_path().string();
    return w.string();
  }

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
                     {"sse_last_id", hub_.last_id()},
                     {"generation", hub_.generation()}};
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
    // 设备表（FR-UI.1）：数据源是 cued 的 SessionRegistry（经控制通道 session.list），
    // 这里只透传 sessions 数组 + 一份汇总。取不到时 devices 是空数组**并且**带
    // devices_error——前端必须能区分「一台设备都没接入」与「设备表取不到」，
    // 否则控制通道一降级，设备页会显示成"设备全掉线了"。
    {
      std::string dev_err;
      const json dev = pipe_->sessions_json(dev_err);
      out["devices"] = dev.value("sessions", json::array());
      out["devices_summary"] = json{{"count", dev.value("count", 0)},
                                    {"live", dev.value("live", 0)},
                                    {"max_sessions", dev.value("max_sessions", 0)},
                                    {"lease_ms", dev.value("lease_ms", 0)},
                                    {"control_channel", dev.value("control_channel", "")}};
      if (!dev_err.empty()) out["devices_error"] = dev_err;
    }
    out["approvals"] = {{"mode", opt_.approval},
                        {"timeout_s", opt_.approval_timeout_s},
                        {"pending", pending_json()},
                        {"recent", recent_approvals_json()}};
    out["events"] = hub_.recent(120);
    out["actions"] = recent_actions_json(20);
    out["tools"] = tools_json();
    return out;
  }

  // GET /metrics：文档 §12.3 的 10 项 + FR-OBS.3 的进程级读数（RSS/CPU/队列深度/连接数）。
  // 生成逻辑在 console/metrics_report.h（纯函数，单测直接断言 JSON 结构）；
  // 这里只负责把各路数据源凑齐：管线快照、probe 事件、工具计数、/proc 采样。
  json metrics(std::size_t connections) {
    cow::MetricsInput in;
    in.proc = "cue-serve";
    in.ts_us = cow::wall_us();
    in.pipe = pipe_->latest();
    in.probe = pipe_->probe_latest();
    // nack_sent / fec_recovered 只在 probe.metric 事件里：取最近一条（缓存按时间序）。
    const auto events = pipe_->recent_events(0);
    for (auto it = events.rbegin(); it != events.rend(); ++it) {
      if (it->type == "probe.metric" && it->data.is_object()) {
        in.probe_event = it->data;
        break;
      }
    }
    {
      std::lock_guard<std::mutex> lk(stats_mu_);
      for (const auto& [key, n] : tool_calls_) {
        in.tool_calls.push_back(cow::ToolCallCount{key[0], key[1], key[2], n});
      }
    }
    in.eval_cases = cow::latest_eval_cases(opt_.runs_dir);
    // 三个进程的 pid 都从管线接口拿：cued 的 pid 在 PipelineMetrics 里，probe 的在
    // ProbeMetrics 里，cue-serve 是自己。拿不到（进程没起）就如实写 null + 0。
    in.procs.push_back(sampler_.sample("cued", in.pipe.pid));
    in.procs.push_back(sampler_.sample("cue-probe", in.probe.pid));
    in.procs.push_back(sampler_.sample("cue-serve", ::getpid()));
    in.log_queue_depth = cue::log::queue_depth();
    in.log_dropped_debug = cue::log::dropped_debug();
    in.connections = connections;
    in.sse_clients = hub_.subscriber_count();
    return cow::build_metrics_json(in);
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
                 {"content", gr.outcome->content},
                 {"data", gr.outcome->data}};
        break;
      case tool::GuardedCall::Status::ApprovalDenied:
        reply = {{"ok", false},
                 {"status", "denied"},
                 {"tool", tool_name},
                 {"risk", tool::risk_name(gr.risk)},
                 {"error_kind", tool::tool_error_kind_name(gr.outcome.error().kind)},
                 {"error", gr.outcome.error().message}};
        break;
      case tool::GuardedCall::Status::Failed:
        reply = {{"ok", false},
                 {"status", "failed"},
                 {"tool", tool_name},
                 {"risk", tool::risk_name(gr.risk)},
                 {"error_kind", tool::tool_error_kind_name(gr.outcome.error().kind)},
                 {"error", gr.outcome.error().message},
                 {"data", gr.outcome.error().data}};
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
    record_action(tool_name, tool::risk_name(gr.risk), tool_args, status, ok,
                  reply.value("error", std::string()));
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

  // GET /api/sessions：会话索引。数据源是 agent 的 append-only jsonl（T-19），
  // 不是 SSE 环形缓冲——缓冲会被 800 条上限挤掉，会话文件才是事实。
  json sessions() const { return cow::scan_sessions(opt_.runs_dir, 50); }

  // GET /api/session?id=…：逐类分组 + seq（行号），前端据此拼回时间线并按阶段切段。
  // 状态码：400 = id 非法/缺失，404 = 没有这个会话或读不到。
  json session_view(const std::string& id, int& status_code) const {
    status_code = 200;
    auto path = cow::find_session_path(opt_.runs_dir, id);
    if (!path) {
      // id 非法与"不存在"对调用者是不同的错误，但都返回 ok:false（前端只认这个字段）
      status_code = cow::valid_session_id(id) ? 404 : 400;
      return json{{"ok", false}, {"error", path.error()}};
    }
    json out = cow::read_session_view(*path);
    out["id"] = id;
    return out;
  }

  // GET /api/diff?a=&b=：case 级回归对比。判定复用 eval::diff_runs —— 与
  // `cue-eval --diff` 是**同一份实现**，前端不再自己算分类，避免两套语义漂移
  // （例如 run_error 到底算不算"新增失败"这种口径，只能有一处）。
  json diff(const std::string& a, const std::string& b, int& status_code) const {
    status_code = 200;
    eval::RunInfo ia;
    eval::RunInfo ib;
    eval::StateMap sa;
    eval::StateMap sb;
    std::string err;
    if (!load_run_for_diff(a, ia, sa, err) || !load_run_for_diff(b, ib, sb, err)) {
      status_code = (cow::valid_session_id(a) && cow::valid_session_id(b)) ? 404 : 400;
      if (err.find("非法") != std::string::npos) status_code = 400;
      return json{{"ok", false}, {"error", err}};
    }
    const eval::DiffReport d = eval::diff_runs(ia, sa, ib, sb);
    json cases = json::array();
    for (const auto& c : d.cases) {
      json row{{"id", c.id},
               {"cls", diff_class_key(c.cls)},
               {"name", eval::diff_class_name(c.cls)},
               {"a", case_agg_json(c.a)},
               {"b", case_agg_json(c.b)}};
      cases.push_back(std::move(row));
    }
    json out{{"ok", true},
             {"a", run_info_json(ia)},
             {"b", run_info_json(ib)},
             {"comparable", d.comparable},
             {"warnings", d.warnings},
             {"counts",
              json{{"new_failures", d.new_failures},
                   {"fixed", d.fixed},
                   {"still_failing", d.still_failing},
                   {"still_passing", d.still_passing},
                   {"inconclusive", d.inconclusive},
                   {"only_a", d.only_a},
                   {"only_b", d.only_b}}},
             {"cases", std::move(cases)}};
    return out;
  }

 private:
  static const char* diff_class_key(eval::DiffClass c) {
    switch (c) {
      case eval::DiffClass::NewFailure: return "new_failure";
      case eval::DiffClass::Fixed: return "fixed";
      case eval::DiffClass::StillFailing: return "still_failing";
      case eval::DiffClass::StillPassing: return "still_passing";
      case eval::DiffClass::Inconclusive: return "inconclusive";
      case eval::DiffClass::OnlyInA: return "only_a";
      case eval::DiffClass::OnlyInB: return "only_b";
    }
    return "unknown";
  }

  static json case_agg_json(const eval::CaseAgg& g) {
    // 文本口径与 cue-eval 的 diff 报告一致（A "2/2 过"），只是这里的消费者是网页：
    // 结构化字段 + 同一条人读文本都给，前端不必再写一套格式化。
    std::string text;
    if (g.attempts == 0) {
      text = "—";
    } else {
      text = std::to_string(g.passes) + "/" + std::to_string(g.attempts) + " 过";
      if (g.run_errors > 0) text += "，run_error=" + std::to_string(g.run_errors);
      if (!g.root_cause.empty()) text += "，root=" + g.root_cause;
    }
    return json{{"attempts", g.attempts},
                {"passes", g.passes},
                {"run_errors", g.run_errors},
                {"pass", g.pass()},
                {"root_cause", g.root_cause},
                {"text", text}};
  }

  static json run_info_json(const eval::RunInfo& i) {
    return json{{"run_id", i.run_id},
                {"suite", i.suite},
                {"mode", i.mode},
                {"repeat", i.repeat},
                {"fingerprint", i.fingerprint}};
  }

  // run_id 会拼进路径，先过白名单；再从 state.jsonl 的 header 恢复 suite/mode/repeat/
  // fingerprint（与 cue-eval --diff 完全同源），否则"配置不同"这一条就判不出来。
  bool load_run_for_diff(const std::string& id, eval::RunInfo& info, eval::StateMap& state,
                         std::string& err) const {
    if (!cow::safe_session_component(id)) {
      err = "run_id 非法（只允许字母数字与 . _ -，且不能包含 ..）：" + id;
      return false;
    }
    const std::string sp = opt_.runs_dir + "/" + id + "/state.jsonl";
    auto hdr = eval::read_state_header(sp);
    if (!hdr) {
      err = "读取 " + id + " 的 state header 失败：" + hdr.error();
      return false;
    }
    auto st = eval::load_state(sp);
    if (!st) {
      err = "读取 " + id + " 的 state 失败：" + st.error();
      return false;
    }
    const auto hstr = [&hdr](const char* k) {
      return hdr->contains(k) && (*hdr)[k].is_string() ? (*hdr)[k].get<std::string>() : std::string();
    };
    info.run_id = id;
    info.fingerprint = hstr("fingerprint");
    info.suite = hstr("suite");
    info.mode = hstr("mode");
    info.repeat = hdr->contains("repeat") && (*hdr)["repeat"].is_number_integer()
                      ? (*hdr)["repeat"].get<int>()
                      : 1;
    state = std::move(*st);
    return true;
  }

  // body 里的 {action, key, value, kind} → 工具名 + args
  bool map_action(const json& body, std::string& name, json& args) {
    const std::string a = body["action"].get<std::string>();
    if (a == "start") {
      name = "pipeline_start";
      // profile 原样转交（工具层校验枚举）：控制台与 agent 走同一条生效路径
      if (body.contains("profile") && body["profile"].is_string()) {
        args = {{"profile", body["profile"]}};
      }
    } else if (a == "stop") {
      name = "pipeline_stop";
    } else if (a == "restart") {
      name = "pipeline_restart";
      if (body.contains("profile") && body["profile"].is_string()) {
        args = {{"profile", body["profile"]}};
      }
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

  // 累计计数（/metrics 的 cue_tool_calls_total{tool,risk,result}）+ 最近 64 条明细
  // （/api/state 的 actions）。两者都在一把锁下更新：计数与明细不能对不上。
  void record_action(const std::string& name, const std::string& risk, const json& args,
                     const std::string& status, bool ok, const std::string& error) {
    std::lock_guard<std::mutex> lk(stats_mu_);
    actions_.push_back({{"t_us", cow::wall_us()},
                        {"tool", name},
                        {"risk", risk},
                        {"args", args},
                        {"status", status},
                        {"ok", ok},
                        {"error", error}});
    while (actions_.size() > 64) actions_.pop_front();
    ++tool_calls_[std::array<std::string, 3>{name, risk.empty() ? "unknown" : risk, status}];
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
    // 请求在进 Registry 之前就被拒了：没有风险等级可言，统一记 "unknown"，
    // 不编一个看起来像真的 risk（指标是要被当证据用的）。
    record_action(tool_name, "unknown", args, status, false, error);
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
  std::map<std::array<std::string, 3>, uint64_t> tool_calls_;  // tool/risk/result → 累计次数
  cow::ProcSampler sampler_;                                  // /proc 采样（CPU 需要上一拍）
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



// GET 查询参数：控制台只传 id/run_id 这类白名单字符串，**不做百分号解码**是刻意的——
// 解码会让 "%2e%2e" 变成 ".."，校验与拼接之间就多出一步需要防守的转换；
// 不解码则任何编码字符都过不了白名单（非法字符直接 400）。
std::string query_param(const std::string& target, const std::string& key) {
  const auto q = target.find('?');
  if (q == std::string::npos) return "";
  const std::string qs = target.substr(q + 1);
  std::size_t pos = 0;
  while (pos <= qs.size()) {
    const auto amp = qs.find('&', pos);
    const std::string pair = qs.substr(pos, (amp == std::string::npos ? qs.size() : amp) - pos);
    const auto eq = pair.find('=');
    if (eq != std::string::npos && pair.compare(0, eq, key) == 0) return pair.substr(eq + 1);
    if (amp == std::string::npos) break;
    pos = amp + 1;
  }
  return "";
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
      // 解阻塞用的「唤醒描述符」：dup 一份专供 stop() 使用。
      // 直接对工作线程手里的 socket/fd 做 shutdown 会和它的 read/write 抢同一份对象状态；
      // 跨线程 close 更糟——fd 号会被内核回收，工作线程的写可能落到被复用的 fd 上
      // （M6 的 TSan 在这里报出 data race：file descriptor destroyed by another thread）。
      // dup 之后两个描述符指向同一个打开文件描述，stop() 只碰自己那份，永不 close 别人的。
      // 注意必须从 sp 上取句柄：上一行的 std::move(sock) 之后 sock 已处于被移动状态，
      // sock.native_handle() 返回 -1，dup 会稳定失败（EBADF）——那样每条连接都拿不到唤醒
      // 描述符，stop() 只能靠 10 s 超时兜底，等于把"优雅停止"退化成"等超时"。
      const int wake_fd = ::dup(sp->native_handle());
      if (wake_fd < 0) {
        std::fprintf(stderr, "[cue-serve] dup 连接描述符失败：%s（该连接退出时可能多等 10 s）\n",
                     std::strerror(errno));
      }
      {
        std::lock_guard<std::mutex> lk(mu_);
        purge_finished_locked();
        auto done = std::make_shared<std::atomic<bool>>(false);
        threads_.push_back(ThreadRec{
            std::jthread([this, sp, done] { serve_conn(sp); done->store(true); }), done, wake_fd});
      }
    }
  }

  // 用 dup 出来的唤醒描述符 shutdown（不动工作线程的 socket），再 join。
  // shutdown 会让阻塞中的 read/write 立刻返回错误，工作线程自己收尾并 close 自己的 fd。
  void stop() {
    std::vector<ThreadRec> threads;
    {
      std::lock_guard<std::mutex> mu_lk(mu_);
      threads.swap(threads_);
    }
    beast::error_code ec;
    acceptor_.close(ec);
    for (auto& t : threads) {
      if (t.done->load() || t.wake_fd < 0) continue;
      ::shutdown(t.wake_fd, SHUT_RDWR);  // 返回值无所谓：对端已走时是 ENOTCONN
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

  // 只在线程结束后 join/释放：socket 与 wake_fd 在连接活着的时候只有工作线程在用，
  // 主线程读 is_open() 之类的状态同样是一次未同步访问（旧的 live_ 清理就在这么干）。
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

  struct ConnGuard {
    std::atomic<std::size_t>& n;
    explicit ConnGuard(std::atomic<std::size_t>& c) : n(c) { n.fetch_add(1); }
    ~ConnGuard() { n.fetch_sub(1); }
  };

  void serve_conn(const std::shared_ptr<tcp::socket>& sp) {
    ConnGuard guard(connections_);
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

    // FR-OBS.3：/healthz 是给探活脚本/反代的纯文本，/api/health 是给前端的 JSON。
    // 两者并存是有意的：前端要结构，脚本只想要一个能 grep 的 "ok"。
    if (target == "/healthz") {
      if (!is_get) return json_res({{"ok", false}, {"error", "method not allowed"}}, req,
                                   http::status::method_not_allowed);
      return make_res(http::status::ok, "ok\n", req, "text/plain; charset=utf-8");
    }
    // FR-OBS.3 / 技术设计 §12.3：默认 JSON（前端直读），不做特殊放行——绑定策略
    // （默认 127.0.0.1）就是暴露边界，这条路由和别的路由遵守同一条规矩。
    if (target == "/metrics") {
      if (!is_get) return json_res({{"ok", false}, {"error", "method not allowed"}}, req,
                                   http::status::method_not_allowed);
      return json_res(app_.metrics(connections_.load()), req);
    }
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
    // 这三条带查询串，所以先取路径部分再比较（其余路由沿用原来的整串比较，不动）。
    // 方法校验放在取参数之前：POST 到已知路径应该是 405，而不是落进静态文件 404。
    {
      const std::string path = target.substr(0, target.find('?'));
      const bool known = path == "/api/sessions" || path == "/api/session" || path == "/api/diff";
      if (known && !is_get) {
        return json_res({{"ok", false}, {"error", "method not allowed"}}, req,
                        http::status::method_not_allowed);
      }
      if (path == "/api/sessions") return json_res(app_.sessions(), req);
      if (path == "/api/session") {
        const std::string id = query_param(target, "id");
        if (id.empty()) {
          return json_res({{"ok", false},
                           {"error", "缺少 id（?id=<session_id> 或 ?id=<run_id>/<session_id>）"}},
                          req, http::status::bad_request);
        }
        int code = 200;
        json out = app_.session_view(id, code);
        return json_res(out, req, static_cast<http::status>(code));
      }
      if (path == "/api/diff") {
        const std::string a = query_param(target, "a");
        const std::string b = query_param(target, "b");
        if (a.empty() || b.empty()) {
          return json_res({{"ok", false}, {"error", "缺少 a/b（?a=<run_id>&b=<run_id>）"}}, req,
                          http::status::bad_request);
        }
        int code = 200;
        json out = app_.diff(a, b, code);
        return json_res(out, req, static_cast<http::status>(code));
      }
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
    const std::string stream = app_.stream_dir();
    auto read_at = [](const std::string& root, const std::string& rel) -> std::optional<std::string> {
      std::error_code ec;
      const fs::path p = fs::path(root) / rel;
      if (!fs::is_regular_file(p, ec)) return std::nullopt;
      std::ifstream in(p, std::ios::binary);
      if (!in) return std::nullopt;
      std::stringstream buf;
      buf << in.rdbuf();
      return buf.str();
    };
    // 先控制台根，再推流页根：`/app.js` 这类资源不在 dist 里，但从 web/ 取得到。
    auto open_file = [&](const std::string& rel) -> std::optional<std::string> {
      if (auto body = read_at(web, rel)) return body;
      if (stream != web) return read_at(stream, rel);
      return std::nullopt;
    };

    // The stream page uses absolute same-origin asset URLs, so both entry paths
    // resolve the same resources without shadowing the console SPA.
    if (target == "/stream" || target == "/stream/") {
      if (auto body = read_at(stream, "index.html")) {
        return make_res(http::status::ok, std::move(*body), req, "text/html; charset=utf-8");
      }
    }

    std::string rel = target == "/" ? "index.html" : target.substr(1);
    if (!rel.empty() && rel.back() == '/') rel += "index.html";
    if (auto body = open_file(rel)) return make_res(http::status::ok, std::move(*body), req, mime_of(rel).c_str());
    // SPA 兜底：没有扩展名的路径交给 index.html（前端是五页单页切换）
    if (rel.find('.') == std::string::npos) {
      if (auto body = open_file("index.html")) {
        return make_res(http::status::ok, std::move(*body), req, "text/html; charset=utf-8");
      }
    }
    std::string msg = "404 " + target + " 不存在（控制台：" + web + "，推流页：" + stream +
                      "；控制台需要 cd web/console && npm run build 生成 web/dist）\n";
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
    std::string resume_cursor;
    const auto resume_header = req.find("Last-Event-ID");
    if (resume_header != req.end()) resume_cursor = std::string(resume_header->value());
    const std::string target(req.target());
    const std::string query_cursor = query_param(target, "last_event_id");
    if (!query_cursor.empty()) resume_cursor = query_cursor;
    const uint64_t last_id = app_.hub().resume_id(resume_cursor);

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

    // Announce the generation before replay, even if the ring is empty.
    json ready{{"ev", "console.ready"}, {"id", app_.hub().last_id()},
               {"generation", app_.hub().generation()}, {"t", cow::wall_us()},
               {"data", {{"replayed_from", last_id}, {"last_id", app_.hub().last_id()}}}};
    bool ok = write_chunk(sock, "retry: 3000\n\ndata: " + ready.dump() + "\n\n");
    if (ok) {
      for (const auto& frame : app_.hub().replay_after(last_id)) {
        if (!write_chunk(sock, frame)) {
          ok = false;
          break;
        }
      }
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
        for (const auto& f : batch) {
          const auto event = json::parse(f, nullptr, false);
          if (event.is_object() && event.contains("id")) {
            data += cow::sse_frame(app_.hub().cursor(event["id"].get<uint64_t>()), f);
          }
        }
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
  // 在线连接数（/metrics 的 cue_serve_connections）：工作线程进出 serve_conn 时加减，
  // 用原子量而不是去 threads_ 里数——threads_ 只在 accept/stop 时持锁，读它要抢同一把锁。
  std::atomic<std::size_t> connections_{0};
};

}  // namespace

int main(int argc, char** argv) {
  auto parsed = cue::cli::parse_serve_options(argc, argv);
  if (!parsed) {
    std::fprintf(stderr, "%s\n", parsed.error().text().c_str());
    return 2;
  }
  Options opt = std::move(*parsed);
  if (opt.help) {
    cue::cli::print_serve_usage();
    return 0;
  }
  opt.log.proc = "cue-serve";
  if (auto ok = cue::log::init(opt.log); !ok) {
    std::fprintf(stderr, "cue-serve: %s\n", ok.error().c_str());
    return 2;
  }
  cue::log::set_session_id(std::to_string(::getpid()));

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::signal(SIGPIPE, SIG_IGN);

  ConsoleApp app(opt);
  Server server(app, app.bind_addr(), app.port());
  std::string err;
  if (!server.listen(err)) {
    cue::log::error("serve", "listen_failed", err);
    cue::log::shutdown();
    return 1;
  }
  cue::log::info("serve", "startup",
                 "监听 http://" + opt.bind + ":" + std::to_string(opt.port) + "/（静态目录：" +
                     app.web_dir() + "，审批模式：" + opt.approval + "，审批超时 " +
                     std::to_string(opt.approval_timeout_s) + " s）");
  cue::log::info("serve", "routes",
                 "SSE: /events  API: /api/state /api/action /api/approve /api/runs "
                 "/api/sessions /api/session /api/diff  "
                 "运维: /healthz /metrics");
  // 一行 debug：`--log-level debug` 时能确认级别真的生效（否则"没看到 debug 行"会
  // 被误读成日志系统没起来）。
  cue::log::debug("serve", "options",
                  "config=" + (opt.config_path.empty() ? std::string("(无)") : opt.config_path) +
                      " log_file=" + (opt.log.file.empty() ? std::string("(无)") : opt.log.file) +
                      " log_level=" + opt.log.level + " queue=" +
                      std::to_string(opt.log.queue_size));

  if (opt.no_pipeline) {
    cue::log::info("serve", "pipeline_skipped", "--no-pipeline：不启动 cued（只读状态/评测）");
  } else if (!app.start_pipeline(err)) {
    // 控制台仍可用：这条是 warn 不是 error，因为进程的整体状态是"能用但不完整"。
    cue::log::warn("serve", "pipeline_start_failed",
                   "管线启动失败，控制台仍可用，可稍后点「启动」", err);
  } else {
    cue::log::info("serve", "pipeline_started",
                   "cued 已启动（ws=" + std::to_string(opt.ws_port) +
                       "，udp=" + std::to_string(opt.probe_port) + "，cued=" + opt.cued_bin + "）");
  }

  server.run();
  cue::log::info("serve", "shutdown", "收到退出信号，正在停止管线与连接…");
  app.shutdown();
  server.stop();
  cue::log::info("serve", "shutdown_done",
                 "已退出（日志丢弃 debug 行数=" + std::to_string(cue::log::dropped_debug()) + "）");
  cue::log::shutdown();
  return 0;
}
