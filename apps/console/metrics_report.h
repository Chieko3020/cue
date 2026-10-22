// /metrics 的生成逻辑（FR-OBS.3 / 技术设计文档 §12.3）
//
// 为什么是"纯函数 + 采样器"而不是直接写在 HTTP 路由里：
//  - 指标口径要能被单测断言（字段名、缺样本时写 0 且带 note、10 项一个不少）；
//  - /proc 的解析（VmRSS / utime+stime）单独可测，不必真的去读某个进程。
//
// 三条口径，全文一致：
//  1. **拿不到就写 0，并带 note 说明为什么是 0**。文档要求"输出 0 而不是省略"，
//     但只写 0 会让人把"没有 run"读成"跑过且是 0"，所以每个可能为 0 的项都留一句口径。
//  2. 分位（p99）的单位是**微秒**（指标名自带 _us），cued 侧上报的是毫秒，这里 ×1000。
//  3. RSS/CPU 来自 /proc，是**估算/近似**：CPU 是两次采样间的平均占用（首次为 0），
//     按单核百分比计（多核可超过 100）。
#pragma once

#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "cue/common/clock.h"
#include "cue/pipeline/metrics.h"

namespace cue::console {

// ---------------------------------------------------------------- /proc 解析（纯函数）

// /proc/<pid>/status 里的 VmRSS（单位 kB → 字节）。字段不在（僵尸进程）返回 false。
inline bool parse_proc_status_rss(std::string_view text, std::uint64_t& rss_bytes) {
  const std::string_view key = "VmRSS:";
  std::size_t pos = 0;
  while (pos < text.size()) {
    const std::size_t eol = text.find('\n', pos);
    const std::string_view line = text.substr(pos, eol == std::string_view::npos ? std::string_view::npos
                                                                                : eol - pos);
    if (line.rfind(key, 0) == 0) {
      std::string_view rest = line.substr(key.size());
      std::size_t i = 0;
      while (i < rest.size() && (rest[i] == ' ' || rest[i] == '\t')) ++i;
      std::uint64_t kb = 0;
      bool any = false;
      for (; i < rest.size() && rest[i] >= '0' && rest[i] <= '9'; ++i) {
        kb = kb * 10 + static_cast<std::uint64_t>(rest[i] - '0');
        any = true;
      }
      if (!any) return false;
      rss_bytes = kb * 1024;
      return true;
    }
    if (eol == std::string_view::npos) break;
    pos = eol + 1;
  }
  return false;
}

// /proc/<pid>/stat → utime+stime（时钟滴答）。comm 字段可能含空格/括号，所以从**最后一个** ')'
// 之后开始数：第 1 个字段是 state（原第 3 字段），utime 是原第 14、stime 是第 15。
inline bool parse_proc_stat_ticks(std::string_view stat, std::uint64_t& utime, std::uint64_t& stime) {
  const std::size_t rp = stat.rfind(')');
  if (rp == std::string_view::npos) return false;
  std::string_view rest = stat.substr(rp + 1);
  int field = 3;  // 下一个字段的编号：')' 之后第一个是 state（原第 3 字段）
  std::size_t i = 0;
  std::uint64_t vals[16] = {0};
  while (i < rest.size() && field <= 15) {
    while (i < rest.size() && rest[i] == ' ') ++i;
    if (i >= rest.size()) break;
    std::size_t start = i;
    while (i < rest.size() && rest[i] != ' ') ++i;
    const std::string_view tok = rest.substr(start, i - start);
    if (field >= 3 && field <= 15) {
      // 非数字字段（state 是字母）按 0 处理即可，我们只关心 14/15。
      std::uint64_t v = 0;
      bool num = true;
      for (const char c : tok) {
        if (c < '0' || c > '9') {
          num = false;
          break;
        }
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
      }
      vals[field] = num ? v : 0;
    }
    ++field;
  }
  utime = vals[14];
  stime = vals[15];
  return true;
}

inline long clock_ticks_per_sec() {
  const long t = ::sysconf(_SC_CLK_TCK);
  return t > 0 ? t : 100;
}

inline bool read_text_file(const std::string& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::stringstream buf;
  buf << in.rdbuf();
  out = buf.str();
  return true;
}

// 一个进程级的采样点。cpu_percent 需要两次采样才能算出来，所以第一次是 0（cpu_sampled=false）。
struct ProcSample {
  std::string proc;
  pid_t pid = 0;
  bool alive = false;
  std::uint64_t rss_bytes = 0;
  double cpu_percent = 0.0;
  bool cpu_sampled = false;  // false = 还没有上一拍，cpu_percent 是占位 0
  std::string note;
};

// 采样器：按 (proc,pid) 记住上一拍的 CPU 滴答与时刻。
class ProcSampler {
 public:
  ProcSampler() = default;

  ProcSample sample(const std::string& proc, pid_t pid) {
    ProcSample out;
    out.proc = proc;
    out.pid = pid;
    if (pid <= 0) {
      out.note = "进程未运行（pid 未知/为 0）";
      return out;
    }
    std::string status;
    std::string stat;
    if (!read_text_file("/proc/" + std::to_string(pid) + "/status", status) ||
        !read_text_file("/proc/" + std::to_string(pid) + "/stat", stat)) {
      out.note = "读不到 /proc/" + std::to_string(pid) + "（进程可能刚退出）";
      return out;
    }
    std::uint64_t rss = 0;
    if (!parse_proc_status_rss(status, rss)) {
      out.note = "status 里没有 VmRSS（僵尸进程？）";
      return out;
    }
    out.alive = true;
    out.rss_bytes = rss;

    std::uint64_t u = 0, s = 0;
    if (!parse_proc_stat_ticks(stat, u, s)) {
      out.note = "stat 解析失败，CPU 不可用";
      return out;
    }
    const std::uint64_t ticks = u + s;
    const int64_t now = cue::now_us();
    const std::string key = proc + ":" + std::to_string(pid);
    const auto it = prev_.find(key);
    if (it != prev_.end() && now > it->second.at_us) {
      const double dt_s = static_cast<double>(now - it->second.at_us) / 1e6;
      const std::uint64_t dt = ticks >= it->second.ticks ? ticks - it->second.ticks : 0;
      out.cpu_percent = static_cast<double>(dt) / static_cast<double>(clock_ticks_per_sec()) / dt_s * 100.0;
      out.cpu_sampled = true;
    }
    prev_[key] = Prev{ticks, now};
    return out;
  }

 private:
  struct Prev {
    std::uint64_t ticks = 0;
    int64_t at_us = 0;
  };
  std::map<std::string, Prev> prev_;
};

// ---------------------------------------------------------------- 指标输入/输出

struct ToolCallCount {
  std::string tool;
  std::string risk;
  std::string result;
  std::uint64_t count = 0;
};

struct MetricsInput {
  std::string proc = "cue-serve";
  std::int64_t ts_us = 0;
  // 管线快照（无 run / 管线没起时用默认值：alive=false、所有计数 0、分位无样本）
  cue::pipeline::PipelineMetrics pipe{};
  cue::pipeline::ProbeMetrics probe{};
  // probe.metric 事件（nack_sent / fec_recovered 只在这条事件的字段里；ProbeMetrics 不含）
  std::optional<nlohmann::json> probe_event;
  std::vector<ToolCallCount> tool_calls;
  // 最近一次评测 run 的 case 统计（scored / run_error）；nullopt = 没有 run
  std::optional<nlohmann::json> eval_cases;
  std::vector<ProcSample> procs;
  std::size_t log_queue_depth = 0;
  std::uint64_t log_dropped_debug = 0;
  std::size_t connections = 0;
  std::size_t sse_clients = 0;
};

namespace detail {

// 分位：ms → us；无样本写 0 并带 note（"0 微秒"与"没采到"必须能区分）。
inline nlohmann::json p99_us_entry(const char* stage, double p99_ms,
                                   const char* no_sample_note = "该段无 p99 样本（cued 未上报或转码关闭）") {
  nlohmann::json j{{"stage", stage}};
  if (cue::pipeline::has_sample(p99_ms)) {
    j["value"] = static_cast<std::int64_t>(p99_ms * 1000.0);
  } else {
    j["value"] = 0;
    j["note"] = no_sample_note;
  }
  return j;
}

inline nlohmann::json drop_entry(const char* reason, std::uint64_t value, const char* note = nullptr) {
  nlohmann::json j{{"reason", reason}, {"value", value}};
  if (note != nullptr) j["note"] = note;
  return j;
}

}  // namespace detail

// 生成 /metrics 的响应体。纯函数（除了调用方已采好的输入），不读 /proc、不碰日志。
inline nlohmann::json build_metrics_json(const MetricsInput& in) {
  using detail::drop_entry;
  using detail::p99_us_entry;
  const auto& m = in.pipe;
  const auto& p = in.probe;

  nlohmann::json metrics = nlohmann::json::object();

  // 1) agent 轮次：cue-serve 自己不跑 agent 会话，所以永远是 0 —— 但"0"必须带口径，
  //    否则看板会把"没有 run"读成"跑了 0 轮"。
  metrics["cue_agent_turns_total"] = {
      {"run_id", nullptr},
      {"value", 0},
      {"note", "cue-serve 不跑 agent 会话；无 run 时为 0（轮次统计在 cue-eval 的 report.json 里）"}};

  // 2) 工具调用（按 tool/risk/result 分桶）：来自控制台的累计计数
  nlohmann::json calls = nlohmann::json::array();
  for (const auto& c : in.tool_calls) {
    calls.push_back({{"tool", c.tool}, {"risk", c.risk}, {"result", c.result}, {"value", c.count}});
  }
  metrics["cue_tool_calls_total"] = std::move(calls);

  // 3) LLM token / 4) 成本：cue-serve 不调模型，恒 0（固定两个 kind，不省略）
  metrics["cue_llm_tokens_total"] = nlohmann::json::array(
      {nlohmann::json{{"kind", "prompt"}, {"value", 0}},
       nlohmann::json{{"kind", "completion"}, {"value", 0}}});
  metrics["cue_llm_cost_yuan_total"] = 0.0;

  // 5) 管线 p99（微秒）：recv/buffer 只有 p50，没有 p99 → 写 0 并注明
  nlohmann::json p99s = nlohmann::json::array();
  p99s.push_back(p99_us_entry("recv", cue::pipeline::kNoSample,
                              "cued 的 METRICS 行没有 recv 段 p99（只上报 p50），恒为 0"));
  p99s.push_back(p99_us_entry("buffer", cue::pipeline::kNoSample,
                              "cued 的 METRICS 行没有 buffer 段 p99（只上报 p50），恒为 0"));
  p99s.push_back(p99_us_entry("decode", m.decode_p99));
  p99s.push_back(p99_us_entry("op", m.op_p99));
  p99s.push_back(p99_us_entry("encode", m.encode_p99));
  p99s.push_back(p99_us_entry("send", m.send_p99));
  metrics["cue_pipeline_p99_us"] = std::move(p99s);

  // 6) 丢帧（按原因）：injected=注入的丢包、budget=令牌桶、queue_backlog=排队策略主动丢、
  //    ingress_late=迟到帧、decode=解码器未消费。五个原因都列出来，0 也是读数。
  nlohmann::json drops = nlohmann::json::array();
  drops.push_back(drop_entry("injected", m.dropped));
  drops.push_back(drop_entry("budget", m.budget_drops));
  drops.push_back(drop_entry("queue_backlog", m.queue_dropped));
  drops.push_back(drop_entry("ingress_late", m.ingress_late));
  drops.push_back(drop_entry("decode", m.decode_drops));
  metrics["cue_pipeline_drop_frames_total"] = std::move(drops);

  // 7) NACK 发送数 / 8) FEC 恢复数：只有 probe.metric 事件里带着这两个计数。
  auto from_probe_event = [&in](const char* key) -> std::uint64_t {
    if (!in.probe_event || !in.probe_event->is_object()) return 0;
    const auto it = in.probe_event->find(key);
    if (it == in.probe_event->end() || !it->is_number_integer()) return 0;
    const auto v = it->get<std::int64_t>();
    return v > 0 ? static_cast<std::uint64_t>(v) : 0;
  };
  metrics["cue_probe_nack_sent_total"] = from_probe_event("nack_sent");
  metrics["cue_probe_fec_recovered_total"] = from_probe_event("fec_recovered");

  // 9) 评测 case（按 status）：来自最近一个 run 的 report.json；没有 run 时两个 status 都写 0
  nlohmann::json cases = nlohmann::json::array();
  if (in.eval_cases) {
    cases.push_back({{"status", "scored"},
                     {"value", in.eval_cases->value("scored", std::uint64_t{0})}});
    cases.push_back({{"status", "run_error"},
                     {"value", in.eval_cases->value("run_error", std::uint64_t{0})}});
  } else {
    cases.push_back({{"status", "scored"}, {"value", 0}, {"note", "没有评测 run（无 run 时为 0）"}});
    cases.push_back({{"status", "run_error"}, {"value", 0}, {"note", "没有评测 run（无 run 时为 0）"}});
  }
  metrics["cue_eval_cases_total"] = std::move(cases);

  // 10) 各进程 RSS：cued / cue-probe / cue-serve 三个进程都列出来，pid 未知写 null
  nlohmann::json rss = nlohmann::json::array();
  nlohmann::json cpu = nlohmann::json::array();
  for (const auto& s : in.procs) {
    rss.push_back({{"proc", s.proc},
                   {"pid", s.alive ? nlohmann::json(static_cast<std::int64_t>(s.pid))
                                   : nlohmann::json(nullptr)},
                   {"value", s.alive ? s.rss_bytes : std::uint64_t{0}}});
    nlohmann::json c{{"proc", s.proc},
                     {"pid", s.alive ? nlohmann::json(static_cast<std::int64_t>(s.pid))
                                     : nlohmann::json(nullptr)},
                     {"value", s.alive ? s.cpu_percent : 0.0},
                     {"note", s.cpu_sampled ? "两次 /proc/<pid>/stat 采样差分（单核百分比，可 >100）"
                                            : "首拍没有上一拍可比，CPU 占位 0"}};
    cpu.push_back(std::move(c));
  }
  metrics["cue_process_rss_bytes"] = std::move(rss);
  metrics["cue_process_cpu_percent"] = std::move(cpu);

  // FR-OBS.3 的另外两类：队列深度与连接数（文档点名"进程级：RSS、CPU、队列深度、连接数"）
  metrics["cue_log_queue_depth"] = in.log_queue_depth;
  metrics["cue_log_dropped_debug_total"] = in.log_dropped_debug;
  metrics["cue_serve_connections"] = in.connections;
  metrics["cue_serve_sse_clients"] = in.sse_clients;

  nlohmann::json out;
  out["ok"] = true;
  out["ts_us"] = in.ts_us;
  out["proc"] = in.proc;
  out["_note"] =
      "无 run 时 cue_agent_turns_total / cue_llm_tokens_total / cue_llm_cost_yuan_total / "
      "cue_eval_cases_total 为 0（是「没有 run」，不是「测出来是 0」）；p99 单位微秒，"
      "无样本的段写 0 并带 note。";
  out["pipeline"] = {{"alive", m.alive},
                     {"pid", m.pid == 0 ? nlohmann::json(nullptr)
                                        : nlohmann::json(static_cast<std::int64_t>(m.pid))},
                     {"samples", m.samples},
                     {"probe_alive", p.alive},
                     {"probe_samples", p.samples}};
  if (!p.unavailable_reason.empty()) out["pipeline"]["probe_reason"] = p.unavailable_reason;
  out["metrics"] = std::move(metrics);
  return out;
}

// 只认整数（report.json 里 scored / run_errors 是整数；null 或缺字段一律按 0）。
inline std::uint64_t json_uint(const nlohmann::json& j, const char* key) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_number_integer()) return 0;
  const auto v = it->get<std::int64_t>();
  return v > 0 ? static_cast<std::uint64_t>(v) : 0;
}

// 最近一个评测 run 的 case 计数（runs/<run-*>/report.json 的 scored / run_errors）。
// 没有 run、读不出来都返回 nullopt —— 调用方据此写 0 并注明"没有 run"。
inline std::optional<nlohmann::json> latest_eval_cases(const std::string& runs_dir) {
  namespace fs = std::filesystem;
  std::error_code ec;
  if (!fs::is_directory(runs_dir, ec)) return std::nullopt;
  std::string best;
  for (const auto& de : fs::directory_iterator(runs_dir, fs::directory_options::skip_permission_denied,
                                               ec)) {
    if (!de.is_directory(ec)) continue;
    const std::string name = de.path().filename().string();
    if (name.rfind("run-", 0) != 0) continue;
    // run id 是 "run-<unix_us>"，位数相同 → 字典序与时间序一致
    if (name > best) {
      std::error_code ec2;
      if (fs::is_regular_file(de.path() / "report.json", ec2)) best = name;
    }
  }
  if (best.empty()) return std::nullopt;
  std::string text;
  if (!read_text_file((fs::path(runs_dir) / best / "report.json").string(), text)) return std::nullopt;
  nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return std::nullopt;
  nlohmann::json out = nlohmann::json::object();
  out["run_id"] = best;
  out["scored"] = json_uint(j, "scored");
  out["run_error"] = json_uint(j, "run_errors");
  return out;
}

}  // namespace cue::console
