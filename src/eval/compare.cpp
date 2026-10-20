#include "cue/eval/compare.h"

#include <sstream>

namespace cue::eval {

const char* diff_class_name(DiffClass c) {
  switch (c) {
    case DiffClass::NewFailure: return "新增失败";
    case DiffClass::Fixed: return "已修复";
    case DiffClass::StillFailing: return "仍失败";
    case DiffClass::StillPassing: return "仍通过";
    case DiffClass::Inconclusive: return "不可判定";
    case DiffClass::OnlyInA: return "仅 A 有";
    case DiffClass::OnlyInB: return "仅 B 有";
  }
  return "?";
}

std::map<std::string, CaseAgg> aggregate(const StateMap& state) {
  std::map<std::string, CaseAgg> out;
  for (const auto& [key, cached] : state) {
    (void)key;
    const auto& o = cached.outcome;
    auto& agg = out[o.id];
    ++agg.attempts;
    if (!o.scored) {
      ++agg.run_errors;
      continue;
    }
    if (o.score.all()) ++agg.passes;
    if (agg.root_cause.empty()) agg.root_cause = o.diagnosis.root_cause;
  }
  return out;
}

DiffReport diff_runs(const RunInfo& a, const StateMap& a_state, const RunInfo& b,
                     const StateMap& b_state) {
  DiffReport d;
  d.a = a;
  d.b = b;
  // 判分器/配置不同时，两边"同一 case"的结论没有可比性：先把不可比说清楚
  const auto warn = [&d](const std::string& w) {
    d.comparable = false;
    d.warnings.push_back(w);
  };
  if (!a.fingerprint.empty() && !b.fingerprint.empty() && a.fingerprint != b.fingerprint) {
    warn("A/B 指纹不同（判分器或生效配置变了），差异不能归因于被测对象");
  }
  if (!a.suite.empty() && !b.suite.empty() && a.suite != b.suite) {
    warn("A/B suite 不同：A=" + a.suite + "，B=" + b.suite);
  }
  if (!a.mode.empty() && !b.mode.empty() && a.mode != b.mode) {
    warn("A/B agent 模式不同：A=" + a.mode + "，B=" + b.mode);
  }
  if (a.repeat != b.repeat) {
    warn("A/B repeat 不同：A=" + std::to_string(a.repeat) + "，B=" + std::to_string(b.repeat));
  }

  const auto aa = aggregate(a_state);
  const auto bb = aggregate(b_state);
  // run_error 不等于失败：任一侧有 run_error 时这条 case 只是"没结论"
  auto classify = [](const CaseAgg& pa, const CaseAgg& pb) {
    if (pa.run_errors > 0 || pb.run_errors > 0) return DiffClass::Inconclusive;
    if (pa.pass() && pb.pass()) return DiffClass::StillPassing;
    if (pa.pass() && !pb.pass()) return DiffClass::NewFailure;
    if (!pa.pass() && pb.pass()) return DiffClass::Fixed;
    return DiffClass::StillFailing;
  };
  for (const auto& [id, agg_a] : aa) {
    auto it = bb.find(id);
    if (it == bb.end()) {
      d.cases.push_back(CaseDiff{id, DiffClass::OnlyInA, agg_a, CaseAgg{}});
      ++d.only_a;
      continue;
    }
    const DiffClass cls = classify(agg_a, it->second);
    d.cases.push_back(CaseDiff{id, cls, agg_a, it->second});
    switch (cls) {
      case DiffClass::NewFailure: ++d.new_failures; break;
      case DiffClass::Fixed: ++d.fixed; break;
      case DiffClass::StillFailing: ++d.still_failing; break;
      case DiffClass::StillPassing: ++d.still_passing; break;
      case DiffClass::Inconclusive: ++d.inconclusive; break;
      default: break;
    }
  }
  for (const auto& [id, agg_b] : bb) {
    if (aa.find(id) == aa.end()) {
      d.cases.push_back(CaseDiff{id, DiffClass::OnlyInB, CaseAgg{}, agg_b});
      ++d.only_b;
    }
  }
  return d;
}

std::string render_diff_markdown(const DiffReport& d) {
  auto agg_text = [](const CaseAgg& g) {
    if (g.attempts == 0) return std::string("—");
    std::ostringstream os;
    os << g.passes << "/" << g.attempts << " 过";
    if (g.run_errors > 0) os << "，run_error=" << g.run_errors << "（不计分母）";
    if (!g.root_cause.empty()) os << "，root=" << g.root_cause;
    return os.str();
  };
  std::ostringstream o;
  o << "# Cue 回归对比\n\n";
  o << "- A: `" << d.a.run_id << "`（suite=" << d.a.suite << "，mode=" << d.a.mode
    << "，repeat=" << d.a.repeat << "）\n";
  o << "- B: `" << d.b.run_id << "`（suite=" << d.b.suite << "，mode=" << d.b.mode
    << "，repeat=" << d.b.repeat << "）\n";
  o << "- A 指纹: `" << d.a.fingerprint << "`\n";
  o << "- B 指纹: `" << d.b.fingerprint << "`\n";
  o << "- 新增失败 " << d.new_failures << "，已修复 " << d.fixed << "，仍失败 " << d.still_failing
    << "，仍通过 " << d.still_passing << "，不可判定 " << d.inconclusive;
  if (d.only_a || d.only_b) o << "，仅 A 有 " << d.only_a << "，仅 B 有 " << d.only_b;
  o << "\n";
  if (!d.comparable) {
    o << "\n> **两 run 不可比**：差异不能归因于被测对象的改动，先对齐配置再比。\n";
    for (const auto& w : d.warnings) o << "> - " << w << "\n";
  }
  if (d.only_a || d.only_b) {
    o << "\n> **case 集发生变化**（仅 A 有 " << d.only_a << "，仅 B 有 " << d.only_b
      << "）：suite 改名/增删会让 diff 的公共部分失去意义。\n";
  }
  o << "\n| case | 结论 | A | B |\n|---|---|---|---|\n";
  for (const auto& c : d.cases) {
    o << "| " << c.id << " | " << diff_class_name(c.cls) << " | " << agg_text(c.a) << " | "
      << agg_text(c.b) << " |\n";
  }
  if (d.new_failures > 0) {
    o << "\n## 新增失败（回归）\n\n";
    for (const auto& c : d.cases) {
      if (c.cls != DiffClass::NewFailure) continue;
      o << "- `" << c.id << "`：A " << agg_text(c.a) << " → B " << agg_text(c.b) << "\n";
    }
  }
  if (d.fixed > 0) {
    o << "\n## 已修复\n\n";
    for (const auto& c : d.cases) {
      if (c.cls != DiffClass::Fixed) continue;
      o << "- `" << c.id << "`：A " << agg_text(c.a) << " → B " << agg_text(c.b) << "\n";
    }
  }
  if (d.still_failing > 0) {
    o << "\n## 仍失败\n\n";
    for (const auto& c : d.cases) {
      if (c.cls != DiffClass::StillFailing) continue;
      o << "- `" << c.id << "`：" << agg_text(c.b) << "\n";
    }
  }
  if (d.inconclusive > 0) {
    o << "\n## 不可判定（run_error，不计入新增失败）\n\n";
    for (const auto& c : d.cases) {
      if (c.cls != DiffClass::Inconclusive) continue;
      o << "- `" << c.id << "`：A " << agg_text(c.a) << " → B " << agg_text(c.b)
        << "（环境/预算问题，需重跑）\n";
    }
  }
  return o.str();
}

}  // namespace cue::eval
