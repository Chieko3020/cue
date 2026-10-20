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

std::expected<void, std::string> validate_args(const ToolSpec& spec, const json& args) {
  if (!args.is_object()) return std::unexpected("args must be a JSON object");

  for (const auto& p : spec.params) {
    if (!args.contains(p.name)) {
      if (p.required) return std::unexpected("missing required param: " + p.name);
      continue;
    }
    const auto& v = args[p.name];
    if (p.type == "string" && !v.is_string()) return std::unexpected(p.name + " must be string");
    if (p.type == "integer" && !v.is_number_integer()) return std::unexpected(p.name + " must be integer");
    if (p.type == "number" && !v.is_number()) return std::unexpected(p.name + " must be number");
    if (p.type == "boolean" && !v.is_boolean()) return std::unexpected(p.name + " must be boolean");

    if (!p.enum_values.empty()) {
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

Risk Registry::risk_of(const std::string& name) const {
  auto it = tools_.find(name);
  if (it == tools_.end()) return Risk::Dangerous;  // 未知工具按最危险处理
  return it->second.spec.risk;
}

bool Registry::has(const std::string& name) const { return tools_.count(name) > 0; }

}  // namespace cue::tool
