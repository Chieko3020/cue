// Real file -> strict manual loader -> Engine/state/report, using synthetic data.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include "cue/eval/eval.h"
#include "cue/eval/manual.h"
#include "cue/eval/state.h"

namespace {
namespace ev = cue::eval;
using json = nlohmann::json;
int failures = 0;
void check(bool yes, const char* name) { if (!yes) { ++failures; std::printf("FAIL: %s\n", name); } }
void write(const std::filesystem::path& p, const json& j) { std::ofstream(p) << j.dump(2); }
}
int main() {
  const auto dir = std::filesystem::temp_directory_path() / ("cue-manual-" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  const auto input = dir / "input.json";
  ev::Case c;
  c.id = "manual-case"; c.manual_only = true; c.fault_kind = "rate";
  c.expect_root_cause = "fec_ghost_frame"; // deliberately outside the normal agent enum
  c.evidence_candidates = {"budget_drops>0"}; c.sla_fps_min = 25;
  json item{{"id", c.id}, {"source", "synthetic unit fixture; not real device evidence"},
    {"diagnosis", {{"root_cause", c.expect_root_cause}, {"evidence", {"budget_drops=82"}}}},
    {"observations", {{{"tool", "query_metrics"}, {"text", "budget_drops=82"},
                         {"data", {{"budget_drops", 82}}}}}},
    {"metrics", {{"fps", 30.0}, {"samples", 1}}}};
  const json good{{"version", 1}, {"cases", {item}}};
  write(input, good);
  auto loaded = ev::load_manual_input(input.string(), {c});
  check(bool(loaded), "valid strict input loads");
  if (!loaded) { std::printf("%s\n", loaded.error().c_str()); return 1; }
  auto graded = ev::grade_manual_record(c, &loaded->records.at(c.id), input.string(), loaded->sha256);
  check(graded.scored && graded.score.all() && graded.evaluation_source == "manual_import" &&
        graded.manual_source == item["source"].get<std::string>() && !graded.injection.checked,
        "manual non-enum diagnosis graded, provenance retained, injection stays unchecked");
  const auto read = ev::outcome_from_json(ev::outcome_to_json(graded));
  check(read && read->manual_input_sha256 == loaded->sha256 && read->manual_source == graded.manual_source &&
        read->evaluation_source == "manual_import" && read->score.all(), "state roundtrip preserves source and verdict");
  auto bad = good;
  const auto rejects = [&](const json& j, const char* name) { write(input, j); check(!ev::load_manual_input(input.string(), {c}), name); };
  bad["version"] = 2; rejects(bad, "bad version rejected");
  bad = good; bad["score"] = true; rejects(bad, "unknown root field rejected");
  bad = good; bad["cases"][0]["scored"] = true; rejects(bad, "supplied verdict rejected");
  bad = good; bad["cases"].push_back(item); rejects(bad, "duplicate id rejected");
  bad = good; bad["cases"][0]["id"] = "unknown"; rejects(bad, "unknown id rejected");
  bad = good; bad["cases"][0]["source"] = "   "; rejects(bad, "empty source rejected");
  bad = good; bad["cases"][0]["diagnosis"]["confidence"] = 1; rejects(bad, "unknown diagnosis key rejected");
  bad = good; bad["cases"][0]["diagnosis"]["evidence"] = {1}; rejects(bad, "non-string evidence rejected");
  bad = good; bad["cases"][0]["observations"][0]["data"] = 1; rejects(bad, "nonobject observation data rejected");
  bad = good; bad["cases"][0]["observations"][0]["unknown"] = 1; rejects(bad, "unknown observation field rejected");
  for (const auto& value : {json(nullptr), json("30"), json(true), json(-1)}) {
    bad = good; bad["cases"][0]["metrics"]["fps"] = value; rejects(bad, "invalid fps rejected");
  }
  for (const auto& value : {json(nullptr), json("1"), json(true), json(-1), json(0)}) {
    bad = good; bad["cases"][0]["metrics"]["samples"] = value; rejects(bad, "invalid samples rejected");
  }
  bad = good; bad["cases"][0]["metrics"]["frames_in"] = -1; rejects(bad, "negative counter rejected");
  bad = good; bad["cases"][0]["metrics"]["updated_us"] = uint64_t{INT64_MAX} + 1;
  rejects(bad, "out-of-range signed timestamp rejected");
  bad = good; bad["cases"][0]["metrics"]["e2e_invented"] = 0; rejects(bad, "unknown metric rejected");
  bad = good; bad["cases"][0]["metrics"]["recv"] = {{"x", 1}}; rejects(bad, "unknown latency field rejected");
  bad = good; bad["cases"][0]["metrics"]["probe"] = {{"samples", -1}}; rejects(bad, "negative probe count rejected");
  bad = good; bad["cases"][0]["metrics"]["probe"] = {{"alive", "yes"}}; rejects(bad, "wrong probe boolean rejected");
  std::ofstream(input) << "{\"version\":1,\"cases\":[],\"overflow\":1e999}";
  check(!ev::load_manual_input(input.string(), {c}), "non-finite parse rejected");
  write(input, good);
  auto drop_required = c; drop_required.sla_dropped_max = 0;
  check(!ev::load_manual_input(input.string(), {drop_required}), "missing active dropped SLA counter rejected");
  auto explicit_zero = good; explicit_zero["cases"][0]["metrics"]["dropped"] = 0;
  write(input, explicit_zero);
  check(bool(ev::load_manual_input(input.string(), {drop_required})), "actual supplied zero dropped is accepted");
  auto stalls_required = c; stalls_required.sla_stalls_max = 0;
  explicit_zero["cases"][0]["metrics"]["probe"] = {{"samples", 1}};
  write(input, explicit_zero);
  check(!ev::load_manual_input(input.string(), {stalls_required}), "missing active stalls SLA counter rejected");
  explicit_zero["cases"][0]["metrics"]["probe"]["stalls"] = 0;
  write(input, explicit_zero);
  check(bool(ev::load_manual_input(input.string(), {stalls_required})), "actual supplied zero stalls is accepted");
  auto frames_required = c; frames_required.sla_frame_completion_min = .9;
  write(input, good);
  check(!ev::load_manual_input(input.string(), {frames_required}), "missing frame completion counts rejected");
  check(!ev::load_manual_input((dir / "missing").string(), {c}), "missing input rejected");
  auto empty_observations = graded;
  empty_observations.observations.clear();
  check(!ev::grade_manual_record(c, &empty_observations, "p", "h").score.evidence,
        "empty observations do not manufacture evidence");
  auto low = graded; low.metrics.fps = 1;
  check(!ev::grade_manual_record(c, &low, "p", "h").score.sla, "imported low FPS fails SLA");
  auto endpoint = c; endpoint.sla_lat_p99_ms = 100;
  auto no_samples = graded; no_samples.metrics.probe.e2e.p99 = 1;
  check(!ev::grade_manual_record(endpoint, &no_samples, "p", "h").score.sla,
        "endpoint number without samples cannot prove SLA");
  ev::RunConfig cfg;
  cfg.agent_mode = "manual"; cfg.manual_input = input.string(); cfg.out_dir = (dir / "runs").string();
  cfg.cued_bin = "/nonexistent-cued"; cfg.probe_bin = "/nonexistent-probe"; cfg.capture_bin = "/nonexistent-capture";
  cfg.suite_path = (dir / "suite.json").string();
  write(cfg.suite_path, json{{"cases", {{{"id", c.id}, {"manual_only", true},
                          {"expect", {{"root_cause", c.expect_root_cause}}}}}}});
  ev::Engine first(cfg);
  const auto before_hash = first.config_hash();
  const auto another_path = dir / "different-input.json";
  auto other_input = good; other_input["cases"][0]["source"] = "different authored provenance";
  write(another_path, other_input);
  auto other_cfg = cfg; other_cfg.manual_input = another_path.string();
  ev::Engine other(other_cfg);
  check(other.config_hash() != before_hash, "public config hash distinguishes files before any run");
  auto report = first.run({c});
  check(report.fatal_error.empty() && report.executed == 1 && report.scored == 1 && report.cases[0].score.all(),
        "Engine manual path grades without backend binaries");
  check(first.config_hash() == before_hash, "public config hash is unchanged before and after run");
  const auto markdown = first.render_markdown(report);
  const auto rendered = json::parse(first.render_json(report));
  check(markdown.find("manual_import") != std::string::npos && markdown.find("未重新测量") != std::string::npos &&
        rendered["manual_input_sha256"] == loaded->sha256 &&
        rendered["cases"][0]["manual_source"] == item["source"], "MD/JSON disclose imported provenance");
  cfg.resume_run_id = report.run_id;
  ev::Engine resumed(cfg); const auto same = resumed.run({c});
  check(same.fatal_error.empty() && same.fingerprint == report.fingerprint && same.cases[0].score.all(), "unchanged input resumes cache");
  bad = good; bad["cases"][0]["diagnosis"]["evidence"] = {"budget_drops=0"};
  bad["cases"][0]["observations"][0] = {{"tool", "query_metrics"}, {"text", "budget_drops=0"}, {"data", {{"budget_drops", 0}}}};
  write(input, bad);
  ev::Engine changed(cfg); const auto changed_report = changed.run({c});
  check(changed_report.fingerprint != report.fingerprint && !changed_report.cases[0].score.evidence,
        "changed input hash invalidates prior verdict");
  const auto rerun = first.run({c});
  const auto old_again = json::parse(first.render_json(report));
  check(rerun.manual_input_sha256 != report.manual_input_sha256 &&
        old_again["manual_input_sha256"] == loaded->sha256 &&
        old_again["cases"][0]["manual_input_sha256"] == loaded->sha256 &&
        first.render_markdown(report).find(loaded->sha256) != std::string::npos,
        "rendering old report after a second run retains original input snapshot");
  cfg.resume_run_id.clear(); write(input, json{{"version", 1}, {"cases", json::array()}});
  ev::Engine missing(cfg); const auto missing_report = missing.run({c});
  check(missing_report.run_errors == 1 && missing_report.cases[0].error_kind == "manual_missing_result", "missing planned record is run_error");
  std::filesystem::remove_all(dir);
  std::printf("test_manual: %d failure(s)\n", failures);
  return failures ? 1 : 0;
}
