#include "cue/tool/tool.h"

#include <cmath>
#include <stdexcept>

namespace cue::tool {

const char* risk_name(Risk r) {
  switch (r) {
    case Risk::ReadOnly: return "read-only";
    case Risk::Mutating: return "mutating";
    case Risk::Dangerous: return "dangerous";
  }
  return "unknown";
}

namespace {

// integer 的宽松判定：模型常把 5000 写成 5000.0（JSON 浮点写法），
// 只要数值可无损转成 int64 就接受；1.5 / NaN / 越界仍然拒绝。
bool is_integral_value(const json& v) {
  if (!v.is_number()) return false;
  if (v.is_number_integer() || v.is_number_unsigned()) return true;
  const double d = v.get<double>();
  if (!std::isfinite(d) || d != std::trunc(d)) return false;
  return d >= -9.2e18 && d <= 9.2e18;
}

}  // namespace

std::expected<void, std::string> validate_args(const ToolSpec& spec, const json& args) {
  if (!args.is_object()) return std::unexpected("args must be a JSON object");

  // schema 声明了 additionalProperties=false，校验必须跟着拒未知字段：
  // 否则模型发明一个参数名会被静默忽略，模型以为生效、实际没生效。
  for (const auto& [key, _] : args.items()) {
    bool declared = false;
    for (const auto& p : spec.params) {
      if (p.name == key) {
        declared = true;
        break;
      }
    }
    if (!declared) return std::unexpected("unknown param: " + key);
  }

  for (const auto& p : spec.params) {
    if (!args.contains(p.name)) {
      if (p.required) return std::unexpected("missing required param: " + p.name);
      continue;
    }
    const auto& v = args[p.name];
    if (p.type == "string" && !v.is_string()) return std::unexpected(p.name + " must be string");
    if (p.type == "integer" && !is_integral_value(v)) return std::unexpected(p.name + " must be integer");
    if (p.type == "number" && !v.is_number()) return std::unexpected(p.name + " must be number");
    if (p.type == "boolean" && !v.is_boolean()) return std::unexpected(p.name + " must be boolean");

    // soft_enum：enum 只作为 schema 提示（见 ParamSpec 注释），由 handler 自己拒绝并留痕
    if (!p.enum_values.empty() && !p.soft_enum) {
      bool hit = false;
      if (v.is_string()) {
        for (const auto& e : p.enum_values) {
          if (v.get<std::string>() == e) {
            hit = true;
            break;
          }
        }
      }
      if (!hit) return std::unexpected(p.name + " value not in enum");
    }

    if (p.has_range && v.is_number()) {
      const double d = v.get<double>();
      if (d < p.min || d > p.max) {
        return std::unexpected(p.name + " out of range [" + std::to_string(p.min) + ", " +
                               std::to_string(p.max) + "]");
      }
    }
  }
  return {};
}

void Registry::add(ToolSpec spec, ToolHandler handler) {
  if (tools_.count(spec.name)) throw std::runtime_error("duplicate tool: " + spec.name);
  // 必须先取出 name：Entry{std::move(spec), ...} 与 spec.name 的求值顺序未定义，
  // 若先 move 再取 name，map 的 key 会是空串（实测踩过：所有工具变成"找不到"）
  const std::string name = spec.name;
  specs_.push_back(spec);
  tools_.emplace(name, Entry{std::move(spec), std::move(handler)});
}

json Registry::schema() const {
  json tools = json::array();
  for (const auto& spec : specs_) {
    json props = json::object();
    json required = json::array();
    for (const auto& p : spec.params) {
      json pj{{"type", p.type}, {"description", p.desc}};
      if (!p.enum_values.empty()) pj["enum"] = p.enum_values;
      if (p.has_range) {
        pj["minimum"] = p.min;
        pj["maximum"] = p.max;
      }
      props[p.name] = std::move(pj);
      if (p.required) required.push_back(p.name);
    }
    json params{{"type", "object"}, {"properties", std::move(props)}};
    if (!required.empty()) params["required"] = std::move(required);
    params["additionalProperties"] = false;
    tools.push_back({{"type", "function"},
                     {"function",
                      {{"name", spec.name}, {"description", spec.desc}, {"parameters", std::move(params)}}}});
  }
  return tools;
}

std::expected<ToolResult, std::string> Registry::call(const std::string& name, const json& args) const {
  auto it = tools_.find(name);
  if (it == tools_.end()) return std::unexpected("unknown tool: " + name);
  auto v = validate_args(it->second.spec, args);
  if (!v) return std::unexpected(v.error());
  try {
    return it->second.handler(args);
  } catch (const std::exception& e) {
    return std::unexpected(std::string("tool threw: ") + e.what());
  }
}

GuardedCall Registry::call_guarded(const std::string& name, const json& args, const CallContext& ctx,
                                   const std::function<void(Risk)>& on_approved) const {
  GuardedCall out;
  out.risk = risk_of(name);
  const ToolSpec* spec = find_spec(name);

  bool approved = true;
  if (out.risk != Risk::ReadOnly) {
    if (spec == nullptr) {
      approved = false;  // 未知工具按最危险处理：无 spec 就没有可审批的对象
    } else if (ctx.mode == ApprovalMode::Deny) {
      approved = false;
    } else if (ctx.mode == ApprovalMode::Auto) {
      if (out.risk == Risk::Dangerous) {
        bool allowed = false;
        for (const auto& a : ctx.allowed_dangerous) {
          if (a == name) {
            allowed = true;
            break;
          }
        }
        approved = allowed;
      } else if (out.risk == Risk::Mutating && !ctx.allowed_mutating.empty()) {
        // 只有调用方显式给了写白名单才收紧（空 = 全部放行，旧行为不变）。
        // 这一条是为评测加的：pipeline_stop 会把被测管线停掉，必须受 case 的
        // allowed_actions 约束，但它不值得升成 Dangerous（那会让所有 case 都要
        // 重新授权一遍写工具）。
        approved = false;
        for (const auto& a : ctx.allowed_mutating) {
          if (a == name) {
            approved = true;
            break;
          }
        }
      }
    } else {  // Ask：没有回调就等于没人批准
      approved = ctx.approve ? ctx.approve(*spec, out.risk, args) : false;
    }
  }

  if (!approved) {
    out.status = GuardedCall::Status::ApprovalDenied;
    out.error = std::string("被拒绝（") + risk_name(out.risk) + "）";
    return out;
  }

  if (on_approved) on_approved(out.risk);
  auto res = call(name, args);
  if (!res) {
    out.status = GuardedCall::Status::Failed;
    out.error = res.error();
    return out;
  }
  out.status = GuardedCall::Status::Executed;
  out.result = std::move(*res);
  return out;
}

Risk Registry::risk_of(const std::string& name) const {
  auto it = tools_.find(name);
  if (it == tools_.end()) return Risk::Dangerous;  // 未知工具按最危险处理
  return it->second.spec.risk;
}

bool Registry::has(const std::string& name) const { return tools_.count(name) > 0; }

const ToolSpec* Registry::find_spec(const std::string& name) const {
  auto it = tools_.find(name);
  if (it == tools_.end()) return nullptr;
  return &it->second.spec;
}

bool Registry::is_observation_tool(const std::string& name) {
  return name == "query_metrics" || name == "fetch_observation";
}

}  // namespace cue::tool
