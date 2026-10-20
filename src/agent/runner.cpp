#include <chrono>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "cue/agent/agent.h"
#include "cue/agent/context.h"
#include "cue/common/clock.h"

namespace cue::agent {

const char* stage_name(Stage s) {
  switch (s) {
    case Stage::Collect: return "collect";
    case Stage::Diagnose: return "diagnose";
    case Stage::Repair: return "repair";
    case Stage::Verify: return "verify";
    case Stage::Done: return "done";
    case Stage::GiveUp: return "give_up";
  }
  return "?";
}

namespace {

// 根因枚举（SPEC §7.6）
const char* kRootCauses[] = {"network_loss", "network_delay", "bandwidth_insufficient",
                             "encoder_too_slow", "resolution_too_high", "queue_backlog",
                             "keyframe_storm", "clock_drift"};

bool valid_root_cause(const std::string& s) {
  for (const char* c : kRootCauses) {
    if (s == c) return true;
  }
  return false;
}

std::string stage_hint(Stage s) {
  switch (s) {
    case Stage::Collect:
      return "当前阶段 collect：先用 query_metrics / fetch_observation 了解现状（至少一次），"
             "然后调用 stage_complete，参数 {\"next\":\"diagnose\"}。";
    case Stage::Diagnose:
      return "当前阶段 diagnose：根据观测给出根因（枚举值）与证据（引用真实观测片段）。"
             "完成后调用 stage_complete，参数 {\"next\":\"repair\",\"root_cause\":\"...\",\"evidence\":[\"...\"],\"confidence\":0.8}。";
    case Stage::Repair:
      return "当前阶段 repair：执行 mutating 工具调整参数（至少一次成功后），"
             "然后调用 stage_complete，参数 {\"next\":\"verify\"}。";
    case Stage::Verify:
      return "当前阶段 verify：查一次指标并与目标对比。若已满足目标（例如 p99 正常、dropped 为 0），"
             "**立即**调用 stage_complete 参数 {\"next\":\"done\"}，不要继续查询；"
             "若仍未满足，调用 stage_complete {\"next\":\"repair\"}。";
    default:
      return "";
  }
}

// 消息构造：Default-init 后按需赋值，避免聚合初始化漏成员（-Wmissing-field-initializers）
llm::ChatMessage msg(const char* role, std::string content) {
  llm::ChatMessage m;
  m.role = role;
  m.content = std::move(content);
  return m;
}

llm::ChatMessage tool_msg(std::string content, const std::string& call_id, const std::string& name) {
  llm::ChatMessage m;
  m.role = "tool";
  m.content = std::move(content);
  m.tool_call_id = call_id;
  m.name = name;
  return m;
}

json stage_complete_schema() {
  json props = {
      {"next", {{"type", "string"}, {"enum", {"diagnose", "repair", "verify", "done", "give_up"}}}},
      {"root_cause", {{"type", "string"}}},
      {"evidence", {{"type", "array"}, {"items", {{"type", "string"}}}}},
      {"confidence", {{"type", "number"}}},
  };
  return json{{"type", "function"},
              {"function",
               {{"name", "stage_complete"},
                {"description", "声明当前阶段完成并请求进入下一阶段"},
                {"parameters", {{"type", "object"}, {"properties", props}, {"required", {"next"}}}}}}};
}

}  // namespace

Runner::Runner(llm::ILlmClient& llm, const tool::Registry& tools, Config cfg)
    : llm_(llm), tools_(tools), cfg_(std::move(cfg)) {}

RunResult Runner::run(const std::string& goal, const Budget& budget,
                      const std::function<void(const Event&)>& on_event) {
  RunResult r;
  Stage stage = Stage::Collect;
  bool used_observe = false;
  bool used_mutating = false;
  bool verified_ok = false;
  int guard_rejects = 0;
  const int64_t t_start = now_us();

  auto emit = [&](const char* type, json data) {
    if (on_event) on_event(Event{type, std::move(data)});
  };

  std::vector<llm::ChatMessage> msgs;
  msgs.push_back(msg("system", cfg_.system_prompt));
  msgs.push_back(msg("user", goal));
  emit("run.start", {{"goal", goal}, {"budget", {{"max_turns", budget.max_turns}}}});

  while (r.turns < budget.max_turns) {
    if (now_us() - t_start > int64_t(budget.max_wall_s) * 1'000'000LL) {
      r.error = "wall clock budget exhausted";
      break;
    }

    llm::ChatRequest req;
    req.model = cfg_.model;
    req.messages = msgs;
    req.tools = tools_.schema();
    req.tools.push_back(stage_complete_schema());
    // 阶段提示作为最后一条 user 消息里追加（不污染历史）
    req.messages.push_back(msg("user", stage_hint(stage)));
    // 上下文预算：超限丢最旧观测，保留 system 与最近若干条
    {
      ContextBudget cb;
      cb.max_tokens = cfg_.max_context_tokens;
      const std::size_t dropped = trim(req.messages, cb);
      if (dropped > 0) {
        emit("context.trim", {{"dropped", dropped}, {"est_tokens", estimate_tokens(req.messages)}});
      }
    }

    auto resp = llm_.chat(req);
    if (!resp) {
      r.error = resp.error();
      break;
    }
    ++r.turns;
    r.cost_yuan += double(resp->usage.prompt_tokens + resp->usage.completion_tokens) / 1e6 * cfg_.yuan_per_mtok;
    emit("llm.response",
         {{"turn", r.turns}, {"content", resp->content}, {"tool_calls", resp->tool_calls.size()}});

    if (r.cost_yuan > budget.max_yuan) {
      r.error = "cost budget exhausted";
      break;
    }

    if (resp->tool_calls.empty()) {
      // 无工具调用：视为模型想直接结束
      if (stage == Stage::Verify) {
        stage = Stage::Done;
        break;
      }
      msgs.push_back(msg("assistant", resp->content));
      msgs.push_back(msg("user", "请继续当前阶段：" + stage_hint(stage)));
      continue;
    }

    // 把 assistant 的 tool_calls 一次性写回历史，再逐个回填 tool 结果
    {
      llm::ChatMessage m = msg("assistant", resp->content);
      m.tool_calls = resp->tool_calls;
      msgs.push_back(std::move(m));
    }

    for (const auto& tc : resp->tool_calls) {
      // ---- 控制工具：阶段推进 ----
      if (tc.name == "stage_complete") {
        json args = json::parse(tc.arguments, nullptr, false);
        if (args.is_discarded()) args = json::object();
        const std::string next = args.value("next", "");
        std::string reject;

        if (next == "diagnose") {
          if (!used_observe) reject = "尚未采集任何观测，不能进入 diagnose";
          else stage = Stage::Diagnose;
        } else if (next == "repair") {
          const std::string rc = args.value("root_cause", "");
          const auto ev = args.value("evidence", std::vector<std::string>{});
          if (!valid_root_cause(rc)) reject = "root_cause 不是合法枚举值";
          else if (ev.empty()) reject = "evidence 为空，修复前必须给出证据";
          else {
            r.diagnosis.root_cause = rc;
            r.diagnosis.evidence = ev;
            r.diagnosis.confidence = args.value("confidence", 0.0);
            stage = Stage::Repair;
          }
        } else if (next == "verify") {
          if (!used_mutating) reject = "尚未执行任何 mutating 工具，不能进入 verify";
          else stage = Stage::Verify;
        } else if (next == "done") {
          if (stage != Stage::Verify) reject = "只有 verify 阶段可以进入 done";
          else {
            verified_ok = true;
            stage = Stage::Done;
          }
        } else if (next == "give_up") {
          stage = Stage::GiveUp;
        } else {
          reject = "next 取值非法";
        }

        if (!reject.empty()) {
          ++guard_rejects;
          emit("guard.reject", {{"next", next}, {"reason", reject}});
          msgs.push_back(tool_msg("拒绝：" + reject, tc.id, "stage_complete"));
          if (guard_rejects >= 3) {
            r.error = "too many guard rejects";
            stage = Stage::GiveUp;
            break;
          }
          continue;
        }

        emit("stage.enter", {{"stage", stage_name(stage)}});
        msgs.push_back(tool_msg("ok, now in stage " + std::string(stage_name(stage)), tc.id, "stage_complete"));
        if (stage == Stage::Done || stage == Stage::GiveUp) break;
        continue;
      }

      // ---- 普通工具：审批 + 校验 + 执行 ----
      json args = json::parse(tc.arguments, nullptr, false);
      if (args.is_discarded()) {
        msgs.push_back(tool_msg("参数不是合法 JSON", tc.id, tc.name));
        continue;
      }

      const tool::Risk risk = tools_.risk_of(tc.name);
      bool approved = true;
      if (risk != tool::Risk::ReadOnly) {
        if (cfg_.ctx.mode == tool::ApprovalMode::Deny) {
          approved = false;
        } else if (cfg_.ctx.mode == tool::ApprovalMode::Auto) {
          if (risk == tool::Risk::Dangerous) {
            bool allowed = false;
            for (const auto& a : cfg_.ctx.allowed_dangerous) {
              if (a == tc.name) {
                allowed = true;
                break;
              }
            }
            approved = allowed;
          }
        } else {  // Ask
          approved = cfg_.ctx.approve ? cfg_.ctx.approve(tools_.specs().front(), args) : false;
        }
      }

      if (!approved) {
        emit("approval.denied", {{"tool", tc.name}, {"risk", tool::risk_name(risk)}});
        msgs.push_back(tool_msg(std::string("被拒绝（") + tool::risk_name(risk) + "）", tc.id, tc.name));
        continue;
      }

      emit("tool.call", {{"tool", tc.name}, {"risk", tool::risk_name(risk)}, {"args", args}});
      auto res = tools_.call(tc.name, args);
      if (!res) {
        emit("tool.error", {{"tool", tc.name}, {"error", res.error()}});
        msgs.push_back(tool_msg("错误：" + res.error(), tc.id, tc.name));
        continue;
      }

      emit("tool.result", {{"tool", tc.name}, {"ok", res->ok}, {"data", res->data}});
      if (tc.name == "query_metrics" || tc.name == "fetch_observation") used_observe = true;
      if (risk == tool::Risk::Mutating && res->ok) used_mutating = true;

      msgs.push_back(tool_msg(res->content, tc.id, tc.name));
    }

    if (stage == Stage::Done || stage == Stage::GiveUp) break;
  }

  r.final_stage = stage;
  if (stage == Stage::Done && !verified_ok) r.error = "done without verify";
  emit("run.end", {{"stage", stage_name(stage)}, {"turns", r.turns}, {"cost_yuan", r.cost_yuan}});
  return r;
}

}  // namespace cue::agent
