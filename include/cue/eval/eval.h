// M4 评测：声明式 case + 三重判分（根因 / 证据 / SLA）
// 判分原则（SPEC §8.3）：
//  - 根因用枚举精确匹配
//  - 证据必须在真实观测里存在（防幻觉），且必须是逐字摘抄的 key=value 片段
//  - SLA 由 harness 重新测量，不信 agent 自报
//
// 分层（SPEC §7.3）：eval/ 不得 include media/、pipeline/。
// 本头文件只认识指标 POD（Metrics）与管线档位 POD（PipelineConfig），
// 与 pipeline 的转换全部封在 src/eval/eval.cpp 里。
#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <limits>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cue/agent/agent.h"

namespace cue::eval {

using json = nlohmann::json;

// 「取不到」用 NaN 表示：报告写 n/a，不拿 0 冒充实测值（0 是合法读数）
inline constexpr double kNoValue = std::numeric_limits<double>::quiet_NaN();
bool has_value(double v);

struct Latency {
  double p50 = kNoValue;
  double p95 = kNoValue;
  double p99 = kNoValue;
};

// 六段延迟 + 帧统计的一次采样快照（评测层视角，字段与 pipeline 解耦）
struct Metrics {
  double fps = 0;
  uint64_t frames_in = 0;
  uint64_t frames_out = 0;
  uint64_t dropped = 0;  // 重测窗口内的丢帧增量（不是进程累计值，累计值随重启清零）
  uint64_t injected = 0;
  uint64_t decoded = 0;
  uint64_t decode_errors = 0;
  Latency recv;
  Latency buffer;
  Latency decode;
  Latency op;
  Latency encode;
  Latency send;
  int64_t updated_us = 0;
  uint64_t samples = 0;
};

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
  // 帧完成率下限（frames_out / frames_in）；0 表示不检查
  double sla_frame_completion_min = 0.0;
  // 该 case 只能人工复现（如 FEC 幽灵帧这类不在 agent 根因枚举内的问题）。
  // real 模式遇到时跳过并记 skipped，不计入成功率分母。
  bool manual_only = false;

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

// 证据校验：证据必须（a）是某条真实观测里逐字摘抄的 key=value 片段，
// （b）除引用、工具名与单位外不含解释性文字。见 review-eval.md 的伪造构造表。
bool evidence_matches(const std::string& evidence, const std::vector<Observation>& obs);

Score judge_case(const Case& c, const agent::RunResult& res, const std::vector<Observation>& obs,
                 const Metrics& final_metrics);

// case 声明的管线档位（评测层 POD；real 模式由 eval.cpp 转成 pipeline::Config）
struct PipelineConfig {
  std::string cued_bin;
  bool transcode = false;
  int width = 0;
  int height = 0;
  int bitrate_kbps = 1500;
  int gop = 60;
  double inject_drop = 0.0;
  uint64_t inject_seed = 42;
};

// ---------------------------------------------------------------- 解析

std::expected<Case, std::string> parse_case(const json& j, const std::string& suite_name);
std::expected<std::vector<Case>, std::string> load_suite(const std::string& path);

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
  // 六段分位与注入→达标耗时（报告 p50/p95/p99 与 MTTR 的数据源；取不到为 NaN）
  Metrics metrics;
  double mttr_s = kNoValue;
  // agent 实际看到的观测（落进 state.jsonl，出争议时可离线复核证据是否成立）
  std::vector<Observation> observations;
  // replay 模式的 cassette 命中/未命中次数（miss 会变成 run_error=cassette_miss）
  std::size_t cassette_hits = 0;
  std::size_t cassette_misses = 0;
};

struct RunConfig {
  std::string suite_path;
  std::string agent_mode = "scripted";  // scripted | real
  int repeat = 1;                       // pass^k 的 k
  double max_yuan = 5.0;                // 单 run 成本闸门
  std::string out_dir = "runs";
  int warmup_s = 6;                     // 管线起来后等推流的时间
  // M4b
  std::string resume_run_id;            // 非空 = 续跑该 run（复用已完成 case）
  std::string cued_bin = "./build/cued";
  // M7 / AC-16：非空 = 被测管线是一个实现了 REST 控制协议的 HTTP 服务（例如
  // apps/cue-mock-svc.cpp），而不是本机子进程。空 = 现有子进程路径，行为完全不变。
  std::string endpoint;
  std::string model = "deepseek-chat";  // real 模式模型名
  double yuan_per_mtok = 2.0;           // 成本核算单价（估算，不是账单）
  // LLM 传输（SPEC §7.9）：replay = 只读 cassette（默认，零成本）；live = 真实网络。
  // real 模式下 replay 缺 cassette 直接报错，绝不静默走网络。
  std::string llm_mode = "replay";
  std::string cassette_dir = "cassettes";
  bool record = false;  // live 时把每次成功调用追加进 cassette_dir
  // SIGINT/SIGTERM：非空时每个 case 边界与等待循环检查，置位后尽快停管线退出
  const std::atomic<bool>* cancel = nullptr;
};

struct RunReport {
  std::string run_id;
  std::string fingerprint;  // 二进制 ⊕ git ⊕ 生效配置 ⊕ suite（SPEC §7.7/AC-9）
  // 非空 = 根本没有跑起来（state 写不进、锁被占）：此时不得覆盖已有 report.md
  std::string fatal_error;
  std::vector<CaseOutcome> cases;
  int total = 0;      // 计划数 = cases × repeat
  int planned = 0;    // 同上（截断时与 executed 对照）
  int executed = 0;   // 实际执行（含复用缓存）的 case 次数
  int scored = 0;
  int run_errors = 0;
  int io_errors = 0;  // state/报告写入失败次数（写失败必须非零退出）
  bool gated = false;      // 成本闸门触发
  bool truncated = false;  // 闸门/取消导致未跑完计划数
  double root_rate = 0;
  double evidence_rate = 0;
  double sla_rate = 0;
  double cost_yuan = 0;
  int repeat = 1;
  // pass^k：对每个 case，k 次里全部成功的比例；整体取均值
  double pass_k = 0;
  // replay 模式 cassette 命中/未命中合计（miss 计入 run_error，不进成功率分母）
  std::size_t cassette_hits = 0;
  std::size_t cassette_misses = 0;
  // 平均 MTTR（秒）：只统计「有 SLA 阈值且测到达标」的 case；取不到为 NaN
  double mttr_avg_s = kNoValue;
  std::vector<std::string> failure_modes;  // "root: 期望 X 实际 Y" 之类的聚合
};

// real 模式：case → 管线配置（注入档位只来自 case；评测器不替 agent 造结论）
PipelineConfig pipeline_config_for(const Case& c, const RunConfig& cfg);

// real 模式错误分类：空串 = 计分；否则是 run_error 的 error_kind（不计入成功率分母）
std::string classify_run_error(const agent::RunResult& res);

class Engine {
 public:
  explicit Engine(RunConfig cfg) : cfg_(std::move(cfg)) {}
  RunReport run(const std::vector<Case>& cases);
  std::string render_markdown(const RunReport& r) const;
  // 生效配置指纹：模式/repeat/成本闸门/模型/cued 二进制内容。
  // 不含它时「换 SUT、换 model、换 mode」指纹相同，续跑会跨配置复用缓存（实测）。
  std::string config_hash() const;

 private:
  CaseOutcome run_scripted_case(const Case& c) const;
  // scripted + --endpoint：假 agent 的观测与指标都来自 HTTP 后端（不能再用写死的值）
  CaseOutcome run_scripted_case_endpoint(const Case& c) const;
  CaseOutcome run_real_case(const Case& c) const;
  void run_real_case_impl(const Case& c, CaseOutcome& o) const;
  void run_cases(const std::vector<Case>& cases, RunReport& r);
  bool cancelled() const;

  RunConfig cfg_;
};

}  // namespace cue::eval
