// LLM 客户端抽象：与 agent4cpp 的 ILLMClient 同构，但把「离线可回归」做成一等公民
// - ScriptedClient：单测/回放，按脚本返回，不碰网络
// - OpenAiCompatibleClient：真实调用（M3b 接入）
#pragma once

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
  };

  explicit OpenAiCompatibleClient(Config cfg);
  std::expected<ChatResponse, std::string> chat(const ChatRequest& req) override;

 private:
  Config cfg_;
};

}  // namespace cue::llm
