// Agent 会话持久化（FR-AGENT.5）：把一次 run 的完整轨迹落成 append-only JSONL，
// 支持进程被 kill -9 之后从「中断时的阶段 + 已消耗的预算 + 当时的对话与观测」继续跑。
//
// 与 cue-eval --resume 的关系（两件事常被混淆，这里刻意不复用它的文件也不改它的语义）：
//   · 本文件恢复的是 **agent 会话**：阶段状态机停在哪儿、轮次/成本/墙钟花了多少、
//     模型当时的 system prompt 与全部 tool 结果——进程内状态接着跑；
//   · cue-eval --resume 恢复的是 **case 级缓存**：已完成的 case 不重跑，它并不知道
//     agent 内部走到了哪一步。两者只共享"append-only + 尾部截断容错"的写法。
//
// 记录类型（每行一个 JSON 对象，`t` 字段区分）：
//   meta        会话级元信息（含**当时那份 system prompt**，恢复后不能换成当前默认值）
//   message     完整对话一条（system/user/assistant/tool，含 tool_calls）
//   tool        一次真实工具调用（参数/结果摘要/耗时/是否被审批拒绝/风险级）
//   observation fetch_observation / query_metrics 的原始观测（审计基础：证据必须能在
//               真实观测里找到；超长时截断并留标记）
//   stage       阶段迁移（或守卫拒绝）与当时的预算消耗、守卫标志
//   budget      每轮累计的 turns / cost / wall（恢复要接着算，不能从 0 开始）
//   end         结束原因与 final_stage（cancelled 的 end **不影响** 可恢复性）
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cue/agent/agent.h"
#include "cue/llm/client.h"

namespace cue::agent {

using json = nlohmann::json;

// 格式版本：记录字段变化时递增；读取端只接受同大版本（v1 内部只增字段不删字段）。
constexpr const char* kSessionFormat = "cue-agent-session/1";
// 写进 meta.agent_version 的版本串。用编译期常量而不是 popen("git describe")：
// 恢复判据需要的是"这份记录是哪个版本的 agent 写的"，编译产物本身就固定了它；
// 也要避免每条记录（或每次打开会话）再 fork 一个 git 进程。
constexpr const char* kAgentVersion = "cue-agent/1";

// 记录的观测文本上限：session 不是观测数据的全量备份，超过就截断并标记（读回时
// truncated=true 仍然可见），避免一次超长日志把会话文件撑到无法恢复的量级。
constexpr std::size_t kMaxObservationBytes = 64 * 1024;
// tool 记录里的"结果摘要"上限（完整结果在 message(tool) 里，摘要只为可读的审计）。
constexpr std::size_t kMaxToolSummaryBytes = 512;

// 观测快照（FR-AGENT.5 的 observation 记录）
struct ObservationRecord {
  std::string tool;
  json args = json::object();
  std::string text;
  json data = json::object();
  bool truncated = false;      // text 是否被 kMaxObservationBytes 截断
  std::size_t original_bytes = 0;
  int turn = 0;
};

// 预算累计值：恢复时接着算
struct SessionProgress {
  int turns = 0;
  double cost_yuan = 0.0;
  // agent 处于活跃状态的墙钟累计（微秒）。**跨进程的近似量**：崩溃到 resume 之间的
  // 停机时间不计入（那段时间 agent 没有活着，计入等于凭空烧预算）。
  int64_t wall_us = 0;
};

// 读回一个 session 之后的全部恢复状态
struct LoadedSession {
  std::string path;
  json meta = json::object();  // 原样保留（session_id/run_id/started_us/model/mode/system_prompt/goal…）
  std::vector<llm::ChatMessage> messages;
  std::vector<ObservationRecord> observations;
  Stage stage = Stage::Collect;
  SessionProgress progress;
  bool used_observe = false;
  bool used_mutating = false;
  int consecutive_rejects = 0;
  Diagnosis diagnosis;
  bool ended = false;           // 已结束且不可恢复（done/give_up/预算耗尽…）
  std::string end_reason;
  std::string end_kind;
  Stage end_final_stage = Stage::Collect;
  // 中断在「assistant 的 tool_calls 已落盘、tool 结果还没落盘」之间时生成的合成 tool 消息
  // （内容写明"进程中断，未执行"）。恢复时补进历史并落盘：否则历史里会有一个没有回填的
  // tool_call，下一次请求直接被 API 400 拒掉。
  std::vector<llm::ChatMessage> synthetic_tail;
  // 尾部残行 / 未知记录类型 / 未知字段等非致命问题（读取端必须报出来，不能静默）
  std::vector<std::string> warnings;
};

// 消息与记录的序列化（恢复要逐字回放，所以字段必须无损）
json message_to_json(const llm::ChatMessage& m, bool synthetic = false);
std::expected<llm::ChatMessage, std::string> message_from_json(const json& j);

// 读文件。**不**加锁（调用方用 open_session 才能拿到单写者保证）。
// 缺失文件是错误（拼错路径必须报，不能静默从零开始）。
// 崩溃只会截断**最后一行**：最后一行解析失败按"无害丢弃"处理并写 warning；
// 中间行坏了说明文件真的坏了，返回错误（和 eval/state.cpp 同一口径）。
std::expected<LoadedSession, std::string> load_session(const std::string& path);

// session 写入端：构造时拿单写者锁（对 <path>.lock 做 flock LOCK_EX|LOCK_NB），
// 每行一次 write(O_APPEND)。析构释放锁与 fd。
class SessionLog {
 public:
  ~SessionLog();
  SessionLog(const SessionLog&) = delete;
  SessionLog& operator=(const SessionLog&) = delete;

  // 拿锁后才允许写。create_if_missing=false 时文件必须已存在（--resume-session：
  // 拼错路径不能悄悄新建一个空会话）。
  static std::expected<std::unique_ptr<SessionLog>, std::string> open(const std::string& path,
                                                                      bool create_if_missing);

  const std::string& path() const { return path_; }

  // meta：会话级元信息。run_id / session_id 由路径反推
  // （<runs_dir>/<run_id>/sessions/<session_id>.jsonl），调用方不需要再传一遍。
  std::expected<void, std::string> write_meta(const std::string& model, const std::string& mode,
                                              const std::string& system_prompt,
                                              const std::string& goal);
  std::expected<void, std::string> write_message(const llm::ChatMessage& m, bool synthetic = false);
  std::expected<void, std::string> write_tool(const std::string& name, const json& args, bool ok,
                                              const std::string& result, int64_t duration_us,
                                              bool approval_denied, const std::string& risk,
                                              int turn);
  std::expected<void, std::string> write_observation(const ObservationRecord& ob);
  // to 用字符串是为了把非法的 next 值（模型给的）也原样留痕，而不是丢掉
  std::expected<void, std::string> write_stage(Stage from, const std::string& to, bool allowed,
                                               const std::string& reason, const SessionProgress& p,
                                               bool used_observe, bool used_mutating,
                                               int consecutive_rejects, const Diagnosis& d,
                                               bool forced = false);
  std::expected<void, std::string> write_budget(const SessionProgress& p, bool used_observe,
                                                bool used_mutating, int consecutive_rejects);
  std::expected<void, std::string> write_end(Stage final_stage, const std::string& reason,
                                             const SessionProgress& p);

 private:
  SessionLog(std::string path, int fd, int lock_fd)
      : path_(std::move(path)), fd_(fd), lock_fd_(lock_fd) {}
  std::expected<void, std::string> append(const json& line);

  std::string path_;
  int fd_ = -1;
  int lock_fd_ = -1;
};

// --session / --resume-session 的统一入口：先拿单写者锁，再决定"新建"还是"续写"。
struct SessionOpen {
  std::unique_ptr<SessionLog> log;
  // 有值 = 文件里已有记录，按恢复语义续写（阶段复位 + 预算续算 + 对话重放）
  std::optional<LoadedSession> resumed;
};

// must_exist=true  → 文件必须存在且非空（--resume-session）；否则报错
// must_exist=false → 不存在则新建；已存在且非空则按续写处理（--session 的"新建或续写"）
// 已结束的会话（done/give_up/预算耗尽…）一律拒绝续写；cancelled 的会话仍然可恢复。
std::expected<SessionOpen, std::string> open_session(const std::string& path, bool must_exist);

}  // namespace cue::agent
