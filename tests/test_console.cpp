// M5 控制台纯逻辑单测：SSE 环形缓冲/补齐、审批队列（批准/拒绝/超时/关闭）、
// report.md 解析与 runs 索引。HTTP/SSE 端到端走 scripts/e2e-console.cjs（浏览器），
// 这里只覆盖「不依赖 socket」的部分，保证 ctest 能在无端口环境下跑。
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "console/approval_queue.h"
#include "console/report_index.h"
#include "console/session_index.h"
#include "console/sse_hub.h"

namespace fs = std::filesystem;
namespace cow = cue::console;

namespace {

int failures = 0;
void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

// ---------------------------------------------------------------- SseHub

void test_hub() {
  cow::SseHub hub(3);
  check(hub.last_id() == 0, "hub: 初始 id 为 0（Last-Event-ID 缺省语义）");

  std::vector<std::string> got;
  const uint64_t sid = hub.subscribe([&got](const std::string& s) { got.push_back(s); });

  const uint64_t id1 = hub.publish("metric.sample", {{"fps", 30.1}});
  const uint64_t id2 = hub.publish("tool.call", {{"tool", "inject_fault"}});
  const uint64_t id3 = hub.publish("approval.request", {{"id", 7}});
  check(id1 == 1 && id2 == 2 && id3 == 3, "hub: id 单调从 1 开始");
  check(got.size() == 3, "hub: 订阅者收到每条事件");
  check(got[0].find("data: ") == std::string::npos, "hub: 回调给的是裸 JSON 而不是 SSE 帧");
  check(got[0].find("\"ev\":\"metric.sample\"") != std::string::npos, "hub: payload 带 ev 字段");

  // 补齐：id > last_id 的帧
  auto replay = hub.replay_after(1);
  check(replay.size() == 2, "hub: replay_after(1) 给出后两条");
  check(replay[0].find("id: 2") == 0, "hub: 补齐帧以 id 开头");
  check(replay[1].find("\"id\":3") != std::string::npos, "hub: 补齐帧内含 id");
  check(hub.replay_after(3).empty(), "hub: 已追上时无补齐");

  // 环形缓冲上限 3：再发两条，最旧的被淘汰，补齐只能给现存最旧
  hub.publish("metric.sample", {{"fps", 29.0}});
  hub.publish("metric.sample", {{"fps", 28.0}});
  auto r2 = hub.replay_after(0);
  check(r2.size() == 3, "hub: 环形缓冲不超过 cap");
  check(r2.back().find("\"fps\":28.0") != std::string::npos, "hub: 最新事件在末尾");

  auto recent = hub.recent(2);
  check(recent.size() == 2, "hub: recent(n) 截取最近 n 条");
  check(recent.back()["data"]["fps"] == 28.0, "hub: recent 解析回 JSON");

  hub.unsubscribe(sid);
  hub.publish("metric.sample", {{"fps", 27.0}});
  check(got.size() == 5, "hub: 退订后不再收到（退订前共 5 条）");

  // 关闭：订阅者收到空串哨兵，后续 publish 只留档不广播
  bool closed_seen = false;
  const uint64_t sid2 = hub.subscribe([&closed_seen](const std::string& s) {
    if (s.empty()) closed_seen = true;
  });
  hub.shutdown();
  check(closed_seen, "hub: shutdown 通知订阅者");
  check(hub.closed(), "hub: closed() 为 true");
  hub.unsubscribe(sid2);

  // 关闭后仍编号（前端不会看到 id 回退）
  const uint64_t after = hub.publish("metric.sample", {{"fps", 1.0}});
  check(after == 7, "hub: 关闭后 id 继续递增");
}

// ---------------------------------------------------------------- 审批队列

cow::ApprovalQueue::Item make_item(const char* tool) {
  cow::ApprovalQueue::Item it;
  it.tool = tool;
  it.risk = "dangerous";
  it.args = {{"kind", "drop"}, {"value", 5.0}};
  return it;
}

void test_approval() {
  {
    cow::ApprovalQueue q;
    bool requested = false;
    cow::ApprovalQueue::Outcome out;
    std::thread waiter([&] {
      out = q.request(make_item("inject_fault"), std::chrono::seconds(5),
                      [&requested](const cow::ApprovalQueue::Item& it) {
                        requested = true;
                        check(it.id == 1, "approval: 入队 id 从 1 开始");
                        check(it.risk == "dangerous", "approval: 携带风险级");
                      });
    });
    // 等它入队
    for (int i = 0; i < 200 && q.pending().empty(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(requested, "approval: on_request 回调被调用（SSE 推 approval.request）");
    check(q.pending().size() == 1, "approval: 待审批列表有 1 条");
    check(q.decide(1, true), "approval: 批准成功");
    waiter.join();
    check(out.decision == cow::ApprovalQueue::Decision::Allowed, "approval: 等待方拿到 Allowed");
    check(q.pending().empty(), "approval: 决定后离开待审批列表");
    check(q.recent(5).size() == 1 && q.recent(5)[0].outcome == "allowed", "approval: 历史记录 allowed");
    check(!q.decide(1, false), "approval: 重复决定返回 false（HTTP 409）");
    check(!q.decide(99, true), "approval: 未知 id 返回 false");
  }
  {
    // 拒绝
    cow::ApprovalQueue q;
    cow::ApprovalQueue::Outcome out;
    std::thread waiter([&] {
      out = q.request(make_item("inject_fault"), std::chrono::seconds(5), nullptr);
    });
    for (int i = 0; i < 200 && q.pending().empty(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    q.decide(1, false);
    waiter.join();
    check(out.decision == cow::ApprovalQueue::Decision::Denied, "approval: 拒绝时拿到 Denied");
  }
  {
    // 超时自动拒绝（用 80ms 模拟 30s 口径）
    cow::ApprovalQueue q;
    const auto t0 = std::chrono::steady_clock::now();
    const auto out = q.request(make_item("inject_fault"), std::chrono::milliseconds(80), nullptr);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    check(out.decision == cow::ApprovalQueue::Decision::Timeout, "approval: 超时 = 拒绝");
    check(elapsed >= std::chrono::milliseconds(70), "approval: 真的等到了超时");
    check(q.pending().empty(), "approval: 超时后不留待审批项");
    check(q.recent(5).size() == 1 && q.recent(5)[0].outcome == "timeout", "approval: 历史记录 timeout");
  }
  {
    // 进程退出：等待者立即醒来按拒绝处理（否则 join 卡到超时）
    cow::ApprovalQueue q;
    cow::ApprovalQueue::Outcome out;
    const auto t0 = std::chrono::steady_clock::now();
    std::thread waiter([&] {
      out = q.request(make_item("run_shell"), std::chrono::seconds(30), nullptr);
    });
    for (int i = 0; i < 200 && q.pending().empty(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    q.shutdown();
    waiter.join();
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    check(out.decision == cow::ApprovalQueue::Decision::Shutdown, "approval: shutdown 唤醒等待者");
    check(elapsed < std::chrono::seconds(5), "approval: shutdown 不必等满超时");
  }
}

// ---------------------------------------------------------------- report 解析

const char* kReport = R"(# Cue 评测报告

- run_id: `run-123`
- suite: `suites/m4-diagnosis.json`
- agent: `scripted`，repeat=2，warmup=6s
- 指纹: `deadbeef`（审计用）
- 生效配置: cued=`./build/cued`，model=`deepseek-chat`，成本闸门 ¥5，单价 ¥2/Mtok
- 用例: 计划 2，执行 2，计分 1，运行失败 1
- 成本: ¥0.128

## 分层成功率

| 指标 | 值 |
|---|---|
| 根因命中 | 100% |
| 证据成立 | 50% |
| SLA 达成 | 100% |
| pass^2 | 50% |
| Run Error 率 | 0% |
| 平均 MTTR | 12.5s |

## 逐用例

| case | root | evidence | sla | root_cause | turns |
|---|---|---|---|---|---|
| netloss-5pct | ✓ | ✓ | ✓ | network_loss | 6 |
| bogus-evidence | ✓ | ✗ | ✓ | network_loss | 9 |

> 末尾说明行不应被当成表格
)";

void test_report() {
  const auto r = cow::parse_report(kReport);
  check(r["run_id"] == "run-123", "report: run_id");
  check(r["suite"] == "suites/m4-diagnosis.json", "report: suite");
  check(r["agent"] == "scripted", "report: agent");
  check(r["repeat"] == 2, "report: repeat 从 agent 行解析");
  check(r["model"] == "deepseek-chat", "report: model 从生效配置行解析");
  check(r["cost_yuan"] == 0.128, "report: 成本");
  check(r["cases"]["planned"] == 2 && r["cases"]["scored"] == 1 && r["cases"]["error"] == 1,
        "report: 用例计数");
  check(r["rate_map"]["root"] == "100%" && r["rate_map"]["evidence"] == "50%",
        "report: 分层成功率按行名映射");
  check(r["rate_map"]["pass_k"] == "50%", "report: pass^k 命名不写死 k");
  check(r["rate_map"]["mttr"] == "12.5s", "report: MTTR");
  check(r["case_rows"].size() == 2, "report: 两条用例行");
  check(r["case_rows"][0]["ok"] == true, "report: 通过的 case ok=true");
  check(r["case_rows"][1]["ok"] == false, "report: 证据未成立的 case ok=false");
  check(r["failed"].size() == 1 && r["failed"][0] == "bogus-evidence", "report: 失败用例列表");
  check(r["ok"] == false, "report: 有失败用例时整份报告 ok=false");

  // 空报告 / 垃圾输入不崩、字段给 null 而不是编造 0
  const auto empty = cow::parse_report("");
  check(empty["cost_yuan"].is_null(), "report: 空报告成本是 null");
  check(empty["case_rows"].empty(), "report: 空报告无用例行");
  check(empty["ok"] == true, "report: 空报告不误报失败");
}


// 失败模式 / 稳定性（FR-UI.5）：报告里是列表项而不是表格，且「运行状态备注」要分开
void test_report_failure_modes() {
  const char* md = R"(# Cue 评测报告

- run_id: `run-fm`
- 稳定性: repeat=1，**无稳定性信息**（只有 1 轮样本）

## 失败模式

- evidence 无法在观测中找到 (2)
- root_cause 不匹配：期望 network_loss (1)

**运行状态备注（不是失败模式，不计入上面任何一类）**：

- 探针不可用：probe_unavailable (1)
)";
  const auto r = cow::parse_report(md);
  check(r["stability"] == "repeat=1，**无稳定性信息**（只有 1 轮样本）", "report: 稳定性原样保留");
  check(r["failure_modes"].size() == 2, "report: 失败模式两条");
  check(r["failure_modes"][0]["text"] == "evidence 无法在观测中找到", "report: 失败模式描述（去掉计数）");
  check(r["failure_modes"][0]["count"] == 2, "report: 失败模式计数");
  check(r["failure_modes"][1]["text"] == "root_cause 不匹配：期望 network_loss", "report: 带冒号的描述不被当成 key: value");
  check(r["failure_modes"][1]["count"] == 1, "report: 第二条计数");
  check(r["run_notes"].size() == 1, "report: 运行状态备注单独收集");
  check(r["run_notes"][0]["text"] == "探针不可用：probe_unavailable", "report: 备注文本");

  // 当前报告版式：## 失败模式 TOP3 下的表格
  const char* md_table = R"(# Cue 评测报告

- run_id: `run-fm2`

## 失败模式 TOP3

> 口径：……

| # | 根因 | miss | 例子 |
|---|---|---|---|
| 1 | network_loss | 2 | `netloss-5pct（evidence）` |
| 2 | __run__（运行错误，未判分） | 1 | `bogus-case` |

共 5 类失败模式，此处只列前 2 类（完整分布见 report.json 的 `failure_modes`）。

## 回归清单

本次没有 baseline，跳过。
)";
  const auto rt = cow::parse_report(md_table);
  check(rt["failure_modes"].size() == 2, "report: TOP3 表格解析出两条失败模式");
  check(rt["failure_modes"][0]["text"] == "network_loss", "report: 失败模式表格-根因列");
  check(rt["failure_modes"][0]["count"] == 2, "report: 失败模式表格-miss 列转数字");
  check(rt["failure_modes"][0]["example"] == "netloss-5pct（evidence）", "report: 失败模式表格-例子列（去反引号）");
  check(rt["failure_modes"][1]["count"] == 1, "report: run_error 组也保留（报告注明了它未判分）");
}

void test_scan_runs() {
  const fs::path root = fs::temp_directory_path() / ("cue-console-test-" + std::to_string(::getpid()));
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "run-a", ec);
  fs::create_directories(root / "run-b", ec);
  fs::create_directories(root / "not-a-run", ec);
  {
    std::ofstream(root / "run-a" / "report.md") << kReport;
    std::ofstream(root / "run-b" / "report.md")
        << "# Cue 评测报告\n\n- run_id: `run-b`\n- 成本: ¥0.5\n\n## 逐用例\n\n| case | root | evidence | sla |\n|---|---|---|---|\n| only-case | ✓ | ✓ | ✓ |\n";
    std::ofstream(root / "not-a-run" / "report.md") << "ignored";
  }
  const auto out = cow::scan_runs(root.string(), 10);
  check(out["runs"].size() == 2, "scan: 只收 run- 前缀且含 report.md 的目录");
  bool saw_a = false, saw_b = false;
  for (const auto& r : out["runs"]) {
    if (r["run_id"] == "run-123") saw_a = true;
    if (r["run_id"] == "run-b") saw_b = true;
  }
  check(saw_a && saw_b, "scan: 两个 run 都解析出来");
  fs::remove_all(root, ec);

  const auto missing = cow::scan_runs((root / "nope").string(), 10);
  check(missing.contains("error"), "scan: 目录不存在时给 error 而不是空数组装没事");
}


// ---------------------------------------------------------------- 会话视图（FR-UI.3）

void test_session_index() {
  namespace fs = std::filesystem;
  const fs::path root =
      fs::temp_directory_path() / ("cue-session-idx-" + std::to_string(::getpid()));
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "run-x" / "sessions", ec);
  const fs::path f = root / "run-x" / "sessions" / "s1.jsonl";

  {
    std::ofstream out(f, std::ios::binary);
    out << R"({"t":"meta","format":"cue-agent-session/1","session_id":"s1","run_id":"run-x","started_us":123,"agent_version":"cue-agent/1","model":"scripted","mode":"demo","system_prompt":"sp","goal":"g"})" << "\n";
    out << R"({"t":"message","role":"system","content":"sp"})" << "\n";
    out << R"({"t":"message","role":"assistant","content":"先看指标"})" << "\n";
    out << R"({"t":"tool","turn":1,"name":"query_metrics","args":{"window_ms":5000},"ok":true,"approval_denied":false,"risk":"read-only","duration_us":13,"summary":"metrics=ok"})" << "\n";
    out << R"({"t":"observation","turn":1,"tool":"query_metrics","args":{},"text":"metrics=ok","data":{"fps":30.3},"truncated":false,"original_bytes":10})" << "\n";
    out << R"({"t":"stage","from":"collect","to":"diagnose","allowed":true,"reason":"","turns":1,"cost_yuan":0.0001,"wall_us":100,"used_observe":true,"used_mutating":false,"consecutive_rejects":0,"diagnosis":null})" << "\n";
    out << R"({"t":"stage","from":"diagnose","to":"repair","allowed":true,"reason":"","turns":2,"cost_yuan":0.0002,"wall_us":200,"used_observe":true,"used_mutating":false,"consecutive_rejects":0,"diagnosis":{"root_cause":"network_loss","confidence":0.8,"evidence":["fps=24.1"]}})" << "\n";
    out << R"({"t":"budget","turns":2,"cost_yuan":0.0002,"wall_us":210,"used_observe":true,"used_mutating":false,"consecutive_rejects":0})" << "\n";
    out << R"({"t":"weird","payload":1})" << "\n";
    out << R"({"t":"end","final_stage":"done","kind":"done","reason":"","turns":3,"cost_yuan":0.0003,"wall_us":300,"resumable":false})" << "\n";
    // 崩溃截断的末行：只跳过、必须有 warning（只读视图不因此失败）
    out << R"({"t":"mess)";
  }
  // 锁文件不是会话：不能被当成一条记录列出来
  std::ofstream(f.string() + ".lock") << "";

  const auto list = cow::scan_sessions(root.string(), 10);
  check(list["sessions"].size() == 1, "sessions: 只收 *.jsonl（.lock 不算）");
  const auto& s = list["sessions"][0];
  check(s["session_id"] == "s1" && s["run_id"] == "run-x", "sessions: id/run 来自 meta");
  check(s["final_stage"] == "done", "sessions: final_stage 来自 end 记录");
  check(s["turns"] == 3, "sessions: turns 来自 end 记录");
  check(s["ended"] == true, "sessions: kind=done 视为已结束");
  check(s["bytes"].get<std::size_t>() > 0 && s["mtime_us"].get<std::int64_t>() > 0,
        "sessions: 带文件大小与 mtime");
  check(s["bad_lines"] == 1, "sessions: 残行计入 bad_lines");

  auto path = cow::find_session_path(root.string(), "s1");
  check(path.has_value(), "find: 按 session_id 找到");
  auto path2 = cow::find_session_path(root.string(), "run-x/s1");
  check(path2.has_value() && *path2 == *path, "find: run_id/session_id 精确写法等价");
  check(!cow::find_session_path(root.string(), "../../etc/passwd").has_value(),
        "find: 含 .. 的 id 一律拒");
  check(!cow::find_session_path(root.string(), "/etc/passwd").has_value(),
        "find: 绝对路径 id 一律拒");
  check(!cow::find_session_path(root.string(), "nope").has_value(), "find: 不存在的 id 报错");
  check(!cow::find_session_path(root.string(), "").has_value(), "find: 空 id 报错");

  const auto v = cow::read_session_view(*path);
  check(v["ok"] == true, "view: ok");
  check(v["meta"]["model"] == "scripted", "view: meta 原样保留");
  check(v["messages"].size() == 2, "view: message 分组");
  check(v["tools"].size() == 1 && v["tools"][0]["name"] == "query_metrics", "view: tool 分组带工具名");
  check(v["tools"][0]["risk"] == "read-only", "view: tool 保留风险级");
  check(v["observations"].size() == 1 && v["observations"][0]["data"]["fps"] == 30.3,
        "view: observation 分组带 data");
  check(v["stages"].size() == 2, "view: stage 分组");
  check(v["stages"][1]["diagnosis"]["evidence"][0] == "fps=24.1", "view: 诊断证据原样保留");
  check(v["budgets"].size() == 1 && v["ends"].size() == 1, "view: budget/end 分组");
  check(v["other"].size() == 1, "view: 未知记录类型进 other（不丢）");
  check(!v["warnings"].empty(), "view: 残行/未知类型给 warning");
  check(v["counts"]["message"] == 2, "view: counts 按类型计数");
  // 分组是为了渲染方便，顺序是事实：每类记录都带 seq（文件行号）
  check(v["messages"][0]["seq"] == 1, "view: 记录带 seq（第 2 行 → seq=1）");
  std::vector<int> seqs;
  for (const char* k : {"messages", "tools", "observations", "stages", "budgets", "ends"}) {
    for (const auto& r : v[k]) seqs.push_back(r["seq"].get<int>());
  }
  std::sort(seqs.begin(), seqs.end());
  check(std::adjacent_find(seqs.begin(), seqs.end()) == seqs.end(), "view: 各组 seq 互不重复");

  fs::remove_all(root, ec);
}

}  // namespace

int main() {
  test_hub();
  test_approval();
  test_report();
  test_report_failure_modes();
  test_scan_runs();
  test_session_index();
  if (failures == 0) {
    std::printf("test_console: all checks passed\n");
    return 0;
  }
  std::printf("test_console: %d failure(s)\n", failures);
  return 1;
}
