#include "cue/eval/manual.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <set>
#include <string_view>

#include "cue/common/sha256.h"
#include "cue/eval/state.h"

namespace cue::eval {
namespace {
using Result = std::expected<void, std::string>;
Result keys(const json& j, std::initializer_list<const char*> allowed, const std::string& where) {
  if (!j.is_object()) return std::unexpected(where + " must be an object");
  for (auto it = j.begin(); it != j.end(); ++it) {
    bool known = false;
    for (auto key : allowed) if (it.key() == key) known = true;
    if (!known) return std::unexpected(where + ": unknown field " + it.key());
  }
  return {};
}
bool nonempty(const json& j, const char* key) {
  return j.contains(key) && j[key].is_string() &&
         j[key].get_ref<const std::string&>().find_first_not_of(" \t\r\n") != std::string::npos;
}
bool counter(const json& v) {
  return v.is_number_unsigned() || (v.is_number_integer() && v.get<int64_t>() >= 0);
}
bool nonnegative(const json& v) {
  return v.is_number() && std::isfinite(v.get<double>()) && v.get<double>() >= 0;
}
Result latency(const json& j, const std::string& where) {
  if (auto ok = keys(j, {"p50", "p95", "p99"}, where); !ok) return ok;
  for (auto it = j.begin(); it != j.end(); ++it)
    if (!it->is_null() && !nonnegative(*it))
      return std::unexpected(where + "." + it.key() + " must be a finite nonnegative number or null");
  return {};
}
Result metrics(const json& j) {
  if (auto ok = keys(j, {"fps", "frames_in", "frames_out", "dropped", "injected", "decoded",
                         "decode_errors", "samples", "updated_us", "recv", "buffer", "decode",
                         "op", "encode", "send", "probe"}, "metrics"); !ok) return ok;
  if (!j.contains("fps") || !nonnegative(j["fps"]) || !j.contains("samples") ||
      !counter(j["samples"]) || j["samples"] == 0)
    return std::unexpected("metrics requires finite nonnegative fps and positive integer samples");
  for (auto it = j.begin(); it != j.end(); ++it) {
    const auto& key = it.key();
    if (key == "probe") continue;
    if (key == "recv" || key == "buffer" || key == "decode" || key == "op" ||
        key == "encode" || key == "send") {
      if (auto ok = latency(*it, "metrics." + key); !ok) return ok;
    } else if (key == "fps") {
      if (!nonnegative(*it)) return std::unexpected("metrics.fps invalid");
    } else if (key == "updated_us") {
      if (!counter(*it) || it->get<uint64_t>() > uint64_t{INT64_MAX})
        return std::unexpected("metrics.updated_us outside signed clock range");
    } else if (!counter(*it)) return std::unexpected("metrics." + key + " must be a nonnegative integer");
  }
  if (!j.contains("probe")) return {};
  const auto& p = j["probe"];
  if (auto ok = keys(p, {"alive", "samples", "fps", "frames", "decoded", "decode_errors", "stalls",
                         "max_stall_ms", "synced", "offset_us", "e2e", "recv_us", "reassembly_us",
                         "buffer_us", "decode_us", "unavailable_reason"}, "metrics.probe"); !ok) return ok;
  for (auto it = p.begin(); it != p.end(); ++it) {
    const auto& key = it.key();
    bool valid = true;
    if (key == "e2e" || key == "recv_us" || key == "reassembly_us" || key == "buffer_us" || key == "decode_us") {
      if (auto ok = latency(*it, "metrics.probe." + key); !ok) return ok;
    } else if (key == "alive" || key == "synced") valid = it->is_boolean();
    else if (key == "fps") valid = nonnegative(*it);
    else if (key == "max_stall_ms") valid = it->is_null() || nonnegative(*it);
    else if (key == "offset_us") valid = it->is_number_integer() &&
        (!it->is_number_unsigned() || it->get<uint64_t>() <= uint64_t{INT64_MAX});
    else if (key == "unavailable_reason") valid = it->is_null() || it->is_string();
    else valid = counter(*it);
    if (!valid) return std::unexpected("metrics.probe." + key + " invalid type or value");
  }
  return {};
}
} // namespace

std::expected<ManualInput, std::string> load_manual_input(
    const std::string& path, const std::vector<Case>& cases) {
  std::ifstream file(path);
  if (!file) return std::unexpected("cannot open manual input: " + path);
  const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  try {
    const auto j = json::parse(text);
    if (auto ok = keys(j, {"version", "cases"}, "manual input"); !ok) return std::unexpected(ok.error());
    if (!j.contains("version") || !j["version"].is_number_integer() || j["version"] != 1 ||
        !j.contains("cases") || !j["cases"].is_array())
      return std::unexpected("manual input requires version 1 and cases array");
    std::set<std::string> ids;
    for (const auto& c : cases) ids.insert(c.id);
    ManualInput out;
    out.sha256 = cue::sha256_hex(text);
    for (const auto& entry : j["cases"]) {
      if (auto ok = keys(entry, {"id", "source", "diagnosis", "observations", "metrics"}, "manual case"); !ok)
        return std::unexpected(ok.error());
      if (!nonempty(entry, "id") || !nonempty(entry, "source") || !entry.contains("diagnosis") ||
          !entry.contains("observations") || !entry["observations"].is_array() || !entry.contains("metrics"))
        return std::unexpected("manual case requires id/source/diagnosis/observations/metrics");
      const auto id = entry["id"].get<std::string>();
      if (!ids.contains(id) || out.records.contains(id)) return std::unexpected("unknown or duplicate manual case: " + id);
      const auto& d = entry["diagnosis"];
      if (auto ok = keys(d, {"root_cause", "evidence"}, "diagnosis"); !ok) return std::unexpected(ok.error());
      if (!nonempty(d, "root_cause") || !d.contains("evidence") || !d["evidence"].is_array())
        return std::unexpected("diagnosis requires root_cause and evidence array");
      for (const auto& ev : d["evidence"]) if (!ev.is_string()) return std::unexpected("evidence must contain strings");
      for (const auto& ob : entry["observations"]) {
        if (auto ok = keys(ob, {"tool", "text", "data"}, "observation"); !ok) return std::unexpected(ok.error());
        if (!nonempty(ob, "tool") || !ob.contains("text") || !ob["text"].is_string() ||
            (ob.contains("data") && !ob["data"].is_object()))
          return std::unexpected("observation requires tool/text and optional object data");
      }
      if (auto ok = metrics(entry["metrics"]); !ok) return std::unexpected(ok.error());
      const auto& declared = *std::find_if(cases.begin(), cases.end(),
          [&](const Case& c) { return c.id == id; });
      const auto& supplied = entry["metrics"];
      if (declared.sla_dropped_max != UINT64_MAX && !supplied.contains("dropped"))
        return std::unexpected("active dropped_max SLA requires metrics.dropped");
      if (declared.sla_frame_completion_min > 0 &&
          (!supplied.contains("frames_in") || !supplied.contains("frames_out") || supplied["frames_in"] == 0))
        return std::unexpected("active frame_completion SLA requires frames_in>0 and frames_out");
      const bool probe_sla = has_value(declared.sla_lat_p99_ms) || has_value(declared.sla_lat_p50_ms) ||
          has_value(declared.sla_max_stall_ms) || declared.sla_stalls_max != UINT64_MAX;
      if (probe_sla && (!supplied.contains("probe") || !supplied["probe"].contains("samples") ||
                        supplied["probe"]["samples"] == 0))
        return std::unexpected("active endpoint SLA requires positive probe.samples");
      if (declared.sla_stalls_max != UINT64_MAX && !supplied["probe"].contains("stalls"))
        return std::unexpected("active stalls_max SLA requires probe.stalls");
      // The strict input schema above excludes every verdict field. Reuse the
      // state POD decoder only after validating all user supplied scalar types.
      json pod{{"id", id}, {"diagnosis", d}, {"observations", entry["observations"]},
               {"metrics", entry["metrics"]}};
      auto decoded = outcome_from_json(pod);
      if (!decoded) return std::unexpected(decoded.error());
      decoded->manual_source = entry["source"].get<std::string>();
      out.records.emplace(id, std::move(*decoded));
    }
    return out;
  } catch (const json::exception& e) {
    return std::unexpected(std::string("invalid manual input: ") + e.what());
  }
}

CaseOutcome grade_manual_record(const Case& c, const CaseOutcome* record,
                               const std::string& path, const std::string& hash) {
  CaseOutcome o;
  if (record) o = *record;
  o.id = c.id;
  o.evaluation_source = "manual_import";
  o.manual_input_path = path;
  o.manual_input_sha256 = hash;
  o.injection.fault_kind = c.fault_kind;
  o.injection.detail = "manual import has no independently observed fault window; unchecked";
  if (!record) { o.error_kind = "manual_missing_result"; return o; }
  agent::RunResult supplied;
  supplied.final_stage = agent::Stage::Done;
  supplied.diagnosis = o.diagnosis;
  o.score = judge_case(c, supplied, o.observations, o.metrics);
  // Imported endpoint values without samples cannot prove an endpoint SLA.
  const bool needs_probe = has_value(c.sla_lat_p99_ms) || has_value(c.sla_lat_p50_ms) ||
                           has_value(c.sla_max_stall_ms) || c.sla_stalls_max != UINT64_MAX;
  if (needs_probe && o.metrics.probe.samples == 0) o.score.sla = false;
  if (c.sla_frame_completion_min > 0 && o.metrics.frames_in == 0) o.score.sla = false;
  o.scored = true;
  o.final_stage = "manual_import";
  o.final_fps = o.metrics.fps;
  o.final_dropped = o.metrics.dropped;
  o.final_samples = o.metrics.samples;
  o.p99_ms_after = o.metrics.probe.e2e.p99;
  o.e2e_null_reason = "Imported metrics; not independently remeasured or authenticated in this run";
  return o;
}
} // namespace cue::eval
