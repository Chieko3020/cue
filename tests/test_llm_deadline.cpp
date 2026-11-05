// Real client/Runner driver. HTTP endpoints must be supplied by the local fixture.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <thread>
#include <vector>
#include "cue/agent/agent.h"
#include "cue/agent/session.h"
#include "cue/llm/cassette.h"
#include "cue/llm/client.h"
#include "cue/tool/tool.h"

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
namespace llm = cue::llm;
namespace agent = cue::agent;

namespace {
struct InspectClient : llm::ILlmClient {
  std::vector<Clock::time_point> deadlines;
  std::expected<llm::ChatResponse,std::string> chat(const llm::ChatRequest& req) override {
    if (!req.deadline) return std::unexpected("missing deadline");
    deadlines.push_back(*req.deadline);
    llm::ChatResponse r; r.content="local scripted response"; return r;
  }
};
int unit() {
  nlohmann::json checks;
  cue::tool::Registry tools;
  InspectClient client;
  agent::Runner::Config cfg;cfg.model="local-test";
  agent::Budget budget;budget.max_wall_s=1;budget.max_turns=3;
  agent::Runner runner(client,tools,cfg);(void)runner.run("unit",budget);
  checks["same_absolute_deadline_across_turns"]=client.deadlines.size()==3 &&
      client.deadlines[0]==client.deadlines[1] && client.deadlines[1]==client.deadlines[2];
  agent::LoadedSession resume;resume.stage=agent::Stage::Collect;
  resume.messages.push_back({.role="system",.content="unit",.tool_call_id={},.name={},.tool_calls={}});
  resume.messages.push_back({.role="user",.content="unit",.tool_call_id={},.name={},.tool_calls={}});
  resume.progress.wall_us=850000;
  cfg.resume=&resume;InspectClient resumed;agent::Runner resumed_runner(resumed,tools,cfg);
  (void)resumed_runner.run("unit",budget);
  checks["resume_does_not_round_to_full_second"]=!resumed.deadlines.empty() &&
      resumed.deadlines.front()-Clock::now()<170ms;
  resume.progress.wall_us=2000000;InspectClient expired;agent::Runner expired_runner(expired,tools,cfg);
  const auto result=expired_runner.run("unit",budget);
  checks["expired_runner_never_calls_client"]=expired.deadlines.empty() &&
      result.error.find("wall clock")!=std::string::npos;
  resume.progress.wall_us=std::numeric_limits<int64_t>::max();
  InspectClient huge;agent::Runner huge_runner(huge,tools,cfg);
  const auto huge_result=huge_runner.run("unit",budget);
  checks["int64_saved_wall_is_exhausted_not_wrapped"]=huge.deadlines.empty() &&
      huge_result.error.find("wall clock")!=std::string::npos;
  llm::ChatRequest a,b;b.deadline=Clock::now()+1s;
  std::atomic<bool> cancel{true};b.cancel=&cancel;
  checks["execution_controls_not_cassette_identity"]=llm::cassette_key(a)==llm::cassette_key(b) &&
      llm::cassette_request_json(a)==llm::cassette_request_json(b);
  llm::OpenAiCompatibleClient::Config local;
  local.base_url="http://127.0.0.1:1";local.api_key="c07-local-test";
  llm::OpenAiCompatibleClient http(local);llm::ChatRequest minimum;
  minimum.deadline=Clock::time_point::min();
  const auto minimum_result=http.chat(minimum);
  checks["minimum_timepoint_is_expired_without_subtraction"]=!minimum_result &&
      minimum_result.error().find("deadline")!=std::string::npos;
  bool passed=true;for(const auto& v:checks)passed=passed&&v.get<bool>();
  std::puts(nlohmann::json{{"passed",passed},{"checks",checks}}.dump().c_str());
  return passed?0:1;
}
}

int main(int argc,char** argv) {
  if(argc==2 && std::string(argv[1])=="--unit")return unit();
  if(argc==2 && std::string(argv[1])=="--cassette-key") {
    llm::ChatRequest req;
    req.messages.push_back({.role="user",.content="local deadline test",.tool_call_id={},.name={},.tool_calls={}});
    std::puts(llm::cassette_key(req,"local-test").c_str());return 0;
  }
  if(argc<2)return 2;
  const std::string scenario=argc>2?argv[2]:"shared-timeout";
  llm::OpenAiCompatibleClient::Config cfg;
  cfg.base_url=argv[1];cfg.api_key="c07-local-test";cfg.model="local-test";cfg.timeout_s=5;
  llm::ChatRequest req;req.timeout_s=1;
  req.messages.push_back({.role="user",.content="local deadline test",.tool_call_id={},.name={},.tool_calls={}});
  std::atomic<bool> cancelled{false};req.cancel=&cancelled;
  if(scenario=="config-cap"){cfg.timeout_s=1;req.timeout_s=5;}
  if(scenario=="absolute" || scenario=="expired" || scenario=="subms") {
    req.timeout_s=5;
    req.deadline=Clock::now()+(scenario=="absolute"?350ms:scenario=="expired"?-1ms:0ms);
    if(scenario=="subms")req.deadline=Clock::now()+100us;
  }
  if(scenario=="minimum")req.deadline=Clock::time_point::min();
  if(scenario=="retry-limit")req.timeout_s=5;
  if(scenario=="expired-cancel"){req.deadline=Clock::time_point::min();cancelled.store(true);}
  if(scenario=="preset-cancel"){cancelled.store(true);req.timeout_s=5;}
  if(scenario=="max0")cfg.max_retries=0;
  if(scenario=="overflow")cfg.max_response_bytes=16;
  if(scenario=="success" || scenario=="record-expiry" || scenario=="record-cancel") {cfg.timeout_s=3;req.timeout_s=3;if(argc>3)cfg.record_dir=argv[3];}
  if(scenario=="record-expiry")req.deadline=Clock::now()+250ms;
  int cancel_after=0;
  if(scenario=="cancel-200")cancel_after=50;
  if(scenario=="cancel-800")cancel_after=350;
  if(scenario=="cancel-transfer" || scenario=="runner-cancel" || scenario=="record-cancel")cancel_after=150;
  if(cancel_after)req.timeout_s=5;
  llm::OpenAiCompatibleClient client(cfg);
  const auto start=Clock::now();
  std::atomic<int64_t> cancel_at_us{-1};std::thread cancellation;
  if(cancel_after)cancellation=std::thread([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(cancel_after));
    cancel_at_us.store(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-start).count());
    cancelled.store(true);
  });
  std::string error,content;bool success=false;int prompt_tokens=0,tool_calls=0;
  if(scenario=="runner" || scenario=="runner-cancel") {
    cue::tool::Registry tools;agent::Runner::Config rc;rc.model="local-test";rc.cancel=&cancelled;
    agent::Runner runner(client,tools,rc);agent::Budget budget;budget.max_wall_s=scenario=="runner"?1:3;
    const auto r=runner.run("local endpoint budget",budget);error=r.error;success=r.ok();
  } else {
    const auto r=client.chat(req);success=r.has_value();
    if(r){content=r->content;prompt_tokens=r->usage.prompt_tokens;tool_calls=static_cast<int>(r->tool_calls.size());}
    else error=r.error();
  }
  const auto finished=Clock::now();
  const double elapsed=std::chrono::duration<double,std::milli>(finished-start).count();
  if(cancellation.joinable())cancellation.join();
  const auto cancel_us=cancel_at_us.load();
  const double cancel_latency=cancel_us<0?-1:elapsed-double(cancel_us)/1000;
  std::puts(nlohmann::json{{"scenario",scenario},{"elapsed_ms",elapsed},{"success",success},
      {"error",error},{"content",content},{"prompt_tokens",prompt_tokens},{"tool_calls",tool_calls},
      {"cancel_latency_ms",cancel_latency}}.dump().c_str());
  return 0;
}
