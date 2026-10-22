// FR-EVAL.5 的矩阵展开实现。
//
// 一条设计主线：**生成器不做物理预测，只做能证明的结构判断**。
//  - 「这个组合能不能跑」里可以证明的部分（单位/量纲、transcode 关闭时编码器轴无效、
//    丢包率上限、延迟必须越过 SLA 阈值、修复动作必须能把底子拉回达标）在这里拒绝；
//  - 「故障态 fps 具体是多少」不可预测（依赖 x264 与本机负载），不在生成器里臆测：
//    每个根因的模板与实测口径来自 base suite，生成器原样继承它的 SLA 与证据候选。
#include "cue/eval/generator.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "cue/eval/eval.h"

namespace cue::eval {
namespace {

using ojson = nlohmann::ordered_json;

// 合成推流的源帧率（scripts/test-client.mjs 固定 30 fps，单分片帧）。
// 上行丢包率 p 下，帧到达率上界就是 30*(1-p)：这是 ingress_drop 的可判性判据来源。
constexpr double kSourceFps = 30.0;
// 故障态与阈值之间至少留的 fps 余量。与验收脚本（scripts/test-real-faults.sh）同口径：
// 1 s 窗口读数本身有 ±30% 的抖动，余量小于 3 fps 的 case 会变成 flaky。
constexpr double kSlaMarginFps = 3.0;
// 延迟/抖动注入必须越过声明的 p99 阈值至少这么多毫秒，否则故障窗口里 SLA 仍然成立，
// "修不修都过"的 case 没有判分意义。
constexpr double kDelayMarginMs = 30.0;
// 本机实测带（2 vCPU、合成源、720p@4000kbps veryfast）：令牌桶按实际发送字节发放，
// 0.1 Mbps 时 fps 约 12（每帧约 1 KB）。限速值超过 0.25 Mbps 后 fps 会回到 25 以上，
// 故障态不再跌破阈值——超过这个上界的组合不可判，直接拒绝而不是生成一个假故障。
constexpr double kRateCapMaxMbps = 0.25;
// cued 的 --send-mbps 参数下限是 0.1（apps/cued.cpp 的 parse_double(0.1, 10000.0)）。
// 更小的值不是"更严重的限速"，而是 cued 直接退出 2、harness 反复重启后拿不到媒体流
// （实测 0.05/0.06 Mbps 全是 crash-loop）。这条是结构约束，与判据阈值无关。
constexpr double kRateCapMinMbps = 0.1;
// 展开规模上限：矩阵写错（例如 8 轴 × 20 值）会让展开在内存里爆炸。
constexpr std::size_t kMaxCombinations = 100000;

const std::set<std::string> kFaultKinds = {"none", "drop", "ingress_drop", "rate",
                                           "backlog", "delay", "jitter"};
const std::set<std::string> kPresets = {"ultrafast", "superfast", "veryfast", "faster", "fast",
                                        "medium",    "slow",      "slower",   "veryslow", "placebo"};
// 故障类 case（修复动作只有「清除注入」）的底子必须自己达标：720p 及以下 + 快档。
// 实测：720p veryfast encode_p50≈6.4 ms（帧预算 33 ms）；1600x900 placebo≈108 ms、
// 4K veryfast≈55 ms——清除注入后仍然达不到 fps_min=25，这类组合不成立。
const std::set<std::string> kSlaSafePresets = {"ultrafast", "superfast", "veryfast"};
constexpr long long kSlaSafePixels = 1280LL * 720LL;

const std::set<std::string> kKnownAxes = {
    "fault.kind",         "fault.value",      "pipeline.transcode", "pipeline.preset",
    "pipeline.size",      "pipeline.bitrate_kbps", "pipeline.gop",  "expect.root_cause"};
// 只有 transcode=on 时才会被读到的轴：转码关闭时它们不改变任何可观测行为，
// 放进来会生成「看起来有参数、实际没生效」的 case。
const std::set<std::string> kEncoderAxes = {"pipeline.preset", "pipeline.size",
                                            "pipeline.bitrate_kbps", "pipeline.gop"};

struct Axis {
  std::string path;
  std::vector<ojson> values;
};

struct Plan {
  std::vector<Axis> axes;
  // 根因 → 允许的 fault.kind。既是「根因从 kind 反查」的数据源，也是合法性规则的数据源。
  std::map<std::string, std::set<std::string>> expect;
};

struct Matrix {
  std::string suite_name;  // 空 = 用矩阵文件名
  std::string base_path;
  std::uint64_t seed_lo = 40;
  std::uint64_t seed_hi = 80;
  std::size_t limit = 0;  // 0 = 全部
  std::vector<Plan> groups;
};

struct Entry {
  ojson case_json;
  std::string root;
  std::string id;
};

std::string join(const std::vector<std::string>& parts, const char* sep) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) out += sep;
    out += parts[i];
  }
  return out;
}

bool is_string_scalar(const ojson& v) { return v.is_string(); }
bool is_number_scalar(const ojson& v) { return v.is_number(); }
bool is_bool_scalar(const ojson& v) { return v.is_boolean(); }

// 人读的数值（拒绝原因里用）：保留小数点。
std::string fmt_number(double v) {
  std::ostringstream os;
  os.precision(6);
  os << v;
  std::string s = os.str();
  if (!s.empty() && s.find('.') != std::string::npos) {
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
  }
  return s.empty() ? "0" : s;
}

// id 用的数值：点换 p（id 会进文件名 <case_id>.tools.jsonl，少一类特殊字符更稳）。
std::string trim_number(double v) {
  std::string s = fmt_number(v);
  std::replace(s.begin(), s.end(), '.', 'p');
  return s;
}

bool parse_size(const std::string& s, int& w, int& h) {
  const auto x = s.find('x');
  if (x == std::string::npos || x == 0 || x + 1 >= s.size()) return false;
  try {
    const int pw = std::stoi(s.substr(0, x));
    const int ph = std::stoi(s.substr(x + 1));
    if (pw < 16 || pw > 4096 || ph < 16 || ph > 4096) return false;
    if (pw % 2 != 0 || ph % 2 != 0) return false;
    w = pw;
    h = ph;
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

// 根因短名：只用于 id 的可读性与消歧（kind=none 时它就是唯一的类别 token）。
std::string root_short(const std::string& root) {
  static const std::map<std::string, std::string> kShort = {
      {"network_loss", "loss"},
      {"network_delay", "delay"},
      {"bandwidth_insufficient", "bw"},
      {"queue_backlog", "queue"},
      {"encoder_too_slow", "enc"},
      {"resolution_too_high", "res"},
      {"keyframe_storm", "kf"},
      {"clock_drift", "clock"}};
  const auto it = kShort.find(root);
  return it == kShort.end() ? root : it->second;
}

std::string sanitize_id(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char ch : s) {
    const auto u = static_cast<unsigned char>(ch);
    const bool ok = std::isalnum(u) || ch == '-' || ch == '_' || ch == 'x';
    out.push_back(ok ? ch : '-');
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  return out;
}

std::string param_key_for(const std::string& kind) {
  if (kind == "drop") return "drop_pct";
  if (kind == "ingress_drop") return "inject_ingress_pct";
  if (kind == "rate") return "rate_mbps";
  if (kind == "backlog") return "queue_fps";
  if (kind == "delay") return "delay_ms";
  if (kind == "jitter") return "jitter_ms";
  return {};
}

std::expected<std::vector<Axis>, std::string> parse_axes(const ojson& j, const std::string& where) {
  if (!j.is_object()) return std::unexpected(where + " 的 axes 必须是对象（轴名 → 取值数组）");
  std::vector<Axis> axes;
  for (auto it = j.begin(); it != j.end(); ++it) {
    const std::string path = it.key();
    if (kKnownAxes.count(path) == 0) {
      if (path == "pipeline.width" || path == "pipeline.height") {
        return std::unexpected(where + " 不支持 " + path +
                               "：宽高必须成对，改用 pipeline.size（如 \"1280x720\"），"
                               "否则会展开出 1280x2160 这类非法配对");
      }
      return std::unexpected(where + " 的轴 " + path +
                             " 不认识（可用：" + join(std::vector<std::string>(kKnownAxes.begin(),
                                                                               kKnownAxes.end()),
                                                      " / ") +
                             "）");
    }
    if (!it.value().is_array() || it.value().empty()) {
      return std::unexpected(where + " 的轴 " + path + " 必须是非空数组");
    }
    Axis axis;
    axis.path = path;
    for (const auto& v : it.value()) {
      if (!v.is_primitive() || v.is_null()) {
        return std::unexpected(where + " 的轴 " + path + " 的取值必须是字符串/数字/布尔");
      }
      if (path == "fault.kind") {
        if (!is_string_scalar(v) || kFaultKinds.count(v.get<std::string>()) == 0) {
          return std::unexpected(where + " 的 fault.kind 取值不在 {none,drop,ingress_drop," +
                                 "rate,backlog,delay,jitter} 内");
        }
      } else if (path == "fault.value") {
        if (!is_number_scalar(v) || v.get<double>() < 0.0) {
          return std::unexpected(where + " 的 fault.value 取值必须是非负数");
        }
      } else if (path == "pipeline.transcode") {
        if (!is_bool_scalar(v)) {
          return std::unexpected(where + " 的 pipeline.transcode 取值必须是 true/false");
        }
      } else if (path == "pipeline.preset") {
        if (!is_string_scalar(v) || kPresets.count(v.get<std::string>()) == 0) {
          return std::unexpected(where + " 的 pipeline.preset 取值不是 x264 的十档之一");
        }
      } else if (path == "pipeline.size") {
        int w = 0;
        int h = 0;
        if (!is_string_scalar(v) || !parse_size(v.get<std::string>(), w, h)) {
          return std::unexpected(where +
                                 " 的 pipeline.size 必须是 \"宽x高\"（偶数、16..4096），"
                                 "例如 \"1280x720\"");
        }
      } else if (path == "pipeline.bitrate_kbps") {
        if (!v.is_number_integer() || v.get<long long>() < 100 || v.get<long long>() > 20000) {
          return std::unexpected(where + " 的 pipeline.bitrate_kbps 必须是 [100,20000] 的整数");
        }
      } else if (path == "pipeline.gop") {
        if (!v.is_number_integer() || v.get<long long>() < 1 || v.get<long long>() > 600) {
          return std::unexpected(where + " 的 pipeline.gop 必须是 [1,600] 的整数");
        }
      } else if (path == "expect.root_cause") {
        if (!is_string_scalar(v) || v.get<std::string>().empty()) {
          return std::unexpected(where + " 的 expect.root_cause 取值必须是非空字符串");
        }
      }
      axis.values.push_back(v);
    }
    axes.push_back(std::move(axis));
  }
  // 排序保证展开顺序与 JSON 文本里的键序无关（确定性：同矩阵同输出）
  std::sort(axes.begin(), axes.end(),
            [](const Axis& a, const Axis& b) { return a.path < b.path; });
  return axes;
}

std::expected<std::map<std::string, std::set<std::string>>, std::string> parse_expect(
    const ojson& j, const std::string& where) {
  std::map<std::string, std::set<std::string>> out;
  if (!j.is_object()) return std::unexpected(where + " 的 expect 必须是对象（根因 → 允许的 fault.kind）");
  for (auto it = j.begin(); it != j.end(); ++it) {
    if (!it.value().is_array() || it.value().empty()) {
      return std::unexpected(where + " 的 expect." + it.key() + " 必须是非空数组");
    }
    std::set<std::string> kinds;
    for (const auto& k : it.value()) {
      if (!is_string_scalar(k) || kFaultKinds.count(k.get<std::string>()) == 0) {
        return std::unexpected(where + " 的 expect." + it.key() +
                               " 含未知 fault.kind（可用：none/drop/ingress_drop/rate/"
                               "backlog/delay/jitter）");
      }
      kinds.insert(k.get<std::string>());
    }
    out[it.key()] = std::move(kinds);
  }
  return out;
}

std::map<std::string, std::set<std::string>> merge_expect(
    const std::map<std::string, std::set<std::string>>& low,
    const std::map<std::string, std::set<std::string>>& high) {
  auto out = low;
  for (const auto& [root, kinds] : high) out[root] = kinds;
  return out;
}

std::expected<Matrix, std::string> parse_matrix(const ojson& m) {
  if (!m.is_object()) return std::unexpected("矩阵根节点必须是对象");
  Matrix out;
  for (auto it = m.begin(); it != m.end(); ++it) {
    const std::string k = it.key();
    if (k != "suite" && k != "base" && k != "seed_range" && k != "count" && k != "axes" &&
        k != "expect" && k != "groups" && k != "note") {
      return std::unexpected("矩阵有未知字段 " + k +
                             "（可用：suite/base/seed_range/count/axes/expect/groups/note）");
    }
  }
  if (!m.contains("base") || !m["base"].is_string() || m["base"].get<std::string>().empty()) {
    return std::unexpected("矩阵缺少 base（继承模板的 suite 路径）");
  }
  out.base_path = m["base"].get<std::string>();
  if (m.contains("suite")) {
    if (!m["suite"].is_string() || m["suite"].get<std::string>().empty()) {
      return std::unexpected("矩阵的 suite 必须是非空字符串");
    }
    out.suite_name = m["suite"].get<std::string>();
  }
  if (m.contains("seed_range")) {
    const auto& r = m["seed_range"];
    if (!r.is_array() || r.size() != 2 || !r[0].is_number_integer() ||
        !r[1].is_number_integer() || r[0].get<long long>() < 0 ||
        r[1].get<long long>() < r[0].get<long long>()) {
      return std::unexpected("矩阵的 seed_range 必须是 [lo,hi]（非负整数，lo<=hi）");
    }
    out.seed_lo = static_cast<std::uint64_t>(r[0].get<long long>());
    out.seed_hi = static_cast<std::uint64_t>(r[1].get<long long>());
  }
  if (m.contains("count")) {
    if (!m["count"].is_number_integer() || m["count"].get<long long>() < 1) {
      return std::unexpected("矩阵的 count 必须是 >=1 的整数");
    }
    out.limit = static_cast<std::size_t>(m["count"].get<long long>());
  }

  std::vector<Axis> top_axes;
  if (m.contains("axes")) {
    auto a = parse_axes(m["axes"], "矩阵");
    if (!a) return std::unexpected(a.error());
    top_axes = std::move(*a);
  }
  std::map<std::string, std::set<std::string>> top_expect;
  if (m.contains("expect")) {
    auto e = parse_expect(m["expect"], "矩阵");
    if (!e) return std::unexpected(e.error());
    top_expect = std::move(*e);
  }

  auto merge_axes = [](const std::vector<Axis>& base, const std::vector<Axis>& over) {
    std::vector<Axis> out = base;
    for (const auto& a : over) {
      auto it = std::find_if(out.begin(), out.end(),
                             [&](const Axis& x) { return x.path == a.path; });
      if (it == out.end()) {
        out.push_back(a);
      } else {
        *it = a;  // 组内同名轴覆盖顶层
      }
    }
    return out;
  };

  if (m.contains("groups")) {
    if (!m["groups"].is_array() || m["groups"].empty()) {
      return std::unexpected("矩阵的 groups 必须是非空数组");
    }
    for (std::size_t gi = 0; gi < m["groups"].size(); ++gi) {
      const auto& g = m["groups"][gi];
      const std::string where = "矩阵 groups[" + std::to_string(gi) + "]";
      if (!g.is_object()) return std::unexpected(where + " 必须是对象");
      for (auto it = g.begin(); it != g.end(); ++it) {
        if (it.key() != "axes" && it.key() != "expect" && it.key() != "note") {
          return std::unexpected(where + " 有未知字段 " + it.key() + "（可用：axes/expect/note）");
        }
      }
      Plan p;
      if (g.contains("axes")) {
        auto a = parse_axes(g["axes"], where);
        if (!a) return std::unexpected(a.error());
        p.axes = merge_axes(top_axes, *a);
      } else {
        p.axes = top_axes;
      }
      p.expect = top_expect;
      if (g.contains("expect")) {
        auto e = parse_expect(g["expect"], where);
        if (!e) return std::unexpected(e.error());
        p.expect = merge_expect(top_expect, *e);
      }
      if (p.axes.empty()) return std::unexpected(where + " 没有可展开的轴");
      out.groups.push_back(std::move(p));
    }
  } else {
    if (top_axes.empty()) return std::unexpected("矩阵没有可展开的轴（axes 为空）");
    Plan p;
    p.axes = std::move(top_axes);
    p.expect = std::move(top_expect);
    out.groups.push_back(std::move(p));
  }
  return out;
}

// 模板索引：每个根因取 base 里**第一个** case。返回的错误在生成开始前就暴露，
// 而不是让某一类组合悄悄全部被拒。
std::expected<std::map<std::string, ojson>, std::string> index_templates(const ojson& base) {
  if (!base.is_object()) return std::unexpected("base 根节点必须是对象");
  if (!base.contains("cases") || !base["cases"].is_array() || base["cases"].empty()) {
    return std::unexpected("base 缺少非空 cases 数组");
  }
  std::map<std::string, ojson> out;
  for (const auto& c : base["cases"]) {
    if (!c.is_object()) return std::unexpected("base 的 case 必须是对象");
    if (!c.contains("expect") || !c["expect"].is_object() ||
        !c["expect"].contains("root_cause") || !c["expect"]["root_cause"].is_string()) {
      return std::unexpected("base 的 case 缺少 expect.root_cause（无法按根因取模板）");
    }
    if (c.value("manual_only", false)) {
      return std::unexpected("base 的 case " + c.value("id", std::string("?")) +
                             " 标了 manual_only：它不能被自动判分，不能当生成模板");
    }
    const std::string root = c["expect"]["root_cause"].get<std::string>();
    if (out.count(root) == 0) out[root] = c;
  }
  return out;
}

std::optional<double> template_fps_min(const ojson& t) {
  if (!t.contains("expect") || !t["expect"].is_object()) return std::nullopt;
  const auto& e = t["expect"];
  if (!e.contains("sla") || !e["sla"].is_object()) return std::nullopt;
  const auto& s = e["sla"];
  if (s.contains("usable_fps_min") && s["usable_fps_min"].is_number()) {
    return s["usable_fps_min"].get<double>();
  }
  if (s.contains("fps_min") && s["fps_min"].is_number()) return s["fps_min"].get<double>();
  return std::nullopt;
}

std::optional<double> template_lat_p99(const ojson& t) {
  if (!t.contains("expect") || !t["expect"].is_object()) return std::nullopt;
  const auto& e = t["expect"];
  if (!e.contains("sla") || !e["sla"].is_object()) return std::nullopt;
  const auto& s = e["sla"];
  if (s.contains("lat_p99_ms") && s["lat_p99_ms"].is_number()) {
    return s["lat_p99_ms"].get<double>();
  }
  return std::nullopt;
}

void collect_combos(const std::vector<Axis>& axes, std::size_t idx,
                    std::map<std::string, ojson>& cur,
                    std::vector<std::map<std::string, ojson>>& out) {
  if (idx == axes.size()) {
    out.push_back(cur);
    return;
  }
  for (const auto& v : axes[idx].values) {
    cur[axes[idx].path] = v;
    collect_combos(axes, idx + 1, cur, out);
    if (out.size() > kMaxCombinations) return;
  }
  cur.erase(axes[idx].path);
}

}  // namespace

std::expected<GeneratedSuite, std::string> generate_cases(const nlohmann::json& matrix_input,
                                                          const nlohmann::json& base_input,
                                                          const std::string& suite_name_in) {
  try {
    const auto matrix_json = ojson::parse(matrix_input.dump());
    const auto base_json = ojson::parse(base_input.dump());
    auto mx = parse_matrix(matrix_json);
    if (!mx) return std::unexpected(mx.error());
    auto templates = index_templates(base_json);
    if (!templates) return std::unexpected(templates.error());

    const std::string suite_name =
        suite_name_in.empty() ? std::string("generated") : suite_name_in;
    GeneratedSuite out;
    out.suite = ojson::object();
    std::vector<Entry> valid;
    // fault.kind → 允许它的根因集合：只有当同一个 kind 对应多个根因时，id 才需要追加
    // 根因短名消歧（否则会出现 gen-delay-200-tc-off-delay 这种重复 token）。
    std::map<std::string, std::set<std::string>> roots_by_kind;
    for (const auto& g : mx->groups) {
      for (const auto& [root, kinds] : g.expect) {
        for (const auto& kind : kinds) roots_by_kind[kind].insert(root);
      }
    }
    // 提前把「矩阵声明了根因但 base 里没有模板」暴露成硬错误：这不是某条组合的问题，
    // 是矩阵与 base 对不上，静默拒绝整类只会让人以为矩阵生效了。
    for (const auto& g : mx->groups) {
      for (const auto& [root, kinds] : g.expect) {
        (void)kinds;
        if (templates->count(root) == 0) {
          return std::unexpected("base 里没有根因 " + root +
                                 " 的模板 case（base 需按根因各给至少一个 case）");
        }
      }
      // expect.root_cause 轴的每个取值都必须有 expect 声明，否则反查规则无从判断
      for (const auto& a : g.axes) {
        if (a.path != "expect.root_cause") continue;
        for (const auto& v : a.values) {
          const std::string root = v.get<std::string>();
          if (g.expect.count(root) == 0) {
            return std::unexpected("expect.root_cause 轴取值 " + root +
                                   " 没有在 expect 里声明允许的 fault.kind");
          }
        }
      }
    }

    std::uint64_t ordinal = 0;
    for (const auto& g : mx->groups) {
      std::vector<std::map<std::string, ojson>> combos;
      std::map<std::string, ojson> cur;
      collect_combos(g.axes, 0, cur, combos);
      if (combos.size() > kMaxCombinations) {
        return std::unexpected("矩阵展开超过 " + std::to_string(kMaxCombinations) + " 个组合");
      }
      const std::uint64_t span = mx->seed_hi - mx->seed_lo + 1;
      for (const auto& combo : combos) {
        const std::uint64_t seed = mx->seed_lo + (ordinal % span);
        ++ordinal;
        ++out.stats.combinations;

        auto note_reject = [&](const std::string& reason) {
          ++out.stats.rejected;
          ++out.stats.rejected_reasons[reason];
        };

        // ---- 1) fault.kind / fault.value ----
        std::string kind = "none";
        if (const auto it = combo.find("fault.kind"); it != combo.end()) {
          kind = it->second.get<std::string>();
        }
        const auto value_it = combo.find("fault.value");
        double value = 0.0;
        if (kind == "none") {
          if (value_it != combo.end()) {
            note_reject("fault.kind=none 却给了 fault.value（无注入可设）");
            continue;
          }
        } else {
          if (value_it != combo.end()) {
            value = value_it->second.get<double>();
          }
        }

        // ---- 2) 根因：轴上显式给出，或由 expect 按 fault.kind 反查 ----
        std::string root;
        if (const auto it = combo.find("expect.root_cause"); it != combo.end()) {
          root = it->second.get<std::string>();
          const auto allowed = g.expect.find(root);
          if (allowed == g.expect.end() || allowed->second.count(kind) == 0) {
            note_reject("根因 " + root + " 的 expect 未声明允许 fault.kind=" + kind);
            continue;
          }
        } else {
          std::vector<std::string> hits;
          for (const auto& [r, kinds] : g.expect) {
            if (kinds.count(kind) != 0) hits.push_back(r);
          }
          if (hits.empty()) {
            note_reject("fault.kind=" + kind + " 不在 expect 的任何根因允许列表里");
            continue;
          }
          if (hits.size() > 1) {
            note_reject("fault.kind=" + kind +
                        " 对应多个根因（需用 expect.root_cause 轴消歧："
                        + join(hits, "/") + "）");
            continue;
          }
          root = hits.front();
        }
        const auto tpl_it = templates->find(root);
        if (tpl_it == templates->end()) {
          note_reject("base 里没有根因 " + root + " 的模板");
          continue;
        }
        const ojson& tpl = tpl_it->second;

        // 非 none 的 kind 必须有注入值：矩阵没给 fault.value 时，只有 base 模板本身的
        // fault.kind 相同才谈得上继承；否则这条组合根本没有故障量。
        if (kind != "none") {
          if (value_it == combo.end()) {
            double inherited = 0.0;
            const bool same_kind = tpl.contains("fault") && tpl["fault"].is_object() &&
                                   tpl["fault"].contains("kind") &&
                                   tpl["fault"].value("kind", std::string()) == kind;
            const std::string key = param_key_for(kind);
            if (same_kind && tpl["fault"].contains("params") &&
                tpl["fault"]["params"].is_object() && !key.empty() &&
                tpl["fault"]["params"].contains(key)) {
              inherited = tpl["fault"]["params"][key].get<double>();
            }
            if (inherited <= 0.0) {
              note_reject("fault.kind=" + kind + " 缺少 fault.value 轴（模板也不含同值）");
              continue;
            }
            value = inherited;
          }
          const bool delay_kind = kind == "delay" || kind == "jitter";
          const double upper = delay_kind ? 2000.0 : 100.0;
          if (value <= 0.0 || value > upper) {
            note_reject("fault.kind=" + kind + " 的注入值必须在 (0," +
                        std::to_string(static_cast<int>(upper)) + "]");
            continue;
          }
        }

        // ---- 3) 应用轴到模板副本 ----
        ojson c = tpl;
        ojson pipeline = (c.contains("pipeline") && c["pipeline"].is_object())
                             ? c["pipeline"]
                             : ojson::object();
        const bool axis_transcode =
            combo.find("pipeline.transcode") != combo.end();
        const bool transcode =
            axis_transcode ? combo.at("pipeline.transcode").get<bool>()
                           : pipeline.value("transcode", false);
        if (axis_transcode) pipeline["transcode"] = transcode;
        for (const auto& [path, v] : combo) {
          if (path == "pipeline.preset") pipeline["preset"] = v.get<std::string>();
          else if (path == "pipeline.size") {
            int w = 0;
            int h = 0;
            (void)parse_size(v.get<std::string>(), w, h);  // 解析已在 parse_axes 校验过
            pipeline["width"] = w;
            pipeline["height"] = h;
          } else if (path == "pipeline.bitrate_kbps") {
            pipeline["bitrate_kbps"] = v.get<long long>();
          } else if (path == "pipeline.gop") pipeline["gop"] = v.get<long long>();
        }

        // ---- 4) 合法性规则（每条都带"为什么"） ----
        bool has_encoder_axis = false;
        for (const auto& [path, v] : combo) {
          (void)v;
          if (kEncoderAxes.count(path) != 0) has_encoder_axis = true;
        }
        if (has_encoder_axis && !transcode) {
          // transcode=off 时 cued 走 passthrough，preset/width/height/bitrate/gop 一个都不读：
          // 这类组合的"参数"在指标上完全不可观测，等于生成了一个假故障 case。
          note_reject("transcode=off 时编码器轴（preset/size/bitrate/gop）不生效");
          continue;
        }
        bool has_pipeline_axis = false;
        for (const auto& [path, v] : combo) {
          (void)v;
          if (path.rfind("pipeline.", 0) == 0) has_pipeline_axis = true;
        }
        if (kind == "none" && !has_pipeline_axis) {
          note_reject("fault.kind=none 且没有改任何管线参数（= 干净基线，不是故障 case）");
          continue;
        }
        const auto fps_min = template_fps_min(tpl);
        const bool fps_judged = fps_min.has_value() && *fps_min > 0.0;

        if (kind == "none") {
          // 管线参数类故障（慢 preset / 超分辨率）没有可清除的注入：修复只能靠 repair。
          // 没有 repair 的模板等于"故障存在且永远修不回来"，SLA 必然判不过。
          if (!tpl.contains("repair") || !tpl["repair"].is_object() || tpl["repair"].empty()) {
            note_reject("根因 " + root + " 的模板没有 repair：管线参数类故障无法修复");
            continue;
          }
          if (!transcode && (root == "encoder_too_slow" || root == "resolution_too_high")) {
            // 转码关闭时 preset / 输出分辨率都不参与编码，"慢"与"高分辨率"无从谈起。
            note_reject("根因 " + root + " 需要 transcode=on（passthrough 不跑编码器）");
            continue;
          }
        }
        if (kind != "none" && transcode) {
          // 故障类 case 的修复动作只有「inject_fault value=0」：底子必须自己达标，
          // 否则清除注入后 SLA 仍不成立，这个 case 永远判不过（或者反过来测的是编码慢）。
          const std::string preset = pipeline.value("preset", std::string("veryfast"));
          const long long w = pipeline.value("width", 0);
          const long long h = pipeline.value("height", 0);
          const long long pixels = w * h;
          if (kSlaSafePresets.count(preset) == 0) {
            note_reject("故障类 case 的底子用 " + preset +
                        "：清除注入后编码耗时仍超过帧预算，修不回来");
            continue;
          }
          if (pixels > kSlaSafePixels) {
            note_reject("故障类 case 的底子超过 720p：清除注入后 fps 仍达不到 fps_min");
            continue;
          }
        }

        if (fps_judged) {
          if (kind == "drop") {
            // 出向丢包发生在 frames_out 结算之后：fps 与 dropped 都不动，只涨 injected。
            // 以 fps 为判据的 case 在故障态就提前达标，修不修都过——不可判。
            note_reject("fault.kind=drop 只涨 injected、不改 fps，fps 类 SLA 在故障态就成立");
            continue;
          }
          if (kind == "ingress_drop") {
            const double fps_upper = kSourceFps * (1.0 - value / 100.0);
            if (fps_upper > *fps_min - kSlaMarginFps) {
              note_reject("上行丢包 " + fmt_number(value) + "% 只能把 fps 压到 " +
                          fmt_number(fps_upper) + "（阈值 " + fmt_number(*fps_min) +
                          "，要求余量 " + fmt_number(kSlaMarginFps) + " fps）");
              continue;
            }
          }
          if (kind == "backlog") {
            if (value + kSlaMarginFps > *fps_min) {
              note_reject("排队节奏 " + fmt_number(value) + " fps 不低于阈值 " +
                          fmt_number(*fps_min) + " - " + fmt_number(kSlaMarginFps) +
                          " fps，故障态 fps 达不到要求");
              continue;
            }
          }
          if (kind == "rate" && value > kRateCapMaxMbps) {
            note_reject("限速 " + fmt_number(value) +
                        " Mbps 高于本机实测可判上界 0.25 Mbps（fps 回到阈值以上）");
            continue;
          }
        }
        if ((kind == "delay" || kind == "jitter") && transcode) {
          // 时延类 case 固定 passthrough：它的唯一判据是 send 段分位 + "fps 不降、三类丢包
          // 计数全 0"。开转码会在同一故障窗口里叠加编码/缩放负载（encode/op 段变化），
          // 把 send 段延迟与 encoder_too_slow / resolution_too_high 的可观测面混在一起，
          // 诊断上不再有唯一解释。
          note_reject("时延类 case 必须 passthrough：transcode=on 会引入编码负载，"
                      "与 encoder/resolution 类的可观测面串味");
          continue;
        }
        if (kind == "rate" && value < kRateCapMinMbps) {
          note_reject("限速 " + fmt_number(value) +
                      " Mbps 低于 cued --send-mbps 下限 0.1（进程拒绝启动，拿不到媒体流）");
          continue;
        }
        if (kind == "delay" || kind == "jitter") {
          const auto lat = template_lat_p99(tpl);
          if (!lat.has_value() || *lat <= 0.0) {
            note_reject("延迟类 case 的模板没有声明 lat_p99_ms：故障不改变 fps，"
                        "没有可判的 SLA 阈值");
            continue;
          }
          if (value < *lat + kDelayMarginMs) {
            note_reject("延迟注入 " + fmt_number(value) + " ms 不足以越过声明的 p99 阈值 " +
                        fmt_number(*lat) + " ms + " + fmt_number(kDelayMarginMs) + " ms");
            continue;
          }
        }

        // ---- 5) 组装 case ----
        ojson fault = ojson::object();
        fault["kind"] = kind;
        if (kind != "none") {
          ojson params = ojson::object();
          params[param_key_for(kind)] = value;
          fault["params"] = params;
        }
        if (!c.contains("expect") || !c["expect"].is_object()) {
          return std::unexpected("base 模板 " + tpl.value("id", std::string("?")) +
                                 " 缺少 expect");
        }
        c["fault"] = fault;
        c["pipeline"] = pipeline;
        c["expect"]["root_cause"] = root;

        // id：可读 + 唯一。它同时是 --record-tools 的文件名，所以只留 [A-Za-z0-9_-]。
        std::vector<std::string> segs;
        if (kind == "none") {
          segs.push_back(root_short(root));
        } else {
          segs.push_back(kind);
          segs.push_back(trim_number(value));
        }
        segs.push_back(transcode ? "tc-on" : "tc-off");
        if (const auto it = combo.find("pipeline.preset"); it != combo.end()) {
          segs.push_back(it->second.get<std::string>());
        }
        if (const auto it = combo.find("pipeline.size"); it != combo.end()) {
          segs.push_back(it->second.get<std::string>());
        }
        if (const auto it = combo.find("pipeline.bitrate_kbps"); it != combo.end()) {
          segs.push_back("b" + std::to_string(it->second.get<long long>()));
        }
        if (const auto it = combo.find("pipeline.gop"); it != combo.end()) {
          segs.push_back("g" + std::to_string(it->second.get<long long>()));
        }
        if (kind != "none" && roots_by_kind[kind].size() > 1) {
          segs.push_back(root_short(root));  // 同一 kind 对应多个根因：id 里必须消歧
        }
        const std::string id =
            sanitize_id("gen-" + join(segs, "-") + "-s" + std::to_string(seed));
        c["id"] = id;
        c["seed"] = seed;

        valid.push_back(Entry{std::move(c), root, id});
      }
    }

    // ---- 6) 数量限制：按根因轮流取，避免小样本被展开顺序（某个轴在前）带偏 ----
    std::vector<Entry> selected;
    if (mx->limit == 0 || valid.size() <= mx->limit) {
      selected = std::move(valid);
    } else {
      std::vector<std::string> order;
      std::map<std::string, std::vector<Entry>> buckets;
      for (auto& e : valid) {
        if (buckets.count(e.root) == 0) order.push_back(e.root);
        buckets[e.root].push_back(std::move(e));
      }
      std::size_t round = 0;
      while (selected.size() < mx->limit) {
        bool took = false;
        for (const auto& root : order) {
          auto& b = buckets[root];
          if (round < b.size() && selected.size() < mx->limit) {
            selected.push_back(std::move(b[round]));
            took = true;
          }
        }
        if (!took) break;
        ++round;
      }
    }

    // ---- 7) 生成物必须能被 load_suite 接受：用同一套 parse_case 做最后一道门 ----
    out.suite["cases"] = ojson::array();
    std::set<std::string> ids;
    for (auto& e : selected) {
      auto parsed = parse_case(e.case_json, suite_name);
      if (!parsed) {
        return std::unexpected("生成器产出了不合法的 case：" + parsed.error());
      }
      if (!ids.insert(e.id).second) {
        return std::unexpected("生成器产出重复 id: " + e.id +
                               "（矩阵里有两条组合只在未进 id 的字段上不同）");
      }
      ++out.stats.by_root_cause[e.root];
      out.suite["cases"].push_back(std::move(e.case_json));
    }
    out.stats.generated = selected.size();

    out.suite["suite"] = suite_name;
    out.suite["version"] = 1;
    out.suite["note"] =
        "由 cue-eval --gen 从矩阵生成（FR-EVAL.5）：注入矩阵 × 管线参数的组合批量生成，"
        "prompt / expect.sla / evidence_candidates / allowed_actions / budget / repair 全部"
        "继承 base 中同根因的模板，只有轴上的参数被替换。合法性规则："
        "（1）fault.kind=none 与 fault.value 互斥；"
        "（2）transcode=off 时编码器轴（preset/size/bitrate/gop）不生效，组合被拒；"
        "（3）根因由矩阵的 expect 按 fault.kind 反查，多根因共用同一 kind 时必须显式给"
        " expect.root_cause；"
        "（4）以 fps 为判据的根因拒绝 fault.kind=drop（出向丢包不改 fps，修不修都过）；"
        "（5）上行丢包率 p 需满足 30*(1-p) <= fps_min-3，排队节奏需 <= fps_min-3；"
        "（6）延迟/抖动需越过声明 lat_p99_ms 至少 30 ms，否则故障窗口仍然达标；"
        "（7）故障类 case 的修复动作只有清除注入，底子须为 720p 以内 + 快档，否则清除后"
        "仍达不到 fps_min；"
        "（8）限速值必须落在 cued 可执行的 [0.1,0.25] Mbps：低于 0.1 进程直接拒绝启动，"
        "高于 0.25 时故障态 fps 回到阈值以上；"
        "（9）时延/抖动类只允许 transcode=off（开转码会叠加编码负载，诊断面与编码类串味）。生成结果只保证「组合合法 + 能被引擎执行」；故障态的实测数值"
        "以 base 里的六类模板与实际运行记录为准，生成器不做 fps 预测。";
    return out;
  } catch (const nlohmann::json::exception& e) {
    return std::unexpected(std::string("矩阵/模板 JSON 结构错误：") + e.what());
  }
}

std::expected<GeneratedSuite, std::string> generate_from_files(const std::string& matrix_path) {
  std::ifstream in(matrix_path);
  if (!in) return std::unexpected("无法打开矩阵文件: " + matrix_path);
  std::stringstream ss;
  ss << in.rdbuf();
  auto matrix = nlohmann::json::parse(ss.str(), nullptr, false);
  if (matrix.is_discarded()) {
    return std::unexpected("矩阵不是合法 JSON: " + matrix_path);
  }
  if (!matrix.is_object() || !matrix.contains("base") || !matrix["base"].is_string()) {
    return std::unexpected("矩阵缺少 base（继承模板的 suite 路径）: " + matrix_path);
  }
  const std::string base_rel = matrix["base"].get<std::string>();
  if (base_rel.empty()) {
    return std::unexpected("矩阵的 base 为空: " + matrix_path);
  }
  const std::filesystem::path matrix_dir = std::filesystem::path(matrix_path).parent_path();
  std::filesystem::path base_path = matrix_dir / base_rel;
  if (!std::filesystem::exists(base_path)) {
    base_path = std::filesystem::path(base_rel);  // 退回 cwd（矩阵里写的是仓库相对路径）
  }
  std::ifstream bin(base_path);
  if (!bin) return std::unexpected("无法打开 base suite: " + base_path.string());
  std::stringstream bss;
  bss << bin.rdbuf();
  auto base = nlohmann::json::parse(bss.str(), nullptr, false);
  if (base.is_discarded()) {
    return std::unexpected("base suite 不是合法 JSON: " + base_path.string());
  }
  std::string name;
  if (matrix.contains("suite") && matrix["suite"].is_string()) {
    name = matrix["suite"].get<std::string>();
  }
  if (name.empty()) {
    name = std::filesystem::path(matrix_path).stem().string();
  }
  return generate_cases(matrix, base, name);
}

}  // namespace cue::eval
