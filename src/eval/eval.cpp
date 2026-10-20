#include "cue/eval/eval.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>
#include <utility>

#include "cue/common/clock.h"
#include "cue/llm/client.h"
#include "cue/pipeline/tools.h"

namespace cue::eval {

namespace {

std::string lower_trim(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  bool prev_space = false;
  for (char ch : s) {
    const unsigned char u = static_cast<unsigned char>(ch);
    if (std::isspace(u)) {
      if (!prev_space) out.push_back(' ');
      prev_space = true;
      continue;
    }
    prev_space = false;
    out.push_back(static_cast<char>(std::tolower(u)));
  }
  return out;
}

// 提取 "key=数字" 里的数字（判分时容忍单位后缀）
std::vector<std::string> extract_numbers(const std::string& s) {
  std::vector<std::string> nums;
  std::string cur;
  for (char ch : s) {
    if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '.' || ch == '-') {
      cur.push_back(ch);
    } else {
      if (cur.size() >= 2) nums.push_back(cur);
      cur.clear();
    }
  }
  if (cur.size() >= 2) nums.push_back(cur);
  return nums;
}

}  // namespace

bool evidence_matches(const std::string& evidence, const std::vector<Observation>& obs) {
  const std::string ev = lower_trim(evidence);
  if (ev.empty()) return false;
  for (const auto& o : obs) {
    const std::string text = lower_trim(o.text);
    if (text.empty()) continue;
    // 1) 直接子串关系（证据引用了观测原文，或观测包含了证据的核心片段）
    if (text.find(ev) != std::string::npos) return true;
    if (ev.find(text) != std::string::npos && text.size() >= 8) return true;
    // 2) 数值命中：证据里出现的每个数字都必须能在该观测里找到（且至少一个 >=2 位）
    const auto nums = extract_numbers(ev);
    if (!nums.empty()) {
      bool all_found = true;
      for (const auto& n : nums) {
        if (text.find(n) == std::string::npos) {
          all_found = false;
          break;
        }
      }
      if (all_found) return true;
    }
  }
  return false;
}

Score judge_case(const Case& c, const agent::RunResult& res, const std::vector<Observation>& obs,
                 const pipeline::PipelineMetrics& m) {
  Score s;
  s.root = !c.expect_root_cause.empty() && res.diagnosis.root_cause == c.expect_root_cause;

  if (!res.diagnosis.evidence.empty()) {
    s.evidence = true;
    for (const auto& ev : res.diagnosis.evidence) {
      if (!evidence_matches(ev, obs)) {
        s.evidence = false;
        break;
      }
    }
  }

  s.sla = true;
  if (c.sla_fps_min > 0 && m.fps < c.sla_fps_min) s.sla = false;
  if (m.dropped > c.sla_dropped_max) s.sla = false;
  // 没有采样 = 无法证明达成，按不达标处理
  if (m.samples == 0) s.sla = false;
  return s;
}

Case parse_case(const json& j, const std::string& suite_name, std::string& err) {
  Case c;
  c.suite = suite_name;
  if (!j.contains("id") || !j["id"].is_string()) {
    err = "case 缺少 id";
    return c;
  }
  c.id = j["id"].get<std::string>();
  c.seed = j.value("seed", 42ULL);

  if (j.contains("fault") && j["fault"].is_object()) {
    const auto& f = j["fault"];
    c.fault_kind = f.value("kind", "none");
    if (f.contains("params") && f["params"].is_object()) {
      const auto& p = f["params"];
      if (p.contains("inject_drop_pct")) c.fault_value = p["inject_drop_pct"].get<double>();
      else if (p.contains("drop_pct")) c.fault_value = p["drop_pct"].get<double>();
    }
  }
  if (j.contains("pipeline") && j["pipeline"].is_object()) {
    const auto& p = j["pipeline"];
    c.transcode = p.value("transcode", false);
    c.bitrate_kbps = p.value("bitrate_kbps", 1500);
    c.gop = p.value("gop", 60);
  }
  c.prompt = j.value("prompt", std::string("定位并修复当前管线异常"));

  if (j.contains("expect") && j["expect"].is_object()) {
    const auto& e = j["expect"];
    c.expect_root_cause = e.value("root_cause", "");
    if (e.contains("evidence_candidates") && e["evidence_candidates"].is_array()) {
      for (const auto& x : e["evidence_candidates"]) c.evidence_candidates.push_back(x.get<std::string>());
    }
    if (e.contains("allowed_actions") && e["allowed_actions"].is_array()) {
      for (const auto& x : e["allowed_actions"]) c.allowed_actions.push_back(x.get<std::string>());
    }
    if (e.contains("sla") && e["sla"].is_object()) {
      const auto& s = e["sla"];
      c.sla_fps_min = s.value("fps_min", 0.0);
      if (s.contains("dropped_max")) c.sla_dropped_max = s["dropped_max"].get<uint64_t>();
    }
  }
  if (j.contains("budget") && j["budget"].is_object()) {
    const auto& b = j["budget"];
    c.max_turns = b.value("max_turns", 12);
    c.max_yuan = b.value("max_yuan", 0.3);
    c.max_wall_s = b.value("max_wall_s", 180);
  }
  return c;
}

std::vector<Case> load_suite(const std::string& path, std::string& err) {
  std::ifstream in(path);
  if (!in) {
    err = "无法打开 suite: " + path;
    return {};
  }
  std::stringstream ss;
  ss << in.rdbuf();
  auto root = json::parse(ss.str(), nullptr, false);
  if (root.is_discarded()) {
    err = "suite 不是合法 JSON: " + path;
    return {};
  }
  const std::string name = root.value("suite", std::string("unknown"));
  std::vector<Case> cases;
  if (root.contains("cases") && root["cases"].is_array()) {
    for (const auto& j : root["cases"]) {
      std::string e2;
      auto c = parse_case(j, name, e2);
      if (!e2.empty()) {
        err = e2;
        return {};
      }
      cases.push_back(std::move(c));
    }
  }
  return cases;
}

// ---------------------------------------------------------------- Engine

pipeline::ProcessPipelineControl::Config pipeline_config_for(const Case& c, const RunConfig& cfg) {
  pipeline::ProcessPipelineControl::Config pc;
  pc.cued_bin = cfg.cued_bin;
  // 注入档位只来自 case：agent 必须自己从指标里发现故障，评测器不能替它"制造"结论
  pc.inject_drop = (c.fault_kind == "drop") ? c.fault_value : 0.0;
  pc.inject_seed = c.seed;
  pc.transcode = c.transcode;
  pc.width = c.width;
  pc.height = c.height;
  pc.bitrate_kbps = c.bitrate_kbps;
  pc.gop = c.gop;
  return pc;
}

std::string classify_run_error(const agent::RunResult& res) {
  if (res.error.empty()) return {};  // 显式 give_up 仍计分（算失败，不算 run_error）
  const std::string& e = res.error;
  if (res.turns == 0) return "llm_unavailable";
  if (e.find("wall clock") != std::string::npos) return "agent_timeout";
  if (e.find("cost budget") != std::string::npos) return "agent_cost_budget";
  if (e.find("too many guard rejects") != std::string::npos) return "agent_guard_loop";
  return "agent_error";
}

namespace {

// agent 结束后等一条新采样：cued 每秒打印一次统计，agent 期间的采样可能早于修复生效
constexpr int kFreshSampleWaitMs = 6000;
constexpr int kSleepStepMs = 200;
// 超过这个年龄的采样算"陈旧"（cued 只在有媒体流时打点，流断了就不会更新）
constexpr int64_t kStaleSampleUs = 3'000'000;

}  // namespace

CaseOutcome Engine::run_scripted_case(const Case& c) const {
  // 引擎自测：用 case 的 expect 构造"正确 agent"的结论，观测由真实管线字段模拟。
  // 目的不是证明模型能力，而是验证「判分器 + 引擎」这条链路本身工作正常。
  CaseOutcome o;
  o.id = c.id;
  std::vector<Observation> obs;
  Observation ob;
  ob.tool = "query_metrics";
  ob.text = "frames_in=247 frames_out=247 dropped=0 injected=59 decoded=247 decode_errors=0 fps=30.3";
  ob.data = json{{"injected", 59}, {"dropped", 0}, {"fps", 30.3}};
  obs.push_back(ob);

  agent::RunResult fake;
  fake.final_stage = agent::Stage::Done;
  fake.turns = 6;
  fake.cost_yuan = 0.01;
  fake.diagnosis.root_cause = c.expect_root_cause;
  fake.diagnosis.evidence = {"injected=59"};

  pipeline::PipelineMetrics m;
  m.samples = 1;
  m.fps = 30.3;
  m.dropped = 0;
  o.score = judge_case(c, fake, obs, m);
  o.scored = true;
  o.turns = fake.turns;
  o.cost_yuan = fake.cost_yuan;
  o.diagnosis = fake.diagnosis;
  o.final_stage = agent::stage_name(fake.final_stage);
  o.final_fps = m.fps;
  o.final_dropped = m.dropped;
  o.final_samples = m.samples;
  return o;
}

CaseOutcome Engine::run_real_case(const Case& c) const {
  CaseOutcome o;
  o.id = c.id;
  o.final_stage = "n/a";

  const char* key = std::getenv("CUE_API_KEY");
  if (!key || !*key) {
    o.error_kind = "missing_api_key";
    return o;
  }
  const char* base_env = std::getenv("CUE_BASE_URL");
  const std::string base_url = (base_env && *base_env) ? base_env : "https://api.deepseek.com";

  // 1) 起管线（cued 由被测进程管理；推流由外部浏览器/采集端提供）
  const auto pc = pipeline_config_for(c, cfg_);
  pipeline::ProcessPipelineControl pipe(pc);
  std::string err;
  std::fprintf(stderr, "[eval][%s] 启动 cued（inject_drop=%.1f%%, bitrate=%d, gop=%d）\n", c.id.c_str(),
               pc.inject_drop, pc.bitrate_kbps, pc.gop);
  if (!pipe.start(err)) {
    o.error_kind = "pipeline_start_failed: " + err;
    return o;
  }

  // 2) warmup：等外部推流起来（agent 早于推流启动会拿到"尚无采样"）
  for (int i = 0; i < cfg_.warmup_s; ++i) std::this_thread::sleep_for(std::chrono::seconds(1));
  for (int i = 0; i < 20; ++i) {
    const auto m = pipe.latest();
    if (m.samples > 0 && m.fps > 0.0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  {
    const auto m = pipe.latest();
    std::fprintf(stderr, "[eval][%s] warmup 完成：samples=%llu fps=%.1f（无采样则推流未起）\n",
                 c.id.c_str(), static_cast<unsigned long long>(m.samples), m.fps);
  }

  // 3) 工具（每个返回都留痕，证据校验只认真实观测）
  std::vector<pipeline::ToolTrace> trace;
  tool::Registry reg;
  pipeline::build_real_tools(reg, pipe, &trace);

  // 4) 真实 LLM + agent 循环
  llm::OpenAiCompatibleClient client({base_url, key, cfg_.model, 60});
  agent::Runner::Config ac;
  ac.model = cfg_.model;
  ac.system_prompt = pipeline::real_ops_system_prompt();
  ac.ctx.mode = tool::ApprovalMode::Auto;
  ac.ctx.allowed_dangerous = {"inject_fault"};
  ac.yuan_per_mtok = cfg_.yuan_per_mtok;

  agent::Runner runner(client, reg, ac);
  agent::Budget budget;
  budget.max_turns = c.max_turns;
  budget.max_yuan = c.max_yuan;
  budget.max_wall_s = c.max_wall_s;

  const std::string tag = "[eval][" + c.id + "] ";
  const auto res = runner.run(c.prompt, budget, [&tag](const agent::Event& e) {
    if (e.type == "llm.response" || e.type == "run.start") return;
    std::fprintf(stderr, "%s%s %s\n", tag.c_str(), e.type.c_str(), e.data.dump().c_str());
  });

  std::vector<Observation> obs;
  obs.reserve(trace.size());
  for (const auto& t : trace) obs.push_back(Observation{t.tool, t.text, t.data});

  // 5) harness 重测 SLA：等 agent 结束之后的一条新采样（不信 agent 自报）
  pipeline::PipelineMetrics m = pipe.latest();
  const uint64_t mark = m.samples;
  for (int waited = 0; waited < kFreshSampleWaitMs; waited += kSleepStepMs) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kSleepStepMs));
    const auto cur = pipe.latest();
    if (cur.samples > mark) {
      m = cur;
      break;
    }
  }
  const int64_t age_us = m.updated_us == 0 ? kStaleSampleUs + 1 : now_us() - m.updated_us;
  if (m.samples == 0 || age_us > kStaleSampleUs) {
    std::fprintf(stderr, "[eval][%s] 重测无有效采样（samples=%llu, age=%lld ms）→ SLA 不可判定为达标\n",
                 c.id.c_str(), static_cast<unsigned long long>(m.samples),
                 static_cast<long long>(age_us / 1000));
    m.samples = 0;  // judge_case: 无采样 = 无法证明达成 = 不达标
  }

  o.final_fps = m.fps;
  o.final_dropped = m.dropped;
  o.final_samples = m.samples;
  o.turns = res.turns;
  o.cost_yuan = res.cost_yuan;
  o.diagnosis = res.diagnosis;
  o.final_stage = agent::stage_name(res.final_stage);

  const std::string ek = classify_run_error(res);
  if (!ek.empty()) {
    // run_error 不计入成功率分母：agent 没跑完，不是"答错"
    o.error_kind = res.error.empty() ? ek : (ek + ": " + res.error);
  } else {
    o.scored = true;
    o.score = judge_case(c, res, obs, m);
  }

  pipe.stop();  // 每个 case 独占一条管线；析构函数兜底
  return o;
}

RunReport Engine::run(const std::vector<Case>& cases) {
  RunReport r;
  r.run_id = "run-" + std::to_string(now_us());
  r.repeat = cfg_.repeat;
  r.total = static_cast<int>(cases.size()) * cfg_.repeat;
  std::printf("[eval] run_id=%s mode=%s repeat=%d\n", r.run_id.c_str(), cfg_.agent_mode.c_str(),
              cfg_.repeat);

  std::map<std::string, int> pass_count;   // case_id -> 全成功次数
  std::map<std::string, int> attempt_count;
  bool gate = false;

  for (std::size_t ci = 0; ci < cases.size() && !gate; ++ci) {
    const auto& c = cases[ci];
    for (int k = 0; k < cfg_.repeat; ++k) {
      CaseOutcome o = (cfg_.agent_mode == "real") ? run_real_case(c) : run_scripted_case(c);

      r.cost_yuan += o.cost_yuan;
      ++attempt_count[c.id];
      if (o.scored && o.score.all()) ++pass_count[c.id];
      r.cases.push_back(std::move(o));

      if (r.cost_yuan > cfg_.max_yuan) {
        r.failure_modes.push_back("cost gate triggered at " + std::to_string(r.cost_yuan));
        gate = true;
        break;
      }
    }
  }

  // 汇总
  for (const auto& o : r.cases) {
    if (!o.scored) {
      ++r.run_errors;
      r.failure_modes.push_back("run_error[" + o.id + "]: " + o.error_kind);
      continue;
    }
    ++r.scored;
    if (o.score.root) ++r.root_rate;
    if (o.score.evidence) ++r.evidence_rate;
    if (o.score.sla) ++r.sla_rate;
  }
  if (r.scored > 0) {
    r.root_rate /= r.scored;
    r.evidence_rate /= r.scored;
    r.sla_rate /= r.scored;
  }
  // pass^k：每个 case 的 k 次都成功才算 pass，整体取比例
  if (!attempt_count.empty()) {
    int pass_cases = 0;
    for (const auto& [id, attempts] : attempt_count) {
      if (pass_count[id] == attempts && attempts == cfg_.repeat) ++pass_cases;
    }
    r.pass_k = double(pass_cases) / double(attempt_count.size());
  }
  // 失败模式聚合
  std::map<std::string, int> miss;
  for (const auto& o : r.cases) {
    if (!o.scored) continue;
    if (!o.score.root) ++miss["root != " + o.diagnosis.root_cause];
    if (!o.score.evidence) ++miss["evidence 无法在观测中找到"];
    if (!o.score.sla) ++miss["SLA 未达成"];
  }
  for (const auto& [k2, v] : miss) {
    if (v > 0) r.failure_modes.push_back(k2 + " (" + std::to_string(v) + ")");
  }
  return r;
}

std::string Engine::render_markdown(const RunReport& r) const {
  std::ostringstream o;
  o << "# Cue 评测报告\n\n";
  o << "- run_id: `" << r.run_id << "`\n";
  o << "- suite: `" << cfg_.suite_path << "`\n";
  o << "- agent: `" << cfg_.agent_mode << "`，repeat=" << r.repeat << "，warmup=" << cfg_.warmup_s
    << "s\n";
  o << "- 生效配置: cued=`" << cfg_.cued_bin << "`，model=`" << cfg_.model
    << "`，成本闸门 ¥" << cfg_.max_yuan << "，单价 ¥" << cfg_.yuan_per_mtok << "/Mtok\n";
  o << "- 用例: " << r.total << "，计分 " << r.scored << "，运行失败 " << r.run_errors << "\n";
  o << "- 成本: ¥" << r.cost_yuan << "\n\n";
  o << "## 分层成功率\n\n";
  o << "| 指标 | 值 |\n|---|---|\n";
  o << "| 根因命中 | " << (r.root_rate * 100) << "% |\n";
  o << "| 证据成立 | " << (r.evidence_rate * 100) << "% |\n";
  o << "| SLA 达成 | " << (r.sla_rate * 100) << "% |\n";
  o << "| pass^" << r.repeat << " | " << (r.pass_k * 100) << "% |\n";
  if (r.total > 0) o << "| Run Error 率 | " << (100.0 * double(r.run_errors) / double(r.total)) << "% |\n";
  o << "\n## 逐用例\n\n";
  o << "| case | root | evidence | sla | root_cause | turns | 成本¥ | 重测 fps | dropped | 采样 |\n";
  o << "|---|---|---|---|---|---|---|---|---|---|\n";
  for (const auto& c : r.cases) {
    if (!c.scored) {
      o << "| " << c.id << " | — | — | — | — | " << c.turns << " | " << c.cost_yuan
        << " | run_error: " << c.error_kind << " | — | — |\n";
      continue;
    }
    o << "| " << c.id << " | " << (c.score.root ? "✓" : "✗") << " | "
      << (c.score.evidence ? "✓" : "✗") << " | " << (c.score.sla ? "✓" : "✗") << " | "
      << c.diagnosis.root_cause << " | " << c.turns << " | " << c.cost_yuan << " | " << c.final_fps
      << " | " << c.final_dropped << " | " << c.final_samples << " |\n";
  }
  if (!r.failure_modes.empty()) {
    o << "\n## 失败模式\n\n";
    for (const auto& f : r.failure_modes) o << "- " << f << "\n";
  }
  o << "\n> 说明：延迟与成功率均为本机实测；跨机场景下的端到端延迟为相对量，不含最小网络传播时间。\n";
  o << "> 判分口径：SLA 由 harness 在 agent 结束后重测（不采信 agent 自报）；run_error 不计入成功率分母。\n";
  return o.str();
}

}  // namespace cue::eval
