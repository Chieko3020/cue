#include "cue/pipeline/tools.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

#include "cue/common/clock.h"
#include "cue/sandbox/sandbox.h"

namespace cue::pipeline {

using json = nlohmann::json;

// data 只放"我看到了什么"（真实测量值），不放"我做了什么"（请求参数）。
// 否则 agent 可以引用自己的动作当观测证据（review-eval.md #26）。
// age_ms / samples 让 agent 能判断这条是不是 cued 重启前的旧指标（#27）。
json metrics_json(const PipelineMetrics& m, int64_t now) {
  const int64_t age_us = m.updated_us == 0 ? -1 : now - m.updated_us;
  // 分位没有样本时输出 null 而不是 0：agent 引用 0 当证据就是伪证
  // （transcode 关闭时 encode 段本来就没有样本，写 0 等于宣称"编码 0 ms"）。
  auto pct = [](double v) { return has_sample(v) ? json(v) : json(nullptr); };
  return json{{"fps", m.fps},
              {"frames_in", m.frames_in},
              {"frames_out", m.frames_out},
              {"dropped", m.dropped},
              {"injected", m.injected},
              {"decoded", m.decoded},
              {"decode_errors", m.decode_errors},
              {"recv_p50_ms", pct(m.recv_p50)},
              {"buffer_p50_ms", pct(m.buffer_p50)},
              {"decode_p50_ms", pct(m.decode_p50)},
              {"decode_p99_ms", pct(m.decode_p99)},
              {"op_p50_ms", pct(m.op_p50)},
              {"op_p99_ms", pct(m.op_p99)},
              {"encode_p50_ms", pct(m.encode_p50)},
              {"encode_p99_ms", pct(m.encode_p99)},
              {"send_p50_ms", pct(m.send_p50)},
              // 进程身份/存活：pid=null 表示当前没有 cued（不是 pid 0 的真进程）
              {"pid", m.pid == 0 ? json(nullptr) : json(static_cast<int64_t>(m.pid))},
              {"alive", m.alive},
              // 协议/发送/编码/预算侧计数：过去只在 cued 的 METRICS 行里，agent 看不到
              {"rejected", m.rejected},
              {"send_errors", m.send_errors},
              {"encode_errors", m.encode_errors},
              {"decode_drops", m.decode_drops},
              {"budget_drops", m.budget_drops},
              {"samples", m.samples},
              {"age_ms", age_us < 0 ? -1 : age_us / 1000}};
}

// 端到端读数（probe 侧）单独一块、键名前缀 e2e_：agent 引用 `e2e_lat_p99_ms=...`
// 时不会与 cued 的 `encode_p50_ms` 混淆。探针不在时只写 probe_alive=false 与原因，
// 各分位整段不出现——"没有端到端数据"必须比"端到端是 0 ms"更显眼。
void add_probe_metrics(json& j, const ProbeMetrics& p) {
  j["probe_alive"] = p.alive;
  j["probe_samples"] = p.samples;
  if (!p.unavailable_reason.empty()) j["probe_reason"] = p.unavailable_reason;
  if (p.samples == 0) return;
  auto pct = [](double v) { return has_sample(v) ? json(v) : json(nullptr); };
  j["e2e_fps"] = p.fps;
  j["e2e_frames"] = p.frames;
  j["e2e_decoded"] = p.decoded;
  j["e2e_decode_errors"] = p.decode_errors;
  j["e2e_stalls"] = p.stalls;
  j["e2e_max_stall_ms"] = p.max_stall_us / 1000.0;
  j["e2e_lat_p50_ms"] = pct(p.lat_p50_ms);
  j["e2e_lat_p95_ms"] = pct(p.lat_p95_ms);
  j["e2e_lat_p99_ms"] = pct(p.lat_p99_ms);
  j["e2e_recv_p50_us"] = pct(p.recv.p50);
  j["e2e_reassembly_p50_us"] = pct(p.reassembly.p50);
  j["e2e_decode_p50_us"] = pct(p.decode.p50);
  // 抖动缓冲段：cued 侧还没实现，probe 如实输出 null 并带原因（不写 0）
  j["e2e_buffer_p50_us"] = pct(p.buffer.p50);
  if (!p.buffer_reason.empty()) j["e2e_buffer_reason"] = p.buffer_reason;
}

namespace {

// 统一的留痕包装：工具 handler 返回前把结果记进 trace（失败也要记，证据校验只认真实观测）
tool::ToolResult record(std::vector<ToolTrace>* trace, const char* name, tool::ToolResult r) {
  if (trace) trace->push_back(ToolTrace{name, r.content, r.data});
  return r;
}

// 动作类工具的返回：动作写进 text，data 只带动作之后重新测到的指标
tool::ToolResult action_result(std::vector<ToolTrace>* trace, const char* name, const std::string& text,
                               IServicePipeline& pipe) {
  const auto m = pipe.latest();
  if (m.samples == 0) {
    return record(trace, name,
                  tool::ToolResult{true, text + "（暂无媒体流采样）", json{{"samples", 0}}});
  }
  auto j = metrics_json(m, now_us());
  return record(trace, name, tool::ToolResult{true, text + " 最新观测: " + j.dump(), j});
}

// ---------------------------------------------------------------- run_shell 审计
//
// 审计是「尝试过什么」的唯一凭据：被拒绝的调用也必须留行，否则 AC-11 只能证明
// "白名单存在"，不能证明"越权尝试被记录"。因此 decision=denied 的记录与 ran 同等重要。
constexpr std::size_t kAuditMaxLine = 4096;   // 单条上限（SPEC 口径）
constexpr std::size_t kAuditMaxArgs = 8;      // 截断时保留的参数个数
constexpr std::size_t kAuditArgBytes = 64;    // 截断时单个参数的字节数
constexpr std::size_t kAuditMaxCmd = 256;     // cmd 也可能被模型写得很长

const std::string& allowed_commands_text() {
  static const std::string kText = [] {
    std::string s;
    for (const auto& c : sandbox::allowed_commands()) {
      if (!s.empty()) s += ", ";
      s += c;
    }
    return s;
  }();
  return kText;
}

// 超长记录按「args 截断 → cmd 截断 → 清空 args」逐级压缩，保证单条 ≤4 KiB。
// 截断必须留痕（"truncated":true），否则审计会看起来像"参数本来就这么少"。
std::string build_audit_line(nlohmann::ordered_json rec) {
  std::string line = rec.dump();
  if (line.size() <= kAuditMaxLine) return line;

  rec["truncated"] = true;
  nlohmann::ordered_json cut = nlohmann::ordered_json::array();
  if (rec.contains("args") && rec["args"].is_array()) {
    for (const auto& a : rec["args"]) {
      if (cut.size() >= kAuditMaxArgs) break;
      std::string s = a.is_string() ? a.get<std::string>() : a.dump();
      if (s.size() > kAuditArgBytes) s.resize(kAuditArgBytes);
      cut.push_back(s);
    }
  }
  rec["args"] = std::move(cut);
  if (rec.contains("cmd") && rec["cmd"].is_string()) {
    std::string cmd = rec["cmd"].get<std::string>();
    if (cmd.size() > kAuditMaxCmd) {
      cmd.resize(kAuditMaxCmd);
      rec["cmd"] = cmd;
    }
  }
  line = rec.dump();
  if (line.size() > kAuditMaxLine) {
    rec["args"] = nlohmann::ordered_json::array();
    line = rec.dump();
  }
  return line;
}

// O_APPEND 单次追加一行：并发进程（cue run 与 cue-eval）写同一文件时不会互相覆盖
// 半行。失败只返回原因，由调用方决定「不阻断工具返回但必须出声」。
bool append_audit(const std::string& path, const nlohmann::ordered_json& rec, std::string& error) {
  if (path.empty()) return true;
  std::string line = build_audit_line(rec);
  line.push_back('\n');

  std::error_code ec;
  const std::filesystem::path p(path);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);

  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd < 0) {
    error = std::strerror(errno);
    return false;
  }
  std::size_t off = 0;
  while (off < line.size()) {
    const ssize_t n = ::write(fd, line.data() + off, line.size() - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      error = std::strerror(errno);
      ::close(fd);
      return false;
    }
    off += static_cast<std::size_t>(n);
  }
  ::close(fd);
  return true;
}

// run_shell 的完整语义：校验参数 → 白名单 → 沙箱执行 → 审计（含拒绝）→ 回灌文本。
tool::ToolResult run_shell_tool(const json& a, std::vector<ToolTrace>* trace,
                                const RealToolsOptions& opts) {
  const std::string cmd = a.at("cmd").get<std::string>();
  const json raw_args = a.contains("args") ? a["args"] : json::array();
  const int timeout_ms = a.contains("timeout_ms") ? a["timeout_ms"].get<int>() : 5000;

  // 参数校验放在 handler 里（tool.h 的 ParamSpec 不认识数组元素类型）：
  // 失败也走"拒绝 + 审计"，不能让非法参数变成 schema 错误而绕过留痕。
  std::string deny;
  std::vector<std::string> args;
  if (!raw_args.is_array()) {
    deny = "args 必须是字符串数组";
  } else if (raw_args.size() > 16) {
    deny = "args 最多 16 个";
  } else {
    for (const auto& v : raw_args) {
      if (!v.is_string()) {
        deny = "args 的每个元素都必须是字符串";
        break;
      }
      const std::string s = v.get<std::string>();
      if (s.size() > 256) {
        deny = "args 单个元素超过 256 字节";
        break;
      }
      args.push_back(s);
    }
  }

  sandbox::Result sr;
  const char* decision = "ran";
  std::string reason;
  if (!deny.empty()) {
    decision = "denied";
    reason = deny;
  } else if (!sandbox::is_allowed_command(cmd)) {
    decision = "denied";
    reason = "命令不在白名单";
  } else {
    sandbox::Limits limits;
    limits.timeout_ms = timeout_ms;
    // cancel 一路透传：工具执行中也能被打断（AC-8）
    sr = sandbox::run(cmd, args, limits, opts.cancel);
    if (!sr.ran) {
      decision = "failed";
      reason = sr.error;
    } else if (sr.cancelled) {
      reason = "cancelled";
    } else if (sr.timed_out) {
      reason = "timeout";
    } else if (sr.signal != 0) {
      reason = "signal";
    } else if (sr.exit_code != 0) {
      reason = "nonzero_exit";
    }
  }

  std::string audit_err;
  if (!opts.audit_log_path.empty()) {
    nlohmann::ordered_json rec;
    rec["ts_us"] = now_us();
    rec["tool"] = "run_shell";
    rec["cmd"] = cmd;
    nlohmann::ordered_json audit_args = nlohmann::ordered_json::array();
    if (raw_args.is_array()) {
      for (const auto& v : raw_args) {
        audit_args.push_back(v.is_string() ? v.get<std::string>() : v.dump());
      }
    }
    rec["args"] = std::move(audit_args);
    rec["decision"] = decision;
    rec["reason"] = reason;
    rec["exit"] = sr.exit_code;
    rec["signal"] = sr.signal;
    rec["duration_ms"] = sr.duration_ms;
    rec["stdout_bytes"] = sr.stdout_bytes;
    rec["stderr_bytes"] = sr.stderr_bytes;
    rec["cancelled"] = sr.cancelled;
    rec["degraded"] = sr.degraded;
    if (!append_audit(opts.audit_log_path, rec, audit_err)) {
      // 审计写失败绝不能静默；但也不能因此丢掉工具结果（用户仍需要知道命令跑没跑）。
      std::fprintf(stderr, "[run_shell] 审计写入失败 path=%s err=%s\n",
                   opts.audit_log_path.c_str(), audit_err.c_str());
    }
  }

  std::string text;
  if (!sr.ran) {
    text = std::string(decision == std::string("denied") ? "拒绝：" : "执行失败：") +
           "run_shell 未执行（cmd=" + cmd + "）；原因：" + reason + "；白名单：" +
           allowed_commands_text() +
           "。参数不经过 shell（execve 直投，且只给一份最小环境），"
           "因此 `;`/`|`/`$()` 无法注入，`env` 也读不到 harness 的环境变量。"
           "（沙箱：landlock + rlimit，无 shell）";
  } else {
    text = "run_shell cmd=" + cmd + " args=" + json(args).dump() +
           " exit=" + std::to_string(sr.exit_code) + " signal=" + std::to_string(sr.signal) +
           " duration_ms=" + std::to_string(sr.duration_ms) + "（沙箱：landlock + rlimit，无 shell）";
    if (sr.timed_out) {
      text += "\n注意：超过 timeout_ms=" + std::to_string(timeout_ms) + "，已 kill 整个进程组";
    }
    if (sr.cancelled) text += "\n注意：执行中被取消，已 kill 整个进程组";
    if (sr.degraded) {
      text += "\n注意：landlock 不可用，本次仅 rlimit 生效（" + sr.degraded_reason +
              "）；白名单仍然生效";
    }
    text += "\nstdout(" + std::to_string(sr.stdout_bytes) + " B" +
            (sr.stdout_truncated ? "，已截断" : "") + "):\n" + sr.stdout_text;
    text += "\nstderr(" + std::to_string(sr.stderr_bytes) + " B" +
            (sr.stderr_truncated ? "，已截断" : "") + "):\n" + sr.stderr_text;
  }
  if (!audit_err.empty()) text += "\n（审计写入失败：" + audit_err + "，本次调用未落盘）";

  const bool ok = sr.ran && !sr.cancelled && !sr.timed_out && sr.signal == 0 && sr.exit_code == 0;
  // data 只放执行事实（退出码/耗时/是否超时），请求参数已经在 text 里
  json data{{"exit", sr.exit_code},
            {"signal", sr.signal},
            {"duration_ms", sr.duration_ms},
            {"timed_out", sr.timed_out},
            {"cancelled", sr.cancelled},
            {"degraded", sr.degraded}};
  return record(trace, "run_shell", tool::ToolResult{ok, text, data});
}

}  // namespace


const char* real_ops_system_prompt() {
  return "你是 Cue 的媒体管线运维 agent。你只能通过工具观测与调整管线。"
         "每轮只能调用工具；结论必须引用真实观测作为证据；每个阶段结束调用 stage_complete。"
         "根因必须使用枚举值：network_loss / network_delay / bandwidth_insufficient / "
         "encoder_too_slow / resolution_too_high / queue_backlog / keyframe_storm / clock_drift。"
         "inject_fault 的 kind 可选 drop（下行丢包 %）/ ingress_drop（上行丢包 %）/ "
         "rate（发送速率上限 Mbps）/ backlog（发送排队节奏 fps）；"
         "value=0 表示清除全部注入（不区分是哪一路）。"
         "diagnose 阶段提交 evidence 时，每条只从某次工具返回里逐字摘抄 key=value 片段"
         "（例如 injected=59、fps=30.3、decode_p99_ms=3.199），最多 4 条；"
         "严格禁止解释性文字、中文句子、根因说明或把多次观测合并成一条——"
         "判分会逐条校验：只要出现引用之外的文字，该条证据即判伪。";
}

void build_real_tools(tool::Registry& reg, IServicePipeline& pipe, std::vector<ToolTrace>* trace,
                      const RealToolsOptions& opts) {
  reg.add(tool::ToolSpec{.name = "query_metrics",
                         .desc = "查询媒体管线指标（六段分位 + 帧统计，预聚合）",
                         .risk = tool::Risk::ReadOnly,
                         .params = {{.name = "window_ms",
                                     .type = "integer",
                                     .desc = "窗口毫秒（当前实现固定 1s 采样）",
                                     .required = false,
                                     .enum_values = {},
                                     .has_range = true,
                                     .min = 200,
                                     .max = 60000}}},
          [&pipe, trace](const json&) {
            const auto m = pipe.latest();
            if (m.samples == 0) {
              return record(trace, "query_metrics",
                            tool::ToolResult{false, "尚无采样（cued 未收到媒体流）", json::object()});
            }
            json j = metrics_json(m, now_us());
            add_probe_metrics(j, pipe.probe_latest());
            return record(trace, "query_metrics", tool::ToolResult{true, "metrics=" + j.dump(), j});
          });

  reg.add(tool::ToolSpec{.name = "pipeline_set_param",
                         .desc = "修改媒体管线参数（生效方式为重启管线，会造成短暂断流）",
                         .risk = tool::Risk::Mutating,
                         .params = {{.name = "key",
                                     .type = "string",
                                     .desc = "参数名",
                                     .required = true,
                                     .enum_values = {"bitrate_kbps", "gop", "preset", "transcode", "width",
                                                                  "height"}},
                                    {.name = "value",
                                     .type = "string",
                                     .desc = "目标值",
                                     .required = true,
                                     .enum_values = {},
                                     .has_range = false,
                                     .min = 0,
                                     .max = 0}}},
          [&pipe, trace](const json& a) {
            std::string err;
            const std::string k = a["key"].get<std::string>();
            const std::string v = a["value"].get<std::string>();
            if (!pipe.set_param(k, v, err)) {
              return record(trace, "pipeline_set_param",
                            tool::ToolResult{false, "设置失败: " + err, json::object()});
            }
            return action_result(trace, "pipeline_set_param",
                                 "已设置 " + k + "=" + v + "（管线已重启）", pipe);
          });

  reg.add(tool::ToolSpec{.name = "inject_fault",
                         .desc = "注入网络损伤（危险动作；value=0 清除全部注入）",
                         .risk = tool::Risk::Dangerous,
                         .params = {{.name = "kind",
                                     .type = "string",
                                     .desc = "类型：drop=下行丢包% / ingress_drop=上行丢包% / "
                                             "rate=发送速率上限Mbps / backlog=发送排队节奏fps",
                                     .required = true,
                                     .enum_values = {"drop", "ingress_drop", "rate", "backlog"}},
                                    {.name = "value",
                                     .type = "number",
                                     .desc = "注入值（drop/ingress_drop 为百分比，rate 为 Mbps，"
                                             "backlog 为帧/s；0 = 清除全部注入）",
                                     .required = true,
                                     .enum_values = {},
                                     .has_range = true,
                                     .min = 0,
                                     .max = 100}}},
          [&pipe, trace](const json& a) {
            std::string err;
            const std::string k = a["kind"].get<std::string>();
            const double v = a["value"].get<double>();
            if (!pipe.inject_fault(k, v, err)) {
              return record(trace, "inject_fault", tool::ToolResult{false, "注入失败: " + err, json::object()});
            }
            if (v <= 0.0) {
              return action_result(trace, "inject_fault", "已清除全部注入（kind=" + k + "）", pipe);
            }
            return action_result(trace, "inject_fault",
                                 "已注入 " + k + "=" + std::to_string(v), pipe);
          });

  reg.add(tool::ToolSpec{.name = "fetch_observation",
                         .desc = "获取观测（日志尾部 / 进程状态 / 帧统计）",
                         .risk = tool::Risk::ReadOnly,
                         .params = {{.name = "source",
                                     .type = "string",
                                     .desc = "来源",
                                     .required = true,
                                     .enum_values = {"logs", "procs", "frames", "events"}},
                                    {.name = "tail",
                                     .type = "integer",
                                     .desc = "日志行数",
                                     .required = false,
                                     .enum_values = {},
                                     .has_range = true,
                                     .min = 1,
                                     .max = 200}}},
          [&pipe, trace](const json& a) {
            std::string err;
            const std::string src = a["source"].get<std::string>();
            const int tail = a.contains("tail") ? a["tail"].get<int>() : 20;
            const std::string obs = pipe.observation(src, tail, err);
            if (!err.empty()) return record(trace, "fetch_observation", tool::ToolResult{false, err, json::object()});
            // data 只带真实读数；source 是请求参数，放 text 里就够
            return record(trace, "fetch_observation",
                          tool::ToolResult{true, obs, json::object()});
          });

  // 第 5 个工具：只读诊断命令。risk=Dangerous 是**审批**口径（要显式授权），
  // 与"它能写什么"无关：真正的写权限由 sandbox 的 landlock + rlimit + 白名单否决。
  // 两者都要，缺一不可：审批决定"要不要让这个 agent 执行命令"，
  // 沙箱决定"即使批准了它最多能做什么"。
  reg.add(tool::ToolSpec{
              .name = "run_shell",
              .desc = "在 landlock + rlimit 沙箱里执行一条只读诊断命令（不经 shell；参数原样"
                      "传给可执行文件，`;`/`|`/`$()` 不会被解释）。cmd 必须是白名单中的命令名，"
                      "或 /usr/bin、/bin 下的同名绝对路径；其它一律拒绝并记入审计。",
              .risk = tool::Risk::Dangerous,
              .params = {{.name = "cmd",
                          .type = "string",
                          .desc = "白名单命令名（或 /usr/bin/、/bin/ 下的绝对路径）",
                          .required = true,
                          .enum_values = sandbox::allowed_commands(),
                          .has_range = false,
                          .min = 0,
                          .max = 0,
                          // 白名单外命令必须到达 handler 才能落审计行（AC-11）：
                          // 在 schema 校验层硬拒会让"尝试了越权命令"这件事消失。
                          .soft_enum = true},
                         {.name = "args",
                          .type = "array",
                          .desc = "参数数组（元素为字符串，最多 16 个、每个 ≤256 字节；不经过 shell）",
                          .required = false,
                          .enum_values = {},
                          .has_range = false,
                          .min = 0,
                          .max = 0},
                         {.name = "timeout_ms",
                          .type = "integer",
                          .desc = "墙钟超时毫秒（200..30000，默认 5000）",
                          .required = false,
                          .enum_values = {},
                          .has_range = true,
                          .min = 200,
                          .max = 30000}}},
          [trace, opts](const json& a) { return run_shell_tool(a, trace, opts); });
}

}  // namespace cue::pipeline
