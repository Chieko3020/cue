#include <algorithm>
#include <chrono>
#include <cstdio>
#include <expected>
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

// 迁移图白名单（SPEC §7.5）：collect→diagnose→repair→verify→done，
// verify 未达标可回 repair；任意阶段可显式 give_up。其余一律拒绝。
// 只做单步守卫会让 collect→verify→done 这种跳步轨迹通过（审查实测）。
bool allowed_transition(Stage from, Stage to) {
  if (to == Stage::GiveUp) return true;
  switch (from) {
    case Stage::Collect: return to == Stage::Diagnose;
    case Stage::Diagnose: return to == Stage::Repair;
    case Stage::Repair: return to == Stage::Verify;
    case Stage::Verify: return to == Stage::Done || to == Stage::Repair;
    case Stage::Done:
    case Stage::GiveUp: return false;
  }
  return false;
}

// 严格解析 next：所有输入来自模型（未受信），类型不符只能当拒绝，不能抛异常。
std::expected<Stage, std::string> parse_next(const json& args) {
  if (!args.is_object()) return std::unexpected("stage_complete 的参数必须是 JSON object");
  if (!args.contains("next") || !args["next"].is_string()) {
    return std::unexpected("缺少 next 或 next 不是字符串");
  }
  const std::string next = args["next"].get<std::string>();
  if (next == "diagnose") return Stage::Diagnose;
  if (next == "repair") return Stage::Repair;
  if (next == "verify") return Stage::Verify;
  if (next == "done") return Stage::Done;
  if (next == "give_up") return Stage::GiveUp;
  return std::unexpected("next 取值非法: " + next);
}

// 解析 diagnose→repair 的诊断三件套：类型不符/枚举非法/证据为空都只当拒绝。
std::expected<Diagnosis, std::string> parse_diagnosis(const json& args) {
  if (!args.contains("root_cause") || !args["root_cause"].is_string()) {
    return std::unexpected("root_cause 缺失或不是字符串");
  }
  const std::string rc = args["root_cause"].get<std::string>();
  if (!valid_root_cause(rc)) return std::unexpected("root_cause 不是合法枚举值");

  if (!args.contains("evidence") || !args["evidence"].is_array()) {
    return std::unexpected("evidence 必须是字符串数组");
  }
  std::vector<std::string> ev;
  for (const auto& e : args["evidence"]) {
    if (!e.is_string()) return std::unexpected("evidence 必须是字符串数组");
    ev.push_back(e.get<std::string>());
  }
  if (ev.empty()) return std::unexpected("evidence 为空，修复前必须给出证据");

  Diagnosis d;
  d.root_cause = rc;
  d.evidence = std::move(ev);
  if (args.contains("confidence")) {
    if (!args["confidence"].is_number()) return std::unexpected("confidence 必须是数字");
    d.confidence = args["confidence"].get<double>();
  }
  return d;
}

std::string stage_hint(Stage s) {
  switch (s) {
    case Stage::Collect:
      return "当前阶段 collect：先用 query_metrics / fetch_observation 了解现状（至少一次成功），"
             "然后调用 stage_complete，参数 {\"next\":\"diagnose\"}。";
    case Stage::Diagnose:
      return "当前阶段 diagnose：根据观测给出根因（枚举值）与证据（引用真实观测片段）。"
             "完成后调用 stage_complete，参数 {\"next\":\"repair\",\"root_cause\":\"...\","
             "\"evidence\":[\"...\"],\"confidence\":0.8}。";
    case Stage::Repair:
      return "当前阶段 repair：执行 mutating 工具调整参数（至少一次成功后），"
             "然后调用 stage_complete，参数 {\"next\":\"verify\"}。";
    case Stage::Verify:
      return "当前阶段 verify：查一次指标并与目标对比。若已满足目标（例如 p99 正常、dropped 为 0），"
             "调用 stage_complete 参数 {\"next\":\"done\"}（harness 会重新实测，不达标会被拒绝）；"
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
  // 所有输入都来自模型（未受信）；任何异常必须在这里收口，不能穿出 Runner（SPEC §9）
  try {
    return run_impl(goal, budget, on_event);
  } catch (const std::exception& e) {
    RunResult r;
    r.final_stage = Stage::GiveUp;
    r.error = std::string("runner exception: ") + e.what();
    return r;
  } catch (...) {
    RunResult r;
    r.final_stage = Stage::GiveUp;
    r.error = "runner unknown exception";
    return r;
  }
}

RunResult Runner::run_impl(const std::string& goal, const Budget& budget,
                           const std::function<void(const Event&)>& on_event) {
  RunResult r;
  Stage stage = Stage::Collect;
  bool used_observe = false;
  bool used_mutating = false;
  bool verified_ok = false;
  // verify→done 时 harness 是否真的重测过（事件里要能区分"实测"与"自称"）
  std::string verify_kind = "model_claimed";
  // SPEC §7.5 是"连续 2 次守卫拒绝"，成功迁移即清零
  int consecutive_rejects = 0;
  // 退出原因：预算耗尽/异常都必须体现为 GiveUp + 非空 error（否则评测会把
  // "没跑完"当成"答错"计入成功率分母，审查实测过这个污染路径）。
  std::string stop_reason;
  const int64_t t_start = now_us();
  const int64_t wall_limit_us = int64_t(budget.max_wall_s) * 1'000'000LL;
  // 成本单价：input/output 分开计价（未配置时回落到统一单价）。估算，非账单。
  const double price_in = cfg_.prompt_yuan_per_mtok > 0 ? cfg_.prompt_yuan_per_mtok : cfg_.yuan_per_mtok;
  const double price_out =
      cfg_.completion_yuan_per_mtok > 0 ? cfg_.completion_yuan_per_mtok : cfg_.yuan_per_mtok;

  auto emit = [&](const char* type, json data) {
    if (on_event) on_event(Event{type, std::move(data)});
  };

  std::vector<llm::ChatMessage> msgs;
  msgs.push_back(msg("system", cfg_.system_prompt));
  msgs.push_back(msg("user", goal));
  emit("run.start", {{"goal", goal}, {"budget", {{"max_turns", budget.max_turns}}}});

  while (r.turns < budget.max_turns) {
    // 墙钟预算：每轮开头查，并把"剩余预算"作为本次 LLM 调用的硬超时，
    // 否则一次 60 s 的调用就能把 max_wall_s 顶穿（审查 P2）。
    const int64_t elapsed_us = now_us() - t_start;
    if (elapsed_us > wall_limit_us) {
      stop_reason = "wall clock budget exhausted";
      break;
    }
    const int remaining_s = static_cast<int>((wall_limit_us - elapsed_us + 999'999) / 1'000'000LL);

    llm::ChatRequest req;
    req.model = cfg_.model;
    req.messages = msgs;
    req.tools = tools_.schema();
    req.tools.push_back(stage_complete_schema());
    req.max_tokens = cfg_.max_tokens;
    req.timeout_s = remaining_s > 0 ? remaining_s : 1;
    // 阶段提示作为最后一条 user 消息里追加（不污染历史）
    req.messages.push_back(msg("user", stage_hint(stage)));
    // 上下文预算：超限丢最旧观测，保留 system 与最近若干条
    {
      ContextBudget cb;
      cb.max_tokens = cfg_.max_context_tokens;
      cb.k = ctx_k_;
      // tools schema 每轮都发，必须单独计入（漏算会系统性低估预算）
      cb.reserve_tokens = estimate_tokens(req.tools);
      const TrimResult tr = trim_report(req.messages, cb);
      if (tr.dropped > 0 || tr.truncated > 0) {
        emit("context.trim", {{"dropped", tr.dropped},
                              {"truncated", tr.truncated},
                              {"est_tokens", estimate_tokens(req.messages)},
                              {"k", ctx_k_}});
      }
    }

    auto resp = llm_.chat(req);
    if (!resp) {
      stop_reason = resp.error();
      break;
    }
    ++r.turns;
    // SPEC §7.9：以 API usage 为真值校准估算系数（一阶平滑，限幅避免单次异常打乱预算）
    if (resp->usage.prompt_tokens > 0) {
      const std::size_t est = estimate_tokens(req.messages);
      if (est > 0) {
        const double sample = double(resp->usage.prompt_tokens) / double(est);
        ctx_k_ = std::min(5.0, std::max(0.3, 0.5 * ctx_k_ + 0.5 * sample));
      }
    }
    r.cost_yuan +=
        (double(resp->usage.prompt_tokens) * price_in + double(resp->usage.completion_tokens) * price_out) / 1e6;
    emit("llm.response",
         {{"turn", r.turns}, {"content", resp->content}, {"tool_calls", resp->tool_calls.size()}});

    if (r.cost_yuan > budget.max_yuan) {
      stop_reason = "cost budget exhausted";
      break;
    }
    // 单轮内工具 + LLM 耗时同样可能超出墙钟，出循环前再查一次
    if (now_us() - t_start > wall_limit_us) {
      stop_reason = "wall clock budget exhausted";
      break;
    }

    if (resp->truncated) {
      // finish_reason=length：输出被 max_tokens 截断，工具参数可能是半截 JSON（SPEC §7.9）。
      // 不执行本轮的调用，回灌要求模型精简后重试（算一轮，受轮次预算约束）。
      emit("llm.truncated", {{"turn", r.turns}, {"content_bytes", resp->content.size()}});
      if (!resp->content.empty()) msgs.push_back(msg("assistant", resp->content));
      msgs.push_back(msg("user", "上一次输出因 max_tokens 被截断（finish_reason=length），"
                                "工具参数可能不完整而无法执行。请精简输出后重试："
                                "一次只调用必要的工具，evidence 只摘抄关键片段。"));
      continue;
    }

    if (resp->tool_calls.empty()) {
      // 无工具调用不能推进状态（审查实测：verify 阶段模型说句"好了"就直接 done）。
      // 只算一轮无效轮次并把要求回灌，由模型自己决定下一步。
      if (!resp->content.empty()) msgs.push_back(msg("assistant", resp->content));
      msgs.push_back(msg("user", "本轮没有调用任何工具，状态不会推进。请调用工具；" + stage_hint(stage)));
      emit("turn.no_tool_call", {{"turn", r.turns}, {"stage", stage_name(stage)}});
      continue;
    }

    // 把 assistant 的 tool_calls 一次性写回历史，再逐个回填 tool 结果
    {
      llm::ChatMessage m = msg("assistant", resp->content);
      m.tool_calls = resp->tool_calls;
      msgs.push_back(std::move(m));
    }

    std::size_t i = 0;
    for (; i < resp->tool_calls.size(); ++i) {
      const auto& tc = resp->tool_calls[i];
      // ---- 控制工具：阶段推进 ----
      if (tc.name == "stage_complete") {
        json args = json::parse(tc.arguments, nullptr, false);
        if (args.is_discarded()) args = json::object();
        std::string reject;

        const auto target = parse_next(args);
        if (!target) {
          reject = target.error();
        } else if (!allowed_transition(stage, *target)) {
          reject = std::string("非法阶段迁移 ") + stage_name(stage) + " → " + stage_name(*target) +
                   "（只允许 collect→diagnose→repair→verify→done，verify 可回 repair）";
        }

        // 各目标阶段自己的守卫
        if (reject.empty()) {
          switch (*target) {
            case Stage::Diagnose:
              if (!used_observe) reject = "尚未采集到任何成功的观测，不能进入 diagnose";
              break;
            case Stage::Repair: {
              // verify→repair 不带诊断；只有 diagnose→repair 才写入诊断
              if (stage == Stage::Diagnose) {
                const auto d = parse_diagnosis(args);
                if (!d) {
                  reject = d.error();
                } else {
                  r.diagnosis = *d;  // 只写一次：后续 verify→repair 不会覆盖已确认的诊断
                }
              }
              break;
            }
            case Stage::Verify:
              if (!used_mutating) reject = "尚未成功执行任何非只读工具，不能进入 verify";
              break;
            case Stage::Done:
              // SPEC §7.5：harness 重新测量并满足 SLA，不信模型自称
              if (cfg_.verify_sla) {
                verify_kind = "measured";
                const bool ok = cfg_.verify_sla(r.diagnosis);
                emit("verify.measured", {{"pass", ok}});
                if (ok) {
                  verified_ok = true;
                } else {
                  reject = "harness 实测未达标（SLA 未满足），不能进入 done：请继续 verify→repair 修复";
                }
              } else {
                // 未配置重测回调：保留旧行为，但事件里明确标注"自称"
                verified_ok = true;
              }
              break;
            case Stage::GiveUp:
            case Stage::Collect:
              break;
          }
        }

        if (!reject.empty()) {
          ++consecutive_rejects;
          const json next_j = args.contains("next") ? args["next"] : json(nullptr);
          emit("guard.reject", {{"next", next_j}, {"reason", reject}});
          msgs.push_back(tool_msg("拒绝：" + reject, tc.id, "stage_complete"));
          if (consecutive_rejects >= 2) {  // SPEC §7.5：连续 2 次守卫拒绝 → give_up
            // 错误串保留 "too many guard rejects" 前缀：src/eval/eval.cpp 的
            // classify_run_error 按这个子串分类为 agent_guard_loop（本轮不能改 eval/*）
            stop_reason = "too many guard rejects (consecutive 2)";
            stage = Stage::GiveUp;
            emit("stage.enter", {{"stage", stage_name(stage)}, {"reason", stop_reason}});
            ++i;
            break;
          }
          continue;
        }

        stage = *target;
        consecutive_rejects = 0;
        {
          json data{{"stage", stage_name(stage)}};
          if (stage == Stage::Done) data["verify"] = verify_kind;
          emit("stage.enter", data);
        }
        msgs.push_back(tool_msg("ok, now in stage " + std::string(stage_name(stage)), tc.id, "stage_complete"));
        if (stage == Stage::Done || stage == Stage::GiveUp) {
          ++i;
          break;
        }
        continue;
      }

      // ---- 普通工具：审批 + 校验 + 执行 ----
      json args = json::parse(tc.arguments, nullptr, false);
      if (args.is_discarded()) {
        msgs.push_back(tool_msg("参数不是合法 JSON", tc.id, tc.name));
        continue;
      }

      // 风险/审批策略只在 Registry::call_guarded 里实现一处——任何新调用点都不会绕过审批
      const auto g = tools_.call_guarded(tc.name, args, cfg_.ctx, [&](tool::Risk risk) {
        emit("tool.call", {{"tool", tc.name}, {"risk", tool::risk_name(risk)}, {"args", args}});
      });

      if (g.status == tool::GuardedCall::Status::ApprovalDenied) {
        emit("approval.denied", {{"tool", tc.name}, {"risk", tool::risk_name(g.risk)}});
        msgs.push_back(tool_msg(g.error, tc.id, tc.name));
        continue;
      }
      if (g.status == tool::GuardedCall::Status::Failed) {
        emit("tool.error", {{"tool", tc.name}, {"error", g.error}});
        msgs.push_back(tool_msg("错误：" + g.error, tc.id, tc.name));
        continue;
      }

      emit("tool.result", {{"tool", tc.name}, {"ok", g.result.ok}, {"data", g.result.data}});
      // 失败的观测不算观测（cued 未收到流时 query_metrics 返回 ok=false）
      if (tools_.is_observation_tool(tc.name) && g.result.ok) used_observe = true;
      // 「修复动作」= 任何非只读工具成功：dangerous（如 inject_fault）同样是改状态，
      // 只算 Mutating 会让「靠取消注入完成修复」的合理轨迹被守卫永远卡在 repair。
      if (g.risk != tool::Risk::ReadOnly && g.result.ok) used_mutating = true;

      msgs.push_back(tool_msg(g.result.content, tc.id, tc.name));
    }

    // 状态已终结时，同批剩余 tool_call 不再执行，但每条都必须回填 tool 消息，
    // 否则 assistant(tool_calls) 后面缺 tool 结果，事件流/历史都是坏的配对。
    if (stage == Stage::Done || stage == Stage::GiveUp) {
      for (; i < resp->tool_calls.size(); ++i) {
        msgs.push_back(
            tool_msg("未执行：本阶段已结束", resp->tool_calls[i].id, resp->tool_calls[i].name));
      }
      break;
    }
  }

  if (r.turns >= budget.max_turns && stop_reason.empty() && stage != Stage::Done &&
      stage != Stage::GiveUp) {
    stop_reason = "turn budget exhausted";
  }

  r.final_stage = stage;
  if (r.final_stage != Stage::Done && r.final_stage != Stage::GiveUp) {
    // 没跑完必须显式失败：静默停在中间态会让评测把预算耗尽当成"答错"
    r.final_stage = Stage::GiveUp;
    if (stop_reason.empty()) stop_reason = "run ended without reaching done";
  }
  // done 必须真的过守卫（无 verify 的 done 是自相矛盾的成功）
  if (r.final_stage == Stage::Done && !verified_ok) {
    r.final_stage = Stage::GiveUp;
    stop_reason = "done without verify";
  }
  r.error = stop_reason;
  emit("run.end", {{"stage", stage_name(r.final_stage)},
                   {"turns", r.turns},
                   {"cost_yuan", r.cost_yuan},
                   {"error", r.error}});
  return r;
}

}  // namespace cue::agent
