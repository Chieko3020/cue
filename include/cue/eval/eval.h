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

// 端到端观测（评测层视角的 POD）：由 src/eval/eval.cpp 从 pipeline::ProbeMetrics 转换，
// eval.h 不认识 pipeline/ 的具体类型（SPEC §7.3 的分层约束：pipeline 的转换只在一处做）。
// 与 Metrics 分开放在同一个字段里，是因为判分同时需要两侧：root/evidence 看 cued 工具面，
// 端到端 SLA 只能看探针——混用就是拿服务端自报当端到端证据。
struct ProbeSummary {
  bool alive = false;
  uint64_t samples = 0;  // 探针指标行数；0 = 端到端不可用
  double fps = 0;
  uint64_t frames = 0;
  uint64_t decoded = 0;
  uint64_t decode_errors = 0;
  // 重测窗口内的卡顿增量（探针侧是进程累计值，跨窗口要差分；探针重启会清零，按 0 重计）
  uint64_t stalls = 0;
  double max_stall_ms = kNoValue;  // 窗口内最长卡顿（ms）；NaN = 无样本
  Latency e2e;                     // 端到端 p50/p95/p99（ms）
  // 四段分解（µs，只有 p50/p99 有值）；buffer 段在 cued 抖动缓冲实现前恒为 NaN
  Latency recv_us, reassembly_us, buffer_us, decode_us;
  bool synced = false;
  int64_t offset_us = 0;
  // 不可用原因（探针没起/起不来/没上报）。非空 = 端到端数据不存在，不是"读数为 0"。
  std::string unavailable_reason;
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
  ProbeSummary probe;  // 端到端（探针）读数，与上面 cued 侧的字段互不代替
};

struct Case {
  std::string id;
  std::string suite;
  uint64_t seed = 42;

  // 故障注入（评测器负责施加，agent 负责发现并撤销）：
  //   none         干净基线
  //   drop         出向丢包（%）：只涨 injected，fps 不动（丢弃在 frames_out 结算之后）
  //   ingress_drop 上行丢包（%）：重组超时 → dropped 涨、frames_out 掉
  //   rate         发送速率上限（Mbps）：令牌桶不足 → budget_drops 涨
  //   backlog      发送排队节奏（帧/s）：不丢包，积压 → frames_in−frames_out 涨
  std::string fault_kind;   // "none" / "drop" / "ingress_drop" / "rate" / "backlog"
  double fault_value = 0.0;

  // 管线档位
  bool transcode = false;
  int width = 0;
  int height = 0;
  int bitrate_kbps = 1500;
  int gop = 60;
  std::string preset = "veryfast";

  // scripted 模式的修复动作（等价于 real 模式下 agent 会做的那几步）。
  // 只做「case → 工具调用」的翻译，不参与判分：SLA 仍然由 harness 复测。
  struct RepairFix {
    bool set_transcode = false;
    bool transcode = false;
    bool set_bitrate = false;
    int bitrate_kbps = 0;
    bool set_gop = false;
    int gop = 0;
    bool set_preset = false;
    std::string preset;
    bool set_size = false;
    int width = 0;
    int height = 0;
  };
  RepairFix repair;

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
  // ---- 端到端 SLA（probe 侧；旧 suite 不写这些字段，行为完全不变）----
  // NaN = 不检查。声明了就必须由探针样本证明：没有探针样本按"无法证明达成"判不达标，
  // 绝不退回 cued 的 fps/分位（那是服务端自报，不是端到端）。
  double sla_lat_p99_ms = kNoValue;
  double sla_lat_p50_ms = kNoValue;
  double sla_max_stall_ms = kNoValue;
  // 窗口内允许的卡顿次数上限（UINT64_MAX = 不检查）
  uint64_t sla_stalls_max = UINT64_MAX;
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
  std::string probe_bin;
  bool transcode = false;
  int width = 0;
  int height = 0;
  int bitrate_kbps = 1500;
  int gop = 60;
  std::string preset = "veryfast";
  double inject_drop = 0.0;
  double inject_ingress_drop = 0.0;
  // 0 = 不排队（默认）；>0 = 发送侧按该节奏排空（fault.kind=backlog）
  double queue_fps = 0.0;
  // 发送速率上限（fault.kind=rate 时生效；默认 10 Mbps = SPEC §7.2 的链路）
  double send_mbps = 10.0;
  // 延迟注入（fault.kind=delay / jitter 时生效；单位毫秒，0 = 关）
  double inject_delay_ms = 0.0;
  double inject_jitter_ms = 0.0;
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
  // 端到端 p99（ms）的前后对比：注入前窗口 / 故障窗口（注入后、修复前）/ 修复后窗口。
  // 为什么三个都要：Process 后端的故障是随 cued 启动参数生效的，"注入前"窗口根本不存在，
  // 只报 before/after 会让报告看起来缺一半数据；把故障窗口也留下才看得出"前后差多少"。
  double p99_ms_before = kNoValue;
  double p99_ms_fault = kNoValue;
  double p99_ms_after = kNoValue;
  // 上面三个里任何一个为 null 的原因（写进报告，不能只留 n/a 让人猜）
  std::string e2e_null_reason;
  // agent 实际看到的观测（落进 state.jsonl，出争议时可离线复核证据是否成立）
  std::vector<Observation> observations;
  // replay 模式的 cassette 命中/未命中次数（miss 会变成 run_error=cassette_miss）
  std::size_t cassette_hits = 0;
  std::size_t cassette_misses = 0;
  // AC-6：工具观测回放的命中/miss（miss = 录制用尽或参数不符，必须报错并留档）
  std::size_t tool_hits = 0;
  std::size_t tool_misses = 0;
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
  // cue-probe 二进制。空 = 自动取 cued 同目录的 cue-probe（--cued 换了目录时不会指错）。
  // 探针由 harness 托管：它是端到端指标的唯一来源，没有它就不能判端到端 SLA。
  std::string probe_bin;
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
  // AC-6：工具观测的另一半录制。cassette 只录 LLM 一侧，真管线每轮观测都在变
  // （fps 抖动、累计计数递增），messages 跟着变 → cassette key miss。
  // record_tools_dir 非空 = 用 RecordingPipeline 包住真实管线；replay_tools_dir 非空 =
  // 只用 ReplayPipeline（不 spawn cued、不连 endpoint）。两者互斥。
  std::string record_tools_dir;
  std::string replay_tools_dir;
  // SLA 重测前的收敛期（毫秒）。修复动作不是瞬间把系统拉回稳态的：
  //  · 抖动缓冲降档需要若干个观测窗口；
  //  · 编码器换档后上游积压要排空（实时流会丢帧，但排空仍需时间）。
  // 直接在 agent 结束后立刻采样，p99 会被恢复期的一两个离群帧顶穿——3 s 窗口约 90 帧，
  // p99 只要 1 帧尖峰就会被拉到几百 ms（实测 AC-15 的 queue-backlog 与 enc-placebo 就是这么掉的）。
  // 等待期不看 agent 行为，所以"什么都不做"骗不过去：故障没修的话指标不会自己变好。
  int sla_settle_ms = 3000;
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
  // AC-6：工具观测回放命中/miss 合计（miss 必须出现在报告里）
  std::size_t tool_hits = 0;
  std::size_t tool_misses = 0;
  // 平均 MTTR（秒）：只统计「有 SLA 阈值且测到达标」的 case；取不到为 NaN
  double mttr_avg_s = kNoValue;
  // run 级的端到端 p99 前后对比（各 case 的中位；NaN = 没有可用样本）
  double p99_ms_before = kNoValue;
  double p99_ms_after = kNoValue;
  // 为 null 的原因（探针不可用 / 没有修复前窗口 / 该 suite 没声明端到端 SLA ...）
  std::string e2e_null_reason;
  // 端到端 SLA 判定用的是探针还是别的来源：报告里写清，避免把 cued 自报读成端到端
  std::string probe_source = "none";
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
  // 机器可读的报告（report.json）：与 render_markdown 同一份数据，字段名稳定。
  // 端到端字段没有样本时写 null 并带 null 原因，不写 0（0 是合法读数）。
  std::string render_json(const RunReport& r) const;
  // 生效配置指纹：模式/repeat/成本闸门/模型/cued 二进制内容。
  // 不含它时「换 SUT、换 model、换 mode」指纹相同，续跑会跨配置复用缓存（实测）。
  std::string config_hash() const;

 private:
  CaseOutcome run_scripted_case(const Case& c) const;
  // scripted 但管线是真的：--endpoint（HTTP 后端）、--record-tools / --replay-tools
  // 都走这里——观测与指标来自真实管线（或它的录制），不能再用 run_scripted_case 的写死值。
  CaseOutcome run_scripted_case_pipeline(const Case& c) const;
  CaseOutcome run_real_case(const Case& c) const;
  void run_real_case_impl(const Case& c, CaseOutcome& o) const;
  void run_cases(const std::vector<Case>& cases, RunReport& r);
  bool cancelled() const;

  RunConfig cfg_;
  // run_shell 的审计路径（<out_dir>/<run_id>/audit.jsonl）。run_id 要到 run_cases 里
  // 才算得出来，而 run_real_case_impl 是 const，所以这里用 mutable 存一次。
  mutable std::string audit_log_path_;
};

}  // namespace cue::eval
