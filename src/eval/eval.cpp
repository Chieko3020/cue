#include "cue/eval/eval.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include "cue/common/clock.h"
#include "cue/common/sha256.h"
#include "cue/eval/state.h"
#include "cue/llm/client.h"
#include "cue/pipeline/tools.h"

namespace cue::eval {

bool has_value(double v) { return !std::isnan(v); }

namespace {

// ---------------------------------------------------------------- 证据匹配
//
// 一套 tokenizer 同时给「证据」和「观测」用（review-eval.md #24：原来 normalize /
// citations / text_value 三套提取器边界不一致，证据与观测的解析规则不同 → 可被钻空子）。

struct Citation {
  std::string key;
  double value = 0;
  std::size_t begin = 0;  // 覆盖区间 [begin, end)：用于判断证据是否只剩解释性文字
  std::size_t end = 0;
};

bool key_char(char ch) {
  const unsigned char u = static_cast<unsigned char>(ch);
  return std::isalnum(u) || ch == '_';
}

// SPEC §7.6 的「归一化子串」：把 JSON 的 "k": v 与证据里的 k=v 归一成同一种写法
std::string normalize(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  bool prev_space = true;
  for (char ch : s) {
    const unsigned char u = static_cast<unsigned char>(ch);
    if (std::isalnum(u) || ch == '.' || ch == '_') {
      out.push_back(static_cast<char>(std::tolower(u)));
      prev_space = false;
    } else if (!prev_space) {
      out.push_back(' ');
      prev_space = true;
    }
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

// 抽出所有「key = value / "key": value / key value」引用。
// 不跨 ( [ 提取：query_metrics(5000ms) 不能被当成 query_metrics=5000。
std::vector<Citation> find_citations(const std::string& raw) {
  std::vector<Citation> out;
  std::size_t i = 0;
  while (i < raw.size()) {
    const unsigned char u = static_cast<unsigned char>(raw[i]);
    if (!(std::isalpha(u) || raw[i] == '_')) {
      ++i;
      continue;
    }
    const std::size_t k0 = i;
    while (i < raw.size() && key_char(raw[i])) ++i;
    std::string key;
    key.reserve(i - k0);
    for (std::size_t j = k0; j < i; ++j) {
      key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(raw[j]))));
    }
    std::size_t j = i;
    while (j < raw.size() && (std::isspace(static_cast<unsigned char>(raw[j])) || raw[j] == '=' ||
                              raw[j] == ':' || raw[j] == '"' || raw[j] == '\'')) {
      ++j;
    }
    const std::size_t num0 = j;
    if (j < raw.size() && (std::isdigit(static_cast<unsigned char>(raw[j])) || raw[j] == '-' ||
                           raw[j] == '+')) {
      ++j;
      while (j < raw.size() &&
             (std::isdigit(static_cast<unsigned char>(raw[j])) || raw[j] == '.')) {
        ++j;
      }
      try {
        out.push_back(Citation{key, std::stod(raw.substr(num0, j - num0)), k0, j});
      } catch (const std::exception&) {
        // 数字解析失败就当没引用
      }
    }
    i = std::max(i, j);
  }
  return out;
}

// 计数类指标必须精确（injected=60 不能对上 59）；连续量按 SPEC §7.6 的 ±2% 容差
bool value_close(double a, double b) {
  const bool a_int = std::fabs(a - std::round(a)) < 1e-9;
  const bool b_int = std::fabs(b - std::round(b)) < 1e-9;
  if (a_int && b_int) return a == b;
  // 一方整数一方小数：只接受"写法不同、值相同"（59.0 vs 59）。
  // 否则 injected=59.4 会被 ±2% 容差放过（review-eval.md #5 实测）。
  if (a_int != b_int) return std::fabs(a - b) < 1e-9;
  return std::fabs(a - b) <= std::max(0.05, 0.02 * std::fabs(b));
}

bool data_value(const json& d, const std::string& key, double& out) {
  if (!d.is_object()) return false;
  for (auto it = d.begin(); it != d.end(); ++it) {
    std::string k = it.key();
    for (auto& ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (k != key) continue;
    if (it.value().is_number()) {
      out = it.value().get<double>();
      return true;
    }
    return false;
  }
  return false;
}

// 证据里允许出现的非引用单词：工具名、字段容器的名字、单位、布尔字面量。
// 其余（尤其是中文句子）都算"解释性文字"，见 citation_only。
bool residual_allowed(const std::string& word) {
  static const std::set<std::string> kMeta = {
      "query_metrics", "fetch_observation", "pipeline_set_param", "inject_fault",
      "pipeline_start", "pipeline_stop", "pipeline_restart", "run_shell",
      "metrics",       "data",              "text",                 "result",
      "ok",            "true",              "false",                "null",
      "cued",          "running",           "pid",                  "on",
      "off",           "ms",                "us",                   "s",
      "pct",           "kbps",              "mbps",                 "fps"};
  std::string lower;
  lower.reserve(word.size());
  for (char ch : word) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  if (kMeta.count(lower) != 0) return true;
  // 数字 + 可选单位：5000ms / 1500kbps / 30
  std::size_t i = 0;
  while (i < lower.size() && (std::isdigit(static_cast<unsigned char>(lower[i])) || lower[i] == '.')) ++i;
  if (i == 0) return false;
  while (i < lower.size() && std::isalpha(static_cast<unsigned char>(lower[i]))) ++i;
  return i == lower.size();
}

// 证据必须是"逐字摘抄的 key=value 片段"：除引用、工具名、单位外不得有别的文字。
// 否则「整段观测原文 + 一句编造结论」会靠引用全部命中拿分（review-eval.md 构造 A 实测）。
bool citation_only(const std::string& raw, const std::vector<Citation>& cites) {
  std::vector<char> covered(raw.size(), 0);
  for (const auto& c : cites) {
    for (std::size_t k = c.begin; k < c.end && k < raw.size(); ++k) covered[k] = 1;
  }
  std::size_t i = 0;
  while (i < raw.size()) {
    const unsigned char u = static_cast<unsigned char>(raw[i]);
    if (u >= 0x80) return false;  // 非 ASCII 解释性文字（中文结论）直接判伪
    if (covered[i] != 0 || std::isspace(u)) {
      ++i;
      continue;
    }
    if (std::isalnum(u) || raw[i] == '_' || raw[i] == '.') {
      const std::size_t s = i;
      while (i < raw.size()) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if (!(std::isalnum(c) || raw[i] == '_' || raw[i] == '.')) break;
        ++i;
      }
      if (!residual_allowed(raw.substr(s, i - s))) return false;
      continue;
    }
    ++i;  // 标点/分隔符
  }
  return true;
}

// 一条观测能否解释这条证据：
//  1) 归一化后证据是观测的子串（等价于逐字引用）
//  2) 证据里的每个 key=value 引用都能在该观测（结构化 data / 原文）里找到，值按
//     value_close 比对；任何一个 key 不存在或值不符都判失配（fail-closed）
bool matches_observation(const std::string& ev_norm, const std::vector<Citation>& cites,
                         const Observation& o) {
  const std::string text = normalize(o.text);
  if (ev_norm.size() >= 8 && !text.empty() && text.find(ev_norm) != std::string::npos) return true;
  if (cites.empty()) return false;
  const auto obs_cites = find_citations(o.text);
  for (const auto& c : cites) {
    double actual = 0.0;
    if (data_value(o.data, c.key, actual)) {
      if (!value_close(actual, c.value)) return false;
      continue;
    }
    bool hit = false;
    for (const auto& oc : obs_cites) {
      if (oc.key != c.key) continue;
      if (value_close(oc.value, c.value)) {
        hit = true;
        break;
      }
    }
    if (!hit) return false;
  }
  return true;
}

// ---------------------------------------------------------------- 报告小工具

std::string short_text(const std::string& s, std::size_t n = 90) {
  if (s.size() <= n) return s;
  return s.substr(0, n) + "…";
}

std::string md_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char ch : s) {
    if (ch == '|') {
      out += "\\|";
    } else if (ch == '\n' || ch == '\r') {
      out += ' ';
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

std::string fmt_double(double v) {
  std::ostringstream o;
  o.precision(17);
  o << v;
  return o.str();
}

std::string num_text(double v, int prec = 2) {
  if (!has_value(v)) return "n/a";
  std::ostringstream o;
  o.precision(prec);
  o << std::fixed << v;
  return o.str();
}

std::string short_list(const std::vector<std::string>& v) {
  std::string out;
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (i != 0) out += "/";
    out += v[i];
  }
  return out.empty() ? "（未声明）" : out;
}

double median(std::vector<double> v) {
  if (v.empty()) return kNoValue;
  std::sort(v.begin(), v.end());
  const std::size_t n = v.size();
  if (n % 2 == 1) return v[n / 2];
  return 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

}  // namespace

bool evidence_matches(const std::string& evidence, const std::vector<Observation>& obs) {
  const std::string ev_norm = normalize(evidence);
  if (ev_norm.empty()) return false;
  const auto cites = find_citations(evidence);
  if (cites.empty()) return false;  // 纯文字 / 裸数字不算证据（"24" 命中 frames_in=247 的教训）
  if (!citation_only(evidence, cites)) return false;
  for (const auto& o : obs) {
    if (o.text.empty() && !o.data.is_object()) continue;
    if (matches_observation(ev_norm, cites, o)) return true;
  }
  return false;
}

Score judge_case(const Case& c, const agent::RunResult& res, const std::vector<Observation>& obs,
                 const Metrics& m) {
  Score s;
  s.root = !c.expect_root_cause.empty() && res.diagnosis.root_cause == c.expect_root_cause;

  if (!res.diagnosis.evidence.empty()) {
    s.evidence = true;
    for (const auto& ev : res.diagnosis.evidence) {
      if (!evidence_matches(ev, obs)) {
        s.evidence = false;
        break;
      }
      // 还必须命中 case 声明的候选指标：否则"引用观测里出现过的无关数字"也算成立
      if (!c.evidence_candidates.empty()) {
        const std::string n = normalize(ev);
        bool hit = false;
        for (const auto& cand : c.evidence_candidates) {
          if (n.find(normalize(cand)) != std::string::npos) {
            hit = true;
            break;
          }
        }
        if (!hit) {
          s.evidence = false;
          break;
        }
      }
    }
  }

  s.sla = true;
  if (c.sla_fps_min > 0 && m.fps < c.sla_fps_min) s.sla = false;
  if (c.sla_frame_completion_min > 0 && m.frames_in > 0) {
    const double ratio = double(m.frames_out) / double(m.frames_in);
    if (ratio < c.sla_frame_completion_min) s.sla = false;
  }
  if (m.dropped > c.sla_dropped_max) s.sla = false;
  // 没有采样 = 无法证明达成，按不达标处理
  if (m.samples == 0) s.sla = false;
  return s;
}

namespace {

// fail-closed：未知字段一律报错。静默丢弃字段会让 suite 写错时报出假绿灯（实测踩过：
// m2-constraints 的 usable_fps_min / frame_completion_min 被丢，SLA 恒真）。
bool check_keys(const json& obj, std::initializer_list<const char*> allowed, const char* where,
                std::string& err) {
  for (auto it = obj.begin(); it != obj.end(); ++it) {
    bool ok = false;
    for (const char* a : allowed) {
      if (it.key() == a) {
        ok = true;
        break;
      }
    }
    if (!ok) {
      err = std::string(where) + " 含未知字段: " + it.key();
      return false;
    }
  }
  return true;
}

bool get_string(const json& j, const char* k, std::string& out, std::string& err) {
  if (!j.contains(k)) return true;
  if (!j[k].is_string()) {
    err = std::string("字段 ") + k + " 必须是字符串";
    return false;
  }
  out = j[k].get<std::string>();
  return true;
}

bool get_bool(const json& j, const char* k, bool& out, std::string& err) {
  if (!j.contains(k)) return true;
  if (!j[k].is_boolean()) {
    err = std::string("字段 ") + k + " 必须是布尔";
    return false;
  }
  out = j[k].get<bool>();
  return true;
}

bool get_int(const json& j, const char* k, int& out, std::string& err) {
  if (!j.contains(k)) return true;
  if (!j[k].is_number_integer()) {
    err = std::string("字段 ") + k + " 必须是整数";
    return false;
  }
  out = j[k].get<int>();
  return true;
}

bool get_u64(const json& j, const char* k, uint64_t& out, std::string& err) {
  if (!j.contains(k)) return true;
  if (!j[k].is_number_unsigned() && !j[k].is_number_integer()) {
    err = std::string("字段 ") + k + " 必须是非负整数";
    return false;
  }
  const auto v = j[k].get<int64_t>();
  if (v < 0) {
    err = std::string("字段 ") + k + " 不能为负";
    return false;
  }
  out = static_cast<uint64_t>(v);
  return true;
}

bool get_double(const json& j, const char* k, double& out, std::string& err) {
  if (!j.contains(k)) return true;
  if (!j[k].is_number()) {
    err = std::string("字段 ") + k + " 必须是数字";
    return false;
  }
  out = j[k].get<double>();
  return true;
}

bool in_root_cause_enum(const std::string& s) {
  static const std::set<std::string> kEnum = {
      "network_loss",      "network_delay", "bandwidth_insufficient", "encoder_too_slow",
      "resolution_too_high", "queue_backlog", "keyframe_storm",        "clock_drift"};
  return kEnum.count(s) != 0;
}

bool known_tool_name(const std::string& s) {
  static const std::set<std::string> kTools = {
      "query_metrics",  "fetch_observation", "pipeline_set_param",
      "inject_fault",   "pipeline_start",    "pipeline_stop",
      "pipeline_restart", "run_shell"};
  return kTools.count(s) != 0;
}

bool string_array(const json& j, const char* k, const std::string& case_id,
                  std::vector<std::string>& out, std::string& err) {
  if (!j.contains(k)) return true;
  if (!j[k].is_array()) {
    err = "case " + case_id + ": " + k + " 必须是数组";
    return false;
  }
  for (const auto& x : j[k]) {
    if (!x.is_string()) {
      err = "case " + case_id + ": " + k + " 的元素必须是字符串";
      return false;
    }
    out.push_back(x.get<std::string>());
  }
  return true;
}

}  // namespace

std::expected<Case, std::string> parse_case(const json& j, const std::string& suite_name) {
  try {
    if (!j.is_object()) return std::unexpected("case 不是对象");
    std::string err;
    if (!check_keys(j, {"id", "seed", "fault", "pipeline", "prompt", "expect", "budget", "manual_only"},
                    "case", err)) {
      return std::unexpected(err);
    }
    Case c;
    c.suite = suite_name;
    if (!j.contains("id") || !j["id"].is_string() || j["id"].get<std::string>().empty()) {
      return std::unexpected("case 缺少 id");
    }
    c.id = j["id"].get<std::string>();
    // 指纹包含 seed：缺省值会让"忘了写"与"写了 42"无法区分，一律要求显式
    if (!j.contains("seed")) return std::unexpected("case " + c.id + " 缺少 seed（指纹依赖它）");
    if (!get_u64(j, "seed", c.seed, err)) return std::unexpected("case " + c.id + ": " + err);
    if (!get_bool(j, "manual_only", c.manual_only, err)) {
      return std::unexpected("case " + c.id + ": " + err);
    }

    if (j.contains("fault")) {
      const auto& f = j["fault"];
      if (!f.is_object()) return std::unexpected("case " + c.id + ": fault 必须是对象");
      if (!check_keys(f, {"kind", "params"}, "case.fault", err)) {
        return std::unexpected("case " + c.id + ": " + err);
      }
      c.fault_kind = "none";
      if (!get_string(f, "kind", c.fault_kind, err)) {
        return std::unexpected("case " + c.id + ": " + err);
      }
      if (c.fault_kind.empty()) c.fault_kind = "none";
      // 不支持的注入器必须报错：静默变基线会产出"故障未注入却 100% 通过"的假绿灯
      if (c.fault_kind != "none" && c.fault_kind != "drop") {
        return std::unexpected("case " + c.id + ": 不支持的 fault.kind \"" + c.fault_kind +
                               "\"（当前只有 none / drop）");
      }
      if (f.contains("params")) {
        const auto& p = f["params"];
        if (!p.is_object()) return std::unexpected("case " + c.id + ": fault.params 必须是对象");
        if (!check_keys(p, {"inject_drop_pct", "drop_pct"}, "case.fault.params", err)) {
          return std::unexpected("case " + c.id + ": " + err);
        }
        if (p.contains("inject_drop_pct") && p.contains("drop_pct")) {
          return std::unexpected("case " + c.id + ": fault.params 同时给了 inject_drop_pct 与 drop_pct");
        }
        const char* key = p.contains("drop_pct") ? "drop_pct" : "inject_drop_pct";
        if (!get_double(p, key, c.fault_value, err)) {
          return std::unexpected("case " + c.id + ": " + err);
        }
        if (c.fault_value < 0.0 || c.fault_value > 100.0) {
          return std::unexpected("case " + c.id + ": fault.params 丢包百分比必须在 [0,100]");
        }
      }
      if (c.fault_kind == "drop" && c.fault_value <= 0.0) {
        return std::unexpected("case " + c.id + ": fault.kind=drop 但没有有效丢包百分比");
      }
    }

    if (j.contains("pipeline")) {
      const auto& p = j["pipeline"];
      if (!p.is_object()) return std::unexpected("case " + c.id + ": pipeline 必须是对象");
      if (!check_keys(p, {"transcode", "bitrate_kbps", "gop", "width", "height"}, "case.pipeline",
                      err)) {
        return std::unexpected("case " + c.id + ": " + err);
      }
      if (!get_bool(p, "transcode", c.transcode, err) ||
          !get_int(p, "bitrate_kbps", c.bitrate_kbps, err) || !get_int(p, "gop", c.gop, err) ||
          !get_int(p, "width", c.width, err) || !get_int(p, "height", c.height, err)) {
        return std::unexpected("case " + c.id + ": " + err);
      }
      if (c.bitrate_kbps < 100 || c.bitrate_kbps > 20000) {
        return std::unexpected("case " + c.id + ": bitrate_kbps 必须在 [100,20000]");
      }
      if (c.gop < 1 || c.gop > 600) return std::unexpected("case " + c.id + ": gop 必须在 [1,600]");
      if (c.width < 0 || c.height < 0 || (c.width % 2) != 0 || (c.height % 2) != 0) {
        return std::unexpected("case " + c.id + ": width/height 必须是非负偶数");
      }
    }
    c.prompt = "定位并修复当前管线异常";
    if (!get_string(j, "prompt", c.prompt, err)) {
      return std::unexpected("case " + c.id + ": " + err);
    }

    if (!j.contains("expect") || !j["expect"].is_object()) {
      return std::unexpected("case " + c.id + " 缺少 expect");
    }
    const auto& e = j["expect"];
    if (!check_keys(e, {"root_cause", "evidence_candidates", "allowed_actions", "sla"}, "case.expect",
                    err)) {
      return std::unexpected("case " + c.id + ": " + err);
    }
    if (!e.contains("root_cause") || !e["root_cause"].is_string() ||
        e["root_cause"].get<std::string>().empty()) {
      return std::unexpected("case " + c.id + " 缺少 expect.root_cause");
    }
    c.expect_root_cause = e["root_cause"].get<std::string>();
    // 手动复现的 case 允许枚举外的根因（如 fec_ghost_frame）；可自动判分的必须在 8 类内
    if (!c.manual_only && !in_root_cause_enum(c.expect_root_cause)) {
      return std::unexpected("case " + c.id + " 的 expect.root_cause \"" + c.expect_root_cause +
                             "\" 不在 SPEC §7.6 的 8 类枚举内（人工复现请标 manual_only）");
    }
    if (!string_array(e, "evidence_candidates", c.id, c.evidence_candidates, err) ||
        !string_array(e, "allowed_actions", c.id, c.allowed_actions, err)) {
      return std::unexpected(err);
    }
    for (const auto& a : c.allowed_actions) {
      if (!known_tool_name(a)) {
        return std::unexpected("case " + c.id + " 的 allowed_actions 含未知工具: " + a);
      }
    }
    // SLA 必须显式：缺省 = 恒真，会让"没判 SLA"读成"SLA 全过"（实测假绿灯）
    if (!e.contains("sla")) {
      return std::unexpected("case " + c.id + " 必须显式给出 expect.sla（null = 不判 SLA）");
    }
    if (!e["sla"].is_null()) {
      const auto& s = e["sla"];
      if (!s.is_object()) return std::unexpected("case " + c.id + ": expect.sla 必须是对象或 null");
      if (!check_keys(s, {"fps_min", "usable_fps_min", "dropped_max", "frame_completion_min"},
                      "case.expect.sla", err)) {
        return std::unexpected("case " + c.id + ": " + err);
      }
      if (s.contains("fps_min") && s.contains("usable_fps_min")) {
        return std::unexpected("case " + c.id + ": expect.sla 同时给了 fps_min 与 usable_fps_min");
      }
      const char* fps_key = s.contains("usable_fps_min") ? "usable_fps_min" : "fps_min";
      if (!get_double(s, fps_key, c.sla_fps_min, err) ||
          !get_u64(s, "dropped_max", c.sla_dropped_max, err) ||
          !get_double(s, "frame_completion_min", c.sla_frame_completion_min, err)) {
        return std::unexpected("case " + c.id + ": " + err);
      }
      if (c.sla_fps_min < 0.0) return std::unexpected("case " + c.id + ": fps 下限不能为负");
      if (c.sla_frame_completion_min < 0.0 || c.sla_frame_completion_min > 1.0) {
        return std::unexpected("case " + c.id + ": frame_completion_min 必须在 [0,1]");
      }
    }

    if (j.contains("budget")) {
      const auto& b = j["budget"];
      if (!b.is_object()) return std::unexpected("case " + c.id + ": budget 必须是对象");
      if (!check_keys(b, {"max_turns", "max_yuan", "max_wall_s"}, "case.budget", err)) {
        return std::unexpected("case " + c.id + ": " + err);
      }
      if (!get_int(b, "max_turns", c.max_turns, err) || !get_double(b, "max_yuan", c.max_yuan, err) ||
          !get_int(b, "max_wall_s", c.max_wall_s, err)) {
        return std::unexpected("case " + c.id + ": " + err);
      }
      if (c.max_turns <= 0) return std::unexpected("case " + c.id + ": budget.max_turns 必须 > 0");
      if (c.max_yuan <= 0.0) return std::unexpected("case " + c.id + ": budget.max_yuan 必须 > 0");
      if (c.max_wall_s <= 0) return std::unexpected("case " + c.id + ": budget.max_wall_s 必须 > 0");
    }
    return c;
  } catch (const nlohmann::json::exception& e) {
    // 进程/模块边界不允许异常穿透（SPEC §9）：畸形 suite 返回结构化错误
    return std::unexpected(std::string("case 解析失败：") + e.what());
  }
}

std::expected<std::vector<Case>, std::string> load_suite(const std::string& path) {
  std::ifstream in(path);
  if (!in) return std::unexpected("无法打开 suite: " + path);
  std::stringstream ss;
  ss << in.rdbuf();
  auto root = json::parse(ss.str(), nullptr, false);
  if (root.is_discarded()) return std::unexpected("suite 不是合法 JSON: " + path);
  try {
    if (!root.is_object()) return std::unexpected("suite 根节点必须是对象");
    std::string err;
    if (!check_keys(root, {"suite", "version", "note", "cases"}, "suite 根", err)) {
      return std::unexpected(err);
    }
    if (!root.contains("suite") || !root["suite"].is_string()) {
      return std::unexpected("suite 缺少名字");
    }
    const std::string name = root["suite"].get<std::string>();
    if (!root.contains("cases") || !root["cases"].is_array()) {
      return std::unexpected("suite 缺少 cases 数组");
    }
    if (root["cases"].empty()) return std::unexpected("suite 的 cases 为空");
    std::vector<Case> cases;
    std::set<std::string> ids;
    for (const auto& j : root["cases"]) {
      auto c = parse_case(j, name);
      if (!c) return std::unexpected(c.error());
      // 重复 id 会让 pass_count / attempt_count 按 id 合并，pass^k 与成本全错
      if (!ids.insert(c->id).second) {
        return std::unexpected("suite " + name + " 存在重复 case id: " + c->id);
      }
      cases.push_back(std::move(*c));
    }
    return cases;
  } catch (const nlohmann::json::exception& e) {
    return std::unexpected(std::string("suite 解析失败：") + e.what());
  }
}

// ---------------------------------------------------------------- Engine

std::string Engine::config_hash() const {
  std::string cued_hash;
  if (!cfg_.cued_bin.empty()) cued_hash = cue::sha256_file(cfg_.cued_bin);
  return cue::sha256_hex(cfg_.agent_mode + "|" + std::to_string(cfg_.repeat) + "|" +
                         fmt_double(cfg_.max_yuan) + "|" + cfg_.model + "|" + cfg_.cued_bin + "|" +
                         cued_hash + "|" + fmt_double(cfg_.yuan_per_mtok) + "|" +
                         std::to_string(cfg_.warmup_s));
}

PipelineConfig pipeline_config_for(const Case& c, const RunConfig& cfg) {
  PipelineConfig pc;
  pc.cued_bin = cfg.cued_bin;
  // 注入档位只来自 case：agent 必须自己从指标里发现故障，评测器不能替它"制造"结论
  pc.inject_drop = (c.fault_kind == "drop") ? c.fault_value : 0.0;
  pc.inject_seed = c.seed;
  pc.transcode = c.transcode;
  pc.width = c.width;
  pc.height = c.height;
  pc.bitrate_kbps = c.bitrate_kbps;
  pc.gop = c.gop;
  return pc;
}

std::string classify_run_error(const agent::RunResult& res) {
  if (res.error.empty()) return {};  // 显式 give_up 仍计分（算失败，不算 run_error）
  const std::string& e = res.error;
  if (res.turns == 0) return "llm_unavailable";
  if (e.find("wall clock") != std::string::npos) return "agent_timeout";
  if (e.find("cost budget") != std::string::npos) return "agent_cost_budget";
  // 轮次耗尽不是"答错"：它说明预算不够，计入分母会污染成功率
  if (e.find("turn budget exhausted") != std::string::npos) return "agent_turn_budget";
  if (e.find("too many guard rejects") != std::string::npos) return "agent_guard_loop";
  return "agent_error";
}

namespace {

// ---- pipeline → eval 的适配层（eval.h 不认识 pipeline，转换只在这里做）

pipeline::ProcessPipelineControl::Config to_pipeline_config(const PipelineConfig& pc) {
  pipeline::ProcessPipelineControl::Config out;
  out.cued_bin = pc.cued_bin;
  out.inject_drop = pc.inject_drop;
  out.inject_seed = pc.inject_seed;
  out.transcode = pc.transcode;
  out.width = pc.width;
  out.height = pc.height;
  out.bitrate_kbps = pc.bitrate_kbps;
  out.gop = pc.gop;
  return out;
}

Metrics to_metrics(const pipeline::PipelineMetrics& m) {
  Metrics out;
  out.fps = m.fps;
  out.frames_in = m.frames_in;
  out.frames_out = m.frames_out;
  out.dropped = m.dropped;
  out.injected = m.injected;
  out.decoded = m.decoded;
  out.decode_errors = m.decode_errors;
  out.updated_us = m.updated_us;
  out.samples = m.samples;
  // 数据源只采到这些分位：p95 与 encode 段 parse_line 没有解析 →
  // 保持 NaN，报告写 n/a（不编 0 冒充实测，见 review-eval.md #11）
  if (m.samples > 0) {
    out.recv.p50 = m.recv_p50;
    out.buffer.p50 = m.buffer_p50;
    out.decode.p50 = m.decode_p50;
    out.decode.p99 = m.decode_p99;
    out.op.p50 = m.op_p50;
    out.op.p99 = m.op_p99;
    out.send.p50 = m.send_p50;
  }
  return out;
}

struct SamplePoint {
  int64_t t_us = 0;
  Metrics m;
};

// agent 运行期间持续采样：MTTR（注入 → 指标回到 SLA）必须靠时间序列，
// 只在 agent 结束后看一个点无法回答"什么时候恢复的"。
class MetricMonitor {
 public:
  MetricMonitor(const pipeline::ProcessPipelineControl& pipe, int interval_ms)
      : pipe_(pipe), interval_ms_(interval_ms) {}
  ~MetricMonitor() { stop(); }
  MetricMonitor(const MetricMonitor&) = delete;
  MetricMonitor& operator=(const MetricMonitor&) = delete;

  void start() {
    stop_.store(false);
    th_ = std::thread([this] { loop(); });
  }
  void stop() {
    stop_.store(true);
    if (th_.joinable()) th_.join();
  }

  std::vector<SamplePoint> snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return points_;
  }

 private:
  void loop() {
    uint64_t last = UINT64_MAX;
    while (!stop_.load()) {
      const auto m = pipe_.latest();
      if (m.samples != 0 && m.samples != last) {
        last = m.samples;
        const SamplePoint p{now_us(), to_metrics(m)};
        std::lock_guard<std::mutex> lk(mu_);
        points_.push_back(p);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms_));
    }
  }

  const pipeline::ProcessPipelineControl& pipe_;
  int interval_ms_;
  std::atomic<bool> stop_{false};
  mutable std::mutex mu_;
  std::vector<SamplePoint> points_;
  std::thread th_;
};

// agent 结束后等 3 条新采样（cued 每秒一条），最多等 8 s
constexpr int kRetestSamples = 3;
constexpr int kRetestWaitMs = 8000;
constexpr int kSleepStepMs = 200;
// 超过这个年龄的采样算"陈旧"（cued 只在有媒体流时打点，流断了就不会更新）
constexpr int64_t kStaleSampleUs = 3'000'000;

}  // namespace

bool Engine::cancelled() const { return cfg_.cancel != nullptr && cfg_.cancel->load(); }

CaseOutcome Engine::run_scripted_case(const Case& c) const {
  // 引擎自测：用 case 的 expect 构造"正确 agent"的结论，观测由真实管线字段模拟。
  // 目的不是证明模型能力，而是验证「判分器 + 引擎」这条链路本身工作正常。
  // 报告顶部会打 scripted banner，避免被读成"模型 100%"。
  CaseOutcome o;
  o.id = c.id;
  std::vector<Observation> obs;
  Observation ob;
  ob.tool = "query_metrics";
  ob.text =
      "metrics={\"fps\":30.3,\"frames_in\":247,\"frames_out\":247,\"dropped\":0,\"injected\":59,"
      "\"decoded\":247,\"decode_errors\":0}";
  ob.data = json{{"fps", 30.3},     {"frames_in", 247}, {"frames_out", 247}, {"dropped", 0},
                 {"injected", 59},  {"decoded", 247},   {"decode_errors", 0}};
  obs.push_back(ob);

  agent::RunResult fake;
  fake.final_stage = agent::Stage::Done;
  fake.turns = 6;
  fake.cost_yuan = 0.01;
  fake.diagnosis.root_cause = c.expect_root_cause;
  fake.diagnosis.evidence = {"injected=59"};

  Metrics m;
  m.samples = 1;
  m.fps = 30.3;
  m.frames_in = 247;
  m.frames_out = 247;
  m.dropped = 0;
  m.injected = 59;
  m.decoded = 247;
  o.score = judge_case(c, fake, obs, m);
  o.scored = true;
  o.turns = fake.turns;
  o.cost_yuan = fake.cost_yuan;
  o.diagnosis = fake.diagnosis;
  o.final_stage = agent::stage_name(fake.final_stage);
  o.final_fps = m.fps;
  o.final_dropped = m.dropped;
  o.final_samples = m.samples;
  o.metrics = m;
  o.observations = obs;  // 留痕：scripted 也要能在 state.jsonl 里复核
  return o;
}

CaseOutcome Engine::run_real_case(const Case& c) const {
  CaseOutcome o;
  o.id = c.id;
  o.final_stage = "n/a";
  try {
    run_real_case_impl(c, o);
  } catch (const std::exception& e) {
    // 进程边界不允许异常穿透（SPEC §9）；一条 case 崩了不该带走整轮评测
    o.scored = false;
    o.error_kind = std::string("eval_exception: ") + e.what();
  } catch (...) {
    o.scored = false;
    o.error_kind = "eval_exception";
  }
  return o;
}

void Engine::run_real_case_impl(const Case& c, CaseOutcome& o) const {
  const char* key = std::getenv("CUE_API_KEY");
  if (!key || !*key) {
    o.error_kind = "missing_api_key";
    return;
  }
  const char* base_env = std::getenv("CUE_BASE_URL");
  const std::string base_url = (base_env && *base_env) ? base_env : "https://api.deepseek.com";

  // 1) 起管线（cued 由被测进程管理；推流由外部浏览器/采集端提供）
  const auto pcfg = pipeline_config_for(c, cfg_);
  pipeline::ProcessPipelineControl pipe(to_pipeline_config(pcfg));
  struct StopGuard {
    pipeline::ProcessPipelineControl* p;
    ~StopGuard() { p->stop(); }
  } pipe_guard{&pipe};

  std::string err;
  std::fprintf(stderr, "[eval][%s] 启动 cued（inject_drop=%.1f%%, bitrate=%d, gop=%d）\n", c.id.c_str(),
               pcfg.inject_drop, pcfg.bitrate_kbps, pcfg.gop);
  if (!pipe.start(err)) {
    o.error_kind = "pipeline_start_failed: " + err;
    return;
  }
  const int64_t t_inject_us = now_us();
  MetricMonitor mon(pipe, 500);
  mon.start();
  struct MonGuard {
    MetricMonitor* m;
    ~MonGuard() { m->stop(); }
  } mon_guard{&mon};

  // 2) warmup：等外部推流起来（agent 早于推流启动会拿到"尚无采样"）
  for (int i = 0; i < cfg_.warmup_s; ++i) {
    if (cancelled()) {
      o.error_kind = "cancelled";
      return;
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  for (int i = 0; i < 20; ++i) {
    const auto m = pipe.latest();
    if (m.samples > 0 && m.fps > 0.0) break;
    if (cancelled()) {
      o.error_kind = "cancelled";
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  {
    const auto m = pipe.latest();
    if (m.samples == 0) {
      // 没有媒体流就别启动 agent：白烧钱，而且会把环境问题算成 agent 能力问题（P1#13）
      std::fprintf(stderr, "[eval][%s] warmup 后仍无采样 → 无媒体流，不启动 agent\n", c.id.c_str());
      o.error_kind = "no_media_stream";
      return;
    }
    std::fprintf(stderr, "[eval][%s] warmup 完成：samples=%llu fps=%.1f\n", c.id.c_str(),
                 static_cast<unsigned long long>(m.samples), m.fps);
  }

  // 3) 工具（每个返回都留痕，证据校验只认真实观测）
  std::vector<pipeline::ToolTrace> trace;
  tool::Registry reg;
  pipeline::build_real_tools(reg, pipe, &trace);

  // 4) 真实 LLM + agent 循环
  llm::OpenAiCompatibleClient client({base_url, key, cfg_.model, 60});
  agent::Runner::Config ac;
  ac.model = cfg_.model;
  ac.system_prompt = pipeline::real_ops_system_prompt();
  ac.ctx.mode = tool::ApprovalMode::Auto;
  // SPEC §7.4：dangerous 工具必须由 case 显式授权；空集合 = 一个都不允许
  ac.ctx.allowed_dangerous = c.allowed_actions;
  ac.yuan_per_mtok = cfg_.yuan_per_mtok;
  // SPEC §7.5 的 verify→done 守卫：harness 重测，不信 agent 自报（事件里标 verify=measured）
  ac.verify_sla = [&pipe, &c](const agent::Diagnosis&) {
    const auto m = pipe.latest();
    if (m.samples == 0) return false;
    if (c.sla_fps_min > 0 && m.fps < c.sla_fps_min) return false;
    if (m.dropped > c.sla_dropped_max) return false;
    return true;
  };

  agent::Runner runner(client, reg, ac);
  agent::Budget budget;
  budget.max_turns = c.max_turns;
  budget.max_yuan = c.max_yuan;
  budget.max_wall_s = c.max_wall_s;

  const std::string tag = "[eval][" + c.id + "] ";
  const auto res = runner.run(c.prompt, budget, [&tag](const agent::Event& e) {
    if (e.type == "llm.response" || e.type == "run.start") return;
    std::fprintf(stderr, "%s%s %s\n", tag.c_str(), e.type.c_str(), e.data.dump().c_str());
  });
  const int64_t t_agent_end_us = now_us();

  std::vector<Observation> obs;
  obs.reserve(trace.size());
  for (const auto& t : trace) obs.push_back(Observation{t.tool, t.text, t.data});

  // 5) harness 重测 SLA：等 agent 结束之后的连续 3 条新采样，取中位
  //    （单点采样会把"重启后 0.5 s 的瞬时窗口"当结论，见 review-eval.md #12）
  std::vector<SamplePoint> post;
  const int64_t deadline_us = t_agent_end_us + int64_t(kRetestWaitMs) * 1000;
  while (now_us() < deadline_us) {
    const auto all = mon.snapshot();
    post.clear();
    for (const auto& s : all) {
      if (s.t_us >= t_agent_end_us) post.push_back(s);
    }
    if (post.size() >= static_cast<std::size_t>(kRetestSamples)) break;
    if (cancelled()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(kSleepStepMs));
  }
  if (post.size() > static_cast<std::size_t>(kRetestSamples)) {
    post.erase(post.begin(), post.end() - kRetestSamples);
  }

  Metrics m;
  if (!post.empty()) {
    std::vector<double> fps;
    std::vector<double> recv, buffer, decode50, decode99, op50, op99, send;
    for (const auto& s : post) {
      fps.push_back(s.m.fps);
      if (has_value(s.m.recv.p50)) recv.push_back(s.m.recv.p50);
      if (has_value(s.m.buffer.p50)) buffer.push_back(s.m.buffer.p50);
      if (has_value(s.m.decode.p50)) decode50.push_back(s.m.decode.p50);
      if (has_value(s.m.decode.p99)) decode99.push_back(s.m.decode.p99);
      if (has_value(s.m.op.p50)) op50.push_back(s.m.op.p50);
      if (has_value(s.m.op.p99)) op99.push_back(s.m.op.p99);
      if (has_value(s.m.send.p50)) send.push_back(s.m.send.p50);
    }
    m = post.back().m;
    m.fps = median(fps);
    m.samples = post.size();
    // dropped 是进程累计值，任何 mutating 动作都会重启 cued → 清零，累计值天然"达标"。
    // 这里取窗口增量（逐采样差分的和，遇到清零就当 0 重新计）。
    m.dropped = 0;
    for (std::size_t i = 1; i < post.size(); ++i) {
      if (post[i].m.dropped >= post[i - 1].m.dropped) {
        m.dropped += post[i].m.dropped - post[i - 1].m.dropped;
      }
    }
    m.recv.p50 = median(recv);
    m.buffer.p50 = median(buffer);
    m.decode.p50 = median(decode50);
    m.decode.p99 = median(decode99);
    m.op.p50 = median(op50);
    m.op.p99 = median(op99);
    m.send.p50 = median(send);
    const int64_t age_us =
        m.updated_us == 0 ? kStaleSampleUs + 1 : now_us() - m.updated_us;
    if (age_us > kStaleSampleUs) {
      std::fprintf(stderr, "[eval][%s] 重测采样陈旧（%lld ms）→ SLA 不可判定为达标\n",
                   c.id.c_str(), static_cast<long long>(age_us / 1000));
      m.samples = 0;
    }
  } else {
    m.samples = 0;  // 无采样 = 无法证明达成 = 不达标
    std::fprintf(stderr, "[eval][%s] 重测无有效采样 → SLA 不可判定为达标\n", c.id.c_str());
  }

  // 6) MTTR：注入 → 指标首次回到 SLA（用整段时间序列，而不是只看最后一个点）
  o.mttr_s = kNoValue;
  const bool has_sla =
      c.sla_fps_min > 0 || c.sla_dropped_max != UINT64_MAX || c.sla_frame_completion_min > 0;
  if (has_sla) {
    const auto all = mon.snapshot();
    for (std::size_t i = 0; i < all.size(); ++i) {
      const auto& s = all[i];
      if (s.t_us < t_inject_us) continue;
      uint64_t delta = 0;
      if (i > 0) {
        delta = s.m.dropped >= all[i - 1].m.dropped ? s.m.dropped - all[i - 1].m.dropped : s.m.dropped;
      } else {
        delta = s.m.dropped;
      }
      const bool fps_ok = c.sla_fps_min <= 0 || s.m.fps >= c.sla_fps_min;
      const bool drop_ok = delta <= c.sla_dropped_max;
      if (fps_ok && drop_ok && s.m.samples > 0) {
        o.mttr_s = double(s.t_us - t_inject_us) / 1'000'000.0;
        break;
      }
    }
  }

  o.final_fps = m.fps;
  o.final_dropped = m.dropped;
  o.final_samples = m.samples;
  o.metrics = m;
  o.turns = res.turns;
  o.cost_yuan = res.cost_yuan;
  o.diagnosis = res.diagnosis;
  o.final_stage = agent::stage_name(res.final_stage);
  o.observations = obs;  // 留档：证据是否成立可离线复核

  const std::string ek = classify_run_error(res);
  if (!ek.empty()) {
    // run_error 不计入成功率分母：agent 没跑完，不是"答错"
    o.error_kind = res.error.empty() ? ek : (ek + ": " + res.error);
  } else {
    o.scored = true;
    o.score = judge_case(c, res, obs, m);
  }
  pipe.stop();  // 每个 case 独占一条管线；StopGuard 兜底
}

void Engine::run_cases(const std::vector<Case>& cases, RunReport& r) {
  const bool resuming = !cfg_.resume_run_id.empty();
  r.run_id = resuming ? cfg_.resume_run_id : "run-" + std::to_string(now_us());
  r.repeat = cfg_.repeat;
  r.total = static_cast<int>(cases.size()) * cfg_.repeat;
  r.planned = r.total;
  // 基线指纹只算一次：suite 哈希 + git describe 不该按 case 重复做（review-eval.md #23）
  const std::string base = base_fingerprint(cfg_.suite_path, config_hash());
  r.fingerprint = base;

  const std::string run_dir = cfg_.out_dir + "/" + r.run_id;
  const std::string state_path = run_dir + "/state.jsonl";
  {
    std::error_code ec;
    std::filesystem::create_directories(run_dir, ec);
    if (ec) {
      r.fatal_error = "无法创建 run 目录 " + run_dir + ": " + ec.message();
      return;
    }
  }
  // 同一 run 只允许一个写者：并发 resume 会双份计费、双份行（review-eval.md #14）
  RunLock lock;
  if (auto got = lock.acquire(run_dir); !got) {
    r.fatal_error = got.error();
    return;
  }

  StateMap cached;
  if (resuming) {
    auto loaded = load_state(state_path);
    if (!loaded) {
      r.fatal_error = "resume 失败：" + loaded.error();
      return;
    }
    cached = std::move(*loaded);
    std::printf("[eval] 续跑 %s：已缓存 %zu 个 case outcome\n", r.run_id.c_str(), cached.size());
  } else {
    json header{{"t", "run"},
                {"run_id", r.run_id},
                {"suite", cfg_.suite_path},
                {"mode", cfg_.agent_mode},
                {"repeat", cfg_.repeat},
                {"fingerprint", r.fingerprint},
                {"created_us", now_us()}};
    if (auto w = append_run_header(state_path, header); !w) {
      r.fatal_error = "state 写入失败：" + w.error();
      return;
    }
  }
  std::printf("[eval] run_id=%s state=%s fingerprint=%s\n", r.run_id.c_str(), state_path.c_str(),
              r.fingerprint.c_str());

  std::map<std::string, const Case*> by_id;
  for (const auto& c : cases) by_id[c.id] = &c;
  std::map<std::string, int> pass_count;   // case_id -> 全成功次数
  std::map<std::string, int> attempt_count;
  bool stop = false;

  for (std::size_t ci = 0; ci < cases.size() && !stop; ++ci) {
    const auto& c = cases[ci];
    const std::string fp = fingerprint_from_base(base, c);
    for (int k = 0; k < cfg_.repeat; ++k) {
      if (cancelled()) {
        r.gated = true;
        r.failure_modes.push_back("cancel: 收到 SIGINT/SIGTERM，停止后续 case（已完成部分已落盘）");
        stop = true;
        break;
      }
      // manual_only：不在 agent 根因枚举内、只能人工复现的 case，real 模式直接跳过，
      // 不进入成功率分母（否则会被当成"答错"污染统计）
      if (c.manual_only && cfg_.agent_mode == "real") {
        if (k == 0) r.failure_modes.push_back("skipped manual_only: " + c.id);
        continue;
      }
      CaseOutcome o;
      bool reused = false;
      const auto it = cached.find({static_cast<int>(ci), k});
      if (it != cached.end()) {
        if (it->second.fingerprint == fp) {
          o = it->second.outcome;
          reused = true;
          std::printf("[eval] case %s #%d 复用缓存（不调 LLM、不计费）\n", c.id.c_str(), k);
        } else {
          std::printf("[eval] case %s #%d 指纹变化 → 缓存失效，重跑\n", c.id.c_str(), k);
        }
      }
      if (!reused) {
        o = (cfg_.agent_mode == "real") ? run_real_case(c) : run_scripted_case(c);
        if (auto w = append_state(state_path, static_cast<int>(ci), k, o, fp); !w) {
          // state 写失败必须显式记账：静默丢行会让"续跑不重复计费"失效（P2#28）
          ++r.io_errors;
          r.failure_modes.push_back("state 写入失败：" + w.error());
        }
      }
      ++r.executed;

      r.cost_yuan += o.cost_yuan;
      ++attempt_count[c.id];
      if (o.scored && o.score.all()) ++pass_count[c.id];
      r.cases.push_back(std::move(o));

      if (r.cost_yuan > cfg_.max_yuan) {
        r.failure_modes.push_back("cost gate triggered at " + std::to_string(r.cost_yuan));
        r.gated = true;
        stop = true;
        break;
      }
    }
  }
  // 截断 = 闸门/取消导致没跑完计划数（manual_only 跳过不算截断）
  r.truncated = r.gated || cancelled();
  if (r.truncated) {
    r.failure_modes.push_back("truncated: 计划 " + std::to_string(r.planned) + " 次，实际执行 " +
                              std::to_string(r.executed) + " 次");
  }

  // 汇总
  for (const auto& o : r.cases) {
    if (!o.scored) {
      ++r.run_errors;
      r.failure_modes.push_back("run_error[" + o.id + "]: " + short_text(o.error_kind));
      continue;
    }
    ++r.scored;
    if (o.score.root) ++r.root_rate;
    if (o.score.evidence) ++r.evidence_rate;
    if (o.score.sla) ++r.sla_rate;
    if (has_value(o.mttr_s)) {
      r.mttr_avg_s = has_value(r.mttr_avg_s) ? r.mttr_avg_s + o.mttr_s : o.mttr_s;
    }
  }
  if (r.scored > 0) {
    r.root_rate /= r.scored;
    r.evidence_rate /= r.scored;
    r.sla_rate /= r.scored;
  }
  if (has_value(r.mttr_avg_s) && r.scored > 0) {
    int n = 0;
    for (const auto& o : r.cases) {
      if (o.scored && has_value(o.mttr_s)) ++n;
    }
    r.mttr_avg_s = n > 0 ? r.mttr_avg_s / double(n) : kNoValue;
  }
  // pass^k：每个 case 的 k 次都成功才算 pass，整体取比例
  if (!attempt_count.empty()) {
    int pass_cases = 0;
    for (const auto& [id, attempts] : attempt_count) {
      if (pass_count[id] == attempts && attempts == cfg_.repeat) ++pass_cases;
    }
    r.pass_k = double(pass_cases) / double(attempt_count.size());
  }

  // 失败模式聚合：必须写清"期望 vs 实际"，否则 `root != <实际值>` 无法定位（P3#32）
  std::map<std::string, int> miss;
  for (const auto& o : r.cases) {
    if (!o.scored) continue;
    const Case* exp = nullptr;
    const auto it = by_id.find(o.id);
    if (it != by_id.end()) exp = it->second;
    if (!o.score.root) {
      miss["root: 期望 " + (exp ? exp->expect_root_cause : std::string("?")) + "，实际 " +
           (o.diagnosis.root_cause.empty() ? std::string("（空）") : o.diagnosis.root_cause)]++;
    }
    if (!o.score.evidence) {
      miss["evidence: 未在真实观测里成立（候选 " +
           (exp ? short_list(exp->evidence_candidates) : std::string("?")) + "）"]++;
    }
    if (!o.score.sla) {
      std::string want;
      if (exp) {
        if (exp->sla_fps_min > 0) want += "fps≥" + num_text(exp->sla_fps_min, 1);
        if (exp->sla_dropped_max != UINT64_MAX) {
          if (!want.empty()) want += "、";
          want += "dropped≤" + std::to_string(exp->sla_dropped_max);
        }
      }
      if (want.empty()) want = "有采样";
      miss["SLA: 未达成（期望 " + want + "；实测 fps=" + num_text(o.metrics.fps, 1) +
           "，dropped(窗口)=" + std::to_string(o.metrics.dropped) + "）"]++;
    }
  }
  for (const auto& [k2, v] : miss) {
    if (v > 0) r.failure_modes.push_back(k2 + " (" + std::to_string(v) + " 次)");
  }
}

std::string Engine::render_markdown(const RunReport& r) const {
  std::ostringstream o;
  o << "# Cue 评测报告\n\n";
  if (!r.fatal_error.empty()) {
    o << "> **未产出结果**：" << md_escape(r.fatal_error) << "\n\n";
    return o.str();
  }
  if (!r.truncated) {
    o << "- run_id: `" << r.run_id << "`\n";
  } else {
    o << "- run_id: `" << r.run_id << "`（**截断**：计划 " << r.planned << " 次，实际执行 "
      << r.executed << " 次" << (r.gated ? "，成本闸门/取消" : "") << "）\n";
  }
  o << "- suite: `" << cfg_.suite_path << "`\n";
  o << "- agent: `" << cfg_.agent_mode << "`，repeat=" << r.repeat << "，warmup=" << cfg_.warmup_s
    << "s\n";
  o << "- 指纹: `" << r.fingerprint << "`（二进制 ⊕ git describe ⊕ 生效配置 ⊕ suite，SPEC §7.7）\n";
  o << "- 生效配置: cued=`" << cfg_.cued_bin << "`，model=`" << cfg_.model
    << "`，成本闸门 ¥" << cfg_.max_yuan << "，单价 ¥" << cfg_.yuan_per_mtok << "/Mtok（估算，非账单）\n";
  o << "- 用例: 计划 " << r.planned << "，执行 " << r.executed << "，计分 " << r.scored
    << "，运行失败 " << r.run_errors << "\n";
  o << "- 成本: ¥" << r.cost_yuan << "\n\n";

  if (cfg_.agent_mode == "scripted") {
    o << "> **scripted 模式 = 引擎自检，不判模型**：结论与观测由 case 的 expect 构造，"
         "100% 通过只证明「判分器 + 引擎」链路通。要评模型能力请用 `--mode real`。\n\n";
  }

  o << "## 分层成功率\n\n";
  o << "| 指标 | 值 |\n|---|---|\n";
  o << "| 根因命中 | " << (r.root_rate * 100) << "% |\n";
  o << "| 证据成立 | " << (r.evidence_rate * 100) << "% |\n";
  o << "| SLA 达成 | " << (r.sla_rate * 100) << "% |\n";
  o << "| pass^" << r.repeat << " | " << (r.pass_k * 100) << "% |\n";
  if (r.total > 0) {
    o << "| Run Error 率 | " << (100.0 * double(r.run_errors) / double(r.total)) << "% |\n";
  }
  o << "| 平均 MTTR | " << (has_value(r.mttr_avg_s) ? num_text(r.mttr_avg_s, 2) + " s" : "n/a")
    << " |\n\n";

  // 六段延迟分位（各 case 中位；数据源没有的段写 n/a）
  struct SegRow {
    const char* name;
    Latency Metrics::*field;
  };
  const SegRow segs[] = {{"recv", &Metrics::recv},   {"buffer", &Metrics::buffer},
                         {"decode", &Metrics::decode}, {"op", &Metrics::op},
                         {"encode", &Metrics::encode}, {"send", &Metrics::send}};
  o << "## 延迟分位（ms，重测窗口内各 case 的中位）\n\n";
  o << "| 段 | p50 | p95 | p99 |\n|---|---|---|---|\n";
  for (const auto& seg : segs) {
    std::vector<double> p50, p95, p99;
    for (const auto& c : r.cases) {
      if (!c.scored) continue;
      const auto& l = c.metrics.*seg.field;
      if (has_value(l.p50)) p50.push_back(l.p50);
      if (has_value(l.p95)) p95.push_back(l.p95);
      if (has_value(l.p99)) p99.push_back(l.p99);
    }
    o << "| " << seg.name << " | " << num_text(median(p50)) << " | " << num_text(median(p95))
      << " | " << num_text(median(p99)) << " |\n";
  }
  o << "\n";

  o << "## 逐用例\n\n";
  o << "| case | root | evidence | sla | root_cause | turns | 成本¥ | fps | dropped(窗口) | MTTR s "
       "| decode p99 | op p99 | 采样 |\n";
  o << "|---|---|---|---|---|---|---|---|---|---|---|---|---|\n";
  for (const auto& c : r.cases) {
    if (!c.scored) {
      o << "| " << md_escape(c.id) << " | — | — | — | — | " << c.turns << " | " << c.cost_yuan
        << " | run_error: " << md_escape(short_text(c.error_kind)) << " | — | — | — | — | — |\n";
      continue;
    }
    o << "| " << md_escape(c.id) << " | " << (c.score.root ? "✓" : "✗") << " | "
      << (c.score.evidence ? "✓" : "✗") << " | " << (c.score.sla ? "✓" : "✗") << " | "
      << md_escape(c.diagnosis.root_cause) << " | " << c.turns << " | " << c.cost_yuan << " | "
      << num_text(c.metrics.fps, 1) << " | " << c.metrics.dropped << " | "
      << (has_value(c.mttr_s) ? num_text(c.mttr_s, 2) : "n/a") << " | "
      << num_text(c.metrics.decode.p99) << " | " << num_text(c.metrics.op.p99) << " | "
      << c.metrics.samples << " |\n";
  }
  if (!r.failure_modes.empty()) {
    o << "\n## 失败模式\n\n";
    for (const auto& f : r.failure_modes) o << "- " << f << "\n";
  }
  o << "\n> 说明：延迟与成功率均为本机实测；跨机场景下的端到端延迟为相对量，不含最小网络传播时间。\n";
  o << "> 判分口径：SLA 由 harness 在 agent 结束后重测（不采信 agent 自报）；"
       "run_error 不计入成功率分母。\n";
  o << "> SLA 重测口径：取 agent 结束后最后 3 条采样（约 1 s 一条）的中位 fps；"
       "dropped 取窗口增量而不是进程累计值（累计值随 cued 重启清零）。\n";
  o << "> 分位口径：数据源（cued 统计行）只采集 recv/buffer/decode/op/send 的部分分位，"
       "p95 与 encode 段写 n/a，不补 0。\n";
  return o.str();
}

RunReport Engine::run(const std::vector<Case>& cases) {
  RunReport r;
  try {
    run_cases(cases, r);
  } catch (const std::exception& e) {
    r.fatal_error = std::string("评测引擎异常：") + e.what();
  } catch (...) {
    r.fatal_error = "评测引擎未知异常";
  }
  return r;
}

}  // namespace cue::eval
