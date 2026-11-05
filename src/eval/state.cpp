#include "cue/eval/state.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

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

// 取不到的值（NaN）落成 null：JSON 没有 NaN，写 0 会被当成真实读数
json num_or_null(double v) { return has_value(v) ? json(v) : json(nullptr); }

double read_num(const json& j, const char* k, double fallback) {
  if (!j.contains(k) || !j[k].is_number()) return fallback;
  return j[k].get<double>();
}

json latency_to_json(const Latency& l) {
  return json{{"p50", num_or_null(l.p50)}, {"p95", num_or_null(l.p95)}, {"p99", num_or_null(l.p99)}};
}

Latency latency_from_json(const json& j) {
  Latency l;
  if (!j.is_object()) return l;
  l.p50 = read_num(j, "p50", kNoValue);
  l.p95 = read_num(j, "p95", kNoValue);
  l.p99 = read_num(j, "p99", kNoValue);
  return l;
}

// 端到端（探针）快照：NaN 一律落 null，回读还原 NaN。state.jsonl 是续跑的缓存，
// 少了这一块，续跑出来的报告会缺少端到端字段（看起来"这次没测"）。
json probe_to_json(const ProbeSummary& p) {
  return json{{"alive", p.alive},
              {"samples", p.samples},
              {"fps", p.fps},
              {"frames", p.frames},
              {"decoded", p.decoded},
              {"decode_errors", p.decode_errors},
              {"stalls", p.stalls},
              {"max_stall_ms", num_or_null(p.max_stall_ms)},
              {"synced", p.synced},
              {"offset_us", p.offset_us},
              {"e2e", latency_to_json(p.e2e)},
              {"recv_us", latency_to_json(p.recv_us)},
              {"reassembly_us", latency_to_json(p.reassembly_us)},
              {"buffer_us", latency_to_json(p.buffer_us)},
              {"decode_us", latency_to_json(p.decode_us)},
              {"unavailable_reason",
               p.unavailable_reason.empty() ? json(nullptr) : json(p.unavailable_reason)}};
}

ProbeSummary probe_from_json(const json& j) {
  ProbeSummary p;
  if (!j.is_object()) return p;
  p.alive = j.value("alive", false);
  p.synced = j.value("synced", false);
  p.fps = read_num(j, "fps", 0.0);
  const auto u64 = [&j](const char* k) -> uint64_t {
    if (!j.contains(k) || !j[k].is_number_integer()) return 0;
    return j[k].get<uint64_t>();
  };
  p.samples = u64("samples");
  p.frames = u64("frames");
  p.decoded = u64("decoded");
  p.decode_errors = u64("decode_errors");
  p.stalls = u64("stalls");
  p.max_stall_ms = read_num(j, "max_stall_ms", kNoValue);
  if (j.contains("offset_us") && j["offset_us"].is_number_integer()) {
    p.offset_us = j["offset_us"].get<int64_t>();
  }
  if (j.contains("e2e")) p.e2e = latency_from_json(j["e2e"]);
  if (j.contains("recv_us")) p.recv_us = latency_from_json(j["recv_us"]);
  if (j.contains("reassembly_us")) p.reassembly_us = latency_from_json(j["reassembly_us"]);
  if (j.contains("buffer_us")) p.buffer_us = latency_from_json(j["buffer_us"]);
  if (j.contains("decode_us")) p.decode_us = latency_from_json(j["decode_us"]);
  if (j.contains("unavailable_reason") && j["unavailable_reason"].is_string()) {
    p.unavailable_reason = j["unavailable_reason"].get<std::string>();
  }
  return p;
}

json metrics_to_json(const Metrics& m) {
  return json{{"fps", m.fps},
              {"frames_in", m.frames_in},
              {"frames_out", m.frames_out},
              {"dropped", m.dropped},
              {"injected", m.injected},
              {"decoded", m.decoded},
              {"decode_errors", m.decode_errors},
              {"recv", latency_to_json(m.recv)},
              {"buffer", latency_to_json(m.buffer)},
              {"decode", latency_to_json(m.decode)},
              {"op", latency_to_json(m.op)},
              {"encode", latency_to_json(m.encode)},
              {"send", latency_to_json(m.send)},
              {"updated_us", m.updated_us},
              {"samples", m.samples},
              {"probe", probe_to_json(m.probe)}};
}

// 全字段 is_* 校验：state 行被手改/写坏时给结构化错误，而不是 nlohmann 类型异常
Metrics metrics_from_json(const json& j, std::string& err) {
  Metrics m;
  if (j.is_null()) return m;
  if (!j.is_object()) {
    err = "metrics 不是对象";
    return m;
  }
  const auto num = [&](const char* k, double& out) {
    if (!j.contains(k)) return true;
    if (j[k].is_null()) return true;  // null = 取不到（NaN）
    if (!j[k].is_number()) {
      err = std::string("metrics.") + k + " 不是数字";
      return false;
    }
    out = j[k].get<double>();
    return true;
  };
  const auto u64 = [&](const char* k, uint64_t& out) {
    if (!j.contains(k)) return true;
    if (!j[k].is_number_unsigned() && !j[k].is_number_integer()) {
      err = std::string("metrics.") + k + " 不是整数";
      return false;
    }
    out = j[k].get<uint64_t>();
    return true;
  };
  if (!num("fps", m.fps)) return m;
  if (!u64("frames_in", m.frames_in) || !u64("frames_out", m.frames_out) ||
      !u64("dropped", m.dropped) || !u64("injected", m.injected) || !u64("decoded", m.decoded) ||
      !u64("decode_errors", m.decode_errors) || !u64("samples", m.samples)) {
    return m;
  }
  if (j.contains("updated_us") && !j["updated_us"].is_number_integer()) {
    err = "metrics.updated_us 不是整数";
    return m;
  }
  if (j.contains("updated_us")) m.updated_us = j["updated_us"].get<int64_t>();
  if (j.contains("recv")) m.recv = latency_from_json(j["recv"]);
  if (j.contains("buffer")) m.buffer = latency_from_json(j["buffer"]);
  if (j.contains("decode")) m.decode = latency_from_json(j["decode"]);
  if (j.contains("op")) m.op = latency_from_json(j["op"]);
  if (j.contains("encode")) m.encode = latency_from_json(j["encode"]);
  if (j.contains("send")) m.send = latency_from_json(j["send"]);
  if (j.contains("probe")) m.probe = probe_from_json(j["probe"]);
  return m;
}

json observations_to_json(const std::vector<Observation>& obs) {
  json arr = json::array();
  for (const auto& x : obs) arr.push_back(json{{"tool", x.tool}, {"text", x.text}, {"data", x.data}});
  return arr;
}

}  // namespace

std::string base_fingerprint(const std::string& suite_path, const std::string& config_hash) {
  const std::string suite_hash = cue::sha256_file(suite_path);
  const std::string git = git_describe();
  return cue::sha256_hex(self_binary_hash() + "|" + git + "|" + suite_hash + "|" + config_hash);
}

std::string fingerprint_from_base(const std::string& base, const Case& c) {
  return cue::sha256_hex(base + "|" + c.id + "|" + std::to_string(c.seed));
}

std::string fingerprint_for(const std::string& suite_path, const Case& c, const std::string& config_hash) {
  return fingerprint_from_base(base_fingerprint(suite_path, config_hash), c);
}

RunLock::~RunLock() {
  if (fd_ >= 0) {
    ::flock(fd_, LOCK_UN);
    ::close(fd_);
    fd_ = -1;
  }
}

std::expected<void, std::string> RunLock::acquire(const std::string& dir) {
  if (fd_ >= 0) return std::unexpected("RunLock 已持有");
  const std::string path = dir + "/run.lock";
  const int fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
  if (fd < 0) return std::unexpected("无法打开锁文件 " + path + ": " + std::strerror(errno));
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    const int e = errno;
    ::close(fd);
    if (e == EWOULDBLOCK) {
      return std::unexpected("该 run 正被另一个进程评测（" + path + "），拒绝并发续跑（会双份计费）");
    }
    return std::unexpected("加锁失败 " + path + ": " + std::strerror(e));
  }
  fd_ = fd;
  return {};
}

json outcome_to_json(const CaseOutcome& o) {
  return json{{"id", o.id},
              {"evaluation_source", o.evaluation_source},
              {"manual_source", o.manual_source},
              {"manual_input_path", o.manual_input_path},
              {"manual_input_sha256", o.manual_input_sha256},
              {"scored", o.scored},
              {"rate_eligible", rate_eligible(o)},
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
              {"final_samples", o.final_samples},
              {"metrics", metrics_to_json(o.metrics)},
              {"mttr_s", num_or_null(o.mttr_s)},
              {"p99_ms_before", num_or_null(o.p99_ms_before)},
              {"p99_ms_fault", num_or_null(o.p99_ms_fault)},
              {"p99_ms_after", num_or_null(o.p99_ms_after)},
              {"e2e_null_reason", o.e2e_null_reason.empty() ? json(nullptr)
                                                            : json(o.e2e_null_reason)},
              {"cassette_hits", o.cassette_hits},
              {"cassette_misses", o.cassette_misses},
              // 注入 trace 的落点（空串 = 本 case 不落 trace）。续跑复用缓存时要能
              // 从 state 里读回来，报告才列得出"这次 run 的 trace 在哪"。
              {"inject_trace", o.inject_trace},
              // 注入是否真的生效（N-23）：续跑复用缓存时这个判定必须一起回来，
              // 否则 resume 出来的 run 会把"注入未生效"悄悄洗掉。
              {"injection",
               {{"fault_kind", o.injection.fault_kind},
                {"checked", o.injection.checked},
                {"effective", o.injection.effective},
                {"budget_drops", o.injection.budget_drops},
                {"injected", o.injection.injected},
                {"detail", o.injection.detail}}},
              {"observations", observations_to_json(o.observations)}};
}

std::expected<CaseOutcome, std::string> outcome_from_json(const json& j) {
  if (!j.is_object()) return std::unexpected("outcome 不是对象");
  if (!j.contains("id") || !j["id"].is_string()) return std::unexpected("outcome 缺少 id");
  try {
    CaseOutcome o;
    o.id = j["id"].get<std::string>();
    for (auto [key, field] : {std::pair{"evaluation_source", &o.evaluation_source},
                              std::pair{"manual_source", &o.manual_source},
                              std::pair{"manual_input_path", &o.manual_input_path},
                              std::pair{"manual_input_sha256", &o.manual_input_sha256}}) {
      if (j.contains(key)) {
        if (!j[key].is_string()) return std::unexpected(std::string(key) + " must be a string");
        *field = j[key].get<std::string>();
      }
    }
    if (j.contains("scored")) {
      if (!j["scored"].is_boolean()) return std::unexpected("outcome.scored 不是布尔");
      o.scored = j["scored"].get<bool>();
    }
    if (j.contains("error_kind")) {
      if (!j["error_kind"].is_string()) return std::unexpected("outcome.error_kind 不是字符串");
      o.error_kind = j["error_kind"].get<std::string>();
    }
    if (j.contains("score") && j["score"].is_object()) {
      const auto& s = j["score"];
      const auto flag = [&](const char* k) {
        return s.contains(k) && s[k].is_boolean() && s[k].get<bool>();
      };
      o.score.root = flag("root");
      o.score.evidence = flag("evidence");
      o.score.sla = flag("sla");
    }
    if (j.contains("turns")) {
      if (!j["turns"].is_number_integer()) return std::unexpected("outcome.turns 不是整数");
      o.turns = j["turns"].get<int>();
    }
    if (j.contains("cost_yuan")) {
      if (!j["cost_yuan"].is_number()) return std::unexpected("outcome.cost_yuan 不是数字");
      o.cost_yuan = j["cost_yuan"].get<double>();
    }
    if (j.contains("diagnosis") && j["diagnosis"].is_object()) {
      const auto& d = j["diagnosis"];
      if (d.contains("root_cause") && d["root_cause"].is_string()) {
        o.diagnosis.root_cause = d["root_cause"].get<std::string>();
      }
      o.diagnosis.confidence = read_num(d, "confidence", 0.0);
      if (d.contains("evidence") && d["evidence"].is_array()) {
        for (const auto& x : d["evidence"]) {
          if (x.is_string()) o.diagnosis.evidence.push_back(x.get<std::string>());
        }
      }
    }
    if (j.contains("final_stage") && j["final_stage"].is_string()) {
      o.final_stage = j["final_stage"].get<std::string>();
    } else {
      o.final_stage = "n/a";
    }
    o.final_fps = read_num(j, "final_fps", 0.0);
    const auto read_u64 = [&](const char* k, uint64_t& out) {
      if (!j.contains(k)) return;
      if (j[k].is_number_unsigned() || j[k].is_number_integer()) out = j[k].get<uint64_t>();
    };
    read_u64("final_dropped", o.final_dropped);
    read_u64("final_samples", o.final_samples);
    read_u64("cassette_hits", o.cassette_hits);
    read_u64("cassette_misses", o.cassette_misses);
    if (j.contains("inject_trace") && j["inject_trace"].is_string()) {
      o.inject_trace = j["inject_trace"].get<std::string>();
    }
    // 旧 state 没有 injection 段：缺省 = 未判定（checked=false），不能补成"生效"。
    if (j.contains("injection") && j["injection"].is_object()) {
      const auto& inj = j["injection"];
      if (inj.contains("fault_kind") && inj["fault_kind"].is_string()) {
        o.injection.fault_kind = inj["fault_kind"].get<std::string>();
      }
      o.injection.checked = inj.value("checked", false);
      o.injection.effective = inj.value("effective", false);
      o.injection.budget_drops = inj.value("budget_drops", uint64_t{0});
      o.injection.injected = inj.value("injected", uint64_t{0});
      if (inj.contains("detail") && inj["detail"].is_string()) {
        o.injection.detail = inj["detail"].get<std::string>();
      }
    }
    if (j.contains("metrics")) {
      std::string merr;
      o.metrics = metrics_from_json(j["metrics"], merr);
      if (!merr.empty()) return std::unexpected(merr);
    }
    o.mttr_s = read_num(j, "mttr_s", kNoValue);
    o.p99_ms_before = read_num(j, "p99_ms_before", kNoValue);
    o.p99_ms_fault = read_num(j, "p99_ms_fault", kNoValue);
    o.p99_ms_after = read_num(j, "p99_ms_after", kNoValue);
    if (j.contains("e2e_null_reason") && j["e2e_null_reason"].is_string()) {
      o.e2e_null_reason = j["e2e_null_reason"].get<std::string>();
    }
    if (j.contains("observations") && j["observations"].is_array()) {
      for (const auto& x : j["observations"]) {
        if (!x.is_object()) continue;
        Observation ob;
        if (x.contains("tool") && x["tool"].is_string()) ob.tool = x["tool"].get<std::string>();
        if (x.contains("text") && x["text"].is_string()) ob.text = x["text"].get<std::string>();
        if (x.contains("data") && x["data"].is_object()) ob.data = x["data"];
        o.observations.push_back(std::move(ob));
      }
    }
    return o;
  } catch (const nlohmann::json::exception& e) {
    // 进程边界不允许异常穿透（SPEC §9）：state 被改坏时返回结构化错误
    return std::unexpected(std::string("outcome 解析失败：") + e.what());
  }
}

std::expected<void, std::string> append_line(const std::string& path, const json& line) {
  std::error_code ec;
  const auto parent = std::filesystem::path(path).parent_path();
  if (!parent.empty()) std::filesystem::create_directories(parent, ec);
  std::ofstream out(path, std::ios::app);
  if (!out) return std::unexpected("无法写入 " + path);
  out << line.dump() << "\n";
  out.flush();
  if (!out) return std::unexpected("写入失败 " + path);
  return {};
}

std::expected<void, std::string> append_run_header(const std::string& path, const json& header) {
  return append_line(path, header);
}

std::expected<void, std::string> append_state(const std::string& path, int case_index, int k,
                                              const CaseOutcome& o, const std::string& fingerprint) {
  json line{{"t", "case"},
            {"ci", case_index},
            {"k", k},
            {"fp", fingerprint},
            {"outcome", outcome_to_json(o)}};
  return append_line(path, line);
}

std::expected<StateMap, std::string> load_state(const std::string& path) {
  std::ifstream in(path);
  if (!in) return std::unexpected("无法打开 state: " + path);

  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) lines.push_back(line);

  StateMap out;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const bool last = (i + 1 == lines.size());
    if (lines[i].find_first_not_of(" \t\r") == std::string::npos) continue;
    auto j = json::parse(lines[i], nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
      // 崩溃只会截断最后一行：丢掉它继续续跑；中间行坏了说明文件真的坏了
      if (last) break;
      return std::unexpected("state 第 " + std::to_string(i + 1) + " 行不是合法 JSON");
    }
    if (!j.contains("t") || !j["t"].is_string()) {
      if (last) break;
      return std::unexpected("state 第 " + std::to_string(i + 1) + " 行缺少 t");
    }
    if (j["t"].get<std::string>() != "case") continue;
    if (!j.contains("ci") || !j.contains("k") || !j["ci"].is_number_integer() ||
        !j["k"].is_number_integer()) {
      return std::unexpected("state 第 " + std::to_string(i + 1) + " 行缺少 ci/k");
    }
    const int ci = j["ci"].get<int>();
    const int k = j["k"].get<int>();
    if (ci < 0 || k < 0) return std::unexpected("state 第 " + std::to_string(i + 1) + " 行 ci/k 为负");
    if (!j.contains("outcome")) return std::unexpected("state 第 " + std::to_string(i + 1) + " 行缺少 outcome");
    auto o = outcome_from_json(j["outcome"]);
    if (!o) return std::unexpected("state 第 " + std::to_string(i + 1) + " 行: " + o.error());
    std::string fp;
    if (j.contains("fp") && j["fp"].is_string()) fp = j["fp"].get<std::string>();
    out[{ci, k}] = CachedOutcome{*o, fp};
  }
  return out;
}

std::expected<json, std::string> read_state_header(const std::string& path) {
  std::ifstream in(path);
  if (!in) return std::unexpected("无法打开 state: " + path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
    auto j = json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return std::unexpected("state header 不是合法 JSON");
    if (j.contains("t") && j["t"].is_string() && j["t"].get<std::string>() == "run") return j;
  }
  return std::unexpected("state 里没有 run header");
}

}  // namespace cue::eval
