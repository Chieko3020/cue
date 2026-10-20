#include "cue/eval/eval.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>

#include "cue/common/clock.h"
#include "cue/eval/state.h"
#include "cue/llm/client.h"
#include "cue/pipeline/tools.h"

namespace cue::eval {

namespace {

// 提取裸数字（没有 key 可挂时的兜底：证据只写了数字）
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

// SPEC §7.6 要求「归一化子串」：把 JSON 的 "k": v 与证据里的 k=v 归一成同一种写法，
// 否则模型写得再准也会因为多一个引号被判伪造。
std::string normalize(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  bool prev_space = true;
  for (char ch : s) {
    const unsigned char u = static_cast<unsigned char>(ch);
    if (std::isalnum(u) || ch == '.' || ch == '_') {
      out.push_back(static_cast<char>(std::tolower(u)));
      prev_space = false;
    } else if (!prev_space) {
      out.push_back(' ');
      prev_space = true;
    }
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

bool key_char(char ch) {
  const unsigned char u = static_cast<unsigned char>(ch);
  return std::isalnum(u) || ch == '_';
}

// 证据里的「指标引用」：key 后面跟 = : 空格 或括号，再接一个数字（如 injected=59 / "fps": 30.3）
struct Citation {
  std::string key;
  double value = 0;
};

std::vector<Citation> citations(const std::string& raw) {
  std::vector<Citation> out;
  std::size_t i = 0;
  while (i < raw.size()) {
    const unsigned char u = static_cast<unsigned char>(raw[i]);
    if (!(std::isalpha(u) || raw[i] == '_')) {
      ++i;
      continue;
    }
    const std::size_t k0 = i;
    while (i < raw.size() && key_char(raw[i])) ++i;
    std::string key;
    key.reserve(i - k0);
    for (std::size_t j = k0; j < i; ++j) {
      key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(raw[j]))));
    }
    std::size_t j = i;
    while (j < raw.size() && (std::isspace(static_cast<unsigned char>(raw[j])) || raw[j] == '=' ||
                              raw[j] == ':' || raw[j] == '"' || raw[j] == '\'' ||
                              raw[j] == '(' || raw[j] == '[')) {
      ++j;
    }
    const std::size_t num0 = j;
    if (j < raw.size() && (std::isdigit(static_cast<unsigned char>(raw[j])) || raw[j] == '-' ||
                           raw[j] == '+')) {
      ++j;
      while (j < raw.size() &&
             (std::isdigit(static_cast<unsigned char>(raw[j])) || raw[j] == '.')) {
        ++j;
      }
      try {
        out.push_back(Citation{key, std::stod(raw.substr(num0, j - num0))});
      } catch (const std::exception&) {
        // 数字解析失败就当没引用
      }
    }
    i = std::max(i, j);
  }
  return out;
}

// 计数类指标必须精确（injected=60 不能对上 injected=59），连续量按 SPEC §7.6 的 ±2% 容差
bool value_close(double a, double b) {
  const bool a_int = std::fabs(a - std::round(a)) < 1e-9;
  const bool b_int = std::fabs(b - std::round(b)) < 1e-9;
  if (a_int && b_int) return a == b;
  return std::fabs(a - b) <= std::max(0.05, 0.02 * std::fabs(b));
}

std::optional<double> data_value(const json& d, const std::string& key) {
  if (!d.is_object()) return std::nullopt;
  for (auto it = d.begin(); it != d.end(); ++it) {
    std::string k = it.key();
    for (auto& ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (k != key) continue;
    if (it.value().is_number()) return it.value().get<double>();
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<double> text_value(const std::string& norm, const std::string& key) {
  std::size_t pos = 0;
  while ((pos = norm.find(key, pos)) != std::string::npos) {
    const bool left_ok = pos == 0 || !key_char(norm[pos - 1]);
    std::size_t j = pos + key.size();
    const bool right_ok = j >= norm.size() || !key_char(norm[j]);
    if (left_ok && right_ok) {
      while (j < norm.size() && (norm[j] == ' ' || norm[j] == '=' || norm[j] == ':')) ++j;
      if (j < norm.size() && (std::isdigit(static_cast<unsigned char>(norm[j])) || norm[j] == '-')) {
        const std::size_t num0 = j;
        ++j;
        while (j < norm.size() &&
               (std::isdigit(static_cast<unsigned char>(norm[j])) || norm[j] == '.')) {
          ++j;
        }
        try {
          return std::stod(norm.substr(num0, j - num0));
        } catch (const std::exception&) {
          return std::nullopt;
        }
      }
    }
    pos += key.size();
  }
  return std::nullopt;
}

// 一条观测能否解释这条证据：
//  1) 归一化后互为子串（最强，等价于逐字引用）
//  2) 证据里的 key=value 引用全部能在该观测里找到，且数值在 ±2% 内；
//     至少 2 个引用，或者证据本身就是短小的单条片段
//  3) 兜底：证据只有裸数字，且这些数字都在观测原文里
bool matches_observation(const std::string& raw_ev, const std::string& ev_norm,
                         const std::vector<Citation>& cites, const Observation& o) {
  const std::string text = normalize(o.text);
  if (!ev_norm.empty() && text.find(ev_norm) != std::string::npos) return true;
  if (ev_norm.size() >= 8 && ev_norm.find(text) != std::string::npos) return true;

  if (!cites.empty()) {
    int checked = 0;
    for (const auto& c : cites) {
      auto actual = data_value(o.data, c.key);
      if (!actual) actual = text_value(text, c.key);
      if (!actual) continue;  // 该观测里没有这个 key（比如工具名/窗口参数）→ 不参与判定
      ++checked;
      if (!value_close(*actual, c.value)) return false;
    }
    if (checked >= 2) return true;
    if (checked == 1 && raw_ev.size() <= 40) return true;
    return false;
  }

  const auto nums = extract_numbers(ev_norm);
  if (nums.empty()) return false;
  for (const auto& n : nums) {
    if (text.find(n) == std::string::npos) return false;
  }
  return true;
}

}  // namespace

bool evidence_matches(const std::string& evidence, const std::vector<Observation>& obs) {
  const std::string ev_norm = normalize(evidence);
  if (ev_norm.empty()) return false;
  const auto cites = citations(evidence);
  for (const auto& o : obs) {
    if (o.text.empty() && !o.data.is_object()) continue;
    if (matches_observation(evidence, ev_norm, cites, o)) return true;
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

// 报告里的错误串要能放进表格（完整错误留在 state.jsonl 里可查）
std::string short_text(const std::string& s, std::size_t n = 90) {
  if (s.size() <= n) return s;
  return s.substr(0, n) + "…";
}

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
  o.observations = obs;  // 留档：证据是否成立可离线复核

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
  const bool resuming = !cfg_.resume_run_id.empty();
  r.run_id = resuming ? cfg_.resume_run_id : "run-" + std::to_string(now_us());
  r.repeat = cfg_.repeat;
  r.total = static_cast<int>(cases.size()) * cfg_.repeat;
  r.fingerprint = base_fingerprint(cfg_.suite_path);

  const std::string run_dir = cfg_.out_dir + "/" + r.run_id;
  const std::string state_path = run_dir + "/state.jsonl";

  StateMap cached;
  if (resuming) {
    std::string err;
    if (!load_state(state_path, cached, err)) {
      r.failure_modes.push_back("resume 失败：" + err);
      return r;
    }
    std::printf("[eval] 续跑 %s：已缓存 %zu 个 case outcome\n", r.run_id.c_str(), cached.size());
  } else {
    json header{{"t", "run"},
                {"run_id", r.run_id},
                {"suite", cfg_.suite_path},
                {"mode", cfg_.agent_mode},
                {"repeat", cfg_.repeat},
                {"fingerprint", r.fingerprint},
                {"created_us", now_us()}};
    std::string err;
    if (!append_state_header(state_path, header, err)) r.failure_modes.push_back("state 写入失败：" + err);
  }
  std::printf("[eval] run_id=%s state=%s fingerprint=%s\n", r.run_id.c_str(), state_path.c_str(),
              r.fingerprint.substr(0, 16).c_str());

  std::map<std::string, int> pass_count;   // case_id -> 全成功次数
  std::map<std::string, int> attempt_count;
  bool gate = false;

  for (std::size_t ci = 0; ci < cases.size() && !gate; ++ci) {
    const auto& c = cases[ci];
    const std::string fp = fingerprint_for(cfg_.suite_path, c);
    for (int k = 0; k < cfg_.repeat; ++k) {
      CaseOutcome o;
      bool reused = false;
      const auto it = cached.find({static_cast<int>(ci), k});
      if (it != cached.end()) {
        if (it->second.fingerprint == fp) {
          o = it->second.outcome;
          reused = true;
          std::printf("[eval] case %s #%d 复用缓存（不调 LLM、不计费）\n", c.id.c_str(), k);
        } else {
          std::printf("[eval] case %s #%d 指纹变化 → 缓存失效，重跑\n", c.id.c_str(), k);
        }
      }
      if (!reused) {
        o = (cfg_.agent_mode == "real") ? run_real_case(c) : run_scripted_case(c);
        std::string serr;
        if (!append_state(state_path, static_cast<int>(ci), k, o, fp, serr)) {
          r.failure_modes.push_back("state 写入失败：" + serr);
        }
      }

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
      r.failure_modes.push_back("run_error[" + o.id + "]: " + short_text(o.error_kind));
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
  o << "- 指纹: `" << r.fingerprint.substr(0, 16) << "`（二进制 ⊕ git describe ⊕ suite，SPEC §7.7）\n";
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
        << " | run_error: " << short_text(c.error_kind) << " | — | — |\n";
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
