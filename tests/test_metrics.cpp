// /metrics 生成器（FR-OBS.3 / 技术设计 §12.3）+ 结构化日志的字段集（FR-OBS.1）。
//
// 都是"纯函数化"的断言：不真起 HTTP 服务，也不真去读某个活进程的 /proc——
// 那种测试在 CI 里既慢又不稳，还会因为"本机刚好没起 cued"而跳过关键分支。
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#include <nlohmann/json.hpp>

#include "console/metrics_report.h"
#include "cue/common/log.h"
#include "cue/common/metrics.h"

namespace {

int failures = 0;

void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

namespace fs = std::filesystem;
using nlohmann::json;

const char* kProcStatus = R"(Name:	cued
Umask:	0022
State:	S (sleeping)
Tgid:	4242
VmPeak:	  921600 kB
VmSize:	  899072 kB
VmHWM:	  512000 kB
VmRSS:	  262144 kB
RssAnon:	  200000 kB
Threads:	12
)";

// /proc/<pid>/stat 的真实形状：comm 里带空格/括号时也必须从**最后一个** ')' 之后数。
// 字段 14/15 = utime=11、stime=22。
const char* kProcStat =
    "4242 (cued worker (x)) S 1 4242 4242 0 -1 4194560 1000 0 0 0 11 22 0 0 20 0 3 0 12345 1 2\n";

std::size_t argb_of(const json& arr, const char* key, const std::string& label) {
  for (const auto& it : arr) {
    if (it.value(key, std::string()) == label) return it.value("value", std::size_t{0});
  }
  return static_cast<std::size_t>(-1);
}

json find_labeled(const json& arr, const char* key, const std::string& label) {
  for (const auto& it : arr) {
    if (it.value(key, std::string()) == label) return it;
  }
  return json();
}

}  // namespace

int main() {
  using cue::console::build_metrics_json;
  using cue::console::MetricsInput;
  using cue::console::ProcSample;
  using cue::console::ToolCallCount;

  // ---------------------------------------------------------------- /proc 解析
  {
    std::uint64_t rss = 0;
    check(cue::console::parse_proc_status_rss(kProcStatus, rss), "VmRSS 能被解析");
    check(rss == 262144ull * 1024ull, "VmRSS 262144 kB → 268435456 字节");
    check(!cue::console::parse_proc_status_rss("Name:\tfoo\n", rss), "没有 VmRSS 字段返回 false");

    std::uint64_t utime = 0, stime = 0;
    check(cue::console::parse_proc_stat_ticks(kProcStat, utime, stime), "stat 能被解析");
    check(utime == 11, "stat 的 utime（第 14 字段）= 11");
    check(stime == 22, "stat 的 stime（第 15 字段）= 22");
    check(!cue::console::parse_proc_stat_ticks("garbage", utime, stime), "坏行返回 false");
  }

  // ---------------------------------------------------------------- 10 项指标一个不少
  {
    MetricsInput in;
    in.ts_us = 1'700'000'000'000'000;
    in.pipe.alive = true;
    in.pipe.pid = 4242;
    in.pipe.samples = 12;
    in.pipe.decode_p99 = 12.5;   // ms
    in.pipe.op_p99 = 0.5;
    in.pipe.encode_p99 = 30.0;
    in.pipe.send_p99 = 2.0;
    in.pipe.dropped = 7;
    in.pipe.budget_drops = 3;
    in.pipe.queue_dropped = 9;
    in.pipe.ingress_late = 1;
    in.pipe.decode_drops = 2;
    in.tool_calls.push_back(ToolCallCount{"pipeline_start", "mutating", "executed", 2});
    in.tool_calls.push_back(ToolCallCount{"run_shell", "dangerous", "denied", 1});
    in.probe_event = json{{"nack_sent", 42}, {"fec_recovered", 5}};
    in.eval_cases = json{{"run_id", "run-123"}, {"scored", 3}, {"run_error", 1}};
    in.procs.push_back(ProcSample{"cued", 4242, true, 268435456, 12.5, true, ""});
    in.procs.push_back(ProcSample{"cue-probe", 0, false, 0, 0.0, false, "进程未运行"});
    in.procs.push_back(ProcSample{"cue-serve", 1111, true, 1048576, 0.0, false, ""});
    in.log_queue_depth = 17;
    in.log_dropped_debug = 4;
    in.connections = 2;
    in.sse_clients = 1;

    const json out = build_metrics_json(in);
    check(out["ok"] == true, "ok=true");
    check(out["proc"] == "cue-serve", "proc 字段是 cue-serve");
    check(out["_note"].is_string() && !out["_note"].get<std::string>().empty(),
          "顶层 _note 说明「无 run 时为 0」");
    const json& m = out["metrics"];

    // 文档 §12.3 的 10 项
    for (const char* name : {"cue_agent_turns_total", "cue_tool_calls_total", "cue_llm_tokens_total",
                             "cue_llm_cost_yuan_total", "cue_pipeline_p99_us",
                             "cue_pipeline_drop_frames_total", "cue_probe_nack_sent_total",
                             "cue_probe_fec_recovered_total", "cue_eval_cases_total",
                             "cue_process_rss_bytes"}) {
      check(m.contains(name), name);
    }
    // FR-OBS.3 的"队列深度 / 连接数"
    check(m.contains("cue_log_queue_depth") && m.contains("cue_serve_connections"),
          "队列深度与连接数也在 metrics 里");

    check(m["cue_tool_calls_total"].size() == 2, "工具调用按 tool/risk/result 分桶");
    check(m["cue_tool_calls_total"][0]["value"] == 2, "pipeline_start 调用 2 次");
    check(m["cue_llm_tokens_total"].size() == 2, "token 指标固定 prompt/completion 两项");
    check(m["cue_llm_tokens_total"][0]["value"] == 0, "无 run 时 token 写 0 而不是省略");
    check(m["cue_llm_cost_yuan_total"] == 0.0, "无 run 时成本写 0");

    // p99：微秒，且没有 p99 样本的段写 0 + note
    const json& p99 = m["cue_pipeline_p99_us"];
    check(p99.size() == 6, "p99 覆盖六个阶段");
    check(find_labeled(p99, "stage", "decode")["value"] == 12500, "decode p99 12.5 ms → 12500 us");
    check(find_labeled(p99, "stage", "encode")["value"] == 30000, "encode p99 30 ms → 30000 us");
    const json recv = find_labeled(p99, "stage", "recv");
    check(recv["value"] == 0 && recv.contains("note"), "recv 段没有 p99 样本：写 0 且带 note");

    // 丢帧按原因
    const json& drops = m["cue_pipeline_drop_frames_total"];
    check(drops.size() == 5, "丢帧覆盖 5 个原因");
    check(find_labeled(drops, "reason", "injected")["value"] == 7, "injected=7");
    check(find_labeled(drops, "reason", "budget")["value"] == 3, "budget=3");
    check(find_labeled(drops, "reason", "queue_backlog")["value"] == 9, "queue_backlog=9");
    check(find_labeled(drops, "reason", "ingress_late")["value"] == 1, "ingress_late=1");
    check(find_labeled(drops, "reason", "decode")["value"] == 2, "decode=2");

    check(m["cue_probe_nack_sent_total"] == 42, "nack_sent 来自 probe.metric 事件");
    check(m["cue_probe_fec_recovered_total"] == 5, "fec_recovered 来自 probe.metric 事件");

    const json& cases = m["cue_eval_cases_total"];
    check(argb_of(cases, "status", "scored") == 3, "scored=3");
    check(argb_of(cases, "status", "run_error") == 1, "run_error=1");

    // RSS：三个进程都列出来；不在的写 pid=null、value=0
    const json& rss = m["cue_process_rss_bytes"];
    check(rss.size() == 3, "RSS 覆盖三个进程");
    const json cued_rss = find_labeled(rss, "proc", "cued");
    check(cued_rss["value"] == 268435456, "cued RSS 字节数");
    check(cued_rss["pid"] == 4242, "cued pid");
    const json probe_rss = find_labeled(rss, "proc", "cue-probe");
    check(probe_rss["value"] == 0, "探针没起：RSS 写 0");
    check(probe_rss["pid"].is_null(), "探针没起：pid 写 null（不写 0 冒充真 pid）");

    check(m["cue_log_queue_depth"] == 17, "日志队列深度");
    check(m["cue_log_dropped_debug_total"] == 4, "debug 丢弃计数");
    check(m["cue_serve_connections"] == 2, "连接数");
    check(m["cue_serve_sse_clients"] == 1, "SSE 客户端数");

    // 管线存活信息（/api/state 之外也给一份，看板不必解析两个端点）
    check(out["pipeline"]["alive"] == true && out["pipeline"]["pid"] == 4242, "pipeline 存活信息");
  }

  // ---------------------------------------------------------------- 一个 run 都没有时
  {
    MetricsInput in;
    const json out = build_metrics_json(in);
    const json& m = out["metrics"];
    check(m["cue_agent_turns_total"]["value"] == 0, "无 run：turns=0");
    check(m["cue_agent_turns_total"].contains("note"), "无 run：turns 带 note 说明为什么是 0");
    const json& cases = m["cue_eval_cases_total"];
    check(cases.size() == 2, "无 run：两个 status 都出现");
    check(cases[0].contains("note") && cases[1].contains("note"), "无 run：case 计数带 note");
    check(m["cue_tool_calls_total"].is_array() && m["cue_tool_calls_total"].empty(),
          "没有工具调用时是空数组（不编假标签）");
    check(m["cue_process_rss_bytes"].empty(), "没有采样时 RSS 是空数组");
  }

  // ---------------------------------------------------------------- 最近一个 run 的 case 计数
  {
    const fs::path root = fs::temp_directory_path() /
                          ("cue-test-metrics-" + std::to_string(::getpid()));
    const fs::path run_old = root / "run-1700000000000001";
    const fs::path run_new = root / "run-1700000000000002";
    fs::create_directories(run_new);
    fs::create_directories(run_old);
    {
      std::ofstream old_report(run_old / "report.json");
      old_report << R"({"run_id":"run-1700000000000001","scored":1,"run_errors":0})";
      std::ofstream new_report(run_new / "report.json");
      new_report << R"({"run_id":"run-1700000000000002","scored":9,"run_errors":2})";
    }
    auto cases = cue::console::latest_eval_cases(root.string());
    check(cases.has_value(), "能读到最近的 run");
    if (cases) {
      check((*cases)["run_id"] == "run-1700000000000002", "取的是最新（字典序最大的 run id）");
      check((*cases)["scored"] == 9, "scored=9");
      check((*cases)["run_error"] == 2, "report.json 的 run_errors → run_error");
    }
    std::error_code ec;
    fs::remove_all(root, ec);
    check(!cue::console::latest_eval_cases(root.string()).has_value(),
          "没有 run 目录时返回 nullopt（调用方据此写 0 + note）");
  }

  // ---------------------------------------------------------------- 日志：JSON 行字段固定
  {
    cue::log::Context ctx;
    ctx.run_id = "run-1";
    ctx.case_id = "case-1";
    ctx.session_id = "sess-1";
    const std::string line = cue::log::format_json_line(ctx, "cue-serve", cue::log::Level::Warn,
                                                        "serve", "listen", "消息里有\"引号\"\n和换行",
                                                        "", 1'700'000'000'000'000);
    check(line.find('\n') == std::string::npos, "一行一条（msg 里的换行被转义，不劈开行）");
    const json j = json::parse(line, nullptr, false);
    check(!j.is_discarded(), "行是合法 JSON");
    if (!j.is_discarded()) {
      for (const char* k : {"ts_us", "level", "proc", "run_id", "case_id", "session_id", "component",
                            "event", "msg"}) {
        check(j.contains(k), k);
      }
      check(!j.contains("err"), "没有错误文本时不写 err（可选字段）");
      check(j["ts_us"] == 1'700'000'000'000'000, "ts_us 是整数微秒");
      check(j["level"] == "warn", "level 是小写级别名");
      check(j["msg"].get<std::string>() == "消息里有\"引号\"\n和换行", "msg 原样往返（由 nlohmann 转义）");

      const std::string with_err = cue::log::format_json_line(
          ctx, "cue-eval", cue::log::Level::Error, "eval", "fatal", "失败", "EACCES", 42);
      const json je = json::parse(with_err, nullptr, false);
      check(!je.is_discarded() && je.contains("err") && je["err"] == "EACCES", "有错误时带 err");
    }
  }

  // ---------------------------------------------------------------- 日志：异步写文件 + debug 过滤
  {
    const fs::path logpath = fs::temp_directory_path() /
                             ("cue-test-log-" + std::to_string(::getpid()) + ".jsonl");
    cue::log::Options opt;
    opt.file = logpath.string();
    opt.level = "debug";
    opt.stderr_sink = false;  // 测试不要往 ctest 输出里撒日志
    auto ok = cue::log::init(opt);
    check(static_cast<bool>(ok), "日志初始化成功");
    check(cue::log::enabled(), "初始化后 enabled()");
    if (ok) {
      cue::log::info("test", "hello", "第一条");
      cue::log::debug("test", "hint", "debug 行");
      // 1 万行：异步 sink 的队列是 8192，正常会边写边排空；这里同时验证"不丢 info"。
      for (int i = 0; i < 10000; ++i) cue::log::info("test", "bulk", "n=" + std::to_string(i));
      cue::log::shutdown();
      check(!cue::log::enabled(), "shutdown 之后 enabled() 为假");

      std::ifstream in(logpath);
      std::string line;
      std::size_t total = 0, bulk = 0, bad = 0, debug_lines = 0;
      while (std::getline(in, line)) {
        ++total;
        const json j = json::parse(line, nullptr, false);
        if (j.is_discarded()) {
          ++bad;
          continue;
        }
        if (j.value("event", std::string()) == "bulk") ++bulk;
        if (j.value("level", std::string()) == "debug") ++debug_lines;
      }
      check(bad == 0, "每一行都是合法 JSON");
      check(bulk == 10000, "1 万行 info 一条不少（异步不丢非 debug）");
      check(debug_lines == 1, "--log-level debug 时能看到 debug 行");
      check(total == 10002, "总行数 = info + debug + 1 万批量");
      std::error_code ec;
      fs::remove(logpath, ec);
    }
  }

  // ---------------------------------------------------------------- debug 在 info 级被过滤
  {
    const fs::path logpath = fs::temp_directory_path() /
                             ("cue-test-log2-" + std::to_string(::getpid()) + ".jsonl");
    cue::log::Options opt;
    opt.file = logpath.string();
    opt.level = "info";
    opt.stderr_sink = false;
    if (cue::log::init(opt)) {
      cue::log::debug("test", "hint", "不该出现");
      cue::log::info("test", "hello", "该出现");
      cue::log::shutdown();
      std::ifstream in(logpath);
      std::string line;
      std::size_t debug_lines = 0, total = 0;
      while (std::getline(in, line)) {
        ++total;
        const json j = json::parse(line, nullptr, false);
        if (!j.is_discarded() && j.value("level", std::string()) == "debug") ++debug_lines;
      }
      check(debug_lines == 0, "info 级下 debug 不落盘");
      check(total == 1, "info 行照写");
      std::error_code ec;
      fs::remove(logpath, ec);
    }
  }

  // ---------------------------------------------------------------- probe 快照解析（N-01 字段）
  //
  // 三个音频恢复计数是后加的。两类输入都必须能解析：
  //   1) 旧录制（没有这三个字段）：缺省为 0，**不能**报解析失败——否则回放旧 cassette
  //      会整体挂掉，等于用"新增观测"破坏了历史数据；
  //   2) 新 probe.metric（带这三个字段）：原样读出。
  // 这是"可缺省"与别处 fail-closed 的区别，也正是这条用例存在的意义。
  {
    using cue::pipeline::ProbeMetrics;
    using cue::pipeline::probe_metrics_from_json;
    std::string err;

    const json old_snapshot = json{{"fps", 30.0},       {"frames", 100},
                                   {"decoded", 99},     {"decode_errors", 0},
                                   {"decode_skips", 0}, {"stalls", 0},
                                   {"max_stall_us", 0.0}, {"samples", 12}};
    ProbeMetrics old;
    const bool ok_old = probe_metrics_from_json(old_snapshot, old, err);
    check(ok_old, "probe 旧快照（无 audio_fec_recovered 等字段）仍能解析");
    check(old.audio_fec_recovered == 0 && old.audio_plc_frames == 0 &&
              old.audio_unconcealed_frames == 0,
          "probe 旧快照缺字段 → 三个计数缺省为 0");

    const json new_snapshot = json{{"fps", 30.0},       {"frames", 100},
                                   {"decoded", 95},     {"decode_errors", 0},
                                   {"decode_skips", 0}, {"stalls", 0},
                                   {"max_stall_us", 0.0}, {"samples", 12},
                                   {"audio_fec_recovered", 4},
                                   {"audio_plc_frames", 2},
                                   {"audio_unconcealed_frames", 1}};
    ProbeMetrics cur;
    const bool ok_new = probe_metrics_from_json(new_snapshot, cur, err);
    check(ok_new, "probe 新快照（带 N-01 字段）能解析");
    check(cur.audio_fec_recovered == 4 && cur.audio_plc_frames == 2 &&
              cur.audio_unconcealed_frames == 1,
          "probe 新快照三个计数按原值读出");

    // 类型错了仍然要报错（fail-closed 的那一半语义不能因为"可缺省"而丢掉）
    const json bad_type = json{{"fps", 30.0}, {"frames", 100}, {"decoded", 1},
                               {"audio_plc_frames", "2"}};
    ProbeMetrics bad;
    check(!probe_metrics_from_json(bad_type, bad, err),
          "probe 快照里 audio_plc_frames 是字符串 → 解析失败（不静默当 0）");
  }

  // ---------------------------------------------------------------- probe 快照解析（N-02 字段）
  //
  // FEC / NACK / 幽灵帧 / 音频帧号账是后加的观测字段（原来 probe 算了但 agent 看不见）。
  // 与 N-01 同一口径：缺字段 = 0 且合法，旧 cassette 不能因此整体解析失败；
  // 但字段存在而类型不对仍然报错（"可缺省"不等于"可以乱写"）。
  {
    using cue::pipeline::ProbeMetrics;
    using cue::pipeline::probe_metrics_from_json;
    std::string err;

    const json old_snapshot = json{{"fps", 30.0},        {"frames", 100}, {"decoded", 95},
                                   {"decode_errors", 0}, {"stalls", 0},  {"samples", 3}};
    ProbeMetrics old;
    const bool ok_old = probe_metrics_from_json(old_snapshot, old, err);
    check(ok_old, "probe 旧快照（无 N-02 字段）仍能解析");
    check(old.fec_recovered == 0 && old.fec_groups_rx == 0 && old.fec_unrecoverable == 0 &&
              old.fec_unrecoverable_pkts == 0 && old.fec_ghost_discarded == 0 &&
              old.frag_ghost_discarded == 0 && old.lost == 0 && old.nack_sent == 0 &&
              old.audio_datagrams == 0 && old.audio_seq_gaps == 0 && old.audio_dups == 0 &&
              old.audio_reordered == 0 && old.audio_stale == 0 && old.audio_decode_errors == 0,
          "probe 旧快照缺 N-02 字段 → 全部缺省为 0");

    const json new_snapshot =
        json{{"fps", 30.0},        {"frames", 100},          {"decoded", 95},
             {"decode_errors", 0}, {"stalls", 0},            {"samples", 3},
             {"fec_recovered", 37},
             {"fec_groups_rx", 118}, {"fec_unrecoverable", 1}, {"fec_unrecoverable_pkts", 3},
             {"fec_ghost_discarded", 9}, {"frag_ghost_discarded", 2}, {"lost", 4},
             {"nack_sent", 11},          {"audio_datagrams", 1500},
             {"audio_seq_gaps", 12},     {"audio_dups", 1},        {"audio_reordered", 2},
             {"audio_stale", 3},         {"audio_decode_errors", 0}};
    ProbeMetrics cur;
    const bool ok_new = probe_metrics_from_json(new_snapshot, cur, err);
    check(ok_new, "probe 新快照（带 N-02 字段）能解析");
    check(cur.fec_recovered == 37 && cur.fec_groups_rx == 118 && cur.fec_unrecoverable == 1 &&
              cur.fec_unrecoverable_pkts == 3 && cur.fec_ghost_discarded == 9 &&
              cur.frag_ghost_discarded == 2 && cur.lost == 4 && cur.nack_sent == 11 &&
              cur.audio_datagrams == 1500 && cur.audio_seq_gaps == 12 && cur.audio_dups == 1 &&
              cur.audio_reordered == 2 && cur.audio_stale == 3,
          "probe 新快照 N-02 计数按原值读出");

    // 类型错了仍然要报错：这一半语义不能因为"可缺省"而丢掉
    const json bad_type = json{{"fps", 30.0}, {"frames", 100}, {"decoded", 1},
                               {"fec_ghost_discarded", "9"}};
    ProbeMetrics bad;
    check(!probe_metrics_from_json(bad_type, bad, err),
          "probe 快照里 fec_ghost_discarded 是字符串 → 解析失败（不静默当 0）");
  }

  if (failures == 0) {
    std::printf("test_metrics: OK\n");
    return 0;
  }
  std::printf("test_metrics: %d failure(s)\n", failures);
  return 1;
}
