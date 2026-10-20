// report_index：把评测产物（runs/<id>/report.md）读成控制台能直接渲染的 JSON
//
// 为什么解析 Markdown 而不是加一个 report.json：本轮边界不允许改 src/eval/*，
// 而 report.md 的版式由评测引擎自己产且已稳定（M4）。解析器只认「行首 ``- key: value``」
// 与「Markdown 表格」，遇到未知行直接忽略——报告将来加字段不会把控制台打挂。
//
// 缺字段一律给 null / 空串，不编造 0（"没采到"与"0"在 UI 上是两件事，沿用 M2 口径）。
#pragma once

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace cue::console {

namespace detail {

inline std::string trim(std::string s) {
  auto is_ws = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!s.empty() && is_ws(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
  while (!s.empty() && is_ws(static_cast<unsigned char>(s.back()))) s.pop_back();
  return s;
}

inline std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> out;
  std::stringstream ss(text);
  std::string line;
  while (std::getline(ss, line)) out.push_back(line);
  return out;
}

// 一行 Markdown 表格 → 单元格（去掉首尾竖线与两边空白）
inline std::vector<std::string> table_cells(const std::string& line) {
  std::string s = trim(line);
  if (s.empty() || s.front() != '|') return {};
  if (!s.empty() && s.back() == '|') s.pop_back();
  if (!s.empty() && s.front() == '|') s.erase(s.begin());
  std::vector<std::string> cells;
  std::string cur;
  for (const char ch : s) {
    if (ch == '|') {
      cells.push_back(trim(cur));
      cur.clear();
    } else {
      cur.push_back(ch);
    }
  }
  cells.push_back(trim(cur));
  return cells;
}

inline bool is_separator_row(const std::vector<std::string>& cells) {
  if (cells.empty()) return false;
  for (const auto& c : cells) {
    if (c.empty()) continue;
    for (const char ch : c) {
      if (ch != '-' && ch != ':' && ch != ' ') return false;
    }
  }
  return true;
}

// 反引号里的值（报告里 run_id / suite 都是 `x`），没有反引号就整段
inline std::string unquote(std::string s) {
  const auto a = s.find('`');
  if (a == std::string::npos) return trim(s);
  const auto b = s.find('`', a + 1);
  if (b == std::string::npos) return trim(s.substr(a + 1));
  return s.substr(a + 1, b - a - 1);
}

inline bool parse_number(const std::string& s, double& out) {
  // 允许 "¥0.02" / "0.02" / "30.3"：只按 ASCII 数字找起点，
  // 其余字节（含 ¥ 的 UTF-8 三字节）一律跳过——不要在 char 上比较非 ASCII 字面量。
  std::size_t i = 0;
  auto is_digit = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; };
  while (i < s.size()) {
    if (is_digit(s[i])) break;
    if ((s[i] == '-' || s[i] == '+') && i + 1 < s.size() && is_digit(s[i + 1])) break;
    ++i;
  }
  if (i >= s.size()) return false;
  errno = 0;
  char* end = nullptr;
  const double v = std::strtod(s.c_str() + i, &end);
  if (errno != 0 || end == nullptr || end == s.c_str() + i) return false;
  out = v;
  return true;
}

// "计划 2，执行 2，计分 2，运行失败 0" → 取 key 之后的第一个整数
inline int int_after(const std::string& line, const std::string& key) {
  const auto pos = line.find(key);
  if (pos == std::string::npos) return -1;
  for (std::size_t i = pos + key.size(); i < line.size(); ++i) {
    if (std::isdigit(static_cast<unsigned char>(line[i]))) {
      int v = 0;
      while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) {
        v = v * 10 + (line[i] - '0');
        ++i;
      }
      return v;
    }
  }
  return -1;
}

// 分层成功率表格的行名 → 稳定字段名（报告未来加中文行也不会打挂）
inline std::string rate_key(const std::string& name) {
  if (name.find("根因") != std::string::npos) return "root";
  if (name.find("证据") != std::string::npos) return "evidence";
  if (name.find("SLA") != std::string::npos) return "sla";
  if (name.find("pass^") != std::string::npos) return "pass_k";
  if (name.find("Run Error") != std::string::npos || name.find("run_error") != std::string::npos)
    return "run_error";
  if (name.find("MTTR") != std::string::npos) return "mttr";
  return "";
}

}  // namespace detail

// 解析 report.md 全文
inline nlohmann::json parse_report(const std::string& md) {
  using namespace detail;
  nlohmann::json out = nlohmann::json::object();
  out["run_id"] = "";
  out["suite"] = "";
  out["agent"] = "";
  out["model"] = "";
  out["repeat"] = nullptr;
  out["cost_yuan"] = nullptr;
  out["cases"] = nlohmann::json{{"planned", nullptr}, {"executed", nullptr}, {"scored", nullptr},
                                {"error", nullptr}};
  out["rates"] = nlohmann::json::array();
  out["rate_map"] = nlohmann::json::object();
  out["case_rows"] = nlohmann::json::array();
  out["failed"] = nlohmann::json::array();
  out["ok"] = true;

  std::string section;
  std::vector<std::string> case_header;
  bool case_header_seen = false;

  for (const auto& raw : split_lines(md)) {
    const std::string line = trim(raw);
    if (line.empty()) continue;
    if (line.rfind("## ", 0) == 0) {
      section = line.substr(3);
      continue;
    }
    if (line.rfind("- ", 0) == 0) {
      const auto colon = line.find(':');
      if (colon == std::string::npos) continue;
      const std::string key = trim(line.substr(2, colon - 2));
      const std::string val = trim(line.substr(colon + 1));
      if (key == "run_id") {
        out["run_id"] = unquote(val);
      } else if (key == "suite") {
        out["suite"] = unquote(val);
      } else if (key == "agent") {
        out["agent"] = unquote(val);
        if (const auto p = val.find("repeat="); p != std::string::npos) {
          const int r = int_after(val.substr(p), "repeat=");
          if (r >= 0) out["repeat"] = r;
        }
      } else if (key == "生效配置") {
        if (const auto p = val.find("model=`"); p != std::string::npos) {
          out["model"] = unquote(val.substr(p + 6));
        }
      } else if (key == "用例") {
        out["cases"]["planned"] = int_after(val, "计划");
        out["cases"]["executed"] = int_after(val, "执行");
        out["cases"]["scored"] = int_after(val, "计分");
        out["cases"]["error"] = int_after(val, "运行失败");
      } else if (key == "成本") {
        double v = 0;
        if (parse_number(val, v)) out["cost_yuan"] = v;
      }
      continue;
    }
    if (line.front() != '|') continue;
    const auto cells = table_cells(line);
    if (cells.empty() || is_separator_row(cells)) continue;

    if (section.find("分层成功率") != std::string::npos) {
      if (cells[0] == "指标") continue;
      if (cells.size() >= 2) {
        const std::string k = rate_key(cells[0]);
        out["rates"].push_back(nlohmann::json{{"name", cells[0]}, {"value", cells[1]}});
        if (!k.empty()) out["rate_map"][k] = cells[1];
      }
    } else if (section.find("逐用例") != std::string::npos) {
      if (!case_header_seen) {
        case_header = cells;
        case_header_seen = true;
        continue;
      }
      nlohmann::json row = nlohmann::json::object();
      for (std::size_t i = 0; i < cells.size() && i < case_header.size(); ++i) {
        row[case_header[i]] = cells[i];
      }
      // ✓/✗/n-a 是判分三列；不是 ✓ 就算这条 case 没拿到分（n/a 也算，UI 里如实显示）
      bool ok = true;
      for (const char* col : {"root", "evidence", "sla"}) {
        auto it = row.find(col);
        if (it == row.end() || !it->is_string() || it->get<std::string>() != "✓") ok = false;
      }
      row["ok"] = ok;
      if (!ok) {
        auto it = row.find("case");
        out["failed"].push_back(it != row.end() ? *it : nlohmann::json(""));
        out["ok"] = false;
      }
      out["case_rows"].push_back(std::move(row));
    }
  }
  return out;
}

// 扫描 runs 目录（只认含 report.md 的子目录），按 mtime 倒序，最多 max_runs 个
inline nlohmann::json scan_runs(const std::string& dir, std::size_t max_runs = 20) {
  namespace fs = std::filesystem;
  nlohmann::json out = nlohmann::json::object();
  out["dir"] = dir;
  out["runs"] = nlohmann::json::array();
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    out["error"] = "runs 目录不存在：" + dir;
    return out;
  }
  struct Entry {
    std::string name;
    std::int64_t mtime = 0;
  };
  std::vector<Entry> entries;
  for (const auto& de : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
    if (!de.is_directory(ec)) continue;
    const std::string name = de.path().filename().string();
    if (name.rfind("run-", 0) != 0) continue;
    std::error_code ec2;
    const auto rep = de.path() / "report.md";
    if (!fs::is_regular_file(rep, ec2)) continue;
    const auto ft = fs::last_write_time(rep, ec2);
    std::int64_t m = 0;
    if (!ec2) m = static_cast<std::int64_t>(ft.time_since_epoch().count());
    entries.push_back(Entry{name, m});
  }
  if (ec) out["error"] = "读取 runs 目录失败：" + ec.message();
  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    if (a.mtime != b.mtime) return a.mtime > b.mtime;
    return a.name > b.name;
  });
  std::size_t n = 0;
  for (const auto& e : entries) {
    if (n++ >= max_runs) break;
    const fs::path rep = fs::path(dir) / e.name / "report.md";
    std::ifstream in(rep, std::ios::binary);
    if (!in) continue;
    std::stringstream buf;
    buf << in.rdbuf();
    nlohmann::json r = parse_report(buf.str());
    if (r["run_id"].get<std::string>().empty()) r["run_id"] = e.name;
    r["dir"] = e.name;
    r["mtime"] = e.mtime;
    out["runs"].push_back(std::move(r));
  }
  return out;
}

}  // namespace cue::console
