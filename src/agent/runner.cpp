#include <algorithm>
#include <chrono>
#include <limits>
#include <cstdio>
#include <expected>
#include <string>
#include <utility>
#include <vector>

#include "cue/agent/agent.h"
#include "cue/agent/context.h"
#include "cue/agent/session.h"
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
  auto wall_started = std::chrono::steady_clock::now();
  int64_t resumed_wall_us = 0;
  const int64_t wall_limit_us = int64_t(budget.max_wall_s) * 1'000'000LL;
  // 成本单价：input/output 分开计价（未配置时回落到统一单价）。估算，非账单。
  const double price_in = cfg_.prompt_yuan_per_mtok > 0 ? cfg_.prompt_yuan_per_mtok : cfg_.yuan_per_mtok;
  const double price_out =
      cfg_.completion_yuan_per_mtok > 0 ? cfg_.completion_yuan_per_mtok : cfg_.yuan_per_mtok;

  auto emit = [&](const char* type, json data) {
    if (on_event) on_event(Event{type, std::move(data)});
  };
  // AC-8：外部取消（SIGTERM）只看这个原子标志。取消不是失败重试、不是 guard 拒绝：
  // 不消耗轮次/成本预算，也不写 done。
  auto cancelled = [this] {
    return cfg_.cancel != nullptr && cfg_.cancel->load(std::memory_order_relaxed);
  };

  // ---- 会话持久化（FR-AGENT.5）----
  // 写盘失败只记一次并报事件：它不能把已经跑完的诊断判成失败，但绝不能静默——
  // "用户以为有存档、崩溃后什么都没有"是最坏的结果。
  auto note_write = [&](std::expected<void, std::string>&& w) {
    if (w) return;
    if (r.session_error.empty()) {
      r.session_error = std::move(w.error());
      emit("session.write_error", {{"error", r.session_error}});
    }
  };
  auto write = [&](auto&& fn) {
    if (cfg_.session == nullptr) return;
    note_write(fn(*cfg_.session));
  };
  // Account for saved wall time without shifting a timepoint into the distant
  // past. Saturate only an exhausted counter, never wrap it into a new budget.
  auto wall_us = [&]() -> int64_t {
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - wall_started).count();
    if (elapsed > std::numeric_limits<int64_t>::max() - resumed_wall_us)
      return std::numeric_limits<int64_t>::max();
    return resumed_wall_us + elapsed;
  };
  auto progress = [&] { return SessionProgress{r.turns, r.cost_yuan, wall_us()}; };

  std::vector<llm::ChatMessage> msgs;
  // 所有入历史的路径都走 push：进上下文的同时落盘。漏掉任何一处，恢复后模型看到的
  // 世界就和中断前不一致（尤其 system prompt 与 tool 结果的配对）。
  auto push = [&](llm::ChatMessage m) {
    write([&](SessionLog& s) { return s.write_message(m); });
    msgs.push_back(std::move(m));
  };

  std::string goal_eff = goal;
  if (cfg_.resume != nullptr) {
    const LoadedSession& rs = *cfg_.resume;
    // 防御：已结束的会话不该走到这里（open_session 会拒绝），但 Runner 不能依赖调用方
    if (rs.stage == Stage::Done || rs.stage == Stage::GiveUp || rs.messages.empty()) {
      r.final_stage = Stage::GiveUp;
      if (rs.messages.empty()) {
        r.error = "session 里没有可恢复的对话（缺 system/user）";
      } else {
        r.error = std::string("该会话已结束（阶段 ") + stage_name(rs.stage) + "），拒绝恢复";
      }
      emit("session.resume_failed", {{"error", r.error}});
      return r;
    }
    msgs = rs.messages;
    stage = rs.stage;
    // 预算续算：轮次与成本是精确值（session 里逐轮记的），不是从头开始
    r.turns = rs.progress.turns;
    r.cost_yuan = rs.progress.cost_yuan;
    used_observe = rs.used_observe;
    used_mutating = rs.used_mutating;
    consecutive_rejects = rs.consecutive_rejects;
    r.diagnosis = rs.diagnosis;
    if (rs.meta.contains("goal") && rs.meta["goal"].is_string()) {
      goal_eff = rs.meta["goal"].get<std::string>();
    }
    resumed_wall_us = std::max<int64_t>(0, rs.progress.wall_us);
    wall_started = std::chrono::steady_clock::now();
    // 中断在「assistant(tool_calls) 已落盘、tool 结果还没落盘」之间：补合成结果并落盘，
    // 否则历史里有一个没回填的 tool_call，下一次请求直接被 API 400 拒掉。
    for (const auto& synthetic : rs.synthetic_tail) {
      write([&](SessionLog& s) { return s.write_message(synthetic, true); });
      msgs.push_back(synthetic);
    }
    emit("session.resumed", {{"path", rs.path},
                             {"from_stage", stage_name(stage)},
                             {"turns", r.turns},
                             {"cost_yuan", r.cost_yuan},
                             {"messages", static_cast<int>(msgs.size())}});
  } else {
    // 新会话：meta 先落盘（含**当时那份** system prompt 与 goal，恢复时以此为准）
    write([&](SessionLog& s) {
      return s.write_meta(cfg_.model, cfg_.session_mode, cfg_.system_prompt, goal);
    });
    push(msg("system", cfg_.system_prompt));
    push(msg("user", goal));
  }
  emit("run.start", {{"goal", goal_eff},
                     {"budget", {{"max_turns", budget.max_turns}}},
                     {"stage", stage_name(stage)},
                     {"resumed", cfg_.resume != nullptr}});

  // Check saved usage before converting to the clock's (typically nanosecond)
  // duration: an already-exhausted int64 counter must not overflow on conversion.
  const auto run_deadline = resumed_wall_us >= wall_limit_us
      ? std::chrono::steady_clock::time_point::min()
      : wall_started + std::chrono::microseconds(wall_limit_us - resumed_wall_us);
  while (r.turns < budget.max_turns) {
    // 每轮开始先看取消：预算检查在它之后，取消优先（否则会被报成预算耗尽）
    if (cancelled()) {
      stop_reason = "cancelled";
      break;
    }
    // 墙钟预算：每轮开头查，并把"剩余预算"作为本次 LLM 调用的硬超时，
    // 否则一次 60 s 的调用就能把 max_wall_s 顶穿（审查 P2）。
    const int64_t elapsed_us = wall_us();
    if (elapsed_us >= wall_limit_us) {
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
    req.deadline = run_deadline;
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
        // 既有字段名保持不变（报告/cassette 有依赖），只新增下面独立的 overflow 事件
        emit("context.trim", {{"dropped", tr.dropped},
                              {"truncated", tr.truncated},
                              {"est_tokens", estimate_tokens(req.messages)},
                              {"k", ctx_k_}});
      }
      // 出口（N-13）：裁完仍超预算就**不能再发这次请求**。此前没有这个判定，
      // 极端情况下会带着超预算的请求继续，只能等模型侧 400——既没有事件，也没停 run。
      // system 提示与当前阶段目标（首条 user、以及本轮追加在末尾的 stage_hint）都不在
      // 裁剪候选集里，这里同样不允许为了让估算过关去截断它们。
      // 事件字段：est_tokens 是生效估算（含 k、1.2、reserve），故 over_by == est_tokens - max_tokens。
      if (tr.still_over) {
        emit("context.overflow", {{"est_tokens", effective_tokens(req.messages, cb)},
                                  {"max_tokens", cb.max_tokens},
                                  {"reserve", cb.reserve_tokens},
                                  {"dropped", tr.dropped},
                                  {"truncated", tr.truncated},
                                  {"over_by", tr.over_by}});
        stop_reason = "context_overflow";
        break;
      }
    }

    // 把取消标志交给传输层：curl 只在 progress 回调里能看见它，否则会一路等到
    // timeout_s（默认 60 s）并且还会重试 2 次——一次取消被拖成三分钟（AC-8 破防点）
    req.cancel = cfg_.cancel;
    if (cancelled()) {
      stop_reason = "cancelled";
      break;
    }
    if (std::chrono::steady_clock::now() >= run_deadline) {
      stop_reason = "wall clock budget exhausted";
      break;
    }
    auto resp = llm_.chat(req);
    if (!resp) {
      if (cancelled()) stop_reason = "cancelled";
      else if (std::chrono::duration_cast<std::chrono::milliseconds>(
                   run_deadline - std::chrono::steady_clock::now()).count() <= 0)
        stop_reason = "wall clock budget exhausted";
      else stop_reason = resp.error();
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
    // 预算落盘放在这里：轮次与成本已经结算，崩溃后从这份累计值接着算
    write([&](SessionLog& s) {
      return s.write_budget(progress(), used_observe, used_mutating, consecutive_rejects);
    });
    emit("llm.response",
         {{"turn", r.turns}, {"content", resp->content}, {"tool_calls", resp->tool_calls.size()}});

    if (r.cost_yuan > budget.max_yuan) {
      stop_reason = "cost budget exhausted";
      break;
    }
    // 单轮内工具 + LLM 耗时同样可能超出墙钟，出循环前再查一次
    if (wall_us() >= wall_limit_us) {
      stop_reason = "wall clock budget exhausted";
      break;
    }

    if (resp->truncated) {
      // finish_reason=length：输出被 max_tokens 截断，工具参数可能是半截 JSON（SPEC §7.9）。
      // 不执行本轮的调用，回灌要求模型精简后重试（算一轮，受轮次预算约束）。
      emit("llm.truncated", {{"turn", r.turns}, {"content_bytes", resp->content.size()}});
      if (!resp->content.empty()) push(msg("assistant", resp->content));
      push(msg("user", "上一次输出因 max_tokens 被截断（finish_reason=length），"
                       "工具参数可能不完整而无法执行。请精简输出后重试："
                       "一次只调用必要的工具，evidence 只摘抄关键片段。"));
      continue;
    }

    if (resp->tool_calls.empty()) {
      // 无工具调用不能推进状态（审查实测：verify 阶段模型说句"好了"就直接 done）。
      // 只算一轮无效轮次并把要求回灌，由模型自己决定下一步。
      if (!resp->content.empty()) push(msg("assistant", resp->content));
      push(msg("user", "本轮没有调用任何工具，状态不会推进。请调用工具；" + stage_hint(stage)));
      emit("turn.no_tool_call", {{"turn", r.turns}, {"stage", stage_name(stage)}});
      continue;
    }

    // 把 assistant 的 tool_calls 一次性写回历史，再逐个回填 tool 结果
    {
      llm::ChatMessage m = msg("assistant", resp->content);
      m.tool_calls = resp->tool_calls;
      push(std::move(m));
    }

    std::size_t i = 0;
    for (; i < resp->tool_calls.size(); ++i) {
      // 工具之间也查取消：一个 run_shell 之类的长工具不该把退出拖到 5 秒之外
      if (cancelled()) break;
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
          // 守卫拒绝也要留痕：恢复后必须能解释"当时为什么没进入下一阶段"
          const std::string to_name =
              next_j.is_string() ? next_j.get<std::string>() : std::string("?");
          write([&](SessionLog& s) {
            return s.write_stage(stage, to_name, false, reject, progress(), used_observe,
                                 used_mutating, consecutive_rejects, r.diagnosis);
          });
          push(tool_msg("拒绝：" + reject, tc.id, "stage_complete"));
          if (consecutive_rejects >= 2) {  // SPEC §7.5：连续 2 次守卫拒绝 → give_up
            // 错误串保留 "too many guard rejects" 前缀：src/eval/eval.cpp 的
            // classify_run_error 按这个子串分类为 agent_guard_loop（本轮不能改 eval/*）
            stop_reason = "too many guard rejects (consecutive 2)";
            // 这一次迁移是 harness 强制的，不是模型请求的：forced 让会话审计能区分两者；
            // from 必须是**迁移前**的阶段（先落盘再改 stage，否则记录里是 give_up→give_up）
            write([&](SessionLog& s) {
              return s.write_stage(stage, "give_up", false, stop_reason, progress(), used_observe,
                                   used_mutating, consecutive_rejects, r.diagnosis,
                                   /*forced=*/true);
            });
            stage = Stage::GiveUp;
            emit("stage.enter", {{"stage", stage_name(stage)}, {"reason", stop_reason}});
            ++i;
            break;
          }
          continue;
        }

        // from 必须是迁移**前**的阶段：先记下再改 stage，否则记录里永远是 X→X
        const Stage from_stage = stage;
        stage = *target;
        consecutive_rejects = 0;
        write([&](SessionLog& s) {
          return s.write_stage(from_stage, stage_name(stage), true, "", progress(), used_observe,
                               used_mutating, consecutive_rejects, r.diagnosis);
        });
        {
          json data{{"stage", stage_name(stage)}};
          if (stage == Stage::Done) data["verify"] = verify_kind;
          emit("stage.enter", data);
        }
        push(tool_msg("ok, now in stage " + std::string(stage_name(stage)), tc.id, "stage_complete"));
        if (stage == Stage::Done || stage == Stage::GiveUp) {
          ++i;
          break;
        }
        continue;
      }

      // ---- 普通工具：审批 + 校验 + 执行 ----
      json args = json::parse(tc.arguments, nullptr, false);
      if (args.is_discarded()) {
        push(tool_msg("参数不是合法 JSON", tc.id, tc.name));
        continue;
      }

      // 风险/审批策略只在 Registry::call_guarded 里实现一处——任何新调用点都不会绕过审批
      const int64_t t_tool = now_us();
      const auto g = tools_.call_guarded(tc.name, args, cfg_.ctx, [&](tool::Risk risk) {
        emit("tool.call", {{"tool", tc.name}, {"risk", tool::risk_name(risk)}, {"args", args}});
      });
      const int64_t tool_us = now_us() - t_tool;

      // GuardedCall 的构造保证：Executed ⇔ outcome 有值，其余 ⇔ outcome 是 error。
      // 所以这里按 status 取对应那一侧，不做二次判断。
      const std::string err_text = g.outcome ? std::string() : g.outcome.error().message;
      const auto err_kind =
          g.outcome ? tool::ToolError::Kind::Execution : g.outcome.error().kind;
      const tool::ToolResult* ok_res = g.outcome ? &*g.outcome : nullptr;

      if (g.status == tool::GuardedCall::Status::ApprovalDenied) {
        write([&](SessionLog& s) {
          return s.write_tool(tc.name, args, false, err_text, tool_us, /*approval_denied=*/true,
                              tool::risk_name(g.risk), r.turns);
        });
        emit("approval.denied", {{"tool", tc.name}, {"risk", tool::risk_name(g.risk)}});
        push(tool_msg(err_text, tc.id, tc.name));
        continue;
      }
      if (g.status == tool::GuardedCall::Status::Failed) {
        write([&](SessionLog& s) {
          return s.write_tool(tc.name, args, false, err_text, tool_us, /*approval_denied=*/false,
                              tool::risk_name(g.risk), r.turns);
        });
        // 分类要进事件与回灌文本：模型看到 "execution" 会改参数，
        // 看到 "sandbox" 才知道该换命令，看到 "timeout" 会缩小范围。
        emit("tool.error", {{"tool", tc.name},
                            {"error", err_text},
                            {"kind", tool::tool_error_kind_name(err_kind)}});
        push(tool_msg(std::string("错误（") + tool::tool_error_kind_name(err_kind) + "）：" +
                          err_text,
                      tc.id, tc.name));
        continue;
      }

      write([&](SessionLog& s) {
        return s.write_tool(tc.name, args, true, ok_res->content, tool_us,
                            /*approval_denied=*/false, tool::risk_name(g.risk), r.turns);
      });
      emit("tool.result", {{"tool", tc.name}, {"ok", true}, {"data", ok_res->data}});
      // 失败的观测不算观测（cued 未收到流时 query_metrics 返回 ok=false）
      if (tools_.is_observation_tool(tc.name)) {
        used_observe = true;
        // 观测快照单独落盘：判分要校验"证据在真实观测里存在"，而对话里的 tool 消息
        // 可能被上下文裁剪或截断，不能当审计真值。
        ObservationRecord ob;
        ob.tool = tc.name;
        ob.args = args;
        ob.text = ok_res->content;
        ob.data = ok_res->data;
        ob.turn = r.turns;
        write([&](SessionLog& s) { return s.write_observation(ob); });
      }
      // 「修复动作」= 任何非只读工具成功：dangerous（如 inject_fault）同样是改状态，
      // 只算 Mutating 会让「靠取消注入完成修复」的合理轨迹被守卫永远卡在 repair。
      if (g.risk != tool::Risk::ReadOnly) used_mutating = true;

      push(tool_msg(ok_res->content, tc.id, tc.name));
    }

    // 守卫标志可能在同一轮里变化（第一次成功观测 / 第一次写操作）：立刻落盘，
    // 否则恢复时守卫会拿一个过时的标志去判断"能不能进 diagnose/verify"。
    write([&](SessionLog& s) {
      return s.write_budget(progress(), used_observe, used_mutating, consecutive_rejects);
    });

    // 取消发生在工具之间：同批剩余 tool_call 不回填（run 到此结束，不会再有下一次
    // LLM 调用需要这份历史），直接以 cancelled 收口
    if (cancelled()) {
      stop_reason = "cancelled";
      break;
    }

    // 状态已终结时，同批剩余 tool_call 不再执行，但每条都必须回填 tool 消息，
    // 否则 assistant(tool_calls) 后面缺 tool 结果，事件流/历史都是坏的配对。
    if (stage == Stage::Done || stage == Stage::GiveUp) {
      for (; i < resp->tool_calls.size(); ++i) {
        push(tool_msg("未执行：本阶段已结束", resp->tool_calls[i].id, resp->tool_calls[i].name));
      }
      break;
    }
  }

  if (r.turns >= budget.max_turns && stop_reason.empty() && stage != Stage::Done &&
      stage != Stage::GiveUp) {
    stop_reason = "turn budget exhausted";
  }

  r.final_stage = stage;
  // 取消的 run 不能报 done：done 是「harness 实测达标」的结论，取消时没有结论
  if (stop_reason == "cancelled") r.final_stage = Stage::GiveUp;
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
  // end 记录：cancelled 的 end 仍标记 resumable（取消只是这次 run 被叫停），
  // 其余结束原因把会话封口
  write([&](SessionLog& s) { return s.write_end(r.final_stage, stop_reason, progress()); });
  emit("run.end", {{"stage", stage_name(r.final_stage)},
                   {"turns", r.turns},
                   {"cost_yuan", r.cost_yuan},
                   {"error", r.error}});
  return r;
}

}  // namespace cue::agent
