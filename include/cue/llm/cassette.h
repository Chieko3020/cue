// Cassette：LLM 调用的录制与回放（SPEC §7.9 / §8）
//
// 设计要点：
// - key = sha256(归一化请求)，**不含时间戳/随机数**；同一 prompts 任意次调用命中同一 key；
// - 文件：<dir>/<key>.jsonl，每行一条录制（同一 key 多次调用按行序消费）；
// - 回放 miss 必须报错（不静默走网络），usage 照抄录制值（成本统计才能复现）；
// - 格式带版本字段 v，便于将来演进；未知版本直接报错而不是猜。
#pragma once

#include <cstddef>
#include <expected>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "cue/llm/client.h"

namespace cue::llm {

// 当前格式版本。新增字段不升版本（向后兼容），语义变化才升。
inline constexpr int kCassetteVersion = 1;

// 归一化后的请求（字段顺序由 nlohmann::json 的键序保证稳定）。
// tool_call_id 映射成首次出现序号（tcid-0/1/...），随机 id 不影响 key。
json cassette_request_json(const ChatRequest& req, const std::string& default_model = {});

// sha256(cassette_request_json(...).dump())，64 位小写十六进制。
std::string cassette_key(const ChatRequest& req, const std::string& default_model = {});

// 追加一条录制到 <dir>/<key>.jsonl；dir 为空时什么都不做。
// 失败返回错误（调用方决定是否降级：录制是辅助能力，不应让成功调用失败）。
std::expected<void, std::string> cassette_append(const std::string& dir, const ChatRequest& req,
                                                const std::string& default_model,
                                                const ChatResponse& resp);

// 评测默认的 replay 客户端：按 key 读 <dir>/<key>.jsonl，命中按行序返回。
// miss / 版本不识别 / 文件损坏一律返回错误；绝不退回网络。
class CassetteClient : public ILlmClient {
 public:
  struct Config {
    std::string dir = "cassettes";
    // req.model 为空时用于计算 key（与录制端 OpenAiCompatibleClient 的 model 保持一致）
    std::string default_model;
  };

  struct Stats {
    std::size_t hits = 0;
    std::size_t misses = 0;
  };

  explicit CassetteClient(Config cfg);
  ~CassetteClient() override;
  CassetteClient(const CassetteClient&) = delete;
  CassetteClient& operator=(const CassetteClient&) = delete;
  CassetteClient(CassetteClient&&) = delete;
  CassetteClient& operator=(CassetteClient&&) = delete;

  std::expected<ChatResponse, std::string> chat(const ChatRequest& req) override;
  Stats stats() const;

 private:
  struct Impl;
  Config cfg_;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cue::llm
