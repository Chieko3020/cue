// M5 控制台纯逻辑单测：SSE 环形缓冲/补齐、审批队列（批准/拒绝/超时/关闭）、
// report.md 解析与 runs 索引。HTTP/SSE 端到端走 scripts/e2e-console.cjs（浏览器），
// 这里只覆盖「不依赖 socket」的部分，保证 ctest 能在无端口环境下跑。
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

}  // namespace

int main() {
  test_hub();
  test_approval();
  test_report();
  test_scan_runs();
  if (failures == 0) {
    std::printf("test_console: all checks passed\n");
    return 0;
  }
  std::printf("test_console: %d failure(s)\n", failures);
  return 1;
}
