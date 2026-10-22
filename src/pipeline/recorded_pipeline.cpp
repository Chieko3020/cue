#include "cue/common/recorded_pipeline.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <utility>

namespace cue::pipeline {

using json = nlohmann::json;

namespace {

constexpr int kFormatVersion = 1;

// case id → 文件名片段：'/' 换成 '-'（suite 可能用层级 id），其它非
// [A-Za-z0-9._-] 的字符也一并换掉；".." 这类不能让录制文件跑出目录。
std::string sanitize_component(const std::string& id) {
  std::string out;
  out.reserve(id.size());
  for (const char ch : id) {
    const unsigned char u = static_cast<unsigned char>(ch);
    if (std::isalnum(u) || ch == '-' || ch == '_' || ch == '.') {
      out.push_back(ch);
    } else {
      out.push_back('-');
    }
  }
  if (out.empty() || out.find_first_not_of('.') == std::string::npos) return "case";
  return out;
}

// NaN（没采到样本）必须落成 null，回读再还原成 NaN；绝不能写 0 冒充实测。
json pct_json(double v) { return has_sample(v) ? json(v) : json(nullptr); }

json metrics_to_json(const PipelineMetrics& m) {
  return json{{"fps", m.fps},
              {"frames_in", m.frames_in},
              {"frames_out", m.frames_out},
              {"dropped", m.dropped},
              {"injected", m.injected},
              {"decoded", m.decoded},
              {"decode_errors", m.decode_errors},
              {"rejected", m.rejected},
              {"send_errors", m.send_errors},
              {"encode_errors", m.encode_errors},
              {"decode_drops", m.decode_drops},
              {"budget_drops", m.budget_drops},
              {"recv_p50_ms", pct_json(m.recv_p50)},
              {"buffer_p50_ms", pct_json(m.buffer_p50)},
              {"decode_p50_ms", pct_json(m.decode_p50)},
              {"op_p50_ms", pct_json(m.op_p50)},
              {"encode_p50_ms", pct_json(m.encode_p50)},
              {"send_p50_ms", pct_json(m.send_p50)},
              {"decode_p99_ms", pct_json(m.decode_p99)},
              {"op_p99_ms", pct_json(m.op_p99)},
              {"encode_p99_ms", pct_json(m.encode_p99)},
              {"updated_us", m.updated_us},
              {"samples", m.samples},
              {"alive", m.alive},
              {"pid", m.pid == 0 ? json(nullptr) : json(static_cast<std::int64_t>(m.pid))}};
}

json events_to_json(const std::vector<PipelineEvent>& evs) {
  json arr = json::array();
  for (const auto& e : evs) {
    arr.push_back(json{{"type", e.type}, {"data", e.data}, {"t_us", e.t_us}, {"seen_us", e.seen_us}});
  }
  return arr;
}

// ---- 严格回读：坏文件 / 字段类型不对 → 加载失败，不「猜一个默认值」继续跑 ----

bool need_object(const json& j, const char* what, std::string& err) {
  if (j.is_object()) return true;
  err = std::string(what) + " 不是 JSON 对象";
  return false;
}

bool get_u64(const json& j, const char* key, uint64_t& out, std::string& err) {
  if (!j.contains(key) || !j[key].is_number_integer()) {
    err = std::string("metrics 字段 ") + key + " 缺失或不是整数";
    return false;
  }
  const int64_t v = j[key].get<int64_t>();
  if (v < 0) {
    err = std::string("metrics 字段 ") + key + " 为负数";
    return false;
  }
  out = static_cast<uint64_t>(v);
  return true;
}

bool get_double(const json& j, const char* key, double& out, std::string& err) {
  if (!j.contains(key) || !j[key].is_number()) {
    err = std::string("metrics 字段 ") + key + " 缺失或不是数字";
    return false;
  }
  out = j[key].get<double>();
  return true;
}

// 分位：null → NaN（没采到）；缺字段也按「没采到」处理（fail-closed，绝不补 0）
bool get_pct(const json& j, const char* key, double& out, std::string& err) {
  if (!j.contains(key) || j[key].is_null()) {
    out = kNoSample;
    return true;
  }
  if (!j[key].is_number()) {
    err = std::string("metrics 字段 ") + key + " 既不是 null 也不是数字";
    return false;
  }
  out = j[key].get<double>();
  return true;
}

bool metrics_from_json(const json& j, PipelineMetrics& m, std::string& err) {
  if (!need_object(j, "latest 的 ret", err)) return false;
  if (!get_double(j, "fps", m.fps, err)) return false;
  if (!get_u64(j, "frames_in", m.frames_in, err)) return false;
  if (!get_u64(j, "frames_out", m.frames_out, err)) return false;
  if (!get_u64(j, "dropped", m.dropped, err)) return false;
  if (!get_u64(j, "injected", m.injected, err)) return false;
  if (!get_u64(j, "decoded", m.decoded, err)) return false;
  if (!get_u64(j, "decode_errors", m.decode_errors, err)) return false;
  if (!get_u64(j, "rejected", m.rejected, err)) return false;
  if (!get_u64(j, "send_errors", m.send_errors, err)) return false;
  if (!get_u64(j, "encode_errors", m.encode_errors, err)) return false;
  if (!get_u64(j, "decode_drops", m.decode_drops, err)) return false;
  if (!get_u64(j, "budget_drops", m.budget_drops, err)) return false;
  if (!get_pct(j, "recv_p50_ms", m.recv_p50, err)) return false;
  if (!get_pct(j, "buffer_p50_ms", m.buffer_p50, err)) return false;
  if (!get_pct(j, "decode_p50_ms", m.decode_p50, err)) return false;
  if (!get_pct(j, "op_p50_ms", m.op_p50, err)) return false;
  if (!get_pct(j, "encode_p50_ms", m.encode_p50, err)) return false;
  if (!get_pct(j, "send_p50_ms", m.send_p50, err)) return false;
  if (!get_pct(j, "decode_p99_ms", m.decode_p99, err)) return false;
  if (!get_pct(j, "op_p99_ms", m.op_p99, err)) return false;
  if (!get_pct(j, "encode_p99_ms", m.encode_p99, err)) return false;
  if (!j.contains("updated_us") || !j["updated_us"].is_number_integer()) {
    err = "metrics 字段 updated_us 缺失或不是整数";
    return false;
  }
  m.updated_us = j["updated_us"].get<int64_t>();
  if (!get_u64(j, "samples", m.samples, err)) return false;
  if (!j.contains("alive") || !j["alive"].is_boolean()) {
    err = "metrics 字段 alive 缺失或不是布尔";
    return false;
  }
  m.alive = j["alive"].get<bool>();
  if (!j.contains("pid") || j["pid"].is_null()) {
    m.pid = 0;
  } else if (j["pid"].is_number_integer()) {
    m.pid = static_cast<pid_t>(j["pid"].get<int64_t>());
  } else {
    err = "metrics 字段 pid 既不是 null 也不是整数";
    return false;
  }
  return true;
}

bool events_from_json(const json& j, std::vector<PipelineEvent>& out, std::string& err) {
  if (!need_object(j, "recent_events 的 ret", err)) return false;
  if (!j.contains("events") || !j["events"].is_array()) {
    err = "recent_events 的 ret 缺少 events 数组";
    return false;
  }
  for (const auto& e : j["events"]) {
    if (!e.is_object() || !e.contains("type") || !e["type"].is_string()) {
      err = "recent_events 里有一条事件没有 type";
      return false;
    }
    PipelineEvent ev;
    ev.type = e["type"].get<std::string>();
    if (e.contains("data")) {
      if (!e["data"].is_object()) {
        err = "recent_events 事件的 data 不是对象";
        return false;
      }
      ev.data = e["data"];
    }
    if (e.contains("t_us") && e["t_us"].is_number_integer()) ev.t_us = e["t_us"].get<int64_t>();
    if (e.contains("seen_us") && e["seen_us"].is_number_integer()) {
      ev.seen_us = e["seen_us"].get<int64_t>();
    }
    out.push_back(std::move(ev));
  }
  return true;
}

bool ok_flag(const json& ret, const char* op, bool& ok, std::string& err_text, std::string& err) {
  if (!ret.contains("ok") || !ret["ok"].is_boolean()) {
    err = std::string(op) + " 的 ret 缺少 ok 布尔字段";
    return false;
  }
  ok = ret["ok"].get<bool>();
  err_text.clear();
  if (!ok && ret.contains("err") && ret["err"].is_string()) err_text = ret["err"].get<std::string>();
  return true;
}

std::string op_label(const char* op) {
  const std::string s(op);
  if (s == "start") return "start()";
  if (s == "set_param") return "set_param()";
  if (s == "inject_fault") return "inject_fault()";
  if (s == "observation") return "observation()";
  if (s == "recent_events") return "recent_events()";
  if (s == "running") return "running()";
  if (s == "latest") return "latest()";
  return s;
}

}  // namespace

std::string tools_record_path(const std::string& dir, const std::string& case_id) {
  std::string base = dir;
  while (base.size() > 1 && base.back() == '/') base.pop_back();
  return base + "/" + sanitize_component(case_id) + ".tools.jsonl";
}

// ---------------------------------------------------------------- RecordingPipeline

RecordingPipeline::RecordingPipeline(std::unique_ptr<IServicePipeline> inner, std::string dir,
                                     std::string case_id)
    : inner_(std::move(inner)), path_(tools_record_path(dir, case_id)) {
  std::error_code ec;
  if (!dir.empty()) std::filesystem::create_directories(dir, ec);
  if (ec) {
    io_error_ = "无法创建工具录制目录 " + dir + ": " + ec.message();
  }
}

RecordingPipeline::~RecordingPipeline() = default;

void RecordingPipeline::write_line(const json& line) const {
  if (!io_error_.empty()) return;
  if (!opened_) {
    out_.open(path_, std::ios::out | std::ios::trunc);
    if (!out_) {
      io_error_ = "无法写入工具录制文件 " + path_;
      return;
    }
    opened_ = true;
  }
  out_ << line.dump() << '\n';
  out_.flush();  // 逐行落盘：进程被杀时已发生的观测仍可回放
  if (!out_) io_error_ = "写入工具录制文件失败 " + path_;
}

bool RecordingPipeline::start(std::string& err) {
  const bool ok = inner_->start(err);
  std::lock_guard<std::mutex> lk(write_mu_);
  write_line(json{{"v", kFormatVersion},
                  {"op", "start"},
                  {"ret", ok ? json{{"ok", true}} : json{{"ok", false}, {"err", err}}}});
  return ok;
}

void RecordingPipeline::stop() {
  inner_->stop();
  std::lock_guard<std::mutex> lk(write_mu_);
  write_line(json{{"v", kFormatVersion}, {"op", "stop"}, {"ret", json::object()}});
}

bool RecordingPipeline::running() const {
  const bool v = inner_->running();
  std::lock_guard<std::mutex> lk(write_mu_);
  write_line(json{{"v", kFormatVersion}, {"op", "running"}, {"ret", {{"value", v}}}});
  return v;
}

PipelineMetrics RecordingPipeline::latest() const {
  const PipelineMetrics m = inner_->latest();
  std::lock_guard<std::mutex> lk(write_mu_);
  write_line(json{{"v", kFormatVersion}, {"op", "latest"}, {"ret", metrics_to_json(m)}});
  return m;
}

ProbeMetrics RecordingPipeline::probe_latest() const {
  const ProbeMetrics p = inner_->probe_latest();
  std::lock_guard<std::mutex> lk(write_mu_);
  write_line(
      json{{"v", kFormatVersion}, {"op", "probe_latest"}, {"ret", probe_metrics_to_json(p)}});
  return p;
}

void RecordingPipeline::set_event_callback(std::function<void(const PipelineEvent&)> cb) {
  inner_->set_event_callback(std::move(cb));
}

std::vector<PipelineEvent> RecordingPipeline::recent_events(std::size_t n) const {
  const auto evs = inner_->recent_events(n);
  std::lock_guard<std::mutex> lk(write_mu_);
  write_line(json{{"v", kFormatVersion},
                  {"op", "recent_events"},
                  {"args", {{"n", n}}},
                  {"ret", {{"events", events_to_json(evs)}}}});
  return evs;
}

bool RecordingPipeline::set_param(const std::string& key, const std::string& value,
                                  std::string& err) {
  const bool ok = inner_->set_param(key, value, err);
  std::lock_guard<std::mutex> lk(write_mu_);
  write_line(json{{"v", kFormatVersion},
                  {"op", "set_param"},
                  {"args", {{"key", key}, {"value", value}}},
                  {"ret", ok ? json{{"ok", true}} : json{{"ok", false}, {"err", err}}}});
  return ok;
}

bool RecordingPipeline::inject_fault(const std::string& kind, double value, std::string& err) {
  const bool ok = inner_->inject_fault(kind, value, err);
  std::lock_guard<std::mutex> lk(write_mu_);
  write_line(json{{"v", kFormatVersion},
                  {"op", "inject_fault"},
                  {"args", {{"kind", kind}, {"value", value}}},
                  {"ret", ok ? json{{"ok", true}} : json{{"ok", false}, {"err", err}}}});
  return ok;
}

std::string RecordingPipeline::observation(const std::string& source, int tail,
                                           std::string& err) const {
  std::string text = inner_->observation(source, tail, err);
  std::lock_guard<std::mutex> lk(write_mu_);
  json ret = json::object();
  if (err.empty()) {
    ret = json{{"ok", true}, {"text", text}};
  } else {
    ret = json{{"ok", false}, {"err", err}};
    text.clear();
  }
  write_line(json{{"v", kFormatVersion},
                  {"op", "observation"},
                  {"args", {{"source", source}, {"tail", tail}}},
                  {"ret", ret}});
  return text;
}

// ---------------------------------------------------------------- ReplayPipeline

ReplayPipeline::ReplayPipeline(std::string path) : path_(std::move(path)) {
  std::ifstream in(path_);
  if (!in) {
    load_error_ = "无法打开工具录制文件 " + path_ + "（先跑一次 --record-tools）";
    return;
  }
  std::string line;
  std::size_t no = 0;
  while (std::getline(in, line)) {
    ++no;
    if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
    json j;
    try {
      j = json::parse(line);
    } catch (const std::exception& e) {
      load_error_ = path_ + " 第 " + std::to_string(no) + " 行不是合法 JSON：" + e.what();
      return;
    }
    if (!j.is_object() || !j.contains("v") || !j["v"].is_number_integer() ||
        j["v"].get<int>() != kFormatVersion) {
      load_error_ = path_ + " 第 " + std::to_string(no) + " 行版本不符（需要 v=" +
                    std::to_string(kFormatVersion) + "）";
      return;
    }
    if (!j.contains("op") || !j["op"].is_string()) {
      load_error_ = path_ + " 第 " + std::to_string(no) + " 行缺少 op";
      return;
    }
    const std::string op = j["op"].get<std::string>();
    if (!j.contains("ret") || !j["ret"].is_object()) {
      load_error_ = path_ + " 第 " + std::to_string(no) + " 行（" + op + "）缺少 ret 对象";
      return;
    }
    const json& ret = j["ret"];
    const json args = j.contains("args") && j["args"].is_object() ? j["args"] : json::object();
    std::string err;
    if (op == "latest") {
      PipelineMetrics m;
      if (!metrics_from_json(ret, m, err)) {
        load_error_ = path_ + " 第 " + std::to_string(no) + " 行： " + err;
        return;
      }
      latest_.push_back(m);
    } else if (op == "probe_latest") {
      ProbeMetrics p;
      if (!probe_metrics_from_json(ret, p, err)) {
        load_error_ = path_ + " 第 " + std::to_string(no) + " 行： " + err;
        return;
      }
      probe_latest_.push_back(std::move(p));
    } else if (op == "start") {
      bool ok = false;
      std::string e;
      if (!ok_flag(ret, "start", ok, e, err)) {
        load_error_ = path_ + " 第 " + std::to_string(no) + " 行： " + err;
        return;
      }
      start_.push_back(Record{args, ret});
    } else if (op == "stop") {
      // stop 在回放里是幂等空操作：不消费、不校验
    } else if (op == "running") {
      if (!ret.contains("value") || !ret["value"].is_boolean()) {
        load_error_ = path_ + " 第 " + std::to_string(no) + " 行： running 的 ret 缺少 value";
        return;
      }
      running_.push_back(Record{args, ret});
    } else if (op == "set_param") {
      if (!args.contains("key") || !args["key"].is_string() || !args.contains("value") ||
          !args["value"].is_string()) {
        load_error_ = path_ + " 第 " + std::to_string(no) + " 行： set_param 的 args 不完整";
        return;
      }
      bool ok = false;
      std::string e;
      if (!ok_flag(ret, "set_param", ok, e, err)) {
        load_error_ = path_ + " 第 " + std::to_string(no) + " 行： " + err;
        return;
      }
      set_param_.push_back(Record{args, ret});
    } else if (op == "inject_fault") {
      if (!args.contains("kind") || !args["kind"].is_string() || !args.contains("value") ||
          !args["value"].is_number()) {
        load_error_ = path_ + " 第 " + std::to_string(no) + " 行： inject_fault 的 args 不完整";
        return;
      }
      bool ok = false;
      std::string e;
      if (!ok_flag(ret, "inject_fault", ok, e, err)) {
        load_error_ = path_ + " 第 " + std::to_string(no) + " 行： " + err;
        return;
      }
      inject_fault_.push_back(Record{args, ret});
    } else if (op == "observation") {
      if (!args.contains("source") || !args["source"].is_string() || !args.contains("tail") ||
          !args["tail"].is_number_integer()) {
        load_error_ = path_ + " 第 " + std::to_string(no) + " 行： observation 的 args 不完整";
        return;
      }
      bool ok = false;
      std::string e;
      if (!ok_flag(ret, "observation", ok, e, err)) {
        load_error_ = path_ + " 第 " + std::to_string(no) + " 行： " + err;
        return;
      }
      if (ok && (!ret.contains("text") || !ret["text"].is_string())) {
        load_error_ = path_ + " 第 " + std::to_string(no) + " 行： observation 成功但没有 text";
        return;
      }
      observation_.push_back(Record{args, ret});
    } else if (op == "recent_events") {
      if (!args.contains("n") || !args["n"].is_number_integer()) {
        load_error_ = path_ + " 第 " + std::to_string(no) + " 行： recent_events 的 args 缺少 n";
        return;
      }
      EventsRecord er;
      er.n = args["n"].get<int>();
      if (!events_from_json(ret, er.events, err)) {
        load_error_ = path_ + " 第 " + std::to_string(no) + " 行： " + err;
        return;
      }
      recent_events_.push_back(std::move(er));
    } else {
      load_error_ = path_ + " 第 " + std::to_string(no) + " 行： 未知 op '" + op + "'";
      return;
    }
  }
  if (latest_.empty() && start_.empty() && observation_.empty() && set_param_.empty() &&
      inject_fault_.empty()) {
    load_error_ = "工具录制文件为空或没有可回放的调用：" + path_;
    return;
  }
  stats_.records = latest_.size() + probe_latest_.size() + start_.size() + running_.size() +
                   set_param_.size() + inject_fault_.size() + observation_.size() +
                   recent_events_.size();

  // 事件回调：把录制里各次 recent_events 看到的事件合并（去重）后按 t_us 排序，
  // 让上层在回放时看到与录制时同一批事件、同一顺序。
  std::vector<PipelineEvent> all;
  for (const auto& er : recent_events_) {
    for (const auto& e : er.events) all.push_back(e);
  }
  std::stable_sort(all.begin(), all.end(), [](const PipelineEvent& a, const PipelineEvent& b) {
    if (a.t_us != b.t_us) return a.t_us < b.t_us;
    return a.seen_us < b.seen_us;
  });
  for (const auto& e : all) {
    const bool dup = std::any_of(events_for_callback_.begin(), events_for_callback_.end(),
                                 [&e](const PipelineEvent& x) {
                                   return x.type == e.type && x.t_us == e.t_us &&
                                          x.seen_us == e.seen_us && x.data == e.data;
                                 });
    if (!dup) events_for_callback_.push_back(e);
  }
}

ReplayStats ReplayPipeline::stats() const {
  std::lock_guard<std::mutex> lk(mu_);
  return stats_;
}

std::string ReplayPipeline::last_error() const {
  std::lock_guard<std::mutex> lk(mu_);
  return error_;
}

void ReplayPipeline::fail(const std::string& why) const {
  stats_.misses++;
  error_ = why;
}

// 取某 op 的下一条录制。args 参与比对：两次 run 走了不同分支（例如录制时 set 的是 gop，
// 回放时 set 的是 bitrate_kbps）必须明确失败，不能「拿另一条顶上」。
bool ReplayPipeline::take(const std::string& op, const json& args, bool check_args, Record& out,
                          std::string& err) const {
  std::lock_guard<std::mutex> lk(mu_);
  const std::vector<Record>* list = nullptr;
  std::size_t* index = nullptr;
  if (op == "start") {
    list = &start_;
    index = &i_start_;
  } else if (op == "set_param") {
    list = &set_param_;
    index = &i_set_param_;
  } else if (op == "inject_fault") {
    list = &inject_fault_;
    index = &i_inject_;
  } else if (op == "observation") {
    list = &observation_;
    index = &i_obs_;
  } else if (op == "running") {
    list = &running_;
    index = &i_running_;
  } else {
    err = "内部错误：未知回放 op " + op;
    return false;
  }
  if (list->empty() || *index >= list->size()) {
    const std::string why = "工具回放 miss：" + op_label(op.c_str()) + " 第 " +
                            std::to_string(*index + 1) + " 次调用在录制中没有对应条目（共 " +
                            std::to_string(list->size()) + " 条）：两次 run 的调用序列不一致";
    fail(why);
    err = why;
    return false;
  }
  const Record& r = (*list)[*index];
  if (check_args && r.args != args) {
    const std::string why = "工具回放参数不符：" + op_label(op.c_str()) + " 第 " +
                            std::to_string(*index + 1) + " 次调用为 " + args.dump() +
                            "，录制里是 " + r.args.dump() +
                            "（两次 run 走了不同分支，不是可复现）";
    fail(why);
    err = why;
    return false;
  }
  out = r;
  ++(*index);
  stats_.hits++;
  return true;
}

bool ReplayPipeline::start(std::string& err) {
  if (!load_error_.empty()) {
    err = load_error_;
    return false;
  }
  Record r;
  if (!take("start", json::object(), false, r, err)) return false;
  bool ok = false;
  std::string recorded_err;
  if (!ok_flag(r.ret, "start", ok, recorded_err, err)) return false;
  if (!ok) err = recorded_err.empty() ? "录制中 start 失败（无错误文案）" : recorded_err;
  return ok;
}

void ReplayPipeline::stop() {
  // 幂等空操作：录制里 stop 没有可回放的效果（也没有需要校验的参数）
}

bool ReplayPipeline::running() const {
  if (!load_error_.empty()) return false;
  std::lock_guard<std::mutex> lk(mu_);
  std::string err;
  if (i_running_ >= running_.size()) {
    fail("工具回放 miss：running() 第 " + std::to_string(i_running_ + 1) +
         " 次调用在录制中没有对应条目（共 " + std::to_string(running_.size()) + " 条）");
    return false;
  }
  const bool v = running_[i_running_].ret["value"].get<bool>();
  ++i_running_;
  ++stats_.hits;
  return v;
}

PipelineMetrics ReplayPipeline::latest() const {
  if (!load_error_.empty()) return PipelineMetrics{};
  std::lock_guard<std::mutex> lk(mu_);
  if (latest_.empty() || i_latest_ >= latest_.size()) {
    // 绝不能伪造一份数据：samples=0 让上层看到「没有样本」，miss 进 stats 与报告
    fail("工具回放 miss：latest() 第 " + std::to_string(i_latest_ + 1) +
         " 次调用在录制中没有对应条目（共 " + std::to_string(latest_.size()) + " 条）");
    return PipelineMetrics{};
  }
  const PipelineMetrics m = latest_[i_latest_];
  ++i_latest_;
  ++stats_.hits;
  return m;
}

ProbeMetrics ReplayPipeline::probe_latest() const {
  std::lock_guard<std::mutex> lk(mu_);
  if (i_probe_ < probe_latest_.size()) return probe_latest_[i_probe_++];
  ProbeMetrics p;
  if (probe_latest_.empty()) {
    // 旧录制里本来就没有 probe 指标：这不是"回放 miss"（不是参数不符/录制用尽），
    // 而是这份录制没录端到端。如实返回不可用，让评测侧报 probe_unavailable，
    // 而不是伪造一份 0 让端到端 SLA 看起来"达标"。
    p.unavailable_reason = "录制不含 probe 指标（录制时未托管探针的旧格式）";
    return p;
  }
  // 有录制但消费超量 = 调用序列与录制不一致：必须报错，不能返回默认值
  fail("工具回放 miss：probe_latest() 第 " + std::to_string(i_probe_ + 1) +
       " 次调用在录制中没有对应条目（共 " + std::to_string(probe_latest_.size()) + " 条）");
  p.unavailable_reason = "录制里的 probe 指标已用尽";
  return p;
}

void ReplayPipeline::set_event_callback(std::function<void(const PipelineEvent&)> cb) {
  cb_ = std::move(cb);
  if (!cb_) return;
  // 按录制里 recent_events 的序列重放（已按 t_us 去重排序）：上层看到的事件批次与录制时一致
  for (const auto& e : events_for_callback_) cb_(e);
}

std::vector<PipelineEvent> ReplayPipeline::recent_events(std::size_t n) const {
  if (!load_error_.empty()) return {};
  std::lock_guard<std::mutex> lk(mu_);
  if (i_events_ >= recent_events_.size()) {
    fail("工具回放 miss：recent_events(" + std::to_string(n) + ") 第 " +
         std::to_string(i_events_ + 1) + " 次调用在录制中没有对应条目（共 " +
         std::to_string(recent_events_.size()) + " 条）");
    return {};
  }
  const EventsRecord& r = recent_events_[i_events_];
  if (r.n != static_cast<int>(n)) {
    fail("工具回放参数不符：recent_events 第 " + std::to_string(i_events_ + 1) +
         " 次调用 n=" + std::to_string(n) + "，录制里是 n=" + std::to_string(r.n));
    return {};
  }
  ++i_events_;
  ++stats_.hits;
  return r.events;
}

bool ReplayPipeline::set_param(const std::string& key, const std::string& value,
                               std::string& err) {
  if (!load_error_.empty()) {
    err = load_error_;
    return false;
  }
  Record r;
  std::string why;
  if (!take("set_param", json{{"key", key}, {"value", value}}, true, r, why)) {
    err = why;
    return false;
  }
  bool ok = false;
  std::string recorded_err;
  if (!ok_flag(r.ret, "set_param", ok, recorded_err, why)) {
    err = why;
    return false;
  }
  if (!ok) err = recorded_err.empty() ? "录制中 set_param 失败（无错误文案）" : recorded_err;
  return ok;
}

bool ReplayPipeline::inject_fault(const std::string& kind, double value, std::string& err) {
  if (!load_error_.empty()) {
    err = load_error_;
    return false;
  }
  Record r;
  std::string why;
  if (!take("inject_fault", json{{"kind", kind}, {"value", value}}, true, r, why)) {
    err = why;
    return false;
  }
  bool ok = false;
  std::string recorded_err;
  if (!ok_flag(r.ret, "inject_fault", ok, recorded_err, why)) {
    err = why;
    return false;
  }
  if (!ok) err = recorded_err.empty() ? "录制中 inject_fault 失败（无错误文案）" : recorded_err;
  return ok;
}

std::string ReplayPipeline::observation(const std::string& source, int tail,
                                        std::string& err) const {
  if (!load_error_.empty()) {
    err = load_error_;
    return {};
  }
  Record r;
  std::string why;
  if (!take("observation", json{{"source", source}, {"tail", tail}}, true, r, why)) {
    err = why;
    return {};
  }
  bool ok = false;
  std::string recorded_err;
  if (!ok_flag(r.ret, "observation", ok, recorded_err, why)) {
    err = why;
    return {};
  }
  if (!ok) {
    err = recorded_err.empty() ? "录制中 observation 失败（无错误文案）" : recorded_err;
    return {};
  }
  return r.ret["text"].get<std::string>();
}

}  // namespace cue::pipeline
