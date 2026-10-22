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

[[maybe_unused]] llm::ChatResponse text(const std::string& content) {
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
          return tool::ToolResult{"metrics=" + m.dump(), m};
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
                                  {.name = "value",
                                   .type = "string",
                                   .desc = "目标值",
                                   .required = true,
                                   .enum_values = {},
                                   .has_range = false,
                                   .min = 0,
                                   .max = 0}}},
        [this](const json& a) {
          ++set_param_calls;
          last_set = a;
          return tool::ToolResult{"ok", a};
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
                                  {.name = "value",
                                   .type = "number",
                                   .desc = "数值",
                                   .required = true,
                                   .enum_values = {},
                                   .has_range = false,
                                   .min = 0,
                                   .max = 0}}},
        [this](const json& a) {
          ++inject_calls;
          return tool::ToolResult{"injected", a};
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
              return tool::ToolResult{"observation", json{{"source", a["source"]}}};
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
    bool entered_diagnose = false;
    const auto res = runner.run("x", agent::Budget{},
                                [&](const agent::Event& e) {
                                  if (e.type == "guard.reject") ++rejects;
                                  if (e.type == "stage.enter" && e.data.value("stage", "") == "diagnose") {
                                    entered_diagnose = true;
                                  }
                                });
    check(rejects == 1, "case2 guard rejected once before observe");
    check(entered_diagnose, "case2 advanced to diagnose after observe");
    // 脚本耗尽后必须显式失败（不能静默停在中间态）
    check(!res.error.empty() && res.final_stage == agent::Stage::GiveUp, "case2 exhausted -> give_up");
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

  // ---------- 用例 7：dangerous 工具也算修复动作（靠取消注入完成修复） ----------
  {
    MockTools mocks;
    tool::Registry reg;
    mocks.build(reg);

    llm::ScriptedClient llm;
    llm.push(call("g1", "query_metrics", json{{"window_ms", 5000}}));
    llm.push(call("g2", "stage_complete", json{{"next", "diagnose"}}));
    llm.push(call("g3", "stage_complete",
                  json{{"next", "repair"},
                       {"root_cause", "network_loss"},
                       {"evidence", {"injected=59"}},
                       {"confidence", 0.8}}));
    llm.push(call("g4", "inject_fault", json{{"kind", "drop"}, {"value", 0}}));
    llm.push(call("g5", "stage_complete", json{{"next", "verify"}}));
    llm.push(call("g6", "query_metrics", json{{"window_ms", 5000}}));
    llm.push(call("g7", "stage_complete", json{{"next", "done"}}));

    auto cfg = make_cfg(tool::ApprovalMode::Auto);
    cfg.ctx.allowed_dangerous = {"inject_fault"};
    agent::Runner runner(llm, reg, cfg);
    int rejects = 0;
    const auto res = runner.run("x", agent::Budget{}, [&](const agent::Event& e) {
      if (e.type == "guard.reject") ++rejects;
    });
    check(rejects == 0, "case7 dangerous tool satisfies repair guard");
    check(res.final_stage == agent::Stage::Done, "case7 reaches done");
    check(mocks.inject_calls == 1, "case7 inject executed once");
  }

  // ---------- 用例 8：Ask 模式审批回调拿到「当前被调用工具」的 spec + Risk ----------
  {
    MockTools mocks;
    tool::Registry reg;
    mocks.build(reg);

    llm::ScriptedClient llm;
    llm.push(call("h1", "pipeline_set_param", json{{"key", "gop"}, {"value", "30"}}));
    llm.push(call("h2", "query_metrics", json::object()));

    std::string seen_name;
    tool::Risk seen_risk = tool::Risk::ReadOnly;
    int approvals = 0;
    auto cfg = make_cfg(tool::ApprovalMode::Ask);
    cfg.ctx.approve = [&](const tool::ToolSpec& spec, tool::Risk risk, const json&) {
      seen_name = spec.name;
      seen_risk = risk;
      ++approvals;
      return true;
    };
    agent::Runner runner(llm, reg, cfg);
    runner.run("x", agent::Budget{}, nullptr);
    check(approvals == 1, "case8 approve called once for non-readonly tool");
    check(seen_name == "pipeline_set_param", "case8 approve sees the actual tool spec");
    check(seen_risk == tool::Risk::Mutating, "case8 approve sees the actual risk");
    check(mocks.set_param_calls == 1, "case8 approved tool executed");

    // 未提供回调时 Ask 等同于拒绝（不能默认放行）
    auto cfg2 = make_cfg(tool::ApprovalMode::Ask);
    llm::ScriptedClient llm2;
    llm2.push(call("h3", "pipeline_set_param", json{{"key", "gop"}, {"value", "30"}}));
    int denied = 0;
    agent::Runner runner2(llm2, reg, cfg2);
    runner2.run("x", agent::Budget{}, [&](const agent::Event& e) {
      if (e.type == "approval.denied") ++denied;
    });
    check(denied == 1, "case8 ask without callback denies");
  }

  // ---------- 用例 9：stage_complete 参数类型异常 → 拒绝而不是崩进程 ----------
  {
    MockTools mocks;
    tool::Registry reg;
    mocks.build(reg);

    llm::ScriptedClient llm;
    llm.push(call("i1", "stage_complete", json{{"next", 7}}));  // next 不是字符串
    llm.push(call("i2", "query_metrics", json::object()));
    llm.push(call("i3", "stage_complete", json{{"next", "diagnose"}}));
    llm.push(call("i4", "stage_complete", json{{"next", "repair"},
                                               {"root_cause", "network_loss"},
                                               {"evidence", "injected=59"},  // 应为数组
                                               {"confidence", 0.8}}));
    llm.push(call("i5", "stage_complete", json{{"next", "repair"},
                                               {"root_cause", "network_loss"},
                                               {"evidence", {"injected=59"}},
                                               {"confidence", "high"}}));  // 应为数字

    agent::Runner runner(llm, reg, make_cfg(tool::ApprovalMode::Auto));
    int rejects = 0;
    std::vector<std::string> reasons;
    const auto res = runner.run("x", agent::Budget{}, [&](const agent::Event& e) {
      if (e.type == "guard.reject") {
        ++rejects;
        reasons.push_back(e.data.value("reason", ""));
      }
    });
    check(rejects == 3, "case9 each malformed stage_complete rejected");
    check(res.final_stage == agent::Stage::GiveUp, "case9 malformed args do not crash");
    check(!res.error.empty(), "case9 give_up has an error");
    check(res.diagnosis.root_cause.empty(), "case9 invalid args never write diagnosis");
  }

  // ---------- 用例 10：轮次耗尽必须 GiveUp + 非空 error（不能静默停在中间态） ----------
  {
    MockTools mocks;
    tool::Registry reg;
    mocks.build(reg);

    llm::ScriptedClient llm;
    llm.push(text("我先观察一下"));
    llm.push(text("再看看"));
    agent::Budget b;
    b.max_turns = 1;
    agent::Runner runner(llm, reg, make_cfg(tool::ApprovalMode::Auto));
    const auto res = runner.run("x", b, nullptr);
    check(res.turns == 1, "case10 respects max_turns");
    check(res.final_stage == agent::Stage::GiveUp, "case10 turn exhaustion -> give_up");
    check(res.error == "turn budget exhausted", "case10 turn exhaustion names the reason");
    check(!res.ok(), "case10 exhausted run is not ok");
  }

  // ---------- 用例 11：verify 阶段不调工具不能直接 done（不再绕过守卫） ----------
  {
    MockTools mocks;
    tool::Registry reg;
    mocks.build(reg);

    llm::ScriptedClient llm;
    llm.push(call("j1", "query_metrics", json::object()));
    llm.push(call("j2", "stage_complete", json{{"next", "diagnose"}}));
    llm.push(call("j3", "stage_complete", json{{"next", "repair"},
                                               {"root_cause", "network_loss"},
                                               {"evidence", {"injected=59"}},
                                               {"confidence", 0.8}}));
    llm.push(call("j4", "pipeline_set_param", json{{"key", "gop"}, {"value", "30"}}));
    llm.push(call("j5", "stage_complete", json{{"next", "verify"}}));
    llm.push(text("我觉得已经好了，不用再查了"));
    llm.push(text("真的好了"));

    agent::Budget b;
    b.max_turns = 7;
    agent::Runner runner(llm, reg, make_cfg(tool::ApprovalMode::Auto));
    int no_tool_turns = 0;
    const auto res = runner.run("x", b, [&](const agent::Event& e) {
      if (e.type == "turn.no_tool_call") ++no_tool_turns;
    });
    check(no_tool_turns == 2, "case11 no-tool-call turns are reported");
    check(res.final_stage != agent::Stage::Done, "case11 cannot reach done without tools");
    check(!res.ok(), "case11 claims nothing");
  }

  // ---------- 用例 12：迁移图跳步被拒（collect → verify） ----------
  {
    MockTools mocks;
    tool::Registry reg;
    mocks.build(reg);

    llm::ScriptedClient llm;
    llm.push(call("k1", "query_metrics", json::object()));
    llm.push(call("k2", "pipeline_set_param", json{{"key", "gop"}, {"value", "30"}}));
    llm.push(call("k3", "stage_complete", json{{"next", "verify"}}));  // 跳过 diagnose/repair
    agent::Budget b;
    b.max_turns = 3;
    agent::Runner runner(llm, reg, make_cfg(tool::ApprovalMode::Auto));
    bool bad_transition = false;
    const auto res = runner.run("x", b, [&](const agent::Event& e) {
      if (e.type == "stage.enter" && e.data.value("stage", "") == "verify") bad_transition = true;
    });
    check(!bad_transition, "case12 collect -> verify rejected");
    check(res.final_stage == agent::Stage::GiveUp, "case12 stays out of verify");
  }

  // ---------- 用例 13：verify → done 走 harness 重测（不达标就拒绝） ----------
  {
    auto script_to_done = [](llm::ScriptedClient& s) {
      s.push(call("m1", "query_metrics", json::object()));
      s.push(call("m2", "stage_complete", json{{"next", "diagnose"}}));
      s.push(call("m3", "stage_complete", json{{"next", "repair"},
                                              {"root_cause", "network_loss"},
                                              {"evidence", {"injected=59"}},
                                              {"confidence", 0.8}}));
      s.push(call("m4", "pipeline_set_param", json{{"key", "gop"}, {"value", "30"}}));
      s.push(call("m5", "stage_complete", json{{"next", "verify"}}));
      s.push(call("m6", "stage_complete", json{{"next", "done"}}));
    };

    {
      MockTools mocks;
      tool::Registry reg;
      mocks.build(reg);
      llm::ScriptedClient llm;
      script_to_done(llm);
      int measured = 0;
      auto cfg = make_cfg(tool::ApprovalMode::Auto);
      cfg.verify_sla = [&](const agent::Diagnosis&) {
        ++measured;
        return false;  // harness 实测未达标
      };
      agent::Runner runner(llm, reg, cfg);
      std::string reason;
      const auto res = runner.run("x", agent::Budget{}, [&](const agent::Event& e) {
        if (e.type == "guard.reject") reason = e.data.value("reason", "");
      });
      check(measured == 1, "case13 verify_sla called before done");
      check(res.final_stage != agent::Stage::Done, "case13 failed measurement blocks done");
      check(reason.find("实测未达标") != std::string::npos, "case13 rejection says measured failure");
    }
    {
      MockTools mocks;
      tool::Registry reg;
      mocks.build(reg);
      llm::ScriptedClient llm;
      script_to_done(llm);
      auto cfg = make_cfg(tool::ApprovalMode::Auto);
      cfg.verify_sla = [](const agent::Diagnosis&) { return true; };
      agent::Runner runner(llm, reg, cfg);
      std::string verify_kind;
      const auto res = runner.run("x", agent::Budget{}, [&](const agent::Event& e) {
        if (e.type == "stage.enter" && e.data.value("stage", "") == "done") {
          verify_kind = e.data.value("verify", "");
        }
      });
      check(res.final_stage == agent::Stage::Done, "case13 measured ok reaches done");
      check(verify_kind == "measured", "case13 done event marks verify=measured");
    }
  }

  // ---------- 用例 14：参数校验收紧 + 审批策略下沉（call_guarded） ----------
  {
    MockTools mocks;
    tool::Registry reg;
    mocks.build(reg);

    const auto r1 = reg.call("pipeline_set_param", json{{"key", "gop"}, {"value", "30"}, {"bogus", 1}});
    check(!r1.has_value() && r1.error().message.find("unknown param") != std::string::npos,
          "case14 unknown param rejected");
    // FR-TOOL.8：错误分类要可区分——参数错必须标成 Argument（模型据此改参数，而不是换工具）
    check(!r1.has_value() && r1.error().kind == cue::tool::ToolError::Kind::Argument,
          "case14 参数错误分类为 Argument");

    // 未知工具是引擎侧问题（注册表与 schema 不一致），不是模型能修的 → Internal
    const auto r_unknown = reg.call("no_such_tool", json::object());
    check(!r_unknown.has_value() &&
              r_unknown.error().kind == cue::tool::ToolError::Kind::Internal,
          "未知工具分类为 Internal");
    // 审批拒绝单独一类：模型该去申请审批，不该改参数重试
    {
      cue::tool::CallContext dctx;
      dctx.mode = cue::tool::ApprovalMode::Deny;
      const auto denied = reg.call_guarded("pipeline_set_param",
                                           json{{"key", "gop"}, {"value", "30"}}, dctx);
      check(denied.status == cue::tool::GuardedCall::Status::ApprovalDenied &&
                !denied.outcome &&
                denied.outcome.error().kind == cue::tool::ToolError::Kind::ApprovalDenied,
            "审批拒绝分类为 ApprovalDenied 且带申请审批提示");
    }
    check(reg.call("query_metrics", json{{"window_ms", 5000.0}}).has_value(),
          "case14 integer accepts 5000.0");
    check(!reg.call("query_metrics", json{{"window_ms", 1.5}}).has_value(),
          "case14 non-integral rejected");

    tool::CallContext deny;
    deny.mode = tool::ApprovalMode::Deny;
    check(reg.call_guarded("pipeline_set_param", json{{"key", "gop"}, {"value", "30"}}, deny).status ==
              tool::GuardedCall::Status::ApprovalDenied,
          "case14 deny blocks mutating");
    check(reg.call_guarded("no_such_tool", json::object(), deny).status ==
              tool::GuardedCall::Status::ApprovalDenied,
          "case14 unknown tool denied");
    check(reg.call_guarded("query_metrics", json::object(), deny).status ==
              tool::GuardedCall::Status::Executed,
          "case14 deny still allows read-only");
  }

  // ---------- 用例 15：finish_reason=length 的截断轮不执行、只回灌 ----------
  {
    MockTools mocks;
    tool::Registry reg;
    mocks.build(reg);

    llm::ScriptedClient llm;
    llm::ChatResponse cut;
    cut.truncated = true;
    cut.finish_reason = "length";
    cut.content = "我先查一下";
    llm::ToolCall half;
    half.id = "t1";
    half.name = "query_metrics";
    half.arguments = "{\"window_ms\":";  // 半截 JSON
    cut.tool_calls.push_back(half);
    cut.usage = {100, 20};
    llm.push(cut);
    llm.push(call("t2", "query_metrics", json::object()));

    agent::Runner runner(llm, reg, make_cfg(tool::ApprovalMode::Auto));
    int truncations = 0;
    runner.run("x", agent::Budget{}, [&](const agent::Event& e) {
      if (e.type == "llm.truncated") ++truncations;
    });
    check(truncations == 1, "case15 truncated turn reported");
    check(mocks.metrics_calls == 1, "case15 truncated tool call not executed");
  }

  if (failures == 0) {
    std::printf("test_agent: OK (15 cases)\n");
    return 0;
  }
  std::printf("test_agent: %d failure(s)\n", failures);
  return 1;
}
