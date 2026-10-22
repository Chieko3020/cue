// HttpServicePipeline 实现：REST 控制协议 ↔ IServicePipeline。
// 协议字段与 apps/cue-serve.cpp 一致（/api/health、/api/state、/api/action），
// 因此同一个后端既能驱动真控制台，也能驱动只实现这套 REST 的假域服务（apps/cue-mock-svc.cpp）。
#include "cue/pipeline/http_pipeline.h"

#include <curl/curl.h>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

#include "cue/common/clock.h"

namespace cue::pipeline {

namespace {

using json = nlohmann::json;

constexpr std::size_t kMaxEvents = 5000;  // 与 ProcessPipelineControl 的事件缓存口径一致

struct HttpResponse {
  CURLcode code = CURLE_OK;
  long status = 0;
  std::string body;
};

std::size_t write_body(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
  auto* s = static_cast<std::string*>(userdata);
  s->append(ptr, size * nmemb);
  return size * nmemb;
}

HttpResponse http_do(const std::string& url, const char* method, const std::string& body,
                     int timeout_ms) {
  // curl_global_init 不是线程安全的，但只做一次；initialize() 本身可重入。
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

  HttpResponse resp;
  CURL* h = curl_easy_init();
  if (h == nullptr) {
    resp.code = CURLE_FAILED_INIT;
    return resp;
  }
  curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Accept: application/json");
  curl_easy_setopt(h, CURLOPT_URL, url.c_str());
  // NOSIGNAL：本进程在多线程里用 curl，不能用信号超时（会打到别的线程）
  curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, static_cast<long>(std::max(1, timeout_ms)));
  curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(std::min(1000, std::max(1, timeout_ms))));
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_body);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &resp.body);
  curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
  if (method != nullptr && std::string(method) == "POST") {
    curl_easy_setopt(h, CURLOPT_POST, 1L);
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  }
  resp.code = curl_easy_perform(h);
  if (resp.code == CURLE_OK) {
    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    resp.status = status;
  }
  curl_easy_cleanup(h);
  curl_slist_free_all(headers);
  return resp;
}

// 事件去重键：优先时间戳（同一服务反复返回最近 N 条时不重复入缓存），
// 没有时间戳的旧后端退回完整 data。
std::string key_of(const PipelineEvent& ev) {
  if (ev.t_us != 0) return ev.type + "|" + std::to_string(ev.t_us);
  return ev.type + "|" + ev.data.dump();
}

void parse_metrics(const json& j, PipelineMetrics& m, int64_t now) {
  auto num = [&j](const char* key, double& out) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return;
    out = it->get<double>();
  };
  auto cnt = [&j](const char* key, uint64_t& out) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return;
    const int64_t v = it->get<int64_t>();
    out = v > 0 ? static_cast<uint64_t>(v) : 0;
  };
  // 分位：字段缺失/null 保持 NaN（"没采到" ≠ 0 ms）
  auto pct = [&j](const char* key, double& out) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return;
    out = it->get<double>();
  };
  num("fps", m.fps);
  cnt("frames_in", m.frames_in);
  cnt("frames_out", m.frames_out);
  cnt("dropped", m.dropped);
  cnt("injected", m.injected);
  cnt("decoded", m.decoded);
  cnt("decode_errors", m.decode_errors);
  cnt("rejected", m.rejected);
  cnt("send_errors", m.send_errors);
  cnt("encode_errors", m.encode_errors);
  cnt("decode_drops", m.decode_drops);
  cnt("budget_drops", m.budget_drops);
  pct("recv_p50_ms", m.recv_p50);
  pct("buffer_p50_ms", m.buffer_p50);
  pct("decode_p50_ms", m.decode_p50);
  pct("decode_p99_ms", m.decode_p99);
  pct("op_p50_ms", m.op_p50);
  pct("op_p99_ms", m.op_p99);
  pct("encode_p50_ms", m.encode_p50);
  pct("encode_p99_ms", m.encode_p99);
  pct("send_p50_ms", m.send_p50);
  cnt("samples", m.samples);
  const auto pid_it = j.find("pid");
  if (pid_it != j.end() && pid_it->is_number_integer()) {
    m.pid = static_cast<pid_t>(pid_it->get<int64_t>());
  }
  const auto alive_it = j.find("alive");
  if (alive_it != j.end() && alive_it->is_boolean()) m.alive = alive_it->get<bool>();
  m.updated_us = now;
}

}  // namespace

HttpServicePipeline::HttpServicePipeline(const PipelineEndpoint& ep)
    : base_(ep.http_base), timeout_ms_(ep.http_timeout_ms > 0 ? ep.http_timeout_ms : 2000) {
  while (!base_.empty() && base_.back() == '/') base_.pop_back();
}

HttpServicePipeline::Reply HttpServicePipeline::request(const char* method, const std::string& path,
                                                       const json* body) const {
  Reply r;
  if (base_.empty()) {
    r.err = "Http 端点为空（PipelineEndpoint::http_base）";
    return r;
  }
  const std::string url = base_ + path;
  const HttpResponse hr = http_do(url, method, body == nullptr ? std::string() : body->dump(), timeout_ms_);
  if (hr.code != CURLE_OK) {
    r.err = std::string("HTTP 请求失败 ") + method + " " + url + "：" + curl_easy_strerror(hr.code);
    return r;
  }
  r.http_status = hr.status;
  if (hr.status != 200) {
    r.err = "HTTP " + std::to_string(hr.status) + " " + method + " " + url;
    return r;
  }
  r.body = json::parse(hr.body, nullptr, false);
  if (r.body.is_discarded() || !r.body.is_object()) {
    r.err = "响应不是合法 JSON 对象：" + url;
    r.body = json::object();
    return r;
  }
  r.ok = r.body.value("ok", false);
  if (!r.ok) {
    r.err = r.body.value("error", std::string("服务端返回 ok=false（未给 error）"));
  }
  return r;
}

HttpServicePipeline::Reply HttpServicePipeline::action(const json& body) const {
  return request("POST", "/api/action", &body);
}

bool HttpServicePipeline::refresh() const {
  const Reply r = request("GET", "/api/state", nullptr);
  if (!r.ok) {
    // 连不上/超时：把缓存标成"没有存活管线"，而不是把上一次的成功读数继续当现状
    std::lock_guard<std::mutex> lk(mu_);
    metrics_ = PipelineMetrics{};
    metrics_.alive = false;
    running_ = false;
    return false;
  }
  PipelineMetrics m;
  m.alive = r.body.value("alive", false);
  bool running = m.alive;
  const auto pipe_it = r.body.find("pipeline");
  if (pipe_it != r.body.end() && pipe_it->is_object()) {
    const auto run_it = pipe_it->find("running");
    if (run_it != pipe_it->end() && run_it->is_boolean()) running = run_it->get<bool>();
  }
  const auto met_it = r.body.find("metrics");
  if (met_it != r.body.end() && met_it->is_object()) {
    parse_metrics(*met_it, m, now_us());
    if (met_it->find("alive") == met_it->end()) m.alive = running;
  }

  std::vector<PipelineEvent> fresh;
  std::function<void(const PipelineEvent&)> cb;
  {
    std::lock_guard<std::mutex> lk(mu_);
    metrics_ = m;
    running_ = running;
    const auto ev_it = r.body.find("events");
    if (ev_it != r.body.end() && ev_it->is_array()) {
      for (const auto& e : *ev_it) {
        if (!e.is_object()) continue;
        PipelineEvent ev;
        ev.type = e.value("type", std::string());
        if (ev.type.empty()) ev.type = e.value("ev", std::string());
        if (ev.type.empty()) continue;
        const auto data_it = e.find("data");
        if (data_it != e.end() && data_it->is_object()) ev.data = *data_it;
        if (const auto t = e.find("t_us"); t != e.end() && t->is_number_integer()) {
          ev.t_us = t->get<int64_t>();
        } else if (const auto t2 = e.find("t"); t2 != e.end() && t2->is_number_integer()) {
          ev.t_us = t2->get<int64_t>();
        }
        ev.seen_us = now_us();
        const std::string key = key_of(ev);
        if (!event_keys_.insert(key).second) continue;
        events_.push_back(ev);
        fresh.push_back(ev);
        while (events_.size() > kMaxEvents) {
          event_keys_.erase(key_of(events_.front()));
          events_.pop_front();
        }
      }
    }
    cb = on_event_;
  }
  // 回调在锁外执行：回调里再调 latest()/recent_events() 不会自死锁
  if (cb) {
    for (const auto& ev : fresh) cb(ev);
  }
  return true;
}

bool HttpServicePipeline::start(std::string& err) {
  const Reply r = action(json{{"action", "start"}});
  if (!r.ok) {
    err = "启动失败：" + r.err;
    return false;
  }
  std::lock_guard<std::mutex> lk(mu_);
  running_ = true;
  return true;
}

void HttpServicePipeline::stop() {
  // stop 是析构路径上的尽力而为（接口签名没有 err 出口）：失败时不改缓存，
  // 下一次 running()/latest() 会从 /api/state 读到真实状态。
  const Reply r = action(json{{"action", "stop"}});
  if (!r.ok) return;
  std::lock_guard<std::mutex> lk(mu_);
  running_ = false;
}

bool HttpServicePipeline::running() const {
  const bool ok = refresh();
  std::lock_guard<std::mutex> lk(mu_);
  return ok && running_;
}

PipelineMetrics HttpServicePipeline::latest() const {
  // 拉一次 state：既是"现在的读数"，也顺带把事件推进缓存。
  // 失败时 refresh() 已把缓存标成 alive=false，这里照原样返回（NaN 分位、samples=0）。
  (void)refresh();
  std::lock_guard<std::mutex> lk(mu_);
  return metrics_;
}

ProbeMetrics HttpServicePipeline::probe_latest() const {
  ProbeMetrics p;
  // 这里不是"忘了实现"：探针是一条独立的接收端进程（cue-probe），HTTP 后端背后是
  // 一个远端服务或本机假域，harness 既没有它的探针、也不该替它编一份端到端读数。
  // 评测侧读到这条原因时必须报 probe_unavailable（见 eval.cpp 的 require_probe）。
  p.unavailable_reason =
      "HTTP 后端（假域/远端服务）没有探针：端到端指标不可用，且不拿服务端自报代替";
  return p;
}

void HttpServicePipeline::set_event_callback(std::function<void(const PipelineEvent&)> cb) {
  std::lock_guard<std::mutex> lk(mu_);
  on_event_ = std::move(cb);
}

nlohmann::json HttpServicePipeline::sessions_json(std::string& err) const {
  // 与 probe_latest() 同理：不是"忘了实现"，是这条后端上真的没有这张表。
  return no_session_table("http", err);
}

std::vector<PipelineEvent> HttpServicePipeline::recent_events(std::size_t n) const {
  std::lock_guard<std::mutex> lk(mu_);
  const std::size_t want = (n == 0 || n >= events_.size()) ? events_.size() : n;
  return std::vector<PipelineEvent>(events_.end() - static_cast<std::ptrdiff_t>(want), events_.end());
}

bool HttpServicePipeline::set_param(const std::string& key, const std::string& value,
                                    std::string& err) {
  const Reply r = action(json{{"action", "set_param"}, {"key", key}, {"value", value}});
  if (!r.ok) {
    err = "设置失败：" + r.err;
    return false;
  }
  return true;
}

bool HttpServicePipeline::inject_fault(const std::string& kind, double value, std::string& err) {
  // kind 原样透传（不写死 drop）：服务的注入类型由服务端定义，客户端不替它假设
  const Reply r = action(json{{"action", "inject"}, {"kind", kind}, {"value", value}});
  if (!r.ok) {
    err = "注入失败：" + r.err;
    return false;
  }
  return true;
}

std::string HttpServicePipeline::observation(const std::string& source, int tail,
                                             std::string& err) const {
  const Reply r = action(json{{"action", "observe"}, {"source", source}, {"tail", tail}});
  if (!r.ok) {
    err = "观测失败：" + r.err;
    return {};
  }
  if (r.body.contains("text") && r.body["text"].is_string()) return r.body["text"].get<std::string>();
  // 兼容 cue-serve 的字段名（工具层只认"回灌给模型的原文"这一件事）
  return r.body.value("content", std::string());
}

}  // namespace cue::pipeline
