#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "cue/llm/cassette.h"
#include "cue/llm/client.h"

namespace cue::llm {

// ---------------------------------------------------------------- ScriptedClient

void ScriptedClient::push(ChatResponse r) { script_.push_back(std::move(r)); }

std::expected<ChatResponse, std::string> ScriptedClient::chat(const ChatRequest&) {
  if (idx_ >= script_.size()) {
    return std::unexpected("scripted client exhausted (" + std::to_string(script_.size()) + " responses)");
  }
  return script_[idx_++];
}

// ------------------------------------------------------- OpenAiCompatibleClient

namespace {

// curl_global_init 不是线程安全的，且必须进程级只做一次（SPEC §9：模块边界不泄漏资源）。
void ensure_curl_global() {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

// 响应体上限：超限即中止传输，避免异常端点把内存撑爆
struct WriteSink {
  std::string* out = nullptr;
  std::size_t limit = 8u * 1024u * 1024u;
  bool overflow = false;
};

std::size_t write_cb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
  auto* sink = static_cast<WriteSink*>(userdata);
  const std::size_t n = size * nmemb;
  if (sink->out->size() + n > sink->limit) {
    sink->overflow = true;
    return 0;  // 触发 CURLE_WRITE_ERROR 中止
  }
  sink->out->append(ptr, n);
  return n;
}

struct HttpOutcome {
  CURLcode rc = CURLE_OK;
  long http_code = 0;
  std::string body;
  bool overflow = false;
};

// 取消通道：libcurl 在传输期间至少每秒调一次 progress 回调（对端一个字节都没发时也一样），
// 所以「外部取消标志」只能在这里被看见——否则请求要么等满 timeout_s，要么等重试退避跑完
// （默认 60 s × 3 次）。回调只做一次原子 load，不碰锁/内存分配。
int xferinfo_cb(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  const auto* flag = static_cast<const std::atomic<bool>*>(userdata);
  return (flag != nullptr && flag->load(std::memory_order_relaxed)) ? 1 : 0;
}

HttpOutcome perform(CURL* curl, const curl_slist* headers, const std::string& url,
                    const std::string& payload, long timeout_ms, std::size_t max_bytes,
                    const std::atomic<bool>* cancel) {
  HttpOutcome o;
  // 复用句柄：reset 掉上次的选项，但保留连接缓存（省掉每轮一次 TLS 握手）
  curl_easy_reset(curl);
  WriteSink sink{&o.body, max_bytes};
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");  // 开启 gzip/deflate，curl 自动解压
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  // 取消：NOPROGRESS 必须关掉，否则 XFERINFOFUNCTION 不会被调用
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, const_cast<std::atomic<bool>*>(cancel));
  o.rc = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &o.http_code);
  o.overflow = sink.overflow;
  return o;
}

// SPEC §7.9：只对 429 / 5xx / 网络错误重试；响应体超限、4xx 等重试无意义
bool retryable(CURLcode rc, long http_code, bool overflow) {
  if (overflow) return false;
  if (rc != CURLE_OK) return true;
  return http_code == 429 || (http_code >= 500 && http_code < 600);
}

int as_int(const json& j) {
  if (!j.is_number()) return 0;
  const double d = j.get<double>();
  if (!std::isfinite(d) || d < 0.0 || d > 2.0e9) return 0;
  return static_cast<int>(d);
}

std::string str_field(const json& obj, const char* key) {
  if (!obj.contains(key)) return {};
  const auto& v = obj[key];
  if (!v.is_string()) return {};
  return v.get<std::string>();
}

// 逐层 is_object/is_array 校验：任何结构不符都返回错误，绝不抛异常穿出模块边界（SPEC §9）
std::expected<ChatResponse, std::string> parse_response(const std::string& body) {
  try {
    const json root = json::parse(body, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
      return std::unexpected("invalid response (not a JSON object): " + body.substr(0, 400));
    }
    if (!root.contains("choices") || !root["choices"].is_array() || root["choices"].empty()) {
      return std::unexpected("invalid response (no choices): " + body.substr(0, 400));
    }
    const auto& choice = root["choices"][0];
    if (!choice.is_object()) {
      return std::unexpected("invalid response (choices[0] is not an object)");
    }

    ChatResponse out;
    out.finish_reason = str_field(choice, "finish_reason");
    // length = 输出被 max_tokens 截断：工具参数可能是半截 JSON（SPEC §7.9）
    out.truncated = (out.finish_reason == "length");

    if (!choice.contains("message") || !choice["message"].is_object()) {
      return std::unexpected("invalid response (missing choices[0].message)");
    }
    const auto& msg = choice["message"];
    out.content = str_field(msg, "content");

    if (msg.contains("tool_calls")) {
      const auto& arr = msg["tool_calls"];
      if (!arr.is_array()) return std::unexpected("invalid response (message.tool_calls is not an array)");
      for (const auto& tcv : arr) {
        if (!tcv.is_object()) return std::unexpected("invalid response (tool_calls item is not an object)");
        ToolCall c;
        c.id = str_field(tcv, "id");
        if (!tcv.contains("function") || !tcv["function"].is_object()) {
          return std::unexpected("invalid response (tool_call missing function object)");
        }
        const auto& fn = tcv["function"];
        c.name = str_field(fn, "name");
        if (c.name.empty()) return std::unexpected("invalid response (tool_call missing name)");
        if (fn.contains("arguments")) {
          if (fn["arguments"].is_string()) {
            c.arguments = fn["arguments"].get<std::string>();
          } else if (fn["arguments"].is_object()) {
            c.arguments = fn["arguments"].dump();  // 少数端点直接把参数给成对象
          } else {
            return std::unexpected("invalid response (tool_call arguments type)");
          }
        }
        out.tool_calls.push_back(std::move(c));
      }
    }

    if (root.contains("usage") && root["usage"].is_object()) {
      const auto& u = root["usage"];
      if (u.contains("prompt_tokens")) out.usage.prompt_tokens = as_int(u["prompt_tokens"]);
      if (u.contains("completion_tokens")) out.usage.completion_tokens = as_int(u["completion_tokens"]);
    }
    return out;
  } catch (const json::exception& e) {
    return std::unexpected(std::string("response parse error: ") + e.what());
  }
}

json to_openai_message(const ChatMessage& m) {
  json j{{"role", m.role}};
  if (!m.content.empty() || m.tool_calls.empty()) j["content"] = m.content;
  if (!m.tool_call_id.empty()) j["tool_call_id"] = m.tool_call_id;
  if (!m.name.empty()) j["name"] = m.name;
  if (!m.tool_calls.empty()) {
    json arr = json::array();
    for (const auto& tc : m.tool_calls) {
      arr.push_back({{"id", tc.id},
                     {"type", "function"},
                     {"function", {{"name", tc.name}, {"arguments", tc.arguments}}}});
    }
    j["tool_calls"] = arr;
  }
  return j;
}

}  // namespace

struct OpenAiCompatibleClient::Impl {
  CURL* curl = nullptr;
  curl_slist* headers = nullptr;

  explicit Impl(const Config& cfg) {
    curl = curl_easy_init();
    headers = curl_slist_append(headers, "Content-Type: application/json");
    const std::string auth = "Authorization: Bearer " + cfg.api_key;
    headers = curl_slist_append(headers, auth.c_str());
  }
  ~Impl() {
    if (headers) curl_slist_free_all(headers);
    if (curl) curl_easy_cleanup(curl);
  }
};

OpenAiCompatibleClient::OpenAiCompatibleClient(Config cfg)
    : cfg_(std::move(cfg)), impl_(std::make_unique<Impl>(cfg_)) {
  ensure_curl_global();
}

OpenAiCompatibleClient::~OpenAiCompatibleClient() = default;

std::expected<ChatResponse, std::string> OpenAiCompatibleClient::chat(const ChatRequest& req) {
  using Clock = std::chrono::steady_clock;
  const auto started = Clock::now();
  if (!impl_->curl) return std::unexpected("curl_easy_init failed");
  // 已经取消就别浪费一次（可能是计费的）请求
  if (req.cancel != nullptr && req.cancel->load()) return std::unexpected("cancelled");

  int timeout_s = cfg_.timeout_s;
  if (req.timeout_s > 0) timeout_s = timeout_s > 0 ? std::min(req.timeout_s, timeout_s) : req.timeout_s;
  if (timeout_s <= 0) timeout_s = 60;
  auto deadline = started + std::chrono::seconds(timeout_s);
  if (req.deadline) deadline = std::min(deadline, *req.deadline);
  const auto cancelled = [&] { return req.cancel != nullptr && req.cancel->load(std::memory_order_relaxed); };
  const auto remaining_ms = [&]() -> int64_t {
    const auto now = Clock::now();
    // Compare first: subtracting time_point::min() from now can overflow.
    if (now >= deadline) return 0;
    return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
  };
  // A sub-millisecond remainder cannot safely become CURLOPT_TIMEOUT_MS=0
  // (which means unlimited). Cancellation wins when both conditions apply.
  if (remaining_ms() <= 0) return std::unexpected("request deadline exceeded");

  json body{
      {"model", req.model.empty() ? cfg_.model : req.model},
      {"temperature", req.temperature},
      {"max_tokens", req.max_tokens},
      {"stream", false},
  };
  json msgs = json::array();
  for (const auto& m : req.messages) msgs.push_back(to_openai_message(m));
  body["messages"] = std::move(msgs);
  if (req.tools.is_array() && !req.tools.empty()) body["tools"] = req.tools;

  const std::string payload = body.dump();
  std::string url = cfg_.base_url;
  if (!url.empty() && url.back() == '/') url.pop_back();
  url += "/v1/chat/completions";

  const int max_retries = cfg_.max_retries < 0 ? 0 : cfg_.max_retries;
  HttpOutcome outcome;
  int attempts = 0;
  for (int attempt = 0; attempt <= max_retries; ++attempt) {
    if (cancelled()) return std::unexpected("cancelled");
    if (remaining_ms() <= 0) return std::unexpected("request deadline exceeded");
    if (attempt > 0) {
      const auto backoff_end = std::min(deadline, Clock::now() +
          std::chrono::milliseconds(attempt == 1 ? 200 : 800));
      while (Clock::now() < backoff_end) {
        if (cancelled()) return std::unexpected("cancelled");
        std::this_thread::sleep_until(std::min(backoff_end, Clock::now() + std::chrono::milliseconds(20)));
      }
    }
    if (cancelled()) return std::unexpected("cancelled");
    const auto left = remaining_ms();
    if (left <= 0) return std::unexpected("request deadline exceeded");
    const long timeout_ms = static_cast<long>(std::min<int64_t>(left, std::numeric_limits<long>::max()));
    ++attempts;
    outcome = perform(impl_->curl, impl_->headers, url, payload, timeout_ms, cfg_.max_response_bytes,
                      req.cancel);
    if (cancelled() || outcome.rc == CURLE_ABORTED_BY_CALLBACK) return std::unexpected("cancelled");
    if (outcome.rc == CURLE_OPERATION_TIMEDOUT || remaining_ms() <= 0)
      return std::unexpected("request deadline exceeded");
    if (!retryable(outcome.rc, outcome.http_code, outcome.overflow)) break;
  }

  const std::string suffix = attempts > 1 ? " (after " + std::to_string(attempts) + " attempts)" : "";
  if (outcome.overflow) {
    return std::unexpected("response too large (> " + std::to_string(cfg_.max_response_bytes) + " bytes)");
  }
  if (outcome.rc == CURLE_ABORTED_BY_CALLBACK) {
    // 与「网络超时」区分开：上层据此知道是取消而不是端点故障（也不会被重试）
    return std::unexpected("cancelled");
  }
  if (outcome.rc != CURLE_OK) {
    return std::unexpected(std::string("curl: ") + curl_easy_strerror(outcome.rc) + suffix);
  }
  if (outcome.http_code < 200 || outcome.http_code >= 300) {
    return std::unexpected("http " + std::to_string(outcome.http_code) + suffix + ": " +
                           outcome.body.substr(0, 400));
  }
  auto parsed = parse_response(outcome.body);
  if (!parsed) return parsed;
  if (cancelled()) return std::unexpected("cancelled");
  if (remaining_ms() <= 0) return std::unexpected("request deadline exceeded");

  // 录制发生在「成功返回」之后：失败/重试中的响应不进 cassette，回放只重放确定结论。
  if (!cfg_.record_dir.empty()) {
    const std::string model = req.model.empty() ? cfg_.model : req.model;
    if (auto w = cassette_append(cfg_.record_dir, req, model, *parsed); !w) {
      // 录制是辅助能力：写盘失败不能把一次成功的 API 调用变成失败（成本已发生）
      std::fprintf(stderr, "[llm] cassette 录制失败：%s\n", w.error().c_str());
    }
  }
  // Recording is synchronous auxiliary I/O. It may finish after the deadline;
  // never report a late/cancelled result as successful (no rollback is claimed).
  if (cancelled()) return std::unexpected("cancelled");
  if (remaining_ms() <= 0) return std::unexpected("request deadline exceeded");
  return parsed;
}

}  // namespace cue::llm
