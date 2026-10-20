#include "cue/eval/compare.h"

#include <sstream>

namespace cue::eval {

const char* diff_class_name(DiffClass c) {
  switch (c) {
    case DiffClass::NewFailure: return "新增失败";
    case DiffClass::Fixed: return "已修复";
    case DiffClass::StillFailing: return "仍失败";
    case DiffClass::StillPassing: return "仍通过";
    case DiffClass::OnlyInA: return "仅 A 有";
    case DiffClass::OnlyInB: return "仅 B 有";
  }
  return "?";
}

std::map<std::string, CaseAgg> aggregate(const StateMap& state) {
  std::map<std::string, CaseAgg> out;
  for (const auto& [key, cached] : state) {
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

DiffReport diff_runs(const std::string& run_a, const StateMap& a, const std::string& run_b,
                     const StateMap& b) {
  DiffReport d;
  d.run_a = run_a;
  d.run_b = run_b;
  const auto aa = aggregate(a);
  const auto bb = aggregate(b);
  auto classify = [](bool pa, bool pb) {
    if (pa && pb) return DiffClass::StillPassing;
    if (pa && !pb) return DiffClass::NewFailure;
    if (!pa && pb) return DiffClass::Fixed;
    return DiffClass::StillFailing;
  };
  for (const auto& [id, agg_a] : aa) {
    auto it = bb.find(id);
    if (it == bb.end()) {
      d.cases.push_back(CaseDiff{id, DiffClass::OnlyInA, agg_a, CaseAgg{}});
      ++d.only_a;
      continue;
    }
    const DiffClass cls = classify(agg_a.pass(), it->second.pass());
    d.cases.push_back(CaseDiff{id, cls, agg_a, it->second});
    switch (cls) {
      case DiffClass::NewFailure: ++d.new_failures; break;
      case DiffClass::Fixed: ++d.fixed; break;
      case DiffClass::StillFailing: ++d.still_failing; break;
      case DiffClass::StillPassing: ++d.still_passing; break;
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
    if (g.run_errors > 0) os << "，run_error=" << g.run_errors;
    if (!g.root_cause.empty()) os << "，root=" << g.root_cause;
    return os.str();
  };
  std::ostringstream o;
  o << "# Cue 回归对比\n\n";
  o << "- A: `" << d.run_a << "`\n";
  o << "- B: `" << d.run_b << "`\n";
  o << "- 新增失败 " << d.new_failures << "，已修复 " << d.fixed << "，仍失败 " << d.still_failing
    << "，仍通过 " << d.still_passing;
  if (d.only_a || d.only_b) o << "，仅 A 有 " << d.only_a << "，仅 B 有 " << d.only_b;
  o << "\n\n";
  o << "| case | 结论 | A | B |\n|---|---|---|---|\n";
  for (const auto& c : d.cases) {
    o << "| " << c.id << " | " << diff_class_name(c.cls) << " | " << agg_text(c.a) << " | "
      << agg_text(c.b) << " |\n";
  }
  if (d.new_failures > 0) {
    o << "\n## 新增失败（回归）\n\n";
    for (const auto& c : d.cases) {
      if (c.cls == DiffClass::NewFailure) o << "- `" << c.id << "`：A " << agg_text(c.a) << " → B " << agg_text(c.b) << "\n";
    }
  }
  if (d.fixed > 0) {
    o << "\n## 已修复\n\n";
    for (const auto& c : d.cases) {
      if (c.cls == DiffClass::Fixed) o << "- `" << c.id << "`：A " << agg_text(c.a) << " → B " << agg_text(c.b) << "\n";
    }
  }
  if (d.still_failing > 0) {
    o << "\n## 仍失败\n\n";
    for (const auto& c : d.cases) {
      if (c.cls == DiffClass::StillFailing) o << "- `" << c.id << "`：" << agg_text(c.b) << "\n";
    }
  }
  return o.str();
}

}  // namespace cue::eval
