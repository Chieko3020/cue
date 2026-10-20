#include <curl/curl.h>

#include <cstdio>
#include <string>

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

std::size_t write_cb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
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

OpenAiCompatibleClient::OpenAiCompatibleClient(Config cfg) : cfg_(std::move(cfg)) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
}

std::expected<ChatResponse, std::string> OpenAiCompatibleClient::chat(const ChatRequest& req) {
  json body{
      {"model", req.model.empty() ? cfg_.model : req.model},
      {"temperature", req.temperature},
      {"max_tokens", req.max_tokens},
      {"stream", false},
  };
  json msgs = json::array();
  for (const auto& m : req.messages) msgs.push_back(to_openai_message(m));
  body["messages"] = std::move(msgs);
  if (!req.tools.empty() && req.tools.is_array() && !req.tools.empty()) body["tools"] = req.tools;

  const std::string payload = body.dump();
  std::string url = cfg_.base_url;
  if (!url.empty() && url.back() == '/') url.pop_back();
  url += "/v1/chat/completions";

  CURL* curl = curl_easy_init();
  if (!curl) return std::unexpected("curl_easy_init failed");

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  const std::string auth = "Authorization: Bearer " + cfg_.api_key;
  headers = curl_slist_append(headers, auth.c_str());

  std::string resp;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(cfg_.timeout_s));

  const CURLcode rc = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK) return std::unexpected(std::string("curl: ") + curl_easy_strerror(rc));
  if (http_code < 200 || http_code >= 300) {
    return std::unexpected("http " + std::to_string(http_code) + ": " + resp.substr(0, 400));
  }

  auto root = json::parse(resp, nullptr, false);
  if (root.is_discarded() || !root.contains("choices") || !root["choices"].is_array() ||
      root["choices"].empty()) {
    return std::unexpected("invalid response: " + resp.substr(0, 400));
  }
  const auto& msg = root["choices"][0]["message"];
  ChatResponse out;
  if (msg.contains("content") && msg["content"].is_string()) out.content = msg["content"];
  if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
    for (const auto& tc : msg["tool_calls"]) {
      ToolCall c;
      c.id = tc.value("id", "");
      if (tc.contains("function")) {
        c.name = tc["function"].value("name", "");
        c.arguments = tc["function"].value("arguments", "{}");
      }
      out.tool_calls.push_back(std::move(c));
    }
  }
  if (root.contains("usage")) {
    out.usage.prompt_tokens = root["usage"].value("prompt_tokens", 0);
    out.usage.completion_tokens = root["usage"].value("completion_tokens", 0);
  }
  return out;
}

}  // namespace cue::llm
