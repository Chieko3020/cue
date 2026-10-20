// M3a 验证：用 ScriptedClient（不碰网络、不花钱）驱动一次完整「诊断→修复→验证」，
// 断言状态机守卫、工具审批、诊断证据回填都按 SPEC 工作。
#include <cstdio>
#include <string>
#include <vector>

#include "cue/agent/agent.h"
#include "cue/llm/client.h"
#include "cue/tool/tool.h"

using json = nlohmann::json;
namespace tool = cue::tool;
namespace llm = cue::llm;
namespace agent = cue::agent;

namespace {

int failures = 0;
void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

llm::ChatResponse text(const std::string& content) {
  llm::ChatResponse r;
  r.content = content;
  r.usage = {100, 20};
  return r;
}

llm::ChatResponse call(const std::string& id, const std::string& name, const json& args) {
  llm::ChatResponse r;
  llm::ToolCall tc;
  tc.id = id;
  tc.name = name;
  tc.arguments = args.dump();
  r.tool_calls.push_back(tc);
  r.usage = {100, 20};
  return r;
}

// 一组 mock 工具：行为可观测，用于断言调用次数与参数
struct MockTools {
  int metrics_calls = 0;
  int set_param_calls = 0;
  int inject_calls = 0;
  json last_set;

  void build(tool::Registry& reg) {
    reg.add(
        tool::ToolSpec{.name = "query_metrics",
                       .desc = "查询管线指标（预聚合摘要）",
                       .risk = tool::Risk::ReadOnly,
                       .params = {{.name = "window_ms", .type = "integer", .desc = "窗口毫秒",
                                   .required = false, .enum_values = {}, .has_range = false}}},
        [this](const json&) {
          ++metrics_calls;
          json m{{"p50_ms", 31.2}, {"p95_ms", 58.9}, {"p99_ms", 181.4}, {"drop_rate", 0.0},
                 {"queue_depth", 3}};
          return tool::ToolResult{true, "metrics=" + m.dump(), m};
        });

    reg.add(
        tool::ToolSpec{.name = "pipeline_set_param",
                       .desc = "修改管线参数",
                       .risk = tool::Risk::Mutating,
                       .params = {{.name = "key",
                                   .type = "string",
                                   .desc = "参数名",
                                   .required = true,
                                   .enum_values = {"bitrate_kbps", "gop", "preset", "transcode"}},
                                  {.name = "value", .type = "string", .desc = "目标值"}}},
        [this](const json& a) {
          ++set_param_calls;
          last_set = a;
          return tool::ToolResult{true, "ok", a};
        });

    reg.add(
        tool::ToolSpec{.name = "inject_fault",
                       .desc = "注入网络损伤",
                       .risk = tool::Risk::Dangerous,
                       .params = {{.name = "kind",
                                   .type = "string",
                                   .desc = "类型",
                                   .required = true,
                                   .enum_values = {"drop", "delay", "jitter", "rate"}},
                                  {.name = "value", .type = "number", .desc = "数值"}}},
        [this](const json& a) {
          ++inject_calls;
          return tool::ToolResult{true, "injected", a};
        });

    reg.add(tool::ToolSpec{.name = "fetch_observation",
                           .desc = "获取观测",
                           .risk = tool::Risk::ReadOnly,
                           .params = {{.name = "source",
                                       .type = "string",
                                       .desc = "来源",
                                       .required = true,
                                       .enum_values = {"logs", "procs", "frames"}}}},
            [](const json& a) {
              return tool::ToolResult{true, "observation", json{{"source", a["source"]}}};
            });
  }
};

agent::Runner::Config make_cfg(tool::ApprovalMode mode) {
  agent::Runner::Config cfg;
  cfg.model = "scripted";
  cfg.system_prompt = "你是 Cue 的媒体管线运维 agent。";
  cfg.ctx.mode = mode;
  cfg.yuan_per_mtok = 2.0;
  return cfg;
}

}  // namespace

int main() {
  // ---------- 用例 1：完整诊断 → 修复 → 验证 ----------
  {
    MockTools mocks;
    tool::Registry reg;
    mocks.build(reg);

    llm::ScriptedClient llm;
    llm.push(call("c1", "query_metrics", json{{"window_ms", 5000}}));
    llm.push(call("c2", "stage_complete", json{{"next", "diagnose"}}));
    llm.push(call("c3", "stage_complete",
                  json{{"next", "repair"},
                       {"root_cause", "network_loss"},
                       {"evidence", {"nack_retransmit_count=42"}},
                       {"confidence", 0.8}}));
    llm.push(call("c4", "pipeline_set_param", json{{"key", "bitrate_kbps"}, {"value", "800"}}));
    llm.push(call("c5", "stage_complete", json{{"next", "verify"}}));
    llm.push(call("c6", "query_metrics", json{{"window_ms", 5000}}));
    llm.push(call("c7", "stage_complete", json{{"next", "done"}}));

    agent::Runner runner(llm, reg, make_cfg(tool::ApprovalMode::Auto));
    std::vector<std::string> events;
    const auto res = runner.run("把下行 p99 压到 120ms 以下", agent::Budget{},
                                [&](const agent::Event& e) { events.push_back(e.type); });

    check(res.final_stage == agent::Stage::Done, "case1 final stage done");
    check(res.diagnosis.root_cause == "network_loss", "case1 root cause");
    check(res.diagnosis.evidence.size() == 1, "case1 evidence");
    check(mocks.metrics_calls == 2, "case1 metrics called twice");
    check(mocks.set_param_calls == 1, "case1 set_param once");
    check(mocks.last_set["key"] == "bitrate_kbps", "case1 set_param key");
    check(res.turns == 7, "case1 turns");
  }

  // ---------- 用例 2：守卫拒绝（未观测就想诊断） ----------
  {
    MockTools mocks;
    tool::Registry reg;
    mocks.build(reg);

    llm::ScriptedClient llm;
    llm.push(call("d1", "stage_complete", json{{"next", "diagnose"}}));
    llm.push(call("d2", "query_metrics", json::object()));
    llm.push(call("d3", "stage_complete", json{{"next", "diagnose"}}));

    agent::Runner runner(llm, reg, make_cfg(tool::ApprovalMode::Auto));
    int rejects = 0;
    const auto res = runner.run("x", agent::Budget{},
                                [&](const agent::Event& e) {
                                  if (e.type == "guard.reject") ++rejects;
                                });
    check(rejects == 1, "case2 guard rejected once before observe");
    check(res.final_stage == agent::Stage::Diagnose, "case2 advanced after observe");
  }

  // ---------- 用例 3：危险工具在 Auto 下未授权被拒 ----------
  {
    MockTools mocks;
    tool::Registry reg;
    mocks.build(reg);

    llm::ScriptedClient llm;
    llm.push(call("e1", "inject_fault", json{{"kind", "drop"}, {"value", 5}}));
    llm.push(call("e2", "query_metrics", json::object()));

    agent::Runner runner(llm, reg, make_cfg(tool::ApprovalMode::Auto));
    int denied = 0;
    runner.run("x", agent::Budget{}, [&](const agent::Event& e) {
      if (e.type == "approval.denied") ++denied;
    });
    check(denied == 1, "case3 dangerous denied without allowlist");
    check(mocks.inject_calls == 0, "case3 inject never executed");
  }

  // ---------- 用例 4：危险工具在 allowlist 下放行 ----------
  {
    MockTools mocks;
    tool::Registry reg;
    mocks.build(reg);

    llm::ScriptedClient llm;
    llm.push(call("f1", "inject_fault", json{{"kind", "drop"}, {"value", 5}}));
    llm.push(call("f2", "query_metrics", json::object()));

    auto cfg = make_cfg(tool::ApprovalMode::Auto);
    cfg.ctx.allowed_dangerous = {"inject_fault"};
    agent::Runner runner(llm, reg, cfg);
    runner.run("x", agent::Budget{}, nullptr);
    check(mocks.inject_calls == 1, "case4 inject executed with allowlist");
  }

  // ---------- 用例 5：参数校验（枚举/范围/必填） ----------
  {
    MockTools mocks;
    tool::Registry reg;
    mocks.build(reg);
    auto r1 = reg.call("pipeline_set_param", json{{"key", "bitrate_kbps"}});  // 缺 value
    check(!r1.has_value(), "case5 missing required rejected");
    auto r2 = reg.call("pipeline_set_param", json{{"key", "bogus"}, {"value", "1"}});
    check(!r2.has_value(), "case5 enum rejected");
    auto r3 = reg.call("inject_fault", json{{"kind", "drop"}, {"value", "high"}});
    check(!r3.has_value(), "case5 type rejected");
    auto r4 = reg.call("query_metrics", json::object());
    check(r4.has_value(), "case5 valid call ok");
  }

  // ---------- 用例 6：脚本耗尽必须报错（不能静默循环） ----------
  {
    MockTools mocks;
    tool::Registry reg;
    mocks.build(reg);
    llm::ScriptedClient llm;  // 空脚本
    agent::Runner runner(llm, reg, make_cfg(tool::ApprovalMode::Auto));
    const auto res = runner.run("x", agent::Budget{}, nullptr);
    check(!res.error.empty(), "case6 empty script reports error");
    check(res.final_stage != agent::Stage::Done, "case6 must not claim done");
  }

  if (failures == 0) {
    std::printf("test_agent: OK (6 cases)\n");
    return 0;
  }
  std::printf("test_agent: %d failure(s)\n", failures);
  return 1;
}
