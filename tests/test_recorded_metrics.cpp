#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>

#include "cue/common/recorded_pipeline.h"
#include "cue/pipeline/http_pipeline.h"
#include "cue/tool/tools.h"

namespace p = cue::pipeline;
using json = nlohmann::json;
namespace {
int failures = 0;
void check(bool ok, const char* message) {
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
class Snapshot : public p::IServicePipeline {
 public:
  p::PipelineMetrics m;
  bool start(std::string&) override { return true; }
  void stop() override {}
  bool running() const override { return true; }
  p::PipelineMetrics latest() const override { return m; }
  p::ProbeMetrics probe_latest() const override { return {}; }
  void set_event_callback(std::function<void(const p::PipelineEvent&)>) override {}
  std::vector<p::PipelineEvent> recent_events(std::size_t) const override { return {}; }
  bool set_param(const std::string&, const std::string&, std::string&) override { return true; }
  bool inject_fault(const std::string&, double, std::string&) override { return true; }
  std::string observation(const std::string&, int, std::string&) const override { return {}; }
};
const char* keys[] = {"queue_dropped", "delayed_pkts", "delayed_max_us", "ingress_late", "control_ops"};
json query(p::IServicePipeline& pipe) {
  cue::tool::Registry registry;
  p::build_real_tools(registry, pipe, nullptr);
  auto result = registry.call("query_metrics", json::object());
  check(result.has_value(), "query_metrics succeeds on observed snapshot");
  return result ? result->data : json::object();
}
}
int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--inspect") {
    p::ReplayPipeline replay(argv[2]);
    if (!replay.load_error().empty()) { std::fprintf(stderr, "%s\n", replay.load_error().c_str()); return 2; }
    std::puts(query(replay).dump().c_str());
    return failures != 0;
  }
  if (argc == 3 && std::string(argv[1]) == "--http") {
    p::PipelineEndpoint ep; ep.http_base = argv[2];
    p::HttpServicePipeline http(ep);
    std::puts(query(http).dump().c_str());
    return failures != 0;
  }
  const auto dir = std::filesystem::temp_directory_path() / ("cue-recorded-metrics-" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  for (const bool known : {false, true}) {
    for (const uint64_t value : {uint64_t{0}, uint64_t{137}, UINT64_MAX}) {
      auto fake = std::make_unique<Snapshot>();
      fake->m.samples = 1; fake->m.fps = 30;
      auto& m = fake->m;
      m.queue_dropped = m.delayed_pkts = m.delayed_max_us = m.ingress_late = m.control_ops = value;
      m.queue_dropped_observed = m.delayed_pkts_observed = m.delayed_max_us_observed =
          m.ingress_late_observed = m.control_ops_observed = known;
      m.control_channel = known ? "uds" : "";
      json before;
      {
        p::RecordingPipeline recording(std::move(fake), dir.string(), "roundtrip");
        before = query(recording);
        check(recording.io_error().empty(), "recording writable");
      }
      const auto file = p::tools_record_path(dir.string(), "roundtrip");
      p::ReplayPipeline replay(file);
      check(replay.load_error().empty(), "recording loads");
      const auto after = query(replay);
      for (const auto* key : keys) {
        check(before[key] == after[key], "actual tool counter survives roundtrip");
        check(known ? after[key] == json(value) : after[key].is_null(), "unknown, actual zero and full uint64 are distinct");
      }
      check(before["control_channel"] == after["control_channel"], "control channel survives roundtrip");
      check(replay.stats().misses == 0, "complete query replay no miss");
    }
  }
  const auto file = p::tools_record_path(dir.string(), "roundtrip");
  std::ifstream in(file);
  std::vector<json> original;
  std::string line;
  while (std::getline(in, line)) original.push_back(json::parse(line));
  in.close();
  const std::vector<json> invalid = {nullptr, -1, 1.5, "0", true, json::object(), json::array()};
  for (std::size_t variant = 0; variant <= invalid.size(); ++variant) {
    auto rows = original;
    for (auto& row : rows) if (row["op"] == "latest") {
      for (const auto* key : keys) {
        if (variant == invalid.size()) row["ret"].erase(key);
        else row["ret"][key] = invalid[variant];
      }
      row["ret"].erase("control_channel");
    }
    { std::ofstream out(file); for (const auto& row : rows) out << row.dump() << '\n'; }
    p::ReplayPipeline legacy(file);
    check(legacy.load_error().empty(), "v1 absent/invalid additive counters load as unknown");
    auto result = query(legacy);
    for (const auto* key : keys) check(result[key].is_null(), "legacy absent/null/invalid counter never becomes zero");
    check(result["control_channel"].is_null(), "missing control channel unknown");
    check(legacy.stats().misses == 0, "unknown metadata is not a replay miss");
  }
  std::filesystem::remove_all(dir);
  return failures != 0;
}
