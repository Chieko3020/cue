// M4 评测：声明式 case + 三重判分（根因 / 证据 / SLA）
// 判分原则（SPEC §8.3）：
//  - 根因用枚举精确匹配
//  - 证据必须在真实观测里存在（防幻觉），且必须是逐字摘抄的 key=value 片段
//  - SLA 由 harness 重新测量，不信 agent 自报
//
// 分层（SPEC §7.3）：eval/ 不得 include media/、pipeline/。
// 这条约束自 T-20 起是**物理的**：cue_eval 的 include 路径里只有
// include/cue/{common,tool,agent,llm,eval,cli}（构建时生成的分层视图），
// `#include "cue/pipeline/..."` 直接编译失败。
// 本头文件只认识指标 POD（Metrics）与管线档位 POD（PipelineConfig）；
// 与管线类型（PipelineEndpoint / IServicePipeline）的转换封在 src/eval/eval.cpp 里，
// 具体后端由 pipeline 层的 make_pipeline 创建。
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
  // 四段分解（µs，p50/p95/p99 都有值；NaN = 该位没有样本）。buffer 段来自探针自己的
  // 抖动缓冲等待（进队 → 放行），不再恒为 NaN。
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
  // 证据不成立时的一句原因（N-22）：写明是"不是逐字引用"还是"哪个候选的什么谓词没被
  // 满足"。没有它，报告里只看到一个 ✗，读者分不清是引用造假还是取值不支持结论。
  std::string evidence_reason;
  bool all() const { return root && evidence && sla; }
};

// 「注入是否真的生效」（N-23）：判的是**故障有没有发生**，与「模型答得对不对」是两件事。
//
// 为什么需要这一层：rate 类 case 的故障是"发送速率上限"，令牌桶只有源码率**超过**上限时
// 才会耗尽。评测的旧产帧源（scripts/test-client.mjs）每帧发同一个 64×64 静止 IDR，
// 经 x264 后每帧只有几百字节，30 fps 合计远低于 0.1 Mbps（12.5 kB/s）——于是
// budget_drops=0、frames_in==frames_out、fps 30，SLA（fps_min=25）**恒真**。
// 没有这一层判定，那种"故障根本没发生"的 run 会以 100% 通过、退出码 0 收场，
// 比失败更危险。
//
// 判据只取三类有明确单一计数的故障：
//   rate         → budget_drops > 0（令牌桶真的被抽干）
//   drop         → injected > 0（出向真的有包被 Injector 吃掉）
//   ingress_drop → injected > 0（上行真的有包被吃掉）
// backlog / delay / jitter 没有单一计数能判"注入了没有"（积压要看 frames_in−frames_out
// 与队列深度、延迟类要看分位），不做自动判定，报告里注明「未做自动判定」，不硬凑。
struct InjectionCheck {
  std::string fault_kind;
  bool checked = false;    // 这个故障类有自动判定判据（上面三类之一）
  bool effective = false;  // checked 且故障窗口的读数满足判据
  // 故障窗口的读数（判据用的两个计数，留档便于复核；未取到样本时为 0 且 checked=false）
  uint64_t budget_drops = 0;
  uint64_t injected = 0;
  // 一句可直接写进报告的中文判定（含读数与原因），空 = 没做判定
  std::string detail;
};

// 纯函数（可单测）：按故障类与故障窗口读数给出「注入是否生效」判定。
InjectionCheck judge_injection(const std::string& fault_kind, uint64_t budget_drops,
                               uint64_t injected);

// 证据校验：证据必须（a）是某条真实观测里逐字摘抄的 key=value 片段，
// （b）除引用、工具名与单位外不含解释性文字。见 review-eval.md 的伪造构造表。
bool evidence_matches(const std::string& evidence, const std::vector<Observation>& obs);

// ---- 证据候选的取值谓词（N-22）----
//
// 为什么需要：旧判分只要求"候选键在证据里出现"，不看**取值能不能支持结论**。
// 实测踩过两次伪证：`encode_width=0`（0 = 沿用解码尺寸 follow，不是 1080p）被当成
// 1080p 尺寸证据；`budget_drops=0`（没限速、令牌桶没被抽干）被当成"带宽不足"证据。
// 两个字段都真实存在、都在观测里，旧口径下都算成立。
//
// 语法：候选串 = "key" 或 "key OP value"，OP ∈ {> >= < <= == !=}，运算符两侧允许空格。
// 例：`budget_drops>0`、`e2e_fec_recovered>=1`、`encode_width==1920`、
//     `encode_transcode==on`（字符串比较）。
//
// 语义（判分时逐条应用）：
//  1) 引用的证据仍然必须是某条真实观测里的**逐字引用**（evidence_matches，未改动）；
//  2) 证据文本里必须出现候选的 key（`==`/`!=` 也先按键命中）；
//  3) 带谓词时，还要用**承载这条引用的那条观测**里该 key 的实际取值去满足谓词。
//     取值优先来自 Observation::data（JSON 对象；数值/字符串/布尔都是标量）；
//     data 不是对象、或对象里没有该键时，退回从 text 里解析 `key=VALUE`
//     （旧录制与纯文本观测只有 text）。
//  4) 两侧都能解析成 double 时按数值比较（含负数、小数，`==` 是精确相等而非 ±2%
//     容差——容差只用于"引用值是否与观测相符"，不能用来模糊判决条件）；
//     否则 `==`/`!=` 按**原样字符串**比较，`> < >= <=` 判不满足，并把
//     "非数值无法比较"写进原因（fail-closed：不猜、不把字符串当 0）。
//  5) **不带谓词的候选与旧行为逐字节一致**（仍然只做归一化子串匹配），
//     既有 suite 的 33 + 2 个 case 不会因为这次改动变味。
//
// 解析规则：从**最后一个**比较运算符处切分（值里不会出现运算符，key 里不能有）；
// key 允许 `_` 与 `.`。没有运算符、运算符不在上面六个之内（例如单个 `=`）、或运算符
// 右侧为空时，整串当 key（= 无谓词）——这样旧候选串即使恰好含运算符字符，也仍是
// 过去的字面匹配，不会突然变成永远不成立的谓词。
struct EvidenceCandidate {
  std::string key;    // 归一化后的键（小写、去首尾空格）
  std::string op;     // "", ">", ">=", "<", "<=", "==", "!="
  std::string value;  // 谓词右值原文（去首尾空格）
  bool has_predicate() const { return !op.empty(); }
};

// 解析（纯函数，可单测）。
EvidenceCandidate parse_evidence_candidate(const std::string& raw);

// 单条观测上的「键在证据里出现 + 谓词成立」判定（纯函数，可单测）。
// 不检查证据本身是否为逐字引用——那是 evidence_matches 的职责；judge_case 只在
// 能承载该引用的观测上调用它。why 写不成立的原因（中文，可直接进日志/报告）。
bool evidence_candidate_matches(const std::string& evidence, const Observation& obs,
                                const EvidenceCandidate& cand, std::string& why);

Score judge_case(const Case& c, const agent::RunResult& res, const std::vector<Observation>& obs,
                 const Metrics& final_metrics);

// case 声明的管线档位（评测层 POD；real 模式由 eval.cpp 转成 pipeline::Config）
struct PipelineConfig {
  std::string cued_bin;
  std::string probe_bin;
  // cued 的 WS 上行端口：既用于 Process 后端的启动参数，也用于产帧源拼
  // `ws://127.0.0.1:<ws_port>/`（N-23）。默认 9000 与 PipelineEndpoint 一致。
  int ws_port = 9000;
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
  // 逐包注入判定的 trace 路径：非空时 supervisor 给 cued 传 `--inject-trace`。
  // 由 pipeline_config_for 按 run 目录算出（N-11），空 = 不落 trace。
  std::string inject_trace;
};

// ---------------------------------------------------------------- 解析

std::expected<Case, std::string> parse_case(const json& j, const std::string& suite_name);
std::expected<std::vector<Case>, std::string> load_suite(const std::string& path);

// ---------------------------------------------------------------- Engine

struct CaseOutcome {
  std::string id;
  // 这是第几次 repeat 轮（0-based）。stability 要按轮聚合每级的率：
  // 只有记住轮次，repeat=3 的"同一指标试了 3 次"才能算均值/离散度。
  int attempt = 0;
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
  // 本 case 的注入 trace 路径（空 = 没有落 trace）。报告里按 case 列出，便于复核
  // "同 seed 两次 run 的丢包判定是否一致"——trace 是证据文件，不进 state 的判分口径。
  std::string inject_trace;
  // 注入是否生效（N-23）：故障窗口的计数判定。checked=false = 该故障类没有自动判据
  // （或无管线可读）。它不改 scored / score（"故障没发生"不是"模型答错"），
  // 但会让整轮 run 不能以退出码 0 收场，见 RunReport::injection_ineffective。
  InjectionCheck injection;
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
  // ---- 产帧源（N-23）----
  // 评测自备的合成源。为什么需要它：rate 类故障注入的是"发送速率上限"，令牌桶只有
  // 源码率超过上限时才耗尽；外部推流脚本发的是 64×64 静止画面，经编码后码率极低，
  // 于是 budget_drops 恒 0、故障不发生。开启后每个 case 由评测自己起 cue-capture
  // （--backend lavfi：testsrc2 合成画面，真有 640×360@1200 kbps 的码率）。
  //   producer = "none"（默认）→ 完全沿用旧行为：靠外部推流，不起任何额外进程
  //   producer = "lavfi"        → 每个 case 在 cued 起来之后起一个 cue-capture，
  //                               case 结束（成功/失败/run_error）时连同它的 ffmpeg
  //                               子进程一起收掉
  std::string producer = "none";
  std::string producer_size = "640x360";  // WxH，宽高需为偶数（yuv420p）
  int producer_bitrate = 1200;            // kbps
  // cue-capture 二进制。与 cued_bin 一样是启动参数，不做"自动找同目录"——
  // 找不到就是评测环境缺件，必须报 run_error 而不是猜一个路径。
  std::string capture_bin = "./build/cue-capture";
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
  //
  // 3000 → 6000 的实测依据（queue-backlog-12fps-tc，逐秒采样）：
  //   清除 backlog 的瞬间 sendq 被清空，帧会**突发**补发，抖动缓冲据此升到 level 2
  //   （hold = 3 × 33 ms ≈ 99 ms）；第 1~4 秒一直是 level 2，**第 5 秒**才降回 level 0。
  //   3 s 的 settle 正好把重测的三个样本全压在 level 2 窗口里，于是
  //   buffer p50 = 99 ms、端到端 p50 = 107 ms，而 cued 侧早就 30 fps、dropped=0。
  // 6 s 覆盖"突发 → 升档 → 回落"的完整过程；代价是每个 case 多 3 s（54 个约 +162 s）。
  // 6 s 之后再延到 10 s：fps 判据改用「最后 3 条的中位数」（与报告口径一致）后，
  // 暴露出 cued 重启后的 fps 爬升期——6 s 时最近 3 条里仍有 2 条是 0，
  // 中位数被拉成 0，fps_min=25 判失败（54 次里 16 次）。
  // 之前用「最后单点」时恰好落在爬升之后，于是虚高成 98.1%。
  // 等 cued 真正稳定再取中位，是唯一诚实的做法（不能靠"挑有利样本"过）。
  int sla_settle_ms = 10000;
  // SIGINT/SIGTERM：非空时每个 case 边界与等待循环检查，置位后尽快停管线退出
  const std::atomic<bool>* cancel = nullptr;
};

// ---------------------------------------------------------------- 报告字段（技术设计文档 §8.4/§8.9）

// 失败模式分布（§8.4）：按 case 的 expect.root_cause 分组的一次判分未命中。
// 注意：这不是"运行状态"。截断/成本闸门/取消/manual_only 跳过/state 写失败属于 run_notes，
// 混在一张表里会让读者分不清「模型答错」和「这次没跑完」。
struct FailureMode {
  // 期望的根因；空 = 没有判分结果（run_error 等），归到 kRunErrorGroup。
  std::string root_cause;
  int miss_count = 0;
  // 最多 kMaxFailureExamples 个 "case_id(未命中的层)"，例如 netloss-uplink-50pct(root,sla)。
  // 带层是必要的：只知道"这条没过"看不出是答错根因、证据不成立还是 SLA 没达标。
  std::vector<std::string> examples;
};
inline constexpr const char* kRunErrorGroup = "__run__";
inline constexpr const char* kUnknownRootGroup = "__unknown__";
inline constexpr std::size_t kMaxFailureExamples = 3;

// 稳定性统计。NaN = 没有样本（写 null，不拿 0 冒充"完全稳定"）。
struct StabilityStats {
  double mean = kNoValue;
  double stddev = kNoValue;
  double min = kNoValue;
  double max = kNoValue;
};

// 按 repeat 轮聚合的稳定性（§8.4）。
// 标准差用**总体标准差**（除以 n）：这里只有本轮这几轮观测，不是从某个总体抽样做推断；
// 且 repeat 通常只有 3，样本标准差要除以 n-1，会把 3 轮的离散度放大 50%，读起来像"抖得多"。
struct Stability {
  int repeat = 1;
  // 实际有 scored 样本的轮数。截断/整轮 run_error 会让它小于 repeat——只有 1 轮有样本时
  // stddev 同样恒为 0，不能因为 repeat>1 就把它读成"稳定"。
  int rounds = 0;
  StabilityStats root_rate;
  StabilityStats evidence_rate;
  StabilityStats sla_rate;
  // 逐轮 pass_k = 该轮里三级全过的 case 占比；与 run 级 pass^k（k 轮都过）不是同一个量，
  // 报告里必须分开写，否则 repeat=3 的"每轮都 80%"会被读成"80% 的 case 三轮全过"。
  StabilityStats pass_k;
  // 少于 2 轮有样本时没有任何跨轮离散度可言：stddev 恒为 0，且不得被读成"稳定"。
  bool informative() const { return rounds > 1; }
};

// 回归清单的一条（case 级）。detail 是给人看的 "A 3/3 过 → B 1/3 过" 形式。
struct RegressionEntry {
  std::string id;
  std::string detail;
};

// 回归清单：本次 run 与 baseline 的 case 级对比（compare.h 的 DiffReport 的投影）。
// 为什么不直接塞 DiffReport：compare.h 依赖 state.h，而 state.h 依赖本头文件，
// 直接引用会成环；报告渲染只需要这几项，投影成轻量 POD 就够。
struct RegressionList {
  bool present = false;      // false = 本次没有 baseline（报告写"跳过"，不留空小节）
  std::string baseline_run_id;
  std::string source;        // "auto" = 自动发现上次同 suite 的 run；"explicit" = --baseline
  bool comparable = true;    // false = 指纹/suite/mode/repeat 有差异，差异不可归因
  std::vector<std::string> warnings;
  std::vector<RegressionEntry> new_failures;
  std::vector<RegressionEntry> fixed;
  std::vector<RegressionEntry> still_failing;
  int still_passing = 0, inconclusive = 0, only_a = 0, only_b = 0;
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
  // 「注入未生效」的 case 尝试数（N-23）。它与 run_error 是两件事，因此单独计数：
  //  · 不进成功率分母，也不改该 case 的 scored/score（模型答得对不对与故障有没有发生无关）；
  //  · 但它让整轮 run 不能以退出码 0 收场，报告/终端摘要各有一段显眼说明。
  // 理由：带这种 case 的 run 证据链不成立（SLA 恒真），给绿灯就是把假通过当结论。
  int injection_ineffective = 0;
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
  // 失败模式分布（§8.4）：按 expect.root_cause 分组，按 miss_count 降序
  std::vector<FailureMode> failure_modes;
  // 运行状态：截断 / 成本闸门 / 取消 / manual_only 跳过 / state 写失败。
  // 与 failure_modes 分开：这些是"这次 run 没按计划跑完"，不是"模型答错了哪一类"。
  std::vector<std::string> run_notes;
  // 跨 repeat 轮的稳定性（§8.4）；repeat=1 时 informative()=false
  Stability stability;
  // 回归清单（§8.9 ④）：由 CLI 在渲染前填充；present=false = 没有 baseline
  RegressionList regression;
  // report.md / report.json 的落点（CLI 填；终端摘要要打印它）
  std::string report_path;
  std::string report_json_path;
};

// 失败模式分布（纯函数，可单测）：按 expect.root_cause 分组，一次 case 尝试里
// root/evidence/sla 任一未命中就计 1 次 miss（三维全中不计、不做三维分别计数——
// "这条 case 失败了几次"才是分布，三维各算一次会把一条 case 的失败记成三次）。
// 没有判分结果（run_error）归到 kRunErrorGroup，examples 标注 (run_error)。
std::vector<FailureMode> summarize_failure_modes(const std::vector<Case>& cases,
                                                 const std::vector<CaseOutcome>& outcomes);

// 稳定性（纯函数，可单测）：按 attempt 轮聚合每级的率，只统计 scored 的尝试。
// 没有任何 scored 样本的那一轮不计入（不能拿 1 个样本的轮冒充稳定）。
Stability compute_stability(const std::vector<CaseOutcome>& outcomes, int repeat);

// real 模式：case → 管线配置（注入档位只来自 case；评测器不替 agent 造结论）。
// run_dir 是本次 run 的目录（`<out_dir>/<run_id>`）：注入类 case 的 trace 落在这里
// （`<run_dir>/inject-trace/<case_id>.trace`）。run_dir 为空 = 不落 trace（单测/离线
// 只检查档位时用），绝不回退到 /tmp 或当前目录。
PipelineConfig pipeline_config_for(const Case& c, const RunConfig& cfg, const std::string& run_dir);

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
  // 终端摘要（FR-EVAL.10）：纯函数，输入 RunReport 输出约 10-15 行文本。
  // 纯函数的意义是可快照测试——不能靠"跑一次看看"验证摘要字段。
  std::string render_terminal_summary(const RunReport& r) const;
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
  // 本次 run 的目录（<out_dir>/<run_id>）。同 audit_log_path_：run_id 在 run_cases 里才
  // 算得出来，而 pipeline_config_for 的调用点在 const 的 run_*_impl 里，故 mutable。
  // 注入类 case 的 trace 路径由它派生，空 = 不落 trace。
  mutable std::string run_dir_;
};

}  // namespace cue::eval
