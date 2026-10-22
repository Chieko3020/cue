#include "cue/agent/session.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include "cue/common/clock.h"

namespace cue::agent {

namespace {

namespace fs = std::filesystem;

// ---- 路径 → (run_id, session_id)
// 约定是 <runs_dir>/<run_id>/sessions/<session_id>.jsonl；反过来从路径里取值，
// 调用方只需要给一条路径，不会出现"路径里写的是 A、meta 里写的是 B"这种漂移。
std::string session_id_of(const std::string& path) { return fs::path(path).stem().string(); }

std::string run_id_of(const std::string& path) {
  fs::path p = fs::path(path).parent_path();  // .../<run_id>/sessions
  if (p.filename() == "sessions") p = p.parent_path();
  return p.filename().string();
}

// ---- 宽容读取：session 是本地文件，可能被手改，也可能来自旧版本。
// 类型不符时给结构化默认值（而不是让 nlohmann 的类型异常穿出去）。
std::string str_field(const json& j, const char* k, const std::string& def = "") {
  if (!j.contains(k) || !j[k].is_string()) return def;
  return j[k].get<std::string>();
}

int int_field(const json& j, const char* k, int def = 0) {
  if (!j.contains(k)) return def;
  const auto& v = j[k];
  if (v.is_number_integer()) return v.get<int>();
  if (v.is_number_unsigned()) return static_cast<int>(v.get<unsigned>());
  return def;
}

int64_t i64_field(const json& j, const char* k, int64_t def = 0) {
  if (!j.contains(k)) return def;
  const auto& v = j[k];
  if (v.is_number_integer()) return v.get<int64_t>();
  if (v.is_number_unsigned()) return static_cast<int64_t>(v.get<uint64_t>());
  return def;
}

double num_field(const json& j, const char* k, double def = 0.0) {
  if (!j.contains(k) || !j[k].is_number()) return def;
  return j[k].get<double>();
}

bool bool_field(const json& j, const char* k, bool def = false) {
  if (!j.contains(k) || !j[k].is_boolean()) return def;
  return j[k].get<bool>();
}

json obj_field(const json& j, const char* k) {
  if (!j.contains(k) || !j[k].is_object()) return json::object();
  return j[k];
}

std::expected<Stage, std::string> stage_from_name(const std::string& s) {
  if (s == "collect") return Stage::Collect;
  if (s == "diagnose") return Stage::Diagnose;
  if (s == "repair") return Stage::Repair;
  if (s == "verify") return Stage::Verify;
  if (s == "done") return Stage::Done;
  if (s == "give_up") return Stage::GiveUp;
  return std::unexpected("未知阶段名: " + s);
}

Diagnosis diagnosis_from_json(const json& j) {
  Diagnosis d;
  if (!j.is_object()) return d;
  d.root_cause = str_field(j, "root_cause");
  d.confidence = num_field(j, "confidence", 0.0);
  if (j.contains("evidence") && j["evidence"].is_array()) {
    for (const auto& e : j["evidence"]) {
      if (e.is_string()) d.evidence.push_back(e.get<std::string>());
    }
  }
  return d;
}

json diagnosis_to_json(const Diagnosis& d) {
  // 没有诊断时写 null：空对象读回来会被当成"有诊断但根因为空"，那是两种不同状态
  if (d.root_cause.empty() && d.evidence.empty()) return json(nullptr);
  return json{{"root_cause", d.root_cause}, {"evidence", d.evidence}, {"confidence", d.confidence}};
}

// 结果摘要：完整结果在 message(tool) 里，这里只留可读的审计片段
std::string summarize(const std::string& s) {
  if (s.size() <= kMaxToolSummaryBytes) return s;
  return s.substr(0, kMaxToolSummaryBytes) + "…[summary truncated " +
         std::to_string(s.size() - kMaxToolSummaryBytes) + " bytes]";
}

std::string trim_ws(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r");
  if (b == std::string::npos) return "";
  const auto e = s.find_last_not_of(" \t\r");
  return s.substr(b, e - b + 1);
}

// 把末尾的半行截掉。为什么必须做：append-only 文件在崩溃后**还会继续追加**，
// 不截掉的话残行就永远夹在文件中间，之后每次读取都只能"容忍中间坏行"——
// 那等于放弃对文件完整性的检查（真正的坏行也会被一起吞掉）。
// 返回值 = 是否真的截断了内容。
std::expected<bool, std::string> drop_partial_tail(const std::string& path) {
  std::error_code ec;
  if (!fs::exists(path, ec)) return false;
  std::string data;
  {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::unexpected("无法读取 session: " + path);
    data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (!in.eof() && in.fail()) return std::unexpected("读取 session 失败: " + path);
  }
  if (data.empty() || data.back() == '\n') return false;
  const auto pos = data.rfind('\n');
  const std::size_t keep = (pos == std::string::npos) ? 0 : pos + 1;
  const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0) return std::unexpected("无法打开 session 做尾部修复 " + path + ": " + std::strerror(errno));
  const int rc = ::ftruncate(fd, static_cast<off_t>(keep));
  const int e = errno;
  ::close(fd);
  if (rc != 0) return std::unexpected("截断 session 残行失败 " + path + ": " + std::strerror(e));
  return true;
}

}  // namespace

json message_to_json(const llm::ChatMessage& m, bool synthetic) {
  json j{{"t", "message"}, {"role", m.role}, {"content", m.content}};
  if (!m.tool_call_id.empty()) j["tool_call_id"] = m.tool_call_id;
  if (!m.name.empty()) j["name"] = m.name;
  if (!m.tool_calls.empty()) {
    json calls = json::array();
    for (const auto& c : m.tool_calls) {
      calls.push_back(json{{"id", c.id}, {"name", c.name}, {"arguments", c.arguments}});
    }
    j["tool_calls"] = std::move(calls);
  }
  // 合成消息（崩溃后补的占位结果）必须可分辨：审计时要能看出这不是模型看到过的真实结果
  if (synthetic) j["synthetic"] = true;
  return j;
}

std::expected<llm::ChatMessage, std::string> message_from_json(const json& j) {
  if (!j.is_object()) return std::unexpected("message 不是对象");
  llm::ChatMessage m;
  m.role = str_field(j, "role");
  if (m.role.empty()) return std::unexpected("message 缺少 role");
  m.content = str_field(j, "content");
  m.tool_call_id = str_field(j, "tool_call_id");
  m.name = str_field(j, "name");
  if (j.contains("tool_calls")) {
    if (!j["tool_calls"].is_array()) return std::unexpected("message.tool_calls 不是数组");
    for (const auto& c : j["tool_calls"]) {
      if (!c.is_object()) return std::unexpected("message.tool_calls 元素不是对象");
      llm::ToolCall tc;
      tc.id = str_field(c, "id");
      tc.name = str_field(c, "name");
      tc.arguments = str_field(c, "arguments");
      if (tc.id.empty() || tc.name.empty()) return std::unexpected("tool_call 缺少 id/name");
      m.tool_calls.push_back(std::move(tc));
    }
  }
  return m;
}

std::expected<LoadedSession, std::string> load_session(const std::string& path) {
  std::ifstream in(path);
  if (!in) return std::unexpected("无法打开 session: " + path);

  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) lines.push_back(line);
  // 文件末尾的空行/换行不代表"有一条记录"：先把它们去掉，最后一条记录的判定才准
  while (!lines.empty() && trim_ws(lines.back()).empty()) lines.pop_back();

  LoadedSession out;
  out.path = path;
  bool have_meta = false;

  for (std::size_t i = 0; i < lines.size(); ++i) {
    const bool last = (i + 1 == lines.size());
    const std::string where = "session 第 " + std::to_string(i + 1) + " 行";
    if (trim_ws(lines[i]).empty()) continue;

    auto j = json::parse(lines[i], nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
      // 崩溃只会截断最后一行：跳过并记警告（这是"被中断"的常态）。
      // 中间行坏了说明文件真的坏了——拒绝恢复，别把半份历史当成完整历史去跑。
      if (last) {
        out.warnings.push_back(where + " 是残行（崩溃截断），已跳过");
        continue;
      }
      return std::unexpected(where + " 不是合法 JSON（中间行损坏，拒绝恢复）");
    }
    if (!j.contains("t") || !j["t"].is_string()) {
      if (last) {
        out.warnings.push_back(where + " 缺少 t（残行），已跳过");
        continue;
      }
      return std::unexpected(where + " 缺少 t");
    }
    const std::string t = j["t"].get<std::string>();

    if (t == "meta") {
      out.meta = j;
      have_meta = true;
    } else if (t == "message") {
      auto m = message_from_json(j);
      if (!m) return std::unexpected(where + ": " + m.error());
      out.messages.push_back(std::move(*m));
    } else if (t == "tool") {
      // tool 记录是审计留痕，恢复不依赖它（对话里的 tool 消息才是真值），
      // 所以这里只做类型校验，不让一条坏记录毁掉整个恢复。
      if (!j.contains("name") || !j["name"].is_string()) {
        out.warnings.push_back(where + " tool 记录缺少 name，已忽略");
      }
    } else if (t == "observation") {
      ObservationRecord ob;
      ob.tool = str_field(j, "tool");
      ob.args = obj_field(j, "args");
      ob.text = str_field(j, "text");
      ob.data = obj_field(j, "data");
      ob.truncated = bool_field(j, "truncated");
      ob.original_bytes = static_cast<std::size_t>(std::max<int64_t>(0, i64_field(j, "original_bytes")));
      ob.turn = int_field(j, "turn");
      out.observations.push_back(std::move(ob));
    } else if (t == "stage") {
      const std::string to = str_field(j, "to");
      if (bool_field(j, "allowed")) {
        auto st = stage_from_name(to);
        if (!st) {
          out.warnings.push_back(where + " 记录了未知阶段 " + to + "，阶段状态保持前值");
        } else {
          out.stage = *st;
        }
      }
      // 守卫拒绝（allowed=false）不改阶段，但记录里带着当时的累计值与守卫标志
      out.progress.turns = int_field(j, "turns", out.progress.turns);
      out.progress.cost_yuan = num_field(j, "cost_yuan", out.progress.cost_yuan);
      out.progress.wall_us = i64_field(j, "wall_us", out.progress.wall_us);
      out.used_observe = bool_field(j, "used_observe", out.used_observe);
      out.used_mutating = bool_field(j, "used_mutating", out.used_mutating);
      out.consecutive_rejects = int_field(j, "consecutive_rejects", out.consecutive_rejects);
      if (j.contains("diagnosis") && j["diagnosis"].is_object()) {
        out.diagnosis = diagnosis_from_json(j["diagnosis"]);
      }
    } else if (t == "budget") {
      out.progress.turns = int_field(j, "turns", out.progress.turns);
      out.progress.cost_yuan = num_field(j, "cost_yuan", out.progress.cost_yuan);
      out.progress.wall_us = i64_field(j, "wall_us", out.progress.wall_us);
      out.used_observe = bool_field(j, "used_observe", out.used_observe);
      out.used_mutating = bool_field(j, "used_mutating", out.used_mutating);
      out.consecutive_rejects = int_field(j, "consecutive_rejects", out.consecutive_rejects);
    } else if (t == "end") {
      out.end_kind = str_field(j, "kind");
      out.end_reason = str_field(j, "reason");
      const std::string fs_name = str_field(j, "final_stage");
      if (auto st = stage_from_name(fs_name)) out.end_final_stage = *st;
      // 取消不是"会话结束"：它只是这次 run 被叫停，文件仍然是可恢复的完整快照。
      // 其余结束原因（done / give_up / 预算耗尽 / 错误）一律视为已结束。
      const bool resumable = bool_field(j, "resumable", out.end_kind == "cancelled");
      out.ended = !resumable;
    } else {
      // 未知记录类型：向前兼容（旧版本读新文件不应直接报废）
      out.warnings.push_back(where + " 未知记录类型 " + t + "，已忽略");
    }
  }

  // 一个 session 没有 meta 说明它不是本格式写的：拒绝，而不是拿默认值硬跑
  if (have_meta) {
    // 被 kill 在第一条 message 之前：用 meta 里记的"当时那份" system prompt 与 goal
    // 把开头补齐，恢复出来的上下文才和中断前一致。
    if (out.messages.empty()) {
      const std::string sys = str_field(out.meta, "system_prompt");
      const std::string g = str_field(out.meta, "goal");
      if (!sys.empty()) {
        llm::ChatMessage m;
        m.role = "system";
        m.content = sys;
        out.messages.push_back(std::move(m));
      }
      if (!g.empty()) {
        llm::ChatMessage m;
        m.role = "user";
        m.content = g;
        out.messages.push_back(std::move(m));
      }
    }
  } else if (!lines.empty()) {
    return std::unexpected("session 缺少 meta 记录（不是 " + std::string(kSessionFormat) + "）");
  }

  // 中断在「assistant 的 tool_calls 已落盘、tool 结果还没落盘」之间：
  // 历史里留下的是没有回填的 tool_call，直接发给模型就是 400。这里为缺失的调用生成
  // 合成结果（内容写明是恢复补的），由 Runner 在 resume 时补进历史并落盘。
  std::size_t last_batch = out.messages.size();
  for (std::size_t i = out.messages.size(); i > 0; --i) {
    const auto& m = out.messages[i - 1];
    if (m.role == "assistant" && !m.tool_calls.empty()) {
      last_batch = i - 1;
      break;
    }
  }
  if (last_batch < out.messages.size()) {
    std::vector<std::string> answered;
    bool only_tools = true;
    for (std::size_t i = last_batch + 1; i < out.messages.size(); ++i) {
      const auto& m = out.messages[i];
      if (m.role == "tool") {
        answered.push_back(m.tool_call_id);
      } else {
        only_tools = false;
      }
    }
    if (!only_tools) {
      out.warnings.push_back("最后一条 assistant(tool_calls) 之后还有非 tool 消息，配对检查跳过");
    } else {
      for (const auto& tc : out.messages[last_batch].tool_calls) {
        bool done = false;
        for (const auto& id : answered) {
          if (id == tc.id) {
            done = true;
            break;
          }
        }
        if (done) continue;
        llm::ChatMessage m;
        m.role = "tool";
        m.tool_call_id = tc.id;
        m.name = tc.name;
        m.content = "未执行：进程在这次工具结果落盘前被中断（session 恢复时补的占位结果）";
        out.synthetic_tail.push_back(std::move(m));
      }
      if (!out.synthetic_tail.empty()) {
        out.warnings.push_back("检测到未回填的 tool_call，已生成 " +
                               std::to_string(out.synthetic_tail.size()) + " 条合成结果用于恢复");
      }
    }
  }
  return out;
}

SessionLog::~SessionLog() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  if (lock_fd_ >= 0) {
    // close 会让内核自动释放 flock；显式 LOCK_UN 只是让意图可读
    ::flock(lock_fd_, LOCK_UN);
    ::close(lock_fd_);
    lock_fd_ = -1;
  }
}

std::expected<std::unique_ptr<SessionLog>, std::string> SessionLog::open(const std::string& path,
                                                                        bool create_if_missing) {
  std::error_code ec;
  if (!fs::exists(path, ec) && !create_if_missing) {
    return std::unexpected("session 不存在: " + path);
  }
  const auto parent = fs::path(path).parent_path();
  if (!parent.empty()) fs::create_directories(parent, ec);

  // 单写者锁：<path>.lock 上的 flock。选 flock 而不是 pidfile 的理由——
  // 锁绑在打开的文件描述上，进程以任何方式结束（包括 SIGKILL / 崩溃）时内核自动
  // 释放，不会留下"看起来有人持锁"的僵尸状态；残留的 .lock 文件本身无害。
  const std::string lock_path = path + ".lock";
  const int lock_fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
  if (lock_fd < 0) {
    return std::unexpected("无法打开 session 锁文件 " + lock_path + ": " + std::strerror(errno));
  }
  if (::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
    const int e = errno;
    ::close(lock_fd);
    if (e == EWOULDBLOCK) {
      return std::unexpected("该 session 正被另一个进程写入（" + lock_path +
                             "），拒绝并发追加（会写出两份交错的对话）");
    }
    return std::unexpected("session 加锁失败 " + lock_path + ": " + std::strerror(e));
  }

  const int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0644);
  if (fd < 0) {
    const int e = errno;
    ::flock(lock_fd, LOCK_UN);
    ::close(lock_fd);
    return std::unexpected("无法打开 session " + path + ": " + std::strerror(e));
  }
  return std::unique_ptr<SessionLog>(new SessionLog(path, fd, lock_fd));
}

std::expected<void, std::string> SessionLog::append(const json& line) {
  if (fd_ < 0) return std::unexpected("session 未打开");
  std::string buf = line.dump();
  buf.push_back('\n');
  std::size_t off = 0;
  // 一行一次 write(O_APPEND)：短写要补齐，否则正常写路径也会制造残行。
  // 写失败必须返回错误，不能静默——用户以为有存档、崩溃后什么都没有，是最坏的情况。
  while (off < buf.size()) {
    const ssize_t n = ::write(fd_, buf.data() + off, buf.size() - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return std::unexpected("写 session 失败 " + path_ + ": " + std::strerror(errno));
    }
    if (n == 0) return std::unexpected("写 session 返回 0 字节: " + path_);
    off += static_cast<std::size_t>(n);
  }
  return {};
}

std::expected<void, std::string> SessionLog::write_meta(const std::string& model,
                                                        const std::string& mode,
                                                        const std::string& system_prompt,
                                                        const std::string& goal) {
  return append(json{{"t", "meta"},
                     {"format", kSessionFormat},
                     {"session_id", session_id_of(path_)},
                     {"run_id", run_id_of(path_)},
                     {"started_us", now_us()},
                     {"agent_version", kAgentVersion},
                     {"model", model},
                     {"mode", mode},
                     // 恢复时必须用「当时那份」：换掉 system prompt 等于把模型换了个世界
                     {"system_prompt", system_prompt},
                     {"goal", goal}});
}

std::expected<void, std::string> SessionLog::write_message(const llm::ChatMessage& m,
                                                           bool synthetic) {
  return append(message_to_json(m, synthetic));
}

std::expected<void, std::string> SessionLog::write_tool(const std::string& name, const json& args,
                                                        bool ok, const std::string& result,
                                                        int64_t duration_us, bool approval_denied,
                                                        const std::string& risk, int turn) {
  return append(json{{"t", "tool"},
                     {"turn", turn},
                     {"name", name},
                     {"args", args},
                     {"ok", ok},
                     {"approval_denied", approval_denied},
                     {"risk", risk},
                     {"duration_us", duration_us},
                     {"summary", summarize(result)}});
}

std::expected<void, std::string> SessionLog::write_observation(const ObservationRecord& ob) {
  // 观测全文入盘（"证据必须在真实观测里存在"的审计基础），只对超大文本截断并留标记：
  // 截断是**记录层的上限**，不是模型当时看到的内容——后者在 message(tool) 里。
  std::string text = ob.text;
  bool truncated = ob.truncated;
  const std::size_t original = ob.original_bytes > 0 ? ob.original_bytes : ob.text.size();
  if (text.size() > kMaxObservationBytes) {
    text.resize(kMaxObservationBytes);
    truncated = true;
  }
  return append(json{{"t", "observation"},
                     {"turn", ob.turn},
                     {"tool", ob.tool},
                     {"args", ob.args},
                     {"text", text},
                     {"data", ob.data},
                     {"truncated", truncated},
                     {"original_bytes", original}});
}

std::expected<void, std::string> SessionLog::write_stage(Stage from, const std::string& to,
                                                         bool allowed, const std::string& reason,
                                                         const SessionProgress& p, bool used_observe,
                                                         bool used_mutating,
                                                         int consecutive_rejects,
                                                         const Diagnosis& d, bool forced) {
  json j{{"t", "stage"},
         {"from", stage_name(from)},
         {"to", to},
         {"allowed", allowed},
         {"reason", reason},
         {"turns", p.turns},
         {"cost_yuan", p.cost_yuan},
         {"wall_us", p.wall_us},
         {"used_observe", used_observe},
         {"used_mutating", used_mutating},
         {"consecutive_rejects", consecutive_rejects},
         {"diagnosis", diagnosis_to_json(d)}};
  if (forced) j["forced"] = true;
  return append(j);
}

std::expected<void, std::string> SessionLog::write_budget(const SessionProgress& p,
                                                          bool used_observe, bool used_mutating,
                                                          int consecutive_rejects) {
  return append(json{{"t", "budget"},
                     {"turns", p.turns},
                     {"cost_yuan", p.cost_yuan},
                     {"wall_us", p.wall_us},
                     {"used_observe", used_observe},
                     {"used_mutating", used_mutating},
                     {"consecutive_rejects", consecutive_rejects}});
}

std::expected<void, std::string> SessionLog::write_end(Stage final_stage,
                                                       const std::string& reason,
                                                       const SessionProgress& p) {
  // kind 是给人和脚本看的分类；resumable 是恢复的判据（只有"取消"可恢复）
  std::string kind = "error";
  if (final_stage == Stage::Done && reason.empty()) {
    kind = "done";
  } else if (reason == "cancelled") {
    kind = "cancelled";
  } else if (reason.find("budget exhausted") != std::string::npos) {
    kind = "budget";
  } else if (reason.find("too many guard rejects") != std::string::npos) {
    kind = "guard_reject";
  } else if (reason == "done without verify" || reason == "run ended without reaching done") {
    kind = "incomplete";
  }
  return append(json{{"t", "end"},
                     {"final_stage", stage_name(final_stage)},
                     {"kind", kind},
                     {"reason", reason},
                     {"turns", p.turns},
                     {"cost_yuan", p.cost_yuan},
                     {"wall_us", p.wall_us},
                     {"resumable", kind == "cancelled"}});
}

std::expected<SessionOpen, std::string> open_session(const std::string& path, bool must_exist) {
  std::error_code ec;
  const bool exists = fs::exists(path, ec);
  const std::uintmax_t size = exists ? fs::file_size(path, ec) : 0;
  if (must_exist && (!exists || size == 0)) {
    return std::unexpected("session 不存在或为空: " + path);
  }

  // 先拿锁再读：否则"读到旧状态 → 另一个进程追加 → 我们也在旧状态上追加"会写出
  // 两份互相矛盾的对话。SessionLog::open 内部先 flock，成功后文件已经归本进程独占。
  auto log = SessionLog::open(path, /*create_if_missing=*/!must_exist);
  if (!log) return std::unexpected(log.error());

  // 锁到手之后再取一次大小（上面 stat 和加锁之间有窗口）
  const std::uintmax_t locked_size = fs::file_size(path, ec);
  SessionOpen out;
  if (locked_size > 0) {
    auto loaded = load_session(path);
    if (!loaded) {
      return std::unexpected(loaded.error());
    }
    // 残行截断：必须在追加之前做（见 drop_partial_tail 的注释）
    auto dropped = drop_partial_tail(path);
    if (!dropped) return std::unexpected(dropped.error());
    if (*dropped) loaded->warnings.push_back("文件尾部有半行（上次写到一半被 kill），已截断");

    if (loaded->ended) {
      return std::unexpected("该会话已结束（final_stage=" + std::string(stage_name(loaded->end_final_stage)) +
                             ", kind=" + loaded->end_kind + ", reason=" + loaded->end_reason +
                             "），拒绝续写：" + path);
    }
    if (loaded->stage == Stage::Done || loaded->stage == Stage::GiveUp) {
      return std::unexpected("该会话已结束（最后一次阶段是 " +
                             std::string(stage_name(loaded->stage)) + "），拒绝续写：" + path);
    }
    out.resumed = std::move(*loaded);
  }
  out.log = std::move(*log);
  return out;
}

}  // namespace cue::agent
