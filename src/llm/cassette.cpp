#include "cue/llm/cassette.h"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "cue/common/sha256.h"

namespace cue::llm {

namespace {

namespace fs = std::filesystem;

// 与 Config::max_response_bytes（默认 8 MiB）对齐：一行 = 请求 + 响应 + 元数据，
// 上限必须大于响应上限，否则「能录不能放」。
constexpr std::size_t kMaxCassetteLineBytes = 32u * 1024u * 1024u;

// ISO-8601（UTC，秒级）。只做留痕，**不进 key**——否则同一 prompts 每次调用都会不同。
std::string iso8601_utc_now() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) return {};
  return buf;
}

// 首次出现序号：同一条对话里 tool_call_id 的随机后缀被抹平，key 只依赖结构。
std::string mapped_id(const std::string& id, std::map<std::string, std::string>& seen) {
  if (id.empty()) return id;
  const auto it = seen.find(id);
  if (it != seen.end()) return it->second;
  const std::string mapped = "tcid-" + std::to_string(seen.size());
  seen.emplace(id, mapped);
  return mapped;
}

json message_json(const ChatMessage& m, std::map<std::string, std::string>& seen) {
  // 字段集合固定：空 content 也写 ""，避免「字段缺省 vs 空串」造成两份 key。
  json j{{"role", m.role}, {"content", m.content}};
  if (!m.tool_call_id.empty()) j["tool_call_id"] = mapped_id(m.tool_call_id, seen);
  if (!m.name.empty()) j["name"] = m.name;
  if (!m.tool_calls.empty()) {
    json arr = json::array();
    for (const auto& tc : m.tool_calls) {
      arr.push_back({{"id", mapped_id(tc.id, seen)}, {"name", tc.name}, {"arguments", tc.arguments}});
    }
    j["tool_calls"] = std::move(arr);
  }
  return j;
}

std::string effective_model(const ChatRequest& req, const std::string& default_model) {
  return req.model.empty() ? default_model : req.model;
}

json response_json(const ChatResponse& r) {
  json calls = json::array();
  for (const auto& tc : r.tool_calls) {
    calls.push_back({{"id", tc.id}, {"name", tc.name}, {"arguments", tc.arguments}});
  }
  return json{{"content", r.content},
              {"tool_calls", std::move(calls)},
              {"finish_reason", r.finish_reason},
              {"truncated", r.truncated}};
}

std::expected<ChatResponse, std::string> parse_recorded(const json& j, const std::string& where) {
  if (!j.is_object()) return std::unexpected("cassette corrupt (" + where + "): 行不是对象");
  if (!j.contains("v") || !j["v"].is_number_integer()) {
    return std::unexpected("cassette corrupt (" + where + "): 缺少版本字段 v");
  }
  const int v = j["v"].get<int>();
  if (v != kCassetteVersion) {
    return std::unexpected("cassette version unsupported (" + where + "): v=" + std::to_string(v) +
                           "，期望 v=" + std::to_string(kCassetteVersion));
  }
  if (!j.contains("response") || !j["response"].is_object()) {
    return std::unexpected("cassette corrupt (" + where + "): 缺少 response 对象");
  }
  const auto& resp = j["response"];
  ChatResponse out;
  if (resp.contains("content")) {
    if (!resp["content"].is_string()) {
      return std::unexpected("cassette corrupt (" + where + "): response.content 不是字符串");
    }
    out.content = resp["content"].get<std::string>();
  }
  if (resp.contains("finish_reason") && resp["finish_reason"].is_string()) {
    out.finish_reason = resp["finish_reason"].get<std::string>();
  }
  if (resp.contains("truncated") && resp["truncated"].is_boolean()) {
    out.truncated = resp["truncated"].get<bool>();
  }
  if (resp.contains("tool_calls")) {
    if (!resp["tool_calls"].is_array()) {
      return std::unexpected("cassette corrupt (" + where + "): response.tool_calls 不是数组");
    }
    for (const auto& tcv : resp["tool_calls"]) {
      if (!tcv.is_object()) {
        return std::unexpected("cassette corrupt (" + where + "): tool_call 不是对象");
      }
      ToolCall tc;
      if (tcv.contains("id") && tcv["id"].is_string()) tc.id = tcv["id"].get<std::string>();
      if (tcv.contains("name") && tcv["name"].is_string()) tc.name = tcv["name"].get<std::string>();
      if (tc.name.empty()) {
        return std::unexpected("cassette corrupt (" + where + "): tool_call 缺少 name");
      }
      if (tcv.contains("arguments")) {
        if (tcv["arguments"].is_string()) {
          tc.arguments = tcv["arguments"].get<std::string>();
        } else if (tcv["arguments"].is_object()) {
          tc.arguments = tcv["arguments"].dump();
        } else {
          return std::unexpected("cassette corrupt (" + where + "): tool_call arguments 类型非法");
        }
      }
      out.tool_calls.push_back(std::move(tc));
    }
  }
  // usage 必须在顶层照抄录制值（成本核算以录制时的 API 计量为准）
  const json* usage = nullptr;
  if (j.contains("usage") && j["usage"].is_object()) {
    usage = &j["usage"];
  } else if (resp.contains("usage") && resp["usage"].is_object()) {
    usage = &resp["usage"];  // 兼容早期草稿格式
  }
  if (usage) {
    const auto read = [&](const char* k, int& out) {
      if (!usage->contains(k)) return;
      if (!(*usage)[k].is_number_integer()) return;
      out = (*usage)[k].get<int>();
    };
    read("prompt_tokens", out.usage.prompt_tokens);
    read("completion_tokens", out.usage.completion_tokens);
  }
  return out;
}

std::mutex& append_mutex() {
  static std::mutex m;
  return m;
}

}  // namespace

json cassette_request_json(const ChatRequest& req, const std::string& default_model) {
  std::map<std::string, std::string> seen;
  json msgs = json::array();
  // 先按消息顺序建立 id 映射（assistant 的 tool_calls 与 tool 消息共用一个映射表）
  for (const auto& m : req.messages) {
    for (const auto& tc : m.tool_calls) (void)mapped_id(tc.id, seen);
    if (!m.tool_call_id.empty()) (void)mapped_id(m.tool_call_id, seen);
  }
  for (const auto& m : req.messages) msgs.push_back(message_json(m, seen));

  return json{{"model", effective_model(req, default_model)},
              {"messages", std::move(msgs)},
              {"tools", req.tools.is_array() ? req.tools : json::array()},
              {"temperature", req.temperature},
              {"max_tokens", req.max_tokens}};
}

std::string cassette_key(const ChatRequest& req, const std::string& default_model) {
  return cue::sha256_hex(cassette_request_json(req, default_model).dump());
}

std::expected<void, std::string> cassette_append(const std::string& dir, const ChatRequest& req,
                                                const std::string& default_model,
                                                const ChatResponse& resp) {
  if (dir.empty()) return {};

  const std::string key = cassette_key(req, default_model);
  const json entry{{"v", kCassetteVersion},
                   {"key", key},
                   {"ts", iso8601_utc_now()},
                   {"model", effective_model(req, default_model)},
                   {"request", cassette_request_json(req, default_model)},
                   {"response", response_json(resp)},
                   {"usage",
                    {{"prompt_tokens", resp.usage.prompt_tokens},
                     {"completion_tokens", resp.usage.completion_tokens}}}};

  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) return std::unexpected("无法创建 cassette 目录 " + dir + ": " + ec.message());

  const std::string path = (fs::path(dir) / (key + ".jsonl")).string();
  const std::string line = entry.dump();
  std::lock_guard<std::mutex> lock(append_mutex());
  std::ofstream out(path, std::ios::app | std::ios::binary);
  if (!out) return std::unexpected("无法打开 cassette 文件 " + path);
  out << line << '\n';
  out.flush();
  if (!out) return std::unexpected("写入 cassette 文件失败 " + path);
  return {};
}

// ---------------------------------------------------------------- CassetteClient

struct CassetteClient::Impl {
  struct Bucket {
    bool loaded = false;
    std::vector<ChatResponse> rows;
    std::size_t cursor = 0;
  };

  mutable std::mutex mu;
  std::map<std::string, Bucket> buckets;
  std::size_t hits = 0;
  std::size_t misses = 0;
};

CassetteClient::CassetteClient(Config cfg) : cfg_(std::move(cfg)), impl_(std::make_unique<Impl>()) {}

CassetteClient::~CassetteClient() = default;

std::expected<ChatResponse, std::string> CassetteClient::chat(const ChatRequest& req) {
  const std::string key = cassette_key(req, cfg_.default_model);
  const std::string path = (fs::path(cfg_.dir) / (key + ".jsonl")).string();

  std::lock_guard<std::mutex> lock(impl_->mu);
  auto& bucket = impl_->buckets[key];
  if (!bucket.loaded) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
      ++impl_->misses;
      return std::unexpected("cassette miss: " + key + "（" + path + " 不存在）");
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      ++impl_->misses;
      return std::unexpected("cassette miss: " + key + "（无法读取 " + path + "）");
    }
    std::string line;
    std::size_t line_no = 0;
    while (std::getline(in, line)) {
      ++line_no;
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) continue;
      if (line.size() > kMaxCassetteLineBytes) {
        return std::unexpected("cassette corrupt (" + path + ":" + std::to_string(line_no) +
                               "): 单行超过上限");
      }
      const json j = json::parse(line, nullptr, false);
      if (j.is_discarded()) {
        return std::unexpected("cassette corrupt (" + path + ":" + std::to_string(line_no) +
                               "): 非法 JSON");
      }
      if (!j.is_object() || !j.contains("key") || !j["key"].is_string()) {
        return std::unexpected("cassette corrupt (" + path + ":" + std::to_string(line_no) +
                               "): 缺少 key");
      }
      if (j["key"].get<std::string>() != key) {
        return std::unexpected("cassette corrupt (" + path + ":" + std::to_string(line_no) +
                               "): key 与文件名不一致");
      }
      auto parsed = parse_recorded(j, path + ":" + std::to_string(line_no));
      if (!parsed) return parsed;
      bucket.rows.push_back(std::move(*parsed));
    }
    bucket.loaded = true;
  }

  if (bucket.cursor >= bucket.rows.size()) {
    ++impl_->misses;
    return std::unexpected("cassette miss: " + key + "（已耗尽 " +
                           std::to_string(bucket.rows.size()) + " 条录制，" + path + "）");
  }
  ++impl_->hits;
  return bucket.rows[bucket.cursor++];
}

CassetteClient::Stats CassetteClient::stats() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return Stats{impl_->hits, impl_->misses};
}

}  // namespace cue::llm
