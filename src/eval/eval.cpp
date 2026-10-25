#include "cue/eval/eval.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cue/common/clock.h"
#include "cue/common/sha256.h"
#include "cue/eval/state.h"
#include "cue/llm/cassette.h"
#include "cue/llm/client.h"
#include "cue/common/recorded_pipeline.h"
#include "cue/common/service_pipeline.h"
#include "cue/tool/tools.h"

namespace cue::eval {

bool has_value(double v) { return !std::isnan(v); }

// 「注入是否真的生效」（N-23）：判据只取三种有明确单一计数的故障类，别的类不做自动
// 判定（见 eval.h 里 InjectionCheck 的注释）。detail 是可以直接贴进报告的一句话，
// 带上读数与原因，读者不必回头翻 metrics。
InjectionCheck judge_injection(const std::string& fault_kind, uint64_t budget_drops,
                               uint64_t injected) {
  InjectionCheck out;
  out.fault_kind = fault_kind;
  out.budget_drops = budget_drops;
  out.injected = injected;
  if (fault_kind == "rate") {
    out.checked = true;
    out.effective = budget_drops > 0;
    out.detail = out.effective
                     ? "rate 注入生效：budget_drops=" + std::to_string(budget_drops)
                     : "rate 注入未生效：budget_drops=0（源码率低于注入上限，令牌桶没被抽干）";
    return out;
  }
  if (fault_kind == "drop" || fault_kind == "ingress_drop") {
    out.checked = true;
    out.effective = injected > 0;
    out.detail = out.effective ? fault_kind + " 注入生效：injected=" + std::to_string(injected)
                               : fault_kind + " 注入未生效：injected=0（没有包被注入吃掉）";
    return out;
  }
  // backlog / delay / jitter / none：没有单一计数能判"注入了没有"，明确不判，
  // 报告里写「未做自动判定」——硬凑一个判据只会制造新的假结论。
  out.checked = false;
  out.effective = false;
  if (!fault_kind.empty() && fault_kind != "none") {
    out.detail = fault_kind + " 未做自动判定（该故障类没有单一计数判据）";
  }
  return out;
}

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
      "pct",           "kbps",              "mbps",                 "fps",
      // observation(frames) 在 M5 前置里新增的媒体元信息，agent 引用它们不算编造
      "codec",         "sps",               "pps",                  "annexb",
      "replayed",      "keyframe"};
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

// 端到端列的文本口径：没样本写字面量 null（不是 n/a、更不是 0）。
// 报告读者需要一眼区分"没测到端到端"与"端到端是 0 ms"，这两者的结论完全相反。
std::string e2e_text(double v, int prec = 1) {
  if (!has_value(v)) return "null";
  std::ostringstream o;
  o.precision(prec);
  o << std::fixed << v;
  return o.str();
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

  // 端到端阈值：判据只有 probe 的样本。探针不在（samples=0）时一律不达标——
  // 「cued 的 fps 正常」不能证明端到端时延达标，用服务端自报顶替就是假绿灯。
  if (has_value(c.sla_lat_p99_ms)) {
    if (!has_value(m.probe.e2e.p99) || m.probe.e2e.p99 > c.sla_lat_p99_ms) s.sla = false;
  }
  if (has_value(c.sla_lat_p50_ms)) {
    if (!has_value(m.probe.e2e.p50) || m.probe.e2e.p50 > c.sla_lat_p50_ms) s.sla = false;
  }
  if (has_value(c.sla_max_stall_ms)) {
    if (!has_value(m.probe.max_stall_ms) || m.probe.max_stall_ms > c.sla_max_stall_ms) {
      s.sla = false;
    }
  }
  if (c.sla_stalls_max != UINT64_MAX) {
    // 卡顿是累计量的窗口增量；没有探针样本就读不到增量，按不达标处理
    if (m.probe.samples == 0 || m.probe.stalls > c.sla_stalls_max) s.sla = false;
  }
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

bool x264_preset(const std::string& s) {
  static const std::set<std::string> kPresets = {"ultrafast", "superfast", "veryfast", "faster",
                                                 "fast",      "medium",    "slow",     "slower",
                                                 "veryslow",  "placebo"};
  return kPresets.count(s) != 0;
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
    if (!check_keys(j,
                    {"id", "seed", "fault", "pipeline", "prompt", "expect", "budget", "manual_only",
                     "repair"},
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
      static const std::set<std::string> kKinds = {"none",    "drop",   "ingress_drop", "rate",
                                                   "backlog", "delay",  "jitter"};
      if (kKinds.count(c.fault_kind) == 0) {
        return std::unexpected("case " + c.id + ": 不支持的 fault.kind \"" + c.fault_kind +
                               "\"（可用：none / drop / ingress_drop / rate / backlog / "
                               "delay / jitter）");
      }
      if (f.contains("params")) {
        const auto& p = f["params"];
        if (!p.is_object()) return std::unexpected("case " + c.id + ": fault.params 必须是对象");
        if (!check_keys(p, {"inject_drop_pct", "drop_pct", "inject_ingress_pct", "rate_mbps",
                            "queue_fps", "delay_ms", "jitter_ms"},
                        "case.fault.params", err)) {
          return std::unexpected("case " + c.id + ": " + err);
        }
        if (p.contains("inject_drop_pct") && p.contains("drop_pct")) {
          return std::unexpected("case " + c.id + ": fault.params 同时给了 inject_drop_pct 与 drop_pct");
        }
        // 每种 kind 只认自己的参数：写错键等于"故障没注入却绿灯"，必须 fail-closed
        const char* key = nullptr;
        if (c.fault_kind == "drop") {
          key = p.contains("drop_pct") ? "drop_pct" : "inject_drop_pct";
        } else if (c.fault_kind == "ingress_drop") {
          key = "inject_ingress_pct";
        } else if (c.fault_kind == "rate") {
          key = "rate_mbps";
        } else if (c.fault_kind == "backlog") {
          key = "queue_fps";
        } else if (c.fault_kind == "delay") {
          // delay/jitter 的单位是**毫秒**，与上面三种（% / Mbps / 帧每秒）不同，
          // 所以键名也分开：一个 "value" 会让人分不清 100 是 100 ms 还是 100%。
          key = "delay_ms";
        } else if (c.fault_kind == "jitter") {
          key = "jitter_ms";
        }
        if (key != nullptr) {
          if (!p.contains(key)) {
            return std::unexpected("case " + c.id + ": fault.kind=" + c.fault_kind +
                                   " 需要 fault.params." + key);
          }
          if (!get_double(p, key, c.fault_value, err)) {
            return std::unexpected("case " + c.id + ": " + err);
          }
        }
        // 单位不同 → 上界不同：百分比类 100、速率类 100 Mbps、延迟类 2000 ms
        const bool delay_kind = c.fault_kind == "delay" || c.fault_kind == "jitter";
        const double upper = delay_kind ? 2000.0 : 100.0;
        if (c.fault_value < 0.0 || c.fault_value > upper) {
          return std::unexpected("case " + c.id + ": fault.params 注入值必须在 [0," +
                                 std::to_string(static_cast<int>(upper)) + "]" +
                                 (delay_kind ? "（毫秒）" : ""));
        }
      }
      if (c.fault_kind != "none" && c.fault_value <= 0.0) {
        return std::unexpected("case " + c.id + ": fault.kind=" + c.fault_kind + " 但没有有效注入值");
      }
    }

    if (j.contains("pipeline")) {
      const auto& p = j["pipeline"];
      if (!p.is_object()) return std::unexpected("case " + c.id + ": pipeline 必须是对象");
      if (!check_keys(p, {"transcode", "bitrate_kbps", "gop", "width", "height", "preset"},
                      "case.pipeline", err)) {
        return std::unexpected("case " + c.id + ": " + err);
      }
      if (!get_bool(p, "transcode", c.transcode, err) ||
          !get_int(p, "bitrate_kbps", c.bitrate_kbps, err) || !get_int(p, "gop", c.gop, err) ||
          !get_int(p, "width", c.width, err) || !get_int(p, "height", c.height, err) ||
          !get_string(p, "preset", c.preset, err)) {
        return std::unexpected("case " + c.id + ": " + err);
      }
      if (c.bitrate_kbps < 100 || c.bitrate_kbps > 20000) {
        return std::unexpected("case " + c.id + ": bitrate_kbps 必须在 [100,20000]");
      }
      if (c.gop < 1 || c.gop > 600) return std::unexpected("case " + c.id + ": gop 必须在 [1,600]");
      if (c.width < 0 || c.height < 0 || (c.width % 2) != 0 || (c.height % 2) != 0) {
        return std::unexpected("case " + c.id + ": width/height 必须是非负偶数");
      }
      if (!x264_preset(c.preset)) {
        return std::unexpected("case " + c.id + ": 未知 preset \"" + c.preset +
                               "\"（x264 只有 ultrafast..placebo 十档）");
      }
    }

    // scripted 模式的修复动作：字段与 pipeline 同形，但只允许 set_param 真能设的键
    if (j.contains("repair")) {
      const auto& r = j["repair"];
      if (!r.is_object()) return std::unexpected("case " + c.id + ": repair 必须是对象");
      if (!check_keys(r, {"transcode", "bitrate_kbps", "gop", "preset", "width", "height"},
                      "case.repair", err)) {
        return std::unexpected("case " + c.id + ": " + err);
      }
      if (r.contains("transcode")) {
        if (!get_bool(r, "transcode", c.repair.transcode, err)) {
          return std::unexpected("case " + c.id + ": " + err);
        }
        c.repair.set_transcode = true;
      }
      if (r.contains("bitrate_kbps")) {
        if (!get_int(r, "bitrate_kbps", c.repair.bitrate_kbps, err)) {
          return std::unexpected("case " + c.id + ": " + err);
        }
        if (c.repair.bitrate_kbps < 100 || c.repair.bitrate_kbps > 20000) {
          return std::unexpected("case " + c.id + ": repair.bitrate_kbps 必须在 [100,20000]");
        }
        c.repair.set_bitrate = true;
      }
      if (r.contains("gop")) {
        if (!get_int(r, "gop", c.repair.gop, err)) {
          return std::unexpected("case " + c.id + ": " + err);
        }
        // set_param 的 gop 上限是 300（cued 的 --gop 允许 600）：修复动作必须真能执行
        if (c.repair.gop < 1 || c.repair.gop > 300) {
          return std::unexpected("case " + c.id + ": repair.gop 必须在 [1,300]");
        }
        c.repair.set_gop = true;
      }
      if (r.contains("preset")) {
        if (!get_string(r, "preset", c.repair.preset, err)) {
          return std::unexpected("case " + c.id + ": " + err);
        }
        if (!x264_preset(c.repair.preset)) {
          return std::unexpected("case " + c.id + ": repair 里未知 preset \"" + c.repair.preset +
                                 "\"");
        }
        c.repair.set_preset = true;
      }
      if (r.contains("width") || r.contains("height")) {
        if (!r.contains("width") || !r.contains("height")) {
          return std::unexpected("case " + c.id + ": repair 的 width/height 必须成对给出");
        }
        if (!get_int(r, "width", c.repair.width, err) ||
            !get_int(r, "height", c.repair.height, err)) {
          return std::unexpected("case " + c.id + ": " + err);
        }
        if (c.repair.width < 16 || c.repair.width > 4096 || c.repair.height < 16 ||
            c.repair.height > 4096 || (c.repair.width % 2) != 0 || (c.repair.height % 2) != 0) {
          return std::unexpected("case " + c.id +
                                 ": repair.width/height 必须是 [16,4096] 内的偶数");
        }
        c.repair.set_size = true;
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
      // 旧字段（fps_min / usable_fps_min / dropped_max / frame_completion_min）保持原样，
      // 新增端到端字段是可选的：不写 = 不检查，旧 suite 的行为完全不变。
      if (!check_keys(s, {"fps_min", "usable_fps_min", "dropped_max", "frame_completion_min",
                          "lat_p99_ms", "lat_p50_ms", "max_stall_ms", "stalls"},
                      "case.expect.sla", err)) {
        return std::unexpected("case " + c.id + ": " + err);
      }
      if (s.contains("fps_min") && s.contains("usable_fps_min")) {
        return std::unexpected("case " + c.id + ": expect.sla 同时给了 fps_min 与 usable_fps_min");
      }
      const char* fps_key = s.contains("usable_fps_min") ? "usable_fps_min" : "fps_min";
      if (!get_double(s, fps_key, c.sla_fps_min, err) ||
          !get_u64(s, "dropped_max", c.sla_dropped_max, err) ||
          !get_double(s, "frame_completion_min", c.sla_frame_completion_min, err) ||
          !get_double(s, "lat_p99_ms", c.sla_lat_p99_ms, err) ||
          !get_double(s, "lat_p50_ms", c.sla_lat_p50_ms, err) ||
          !get_double(s, "max_stall_ms", c.sla_max_stall_ms, err) ||
          !get_u64(s, "stalls", c.sla_stalls_max, err)) {
        return std::unexpected("case " + c.id + ": " + err);
      }
      if (c.sla_fps_min < 0.0) return std::unexpected("case " + c.id + ": fps 下限不能为负");
      if (c.sla_frame_completion_min < 0.0 || c.sla_frame_completion_min > 1.0) {
        return std::unexpected("case " + c.id + ": frame_completion_min 必须在 [0,1]");
      }
      // 端到端阈值是"上限"，负数没有意义（写负会让任何读数都"达标"，等于关掉判分）
      if ((has_value(c.sla_lat_p99_ms) && c.sla_lat_p99_ms < 0.0) ||
          (has_value(c.sla_lat_p50_ms) && c.sla_lat_p50_ms < 0.0) ||
          (has_value(c.sla_max_stall_ms) && c.sla_max_stall_ms < 0.0)) {
        return std::unexpected("case " + c.id + ": 端到端 SLA 阈值不能为负");
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

namespace {

// cassette 目录内容指纹（SPEC §7.7）：文件名 + 文件内容都进哈希。
// 只取 *.jsonl，避免临时文件/README 让缓存无谓失效。
std::string cassette_dir_hash(const std::string& dir) {
  std::vector<std::string> files;
  std::error_code ec;
  if (!dir.empty()) {
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      if (it->path().extension() != ".jsonl") continue;
      files.push_back(it->path().string());
    }
  }
  std::sort(files.begin(), files.end());
  std::string acc;
  // 只哈希相对文件名与内容：同一份 cassette 换目录/换机器不该让指纹漂移。
  for (const auto& f : files) {
    acc += std::filesystem::path(f).filename().string() + "\n" + cue::sha256_file(f) + "\n";
  }
  return cue::sha256_hex(acc);
}

// 读日志尾部（产帧源起不来时把它的最后几句话塞进 run_error）：
// 只读最后 max_bytes 字节，避免一个大日志被整份读进内存。
std::string read_log_tail(const std::string& path, std::size_t max_bytes) {
  if (path.empty()) return {};
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) return {};
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  const std::uintmax_t begin = size > max_bytes ? size - max_bytes : 0;
  in.seekg(static_cast<std::streamoff>(begin));
  std::string out((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  return out;
}

// ---------------------------------------------------------------- 产帧源（N-23）
//
// 为什么评测要自己起源：rate 类注入是「发送速率上限」，令牌桶只有在源码率**超过**
// 上限时才会耗尽。旧的产帧源 `scripts/test-client.mjs` 每帧发同一个 64×64 内嵌 IDR
// （约 677 B），30 fps 合计远低于 0.1 Mbps（12.5 kB/s）——budget_drops 恒 0、
// frames_in == frames_out、fps 30，故障根本没发生，SLA 却判达标。
//
// 这里起的是 `cue-capture --backend lavfi`（testsrc2 合成画面），参数形式与
// `scripts/run.sh:161-166` 起采集端时一致，只有 WS 目标端口来自本 case 的 cued。
// 默认（--producer none）一个进程都不起，命令与环境与改动前完全一致。
//
// 生命周期（谁起谁收，顺序不能反）：
//   cued 起来（pipe->start 成功）→ 起 cue-capture → warmup 等流 → 跑 case
//   → 收 cue-capture（连同它的 ffmpeg 子进程）→ 收 cued
// 先收 cued 的话采集端会疯狂重连、日志刷屏，还可能留下一个仍在跑的 ffmpeg。
//
// 为什么不做"采集端挂了自动重拉"：Process 后端的注入（fault.set）与档位
// （param.set 的 bitrate/gop/preset/transcode）都是热生效，一个 case 内 cued
// 不会重启；会重启的只有崩溃，而崩溃本身就该让该 case 以无流/重测无采样显式失败
// （cue-capture 在 WS 断开时自己退出，不会留下一个"看起来还在跑"的假源）。
// 加一个后台重拉线程反而会在这种时候把"管线崩了"变成"流又回来了"，掩盖真问题。
class Producer {
 public:
  Producer() = default;
  ~Producer() { stop(); }
  Producer(const Producer&) = delete;
  Producer& operator=(const Producer&) = delete;

  // 起进程并确认它真的活着（fork 后 1.5 s 内退出 = 起不来，必须报 run_error，
  // 否则会静默变成"无流"→ warmup 失败 → 更糟的是被读成"SLA 恒真"）。
  bool start(const RunConfig& cfg, int ws_port, int fps, const std::string& case_id,
             const std::string& run_dir, std::string& err);
  void stop();
  const std::string& command() const { return command_; }

 private:
  pid_t pid_ = -1;
  std::string command_;
  std::string log_path_;
};

bool Producer::start(const RunConfig& cfg, int ws_port, int fps, const std::string& case_id,
                     const std::string& run_dir, std::string& err) {
  stop();
  if (cfg.capture_bin.empty()) {
    err = "--capture 为空";
    return false;
  }
  std::error_code fec;
  if (!std::filesystem::is_regular_file(cfg.capture_bin, fec)) {
    err = "找不到 " + cfg.capture_bin + "（需先构建 cue-capture，或用 --capture 指路径）";
    return false;
  }
  std::vector<std::string> args{
      cfg.capture_bin,
      "--backend", "lavfi",
      "--size", cfg.producer_size,
      "--bitrate", std::to_string(cfg.producer_bitrate),
      "--fps", std::to_string(fps),
      "--audio", "off",
      "--duration", "0",
      "--ws", "ws://127.0.0.1:" + std::to_string(ws_port) + "/",
  };
  command_.clear();
  for (std::size_t i = 0; i < args.size(); ++i) command_ += (i == 0 ? "" : " ") + args[i];

  // 采集端的输出落到 run 目录的 producer.log（不进 git）：它起不来时的原因几乎只在
  // 自己的 stderr 里（例如 ffmpeg 缺失），吞掉就成了"报了个 run_error 但不知道为什么"。
  int out_fd = -1;
  log_path_.clear();
  if (!run_dir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(run_dir, ec);
    log_path_ = run_dir + "/producer.log";
    out_fd = ::open(log_path_.c_str(), O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0644);
    if (out_fd >= 0) {
      const std::string head = "\n=== " + case_id + " " + command_ + "\n";
      const ssize_t ignored = ::write(out_fd, head.data(), head.size());
      (void)ignored;
    }
  }
  if (out_fd < 0) out_fd = ::open("/dev/null", O_WRONLY | O_CLOEXEC);

  int hs[2];
  if (::pipe2(hs, O_CLOEXEC) != 0) {
    err = std::string("pipe2: ") + std::strerror(errno);
    if (out_fd >= 0) ::close(out_fd);
    return false;
  }
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);

  const pid_t pid = ::fork();
  if (pid < 0) {
    err = std::string("fork: ") + std::strerror(errno);
    ::close(hs[0]);
    ::close(hs[1]);
    if (out_fd >= 0) ::close(out_fd);
    return false;
  }
  if (pid == 0) {
    // 自成进程组：收的时候 kill(-pid) 一次连它拉起的 ffmpeg 一起收（不留孤儿）。
    ::setpgid(0, 0);
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (::getppid() == 1) _exit(127);
    if (out_fd >= 0) {
      ::dup2(out_fd, STDOUT_FILENO);
      ::dup2(out_fd, STDERR_FILENO);
      if (out_fd > STDERR_FILENO) ::close(out_fd);
    }
    // 把父进程的其它 fd 全部打上 CLOEXEC：不这样做，采集端会白持有 cued 控制通道、
    // probe 管道之类的 fd，harness 侧的 EOF/关闭语义就变得莫名其妙。
    // 与 src/pipeline/process_control.cpp 的 mark_inherited_fds_cloexec 同一做法。
    long maxfd = ::sysconf(_SC_OPEN_MAX);
    if (maxfd < 0 || maxfd > 4096) maxfd = 4096;
    for (int fd = 3; fd < static_cast<int>(maxfd); ++fd) {
      (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    ::execv(argv[0], argv.data());
    const int exec_errno = errno;
    const ssize_t ignored = ::write(hs[1], &exec_errno, sizeof(exec_errno));
    (void)ignored;
    _exit(127);
  }
  ::close(hs[1]);
  if (out_fd >= 0) ::close(out_fd);

  // exec 结果：成功时子进程的 hs[1] 被 CLOEXEC 关掉 → read 返回 0；
  // 失败时子进程把 errno 写回来（"假装起来了"是评测里最贵的错误之一）。
  int child_errno = 0;
  ssize_t hn = 0;
  do {
    hn = ::read(hs[0], &child_errno, sizeof(child_errno));
  } while (hn < 0 && errno == EINTR);
  ::close(hs[0]);
  if (hn == static_cast<ssize_t>(sizeof(child_errno))) {
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    err = "exec " + cfg.capture_bin + ": " + std::strerror(child_errno);
    return false;
  }

  // 起来就退（典型：ffmpeg 不在、lavfi 参数被拒）也算起不来。1.5 s 覆盖 ffmpeg
  // 进程创建 + 编码器初始化（实测 0.3–0.5 s）与首次 WS 连接。
  for (int i = 0; i < 15; ++i) {
    int status = 0;
    const pid_t w = ::waitpid(pid, &status, WNOHANG);
    if (w == pid) {
      const std::string tail = read_log_tail(log_path_, 600);
      err = "'" + command_ + "' 起来后立刻退出（" +
            (WIFSIGNALED(status) ? "signal=" + std::to_string(WTERMSIG(status))
                                 : "exit=" + std::to_string(WEXITSTATUS(status))) +
            "）" + (tail.empty() ? std::string() : "；它会话日志尾部：" + tail);
      return false;
    }
    if (w < 0 && errno == ECHILD) {
      err = "无法回收产帧源进程（waitpid: " + std::string(std::strerror(errno)) + "）";
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  pid_ = pid;
  return true;
}

void Producer::stop() {
  if (pid_ <= 0) return;
  // 先整组 SIGTERM（cue-capture 自己也会收 ffmpeg，这里是兜底）：
  // 进程组是产帧源的，所以 kill(-pid) 不会波及 cued / probe。
  ::kill(-pid_, SIGTERM);
  ::kill(pid_, SIGTERM);
  bool reaped = false;
  for (int i = 0; i < 50; ++i) {  // 最多 5 s；它有 1 s 的 ffmpeg 收尾窗口
    int status = 0;
    const pid_t w = ::waitpid(pid_, &status, WNOHANG);
    if (w == pid_ || (w < 0 && errno == ECHILD)) {
      reaped = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (!reaped) {
    ::kill(-pid_, SIGKILL);
    ::kill(pid_, SIGKILL);
    int status = 0;
    while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
    }
  }
  pid_ = -1;
}

// 起产帧源（N-23）：只在 Process 后端且 producer=lavfi 时生效。
// 返回值语义：nullptr + err 为空 = 本次不需要产帧源（默认）；nullptr + err 非空 =
// 需要但起不来（调用方记 run_error=producer_start_failed）。
// HTTP / 录制回放后端没有可推流的本机 cued，给了 --producer 也只能报错：
// 静默忽略会让"SLA 恒真"以另一种形式回来（没人推流，case 却按达标收场）。
std::unique_ptr<Producer> start_producer_if_needed(const RunConfig& cfg, const Case& c,
                                                   const PipelineConfig& pc, bool process_backend,
                                                   int fps, const std::string& run_dir,
                                                   std::string& err) {
  if (cfg.producer == "none") return nullptr;
  if (!process_backend) {
    err = "--producer " + cfg.producer + " 只对本机子进程后端（Process）有意义；"
          "HTTP 端点与工具回放不起进程，也就没有可推流的目标";
    return nullptr;
  }
  std::string perr;
  auto prod = std::make_unique<Producer>();
  if (!prod->start(cfg, pc.ws_port, fps, c.id, run_dir, perr)) {
    err = perr;
    return nullptr;
  }
  std::fprintf(stderr, "[eval][%s] 产帧源已起：%s\n", c.id.c_str(), prod->command().c_str());
  return prod;
}

}  // namespace

std::string Engine::config_hash() const {
  std::string cued_hash;
  if (!cfg_.cued_bin.empty()) cued_hash = cue::sha256_file(cfg_.cued_bin);
  // max_yuan（成本闸门）不进指纹：它只是"跑多少"的护栏，不是被测配置。
  // 进指纹会让"闸门触发后续跑"因为换了闸门值而全量作废，与断点续跑语义冲突。
  //
  // cassette（SPEC §7.7）：replay 的结果取决于 cassette 内容，内容变了缓存必须失效。
  // live/record 不把 cassette 哈希放进指纹——录制会改变目录内容，不该让本次 run 漂移。
  std::string llm_hash;
  if (cfg_.agent_mode == "real") {
    llm_hash = "|llm=" + cfg_.llm_mode;
    if (cfg_.llm_mode == "replay") llm_hash += "|cassette=" + cassette_dir_hash(cfg_.cassette_dir);
  }
  // endpoint 非空时被测 SUT 换成了 HTTP 服务：不进指纹会让"同一 suite 换个端点"复用旧缓存
  // （端点为空的旧指纹保持逐字节不变，续跑兼容）。
  const std::string ep_hash = cfg_.endpoint.empty() ? std::string() : "|endpoint=" + cfg_.endpoint;
  // AC-6：回放结果取决于录制内容，内容变了缓存必须失效；录制中（内容在变）不进指纹。
  const std::string tools_hash =
      cfg_.replay_tools_dir.empty() ? std::string()
                                    : "|replay_tools=" + cassette_dir_hash(cfg_.replay_tools_dir);
  // 产帧源（N-23）：换了源，同一 case 的 metrics 完全不同，缓存必须失效。
  // 只在启用时进指纹——默认（none）的指纹必须与改动前逐字节一致，否则既有
  // runs/ 的续跑缓存会整体作废。
  const std::string producer_hash =
      (cfg_.producer == "none" || cfg_.producer.empty())
          ? std::string()
          : "|producer=" + cfg_.producer + ":" + cfg_.producer_size + "@" +
                std::to_string(cfg_.producer_bitrate);
  return cue::sha256_hex(cfg_.agent_mode + "|" + std::to_string(cfg_.repeat) + "|" + cfg_.model +
                         "|" + cfg_.cued_bin + "|" + cued_hash + "|" +
                         fmt_double(cfg_.yuan_per_mtok) + "|" + std::to_string(cfg_.warmup_s) +
                         llm_hash + ep_hash + tools_hash + producer_hash);
}

PipelineConfig pipeline_config_for(const Case& c, const RunConfig& cfg, const std::string& run_dir) {
  PipelineConfig pc;
  pc.cued_bin = cfg.cued_bin;
  // 探针默认跟着 cued 走：显式 --probe 优先，否则取 cued 同目录的 cue-probe。
  // 只认 "./build/cue-probe" 这种写死路径的话，--cued build-final/cued 就会配错探针，
  // 表现为 probe_unavailable——那不是"探针没实现"，是我们指错了二进制。
  if (!cfg.probe_bin.empty()) {
    pc.probe_bin = cfg.probe_bin;
  } else if (!cfg.cued_bin.empty()) {
    const auto slash = cfg.cued_bin.find_last_of('/');
    pc.probe_bin = (slash == std::string::npos ? std::string()
                                              : cfg.cued_bin.substr(0, slash + 1)) +
                   "cue-probe";
  }
  // 注入档位只来自 case：agent 必须自己从指标里发现故障，评测器不能替它"制造"结论
  pc.inject_drop = (c.fault_kind == "drop") ? c.fault_value : 0.0;
  pc.inject_ingress_drop = (c.fault_kind == "ingress_drop") ? c.fault_value : 0.0;
  pc.send_mbps = (c.fault_kind == "rate") ? c.fault_value : 10.0;
  pc.queue_fps = (c.fault_kind == "backlog") ? c.fault_value : 0.0;
  // 延迟注入也"启动即生效"：与 drop/rate 同一条规则，评测器不替 agent 制造结论，
  // 但故障本身必须在 agent 动手之前就存在。
  pc.inject_delay_ms = (c.fault_kind == "delay") ? c.fault_value : 0.0;
  pc.inject_jitter_ms = (c.fault_kind == "jitter") ? c.fault_value : 0.0;
  pc.inject_seed = c.seed;
  pc.transcode = c.transcode;
  pc.width = c.width;
  pc.height = c.height;
  pc.bitrate_kbps = c.bitrate_kbps;
  pc.gop = c.gop;
  pc.preset = c.preset;
  // 注入 trace（N-11）：M2-3 的「同 seed 逐包可复现」要有自动落盘的文件，而不是让人
  // 手动去敲 `cued --inject-trace`。路径按 run 目录算：<run_dir>/inject-trace/<case>.trace，
  // 同一份 suite 两次 run 的 trace 因此可以逐行 diff（run_id 不同、case 名相同）。
  //
  // 白名单而不是「有注入就落」：cued 只有两种逐包记录点——出向视频丢包判决
  // `<frame_id> <pk>` 行（fault.kind=drop）与抖动注入的 `j <frame_id> <pk> <extra_us>`
  // 行（fault.kind=jitter）。上行丢包 / rate / backlog / 纯 delay 在 cued 里没有逐包
  // 判定可记，给它们指一个 trace 只会得到一个 0 字节文件，读起来像"trace 丢了"。
  // run_dir 为空（离线只算档位）时一律不落 trace，绝不回退到 /tmp 或当前目录。
  const bool traceable = c.fault_kind == "drop" || c.fault_kind == "jitter";
  if (traceable && c.fault_value > 0.0 && !run_dir.empty()) {
    pc.inject_trace = run_dir + "/inject-trace/" + c.id + ".trace";
  }
  return pc;
}

namespace {

// 注入 trace 的目录（<run_dir>/inject-trace/）随 run 建一次：不存在时 cued 的
// `fopen(w)` 会失败，trace 就静默没了。建不出来时清掉路径并大声报错——宁可不落
// trace，也不要让 cued 对着一个不存在的目录写（那样日志里只有一行 "cannot open trace"）。
void ensure_inject_trace_dir(PipelineConfig& pc, const std::string& case_id) {
  if (pc.inject_trace.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(pc.inject_trace).parent_path(), ec);
  if (ec) {
    std::fprintf(stderr, "[eval][%s] 创建注入 trace 目录失败：%s（本次不落 trace）\n",
                 case_id.c_str(), ec.message().c_str());
    pc.inject_trace.clear();
  }
}

// Process 分支专用：建目录，并把最终路径记进 outcome 与控制台日志。
// 只有真的拉起 cued 时才有意义（回放/HTTP 端点不 spawn，也就没有 trace）。
void prepare_inject_trace(PipelineConfig& pc, const Case& c, CaseOutcome& o) {
  ensure_inject_trace_dir(pc, c.id);
  if (pc.inject_trace.empty()) return;
  o.inject_trace = pc.inject_trace;
  std::fprintf(stderr, "[eval][%s] 注入 trace → %s（同 seed 的两次 run 可逐行 diff）\n",
               c.id.c_str(), pc.inject_trace.c_str());
}

}  // namespace

std::string classify_run_error(const agent::RunResult& res) {
  if (res.error.empty()) return {};  // 显式 give_up 仍计分（算失败，不算 run_error）
  const std::string& e = res.error;
  // replay 缺 cassette 是环境/素材问题，不是模型答错：必须先于「零轮次」判定，
  // 否则会被归成 llm_unavailable，报告读不出「该补录制」这一动作。
  if (e.find("cassette miss") != std::string::npos) return "cassette_miss";
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

// case 的管线档位 → 端点描述。评测层只认识 common 层的 PipelineEndpoint 与
// IServicePipeline：具体后端由 pipeline 层的 make_pipeline 创建（IMPL 在
// src/pipeline/service_pipeline.cpp）。这样 eval 不再 include pipeline/ 下任何头文件，
// CMake 侧才可能把 include/cue/pipeline 从 cue_eval 的 include 路径里物理拿掉。
pipeline::PipelineEndpoint to_endpoint(const PipelineConfig& pc) {
  pipeline::PipelineEndpoint out;
  out.kind = pipeline::PipelineEndpoint::Kind::Process;
  out.cued_bin = pc.cued_bin;
  // 产帧源（N-23）要按这个端口拼 ws://127.0.0.1:<port>/，必须与 cued 的监听一致。
  out.ws_port = pc.ws_port;
  out.probe_bin = pc.probe_bin;
  out.inject_drop = pc.inject_drop;
  out.inject_ingress_drop = pc.inject_ingress_drop;
  out.send_mbps = pc.send_mbps;
  out.queue_fps = pc.queue_fps;
  out.inject_delay_ms = pc.inject_delay_ms;
  out.inject_jitter_ms = pc.inject_jitter_ms;
  out.inject_seed = pc.inject_seed;
  out.inject_trace = pc.inject_trace;
  out.transcode = pc.transcode;
  out.width = pc.width;
  out.height = pc.height;
  out.bitrate_kbps = pc.bitrate_kbps;
  out.gop = pc.gop;
  out.preset = pc.preset;
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
  // 六段分位在数据源侧就是齐的：cued 的 METRICS / metrics.query 一直发 p50/p95/p99，
  // consume 侧（process_control → PipelineMetrics）N-05 起也把六段三位都收下，
  // 包括此前连字段都没有的 recv/buffer p99。这里逐段按位搬运，不做任何"补 0"：
  // 某一位在 cued 侧没有样本（段为 null）就保持 NaN，报告照旧写 n/a。
  if (m.samples > 0) {
    out.recv.p50 = m.recv_p50;
    out.recv.p95 = m.recv_p95;
    out.recv.p99 = m.recv_p99;
    out.buffer.p50 = m.buffer_p50;
    out.buffer.p95 = m.buffer_p95;
    out.buffer.p99 = m.buffer_p99;
    out.decode.p50 = m.decode_p50;
    out.decode.p95 = m.decode_p95;
    out.decode.p99 = m.decode_p99;
    out.op.p50 = m.op_p50;
    out.op.p95 = m.op_p95;
    out.op.p99 = m.op_p99;
    out.encode.p50 = m.encode_p50;
    out.encode.p95 = m.encode_p95;
    out.encode.p99 = m.encode_p99;
    out.send.p50 = m.send_p50;
    out.send.p95 = m.send_p95;
    out.send.p99 = m.send_p99;
  }
  return out;
}

// pipeline::ProbeMetrics → 评测层 POD。NaN 一个都不补成 0：端到端的"没测到"
// 与"测到 0 ms"在判分上是相反的结论（前者不可证明、后者是优秀）。
ProbeSummary to_probe_summary(const pipeline::ProbeMetrics& p) {
  ProbeSummary out;
  out.alive = p.alive;
  out.samples = p.samples;
  out.fps = p.fps;
  out.frames = p.frames;
  out.decoded = p.decoded;
  out.decode_errors = p.decode_errors;
  out.stalls = p.stalls;
  if (p.samples > 0) {
    out.max_stall_ms = double(p.max_stall_us) / 1000.0;
    out.e2e.p50 = p.lat_p50_ms;
    out.e2e.p95 = p.lat_p95_ms;
    out.e2e.p99 = p.lat_p99_ms;
    out.recv_us.p50 = p.recv.p50;
    out.recv_us.p95 = p.recv.p95;
    out.recv_us.p99 = p.recv.p99;
    out.reassembly_us.p50 = p.reassembly.p50;
    out.reassembly_us.p95 = p.reassembly.p95;
    out.reassembly_us.p99 = p.reassembly.p99;
    out.buffer_us.p50 = p.buffer.p50;
    out.buffer_us.p95 = p.buffer.p95;
    out.buffer_us.p99 = p.buffer.p99;
    out.decode_us.p50 = p.decode.p50;
    out.decode_us.p95 = p.decode.p95;
    out.decode_us.p99 = p.decode.p99;
  }
  out.synced = p.synced;
  out.offset_us = p.offset_us;
  out.unavailable_reason = p.unavailable_reason;
  return out;
}

struct SamplePoint {
  int64_t t_us = 0;
  Metrics m;
  // 同一时刻的探针快照：端到端读数与 cued 侧指标是两条独立的时间序列，
  // 只在需要的地方各自取中位（不把探针的值塞进 cued 的字段）。
  pipeline::ProbeMetrics probe;
};

// agent 运行期间持续采样：MTTR（注入 → 指标回到 SLA）必须靠时间序列，
// 只在 agent 结束后看一个点无法回答"什么时候恢复的"。
class MetricMonitor {
 public:
  // 只认接口：子进程 / HTTP 两种后端在监控线程眼里完全一样
  MetricMonitor(const pipeline::IServicePipeline& pipe, int interval_ms)
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
    uint64_t last_probe = UINT64_MAX;
    while (!stop_.load()) {
      const auto m = pipe_.latest();
      const auto pr = pipe_.probe_latest();
      // 任一侧有新采样就落一个点：端到端序列不必等 cued 的那一秒（两者节奏不同）。
      if ((m.samples != 0 && m.samples != last) ||
          (pr.samples != 0 && pr.samples != last_probe)) {
        last = m.samples;
        last_probe = pr.samples;
        const SamplePoint p{now_us(), to_metrics(m), pr};
        std::lock_guard<std::mutex> lk(mu_);
        points_.push_back(p);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms_));
    }
  }

  const pipeline::IServicePipeline& pipe_;
  int interval_ms_;
  std::atomic<bool> stop_{false};
  mutable std::mutex mu_;
  std::vector<SamplePoint> points_;
  std::thread th_;
};

// agent 结束后等 3 条新采样（cued 每秒一条），最多等 8 s
constexpr int kRetestSamples = 3;
// 等探针刷新一条新指标行的轮数（200 ms 一步 → 2 s）：探针每秒一条，
// 2 s 还没刷新说明它不再上报（此时按 probe_unavailable 处理，而不是干等）
constexpr int kProbeWaitSteps = 10;
constexpr int kRetestWaitMs = 8000;
constexpr int kSleepStepMs = 200;
// 超过这个年龄的采样算"陈旧"（cued 只在有媒体流时打点，流断了就不会更新）
constexpr int64_t kStaleSampleUs = 3'000'000;

// 窗口内的端到端汇总：分位取中位、卡顿取增量、最长卡顿取窗口内最大。
// stalls 是探针进程的累计量，跨窗口要差分；探针崩溃重启会让计数清零，
// 遇到清零按 0 重新计（与 dropped 的窗口增量同一个处理，避免"重启即达标"）。
ProbeSummary summarize_probe_window(const std::vector<SamplePoint>& pts) {
  ProbeSummary out;
  std::vector<double> p50, p95, p99, fps, max_stall;
  const SamplePoint* first = nullptr;
  const SamplePoint* last = nullptr;
  for (const auto& s : pts) {
    if (s.probe.samples == 0) continue;
    const ProbeSummary one = to_probe_summary(s.probe);
    if (first == nullptr) first = &s;
    last = &s;
    if (has_value(one.e2e.p50)) p50.push_back(one.e2e.p50);
    if (has_value(one.e2e.p95)) p95.push_back(one.e2e.p95);
    if (has_value(one.e2e.p99)) p99.push_back(one.e2e.p99);
    if (has_value(one.max_stall_ms)) max_stall.push_back(one.max_stall_ms);
    fps.push_back(one.fps);
  }
  if (last == nullptr) {
    // 一条探针指标都没收到：字段保持 NaN，但**原因**必须带出去（探针没起/绑不上端口/
    // 根本没上报），否则调用方只能看到"没有样本"，定位不到是哪一种。
    if (!pts.empty()) out = to_probe_summary(pts.back().probe);
    out.samples = 0;
    return out;
  }
  out = to_probe_summary(last->probe);
  out.e2e.p50 = median(p50);
  out.e2e.p95 = median(p95);
  out.e2e.p99 = median(p99);
  out.fps = median(fps);
  // 最长卡顿：探针报的是会话内累计最大，取窗口内最大即可（只会高估，不会漏报）
  double worst = kNoValue;
  for (double v : max_stall) worst = has_value(worst) ? std::max(worst, v) : v;
  out.max_stall_ms = worst;
  out.stalls = 0;
  if (first != nullptr && last != first) {
    out.stalls = last->probe.stalls >= first->probe.stalls
                     ? last->probe.stalls - first->probe.stalls
                     : last->probe.stalls;  // 探针重启清零：只算这一代
  } else if (last != nullptr) {
    // 窗口里只有一条探针采样：探针每次重启都从 0 开始计（修复动作本身就会重启它），
    // 所以这一代的累计值就是窗口值。写 0 会被读成"这一秒没有卡顿"，那是编出来的。
    out.stalls = last->probe.stalls;
  }
  return out;
}

// 某个时间窗口内端到端 p99 的中位；没有样本返回 NaN（报告写 null 并带原因）
double window_probe_p99(const std::vector<SamplePoint>& pts, int64_t from_us, int64_t to_us) {
  std::vector<double> v;
  for (const auto& s : pts) {
    if (s.t_us < from_us || s.t_us > to_us) continue;
    if (s.probe.samples == 0) continue;
    const ProbeSummary one = to_probe_summary(s.probe);
    if (has_value(one.e2e.p99)) v.push_back(one.e2e.p99);
  }
  return median(v);
}

// 端到端字段为 null 的原因文案：报告不能只写 n/a 让人猜
std::string e2e_null_reason_for(const ProbeSummary& probe, bool require_probe, bool has_before,
                                bool has_fault) {
  if (probe.samples == 0) {
    if (!probe.unavailable_reason.empty()) return "探针不可用：" + probe.unavailable_reason;
    return require_probe ? "探针没有上报任何指标样本" : "该后端没有探针（假域/回放）";
  }
  std::string why;
  if (!has_before) why += "注入前窗口无探针样本（Process 后端的故障随 cued 启动即生效）";
  if (!has_fault) why += (why.empty() ? "" : "；") + std::string("故障窗口无探针样本");
  return why;
}

// 从观测原文里取出 key 的 value 的**逐字**片段（JSON 的 "k": v 或文本的 k=v）。
// scripted+endpoint 的假 agent 用它在真实观测里挑证据；不做格式化/四舍五入——
// 判分侧按归一化子串与数值比对，写法一旦不同就可能失配（review-eval.md #5）。
std::string observation_fragment(const std::string& text, const std::string& key) {
  std::size_t value_at = std::string::npos;
  const std::string json_pat = "\"" + key + "\":";
  const std::size_t pos = text.find(json_pat);
  if (pos != std::string::npos) {
    value_at = pos + json_pat.size();
  } else {
    const std::string text_pat = key + "=";
    const std::size_t pos2 = text.find(text_pat);
    if (pos2 == std::string::npos) return {};
    value_at = pos2 + text_pat.size();
  }
  while (value_at < text.size() && std::isspace(static_cast<unsigned char>(text[value_at]))) ++value_at;
  std::size_t end = value_at;
  while (end < text.size()) {
    const unsigned char u = static_cast<unsigned char>(text[end]);
    if (std::isalnum(u) || text[end] == '.' || text[end] == '-' || text[end] == '+') {
      ++end;
    } else {
      break;
    }
  }
  if (end == value_at) return {};
  return key + "=" + text.substr(value_at, end - value_at);
}

// 自检路径（scripted，无真管线）的探针读数：按 case 声明的阈值构造一份「刚好达标」的
// 端到端数字。它的性质与这里把 root_cause 取自 expect 完全一样——这条路径证明的是
// 「判分器 + 引擎」链路能被端到端 SLA 驱动，不是任何实测值（报告顶部有 scripted banner）。
ProbeSummary simulated_probe(const Case& c) {
  ProbeSummary p;
  p.alive = true;
  p.samples = 1;
  p.fps = 30.0;
  p.frames = 300;
  p.decoded = 300;
  p.synced = true;
  p.e2e.p50 = has_value(c.sla_lat_p50_ms) ? c.sla_lat_p50_ms * 0.5 : 12.0;
  p.e2e.p95 = 20.0;
  p.e2e.p99 = has_value(c.sla_lat_p99_ms) ? c.sla_lat_p99_ms * 0.5 : 33.0;
  p.max_stall_ms = has_value(c.sla_max_stall_ms) ? c.sla_max_stall_ms * 0.5 : 33.0;
  p.stalls = c.sla_stalls_max != UINT64_MAX ? c.sla_stalls_max : 0;
  p.recv_us.p50 = 30.0;
  p.reassembly_us.p50 = 40.0;
  p.decode_us.p50 = 900.0;
  return p;
}

}  // namespace

bool Engine::cancelled() const { return cfg_.cancel != nullptr && cfg_.cancel->load(); }

CaseOutcome Engine::run_scripted_case(const Case& c) const {
  // 引擎自测：用 case 的 expect 构造"正确 agent"的结论，观测由真实管线字段模拟。
  // 目的不是证明模型能力，而是验证「判分器 + 引擎」这条链路本身工作正常。
  // 报告顶部会打 scripted banner，避免被读成"模型 100%"。
  //
  // --endpoint 时不能再"模拟"观测：AC-16 要的是假域被真驱动一遍，所以观测与 SLA
  // 都取自 HTTP 后端（脚本里的写死值在假域里根本不存在，会直接把这条验收证伪）。
  if (!cfg_.endpoint.empty() || !cfg_.record_tools_dir.empty() || !cfg_.replay_tools_dir.empty()) {
    return run_scripted_case_pipeline(c);
  }
  CaseOutcome o;
  o.id = c.id;
  // 注入有效性（N-23）：这条自检路径不起管线，注入只是 case 里的声明，没有可判的读数。
  // 明确写"未判定"而不是补一个结论——观测是构造的，任何"生效/未生效"都会是假读数。
  o.injection.fault_kind = c.fault_kind;
  if (c.fault_kind != "none") {
    o.injection.detail = "scripted 自检不起管线，没有注入读数（要判注入是否生效需 --record-tools 或 --producer）";
  }
  std::vector<Observation> obs;
  Observation ob;
  ob.tool = "query_metrics";
  // 假观测覆盖各类 case 会声明的候选指标（不同故障类的证据键不同：injected / budget_drops /
  // frames_in-frames_out / encode_p50_ms / send_p99_ms / delayed_pkts ...）。
  // 值写死是自检路径的既定前提，报告顶部有 banner。
  //
  // send 段分位与延迟计数是 T-10 新增的候选键：这条自检路径不跑管线，所以这里给的
  // 是"时延类故障会看到的那组数"（send 段被压住、延迟计数在涨），不是实测值——
  // 与上面 injected=59 同性质。真管线路径（--record-tools / --endpoint）的读数才是证据。
  // N-02：FEC/幽灵帧/音频帧号与编码档位也是各类 case 会声明的候选键。这条自检路径
  // 不跑管线，所以这里给的同样是"该类故障会看到的那组数"：
  //   - e2e_fec_*：注入 5% 丢包 + FEC 时的读数（恢复涨、迟到分片守卫命中、有坏组）；
  //   - encode_*：1080p30 的真实档位（AVC level 类 case 的唯一可引用观测）。
  // 与 injected=59 同性质，不是实测值；真管线路径的读数才是证据。
  // 注意 e2e_fec_ghost_discarded 要按**常态**给（FEC 校验包每帧一次），
  // 不能写成 0——那会让"零丢包也非零"的真实口径在自检路径上失真。
  ob.data = json{{"fps", 30.3},        {"frames_in", 247},   {"frames_out", 247},
                 {"dropped", 0},       {"injected", 59},     {"decoded", 247},
                 {"decode_errors", 0}, {"budget_drops", 0},  {"encode_p50_ms", 6.4},
                 {"encode_p99_ms", 9.1}, {"op_p50_ms", 0.3}, {"samples", 1},
                 {"send_p50_ms", 201.2}, {"send_p99_ms", 233.0},
                 {"delayed_pkts", 120},  {"delayed_max_us", 233000}, {"ingress_late", 0},
                 {"encode_width", 1920}, {"encode_height", 1080},
                 {"encode_bitrate_kbps", 6000}, {"encode_gop", 60},
                 {"encode_preset", "veryfast"}, {"encode_transcode", "on"},
                 {"e2e_fec_recovered", 37}, {"e2e_fec_groups_rx", 118},
                 {"e2e_fec_unrecoverable", 1}, {"e2e_fec_unrecoverable_pkts", 3},
                 {"e2e_fec_ghost_discarded", 247}, {"e2e_frag_ghost_discarded", 2},
                 {"e2e_lost", 4}, {"e2e_audio_datagrams", 1500},
                 {"e2e_audio_seq_gaps", 12}, {"e2e_audio_dups", 0},
                 {"e2e_audio_reordered", 1}, {"e2e_audio_stale", 0}};
  ob.text = "metrics=" + ob.data.dump();
  obs.push_back(ob);

  agent::RunResult fake;
  fake.final_stage = agent::Stage::Done;
  fake.turns = 6;
  fake.cost_yuan = 0.01;
  fake.diagnosis.root_cause = c.expect_root_cause;
  // 证据同样按 case 的候选指标构造（否则非丢包类的 case 会在这条自检路径里恒假）：
  // 逐字从假观测里摘 key=value，和真管线路径的口径一致。
  for (const auto& cand : c.evidence_candidates) {
    const std::string frag = observation_fragment(ob.text, cand);
    if (frag.empty()) continue;
    fake.diagnosis.evidence.push_back(frag);
    if (fake.diagnosis.evidence.size() >= 2) break;
  }
  if (fake.diagnosis.evidence.empty()) fake.diagnosis.evidence = {"fps=30.3"};

  Metrics m;
  m.samples = 1;
  m.fps = 30.3;
  m.frames_in = 247;
  m.frames_out = 247;
  m.dropped = 0;
  m.injected = 59;
  m.decoded = 247;
  m.probe = simulated_probe(c);
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

CaseOutcome Engine::run_scripted_case_pipeline(const Case& c) const {
  // 假 agent 通过**同一份**工具层（build_real_tools）驱动真实管线：
  //  - --endpoint：HTTP 后端（AC-16 主路径）
  //  - --record-tools / --replay-tools：本机子进程后端的录制与回放（AC-6）
  // 观测来自管线（或它的录制）、证据从观测里逐字摘抄、SLA 由 harness 复测。
  // 任何一步失败都记 run_error，不拿写死的值兜底（否则这条验收只是在自证）。
  CaseOutcome o;
  o.id = c.id;
  o.final_stage = "n/a";
  // 故障类先记下：run_error 早退时报告也要能列出"这条注入类 case 判没判"。
  o.injection.fault_kind = c.fault_kind;

  const bool replay_tools = !cfg_.replay_tools_dir.empty();
  const bool record_tools = !cfg_.record_tools_dir.empty();
  const bool http = !cfg_.endpoint.empty();
  auto pcfg = pipeline_config_for(c, cfg_, run_dir_);

  std::string err;
  std::unique_ptr<pipeline::IServicePipeline> pipe;
  pipeline::ReplayPipeline* replay = nullptr;
  if (replay_tools) {
    const std::string path = pipeline::tools_record_path(cfg_.replay_tools_dir, c.id);
    auto rp = std::make_unique<pipeline::ReplayPipeline>(path);
    if (rp->load_error().empty()) {
      replay = rp.get();
      pipe = std::move(rp);
    } else {
      o.error_kind = "tools_replay_load_failed: " + rp->load_error();
      return o;
    }
    std::fprintf(stderr, "[eval][%s] 回放工具观测 %s（不 spawn cued、不连 endpoint）\n",
                 c.id.c_str(), path.c_str());
  } else if (http) {
    pipeline::PipelineEndpoint ep;
    ep.kind = pipeline::PipelineEndpoint::Kind::Http;
    ep.http_base = cfg_.endpoint;
    ep.http_timeout_ms = 3000;
    pipe = pipeline::make_pipeline(ep, err);
    if (!pipe) {
      o.error_kind = "pipeline_create_failed: " + err;
      return o;
    }
  } else {
    // Process 分支：真实 cued 子进程（外部推流由验收脚本提供）
    prepare_inject_trace(pcfg, c, o);
    pipe = pipeline::make_pipeline(to_endpoint(pcfg), err);
    if (!pipe) {
      o.error_kind = "pipeline_create_failed: " + err;
      return o;
    }
    std::fprintf(stderr,
                 "[eval][%s] 启动 cued（inject_drop=%.1f%% ingress=%.1f%% send_mbps=%.2f "
                 "queue_fps=%.1f bitrate=%d gop=%d preset=%s transcode=%d）\n",
                 c.id.c_str(), pcfg.inject_drop, pcfg.inject_ingress_drop, pcfg.send_mbps,
                 pcfg.queue_fps, pcfg.bitrate_kbps, pcfg.gop, pcfg.preset.c_str(),
                 static_cast<int>(pcfg.transcode));
  }
  if (record_tools && !replay_tools) {
    const std::string path = pipeline::tools_record_path(cfg_.record_tools_dir, c.id);
    auto rec = std::make_unique<pipeline::RecordingPipeline>(std::move(pipe),
                                                             cfg_.record_tools_dir, c.id);
    if (!rec->io_error().empty()) {
      o.error_kind = "tools_record_io_failed: " + rec->io_error();
      return o;
    }
    pipe = std::move(rec);
    std::fprintf(stderr, "[eval][%s] 录制工具观测到 %s\n", c.id.c_str(), path.c_str());
  }
  // 回放统计要在所有返回路径上收集（参数不符会让脚本提前 return），
  // 因此用 RAII；miss>0 必须把这条件记成 run_error，不能静默。
  struct ReplayStatsGuard {
    pipeline::ReplayPipeline* rp;
    CaseOutcome* o;
    ~ReplayStatsGuard() {
      if (rp == nullptr) return;
      const auto st = rp->stats();
      o->tool_hits = st.hits;
      o->tool_misses = st.misses;
      if (st.misses > 0) {
        // 回放 miss 是根因：直接用它作 error_kind，别把工具失败链路层层套进去
        o->error_kind = "tools_replay_miss: " + rp->last_error();
        o->scored = false;
      }
    }
  } stats_guard{replay, &o};

  struct StopGuard {
    pipeline::IServicePipeline* p;
    ~StopGuard() { p->stop(); }
  } pipe_guard{pipe.get()};

  if (!pipe->start(err)) {
    o.error_kind = "pipeline_start_failed: " + err;
    return o;
  }
  // 真路径每个 case 起一个新 cued 进程（计数器从 0 开始）。HTTP 端点在 case 之间是同一个
  // 服务，用 stop→start 取得同样的"新管线"语义——否则上一个 case 的 dropped 会累进本 case
  // 的 SLA 预算，同一份 suite 连跑第二遍就会假失败。回放没有真实服务可重启，不做这一步。
  if (http && !replay_tools) {
    pipe->stop();
    if (!pipe->start(err)) {
      o.error_kind = "pipeline_restart_failed: " + err;
      return o;
    }
  }
  // 产帧源（N-23）：cued 起来之后再起采集端（与 scripts/run.sh 的顺序一致）。
  // 起不来必须记 run_error：静默"无流"会让 SLA 变成恒真的假绿灯。
  std::unique_ptr<Producer> producer;
  {
    std::string perr;
    producer = start_producer_if_needed(cfg_, c, pcfg, !http && !replay_tools,
                                        /*fps=*/30, run_dir_, perr);
    if (producer == nullptr && !perr.empty()) {
      o.error_kind = "producer_start_failed: " + perr;
      return o;
    }
  }
  // 析构顺序 = 声明的逆序：先收产帧源，再收管线（StopGuard 在上面、先声明，故后析构）。
  struct ProducerGuard {
    Producer* p;
    ~ProducerGuard() {
      if (p != nullptr) p->stop();
    }
  } prod_guard{producer.get()};

  // 等媒体流。终止条件只看 latest() 的返回值（不按墙上时间），回放时同一串录制值会
  // 走出一模一样的循环次数——这就是「同一份观测」的前提。
  constexpr int kWaitAttempts = 100;  // 录制时最多等 20 s（200 ms 一轮）；回放不睡
  bool got_stream = false;
  for (int i = 0; i < kWaitAttempts; ++i) {
    if (cancelled()) {
      o.error_kind = "cancelled";
      return o;
    }
    const auto pm = pipe->latest();
    if (pm.samples > 0 && pm.fps > 0.0) {
      got_stream = true;
      break;
    }
    if (!replay_tools) std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  if (!got_stream) {
    std::fprintf(stderr, "[eval][%s] 等不到媒体流采样（samples=0 或 fps=0）→ 不启动假 agent\n",
                 c.id.c_str());
    o.error_kind = "no_media_stream";
    return o;
  }

  std::vector<pipeline::ToolTrace> trace;
  tool::Registry reg;
  // 审计落在本 run 目录（runs/<run_id>/audit.jsonl），不进 git；
  // cancel 透传到 run_shell，评测路径的 SIGTERM 同样能打断工具执行（AC-8）。
  pipeline::build_real_tools(
      reg, *pipe, &trace,
      pipeline::RealToolsOptions{.audit_log_path = audit_log_path_, .cancel = cfg_.cancel});
  tool::CallContext ctx;
  ctx.mode = tool::ApprovalMode::Auto;
  ctx.allowed_dangerous = {"inject_fault"};
  auto call = [&reg, &ctx](const char* name, const json& args, std::string& fail) -> bool {
    const auto gr = reg.call_guarded(name, args, ctx);
    // Executed 表示 handler 跑完了；但 handler 内部失败（set_param 返回 false）会体现在
    // outcome 的 error 侧。两者都要当失败，否则回放参数不符会被「继续往下跑」掩盖。
    if (gr.status == tool::GuardedCall::Status::Executed) return true;
    // 失败时带上分类：录制回放的报错里能看出是参数不符（argument）还是后端拒绝（sandbox）。
    fail = std::string(name) + "(" +
           tool::tool_error_kind_name(gr.outcome ? tool::ToolError::Kind::Execution
                                                 : gr.outcome.error().kind) +
           "): " +
           (gr.outcome ? std::string("<no detail>") : gr.outcome.error().message);
    return false;
  };

  std::string fail;
  // case 的管线档位：子进程后端是 cued 启动参数（同值 set_param 幂等、不重启）；
  // HTTP 后端没有进程参数，只能走同一套 set_param 工具生效。
  if (!call("pipeline_set_param", json{{"key", "bitrate_kbps"}, {"value", std::to_string(c.bitrate_kbps)}}, fail) ||
      !call("pipeline_set_param", json{{"key", "gop"}, {"value", std::to_string(c.gop)}}, fail) ||
      (c.transcode && !call("pipeline_set_param", json{{"key", "transcode"}, {"value", "true"}}, fail)) ||
      (c.transcode && !call("pipeline_set_param", json{{"key", "preset"}, {"value", c.preset}}, fail))) {
    o.error_kind = "set_param_failed: " + fail;
    return o;
  }

  // 故障注入由评测器负责（与 cued 的启动参数同义）：agent 的任务是发现并修好它
  bool injected = false;
  if (c.fault_kind != "none" && c.fault_value > 0.0) {
    if (!call("inject_fault", json{{"kind", c.fault_kind}, {"value", c.fault_value}}, fail)) {
      o.error_kind = "inject_failed: " + fail;
      return o;
    }
    injected = true;
  }

  // 注入会重启 cued（计数器归零），第一条新采样往往只覆盖半个窗口：直接取证会让
  // 故障态读数（budget_drops / frames_out / encode_p50）停在"还没坏起来"的瞬间。
  // 这里按**返回值**等到 4 条新采样（约 4 s；令牌桶的 512 包突发在 10 Mbps 下要约 0.5 s
  // 才抽空，限速类故障在头两秒还看不出来）。与回放同构：只按样本号终止，回放不睡。
  //
  // 只对**真管线**（子进程后端）做这件事：HTTP 假域（cue-mock-svc）的计数器是按
  // 「每读一次前进一拍」推进的，多读几次会把 dropped 预算直接读掉（实测 5% 丢包
  // 从 16 涨到 48，m4 的 dropped_max=30 立刻假失败）。端点的假域不需要等稳态：
  // 它的模型是确定的，也没有"重启后第一个窗口只覆盖半秒"的问题。
  if (cfg_.endpoint.empty()) {
    constexpr int kStableSamples = 4;
    int seen = 0;
    uint64_t last_samples = 0;
    for (int i = 0; i < kWaitAttempts && seen < kStableSamples; ++i) {
      const auto pm = pipe->latest();
      if (pm.samples > last_samples) {
        last_samples = pm.samples;
        ++seen;
      } else if (!replay_tools) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
    }
  }

  // 故障窗口的端到端读数：注入后、修复前取一次探针快照。调用点是固定的（不按时间/
  // 不按数值决定是否调用），所以录制与回放消费同一条记录，窗口口径可复现。
  const ProbeSummary fault_probe = to_probe_summary(pipe->probe_latest());
  // 「注入是否真的生效」（N-23）：同一时刻取一次 cued 侧计数快照，判 rate/drop/
  // ingress_drop 的故障有没有真的发生。取在故障窗口内、修复动作之前，读数就是
  // 该窗口的累计值（cued 每 case 重启，计数器从 0 起）。
  {
    const auto fm = pipe->latest();
    o.injection = judge_injection(c.fault_kind, fm.budget_drops, fm.injected);
    if (o.injection.checked) {
      std::fprintf(stderr, "[eval][%s] %s\n", c.id.c_str(), o.injection.detail.c_str());
    }
  }

  // 观测：真调 query_metrics（content 是 "metrics={...}" 原文）
  const auto q = reg.call("query_metrics", json::object());
  if (!q) {
    o.error_kind = std::string("observe_failed: ") + tool::tool_error_kind_name(q.error().kind) +
                   " / " + q.error().message;
    return o;
  }
  // 假 agent 的证据 = 从这次真实观测里逐字摘抄 case 声明的候选指标（不编造、不改写）
  std::vector<std::string> evidence;
  for (const auto& cand : c.evidence_candidates) {
    const std::string frag = observation_fragment(q->content, cand);
    if (frag.empty()) continue;
    evidence.push_back(frag);
    if (evidence.size() >= 2) break;
  }
  if (evidence.empty()) {
    o.error_kind = "no_evidence_in_observation";
    return o;
  }

  // 假 agent 的"修复"：先清除注入（真 agent 也是 inject_fault value=0），
  // 再按 case 的 repair 逐项 set_param（等价于真 agent 会做的改档动作）。
  // 顺序固定，回放/重跑都走同一条调用序列。
  if (injected) {
    if (!call("inject_fault", json{{"kind", c.fault_kind}, {"value", 0.0}}, fail)) {
      o.error_kind = "repair_failed: " + fail;
      return o;
    }
  }
  if (c.repair.set_transcode &&
      !call("pipeline_set_param",
            json{{"key", "transcode"}, {"value", c.repair.transcode ? "true" : "false"}}, fail)) {
    o.error_kind = "repair_failed: " + fail;
    return o;
  }
  if (c.repair.set_preset &&
      !call("pipeline_set_param", json{{"key", "preset"}, {"value", c.repair.preset}}, fail)) {
    o.error_kind = "repair_failed: " + fail;
    return o;
  }
  if (c.repair.set_gop &&
      !call("pipeline_set_param", json{{"key", "gop"}, {"value", std::to_string(c.repair.gop)}},
            fail)) {
    o.error_kind = "repair_failed: " + fail;
    return o;
  }
  if (c.repair.set_bitrate &&
      !call("pipeline_set_param",
            json{{"key", "bitrate_kbps"}, {"value", std::to_string(c.repair.bitrate_kbps)}}, fail)) {
    o.error_kind = "repair_failed: " + fail;
    return o;
  }
  // width/height 必须成对：set_param 一次只改一个键，先 width 再 height（两次重启）
  if (c.repair.set_size &&
      (!call("pipeline_set_param",
             json{{"key", "width"}, {"value", std::to_string(c.repair.width)}}, fail) ||
       !call("pipeline_set_param",
             json{{"key", "height"}, {"value", std::to_string(c.repair.height)}}, fail))) {
    o.error_kind = "repair_failed: " + fail;
    return o;
  }

  // harness 复测 SLA：直接调接口（与 real 路径同一口径，不信 agent 自报）。
  // 录制/回放路径先等一条修复后的新采样（修复会重启管线，立即读会拿到清零后的 0），
  // 再取 kRetestSamples 条——等待只按返回值终止，回放才能逐步复现同一串调用。
  // 重测窗口：cued 与探针两侧一起采（探针在 Process 后端是 harness 自己托管的，回放模式下
  // 由录制提供）。调用序列必须与录制时逐次对齐，所以循环结构一个字都不改，只是每次多取
  // 一份 probe_latest()。
  const bool require_probe = cfg_.endpoint.empty() && !replay_tools;
  std::vector<SamplePoint> post;
  auto take_sample = [&]() {
    const auto cued = pipe->latest();
    post.push_back(SamplePoint{now_us(), to_metrics(cued), pipe->probe_latest()});
    return cued;
  };
  // 收敛期：见 RunConfig::sla_settle_ms 的注释。回放模式跳过——录制时已经等过，
  // 回放要确定性，不能引入墙钟依赖（否则 AC-6 的回放一致性会被破坏）。
  if (!replay_tools && cfg_.sla_settle_ms > 0) {
    const int64_t settle_deadline = now_us() + int64_t(cfg_.sla_settle_ms) * 1000;
    while (now_us() < settle_deadline) {
      if (cfg_.cancel != nullptr && cfg_.cancel->load()) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  bool have_sample = false;
  if (replay_tools || record_tools) {
    for (int i = 0; i < kWaitAttempts; ++i) {
      const auto pm = take_sample();
      if (pm.samples > 0) {
        have_sample = true;
        break;
      }
      if (!replay_tools) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (have_sample) {
      // 第一条是"修复后刚重启"的过渡窗口：里面可能还有旧流残留的帧（跨重启的 pts），
      // 端到端分位会被一两个离群样本拉爆。丢掉它，重测窗口只取重启之后的三个完整窗口。
      post.clear();
      // 探针的指标行每秒一条：三个"重测样本"若在同一秒内连读，端到端读数其实只刷新了
      // 一次（甚至没刷新），重测窗口就退化成单点。这里按「探针样本号必须增长」推进，
      // 停止条件只看返回值，所以回放会消费同样的条数、得到同一串数值。
      for (int i = 0; i < kRetestSamples; ++i) {
        const uint64_t want = post.empty() ? 0 : post.back().probe.samples + 1;
        for (int w = 0; w < kProbeWaitSteps; ++w) {
          if (pipe->probe_latest().samples >= want) break;  // 只为推进窗口，不落样本
          if (!replay_tools) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        take_sample();
      }
    }
  } else {
    for (int i = 0; i < kRetestSamples; ++i) {
      const auto pm = take_sample();
      if (pm.samples == 0) continue;
      have_sample = true;
    }
  }
  if (!have_sample) {
    o.error_kind = "no_retest_sample";
    return o;
  }
  // 重测窗口的读数：**fps 取最后 3 条的中位数**（报告口径一直这么写），
  // 其余字段取最新一条。
  //
  // 只取"最后一条 samples>0 的点"是不够的：samples>0 并不保证 fps>0。
  // cued 刚重启的那一瞬间，采样是有的，但窗口里还没有帧 → fps 记 0，
  // 于是 fps_min=25 判失败，而同一 case 的日志里"复测 fps"（三条件中位）是 28.7。
  // 实测 netdelay-400ms 三次 repeat 里就有一次踩到这个窗口，
  // 表现为 report.final_fps=0.0 但 eval.log 三次都是 sla=1 —— 两个口径不一致。
  Metrics m;
  std::vector<double> recent_fps;
  for (auto it = post.rbegin(); it != post.rend() && recent_fps.size() < 3; ++it) {
    if (it->m.samples > 0) {
      if (m.samples == 0) m = it->m;  // 最新一条作为其余字段的基准
      recent_fps.push_back(it->m.fps);
    }
  }
  if (!recent_fps.empty()) {
    std::sort(recent_fps.begin(), recent_fps.end());
    m.fps = recent_fps[recent_fps.size() / 2];
  }
  m.probe = summarize_probe_window(post);

  // 探针不可用 = 明确失败，绝不静默退回 cued 指标：端到端 SLA 若拿 cued 的自报去判，
  // "探针根本没起来"就会被读成"端到端达标"，这是本项目最不能接受的假绿灯。
  // 假域/回放后端本来就没有探针，不在这条要求之内（它们的 SLA 只用 cued 侧字段）。
  if (require_probe && m.probe.samples == 0) {
    o.error_kind = "probe_unavailable: " +
                   (m.probe.unavailable_reason.empty() ? std::string("探针没有上报任何指标样本")
                                                       : m.probe.unavailable_reason);
    // 指标快照也要留档：报告/state 里要能看到"探针为什么不可用"，而不是只有一行 error_kind
    o.metrics = m;
    o.e2e_null_reason = e2e_null_reason_for(m.probe, true, false, false);
    return o;
  }
  o.p99_ms_after = m.probe.e2e.p99;
  // 故障窗口 = 注入后、修复前的那个点；注入前窗口在 Process 后端不存在（故障随启动生效），
  // 所以这里如实留 NaN，报告会说明原因。
  if (fault_probe.samples > 0) o.p99_ms_fault = fault_probe.e2e.p99;
  o.e2e_null_reason = e2e_null_reason_for(m.probe, require_probe, false,
                                          has_value(o.p99_ms_fault));

  agent::RunResult fake;
  fake.final_stage = agent::Stage::Done;
  fake.turns = 6;
  fake.cost_yuan = 0.01;
  fake.diagnosis.root_cause = c.expect_root_cause;
  fake.diagnosis.evidence = evidence;

  std::vector<Observation> obs;
  obs.reserve(trace.size());
  for (const auto& t : trace) obs.push_back(Observation{t.tool, t.text, t.data});

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
  o.observations = obs;
  if (replay != nullptr) {
    const auto st = replay->stats();  // 只为下面这行日志先读一次；终态由 RAII 守卫写回
    o.tool_hits = st.hits;
    o.tool_misses = st.misses;
  }
  const char* how = replay_tools ? "replay-tools" : (record_tools ? "record-tools" : "endpoint");
  std::string miss_note;
  if (replay) miss_note = "，工具回放 miss=" + std::to_string(o.tool_misses);
  std::fprintf(stderr,
               "[eval][%s] scripted+%s：root=%d evidence=%d sla=%d（证据 %s；复测 fps=%.2f "
               "dropped=%llu%s）\n",
               c.id.c_str(), how, static_cast<int>(o.score.root),
               static_cast<int>(o.score.evidence), static_cast<int>(o.score.sla),
               evidence.front().c_str(), m.fps,
               static_cast<unsigned long long>(m.dropped), miss_note.c_str());
  return o;
}

CaseOutcome Engine::run_real_case(const Case& c) const {
  CaseOutcome o;
  o.id = c.id;
  o.final_stage = "n/a";
  // 故障类先记下：run_error 早退时报告也要能列出"这条注入类 case 判没判"。
  o.injection.fault_kind = c.fault_kind;
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
  // replay 不需要 key（这正是评测零成本的原因）；只有 live 才要求凭证。
  const bool live = cfg_.llm_mode == "live";
  if (live && (!key || !*key)) {
    o.error_kind = "missing_api_key";
    return;
  }
  const char* base_env = std::getenv("CUE_BASE_URL");
  const std::string base_url = (base_env && *base_env) ? base_env : "https://api.deepseek.com";

  // 1) 起管线：默认起本机 cued；--endpoint 时改为连一个实现 REST 控制协议的 HTTP 服务。
  //    此后所有使用（监控 / 工具 / SLA 复测）都只经 IServicePipeline，不认识具体后端。
  auto pcfg = pipeline_config_for(c, cfg_, run_dir_);
  const bool replay_tools = !cfg_.replay_tools_dir.empty();
  const bool record_tools = !cfg_.record_tools_dir.empty();
  // 录制/回放工具观测时不能有后台轮询线程：latest() 的序列按调用顺序消费，
  // MetricMonitor 每 500 ms 抽一条会让回放侧消费位置漂移 → 同一份录制回放出不同数值。
  // 代价是这两种模式没有 MTTR（记 n/a），换的是「同一份观测必然得到同一判定」。
  const bool deterministic_tools = replay_tools || record_tools;
  std::string err;
  std::unique_ptr<pipeline::IServicePipeline> pipe;
  pipeline::ReplayPipeline* replay = nullptr;
  if (replay_tools) {
    const std::string path = pipeline::tools_record_path(cfg_.replay_tools_dir, c.id);
    auto rp = std::make_unique<pipeline::ReplayPipeline>(path);
    if (!rp->load_error().empty()) {
      o.error_kind = "tools_replay_load_failed: " + rp->load_error();
      return;
    }
    replay = rp.get();
    pipe = std::move(rp);
    std::fprintf(stderr, "[eval][%s] 回放工具观测 %s（不 spawn cued、不连 endpoint）\n",
                 c.id.c_str(), path.c_str());
  } else if (!cfg_.endpoint.empty()) {
    pipeline::PipelineEndpoint ep;
    ep.kind = pipeline::PipelineEndpoint::Kind::Http;
    ep.http_base = cfg_.endpoint;
    ep.http_timeout_ms = 3000;
    pipe = pipeline::make_pipeline(ep, err);
    if (!pipe) {
      o.error_kind = "pipeline_create_failed: " + err;
      return;
    }
    std::fprintf(stderr, "[eval][%s] 连接管线端点 %s（inject_drop=%.1f%%）\n", c.id.c_str(),
                 cfg_.endpoint.c_str(), pcfg.inject_drop);
  } else {
    // Process 分支：cued 的启动参数原样进 PipelineEndpoint，由 pipeline 层的工厂
    // 造出 ProcessPipelineControl（eval 不认识这个具体类型）。
    prepare_inject_trace(pcfg, c, o);
    pipe = pipeline::make_pipeline(to_endpoint(pcfg), err);
    if (!pipe) {
      o.error_kind = "pipeline_create_failed: " + err;
      return;
    }
    std::fprintf(stderr,
                 "[eval][%s] 启动 cued（inject_drop=%.1f%% ingress=%.1f%% send_mbps=%.2f "
                 "queue_fps=%.1f bitrate=%d gop=%d preset=%s transcode=%d）\n",
                 c.id.c_str(), pcfg.inject_drop, pcfg.inject_ingress_drop, pcfg.send_mbps,
                 pcfg.queue_fps, pcfg.bitrate_kbps, pcfg.gop, pcfg.preset.c_str(),
                 static_cast<int>(pcfg.transcode));
  }
  if (record_tools && !replay_tools) {
    auto rec = std::make_unique<pipeline::RecordingPipeline>(std::move(pipe),
                                                             cfg_.record_tools_dir, c.id);
    if (!rec->io_error().empty()) {
      o.error_kind = "tools_record_io_failed: " + rec->io_error();
      return;
    }
    pipe = std::move(rec);
    std::fprintf(stderr, "[eval][%s] 录制工具观测到 %s\n", c.id.c_str(),
                 pipeline::tools_record_path(cfg_.record_tools_dir, c.id).c_str());
  }
  // 回放统计要在所有返回路径上收集（参数不符会让脚本提前 return），
  // 因此用 RAII；miss>0 必须把这条件记成 run_error，不能静默。
  struct ReplayStatsGuard {
    pipeline::ReplayPipeline* rp;
    CaseOutcome* o;
    ~ReplayStatsGuard() {
      if (rp == nullptr) return;
      const auto st = rp->stats();
      o->tool_hits = st.hits;
      o->tool_misses = st.misses;
      if (st.misses > 0) {
        const std::string miss = rp->last_error();
        // 工具失败时 handler 已经把同一条 miss 文案带进了 error_kind，别重复一遍
        const bool already = o->error_kind.find(miss) != std::string::npos;
        if (!already) {
          o->error_kind = o->error_kind.empty() ? miss : miss + "；" + o->error_kind;
        }
        if (o->error_kind.rfind("tools_replay_miss", 0) != 0) {
          o->error_kind = "tools_replay_miss: " + o->error_kind;
        }
        o->scored = false;
      }
    }
  } stats_guard{replay, &o};

  struct StopGuard {
    pipeline::IServicePipeline* p;
    ~StopGuard() { p->stop(); }
  } pipe_guard{pipe.get()};

  if (!pipe->start(err)) {
    o.error_kind = "pipeline_start_failed: " + err;
    return;
  }
  // Http 后端没有"进程启动参数"，case 的注入档位只能经接口补上（与 cued 启动参数同义）。
  // 回放时注入早已在录制里生效，不再调一次（那会多消费一条不存在的录制）。
  if (!replay_tools && !cfg_.endpoint.empty() && c.fault_kind != "none" && c.fault_value > 0.0) {
    std::string ierr;
    if (!pipe->inject_fault(c.fault_kind, c.fault_value, ierr)) {
      o.error_kind = "inject_failed: " + ierr;
      return;
    }
  }
  const int64_t t_inject_us = now_us();
  // 产帧源（N-23）：cued 起来之后起采集端。Process 后端才有意义（--endpoint /
  // 回放没有可推流的目标，CLI 也已在启动时拒绝这种组合）。
  std::unique_ptr<Producer> producer;
  {
    std::string perr;
    producer = start_producer_if_needed(cfg_, c, pcfg, cfg_.endpoint.empty() && !replay_tools,
                                        /*fps=*/30, run_dir_, perr);
    if (producer == nullptr && !perr.empty()) {
      o.error_kind = "producer_start_failed: " + perr;
      return;
    }
  }
  struct ProducerGuard {
    Producer* p;
    ~ProducerGuard() {
      if (p != nullptr) p->stop();
    }
  } prod_guard{producer.get()};
  std::unique_ptr<MetricMonitor> mon;
  if (!deterministic_tools) {
    mon = std::make_unique<MetricMonitor>(*pipe, 500);
    mon->start();
  }
  struct MonGuard {
    MetricMonitor* m;
    ~MonGuard() {
      if (m != nullptr) m->stop();
    }
  } mon_guard{mon.get()};

  // 2) warmup：等外部推流起来（agent 早于推流启动会拿到"尚无采样"）。
  //    --endpoint（假域/远端服务）：推流早已在服务内部，跳过等待，只做一次采样检查。
  //    --replay-tools：观测已录好，不再等真实推流（也不得 spawn）。
  if (!replay_tools && cfg_.endpoint.empty()) {
    for (int i = 0; i < cfg_.warmup_s; ++i) {
      if (cancelled()) {
        o.error_kind = "cancelled";
        return;
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    for (int i = 0; i < 20; ++i) {
      const auto m = pipe->latest();
      if (m.samples > 0 && m.fps > 0.0) break;
      if (cancelled()) {
        o.error_kind = "cancelled";
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
  }
  {
    pipeline::PipelineMetrics m = pipe->latest();
    if (replay_tools) {
      // 回放：录制侧的 warmup 轮询可能先记录到 samples=0 再记录到有样本；
      // 这里按「值」继续消费到第一条有样本的记录，不按时间（回放不 sleep）。
      for (int i = 0; i < 20 && m.samples == 0; ++i) m = pipe->latest();
    }
    if (m.samples == 0) {
      // 没有媒体流就别启动 agent：白烧钱，而且会把环境问题算成 agent 能力问题（P1#13）
      std::fprintf(stderr, "[eval][%s] %s\n", c.id.c_str(),
                   cfg_.endpoint.empty()
                       ? "warmup 后仍无采样 → 无媒体流，不启动 agent"
                       : "端点没有返回有效采样（samples=0，或 /api/state 不可达）→ 不启动 agent");
      o.error_kind = "no_media_stream";
      return;
    }
    std::fprintf(stderr, "[eval][%s] warmup 完成：samples=%llu fps=%.1f\n", c.id.c_str(),
                 static_cast<unsigned long long>(m.samples), m.fps);
    // 「注入是否真的生效」（N-23）：故障窗口（注入后、修复前）的 cued 侧计数快照。
    // 取在 warmup 之后、agent 动手之前：此时计数器已经覆盖了至少一个完整窗口，
    // 又不会被 agent 的修复动作影响。rate 判 budget_drops、drop/ingress_drop 判 injected。
    o.injection = judge_injection(c.fault_kind, m.budget_drops, m.injected);
    if (o.injection.checked) {
      std::fprintf(stderr, "[eval][%s] %s\n", c.id.c_str(), o.injection.detail.c_str());
    }
  }

  // 3) 工具（每个返回都留痕，证据校验只认真实观测）
  std::vector<pipeline::ToolTrace> trace;
  tool::Registry reg;
  // 与 scripted-pipeline 路径同一份工具语义：审计路径 + 取消指针都要传，
  // 否则 real 模式下的 run_shell 既无审计也不会被打断。
  pipeline::build_real_tools(
      reg, *pipe, &trace,
      pipeline::RealToolsOptions{.audit_log_path = audit_log_path_, .cancel = cfg_.cancel});

  // 4) LLM 客户端：replay 读 cassette（默认，零成本、可回归），live 才走网络。
  //    live + record 时把每次成功调用追加进 cassette（SPEC §7.9）。
  std::unique_ptr<llm::ILlmClient> client;
  llm::CassetteClient* cassette = nullptr;
  if (live) {
    llm::OpenAiCompatibleClient::Config cc;
    cc.base_url = base_url;
    cc.api_key = key ? key : "";
    cc.model = cfg_.model;
    cc.timeout_s = 60;
    if (cfg_.record) cc.record_dir = cfg_.cassette_dir;
    client = std::make_unique<llm::OpenAiCompatibleClient>(std::move(cc));
  } else {
    llm::CassetteClient::Config cc;
    cc.dir = cfg_.cassette_dir;
    cc.default_model = cfg_.model;
    auto replay = std::make_unique<llm::CassetteClient>(std::move(cc));
    cassette = replay.get();
    client = std::move(replay);
  }
  agent::Runner::Config ac;
  ac.model = cfg_.model;
  ac.system_prompt = pipeline::real_ops_system_prompt();
  ac.ctx.mode = tool::ApprovalMode::Auto;
  // SPEC §7.4：dangerous 工具必须由 case 显式授权；空集合 = 一个都不允许
  ac.ctx.allowed_dangerous = c.allowed_actions;
  // 写工具（含 lifecycle）同样受 case 的 allowed_actions 约束：pipeline_stop 会直接
  // 停掉被测管线，让 case 断流（脚本路径不走 agent，所以只影响 real 模式）。
  ac.ctx.allowed_mutating = c.allowed_actions;
  ac.yuan_per_mtok = cfg_.yuan_per_mtok;
  // AC-8：评测路径同样要能被打断。RunConfig::cancel 由 apps/cue-eval.cpp 的信号处理置位，
  // Runner 每轮/每次 LLM 调用前检查，并把指针写进 ChatRequest.cancel 交给 curl 的中止回调。
  // 少了这一行，SIGTERM 后 agent 会一直卡到 LLM 超时（最坏 3 分钟），AC-8 的 5 秒失效。
  ac.cancel = cfg_.cancel;
  // SPEC §7.5 的 verify→done 守卫：harness 重测，不信 agent 自报（事件里标 verify=measured）。
  // 声明了端到端阈值的 case，守卫也必须看探针：只看 cued 的 fps 会让 agent 在"延迟还没降下来"
  // 时就过 verify→done，把没修好的状态记成修好了。
  ac.verify_sla = [&pipe, &c](const agent::Diagnosis&) {
    const auto m = pipe->latest();
    if (m.samples == 0) return false;
    if (c.sla_fps_min > 0 && m.fps < c.sla_fps_min) return false;
    if (m.dropped > c.sla_dropped_max) return false;
    const bool e2e_required = has_value(c.sla_lat_p99_ms) || has_value(c.sla_lat_p50_ms) ||
                              has_value(c.sla_max_stall_ms) || c.sla_stalls_max != UINT64_MAX;
    if (!e2e_required) return true;
    const auto p = pipe->probe_latest();
    if (p.samples == 0) return false;  // 探针没有样本 = 无法证明端到端达标
    if (has_value(c.sla_lat_p99_ms) &&
        (!has_value(p.lat_p99_ms) || p.lat_p99_ms > c.sla_lat_p99_ms)) {
      return false;
    }
    if (has_value(c.sla_lat_p50_ms) &&
        (!has_value(p.lat_p50_ms) || p.lat_p50_ms > c.sla_lat_p50_ms)) {
      return false;
    }
    if (has_value(c.sla_max_stall_ms) &&
        double(p.max_stall_us) / 1000.0 > c.sla_max_stall_ms) {
      return false;
    }
    return true;
  };

  agent::Runner runner(*client, reg, ac);
  agent::Budget budget;
  budget.max_turns = c.max_turns;
  budget.max_yuan = c.max_yuan;
  budget.max_wall_s = c.max_wall_s;

  const std::string tag = "[eval][" + c.id + "] ";
  const auto res = runner.run(c.prompt, budget, [&tag](const agent::Event& e) {
    if (e.type == "llm.response" || e.type == "run.start") return;
    std::fprintf(stderr, "%s%s %s\n", tag.c_str(), e.type.c_str(), e.data.dump().c_str());
  });
  if (cassette) {
    const auto st = cassette->stats();
    o.cassette_hits = st.hits;
    o.cassette_misses = st.misses;
  }
  const int64_t t_agent_end_us = now_us();

  std::vector<Observation> obs;
  obs.reserve(trace.size());
  for (const auto& t : trace) obs.push_back(Observation{t.tool, t.text, t.data});

  // 5) harness 重测 SLA：等 agent 结束之后的连续 3 条新采样，取中位
  //    （单点采样会把"重启后 0.5 s 的瞬时窗口"当结论，见 review-eval.md #12）
  std::vector<SamplePoint> post;
  if (deterministic_tools) {
    // 录制/回放：重测也是「等一条新采样 + 再取 3 条」的固定序列，终止只看返回值。
    // 不做「采样陈旧」判定：updated_us 是录制时的时间，回放时必然显得很旧，
    // 拿墙上时钟判它会把有效录制误判成 0 采样。
    constexpr int kWaitAttempts = 100;  // 录制时最多等 20 s；回放不睡
    for (int i = 0; i < kWaitAttempts && post.empty(); ++i) {
      const auto pm = pipe->latest();
      const auto pr = pipe->probe_latest();
      if (pm.samples > 0) {
        post.push_back(SamplePoint{now_us(), to_metrics(pm), pr});
      } else if (!replay_tools) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kSleepStepMs));
      }
      if (cancelled()) break;
    }
    for (int i = 0; i < kRetestSamples && !post.empty(); ++i) {
      const auto pm = pipe->latest();
      const auto pr = pipe->probe_latest();
      if (pm.samples == 0) continue;
      post.push_back(SamplePoint{now_us(), to_metrics(pm), pr});
    }
  } else {
    const int64_t deadline_us = t_agent_end_us + int64_t(kRetestWaitMs) * 1000;
    while (now_us() < deadline_us) {
      const auto all = mon->snapshot();
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
    if (!deterministic_tools) {
      const int64_t age_us =
          m.updated_us == 0 ? kStaleSampleUs + 1 : now_us() - m.updated_us;
      if (age_us > kStaleSampleUs) {
        std::fprintf(stderr, "[eval][%s] 重测采样陈旧（%lld ms）→ SLA 不可判定为达标\n",
                     c.id.c_str(), static_cast<long long>(age_us / 1000));
        m.samples = 0;
      }
    }
  } else {
    m.samples = 0;  // 无采样 = 无法证明达成 = 不达标
    std::fprintf(stderr, "[eval][%s] 重测无有效采样 → SLA 不可判定为达标\n", c.id.c_str());
  }

  // 端到端（探针）读数与前后窗口。三个窗口的边界：
  //   注入前 = [开始, t_inject_us)；故障窗口 = [t_inject_us, agent 结束]；修复后 = 重测窗口。
  // Process 后端的故障随 cued 启动参数即生效，所以"注入前"窗口通常是空的——那不是
  // 数据丢失，而是这条链路本来就没有故障前状态，报告里会写明原因。
  const bool require_probe = cfg_.endpoint.empty() && !replay_tools;
  m.probe = summarize_probe_window(post);
  o.p99_ms_after = m.probe.e2e.p99;
  if (mon != nullptr) {
    const auto all = mon->snapshot();
    o.p99_ms_before = window_probe_p99(all, std::numeric_limits<int64_t>::min(), t_inject_us - 1);
    o.p99_ms_fault = window_probe_p99(all, t_inject_us, t_agent_end_us);
  }
  o.e2e_null_reason = e2e_null_reason_for(m.probe, require_probe, has_value(o.p99_ms_before),
                                          has_value(o.p99_ms_fault));
  if (require_probe && m.probe.samples == 0) {
    // 探针不可用时必须显式失败：静默退回 cued 指标会让"端到端"这三个字变成服务端自报。
    // 记成 run_error（不计入成功率分母），报告与 state.jsonl 里都留完整原因。
    o.scored = false;
    o.error_kind = "probe_unavailable: " +
                   (m.probe.unavailable_reason.empty() ? std::string("探针没有上报任何指标样本")
                                                       : m.probe.unavailable_reason);
    o.metrics = m;
    std::fprintf(stderr, "[eval][%s] 探针不可用 → 端到端 SLA 无法判定：%s\n", c.id.c_str(),
                 o.error_kind.c_str());
    return;
  }

  // 6) MTTR：注入 → 指标首次回到 SLA（用整段时间序列，而不是只看最后一个点）。
  //    录制/回放模式没有后台时间序列，MTTR 记 n/a（不拿墙上时钟凑一个假数）。
  o.mttr_s = kNoValue;
  const bool has_sla = c.sla_fps_min > 0 || c.sla_dropped_max != UINT64_MAX ||
                       c.sla_frame_completion_min > 0 || has_value(c.sla_lat_p99_ms) ||
                       has_value(c.sla_lat_p50_ms) || has_value(c.sla_max_stall_ms) ||
                       c.sla_stalls_max != UINT64_MAX;
  if (has_sla && mon != nullptr) {
    const auto all = mon->snapshot();
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
      // 端到端阈值也参与"什么时候算恢复"：只看 fps 会把"延迟还没降下来"记成已恢复。
      // 卡顿次数是累计量，逐点判定"首次回到 SLA"要差分两次采样，MTTR 里只用分位口径；
      // 卡顿阈值仍然由最终重测窗口判（见 judge_case）。
      const bool e2e_ok =
          s.probe.samples > 0 &&
          (!has_value(c.sla_lat_p99_ms) ||
           (has_value(s.probe.lat_p99_ms) && s.probe.lat_p99_ms <= c.sla_lat_p99_ms)) &&
          (!has_value(c.sla_lat_p50_ms) ||
           (has_value(s.probe.lat_p50_ms) && s.probe.lat_p50_ms <= c.sla_lat_p50_ms)) &&
          (!has_value(c.sla_max_stall_ms) ||
           (has_value(s.probe.max_stall_us) &&
            s.probe.max_stall_us / 1000.0 <= c.sla_max_stall_ms));
      const bool e2e_required = has_value(c.sla_lat_p99_ms) || has_value(c.sla_lat_p50_ms) ||
                                has_value(c.sla_max_stall_ms) || c.sla_stalls_max != UINT64_MAX;
      if (fps_ok && drop_ok && (!e2e_required || e2e_ok) && s.m.samples > 0) {
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
  pipe->stop();  // 每个 case 独占一条管线；StopGuard 兜底
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
  // AC-11 审计落点：跟着 run 目录走，run 目录本来就被 .gitignore 排除
  audit_log_path_ = run_dir + "/audit.jsonl";
  // 注入 trace 的落点也从这里派生（<run_dir>/inject-trace/<case>.trace，N-11）
  run_dir_ = run_dir;
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
                {"llm", cfg_.llm_mode},
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

  std::map<std::string, int> pass_count;   // case_id -> 全成功次数
  std::map<std::string, int> attempt_count;
  bool stop = false;
  int skipped_manual = 0;

  for (std::size_t ci = 0; ci < cases.size() && !stop; ++ci) {
    const auto& c = cases[ci];
    const std::string fp = fingerprint_from_base(base, c);
    for (int k = 0; k < cfg_.repeat; ++k) {
      if (cancelled()) {
        r.gated = true;
        r.run_notes.push_back("cancel: 收到 SIGINT/SIGTERM，停止后续 case（已完成部分已落盘）");
        stop = true;
        break;
      }
      // manual_only：不在 agent 根因枚举内、只能人工复现的 case，real 模式直接跳过，
      // 不进入成功率分母（否则会被当成"答错"污染统计）
      if (c.manual_only && cfg_.agent_mode == "real") {
        if (k == 0) r.run_notes.push_back("skipped manual_only: " + c.id);
        ++skipped_manual;
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
          r.run_notes.push_back("state 写入失败：" + w.error());
        }
      }
      // 轮次只在这里赋值：复用缓存时 outcome 来自 state，里面没有轮次（stability 需要它）
      o.attempt = k;
      ++r.executed;

      r.cost_yuan += o.cost_yuan;
      r.cassette_hits += o.cassette_hits;
      r.cassette_misses += o.cassette_misses;
      r.tool_hits += o.tool_hits;
      r.tool_misses += o.tool_misses;
      ++attempt_count[c.id];
      if (o.scored && o.score.all()) ++pass_count[c.id];
      r.cases.push_back(std::move(o));

      if (r.cost_yuan > cfg_.max_yuan) {
        r.run_notes.push_back("cost gate triggered at " + std::to_string(r.cost_yuan));
        r.gated = true;
        stop = true;
        break;
      }
    }
  }
  // 截断 = 闸门/取消导致没跑完计划数（manual_only 跳过不算截断）
  r.truncated = cancelled() || (r.executed + skipped_manual < r.planned);
  if (r.truncated) {
    r.run_notes.push_back("truncated: 计划 " + std::to_string(r.planned) + " 次，实际执行 " +
                          std::to_string(r.executed) + " 次");
  }

  // 汇总
  for (const auto& o : r.cases) {
    if (!o.scored) {
      ++r.run_errors;
      continue;
    }
    ++r.scored;
    if (o.score.root) ++r.root_rate;
    if (o.score.evidence) ++r.evidence_rate;
    if (o.score.sla) ++r.sla_rate;
    // 「注入未生效」（N-23）：单独计数，不进成功率分母、也不改 scored —— 故障有没有
    // 发生与模型答得对不对是两件事。但它进 run_notes，并在报告里有独立小节：
    // 这种 case 的 SLA 是恒真的，不能让它悄悄混进"100% 通过"。
    if (o.injection.checked && !o.injection.effective) {
      ++r.injection_ineffective;
      r.run_notes.push_back("注入未生效：" + o.id + " — " + o.injection.detail);
    }
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

  // 端到端 p99 的 run 级对比：各 case 的中位（只统计真正有探针样本的 case）。
  // 没有任何样本时写 NaN + 原因，报告里渲染成 null——不用 0 冒充。
  {
    std::vector<double> before, after;
    bool any_probe = false;
    for (const auto& o : r.cases) {
      if (!o.scored) continue;
      if (o.metrics.probe.samples > 0) any_probe = true;
      if (has_value(o.p99_ms_before)) before.push_back(o.p99_ms_before);
      if (has_value(o.p99_ms_after)) after.push_back(o.p99_ms_after);
    }
    r.p99_ms_before = median(before);
    r.p99_ms_after = median(after);
    if (!cfg_.replay_tools_dir.empty()) {
      r.probe_source = "probe 指标来自工具录制（--replay-tools）";
    } else if (!cfg_.endpoint.empty()) {
      r.probe_source = "无（HTTP 后端没有探针，端到端字段恒为 null）";
    } else {
      r.probe_source = "cue-probe（harness 托管，端到端）";
    }
    if (!any_probe) {
      r.e2e_null_reason = cfg_.endpoint.empty() && cfg_.replay_tools_dir.empty()
                              ? "该 run 没有任何探针样本（probe_unavailable 会记在 run_error 里）"
                              : "该 run 的后端没有探针（假域/回放没有端到端数据）";
    } else if (!has_value(r.p99_ms_before)) {
      r.e2e_null_reason = "注入前窗口没有探针样本（故障随管线启动即生效），修复后 p99 见右侧";
    }
  }

  // 失败模式分布（§8.4）：按 expect.root_cause 归组。运行状态已经进 run_notes，
  // 这里只剩"哪一类根因判分没命中"，与"这次 run 有没有跑完"是两件事。
  r.failure_modes = summarize_failure_modes(cases, r.cases);
  // 稳定性（§8.4）：按 repeat 轮聚合三级率与逐轮通过率；少于 2 轮有样本时
  // informative()=false，报告里必须标注"无稳定性信息"，不能让 0 方差看起来像"很稳"。
  r.stability = compute_stability(r.cases, cfg_.repeat);
}

std::vector<FailureMode> summarize_failure_modes(const std::vector<Case>& cases,
                                                 const std::vector<CaseOutcome>& outcomes) {
  std::map<std::string, const Case*> by_id;
  for (const auto& c : cases) by_id[c.id] = &c;

  struct Acc {
    int miss = 0;
    std::vector<std::string> examples;
  };
  std::map<std::string, Acc> groups;

  const auto add_example = [](Acc& a, const std::string& ex) {
    if (a.examples.size() < kMaxFailureExamples) a.examples.push_back(ex);
  };

  for (const auto& o : outcomes) {
    if (!o.scored) {
      // run_error = 没有判分结果，不是"答错"。单独一组，语义上不能并进某个根因。
      auto& a = groups[kRunErrorGroup];
      ++a.miss;
      add_example(a, o.id + "(run_error)");
      continue;
    }
    std::vector<std::string> layers;
    if (!o.score.root) layers.push_back("root");
    if (!o.score.evidence) layers.push_back("evidence");
    if (!o.score.sla) layers.push_back("sla");
    // 三维全中 = 这一条没过判分；一次尝试只计 1 次 miss（不是按层计 3 次）：
    // 失败模式分布问的是"这类故障失败了几次"，按层计数会让失败率超过尝试数。
    if (layers.empty()) continue;
    const auto it = by_id.find(o.id);
    const std::string key = (it != by_id.end() && !it->second->expect_root_cause.empty())
                                ? it->second->expect_root_cause
                                : std::string(kUnknownRootGroup);
    auto& a = groups[key];
    ++a.miss;
    std::string ex = o.id + "(";
    for (std::size_t i = 0; i < layers.size(); ++i) ex += (i == 0 ? "" : ",") + layers[i];
    ex += ")";
    add_example(a, ex);
  }

  std::vector<FailureMode> out;
  for (auto& [root, a] : groups) {
    FailureMode fm;
    fm.root_cause = root;
    fm.miss_count = a.miss;
    fm.examples = std::move(a.examples);
    out.push_back(std::move(fm));
  }
  // 降序：报告 TOP3 与 JSON 都按这个顺序读，出现并列时按根因名字稳定排序
  std::sort(out.begin(), out.end(), [](const FailureMode& x, const FailureMode& y) {
    if (x.miss_count != y.miss_count) return x.miss_count > y.miss_count;
    return x.root_cause < y.root_cause;
  });
  return out;
}

Stability compute_stability(const std::vector<CaseOutcome>& outcomes, int repeat) {
  Stability s;
  s.repeat = repeat > 0 ? repeat : 1;

  struct Round {
    int scored = 0;
    int root = 0, evidence = 0, sla = 0, all = 0;
  };
  std::map<int, Round> rounds;
  for (const auto& o : outcomes) {
    if (!o.scored) continue;  // run_error 不计入分母，也不占轮次样本
    auto& rd = rounds[o.attempt];
    ++rd.scored;
    if (o.score.root) ++rd.root;
    if (o.score.evidence) ++rd.evidence;
    if (o.score.sla) ++rd.sla;
    if (o.score.all()) ++rd.all;
  }

  // 每一轮一个率；没有 scored 样本的轮（整轮 run_error / 被截断）不计入，
  // 否则会把"没测到"当成"0% 通过"，与不拿 0 冒充实测值的口径矛盾。
  std::vector<double> root, evidence, sla, pass;
  for (const auto& [k, rd] : rounds) {
    (void)k;
    if (rd.scored == 0) continue;
    const double n = double(rd.scored);
    root.push_back(double(rd.root) / n);
    evidence.push_back(double(rd.evidence) / n);
    sla.push_back(double(rd.sla) / n);
    pass.push_back(double(rd.all) / n);
  }

  // 总体标准差（除以 n）：本轮这几轮就是全部观测，不做总体推断；repeat=3 时除以 n-1
  // 会把离散度放大 1.41 倍，读起来比实际抖。repeat=1 时 n=1 → stddev 恒为 0。
  const auto stats = [](const std::vector<double>& v) {
    StabilityStats st;
    if (v.empty()) return st;  // 全 NaN：没有跨轮样本
    double sum = 0, lo = v[0], hi = v[0];
    for (double x : v) {
      sum += x;
      lo = std::min(lo, x);
      hi = std::max(hi, x);
    }
    const double mean = sum / double(v.size());
    double var = 0;
    for (double x : v) var += (x - mean) * (x - mean);
    st.mean = mean;
    st.stddev = std::sqrt(var / double(v.size()));
    st.min = lo;
    st.max = hi;
    return st;
  };
  s.rounds = static_cast<int>(root.size());
  s.root_rate = stats(root);
  s.evidence_rate = stats(evidence);
  s.sla_rate = stats(sla);
  s.pass_k = stats(pass);
  return s;
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
  if (!cfg_.record_tools_dir.empty()) {
    o << "- 工具观测: 录制到 `" << cfg_.record_tools_dir << "`（每个 case 一份 <case_id>.tools.jsonl）\n";
  }
  if (!cfg_.replay_tools_dir.empty()) {
    o << "- 工具观测: 回放 `" << cfg_.replay_tools_dir << "`，命中 " << r.tool_hits
      << " 次，miss " << r.tool_misses << " 次\n";
  }
  if (cfg_.agent_mode == "real") {
    o << "- LLM: `" << cfg_.llm_mode << "`，cassette=`" << cfg_.cassette_dir << "`";
    if (cfg_.llm_mode == "replay") {
      o << "，命中 " << r.cassette_hits << " 次，miss " << r.cassette_misses << " 次";
    } else if (cfg_.record) {
      o << "（录制到该目录）";
    }
    o << "\n";
  }
  if (cfg_.producer != "none" && !cfg_.producer.empty()) {
    // 产帧源（N-23）：不写清楚是哪个源，rate 类 case 的 budget_drops 就看不出可不可信。
    o << "- 产帧源: `" << cfg_.producer << "`（" << cfg_.capture_bin << " --backend " << cfg_.producer
      << "，" << cfg_.producer_size << "@" << cfg_.producer_bitrate << " kbps，30 fps）\n";
  }
  o << "- 用例: 计划 " << r.planned << "，执行 " << r.executed << "，计分 " << r.scored
    << "，运行失败 " << r.run_errors << "\n";
  o << "- 成本: ¥" << r.cost_yuan << "\n\n";

  if (cfg_.agent_mode == "scripted") {
    o << "> **scripted 模式 = 引擎自检，不判模型**：结论与观测由 case 的 expect 构造，"
         "100% 通过只证明「判分器 + 引擎」链路通。要评模型能力请用 `--mode real`。\n\n";
  }
  if (!cfg_.replay_tools_dir.empty()) {
    o << "> **工具回放**：管线调用来自 `" << cfg_.replay_tools_dir
      << "` 的录制，不 spawn 后端、不连网络。回放证明的是**评测链路的确定性**"
         "（同一份观测必然得到同一判定），不是「真实系统多次跑出同样数值」——"
         "真管线的观测每轮都有抖动，那正是需要录制的原因。\n\n";
  }
  if (cfg_.agent_mode == "real" && cfg_.llm_mode == "replay") {
    o << "> **replay 模式**：LLM 响应来自 cassette（usage 照抄录制值），零网络、零成本；"
         "缺录制时记 `run_error=cassette_miss`，不进成功率分母。要跑真实模型用 `--live`。\n\n";
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

  // 稳定性（§8.4）：repeat>1 时给每级的 mean/stddev/min/max；repeat=1 时必须明说
  // "无稳定性信息"——1 次运行的 stddev 恒为 0，那个 0 不是"稳定"，是"没测"。
  {
    const auto& st = r.stability;
    o << "- 稳定性: repeat=" << st.repeat;
    if (!st.informative()) {
      o << "，**无稳定性信息**（只有 " << st.rounds
        << " 轮有可计分样本，跨轮离散度不存在；stddev 记 0 不代表稳定）\n";
    } else {
      const auto stat_text = [](const StabilityStats& s2) {
        if (!has_value(s2.mean)) return std::string("n/a（该轮没有可计分样本）");
        return "mean " + num_text(s2.mean, 3) + "，stddev " + num_text(s2.stddev, 3) + "，min " +
               num_text(s2.min, 3) + "，max " + num_text(s2.max, 3);
      };
      o << "（总体标准差 = 除以轮数 n，不是样本标准差的 n−1）\n";
      o << "  - root_rate: " << stat_text(st.root_rate) << "\n";
      o << "  - evidence_rate: " << stat_text(st.evidence_rate) << "\n";
      o << "  - sla_rate: " << stat_text(st.sla_rate) << "\n";
      o << "  - 逐轮通过率（该轮三级全过的 case 占比；与 pass^" << r.repeat
        << " 不是同一个量）: " << stat_text(st.pass_k) << "\n";
    }
    o << "\n";
  }

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

  // 端到端前后对比：这是 T-03 要的那张表。三列窗口的定义写在表头下的说明里，
  // 因为「注入前」在 Process 后端通常不存在（故障随 cued 启动即生效），只给 before/after
  // 会让读者以为数据缺了一半。
  o << "## 端到端指标前后对比\n\n";
  o << "> 数据源：" << md_escape(r.probe_source)
    << "。端到端 = probe 收帧时刻（换算到 cued 时钟域）− 帧 PTS；只有探针样本能证明它，"
       "cued 的 fps / 六段分位**不参与**这一节。\n";
  o << "> 窗口：注入前 = 故障注入之前；故障窗口 = 注入后、修复前的 agent 观察期；"
       "修复后 = harness 在 agent 结束后重测的窗口。单位 ms，`null` = 该窗口没有探针样本"
       "（不是 0）。\n";
  // run 级汇总放在这一节（而不是塞进"分层成功率"表）：那张表是成功率口径，
  // 混入延迟数字会让控制台的 rate 面板把它当成一项"成功率"。
  o << "\n- 端到端 p99 修复前（各 case 中位）: " << e2e_text(r.p99_ms_before) << "\n";
  o << "- 端到端 p99 修复后（各 case 中位）: " << e2e_text(r.p99_ms_after) << "\n";
  if (!r.e2e_null_reason.empty()) {
    o << "- null 原因: " << md_escape(r.e2e_null_reason) << "\n";
  }
  o << "\n";
  o << "| case | 注入前 p99 | 故障窗口 p99 | 修复后 p99 | 卡顿(窗口) | 最长卡顿 ms | 说明 |\n";
  o << "|---|---|---|---|---|---|---|\n";
  for (const auto& c : r.cases) {
    if (!c.scored) {
      o << "| " << md_escape(c.id)
        << " | null | null | null | — | — | run_error: " << md_escape(short_text(c.error_kind))
        << " |\n";
      continue;
    }
    const auto& pr = c.metrics.probe;
    o << "| " << md_escape(c.id) << " | " << e2e_text(c.p99_ms_before) << " | "
      << e2e_text(c.p99_ms_fault) << " | " << e2e_text(c.p99_ms_after) << " | "
      << (pr.samples > 0 ? std::to_string(pr.stalls) : std::string("null")) << " | "
      << e2e_text(pr.max_stall_ms) << " | " << md_escape(c.e2e_null_reason) << " |\n";
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
  // 注入有效性（N-23）：故障有没有真的发生。放在逐用例之后、失败模式之前——
  // 它回答的是"这次 run 的证据链成不成立"，比"模型答错了哪一类"更靠前。
  //
  // 为什么单独一节而不是塞进逐用例表：一张表里同时有 ✓/✗/— 三种语义会读错；
  // 而且"故障没发生"与"模型答错"是两件事，混在一起会让失败率失去含义。
  {
    std::vector<const CaseOutcome*> inj;
    for (const auto& c : r.cases) {
      if (!c.injection.fault_kind.empty() && c.injection.fault_kind != "none") inj.push_back(&c);
    }
    if (!inj.empty()) {
      o << "\n## 注入有效性（故障有没有真的发生）\n\n";
      o << "> 口径：`rate` 判 `budget_drops > 0`（令牌桶被抽干），`drop` / `ingress_drop` 判 "
           "`injected > 0`（真的有包被注入吃掉）；`backlog` / `delay` / `jitter` 没有单一计数"
           "判据，**未做自动判定**。注入未生效的 case 不改它的 scored 与三级判定（故障有没有"
           "发生与模型答得对不对是两件事，不进成功率分母），但整轮 run 不会以退出码 0 收场："
           "这种 case 的 SLA 是恒真的，给绿灯等于把假通过当结论。\n\n";
      o << "| case | 故障类 | 判定 | 依据 |\n|---|---|---|---|\n";
      for (const auto* c : inj) {
        const auto& chk = c->injection;
        const std::string verdict = chk.checked ? (chk.effective ? "生效" : "**未生效**")
                                                : std::string("未判定");
        std::string detail = chk.detail;
        if (detail.empty()) {
          detail = c->scored ? "该故障类没有单一计数判据"
                             : "run_error（未取到故障窗口读数）: " + short_text(c->error_kind);
        }
        o << "| " << md_escape(c->id) << " | " << md_escape(chk.fault_kind) << " | " << verdict
          << " | " << md_escape(detail) << " |\n";
      }
      if (r.injection_ineffective > 0) {
        o << "\n**" << r.injection_ineffective
          << " 条注入未生效**：这些 case 的故障窗口读数为 0，SLA（fps 等）因此恒真。"
             "要么换一个有真实码率的产帧源（`--producer lavfi`），要么把注入档位调到"
             "源码率能被打穿的水平——总之不能拿这份结果当通过。\n";
      } else {
        o << "\n本次所有可判定的注入类 case 都真的发生了故障。\n";
      }
    }
  }

  // 失败模式 TOP3（§8.9 ③）：按 miss_count 降序取前三，每类附 examples 里的 1 个具体 case。
  // 运行状态（run_notes）在下面单独列：把"这次没跑完"混进失败分布，图会看起来像模型答错。
  o << "\n## 失败模式 TOP3\n\n";
  o << "> 口径：一次 case 尝试里 root / evidence / sla 任一未命中即计 1 次 miss"
       "（三维全中不计；一次尝试只计 1，不按层计 3 次），按 case 的 `expect.root_cause` 归组；"
       "`__run__` = 没有判分结果（run_error），它不是答错，单列以便与判分未命中区分。"
       "examples 括号内是该次尝试未命中的层。\n\n";
  if (r.failure_modes.empty()) {
    o << "本次没有判分未命中，也没有 run_error：失败模式为空。\n";
  } else {
    const std::size_t top = std::min<std::size_t>(3, r.failure_modes.size());
    o << "| # | 根因 | miss | 例子 |\n|---|---|---|---|\n";
    for (std::size_t i = 0; i < top; ++i) {
      const auto& f = r.failure_modes[i];
      std::string name = f.root_cause.empty() ? std::string(kRunErrorGroup) : f.root_cause;
      if (name == kRunErrorGroup) name += "（运行错误，未判分）";
      const std::string ex = f.examples.empty() ? std::string("—") : f.examples.front();
      o << "| " << (i + 1) << " | " << md_escape(name) << " | " << f.miss_count << " | `"
        << md_escape(ex) << "` |\n";
    }
    if (r.failure_modes.size() > top) {
      o << "\n共 " << r.failure_modes.size() << " 类失败模式，此处只列前 " << top
        << " 类（完整分布见 report.json 的 `failure_modes`）。\n";
    }
  }
  if (!r.run_notes.empty()) {
    o << "\n**运行状态备注（不是失败模式，不计入上面任何一类）**：\n\n";
    for (const auto& n : r.run_notes) o << "- " << md_escape(n) << "\n";
  }

  // 回归清单（§8.9 ④）：没有 baseline 时明确写"跳过"，不留空小节——
  // 空小节会被读成"没有回归"，而实际是"没比过"。
  o << "\n## 回归清单\n\n";
  const auto& g = r.regression;
  if (!g.present) {
    o << "本次没有 baseline，跳过（同一个 suite 只跑过这一次时属正常）；"
         "要对比可加 `--baseline <run_id>` 指定对照 run，或先跑一次同 suite 的 run。\n";
  } else {
    o << "> baseline: `" << g.baseline_run_id << "`（来源："
      << (g.source == "explicit" ? "显式 `--baseline`" : "自动发现上次同 suite / mode / repeat 的 run")
      << "）\n";
    o << "> 口径：run_error 不是失败，单列「不可判定」不计入新增失败；"
         "指纹/suite/mode/repeat 不同时差异不可归因，先对齐配置再比。\n";
    if (!g.comparable) {
      o << "\n> **两次 run 不可比**：差异不能归因于被测对象的改动。\n";
      for (const auto& w : g.warnings) o << "> - " << md_escape(w) << "\n";
    }
    o << "\n- 新增失败 " << g.new_failures.size() << "，已修复 " << g.fixed.size() << "，仍失败 "
      << g.still_failing.size() << "，仍通过 " << g.still_passing << "，不可判定 " << g.inconclusive;
    if (g.only_a > 0 || g.only_b > 0) {
      o << "，仅 baseline 有 " << g.only_a << "，仅本次有 " << g.only_b;
    }
    o << "\n";
    const auto list = [&o](const char* title, const std::vector<RegressionEntry>& v) {
      if (v.empty()) return;
      o << "\n### " << title << "\n\n";
      for (const auto& e : v) o << "- `" << md_escape(e.id) << "`：" << md_escape(e.detail) << "\n";
    };
    list("新增失败（回归）", g.new_failures);
    list("已修复", g.fixed);
    list("仍失败", g.still_failing);
    if (g.new_failures.empty() && g.fixed.empty() && g.still_failing.empty()) {
      o << "\n本次相对 baseline 没有新增失败、没有已修复、没有仍失败。\n";
    }
    if (g.only_a > 0 || g.only_b > 0) {
      o << "\n> case 集有变动（仅 baseline 有 " << g.only_a << "，仅本次有 " << g.only_b
        << "）：suite 改名/增删会让公共部分的 diff 失去意义。\n";
    }
  }
  // 注入 trace（N-11）：只有注入类 case 才有，逐 case 列出落点——读者要能直接找到
  // 文件去 diff 同 seed 的两次 run，而不是去翻控制台日志。
  {
    std::vector<const CaseOutcome*> traced;
    for (const auto& c : r.cases) {
      if (!c.inject_trace.empty()) traced.push_back(&c);
    }
    if (!traced.empty()) {
      o << "\n> 注入 trace（" << traced.size()
        << " 个注入类 case 的逐包判定；同一 suite 同 seed 的两次 run 可逐行 diff，"
           "行数随推流时长变化，前缀一致即判定序列一致）：\n";
      for (const auto* c : traced) {
        o << "> - `" << md_escape(c->id) << "` → `" << md_escape(c->inject_trace) << "`\n";
      }
    }
  }
  o << "\n> 说明：延迟与成功率均为本机实测；跨机场景下的端到端延迟为相对量，不含最小网络传播时间。\n";
  o << "> 判分口径：SLA 由 harness 在 agent 结束后重测（不采信 agent 自报）；"
       "run_error 不计入成功率分母。\n";
  o << "> SLA 重测口径：取 agent 结束后最后 3 条采样（约 1 s 一条）的中位 fps；"
       "dropped 取窗口增量而不是进程累计值（累计值随 cued 重启清零）。\n";
  o << "> 分位口径：六段（recv/buffer/decode/op/encode/send）的 p50/p95/p99 都由 cued 的"
       "统计行采集、经 `PipelineMetrics` 全量搬运到本报告；某格写 n/a 只表示**该段在该重测"
       "窗口没有样本**（典型：未开转码时 encode 段为空；某段整段输出 null），不补 0。\n";
  o << "> 端到端口径：`case.expect.sla` 的 lat_p50_ms / lat_p99_ms / max_stall_ms / stalls "
       "由 harness 用探针（cue-probe）重测；探针不可用时该 case 记 "
       "`run_error=probe_unavailable`，不会退回 cued 指标判分。\n";
  return o.str();
}

std::string Engine::render_json(const RunReport& r) const {
  // 与 render_markdown 同一份数据、同一套 null 口径：NaN（没测到）→ null，绝不写 0。
  // 为什么单独给 report.json：report.md 是给人读的摘要，机器消费（回归脚本、看板）
  // 需要稳定字段名，从 Markdown 里抠表格是注定要坏的耦合。
  auto num = [](double v) { return has_value(v) ? json(v) : json(nullptr); };
  auto lat = [&num](const Latency& l) {
    return json{{"p50", num(l.p50)}, {"p95", num(l.p95)}, {"p99", num(l.p99)}};
  };
  const auto stat_json = [&num](const StabilityStats& s) {
    return json{{"mean", num(s.mean)},
                {"stddev", num(s.stddev)},
                {"min", num(s.min)},
                {"max", num(s.max)}};
  };
  // 失败模式：对象数组（§8.4），不再是字符串数组——字符串数组没法机器消费。
  json failure_modes = json::array();
  for (const auto& f : r.failure_modes) {
    failure_modes.push_back(json{{"root_cause", f.root_cause},
                                 {"miss_count", f.miss_count},
                                 {"examples", f.examples}});
  }
  json regression = json{{"present", r.regression.present},
                         {"baseline_run_id",
                          r.regression.baseline_run_id.empty() ? json(nullptr)
                                                               : json(r.regression.baseline_run_id)},
                         {"source", r.regression.source},
                         {"comparable", r.regression.comparable},
                         {"warnings", r.regression.warnings},
                         {"new_failures", json::array()},
                         {"fixed", json::array()},
                         {"still_failing", json::array()},
                         {"still_passing", r.regression.still_passing},
                         {"inconclusive", r.regression.inconclusive},
                         {"only_a", r.regression.only_a},
                         {"only_b", r.regression.only_b}};
  for (const auto& e : r.regression.new_failures) {
    regression["new_failures"].push_back(json{{"id", e.id}, {"detail", e.detail}});
  }
  for (const auto& e : r.regression.fixed) {
    regression["fixed"].push_back(json{{"id", e.id}, {"detail", e.detail}});
  }
  for (const auto& e : r.regression.still_failing) {
    regression["still_failing"].push_back(json{{"id", e.id}, {"detail", e.detail}});
  }
  json root{
      {"run_id", r.run_id},
      {"fatal_error", r.fatal_error.empty() ? json(nullptr) : json(r.fatal_error)},
      {"suite", cfg_.suite_path},
      {"agent_mode", cfg_.agent_mode},
      {"llm_mode", cfg_.llm_mode},
      {"repeat", r.repeat},
      {"fingerprint", r.fingerprint},
      {"cued_bin", cfg_.cued_bin},
      {"planned", r.planned},
      {"executed", r.executed},
      {"scored", r.scored},
      {"run_errors", r.run_errors},
      {"gated", r.gated},
      {"truncated", r.truncated},
      // 注入未生效（N-23）：不进成功率分母，但 >0 的 run 不能给绿灯
      {"injection_ineffective", r.injection_ineffective},
      // 产帧源：默认 none（不起任何进程）；consumer 据此判断 rate 类的读数可不可信
      {"producer", cfg_.producer},
      {"producer_size", cfg_.producer_size},
      {"producer_bitrate", cfg_.producer_bitrate},
      {"root_rate", r.root_rate},
      {"evidence_rate", r.evidence_rate},
      {"sla_rate", r.sla_rate},
      {"pass_k", r.pass_k},
      {"cost_yuan", r.cost_yuan},
      {"mttr_avg_s", num(r.mttr_avg_s)},
      {"cassette_hits", r.cassette_hits},
      {"cassette_misses", r.cassette_misses},
      {"tool_hits", r.tool_hits},
      {"tool_misses", r.tool_misses},
      // 端到端前后对比（run 级中位）：null 时 e2e_null_reason 说明原因
      {"p99_ms_before", num(r.p99_ms_before)},
      {"p99_ms_after", num(r.p99_ms_after)},
      {"e2e_null_reason", r.e2e_null_reason.empty() ? json(nullptr) : json(r.e2e_null_reason)},
      {"probe_source", r.probe_source},
      // 稳定性（§8.4）：repeat=1 时 stddev=0 且 informative=false，consumer 不能被 0 骗
      {"stability", json{{"repeat", r.stability.repeat},
                         {"rounds", r.stability.rounds},
                         {"informative", r.stability.informative()},
                         {"stddev_kind", "population"},
                         {"root_rate", stat_json(r.stability.root_rate)},
                         {"evidence_rate", stat_json(r.stability.evidence_rate)},
                         {"sla_rate", stat_json(r.stability.sla_rate)},
                         {"pass_k", stat_json(r.stability.pass_k)}}},
      // 运行状态与失败模式分属两个字段（§8.4）：混在一起会让失败分布失真
      {"failure_modes", failure_modes},
      {"run_notes", r.run_notes},
      // 回归清单（§8.9 ④）：present=false 时其余字段留空，consumer 不必猜
      {"regression", regression},
  };
  json cases = json::array();
  for (const auto& c : r.cases) {
    json cj{
        {"id", c.id},
        {"scored", c.scored},
        {"error_kind", c.error_kind},
        {"turns", c.turns},
        {"cost_yuan", c.cost_yuan},
        {"root_cause", c.diagnosis.root_cause},
        {"final_stage", c.final_stage},
        {"final_fps", c.final_fps},
        {"final_dropped", c.final_dropped},
        {"final_samples", c.final_samples},
        {"mttr_s", num(c.mttr_s)},
        {"p99_ms_before", num(c.p99_ms_before)},
        {"p99_ms_fault", num(c.p99_ms_fault)},
        {"p99_ms_after", num(c.p99_ms_after)},
        {"e2e_null_reason", c.e2e_null_reason.empty() ? json(nullptr) : json(c.e2e_null_reason)},
        // 注入 trace 的落点：没有 trace 的 case 写 null（"没有这个文件"不是空路径）
        {"inject_trace", c.inject_trace.empty() ? json(nullptr) : json(c.inject_trace)},
        // 注入是否真的生效（N-23）：checked=false 的 case（backlog/delay/jitter 或没起管线）
        // 写 null，不写 false——"没判"与"判了没生效"必须能分开。
        {"injection", c.injection.fault_kind.empty()
                          ? json(nullptr)
                          : json{{"fault_kind", c.injection.fault_kind},
                                 {"checked", c.injection.checked},
                                 {"effective", c.injection.checked ? json(c.injection.effective)
                                                                   : json(nullptr)},
                                 {"budget_drops", c.injection.budget_drops},
                                 {"injected", c.injection.injected},
                                 {"detail", c.injection.detail.empty() ? json(nullptr)
                                                                       : json(c.injection.detail)}}},
    };
    if (c.scored) {
      cj["score"] = json{{"root", c.score.root},
                         {"evidence", c.score.evidence},
                         {"sla", c.score.sla}};
    } else {
      cj["score"] = nullptr;
    }
    cj["metrics"] = json{{"fps", c.metrics.fps},
                         {"frames_in", c.metrics.frames_in},
                         {"frames_out", c.metrics.frames_out},
                         {"dropped", c.metrics.dropped},
                         {"samples", c.metrics.samples},
                         {"recv", lat(c.metrics.recv)},
                         {"buffer", lat(c.metrics.buffer)},
                         {"decode", lat(c.metrics.decode)},
                         {"op", lat(c.metrics.op)},
                         {"encode", lat(c.metrics.encode)},
                         {"send", lat(c.metrics.send)}};
    const auto& pr = c.metrics.probe;
    cj["probe"] = json{{"alive", pr.alive},
                       {"samples", pr.samples},
                       {"fps", pr.fps},
                       {"frames", pr.frames},
                       {"decoded", pr.decoded},
                       {"decode_errors", pr.decode_errors},
                       {"stalls", pr.stalls},
                       {"max_stall_ms", num(pr.max_stall_ms)},
                       {"synced", pr.synced},
                       {"offset_us", pr.offset_us},
                       {"e2e", lat(pr.e2e)},
                       {"recv_us", lat(pr.recv_us)},
                       {"reassembly_us", lat(pr.reassembly_us)},
                       {"buffer_us", lat(pr.buffer_us)},
                       {"decode_us", lat(pr.decode_us)},
                       {"unavailable_reason",
                        pr.unavailable_reason.empty() ? json(nullptr)
                                                      : json(pr.unavailable_reason)}};
    cases.push_back(std::move(cj));
  }
  root["cases"] = std::move(cases);
  return root.dump(2) + "\n";
}

// 终端摘要（FR-EVAL.10）：cue-eval 结束后的 10-15 行。纯函数——只读 RunReport 与 cfg_，
// 不碰文件系统，所以可以快照测试（"摘要里到底写了什么"不该靠跑一次真管线来验证）。
std::string Engine::render_terminal_summary(const RunReport& r) const {
  const auto fmt = [](double v, int prec) {
    return has_value(v) ? num_text(v, prec) : std::string("n/a");
  };
  std::ostringstream o;
  o << "==================== Cue 评测摘要 ====================\n";
  o << "run_id  : " << r.run_id << "\n";
  o << "suite   : " << cfg_.suite_path << "\n";
  o << "模式    : " << cfg_.agent_mode;
  if (cfg_.agent_mode == "real") o << "（LLM=" << cfg_.llm_mode << "）";
  o << "，repeat=" << r.repeat << "\n";
  if (cfg_.producer != "none" && !cfg_.producer.empty()) {
    o << "产帧源  : " << cfg_.producer << "（" << cfg_.producer_size << "@"
      << cfg_.producer_bitrate << " kbps）\n";
  }
  if (!r.fatal_error.empty()) {
    // 没有产出结果时也走摘要：先让终端读者看到"这次没结果"，再给原因
    o << "结果    : **未产出**（" << r.fatal_error << "）\n";
    o << "报告    : 未写入（失败不覆盖已有 report.md）\n";
    o << "=====================================================\n";
    return o.str();
  }
  o << "用例    : 执行 " << r.executed << " / 计划 " << r.planned << "，计分 " << r.scored
    << "，run_error " << r.run_errors << (r.truncated ? "，已截断" : "") << "\n";
  o << "成功率  : 根因 " << fmt(r.root_rate * 100, 1) << "% | 证据 " << fmt(r.evidence_rate * 100, 1)
    << "% | SLA " << fmt(r.sla_rate * 100, 1) << "%\n";
  o << "pass^" << r.repeat << "  : " << fmt(r.pass_k * 100, 1) << "%\n";
  o << "稳定性  : ";
  if (!r.stability.informative()) {
    // stddev 恒为 0：写出来会被读成"很稳"，这里必须说清是"没测"
    o << "repeat=" << r.stability.repeat << "，只有 " << r.stability.rounds
      << " 轮有样本，无稳定性信息";
  } else {
    const auto& st = r.stability;
    o << "root " << fmt(st.root_rate.mean, 3) << "±" << fmt(st.root_rate.stddev, 3) << "，evidence "
      << fmt(st.evidence_rate.mean, 3) << "±" << fmt(st.evidence_rate.stddev, 3) << "，sla "
      << fmt(st.sla_rate.mean, 3) << "±" << fmt(st.sla_rate.stddev, 3) << "（总体标准差）";
  }
  o << "\n";
  // 注入未生效（N-23）：默认（0 条）不打印，既有的摘要快照因此一字不变；
  // 一旦出现就必须出现在摘要里——它让整轮 run 不是绿灯，读者得先知道原因。
  if (r.injection_ineffective > 0) {
    o << "注入    : **" << r.injection_ineffective
      << " 条未生效**（故障没发生 → SLA 恒真；不计入成功率分母，但本次不能算通过）\n";
  }
  o << "成本    : ¥" << fmt(r.cost_yuan, 4) << "（闸门 ¥" << fmt(cfg_.max_yuan, 2) << "，单价 ¥"
    << fmt(cfg_.yuan_per_mtok, 2) << "/Mtok 估算）\n";
  o << "MTTR    : " << (has_value(r.mttr_avg_s) ? fmt(r.mttr_avg_s, 2) + " s" : std::string("n/a"))
    << "\n";
  // 指纹前 12 位：够人眼比对、又不至于把终端刷爆（完整值在 report.md / report.json 里）
  o << "指纹    : " << (r.fingerprint.size() > 12 ? r.fingerprint.substr(0, 12) : r.fingerprint)
    << "\n";
  o << "报告    : " << (r.report_path.empty() ? std::string("(未落盘)") : r.report_path);
  if (!r.report_json_path.empty()) o << "（JSON: " << r.report_json_path << "）";
  o << "\n";
  o << "=====================================================\n";
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
