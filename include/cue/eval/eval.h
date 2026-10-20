// M4 评测：声明式 case + 三重判分（根因 / 证据 / SLA）
// 判分原则（SPEC §8.3）：
//  - 根因用枚举精确匹配
//  - 证据必须在真实观测里存在（防幻觉）
//  - SLA 由 harness 重新测量，不信 agent 自报
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cue/agent/agent.h"
#include "cue/pipeline/process_control.h"

namespace cue::eval {

using json = nlohmann::json;

struct Case {
  std::string id;
  std::string suite;
  uint64_t seed = 42;

  // 故障注入
  std::string fault_kind;   // "drop" / "none"
  double fault_value = 0.0;

  // 管线档位
  bool transcode = false;
  int width = 0;
  int height = 0;
  int bitrate_kbps = 1500;
  int gop = 60;

  std::string prompt;

  // 期望
  std::string expect_root_cause;
  std::vector<std::string> evidence_candidates;
  std::vector<std::string> allowed_actions;

  // SLA（harness 重测）
  double sla_fps_min = 0.0;
  uint64_t sla_dropped_max = UINT64_MAX;

  // 预算
  int max_turns = 12;
  double max_yuan = 0.3;
  int max_wall_s = 180;
};

struct Observation {
  std::string tool;
  std::string text;
  json data = json::object();
};

struct Score {
  bool root = false;
  bool evidence = false;
  bool sla = false;
  bool all() const { return root && evidence && sla; }
};

// 证据校验：evidence 文本必须与某条真实观测存在实质重叠（子串或关键数值命中）
bool evidence_matches(const std::string& evidence, const std::vector<Observation>& obs);

Score judge_case(const Case& c, const agent::RunResult& res, const std::vector<Observation>& obs,
                 const pipeline::PipelineMetrics& final_metrics);

Case parse_case(const json& j, const std::string& suite_name, std::string& err);
std::vector<Case> load_suite(const std::string& path, std::string& err);

// ---------------------------------------------------------------- Engine

struct CaseOutcome {
  std::string id;
  bool scored = false;       // false = run_error（超时/崩溃/输出不合规），不计入成功率分母
  std::string error_kind;
  Score score;
  int turns = 0;
  double cost_yuan = 0.0;
  agent::Diagnosis diagnosis;
  std::string final_stage;
};

struct RunConfig {
  std::string suite_path;
  std::string agent_mode = "scripted";  // scripted | real
  int repeat = 1;                       // pass^k 的 k
  double max_yuan = 5.0;                // 单 run 成本闸门
  std::string out_dir = "runs";
  int warmup_s = 6;                     // 管线起来后等推流的时间
  bool keep_pipeline = false;
};

struct RunReport {
  std::string run_id;
  std::vector<CaseOutcome> cases;
  int total = 0;
  int scored = 0;
  int run_errors = 0;
  double root_rate = 0;
  double evidence_rate = 0;
  double sla_rate = 0;
  double cost_yuan = 0;
  int repeat = 1;
  // pass^k：对每个 case，k 次里全部成功的比例；整体取均值
  double pass_k = 0;
  std::vector<std::string> failure_modes;  // "root=network_loss: 3 次未命中" 之类的聚合
};

class Engine {
 public:
  explicit Engine(RunConfig cfg) : cfg_(std::move(cfg)) {}
  RunReport run(const std::vector<Case>& cases);
  std::string render_markdown(const RunReport& r) const;

 private:
  RunConfig cfg_;
};

}  // namespace cue::eval
