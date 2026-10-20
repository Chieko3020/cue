// 工具层：声明 → JSON Schema → 校验 → 分发
// 三职责分离（借鉴 agent4cpp，但升级为：声明式 spec + 三级风险 + 锁外执行）
#pragma once

#include <expected>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace cue::tool {

using json = nlohmann::json;

enum class Risk { ReadOnly, Mutating, Dangerous };

// 审批模式：评测用 auto（自动批准但全量留痕），交互用 ask，禁危险动作用 deny
enum class ApprovalMode { Ask, Auto, Deny };

struct ToolSpec;  // 前向声明：CallContext 的审批回调用它

struct CallContext {
  ApprovalMode mode = ApprovalMode::Auto;
  // 评测模式下 case 允许的危险动作白名单（空 = 不允许 dangerous）
  std::vector<std::string> allowed_dangerous;
  // Ask 模式的回调，返回是否批准。参数带 Risk 是为了让审批 UI 能按风险分级呈现；
  // 阶段（agent::Stage）不在这里传——tool 层不得 include agent 层（SPEC §7.3）。
  std::function<bool(const ToolSpec&, Risk, const json& args)> approve;
};

const char* risk_name(Risk r);

struct ParamSpec {
  std::string name;
  std::string type;  // string / integer / number / boolean
  std::string desc;
  bool required = true;
  std::vector<std::string> enum_values;
  bool has_range = false;
  double min = 0;
  double max = 0;
};

struct ToolSpec {
  std::string name;
  std::string desc;
  Risk risk = Risk::ReadOnly;
  std::vector<ParamSpec> params;
};

struct ToolResult {
  bool ok = true;
  std::string content;  // 回灌给模型的文本（预聚合、可读）
  json data = json::object();
};

using ToolHandler = std::function<ToolResult(const json& args)>;

// 受策略保护的一次调用结果：必须能区分「审批拒绝」「参数/执行失败」「成功」，
// 否则调用方只能把拒绝当错误回灌，模型看不到"去申请审批"这个信息。
struct GuardedCall {
  enum class Status { Executed, ApprovalDenied, Failed };
  Status status = Status::Failed;
  Risk risk = Risk::Dangerous;
  ToolResult result;
  std::string error;
};

class Registry {
 public:
  void add(ToolSpec spec, ToolHandler handler);

  // OpenAI function calling 格式的 tools 数组
  json schema() const;

  // 三层校验：JSON 类型 → 枚举/范围 → 语义（由 handler 自行检查）。
  // 注意：本方法不做风险/审批判断，仅供已知安全的调用点（内部/单测）使用；
  // 面向模型或外部入口一律用 call_guarded。
  std::expected<ToolResult, std::string> call(const std::string& name, const json& args) const;

  // 风险策略的唯一实现处（SPEC §7.4：dangerous 必须显式授权）：
  //   Deny → 拒一切非只读；Auto → 只放行 allowed_dangerous 内的 dangerous；
  //   Ask  → 交给 ctx.approve，未提供回调按拒绝处理；未知工具按 Dangerous + 拒绝。
  // on_approved 在"已批准、尚未执行"时调用（审计事件需要记录意图先于结果）。
  GuardedCall call_guarded(const std::string& name, const json& args, const CallContext& ctx,
                           const std::function<void(Risk)>& on_approved = nullptr) const;

  Risk risk_of(const std::string& name) const;
  bool has(const std::string& name) const;
  const std::vector<ToolSpec>& specs() const { return specs_; }
  // 按名取 spec（审批回调必须拿到「当前被调用工具」的 spec，不能拿 specs().front()）。
  // 找不到返回 nullptr —— 调用方按 Deny 处理，不要回退到任何默认 spec。
  const ToolSpec* find_spec(const std::string& name) const;
  // 是否观测类工具（SPEC §7.5 collect→diagnose 守卫用）。
  // 用名字表而不是 risk：两者语义不同，观测工具将来可能新增。
  static bool is_observation_tool(const std::string& name);

 private:
  struct Entry {
    ToolSpec spec;
    ToolHandler handler;
  };
  std::map<std::string, Entry> tools_;
  std::vector<ToolSpec> specs_;
};

// 校验单个工具的参数（导出供单测）
std::expected<void, std::string> validate_args(const ToolSpec& spec, const json& args);

}  // namespace cue::tool
