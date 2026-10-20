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
  // harness 重测到的最终指标（SLA 判分的依据，留档便于复核）
  double final_fps = 0.0;
  uint64_t final_dropped = 0;
  uint64_t final_samples = 0;
};

struct RunConfig {
  std::string suite_path;
  std::string agent_mode = "scripted";  // scripted | real
  int repeat = 1;                       // pass^k 的 k
  double max_yuan = 5.0;                // 单 run 成本闸门
  std::string out_dir = "runs";
  int warmup_s = 6;                     // 管线起来后等推流的时间
  bool keep_pipeline = false;
  // M4b
  std::string resume_run_id;            // 非空 = 续跑该 run（复用已完成 case）
  std::string cued_bin = "./build/cued";
  std::string model = "deepseek-chat";  // real 模式模型名
  double yuan_per_mtok = 2.0;           // 成本核算单价
};

struct RunReport {
  std::string run_id;
  std::string fingerprint;  // 二进制 ⊕ git ⊕ suite（报告留档，见 SPEC §7.7/AC-9）
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

// real 模式：case → 管线配置（inject_drop 取自 case.fault_value，其余档位照抄 case）
pipeline::ProcessPipelineControl::Config pipeline_config_for(const Case& c, const RunConfig& cfg);

// real 模式错误分类：空串 = 计分；否则是 run_error 的 error_kind（不计入成功率分母）
std::string classify_run_error(const agent::RunResult& res);

class Engine {
 public:
  explicit Engine(RunConfig cfg) : cfg_(std::move(cfg)) {}
  RunReport run(const std::vector<Case>& cases);
  std::string render_markdown(const RunReport& r) const;

 private:
  CaseOutcome run_scripted_case(const Case& c) const;
  CaseOutcome run_real_case(const Case& c) const;
  RunConfig cfg_;
};

}  // namespace cue::eval
