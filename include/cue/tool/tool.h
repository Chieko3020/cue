// 工具层：声明 → JSON Schema → 校验 → 分发
// 三职责分离（借鉴 agent4cpp，但升级为：声明式 spec + 三级风险 + 锁外执行）
#pragma once

#include <expected>
#include <functional>
#include <map>
#include <optional>
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
  // Mutating 工具的限制，Auto/Ask 均适用：未设置保留交互行为；
  // 显式空 vector 拒绝全部 Mutating；非空只允许列出的工具。
  // 用 std::vector<std::string>{} 表示空授权，reset()/nullopt 解除限制。
  // Dangerous 仍独立遵守 allowed_dangerous (Auto) 或人工审批 (Ask)。
  std::optional<std::vector<std::string>> allowed_mutating;
  // Ask 模式的回调，返回是否批准。参数带 Risk 是为了让审批 UI 能按风险分级呈现；
  // 阶段（agent::Stage）不在这里传——tool 层不得 include agent 层（SPEC §7.3）。
  std::function<bool(const ToolSpec&, Risk, const json& args)> approve;
};

const char* risk_name(Risk r);

struct ParamSpec {
  std::string name;
  std::string type;  // string / integer / number / boolean / array
  std::string desc;
  bool required = true;
  std::vector<std::string> enum_values;
  bool has_range = false;
  double min = 0;
  double max = 0;
  // Suggested values appear in the description, never as a restrictive schema enum.
  // 用于 run_shell.cmd：被拒的命令必须真正走到 handler 才能落审计行（AC-11），
  // 在 schema 校验层就拒掉会让「尝试了白名单外命令」这件事在审计里消失。
  // 此时 handler 自己负责拒绝并留痕。
  bool soft_enum = false;
  // Supported array contract: empty means untyped; "string" validates each item.
  std::string items_type = {};
};

struct ToolSpec {
  std::string name;
  std::string desc;
  Risk risk = Risk::ReadOnly;
  std::vector<ParamSpec> params;
};

struct ToolResult {
  std::string content;  // 回灌给模型的文本（预聚合、可读）
  json data = json::object();
};

// 工具失败的原因（FR-TOOL.8：错误分类可区分）。
//
// 为什么不用裸字符串：模型看到笼统的 "failed" 只能猜下一步——
// 改参数？换个方法？还是这条路根本不通？分类要能**直接指导动作**，
// 所以按「接下来该做什么」分，而不是按异常类型分：
//   Argument        → 改参数重试（少给一个字段、值域超了）
//   Execution       → 换方法或换参数（命令失败、取不到数据）
//   Timeout         → 缩小范围或放弃（不是参数错）
//   Sandbox         → 换命令（被白名单/landlock 拒了，重试同一命令没用）
//   ApprovalDenied  → 去申请审批，**不要重试**（这是最容易做错的一类：
//                     把它当普通错误重试，模型会陷进死循环）
//   Internal        → 引擎自身问题，模型做什么都没用
struct ToolError {
  enum class Kind { Argument, Execution, Timeout, Sandbox, ApprovalDenied, Internal };
  Kind kind = Kind::Execution;
  std::string message;
  // 执行事实（退出码 / 耗时 / 是否超时）。失败时这些同样重要：
  // run_shell 超时与"命令不存在"都算失败，但模型该做的事完全不同。
  json data = json::object();
};

// 稳定名字（进事件流与报告，别在别处再写一遍 switch）
const char* tool_error_kind_name(ToolError::Kind k);

using ToolOutcome = std::expected<ToolResult, ToolError>;
using ToolHandler = std::function<ToolOutcome(const json& args)>;

// 受策略保护的一次调用结果。
// status 与 outcome 并存而不是二选一：审批拒绝时 outcome 也是 error，
// 但调用方通常更关心「这次到底执行了没有」——审计与阶段守卫都按 executed 判断，
// 不该让它们各自去 outcome.error().kind 里翻找。
struct GuardedCall {
  enum class Status { Executed, ApprovalDenied, Failed };
  Status status = Status::Failed;
  Risk risk = Risk::Dangerous;
  ToolOutcome outcome;   // 成功 = ToolResult；失败 = ToolError（kind 区分原因）
};

class Registry {
 public:
  void add(ToolSpec spec, ToolHandler handler);

  // OpenAI function calling 格式的 tools 数组
  json schema() const;

  // 三层校验：JSON 类型 → 枚举/范围 → 语义（由 handler 自行检查）。
  // 注意：本方法不做风险/审批判断，仅供已知安全的调用点（内部/单测）使用；
  // 面向模型或外部入口一律用 call_guarded。
  ToolOutcome call(const std::string& name, const json& args) const;

  // 风险策略的唯一实现处（SPEC §7.4：dangerous 必须显式授权）：
  //   Deny → 拒一切非只读；Auto → 只放行 allowed_dangerous 内的 dangerous；
  //   Mutating 的显式限制在 Auto/Ask 审批前生效。
  //   Ask → 限制内交给 ctx.approve，未提供回调按拒绝处理；未知工具拒绝。
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
