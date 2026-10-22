// 结构化日志门面（FR-OBS.1 / 技术设计文档 §12.1）
//
// 分工是文档点名的：**spdlog 只负责异步队列、多 sink 与文件轮转**，JSON 由 nlohmann
// 序列化后整串写进消息体（`log(level, "{}", json.dump())`），绝不手拼转义。手拼过一次
// 就知道为什么不行：msg 里出现引号/换行/控制字符时，行会从「一行一条」变成「两条坏行」，
// 而下游的 JSON-line 解析器只会静默少读一条。
//
// 队列语义（技术设计 §12.1「队列 8192 行，满时丢弃 debug 级并计数」）：
//  - 队列长度由 Options::queue_size 决定，默认 8192；
//  - **debug** 在入队前先看队列深度，满了就丢并计数（dropped_debug_total）；
//  - info/warn/error 走 block 策略：宁可让调用线程等一瞬，也不丢一条错误（日志是
//    审计与事后归因的唯一凭据，丢错等于丢证据）。
//
// 刻意不做的事：
//  - 不接管 cued / cue-probe 的 stdout。它们的 stdout 是 harness 的解析输入（[cue-event]
//    行、METRICS 行、probe.metric 行），改成 JSON 日志会直接打断控制面。这两个进程的
//    人读日志保持原样。
//  - 不做跨线程上下文传播。关联 id 是 thread_local：谁处理这个 run/case，谁自己设。
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace cue::log {

// 级别顺序与 spdlog 一致（数值越大越严重），过滤用 >= level 判断。
enum class Level : int { Debug = 0, Info = 1, Warn = 2, Error = 3 };

// JSON-line 的固定字段（FR-OBS.1）：
//   ts_us / level / proc / run_id / case_id / session_id / component / event / msg
// 外加可选的 err（只在有错误文本时出现）。Context 提供其中三个关联 id：
// 一条日志属于哪个 run、哪个 case、哪个会话，靠它们与事件流/报告串起来。
struct Context {
  std::string run_id;
  std::string case_id;
  std::string session_id;
};

struct Options {
  std::string file;                // 空 = 不写文件（只写 stderr）
  std::string level = "info";      // debug | info | warn | error
  // true（默认）= JSON-line，这是 FR-OBS.1 的口径；false = 人读格式。
  // 给 false 的唯一理由是 tail 一个文件时不想看 JSON——但那时就不再满足 FR-OBS.1，
  // 所以默认必须是 true，而不是"控制台好看优先"。
  bool json = true;
  bool stderr_sink = true;         // 默认同时写 stderr：日志不该只落在某个文件里
  std::size_t queue_size = 8192;   // §12.1 的 8192
  int rotate_mb = 64;              // 单文件上限（MB）
  int rotate_files = 3;            // 轮转保留个数（含当前文件）
  std::string proc = "cue";        // 字段 proc：进程名（cued / cue-probe / cue-serve / cue-eval）
};

// --log-level 的取值解析。非法值时返回 false，由调用方补上下文报错。
bool parse_level(std::string_view text, Level& out);

// 级别名（JSON 里的 level 字段）：小写，稳定，便于 jq 过滤。
const char* level_name(Level lvl);

// 全局初始化（每进程一次）。日志文件打不开 / 目录不存在时返回错误：
// 配置说"要日志"却静默没有日志，比拒绝启动更糟。
std::expected<void, std::string> init(const Options& opt);

// 刷盘并停掉异步线程。退出前调用；未初始化时是空操作。
void shutdown();

// 关联 id（thread_local）。空字符串 = 没有该上下文，字段仍然写出（固定字段集）。
void set_context(Context ctx);
void set_run_id(std::string run_id);
void set_session_id(std::string session_id);
void clear_context();
const Context& context();

void log(Level lvl, std::string_view component, std::string_view event, std::string_view msg,
         std::string_view err = {});
inline void debug(std::string_view c, std::string_view e, std::string_view m,
                  std::string_view err = {}) {
  log(Level::Debug, c, e, m, err);
}
inline void info(std::string_view c, std::string_view e, std::string_view m,
                 std::string_view err = {}) {
  log(Level::Info, c, e, m, err);
}
inline void warn(std::string_view c, std::string_view e, std::string_view m,
                 std::string_view err = {}) {
  log(Level::Warn, c, e, m, err);
}
inline void error(std::string_view c, std::string_view e, std::string_view m,
                  std::string_view err = {}) {
  log(Level::Error, c, e, m, err);
}

// ---- 观测（/metrics / 单测用）----
bool enabled();
std::size_t queue_depth();       // 异步队列当前深度（近似值）
std::uint64_t dropped_debug();   // 因队列满被丢弃的 debug 行数（进程累计）
std::size_t queue_capacity();

// 构造一条 JSON-line（纯函数，便于断言"字段固定"而不必起线程/写文件）。
// 返回单行文本，不含结尾换行；字段顺序固定为 ts_us / level / proc / run_id / case_id /
// session_id / component / event / msg [/ err]。
std::string format_json_line(const Context& ctx, std::string_view proc, Level lvl,
                             std::string_view component, std::string_view event,
                             std::string_view msg, std::string_view err, std::int64_t ts_us);

}  // namespace cue::log
