#include "cue/eval/state.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "cue/common/sha256.h"

namespace cue::eval {

namespace {

std::string git_describe() {
  std::string out;
  FILE* p = ::popen("git describe --always --dirty 2>/dev/null", "r");
  if (!p) return out;
  char buf[128];
  while (std::fgets(buf, sizeof(buf), p)) out += buf;
  ::pclose(p);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  return out;
}

// 进程自身二进制哈希：只算一次（约 10 MB，续跑与多 case 都要用）
const std::string& self_binary_hash() {
  static const std::string h = cue::sha256_file("/proc/self/exe");
  return h;
}

}  // namespace

std::string base_fingerprint(const std::string& suite_path) {
  const std::string suite_hash = cue::sha256_file(suite_path);
  const std::string git = git_describe();
  return cue::sha256_hex(self_binary_hash() + "|" + git + "|" + suite_hash);
}

std::string fingerprint_for(const std::string& suite_path, const Case& c) {
  return cue::sha256_hex(base_fingerprint(suite_path) + "|" + c.id + "|" + std::to_string(c.seed));
}

json outcome_to_json(const CaseOutcome& o) {
  return json{{"id", o.id},
              {"scored", o.scored},
              {"error_kind", o.error_kind},
              {"score", {{"root", o.score.root}, {"evidence", o.score.evidence}, {"sla", o.score.sla}}},
              {"turns", o.turns},
              {"cost_yuan", o.cost_yuan},
              {"diagnosis",
               {{"root_cause", o.diagnosis.root_cause},
                {"evidence", o.diagnosis.evidence},
                {"confidence", o.diagnosis.confidence}}},
              {"final_stage", o.final_stage},
              {"final_fps", o.final_fps},
              {"final_dropped", o.final_dropped},
              {"final_samples", o.final_samples}};
}

std::expected<CaseOutcome, std::string> outcome_from_json(const json& j) {
  if (!j.is_object()) return std::unexpected("outcome 不是对象");
  if (!j.contains("id") || !j["id"].is_string()) return std::unexpected("outcome 缺少 id");
  CaseOutcome o;
  o.id = j["id"].get<std::string>();
  o.scored = j.value("scored", false);
  o.error_kind = j.value("error_kind", std::string());
  if (j.contains("score") && j["score"].is_object()) {
    const auto& s = j["score"];
    o.score.root = s.value("root", false);
    o.score.evidence = s.value("evidence", false);
    o.score.sla = s.value("sla", false);
  }
  o.turns = j.value("turns", 0);
  o.cost_yuan = j.value("cost_yuan", 0.0);
  if (j.contains("diagnosis") && j["diagnosis"].is_object()) {
    const auto& d = j["diagnosis"];
    o.diagnosis.root_cause = d.value("root_cause", std::string());
    o.diagnosis.confidence = d.value("confidence", 0.0);
    if (d.contains("evidence") && d["evidence"].is_array()) {
      for (const auto& x : d["evidence"]) {
        if (x.is_string()) o.diagnosis.evidence.push_back(x.get<std::string>());
      }
    }
  }
  o.final_stage = j.value("final_stage", std::string("n/a"));
  o.final_fps = j.value("final_fps", 0.0);
  o.final_dropped = j.value("final_dropped", uint64_t{0});
  o.final_samples = j.value("final_samples", uint64_t{0});
  return o;
}

bool append_state_header(const std::string& path, const json& header, std::string& err) {
  std::error_code ec;
  const auto parent = std::filesystem::path(path).parent_path();
  if (!parent.empty()) std::filesystem::create_directories(parent, ec);
  std::ofstream out(path, std::ios::app);
  if (!out) {
    err = "无法写入 " + path;
    return false;
  }
  out << header.dump() << "\n";
  out.flush();
  if (!out) {
    err = "写入失败 " + path;
    return false;
  }
  return true;
}

bool append_state(const std::string& path, int case_index, int k, const CaseOutcome& o,
                  const std::string& fingerprint, std::string& err) {
  json line{{"t", "case"},
            {"ci", case_index},
            {"k", k},
            {"fp", fingerprint},
            {"outcome", outcome_to_json(o)}};
  return append_state_header(path, line, err);
}

bool load_state(const std::string& path, StateMap& out, std::string& err) {
  std::ifstream in(path);
  if (!in) {
    err = "无法打开 state: " + path;
    return false;
  }
  std::string line;
  int lineno = 0;
  while (std::getline(in, line)) {
    ++lineno;
    if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
    auto j = json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
      err = "state 第 " + std::to_string(lineno) + " 行不是合法 JSON";
      return false;
    }
    if (j.value("t", std::string()) != "case") continue;
    const int ci = j.value("ci", -1);
    const int k = j.value("k", -1);
    if (ci < 0 || k < 0) {
      err = "state 第 " + std::to_string(lineno) + " 行缺少 ci/k";
      return false;
    }
    auto o = outcome_from_json(j.value("outcome", json::object()));
    if (!o) {
      err = "state 第 " + std::to_string(lineno) + " 行: " + o.error();
      return false;
    }
    out[{ci, k}] = CachedOutcome{*o, j.value("fp", std::string())};
  }
  return true;
}

std::expected<json, std::string> read_state_header(const std::string& path) {
  std::ifstream in(path);
  if (!in) return std::unexpected("无法打开 state: " + path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
    auto j = json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return std::unexpected("state header 不是合法 JSON");
    if (j.value("t", std::string()) == "run") return j;
  }
  return std::unexpected("state 里没有 run header");
}

}  // namespace cue::eval
