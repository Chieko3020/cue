#include "cue/tool/tools.h"

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
              {"injected", m.injected_observed ? json(m.injected) : json(nullptr)},
              {"decoded", m.decoded},
              {"decode_errors", m.decode_errors},
              // 六段分位（ms）：每段 p50/p95/p99 一律输出；值为 null = 该位没有样本。
              // N-05 之前这里只有 p50 与部分 p99，且 p95 在解析侧就被丢掉了——
              // agent 的 query_metrics 里根本读不到 p95，"证据里写 p95=..."无从谈起。
              {"recv_p50_ms", pct(m.recv_p50)},
              {"recv_p95_ms", pct(m.recv_p95)},
              {"recv_p99_ms", pct(m.recv_p99)},
              {"buffer_p50_ms", pct(m.buffer_p50)},
              {"buffer_p95_ms", pct(m.buffer_p95)},
              {"buffer_p99_ms", pct(m.buffer_p99)},
              {"decode_p50_ms", pct(m.decode_p50)},
              {"decode_p95_ms", pct(m.decode_p95)},
              {"decode_p99_ms", pct(m.decode_p99)},
              {"op_p50_ms", pct(m.op_p50)},
              {"op_p95_ms", pct(m.op_p95)},
              {"op_p99_ms", pct(m.op_p99)},
              {"encode_p50_ms", pct(m.encode_p50)},
              {"encode_p95_ms", pct(m.encode_p95)},
              {"encode_p99_ms", pct(m.encode_p99)},
              {"send_p50_ms", pct(m.send_p50)},
              // send 段 p95/p99：延迟注入（FR-PIPE.10 的 delay / jitter）的主观测点。
              // 没有它，网络时延类根因在 cued 侧就没有任何可引用的分位（D-7）。
              {"send_p95_ms", pct(m.send_p95)},
              {"send_p99_ms", pct(m.send_p99)},
              // 进程身份/存活：pid=null 表示当前没有 cued（不是 pid 0 的真进程）
              {"pid", m.pid == 0 ? json(nullptr) : json(static_cast<int64_t>(m.pid))},
              {"alive", m.alive},
              // 协议/发送/编码/预算侧计数：过去只在 cued 的 METRICS 行里，agent 看不到
              {"rejected", m.rejected},
              {"send_errors", m.send_errors},
              {"encode_errors", m.encode_errors},
              {"decode_drops", m.decode_drops},
              {"budget_drops", m.budget_drops_observed ? json(m.budget_drops) : json(nullptr)},
              {"queue_dropped", m.queue_dropped},
              // 延迟注入的两个计数：被压住的数据报数 + 最长压住时长（微秒）。
              // 「延迟而非丢包」的判据就在这一对与 injected/budget_drops 的对照上。
              {"delayed_pkts", m.delayed_pkts},
              {"delayed_max_us", m.delayed_max_us},
              // 迟到帧：延迟注入**不**该让它涨（注入只压下行包，不改帧年龄）。
              // 它涨了说明问题在积压/时钟，而不是网络时延——这是区分两种根因的一票。
              {"ingress_late", m.ingress_late},
              {"samples", m.samples},
              // 控制面（T-05）：通道类型与 cued 侧累计处理的控制请求数。旧版 cued 不上报
              // 时 control_channel 为 null——"不知道"不能写成 "stdout"。
              {"control_channel", m.control_channel.empty() ? json(nullptr)
                                                            : json(m.control_channel)},
              {"control_ops", m.control_ops},
              // 编码档位（N-02）：AVC level / 分辨率类根因过去只有一个报错字符串可讲，
              // 因为 agent 看不到"实际在用什么尺寸/码率编码"。这里输出的是 cued
              // ParamStore 的运行期真值（set_param 改过也在内）。
              // encoder_known=false（控制通道降级 / HTTP 后端不带 params）时写 null——
              // "不知道"不能写成 0（0 会被读成"没有转码"或"尺寸为 0"）。
              {"encode_width", m.encoder_known ? json(m.enc_width) : json(nullptr)},
              {"encode_height", m.encoder_known ? json(m.enc_height) : json(nullptr)},
              {"encode_bitrate_kbps", m.encoder_known ? json(m.enc_bitrate_kbps) : json(nullptr)},
              {"encode_gop", m.encoder_known ? json(m.enc_gop) : json(nullptr)},
              {"encode_preset", m.encoder_known && !m.enc_preset.empty() ? json(m.enc_preset)
                                                                         : json(nullptr)},
              {"encode_transcode", m.encoder_known && !m.enc_transcode.empty()
                                       ? json(m.enc_transcode)
                                       : json(nullptr)},
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
  // 四段的 p95：probe 的 METRICS 与 probe.metric 一直带 p50/p95/p99，
  // 只是过去这里没搬（与 cued 侧 p95 同类问题）。命名沿用 e2e_*_p50_us/p99_us。
  j["e2e_recv_p95_us"] = pct(p.recv.p95);
  j["e2e_reassembly_p95_us"] = pct(p.reassembly.p95);
  j["e2e_decode_p95_us"] = pct(p.decode.p95);
  // 抖动缓冲段：probe 侧的等待时长（进队 → 放行）。没有样本时是 null，绝不写 0。
  j["e2e_buffer_p50_us"] = pct(p.buffer.p50);
  j["e2e_buffer_p95_us"] = pct(p.buffer.p95);
  j["e2e_buffer_p99_us"] = pct(p.buffer.p99);
  // 四段 p99 也补齐（过去只搬了 buffer 的 p99）：agent 引用 e2e recv/decode 尾部时
  // 不该只有 p50 可讲。同样 null = 没有样本。
  j["e2e_recv_p99_us"] = pct(p.recv.p99);
  j["e2e_reassembly_p99_us"] = pct(p.reassembly.p99);
  j["e2e_decode_p99_us"] = pct(p.decode.p99);
  // 抖动缓冲的状态：level 涨说明升档在换平滑（代价是延迟），skipped 涨说明帧真的丢了。
  j["e2e_jb_level"] = p.jb_level;
  j["e2e_jb_switches"] = p.jb_switches;
  j["e2e_jb_target_frames"] = p.jb_target_frames;
  j["e2e_jb_depth"] = p.jb_depth;
  j["e2e_jb_skipped"] = p.jb_skipped;
  j["e2e_jb_late"] = p.jb_late;
  // N-01：音频下行的恢复账。三者口径互不重叠（见 include/cue/common/metrics.h）：
  // fec 是"用后一包冗余恢复的真实上一帧"，plc 是"缺口处外推的隐藏帧"，
  // unconcealed 是"没补上的缺口"。agent 判"音频修好了没有"要看后两个不涨。
  j["e2e_audio_fec_recovered"] = p.audio_fec_recovered;
  j["e2e_audio_plc_frames"] = p.audio_plc_frames;
  j["e2e_audio_unconcealed_frames"] = p.audio_unconcealed_frames;
  // N-02：FEC / NACK / 幽灵帧 / 音频帧号账。这些数 probe 一直在算、也一直在 METRICS 行
  // 与 probe.metric 事件里上报，只是没进 agent 的观测面——于是"M0 幽灵帧到底发生过没有"
  // 在证据层无法成立（m2-constraints 的候选引用了根本不存在的 frames_no_data）。
  // 口径见 include/cue/common/metrics.h：fec_* 是**视频片**的账，audio_fec_* 是音频帧的账。
  j["e2e_fec_recovered"] = p.fec_recovered;
  j["e2e_fec_groups_rx"] = p.fec_groups_rx;
  j["e2e_fec_unrecoverable"] = p.fec_unrecoverable;
  j["e2e_fec_unrecoverable_pkts"] = p.fec_unrecoverable_pkts;
  // 幽灵帧守卫：两条分支分开报，判读方式不同（实测见 metrics.h 同名字段）。
  //   e2e_fec_ghost_discarded  = 常态：FEC 校验包总在整帧之后到（零丢包 8 s/241 帧 →
  //     250 次），只能当"守卫在跑"的读数，**不是故障指示**；
  //   e2e_frag_ghost_discarded = 异常：迟到分片/重传去重建已完成帧（零丢包 0、5% 丢包 2~3）。
  // 所以 suite 的幽灵帧证据用 e2e_fec_recovered + e2e_frag_ghost_discarded。
  j["e2e_fec_ghost_discarded"] = p.fec_ghost_discarded;
  j["e2e_frag_ghost_discarded"] = p.frag_ghost_discarded;
  // N-10（FR-PIPE.14）：重组表的帧数/字节双上限。三个数一起看才有意义：
  //   e2e_reassembly_pending_dropped = 因预算淘汰的帧数（**已计入 e2e_lost**，是它的子集），
  //     它涨而 e2e_lost 同比涨 = 接收端在自保，不是在丢链路；
  //   e2e_reassembly_pending_bytes   = 当前占用（gauge，不是累计），持续贴近 64 MiB 才是异常；
  //   e2e_reassembly_rejected        = 病态单帧被拒收的包数（与协议层 rejected 不同）。
  j["e2e_reassembly_pending_dropped"] = p.reassembly_pending_dropped;
  j["e2e_reassembly_pending_bytes"] = p.reassembly_pending_bytes;
  j["e2e_reassembly_rejected"] = p.reassembly_rejected;
  j["e2e_lost"] = p.lost;
  j["e2e_nack_sent"] = p.nack_sent;
  j["e2e_audio_datagrams"] = p.audio_datagrams;
  j["e2e_audio_seq_gaps"] = p.audio_seq_gaps;
  j["e2e_audio_dups"] = p.audio_dups;
  j["e2e_audio_reordered"] = p.audio_reordered;
  j["e2e_audio_stale"] = p.audio_stale;
  j["e2e_audio_decode_errors"] = p.audio_decode_errors;
  if (!p.buffer_reason.empty()) j["e2e_buffer_reason"] = p.buffer_reason;
}

namespace {

// 统一的留痕包装：工具 handler 返回前把结果记进 trace（失败也要记，证据校验只认真实观测）
// 成功才记 trace：失败由 expected 的 error 承载，没有 content/data 可记
// （data 在 error 里，见 ToolError::data）。
tool::ToolOutcome record(std::vector<ToolTrace>* trace, const char* name, tool::ToolOutcome r) {
  if (trace && r) trace->push_back(ToolTrace{name, r->content, r->data});
  return r;
}

// 动作类工具的返回：动作写进 text，data 只带动作之后重新测到的指标
tool::ToolOutcome action_result(std::vector<ToolTrace>* trace, const char* name, const std::string& text,
                               IServicePipeline& pipe) {
  const auto m = pipe.latest();
  if (m.samples == 0) {
    return record(trace, name,
                  tool::ToolResult{text + "（暂无媒体流采样）", json{{"samples", 0}}});
  }
  auto j = metrics_json(m, now_us());
  return record(trace, name, tool::ToolResult{text + " 最新观测: " + j.dump(), j});
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

// 后端字符串 → 枚举。不认识的取值回退到 landlock（安全默认）并出声：
// 安全选项"静默变成默认值"等于让调用方以为 docker 生效了。
sandbox::Backend resolve_backend(const std::string& name) {
  if (name == "landlock") return sandbox::Backend::Landlock;
  if (name == "docker") return sandbox::Backend::Docker;
  std::fprintf(stderr,
               "[run_shell] sandbox_backend=\"%s\" 不认识（可选 landlock/docker），本次使用 "
               "landlock\n",
               name.c_str());
  return sandbox::Backend::Landlock;
}

// run_shell 的完整语义：校验参数 → 白名单 → 沙箱执行（landlock/docker + 降权）→ 审计
// （含拒绝）→ 回灌文本。
tool::ToolOutcome run_shell_tool(const json& a, std::vector<ToolTrace>* trace,
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
    limits.backend = resolve_backend(opts.sandbox_backend);
    limits.docker_image = opts.docker_image;
    // FR-SANDBOX.2：可写的只有临时目录与本 run 的目录（runs/<run_id>/）。
    // 审计文件就落在那一层，因此从它推目录最不容易漂（换个 --out 也自动跟着走）。
    std::vector<std::string> rw{"/tmp"};
    std::string run_dir = "runs";
    if (!opts.audit_log_path.empty()) {
      const auto parent = std::filesystem::path(opts.audit_log_path).parent_path();
      if (!parent.empty()) run_dir = parent.string();
    }
    rw.push_back(run_dir);
    limits.rw_paths = std::move(rw);
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
    // FR-SANDBOX.6：沙箱的**决策**也要留痕，而不只是命令的结局。
    // 这一条要能回答"它被允许写哪里""有没有降权""用的哪个后端"——
    // 只记 exit/stderr 的话，一次因 landlock 而失败的写入看起来和"命令自己报错"一样。
    nlohmann::ordered_json sb_rec;
    sb_rec["backend"] = sr.backend.empty() ? std::string("none") : sr.backend;
    sb_rec["rw_paths"] = sr.rw_paths_applied;
    sb_rec["rw_not_applied"] = sr.paths_not_applied;
    sb_rec["privilege"] = sr.privilege_drop;
    sb_rec["landlock_degraded"] = sr.degraded;
    rec["sandbox"] = std::move(sb_rec);
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
    if (!sr.privilege_drop.empty() && sr.privilege_drop.rfind("not_root", 0) == 0) {
      text += "\n注意：以非 root 启动，无法降权（" + sr.privilege_drop +
              "）；landlock 与 rlimit 仍然生效";
    }
    if (!sr.paths_not_applied.empty()) {
      text += "\n注意：以下路径请求了但没生效（不可写）：";
      for (const auto& p : sr.paths_not_applied) text += "\n  - " + p;
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
  if (ok) return record(trace, "run_shell", tool::ToolResult{text, data});
  // 失败分三类，模型该做的事完全不同：
  //   白名单/landlock 拒绝 → 换命令（重试同一命令没用）
  //   超时               → 缩小范围或放弃
  //   非零退出/被信号杀   → 看 stderr 改参数
  const auto kind = sr.timed_out ? tool::ToolError::Kind::Timeout
                    : !sr.ran    ? tool::ToolError::Kind::Sandbox
                                 : tool::ToolError::Kind::Execution;
  return record(trace, "run_shell",
                std::unexpected(tool::ToolError{kind, text, data}));
}

// ---------------------------------------------------------------- lifecycle 档位
//
// profile 是**真生效**的：它映射成一串 set_param（走同一个控制通道，不重启进程）。
// 早期版本只把它记在文本里、不改变任何参数——那种"看起来能改、实际不动"的参数
// 比没有更坏：agent 会以为改完了，然后再去别处找根因。
//
// 为什么刻意不含 width/height：控制面要求这两个键成对下发（0/0 = 沿用解码尺寸），
// 而 IServicePipeline::set_param 一次只改一个键；从 0/0 出发先改任一边都会被
// "必须成对"拒掉。与其留一个"一半会失败"的档位，不如让 profile 只管单键就能
// 一致生效的码控参数，分辨率仍然由 pipeline_set_param 成对修改。
struct ProfileStep {
  const char* key;
  const char* value;
};

struct Profile {
  const char* name;
  std::vector<ProfileStep> steps;
};

const std::vector<Profile>& profile_table() {
  static const std::vector<Profile> kProfiles = {
      {"passthrough", {{"transcode", "off"}}},
      // 码档：transcode 打开 + 一路码控参数。fps=30 与推流侧一致，
      // 否则 x264 的码率控制会按错误的帧率分配 bit，改完档反而更糟。
      {"low-600k",
       {{"transcode", "on"}, {"bitrate_kbps", "600"}, {"fps", "30"}, {"gop", "60"},
        {"preset", "veryfast"}}},
      {"std-1500k",
       {{"transcode", "on"}, {"bitrate_kbps", "1500"}, {"fps", "30"}, {"gop", "60"},
        {"preset", "veryfast"}}},
      {"high-4000k",
       {{"transcode", "on"}, {"bitrate_kbps", "4000"}, {"fps", "30"}, {"gop", "60"},
        {"preset", "faster"}}},
  };
  return kProfiles;
}

std::vector<std::string> profile_names() {
  std::vector<std::string> out;
  out.reserve(profile_table().size());
  for (const auto& p : profile_table()) out.emplace_back(p.name);
  return out;
}

// 应用一个档位。失败时把"卡在哪一步"写进 err：只说"profile 应用失败"会让调用方
// 以为档位名写错了，而真实原因往往是被拒的键（例如运行期不允许的键）。
bool apply_profile(IServicePipeline& pipe, const std::string& name, std::string& applied,
                   std::string& err) {
  const Profile* hit = nullptr;
  for (const auto& p : profile_table()) {
    if (name == p.name) {
      hit = &p;
      break;
    }
  }
  if (hit == nullptr) {
    err = "未知 profile \"" + name + "\"（可用：" + [&] {
      std::string s;
      for (const auto& n : profile_names()) {
        if (!s.empty()) s += " / ";
        s += n;
      }
      return s;
    }() + "）";
    return false;
  }
  std::string out;
  for (const auto& step : hit->steps) {
    std::string serr;
    if (!pipe.set_param(step.key, step.value, serr)) {
      err = std::string("profile=") + name + " 的 " + step.key + "=" + step.value +
            " 未生效: " + serr;
      return false;
    }
    if (!out.empty()) out += ", ";
    out += step.key;
    out += "=";
    out += step.value;
  }
  applied = out;
  return true;
}

}  // namespace


const char* real_ops_system_prompt() {
  return "你是 Cue 的媒体管线运维 agent。你只能通过工具观测与调整管线。"
         "每轮只能调用工具；结论必须引用真实观测作为证据；每个阶段结束调用 stage_complete。"
         "根因必须使用枚举值：network_loss / network_delay / bandwidth_insufficient / "
         "encoder_too_slow / resolution_too_high / queue_backlog / keyframe_storm / clock_drift。"
         "inject_fault 的 kind 可选 drop（下行丢包 %）/ ingress_drop（上行丢包 %）/ "
         "rate（发送速率上限 Mbps）/ backlog（发送排队节奏 fps）/ delay（固定延迟 ms）/ "
         "jitter（抖动幅度 ms）；value=0 表示清除全部注入（不区分是哪一路）。"
         "区分丢包与时延：丢包涨 injected / budget_drops，时延涨 delayed_pkts 与 send 段分位"
         "（send_p50_ms / send_p99_ms）而 injected 不涨。"
         "pipeline_start / pipeline_restart 可带 profile（passthrough / low-600k / "
         "std-1500k / high-4000k）一键套用码控档位。"
         "diagnose 阶段提交 evidence 时，每条只从某次工具返回里逐字摘抄 key=value 片段"
         "（例如 injected=59、fps=30.3、decode_p99_ms=3.199），最多 4 条；"
         "严格禁止解释性文字、中文句子、根因说明或把多次观测合并成一条——"
         "判分会逐条校验：只要出现引用之外的文字，该条证据即判伪。";
}

// SPEC §7.4 的三个 mutating 工具。抽成一份、两处（cue-serve 与 build_real_tools）共用的理由：
// 它们描述的是**同一组动作**，schema/风险级/档位表任何一处改了两边就会分叉——
// 「控制台能做的 agent 不能做」正是这种分叉最直观的症状。
//
// profile 只挂在 start/restart 上：stop 没有任何"档位"可套，把 profile 留在 stop 的
// schema 里就是那个「看起来能改、实际不动」的参数，这里直接不给。
void add_lifecycle_tools(tool::Registry& reg, IServicePipeline& pipe,
                         std::vector<ToolTrace>* trace) {
  const tool::ParamSpec profile_param{.name = "profile",
                                      .type = "string",
                                      .desc = "码控档位（真的生效：映射成一串 set_param，"
                                              "不重启进程）：passthrough=关转码；"
                                              "low-600k / std-1500k / high-4000k=开转码 + "
                                              "对应码率/fps30/gop60/preset。"
                                              "分辨率不在档位里（width/height 必须成对设置，"
                                              "用 pipeline_set_param 改）。",
                                      .required = false,
                                      .enum_values = profile_names(),
                                      .has_range = false,
                                      .min = 0,
                                      .max = 0};

  reg.add(tool::ToolSpec{.name = "pipeline_start",
                         .desc = "启动媒体管线（cued 子进程）；已在运行时只套用 profile",
                         .risk = tool::Risk::Mutating,
                         .params = {profile_param}},
          [&pipe, trace](const json& a) {
            const std::string profile =
                a.contains("profile") ? a["profile"].get<std::string>() : std::string();
            const bool was_running = pipe.running();
            if (!was_running) {
              std::string err;
              if (!pipe.start(err)) {
                return record(trace, "pipeline_start",
                              std::unexpected(tool::ToolError{tool::ToolError::Kind::Execution,
                                                              "启动失败: " + err,
                                                              json{{"running", false}}}));
              }
            }
            std::string note = was_running ? "管线已在运行" : "管线已启动";
            json data{{"running", true}, {"already_running", was_running}};
            if (!profile.empty()) {
              std::string applied;
              std::string perr;
              if (!apply_profile(pipe, profile, applied, perr)) {
                // 启动成功但档位失败：必须如实分开说，不能因为 start 成功就报整体成功
                return record(trace, "pipeline_start",
                              std::unexpected(tool::ToolError{tool::ToolError::Kind::Execution,
                                                              note + "，但 profile 未生效: " + perr,
                                                              data}));
              }
              note += "；profile=" + profile + " → " + applied;
              data["profile"] = profile;
            }
            return record(trace, "pipeline_start",
                          tool::ToolResult{note, std::move(data)});
          });

  reg.add(tool::ToolSpec{.name = "pipeline_stop",
                         .desc = "停止媒体管线（cued 子进程）。评测里这会让当前 case 断流，"
                                 "只在确实需要重启时才用；能用 pipeline_set_param / "
                                 "inject_fault 解决的不要停进程。",
                         .risk = tool::Risk::Mutating,
                         .params = {}},
          [&pipe, trace](const json&) {
            pipe.stop();
            return record(trace, "pipeline_stop",
                          tool::ToolResult{"管线已停止", json{{"running", false}}});
          });

  reg.add(tool::ToolSpec{.name = "pipeline_restart",
                         .desc = "重启媒体管线（会造成短暂断流）；可顺带套用 profile",
                         .risk = tool::Risk::Mutating,
                         .params = {profile_param}},
          [&pipe, trace](const json& a) {
            pipe.stop();
            std::string err;
            if (!pipe.start(err)) {
              return record(trace, "pipeline_restart",
                            std::unexpected(tool::ToolError{tool::ToolError::Kind::Execution,
                                                            "重启失败: " + err,
                                                            json{{"running", false}}}));
            }
            std::string note = "管线已重启";
            json data{{"running", true}};
            const std::string profile =
                a.contains("profile") ? a["profile"].get<std::string>() : std::string();
            if (!profile.empty()) {
              std::string applied;
              std::string perr;
              if (!apply_profile(pipe, profile, applied, perr)) {
                return record(trace, "pipeline_restart",
                              std::unexpected(tool::ToolError{tool::ToolError::Kind::Execution,
                                                              note + "，但 profile 未生效: " + perr,
                                                              data}));
              }
              note += "；profile=" + profile + " → " + applied;
              data["profile"] = profile;
            }
            return record(trace, "pipeline_restart", tool::ToolResult{note, std::move(data)});
          });
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
            // 先主动查一次（Process 后端走 UDS metrics.query）：拿到的才是"此刻"，
            // 而 latest() 是 1 Hz 的 stdout 缓存。查询失败不致命——缓存里如果有数，
            // 仍然可用，age_ms 会如实告诉 agent 它有多旧。
            // 返回值不参与判定：失败就用缓存，缓存有多旧由 metrics_json 的 age_ms 说明。
            std::string qerr;
            (void)pipe.refresh_metrics(qerr);
            const auto m = pipe.latest();
            if (m.samples == 0) {
              std::string why = "尚无采样（cued 未收到媒体流）";
              if (!qerr.empty()) why += "；主动查询：" + qerr;
              return record(trace, "query_metrics",
                            std::unexpected(tool::ToolError{tool::ToolError::Kind::Execution, why, json::object()}));
            }
            json j = metrics_json(m, now_us());
            add_probe_metrics(j, pipe.probe_latest());
            return record(trace, "query_metrics", tool::ToolResult{"metrics=" + j.dump(), j});
          });

  reg.add(tool::ToolSpec{.name = "pipeline_set_param",
                         .desc = "修改媒体管线参数（走 UDS 控制通道，运行期生效、不重启进程；"
                                 "改编码参数会在下一帧重建编码器）",
                         .risk = tool::Risk::Mutating,
                         .params = {{.name = "key",
                                     .type = "string",
                                     .desc = "参数名",
                                     .required = true,
                                     .enum_values = {"bitrate_kbps", "gop", "preset", "transcode", "width",
                                                     "height", "fps"}},
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
                            std::unexpected(tool::ToolError{tool::ToolError::Kind::Execution, "设置失败: " + err, json::object()}));
            }
            return action_result(trace, "pipeline_set_param",
                                 "已设置 " + k + "=" + v + "（运行期生效，未重启 cued）", pipe);
          });

  reg.add(tool::ToolSpec{.name = "inject_fault",
                         .desc = "注入网络损伤（危险动作；value=0 清除全部注入）。"
                                 "drop/ingress_drop 让包丢失（injected 涨）；"
                                 "delay/jitter 只让包晚到（delayed_pkts 与 send 段分位涨，"
                                 "injected 不涨）——两者在指标上可区分，不要混用。",
                         .risk = tool::Risk::Dangerous,
                         .params = {{.name = "kind",
                                     .type = "string",
                                     .desc = "类型：drop=下行丢包% / ingress_drop=上行丢包% / "
                                             "rate=发送速率上限Mbps / backlog=发送排队节奏fps / "
                                             "delay=固定延迟ms / jitter=抖动幅度ms",
                                     .required = true,
                                     .enum_values = {"drop", "ingress_drop", "rate", "backlog",
                                                     "delay", "jitter"}},
                                    {.name = "value",
                                     .type = "number",
                                     .desc = "注入值，单位随 kind：drop/ingress_drop 是百分比 "
                                             "(0..100)，rate 是 Mbps (0.1..100)，backlog 是帧/s "
                                             "(0..100)，delay/jitter 是**毫秒** (0..2000)。"
                                             "0 = 清除全部注入（不分哪一路）。",
                                     .required = true,
                                     .enum_values = {},
                                     // 不做 schema 级范围：四个 kind 的单位不同（% / Mbps /
                                     // 帧每秒 / 毫秒），一个 [min,max] 无论取哪一边都是错的。
                                     // 范围与单位校验只有一份，在 control_plane 的 apply_fault
                                     // （cued 与工具层共用），这里不重复也不放宽。
                                     .has_range = false,
                                     .min = 0,
                                     .max = 0}}},
          [&pipe, trace](const json& a) {
            std::string err;
            const std::string k = a["kind"].get<std::string>();
            const double v = a["value"].get<double>();
            if (!pipe.inject_fault(k, v, err)) {
              return record(trace, "inject_fault", std::unexpected(tool::ToolError{tool::ToolError::Kind::Execution, "注入失败: " + err, json::object()}));
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
            if (!err.empty()) return record(trace, "fetch_observation", std::unexpected(tool::ToolError{tool::ToolError::Kind::Execution, err, json::object()}));
            // data 只带真实读数；source 是请求参数，放 text 里就够
            return record(trace, "fetch_observation",
                          tool::ToolResult{obs, json::object()});
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

  // 第 6~8 个工具：lifecycle（与 cue-serve 共用同一份实现，见 add_lifecycle_tools）。
  // 放在这里而不是各入口自己注册：需求 FR-TOOL.1 列的是 8 个工具，agent 侧缺这三个
  // 就是一条真实的功能缺口（agent 无法自己重启管线，也就无法验证「重启后是否恢复」）。
  add_lifecycle_tools(reg, pipe, trace);
}

}  // namespace cue::pipeline
