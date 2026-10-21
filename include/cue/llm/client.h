// LLM 客户端抽象：与 agent4cpp 的 ILLMClient 同构，但把「离线可回归」做成一等公民
// - ScriptedClient：单测/回放，按脚本返回，不碰网络
// - OpenAiCompatibleClient：真实调用（M3b 接入）
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace cue::llm {

using json = nlohmann::json;

struct ToolCall {
  std::string id;
  std::string name;
  std::string arguments;  // JSON string（与 OpenAI 一致）
};

struct ChatMessage {
  std::string role;  // system / user / assistant / tool
  std::string content;
  std::string tool_call_id;
  std::string name;
  std::vector<ToolCall> tool_calls;
};

struct Usage {
  int prompt_tokens = 0;
  int completion_tokens = 0;
};

struct ChatRequest {
  std::string model;
  std::vector<ChatMessage> messages;
  json tools = json::array();
  double temperature = 0.0;
  // 1024 会在多工具/长证据时被截断（SPEC §7.9）。默认 2048，由 Runner::Config 覆盖。
  int max_tokens = 2048;
  // 本次请求的超时（秒）；0 表示用客户端配置。Runner 用它把"剩余墙钟预算"传给 client，
  // 避免单次调用把整轮预算顶穿（SPEC §7.5）。只允许**收紧**，不能放宽客户端配置。
  int timeout_s = 0;
  // 非空且被置位 = 调用方要求中止本次请求（SIGTERM/预算耗尽）。传输层必须尽快返回
  // 失败，不允许把「用户已经放弃」拖成一次正常超时。
  // 它**不是**请求内容：cassette key 不包含它（否则回放必 miss）。
  const std::atomic<bool>* cancel = nullptr;
};

struct ChatResponse {
  std::string content;
  std::vector<ToolCall> tool_calls;
  Usage usage;
  // 原始 finish_reason（"stop" / "length" / "tool_calls" / ...），缺省为空串
  std::string finish_reason;
  // finish_reason == "length"：输出被 max_tokens 截断，工具参数可能是半截 JSON
  bool truncated = false;
};

class ILlmClient {
 public:
  virtual ~ILlmClient() = default;
  virtual std::expected<ChatResponse, std::string> chat(const ChatRequest& req) = 0;
};

// 按脚本返回；脚本耗尽后返回错误（防止无限循环掩盖 bug）
class ScriptedClient : public ILlmClient {
 public:
  void push(ChatResponse r);
  std::size_t remaining() const { return script_.size() - idx_; }
  std::expected<ChatResponse, std::string> chat(const ChatRequest& req) override;

 private:
  std::vector<ChatResponse> script_;
  std::size_t idx_ = 0;
};

// OpenAI 兼容（DeepSeek / OpenAI / 本地兼容服务）
class OpenAiCompatibleClient : public ILlmClient {
 public:
  struct Config {
    std::string base_url;  // 例如 https://api.deepseek.com
    std::string api_key;
    std::string model;
    int timeout_s = 60;
    // SPEC §7.9：重试仅对 429/5xx/网络错误，退避 200→800 ms，最多 2 次
    int max_retries = 2;
    // 响应体上限：异常端点不能把进程内存撑爆
    std::size_t max_response_bytes = 8u * 1024u * 1024u;
    // 非空 = 每次 chat() 成功后把 {key, request, response, usage, ts, model} 追加到
    // <record_dir>/<key>.jsonl（cassette 录制，SPEC §7.9）。空 = 不录。
    std::string record_dir;
  };

  explicit OpenAiCompatibleClient(Config cfg);
  ~OpenAiCompatibleClient() override;
  OpenAiCompatibleClient(const OpenAiCompatibleClient&) = delete;
  OpenAiCompatibleClient& operator=(const OpenAiCompatibleClient&) = delete;
  OpenAiCompatibleClient(OpenAiCompatibleClient&&) = delete;
  OpenAiCompatibleClient& operator=(OpenAiCompatibleClient&&) = delete;

  std::expected<ChatResponse, std::string> chat(const ChatRequest& req) override;

 private:
  struct Impl;  // CURL 句柄/头部复用（不把 libcurl 头暴露给调用方）
  Config cfg_;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cue::llm
