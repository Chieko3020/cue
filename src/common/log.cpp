// 结构化日志实现：spdlog 异步 sink + nlohmann 序列化（FR-OBS.1）
#include "cue/common/log.h"

#include <spdlog/async.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

#include <nlohmann/json.hpp>

namespace cue::log {
namespace {

std::mutex g_mu;                                  // 保护初始化/销毁（读路径不加锁）
std::shared_ptr<spdlog::logger> g_logger;
std::shared_ptr<spdlog::details::thread_pool> g_pool;
Options g_opt;
// 计数被所有写日志的线程并发递增（debug 的丢弃判定在调用线程上做），必须是原子量；
// /metrics 读它时也不想为了一个计数器去抢全局锁。
std::atomic<std::uint64_t> g_dropped_debug{0};
thread_local Context t_ctx;

std::int64_t wall_us() {
  // 日志的 ts_us 是**墙上时钟**（unix 微秒）：日志要跨进程/跨机器对齐，单调时钟做不到。
  // 延迟测量仍然走 cue::now_us()（steady_clock），两者不能混用。
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// 人读格式（--log-text）：时间戳/级别前缀由 spdlog 的 pattern 提供，这里只拼
// component/event: msg（有 err 时追加在括号里）。两条路径都只做字符串拼接，
// 差别在于"要不要 JSON 那些字段名"。
std::string format_text_line(std::string_view component, std::string_view event,
                             std::string_view msg, std::string_view err) {
  std::string out;
  out.reserve(component.size() + event.size() + msg.size() + err.size() + 16);
  out.append(component).append("/").append(event).append(": ").append(msg);
  if (!err.empty()) out.append("（").append(err).append("）");
  return out;
}

spdlog::level::level_enum to_spdlog(Level lvl) {
  switch (lvl) {
    case Level::Debug: return spdlog::level::debug;
    case Level::Info: return spdlog::level::info;
    case Level::Warn: return spdlog::level::warn;
    case Level::Error: return spdlog::level::err;
  }
  return spdlog::level::info;
}

}  // namespace

bool parse_level(std::string_view text, Level& out) {
  if (text == "debug") {
    out = Level::Debug;
    return true;
  }
  if (text == "info") {
    out = Level::Info;
    return true;
  }
  if (text == "warn" || text == "warning") {
    out = Level::Warn;
    return true;
  }
  if (text == "error" || text == "err") {
    out = Level::Error;
    return true;
  }
  return false;
}

const char* level_name(Level lvl) {
  switch (lvl) {
    case Level::Debug: return "debug";
    case Level::Info: return "info";
    case Level::Warn: return "warn";
    case Level::Error: return "error";
  }
  return "info";
}

std::string format_json_line(const Context& ctx, std::string_view proc, Level lvl,
                             std::string_view component, std::string_view event,
                             std::string_view msg, std::string_view err, std::int64_t ts_us) {
  // ordered_json：字段顺序固定，人读与 diff 都稳定（普通 json 是 std::map，按字母序）。
  nlohmann::ordered_json j;
  j["ts_us"] = ts_us;
  j["level"] = level_name(lvl);
  j["proc"] = proc;
  j["run_id"] = ctx.run_id;
  j["case_id"] = ctx.case_id;
  j["session_id"] = ctx.session_id;
  j["component"] = component;
  j["event"] = event;
  j["msg"] = msg;
  // err 是唯一可选字段（文档口径）：没有错误文本就不写，避免每行都挂一个空串。
  if (!err.empty()) j["err"] = err;
  return j.dump();
}

std::expected<void, std::string> init(const Options& opt) {
  Level lvl = Level::Info;
  if (!parse_level(opt.level, lvl)) {
    return std::unexpected("--log-level 只能是 debug|info|warn|error，收到：" + opt.level);
  }
  if (opt.rotate_mb <= 0 || opt.rotate_files <= 0) {
    return std::unexpected("日志轮转参数非法（rotate_mb / rotate_files 必须为正）");
  }
  if (opt.queue_size == 0) {
    return std::unexpected("日志队列长度不能为 0（异步 sink 需要至少 1 个槽位）");
  }

  std::vector<spdlog::sink_ptr> sinks;
  try {
    if (!opt.file.empty()) {
      // rotating：单文件 rotate_mb 上限、保留 rotate_files 个。路径不可写时这里抛异常，
      // 由下面的 catch 转成"拒绝启动"，而不是让进程带着一半日志跑起来。
      const auto max_size = static_cast<std::size_t>(opt.rotate_mb) * 1024 * 1024;
      sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
          opt.file, max_size, static_cast<std::size_t>(opt.rotate_files)));
    }
  } catch (const std::exception& e) {
    return std::unexpected(std::string("打开日志文件失败（") + opt.file + "）：" + e.what());
  }
  if (opt.stderr_sink) sinks.push_back(std::make_shared<spdlog::sinks::stderr_sink_mt>());
  if (sinks.empty()) return std::unexpected("日志没有任何输出目标（file 与 stderr 都关了）");

  std::lock_guard<std::mutex> lk(g_mu);
  // 重复 init（单测会这么用）时先把上一份收干净，避免两个线程池同时存在。
  if (g_logger) {
    g_logger->flush();
    spdlog::drop_all();
    g_logger.reset();
  }
  g_pool.reset();
  spdlog::init_thread_pool(opt.queue_size, 1);
  g_pool = spdlog::thread_pool();
  if (!g_pool) return std::unexpected("spdlog 线程池创建失败");

  auto logger = std::make_shared<spdlog::async_logger>(
      "cue", sinks.begin(), sinks.end(), g_pool, spdlog::async_overflow_policy::block);
  logger->set_level(to_spdlog(lvl));
  // JSON（默认）：消息体已经是一整行 JSON，pattern 只能是纯消息，否则前面会挂上时间戳
  // 让整行不再是合法 JSON。人读格式才要时间戳/级别前缀。
  logger->set_pattern(opt.json ? "%v" : "[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %v");
  // 刷盘发生在**异步线程**，不会阻塞调用方；换来的是 tail -f 能立刻看到、崩溃前最后
  // 几行也不会停在用户态缓冲区里。日志的价值一半在事后，这一下必须付出。
  logger->flush_on(spdlog::level::debug);

  g_logger = std::move(logger);
  g_opt = opt;
  g_dropped_debug.store(0, std::memory_order_relaxed);
  return {};
}

void shutdown() {
  std::shared_ptr<spdlog::logger> logger;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    logger = std::move(g_logger);
    g_logger.reset();
  }
  if (!logger) return;
  // 顺序要紧：先刷（flush 是排进队列的消息），再从 registry 摘掉，再放掉线程池。
  logger->flush();
  spdlog::drop_all();
  spdlog::shutdown();  // registry 放掉全局线程池；队列里的消息处理完后 worker 自行退出
  g_pool.reset();
  g_logger.reset();
}

void set_context(Context ctx) { t_ctx = std::move(ctx); }
void set_run_id(std::string run_id) { t_ctx.run_id = std::move(run_id); }
void set_session_id(std::string session_id) { t_ctx.session_id = std::move(session_id); }
void clear_context() { t_ctx = Context{}; }
const Context& context() { return t_ctx; }

void log(Level lvl, std::string_view component, std::string_view event, std::string_view msg,
         std::string_view err) {
  // 未初始化：静默返回。日志基础设施没起来不该把调用方带走（例如单测里只测纯函数）。
  if (!g_logger) return;
  const auto sl = to_spdlog(lvl);
  if (!g_logger->should_log(sl)) return;

  // §12.1「满时丢弃 debug 级并计数」。这个检查在**入队前**：debug 是唯一可以被丢的
  // 级别，丢之前先看队列，就不会因为一条 debug 把队列顶满、进而让 info 走 block。
  // （检查与入队之间仍有窗口，真有 debug 撞进满队列时最多等一瞬，不会丢 info。）
  if (lvl == Level::Debug && g_pool && g_pool->queue_size() >= g_opt.queue_size) {
    g_dropped_debug.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // JSON 是默认口径（FR-OBS.1）；--log-text 才走人读格式。两条路径都只把最终字符串
  // 交给 spdlog，pattern 负责加不加时间前缀（JSON 模式加前缀就不再是合法 JSON 行了）。
  const std::string line =
      g_opt.json ? format_json_line(t_ctx, g_opt.proc, lvl, component, event, msg, err, wall_us())
                 : format_text_line(component, event, msg, err);
  // 直接走「整串消息」重载（不走 fmt 的格式化路径）：消息已经是一整行，没有占位符要填。
  // 顺带避开 fmt 9.1 + GCC 14 的 -Warray-bounds 误报（format_dragon 的已知问题）。
  g_logger->log(sl, spdlog::string_view_t(line.data(), line.size()));
}

bool enabled() { return static_cast<bool>(g_logger); }
std::size_t queue_depth() { return g_pool ? g_pool->queue_size() : 0; }
std::uint64_t dropped_debug() { return g_dropped_debug.load(std::memory_order_relaxed); }
std::size_t queue_capacity() { return g_opt.queue_size; }

}  // namespace cue::log
