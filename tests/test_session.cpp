// T-19（FR-AGENT.5）：Agent 会话持久化与中断恢复。
// 覆盖：六类记录往返 / 尾部残行容错 / 预算续算 / 阶段复位 / 已结束会话拒绝续写 /
// 单写者锁 / 未回填 tool_call 的合成修复 / cancelled 会话仍可恢复。
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "cue/agent/agent.h"
#include "cue/agent/session.h"
#include "cue/llm/client.h"
#include "cue/tool/tool.h"

namespace fs = std::filesystem;
namespace agent = cue::agent;
namespace llm = cue::llm;
namespace tool = cue::tool;
using json = nlohmann::json;

namespace {

int failures = 0;
void check(bool cond, const std::string& what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}

bool write_text(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary);
  if (!out) return false;
  out << text;
  return static_cast<bool>(out);
}

bool append_text(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::app);
  if (!out) return false;
  out << text;
  return static_cast<bool>(out);
}

llm::ChatMessage text_msg(const char* role, std::string content) {
  llm::ChatMessage m;
  m.role = role;
  m.content = std::move(content);
  return m;
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

// 记录每次请求的客户端：恢复后"模型看到的上下文"必须能被断言
// （system prompt 是不是当时那份、历史是不是按原顺序重放）。
struct CaptureClient : llm::ILlmClient {
  llm::ScriptedClient inner;
  std::vector<std::vector<llm::ChatMessage>> requests;

  std::expected<llm::ChatResponse, std::string> chat(const llm::ChatRequest& req) override {
    requests.push_back(req.messages);
    return inner.chat(req);
  }
};

// 最小工具集：一个只读观测工具 + 一个写工具（覆盖 repair→verify 的守卫）
struct MockTools {
  void build(tool::Registry& reg) {
    reg.add(tool::ToolSpec{.name = "query_metrics",
                           .desc = "查询指标",
                           .risk = tool::Risk::ReadOnly,
                           .params = {{.name = "window_ms",
                                       .type = "integer",
                                       .desc = "窗口毫秒",
                                       .required = false,
                                       .enum_values = {},
                                       .has_range = false}}},
            [](const json&) {
              json m{{"p99_ms", 181.4}, {"nack_retransmit", 42}};
              return tool::ToolResult{true, "metrics=" + m.dump(), m};
            });
    reg.add(tool::ToolSpec{.name = "pipeline_set_param",
                           .desc = "改参数",
                           .risk = tool::Risk::Mutating,
                           .params = {{.name = "key",
                                       .type = "string",
                                       .desc = "参数名",
                                       .required = true,
                                       .enum_values = {"bitrate_kbps"},
                                       .has_range = false,
                                       .min = 0,
                                       .max = 0},
                                      {.name = "value",
                                       .type = "string",
                                       .desc = "值",
                                       .required = true,
                                       .enum_values = {},
                                       .has_range = false,
                                       .min = 0,
                                       .max = 0}}},
            [](const json& a) { return tool::ToolResult{true, "ok", a}; });
  }
};

// 一个"中断在 repair"的会话文件：六类记录都用上，recovery 需要的东西一样不少
void write_interrupted_session(const std::string& path) {
  auto log = agent::SessionLog::open(path, /*create_if_missing=*/true);
  if (!log) {
    check(false, "构造会话：打开失败 " + log.error());
    return;
  }
  agent::SessionLog& L = **log;
  check(L.write_meta("scripted", "run", "OLD SYSTEM PROMPT", "把 p99 压到 120ms").has_value(),
        "写 meta");
  check(L.write_message(text_msg("system", "OLD SYSTEM PROMPT")).has_value(), "写 system");
  check(L.write_message(text_msg("user", "把 p99 压到 120ms")).has_value(), "写 user");

  llm::ChatMessage asst = text_msg("assistant", "先看指标");
  llm::ToolCall tc{"c1", "query_metrics", "{\"window_ms\":5000}"};
  asst.tool_calls.push_back(tc);
  check(L.write_message(asst).has_value(), "写 assistant(tool_calls)");
  check(L.write_tool("query_metrics", json{{"window_ms", 5000}}, true, "metrics={\"p99_ms\":181.4}",
                     1234, false, "read-only", 1)
            .has_value(),
        "写 tool");
  llm::ChatMessage tm = text_msg("tool", "metrics={\"p99_ms\":181.4}");
  tm.tool_call_id = "c1";
  tm.name = "query_metrics";
  check(L.write_message(tm).has_value(), "写 tool 消息");

  agent::ObservationRecord ob;
  ob.tool = "query_metrics";
  ob.args = json{{"window_ms", 5000}};
  ob.text = "metrics={\"p99_ms\":181.4}";
  ob.data = json{{"p99_ms", 181.4}};
  ob.turn = 1;
  check(L.write_observation(ob).has_value(), "写 observation");

  agent::Diagnosis d;
  d.root_cause = "network_loss";
  d.evidence = {"nack_retransmit=42"};
  d.confidence = 0.8;
  // 两条 stage 记录：collect→diagnose、diagnose→repair（带诊断）
  check(L.write_stage(agent::Stage::Collect, "diagnose", true, "",
                      agent::SessionProgress{1, 0.1, 1'000'000}, true, false, 0, {})
            .has_value(),
        "写 stage collect→diagnose");
  check(L.write_stage(agent::Stage::Diagnose, "repair", true, "",
                      agent::SessionProgress{2, 0.2, 2'000'000}, true, false, 0, d)
            .has_value(),
        "写 stage diagnose→repair");
  check(L.write_budget(agent::SessionProgress{5, 0.3, 2'000'000}, true, false, 0).has_value(),
        "写 budget");
  // 被 kill -9 的会话通常**没有** end 记录；这里刻意不写，模拟真实的崩溃现场
}

}  // namespace

int main() {
  std::error_code ec;
  const fs::path root = fs::temp_directory_path() / "cue-test-session";
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);

  // 1) 六类记录往返：meta / message / tool / observation / stage / budget（+ end）
  {
    const std::string p = (root / "runA" / "sessions" / "s1.jsonl").string();
    {
      auto log = agent::SessionLog::open(p, true);
      check(log.has_value(), "新建 session 文件");
      if (log) {
        auto& L = **log;
        check(L.write_meta("deepseek-chat", "demo", "SYS-1", "GOAL-1").has_value(), "meta 写入");
        llm::ChatMessage sys = text_msg("system", "SYS-1");
        check(L.write_message(sys).has_value(), "message(system) 写入");
        llm::ChatMessage user = text_msg("user", "GOAL-1");
        check(L.write_message(user).has_value(), "message(user) 写入");
        llm::ChatMessage asst = text_msg("assistant", "看看指标");
        asst.tool_calls.push_back(llm::ToolCall{"c1", "query_metrics", "{\"window_ms\":5000}"});
        check(L.write_message(asst).has_value(), "message(assistant) 写入");
        llm::ChatMessage tm = text_msg("tool", "metrics={}");
        tm.tool_call_id = "c1";
        tm.name = "query_metrics";
        check(L.write_message(tm).has_value(), "message(tool) 写入");
        check(L.write_tool("query_metrics", json{{"window_ms", 5000}}, true, "metrics={}", 4242,
                           false, "read-only", 1)
                  .has_value(),
              "tool 记录写入");
        agent::ObservationRecord ob;
        ob.tool = "query_metrics";
        ob.args = json{{"window_ms", 5000}};
        ob.text = "metrics={}";
        ob.data = json{{"p99_ms", 181.4}};
        ob.turn = 1;
        check(L.write_observation(ob).has_value(), "observation 记录写入");
        check(L.write_stage(agent::Stage::Collect, "diagnose", true, "",
                            agent::SessionProgress{1, 0.01, 5000}, true, false, 0, {})
                  .has_value(),
              "stage 记录写入");
        check(L.write_budget(agent::SessionProgress{1, 0.01, 5000}, true, false, 0).has_value(),
              "budget 记录写入");
        check(L.write_end(agent::Stage::GiveUp, "cancelled",
                          agent::SessionProgress{1, 0.01, 5000})
                  .has_value(),
              "end 记录写入");
      }
    }
    auto loaded = agent::load_session(p);
    check(loaded.has_value(), "读回 session");
    if (loaded) {
      check(loaded->meta.value("session_id", std::string()) == "s1", "meta.session_id 由路径反推");
      check(loaded->meta.value("run_id", std::string()) == "runA", "meta.run_id 由路径反推");
      check(loaded->meta.value("model", std::string()) == "deepseek-chat", "meta.model 往返");
      check(loaded->meta.value("mode", std::string()) == "demo", "meta.mode 往返");
      check(loaded->meta.value("system_prompt", std::string()) == "SYS-1",
            "meta 里保存了当时的 system prompt");
      check(loaded->meta.value("goal", std::string()) == "GOAL-1", "meta.goal 往返");
      check(!loaded->meta.value("agent_version", std::string()).empty(), "meta.agent_version");
      check(loaded->meta.value("started_us", int64_t(0)) > 0, "meta.started_us");
      check(loaded->messages.size() == 4, "四条 message 往返");
      if (loaded->messages.size() == 4) {
        check(loaded->messages[0].role == "system" && loaded->messages[0].content == "SYS-1",
              "system 消息往返");
        check(loaded->messages[2].tool_calls.size() == 1 &&
                  loaded->messages[2].tool_calls[0].id == "c1" &&
                  loaded->messages[2].tool_calls[0].name == "query_metrics",
              "assistant.tool_calls 往返");
        check(loaded->messages[3].tool_call_id == "c1" &&
                  loaded->messages[3].name == "query_metrics",
              "tool 消息的配对字段往返");
      }
      check(loaded->observations.size() == 1, "observation 往返条数");
      if (!loaded->observations.empty()) {
        check(loaded->observations[0].tool == "query_metrics" &&
                  loaded->observations[0].text == "metrics={}" &&
                  loaded->observations[0].data["p99_ms"] == 181.4,
              "observation 内容往返");
        check(!loaded->observations[0].truncated &&
                  loaded->observations[0].original_bytes == loaded->observations[0].text.size(),
              "observation 未截断时标记与原始大小正确");
      }
      check(loaded->stage == agent::Stage::Diagnose, "stage 记录把状态推进到 diagnose");
      check(loaded->progress.turns == 1, "budget.turns 往返");
      check(loaded->progress.wall_us == 5000, "budget.wall_us 往返");
      check(loaded->used_observe, "used_observe 往返");
      check(!loaded->used_mutating, "used_mutating 往返");
      check(loaded->end_kind == "cancelled", "end.kind 往返");
      // 取消只是这次 run 被叫停，文件仍是可恢复的完整快照
      check(!loaded->ended, "cancelled 的 end 不把会话封口");
    }
    // 取消后的会话必须能 resume（阶段仍停在 diagnose）
    auto opened = agent::open_session(p, /*must_exist=*/true);
    check(opened.has_value(), "cancelled 会话允许恢复");
    if (opened) check(opened->resumed && opened->resumed->stage == agent::Stage::Diagnose,
                      "恢复时阶段仍是中断时的 diagnose");
  }

  // 2) 尾部残行容错：最后一行是半截 JSON → 跳过并告警，前面的记录仍可用
  {
    const std::string p = (root / "runA" / "sessions" / "s2.jsonl").string();
    {
      auto log = agent::SessionLog::open(p, true);
      if (log) {
        auto& L = **log;
        L.write_meta("m", "demo", "SYS", "G");
        L.write_message(text_msg("system", "SYS"));
        L.write_message(text_msg("user", "G"));
      }
    }
    check(append_text(p, R"({"t":"message","role":"assis)"),
          "手工制造尾部半行（模拟写到一半被 kill）");
    auto loaded = agent::load_session(p);
    check(loaded.has_value(), "残行不阻止读取");
    if (loaded) {
      check(loaded->messages.size() == 2, "残行之后的记录仍可用");
      check(!loaded->warnings.empty(), "残行必须产生警告");
    }
    // 续写：open_session 先截掉残行再追加，文件里不会留下中间坏行
    auto opened = agent::open_session(p, /*must_exist=*/false);
    check(opened.has_value(), "残行文件可以续写");
    if (opened) {
      check(opened->resumed.has_value(), "有内容即按续写处理");
      check(opened->log->write_message(text_msg("user", "继续")).has_value(), "残行之后追加成功");
    }
    auto again = agent::load_session(p);
    check(again.has_value(), "截断后文件仍是合法 JSONL");
    if (again) {
      check(again->messages.size() == 3, "截断后追加的记录可读回");
      check(again->warnings.empty(), "截断后不再有残行警告");
    }

    // 中间行坏 = 文件真坏了：必须拒绝，而不是拿半份历史硬跑
    const std::string bad = (root / "runA" / "sessions" / "bad.jsonl").string();
    write_text(bad,
               "{\"t\":\"meta\",\"format\":\"cue-agent-session/1\"}\n"
               "{这不是 JSON}\n"
               "{\"t\":\"message\",\"role\":\"user\",\"content\":\"x\"}\n");
    auto broken = agent::load_session(bad);
    check(!broken.has_value(), "中间坏行拒绝恢复");
  }

  // 3) + 4) 预算续算与阶段复位：中断在 repair，恢复后 turns 从 5 起算、阶段直接进 repair
  {
    const std::string p = (root / "runB" / "sessions" / "s3.jsonl").string();
    write_interrupted_session(p);

    auto opened = agent::open_session(p, /*must_exist=*/false);
    check(opened.has_value() && opened->resumed.has_value(), "中断的会话按续写打开");
    if (opened && opened->resumed) {
      check(opened->resumed->stage == agent::Stage::Repair, "阶段复位：中断在 repair");
      check(opened->resumed->progress.turns == 5, "预算复位：turns=5");
      check(opened->resumed->diagnosis.root_cause == "network_loss", "诊断从 session 恢复");
      check(opened->resumed->observations.size() == 1, "观测快照从 session 恢复");

      CaptureClient client;
      // 恢复后的脚本从 repair 继续：先写一次参数，再进 verify，最后 done
      client.inner.push(call("r1", "pipeline_set_param",
                             json{{"key", "bitrate_kbps"}, {"value", "800"}}));
      client.inner.push(call("r2", "stage_complete", json{{"next", "verify"}}));
      client.inner.push(call("r3", "stage_complete", json{{"next", "done"}}));

      MockTools mt;
      tool::Registry reg;
      mt.build(reg);

      agent::Runner::Config cfg;
      cfg.model = "scripted";
      // 当前代码的默认 prompt 必须**不被**使用（否则模型看到的世界变了）
      cfg.system_prompt = "NEW SYSTEM PROMPT";
      cfg.session = opened->log.get();
      cfg.resume = &*opened->resumed;

      std::vector<std::string> stages;
      std::vector<int> turn_numbers;
      int resumed_turns = -1;
      std::string resumed_stage;
      int resumed_messages = -1;
      // 预算必须比 default 宽：session 里已经花了 0.3，而默认 max_yuan 就是 0.3——
      // 若从 0 起算，这段恢复会"看起来还能花"，实际上是超预算的（这正是续算要证明的事）
      agent::Budget budget;
      budget.max_yuan = 1.0;
      agent::Runner runner(client, reg, cfg);
      const auto res = runner.run("把 p99 压到 120ms", budget, [&](const agent::Event& e) {
        if (e.type == "llm.response") turn_numbers.push_back(e.data["turn"].get<int>());
        if (e.type == "stage.enter") stages.push_back(e.data["stage"].get<std::string>());
        if (e.type == "session.resumed") {
          resumed_stage = e.data["from_stage"].get<std::string>();
          resumed_turns = e.data["turns"].get<int>();
          resumed_messages = e.data["messages"].get<int>();
        }
      });

      check(resumed_stage == "repair", "session.resumed 事件里的 from_stage=repair");
      check(resumed_turns == 5, "session.resumed 事件里的 turns=5");
      check(resumed_messages == 4, "session.resumed 事件里的 messages=会话里的 4 条");
      check(!turn_numbers.empty() && turn_numbers.front() == 6, "恢复后第一轮从 6 起算（turns=5）");
      check(res.turns == 8, "最终 turns 接着累计（5 + 3）");
      check(res.cost_yuan > 0.3, "最终成本接着累计（> 0.3）");
      check(res.final_stage == agent::Stage::Done && res.error.empty(), "恢复后跑到 done");
      // 已完成阶段不重跑：不出现 collect / diagnose 的 stage.enter
      bool rerun = false;
      for (const auto& s : stages) rerun = rerun || s == "collect" || s == "diagnose";
      check(!rerun, "恢复后不重跑 collect/diagnose");
      // 对话按原顺序重放，system prompt 用 session 里那份
      check(!client.requests.empty(), "恢复后确实发起了 LLM 调用");
      if (!client.requests.empty()) {
        const auto& first = client.requests.front();
        check(!first.empty() && first.front().role == "system", "恢复后的第一条是 system");
        check(!first.empty() && first.front().content == "OLD SYSTEM PROMPT",
              "system prompt 用 session 里记录的（不是当前默认值）");
        bool has_assistant = false;
        for (const auto& m : first) has_assistant = has_assistant || m.role == "assistant";
        check(has_assistant, "历史对话被重放（含 assistant(tool_calls)）");
      }
      check(res.session_error.empty(), "恢复并写盘全程没有 session 错误");

      // 恢复后的追加都落在同一个文件里：再读一次应该能看到 end 记录与新增消息
      auto after = agent::load_session(p);
      check(after.has_value(), "恢复后的 session 仍可读");
      if (after) {
        check(after->ended && after->end_final_stage == agent::Stage::Done,
              "恢复后写入的 end 记录把会话封口为 done");
        check(after->messages.size() > 4, "恢复后新增的对话也落盘了");
        check(after->progress.turns == 8, "落盘的累计轮次是 8");
        check(after->diagnosis.root_cause == "network_loss", "诊断在恢复后仍保留");
      }
    }
  }

  // 5) 已结束的会话拒绝续写
  {
    const std::string p = (root / "runC" / "sessions" / "s4.jsonl").string();
    {
      auto log = agent::SessionLog::open(p, true);
      if (log) {
        auto& L = **log;
        L.write_meta("m", "run", "SYS", "G");
        L.write_message(text_msg("system", "SYS"));
        L.write_message(text_msg("user", "G"));
        L.write_stage(agent::Stage::Collect, "give_up", true, "",
                      agent::SessionProgress{3, 0.05, 900}, false, false, 0, {});
        L.write_end(agent::Stage::GiveUp, "too many guard rejects (consecutive 2)",
                    agent::SessionProgress{3, 0.05, 900});
      }
    }
    auto opened = agent::open_session(p, /*must_exist=*/true);
    check(!opened.has_value(), "已结束的会话拒绝续写");
    if (!opened) {
      check(opened.error().find("已结束") != std::string::npos, "错误信息说明会话已结束");
    }
    auto loaded = agent::load_session(p);
    check(loaded.has_value() && loaded->ended, "已结束的会话仍能读出来（审计/展示）");
    if (loaded) check(loaded->end_kind == "guard_reject", "end.kind 分类为守卫连拒");

    // --resume-session 指向不存在的路径必须报错，不能悄悄新建
    auto missing = agent::open_session((root / "runC" / "sessions" / "nope.jsonl").string(), true);
    check(!missing.has_value(), "resume 不存在的会话必须报错");
  }

  // 6) 单写者锁：第二个打开者必须失败；第一个释放后可以再打开
  {
    const std::string p = (root / "runD" / "sessions" / "s5.jsonl").string();
    auto first = agent::open_session(p, /*must_exist=*/false);
    check(first.has_value(), "第一个打开者拿到会话（新建）");
    auto second = agent::SessionLog::open(p, /*create_if_missing=*/true);
    check(!second.has_value(), "第二个打开者被单写者锁拒绝");
    if (!second) {
      check(second.error().find("另一个进程") != std::string::npos,
            "锁错误说明是并发占用（而不是文件坏）");
    }
    if (first) {
      check(first->log->write_meta("m", "run", "SYS", "G").has_value(), "持锁期间可以写");
      first->log.reset();  // 释放锁
    }
    auto third = agent::open_session(p, /*must_exist=*/false);
    check(third.has_value(), "释放后可以重新打开");
    if (third) check(third->resumed.has_value(), "重新打开读到已有内容（按续写处理）");
  }

  // 7) 未回填的 tool_call：中断在 tool 结果落盘之前时，恢复要补合成结果
  {
    const std::string p = (root / "runE" / "sessions" / "s6.jsonl").string();
    {
      auto log = agent::SessionLog::open(p, true);
      if (log) {
        auto& L = **log;
        L.write_meta("m", "run", "SYS", "G");
        L.write_message(text_msg("system", "SYS"));
        L.write_message(text_msg("user", "G"));
        llm::ChatMessage asst = text_msg("assistant", "");
        asst.tool_calls.push_back(llm::ToolCall{"c1", "query_metrics", "{}"});
        asst.tool_calls.push_back(llm::ToolCall{"c2", "pipeline_set_param", "{}"});
        L.write_message(asst);
        llm::ChatMessage tm = text_msg("tool", "metrics={}");
        tm.tool_call_id = "c1";
        tm.name = "query_metrics";
        L.write_message(tm);  // c2 的结果没有落盘（进程被杀在这里）
        L.write_budget(agent::SessionProgress{1, 0.01, 700}, true, false, 0);
      }
    }
    auto loaded = agent::load_session(p);
    check(loaded.has_value(), "未回填 tool_call 的会话仍可读");
    if (loaded) {
      check(loaded->synthetic_tail.size() == 1, "缺一条 tool 结果 → 生成一条合成结果");
      if (!loaded->synthetic_tail.empty()) {
        check(loaded->synthetic_tail[0].role == "tool" &&
                  loaded->synthetic_tail[0].tool_call_id == "c2",
              "合成结果回填到缺失的那条 tool_call 上");
        check(loaded->synthetic_tail[0].content.find("中断") != std::string::npos,
              "合成结果写明是中断补的（不是真实观测）");
      }
    }
  }

  std::printf(failures == 0 ? "test_session: OK\n" : "test_session: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
