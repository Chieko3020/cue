// session_index：把 agent 会话文件（runs/<run_id>/sessions/<session_id>.jsonl）读成
// 控制台能直接渲染的 JSON（FR-UI.3 的「按阶段组织的轨迹」）。
//
// 为什么在控制台里再解析一遍，而不复用 agent/session.h 的 load_session：
//   load_session 的产物是**恢复状态**（阶段停在哪儿、预算剩多少、合成 tool 结果），
//   它按语义把记录投影成结构体：tool 记录只剩"校验一下有没有 name"、观测与消息各自
//   成数组、原始顺序丢失。轨迹视图要的恰好是相反的东西——**原始顺序 + 原始字段**
//   （工具参数、结果摘要、耗时、风险级、守卫拒绝原因）。两者目的不同，复用会让
//   "给恢复用的模型"越改越像"给渲染用的模型"。
//   这里刻意不理解记录语义：只按 t 分组、保留全部字段。写端以后加字段，视图自动
//   多一列可看；视图也不会把"恢复判据"（什么算 ended、什么是守卫拒绝）再实现一遍——
//   那是 agent 侧的事，重复实现才是漂移的来源。
//
// 与恢复的另一个差别（容错口径）：恢复遇到**中间坏行**必须拒绝（半份历史被当成完整
// 历史去跑，比拒绝更糟）；视图是只读审计，中间坏行只跳过并记 warning，不能因为一行
// 坏了就把整次运行藏起来。
#pragma once

#include <algorithm>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <sys/stat.h>

#include <nlohmann/json.hpp>

namespace cue::console {

namespace detail_session {

namespace fs = std::filesystem;
using json = nlohmann::json;

// 读入上限：会话文件是 append-only 的，正常几 KB～几百 KB。超过上限时只解析前面这一段
// 并留 warning（界面上要能看出"这不是全文"），不作整体失败——审计视图看半份也比空白强。
constexpr std::size_t kMaxSessionBytes = 8u * 1024 * 1024;
constexpr std::size_t kMaxSessionLines = 200000;

inline std::int64_t mtime_us_of(const std::string& path) {
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) return 0;
  return static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1000000 + st.st_mtim.tv_nsec / 1000;
}

inline std::uintmax_t bytes_of(const std::string& path) {
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) return 0;
  return static_cast<std::uintmax_t>(st.st_size);
}

inline std::string str_field(const json& j, const char* k, const std::string& def = "") {
  if (!j.contains(k) || !j[k].is_string()) return def;
  return j[k].get<std::string>();
}

inline int int_field(const json& j, const char* k, int def = 0) {
  if (!j.contains(k)) return def;
  const auto& v = j[k];
  if (v.is_number_integer()) return v.get<int>();
  if (v.is_number_unsigned()) return static_cast<int>(v.get<unsigned>());
  return def;
}

inline std::int64_t i64_field(const json& j, const char* k, std::int64_t def = 0) {
  if (!j.contains(k)) return def;
  const auto& v = j[k];
  if (v.is_number_integer()) return v.get<std::int64_t>();
  if (v.is_number_unsigned()) return static_cast<std::int64_t>(v.get<std::uint64_t>());
  return def;
}

inline double num_field(const json& j, const char* k, double def = 0.0) {
  if (!j.contains(k) || !j[k].is_number()) return def;
  return j[k].get<double>();
}

inline bool known_stage(const std::string& s) {
  return s == "collect" || s == "diagnose" || s == "repair" || s == "verify" || s == "done" ||
         s == "give_up";
}

// 一次解析的产物：列表只要摘要，详情还要分组记录。两种用途共用同一遍解析，
// 避免"列表和详情对同一个文件给出不同数字"（那是最难查的一类不一致）。
struct Parsed {
  json meta = nullptr;
  json groups = json::object();  // 只有 keep_records 时填充
  std::vector<std::string> warnings;
  // 摘要字段
  std::string session_id;
  std::string run_id;
  std::string format;
  std::string mode;
  std::string model;
  std::string goal;
  std::int64_t started_us = 0;
  std::string final_stage;
  std::string end_kind;
  std::string end_reason;
  bool ended = false;
  bool have_end = false;
  int turns = 0;
  double cost_yuan = 0.0;
  std::int64_t wall_us = 0;
  int lines = 0;
  int bad_lines = 0;
  bool truncated = false;  // 文件超过上限，只解析了前半段
  json counts = json::object();
};

inline void bump(json& counts, const std::string& t) {
  if (!counts.contains(t) || !counts[t].is_number_unsigned()) counts[t] = 0u;
  counts[t] = counts[t].get<unsigned>() + 1u;
}

// 解析一个 JSONL 会话文件。keep_records=true 时把每组记录也带上（带 seq = 行号）。
inline Parsed parse_session_file(const std::string& path, bool keep_records) {
  Parsed out;
  // 坏行很多时不要把几百条 warning 全塞进响应：前 40 条逐条列，其余只计数。
  std::size_t suppressed = 0;
  auto warn = [&out, &suppressed](std::string text) {
    if (out.warnings.size() < 40) {
      out.warnings.push_back(std::move(text));
    } else {
      ++suppressed;
    }
  };

  std::ifstream in(path, std::ios::binary);
  if (!in) {
    warn("无法打开 session：" + path);
    return out;
  }

  if (keep_records) {
    out.groups = json{{"messages", json::array()}, {"tools", json::array()},
                      {"observations", json::array()}, {"stages", json::array()},
                      {"budgets", json::array()}, {"ends", json::array()},
                      {"other", json::array()}};
  }

  std::string line;
  std::size_t file_bytes = 0;
  int idx = 0;
  while (std::getline(in, line)) {
    file_bytes += line.size() + 1;
    if (file_bytes > kMaxSessionBytes || static_cast<std::size_t>(idx) >= kMaxSessionLines) {
      out.truncated = true;
      warn("session 超过解析上限（" + std::to_string(kMaxSessionBytes) +
                             " 字节 / " + std::to_string(kMaxSessionLines) + " 行），后面的记录没有读");
      break;
    }
    const int seq = idx++;
    // 末尾空行不代表有一条记录（append-only 文件以 '\n' 结尾）
    if (line.find_first_not_of(" \t\r") == std::string::npos) continue;

    json j = json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
      ++out.bad_lines;
      warn("第 " + std::to_string(seq + 1) + " 行不是合法 JSON，已跳过（只影响这一行）");
      continue;
    }
    if (!j.contains("t") || !j["t"].is_string()) {
      ++out.bad_lines;
      warn("第 " + std::to_string(seq + 1) + " 行缺少 t 字段，已跳过");
      continue;
    }
    const std::string t = j["t"].get<std::string>();
    ++out.lines;
    bump(out.counts, t);
    if (keep_records) j["seq"] = seq;

    if (t == "meta") {
      if (out.meta.is_null()) {
        out.meta = j;
        out.format = str_field(j, "format");
        out.session_id = str_field(j, "session_id");
        out.run_id = str_field(j, "run_id");
        out.mode = str_field(j, "mode");
        out.model = str_field(j, "model");
        out.goal = str_field(j, "goal");
        out.started_us = i64_field(j, "started_us");
      } else if (keep_records) {
        warn("第 " + std::to_string(seq + 1) + " 行是第二条 meta（已忽略，保留第一条）");
        out.groups["other"].push_back(std::move(j));
      }
    } else if (t == "stage") {
      // 只有 allowed=true 的迁移才真的改变阶段；守卫拒绝记录也保留（它是"agent 想越权"
      // 的唯一留痕），但不参与摘要阶段。
      if (j.value("allowed", false)) {
        const std::string to = str_field(j, "to");
        if (known_stage(to)) out.final_stage = to;
      }
      out.turns = int_field(j, "turns", out.turns);
      out.cost_yuan = num_field(j, "cost_yuan", out.cost_yuan);
      out.wall_us = i64_field(j, "wall_us", out.wall_us);
      if (keep_records) out.groups["stages"].push_back(std::move(j));
    } else if (t == "budget") {
      out.turns = int_field(j, "turns", out.turns);
      out.cost_yuan = num_field(j, "cost_yuan", out.cost_yuan);
      out.wall_us = i64_field(j, "wall_us", out.wall_us);
      if (keep_records) out.groups["budgets"].push_back(std::move(j));
    } else if (t == "end") {
      out.have_end = true;
      out.end_kind = str_field(j, "kind");
      out.end_reason = str_field(j, "reason");
      const std::string fs_name = str_field(j, "final_stage");
      if (known_stage(fs_name)) out.final_stage = fs_name;
      // resumable 是恢复判据（cancelled 才算"没结束"），摘要里照抄，不自己再判一次
      out.ended = !j.value("resumable", out.end_kind == "cancelled");
      out.turns = int_field(j, "turns", out.turns);
      out.cost_yuan = num_field(j, "cost_yuan", out.cost_yuan);
      out.wall_us = i64_field(j, "wall_us", out.wall_us);
      if (keep_records) out.groups["ends"].push_back(std::move(j));
    } else if (t == "message") {
      if (keep_records) out.groups["messages"].push_back(std::move(j));
    } else if (t == "tool") {
      if (keep_records) out.groups["tools"].push_back(std::move(j));
    } else if (t == "observation") {
      if (keep_records) {
        out.groups["observations"].push_back(std::move(j));
      }
    } else {
      // 未知记录类型：向前兼容，原样留在 other 里（视图要能看见新版写的东西）
      warn("第 " + std::to_string(seq + 1) + " 行是未知记录类型 " + t + "，原样保留在 other");
      if (keep_records) out.groups["other"].push_back(std::move(j));
    }
  }
  if (suppressed > 0) {
    out.warnings.push_back("另有 " + std::to_string(suppressed) + " 条同类告警被省略");
  }
  if (!out.meta.is_null()) {
    // session_id/run_id 以 meta 为准；缺失时用路径反推（老文件可能没有这两个字段）
    if (out.session_id.empty()) out.session_id = fs::path(path).stem().string();
    if (out.run_id.empty()) {
      fs::path p = fs::path(path).parent_path();
      if (p.filename() == "sessions") p = p.parent_path();
      out.run_id = p.filename().string();
    }
  }
  return out;
}

inline json summary_json(const Parsed& p, const std::string& path) {
  return json{{"session_id", p.session_id},
              {"run_id", p.run_id},
              {"path", path},
              {"format", p.format},
              {"mode", p.mode},
              {"model", p.model},
              {"goal", p.goal},
              {"started_us", p.started_us},
              {"final_stage", p.final_stage},
              {"turns", p.turns},
              {"cost_yuan", p.cost_yuan},
              {"wall_us", p.wall_us},
              {"ended", p.ended},
              {"end_kind", p.end_kind},
              {"end_reason", p.end_reason},
              {"bytes", bytes_of(path)},
              {"mtime_us", mtime_us_of(path)},
              {"records", p.lines},
              {"bad_lines", p.bad_lines},
              {"counts", p.counts}};
}

}  // namespace detail_session

// id 会参与拼路径（虽然本模块只拿它去匹配扫描结果，不直接拼），所以校验从严：
// 只允许字母数字与 . _ -，且任何位置出现 ".." 一律拒。不是白名单式的"过滤"，
// 是"看起来像想跳目录就直接拒绝"——拒绝比修正更容易审计。
inline bool safe_session_component(const std::string& s) {
  if (s.empty() || s.size() > 200) return false;
  if (s.find("..") != std::string::npos) return false;
  for (const char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '_' || c == '-';
    if (!ok) return false;
  }
  return s != ".";
}

// 合法的会话 id：`<session_id>` 或 `<run_id>/<session_id>`（后者只有一个 '/'）。
// 语法校验与 find_session_path 用同一个判据，这样 HTTP 层能据此区分 400（写法非法）
// 与 404（写法合法但没有这个会话）——两者对调用者是不同的事情。
inline bool valid_session_id(const std::string& id) {
  const auto slash = id.find('/');
  if (slash == std::string::npos) return safe_session_component(id);
  if (id.find('/', slash + 1) != std::string::npos) return false;
  return safe_session_component(id.substr(0, slash)) && safe_session_component(id.substr(slash + 1));
}

// 列出 runs/*/sessions/*.jsonl，按文件 mtime 倒序（最新一次运行在最上面）。
inline nlohmann::json scan_sessions(const std::string& runs_dir, std::size_t max_sessions = 50) {
  using namespace detail_session;
  namespace fs = std::filesystem;
  json out = json::object();
  out["dir"] = runs_dir;
  out["sessions"] = json::array();
  std::error_code ec;
  if (!fs::is_directory(runs_dir, ec)) {
    out["error"] = "runs 目录不存在：" + runs_dir;
    return out;
  }
  struct Entry {
    std::string path;
    std::int64_t mtime = 0;
  };
  std::vector<Entry> entries;
  for (const auto& run_dir : fs::directory_iterator(runs_dir, fs::directory_options::skip_permission_denied, ec)) {
    if (!run_dir.is_directory(ec)) continue;
    const fs::path sess_dir = run_dir.path() / "sessions";
    std::error_code ec2;
    if (!fs::is_directory(sess_dir, ec2)) continue;
    for (const auto& de : fs::directory_iterator(sess_dir, fs::directory_options::skip_permission_denied, ec2)) {
      if (!de.is_regular_file(ec2)) continue;
      if (de.path().extension() != ".jsonl") continue;  // .jsonl.lock 与其它文件一律不收
      entries.push_back(Entry{de.path().string(), mtime_us_of(de.path().string())});
    }
  }
  if (ec) out["error"] = "读取 runs 目录失败：" + ec.message();
  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    if (a.mtime != b.mtime) return a.mtime > b.mtime;
    return a.path > b.path;
  });
  std::size_t n = 0;
  for (const auto& e : entries) {
    if (n++ >= max_sessions) break;
    const auto p = parse_session_file(e.path, /*keep_records=*/false);
    out["sessions"].push_back(summary_json(p, e.path));
  }
  return out;
}

// 会话详情：逐类分组 + 摘要 + 警告。seq 是文件里的行号，前端据此把分组重新按时间
// 拼回一条时间线（分组是为了渲染方便，顺序是事实，两者都要给）。
inline nlohmann::json read_session_view(const std::string& path) {
  using namespace detail_session;
  auto p = parse_session_file(path, /*keep_records=*/true);
  json out = summary_json(p, path);
  out["ok"] = true;
  out["meta"] = p.meta;
  out["warnings"] = p.warnings;
  out["truncated"] = p.truncated;
  for (auto it = p.groups.begin(); it != p.groups.end(); ++it) out[it.key()] = it.value();
  return out;
}

// 由 id 定位会话文件：**只在扫描结果里找**，不拿用户输入拼路径——这样"路径穿越"
// 不是靠过滤关键词挡住的，而是根本没有拼接这一步。返回的错误文本区分「非法 id」
// 与「没有这个会话」，两者对调用者是不同的（400 vs 404）。
inline std::expected<std::string, std::string> find_session_path(const std::string& runs_dir,
                                                                 const std::string& id) {
  namespace fs = std::filesystem;
  // 两种写法：`<session_id>` 或 `<run_id>/<session_id>`（同名 session 在不同 run 下时用后者）。
  // 每一段都走同一个白名单校验，所以 "/" 本身不是"允许的字符"，而是唯一合法的分隔符。
  std::string want_run;
  std::string want_sid = id;
  if (const auto slash = id.find('/'); slash != std::string::npos) {
    if (id.find('/', slash + 1) != std::string::npos) {
      return std::unexpected("session id 只能有一个 '/' 分隔 run_id 与 session_id：" + id);
    }
    want_run = id.substr(0, slash);
    want_sid = id.substr(slash + 1);
    if (!safe_session_component(want_run)) {
      return std::unexpected("run_id 非法（只允许字母数字与 . _ -，且不能包含 ..）：" + want_run);
    }
  }
  if (!safe_session_component(want_sid)) {
    return std::unexpected("session id 非法（只允许字母数字与 . _ -，且不能包含 ..）：" + want_sid);
  }
  const auto all = scan_sessions(runs_dir, 1000);
  if (all.contains("error")) return std::unexpected(all["error"].get<std::string>());
  std::string best;
  std::int64_t best_mtime = -1;
  for (const auto& s : all["sessions"]) {
    const std::string sid = s.value("session_id", std::string());
    const std::string rid = s.value("run_id", std::string());
    if (sid != want_sid) continue;
    if (!want_run.empty() && rid != want_run) continue;
    const std::int64_t m = s.value("mtime_us", std::int64_t{0});
    if (m > best_mtime) {
      best_mtime = m;
      best = s.value("path", std::string());
    }
  }
  if (best.empty()) return std::unexpected("没有这个会话：" + id + "（在 " + runs_dir + " 下没找到）");
  // 纵深防御：即使扫描结果被符号链接/手改文件构造过，也只放行 runs 目录里的真实文件
  std::error_code ec;
  const auto root = fs::weakly_canonical(fs::path(runs_dir), ec);
  const auto real = fs::weakly_canonical(fs::path(best), ec);
  if (ec) return std::unexpected("解析会话路径失败：" + ec.message());
  const auto root_s = root.string();
  const auto real_s = real.string();
  if (real_s.compare(0, root_s.size(), root_s) != 0 ||
      (real_s.size() > root_s.size() && real_s[root_s.size()] != '/')) {
    return std::unexpected("拒绝读取 runs 目录之外的文件：" + real_s);
  }
  // 返回**扫描到的原始路径**而不是 canonical 的：列表与详情里的 path 字段必须一致，
  // 检测多出来的那层软链只会让用户看到两条不同的路径。
  return best;
}

}  // namespace cue::console
