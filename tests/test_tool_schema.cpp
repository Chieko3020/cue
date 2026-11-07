// N-14：tool/schema 一致性测试——真实注册表 ↔ 模型侧契约的逐项比对
//
// 存在理由：schema() 是**发给模型**的那份契约（OpenAI function calling 的 tools 数组），
// 而工具的真实声明在 src/pipeline/tools.cpp 的 ToolSpec 里。此前两者只靠"代码看着一致"
// 保证——tests/ 里没有任何一处调用 .schema()（`grep -rn "schema()" tests/` 为空），于是
// 下面这些漂移全都会被静默放行，模型只拿到坏 schema：
//   * 新增参数忘了写 desc          → 模型只能靠参数名猜含义
//   * enum_values 写错一个值/漏一个 → 模型传了 schema 提示的值却被校验层拒
//   * required 与 has_range 矛盾    → schema 说可选、校验层说必填（或反过来）
//   * 只往 schema 里加字段、忘了在 ToolSpec 里声明（或反过来）
//
// 做法：用**真实注册表**（build_real_tools，cue/cue-serve/cue-eval 三个入口共用的那一份）
// 造 Registry，再把 schema() 的输出与 reg.specs() 里同一份 ToolSpec 逐项比对。
// 期望值一律取自 ToolSpec 本身，**不在这里另抄一份**——手抄期望值等于自证：两份一起改
// 就永远不会失败，测试也就失去了意义。
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <limits>
#include <filesystem>
#include <chrono>

#include "cue/llm/cassette.h"
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cue/tool/tool.h"
#include "cue/tool/tools.h"

using json = nlohmann::json;
namespace tool = cue::tool;
namespace pipeline = cue::pipeline;

namespace {

int failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}

// 失败时打出「期望 / 实际」两行：故意破坏时能直接从 ctest 输出里看出是哪一项在锁。
void check_str(const std::string& got, const std::string& want, const std::string& what) {
  if (got == want) return;
  std::printf("FAIL: %s\n  期望: \"%s\"\n  实际: \"%s\"\n", what.c_str(), want.c_str(), got.c_str());
  ++failures;
}

void check_json(const json& got, const json& want, const std::string& what) {
  if (got == want) return;
  std::printf("FAIL: %s\n  期望: %s\n  实际: %s\n", what.c_str(), want.dump().c_str(), got.dump().c_str());
  ++failures;
}

void check_size(std::size_t got, std::size_t want, const std::string& what) {
  if (got == want) return;
  std::printf("FAIL: %s\n  期望: %zu\n  实际: %zu\n", what.c_str(), want, got);
  ++failures;
}

void check_num(double got, double want, const std::string& what) {
  if (got == want) return;
  std::printf("FAIL: %s\n  期望: %g\n  实际: %g\n", what.c_str(), want, got);
  ++failures;
}

// 对象的键集（排序后）。断言的是"schema 里不许多出实现读不到的键"：
// 模型看得见、引擎读不到的字段等于骗模型（validate_args 按 additionalProperties=false
// 会把它当未知参数拒掉），所以键集本身也是契约的一部分。
json keys_of(const json& obj) {
  std::vector<std::string> ks;
  for (auto it = obj.begin(); it != obj.end(); ++it) ks.push_back(it.key());
  std::sort(ks.begin(), ks.end());
  return json(ks);
}

// 参数在 schema 里**允许**出现的键，按 ParamSpec 的字段唯一确定：
//   type/description 恒有；enum_values 非空才有 enum；has_range 才有 minimum/maximum。
json allowed_param_keys(const tool::ParamSpec& p) {
  std::vector<std::string> ks = {"description", "type"};
  if (!p.enum_values.empty() && !p.soft_enum) ks.push_back("enum");
  if (p.type == "array" && !p.items_type.empty()) ks.push_back("items");
  if (p.has_range) {
    ks.push_back("maximum");
    ks.push_back("minimum");
  }
  std::sort(ks.begin(), ks.end());
  return json(ks);
}

// 只记录"被要求做什么"的假管线：schema 生成只读 ToolSpec，不碰 IServicePipeline 的方法，
// 所以这里的实现全是空桩。用假后端而不是真 cued：本测试不需要 FFmpeg / WS / 端口。
class FakePipeline : public pipeline::IServicePipeline {
 public:
  bool start(std::string&) override {
    running_ = true;
    return true;
  }
  void stop() override { running_ = false; }
  bool running() const override { return running_; }
  pipeline::PipelineMetrics latest() const override { return {}; }
  pipeline::ProbeMetrics probe_latest() const override { return {}; }
  void set_event_callback(std::function<void(const pipeline::PipelineEvent&)>) override {}
  std::vector<pipeline::PipelineEvent> recent_events(std::size_t) const override { return {}; }
  bool set_param(const std::string&, const std::string&, std::string&) override { return true; }
  bool inject_fault(const std::string&, double, std::string&) override { return true; }
  std::string observation(const std::string&, int, std::string&) const override { return {}; }

  bool running_ = false;
};

// 核心比对：schema() 的输出 vs reg.specs()。
// 六类断言对应任务书 A.1~A.6，顺序也照它排，便于对照。
void compare_registry(const tool::Registry& reg, const char* tag) {
  const json schema = reg.schema();
  const auto& specs = reg.specs();
  const std::string pfx = std::string(tag) + ": ";

  // ---------- 1) 工具集合与顺序 ----------
  // 顺序敏感：schema() 按 specs_ 的注册顺序生成，模型看到 tools 数组的顺序就是这个顺序。
  // 顺序漂移本身不致命，但它意味着"schema 不是从这份注册表算出来的"——那才是致命的。
  check(schema.is_array(), pfx + "schema() 返回数组");
  check_size(schema.size(), specs.size(), pfx + "工具数量与注册表一致（不多不少）");
  for (std::size_t i = 0; i < specs.size() && i < schema.size(); ++i) {
    const std::string where = pfx + "第 " + std::to_string(i) + " 项（" + specs[i].name + "）";
    check(schema[i].contains("function"), where + ": 是 function 类型条目");
    if (!schema[i].contains("function")) continue;
    const json& fn = schema[i]["function"];

    check_str(schema[i].value("type", std::string()), "function", where + ": type=function");
    // 外层的键集也一样：多一个模型不认识的键（例如把内部字段漏出去）就是坏 schema
    check_json(keys_of(schema[i]), json({"function", "type"}), where + ": 条目键集为 {type,function}");
    check_json(keys_of(fn), json({"description", "name", "parameters"}),
               where + ": function 键集为 {name,description,parameters}");

    check_str(fn.value("name", std::string()), specs[i].name,
              where + ": 工具名与注册顺序一致（第 " + std::to_string(i) + " 位）");
    check(reg.has(specs[i].name), where + ": 注册表可查（反向：schema 里的名字注册表里真有）");

    // ---------- 2) description ----------
    const std::string desc = fn.value("description", std::string());
    check(!desc.empty(), where + ": 工具的 description 非空（模型可用性的下限）");
    check_str(desc, specs[i].desc, where + ": description == ToolSpec::desc");

    // ---------- 3) 参数：存在 / type / required / enum ----------
    check(fn.contains("parameters"), where + ": 带 parameters");
    if (!fn.contains("parameters")) continue;
    const json& params = fn["parameters"];
    check_str(params.value("type", std::string()), "object", where + ": parameters.type=object");
    // 与 validate_args 的"未知参数直接拒"必须一致：声明了 false 校验就必须跟着拒，
    // 否则模型发明一个参数名会被静默忽略（它以为生效、实际没生效）。
    check(params.value("additionalProperties", true) == false,
          where + ": parameters.additionalProperties=false");
    check(params.contains("properties"), where + ": 带 properties");
    if (!params.contains("properties")) continue;
    const json& props = params["properties"];

    check_size(props.size(), specs[i].params.size(),
               where + ": 参数数量与声明一致（反向：schema 里没有未声明的参数）");

    // required：实现口径是「按 params 声明顺序追加名字，且一个必填都没有时**整键不出现**」。
    // 断言按这个口径写，而不是"空数组也算对"——空数组与整键缺席对模型是不同的输入。
    json want_required = json::array();
    for (const auto& p : specs[i].params) {
      if (p.required) want_required.push_back(p.name);
    }
    if (want_required.empty()) {
      check(!params.contains("required"), where + ": 无必填参数时不出现 required 键");
    } else {
      check(params.contains("required"), where + ": 有必填参数时带 required");
      // 顺序敏感：required 与声明顺序一致（模型侧的字段预览顺序跟着它）
      check_json(params.value("required", json::array()), want_required,
                 where + ": required 逐项一致（顺序敏感）");
    }

    for (const auto& p : specs[i].params) {
      const std::string w = where + " 参数 " + p.name;
      check(props.contains(p.name), w + ": schema 里存在");
      if (!props.contains(p.name)) continue;
      const json& pj = props[p.name];

      // 键集白名单：多一个实现读不到的键就是"模型看得见、引擎读不到"的假参数
      check_json(keys_of(pj), allowed_param_keys(p), w + ": 键集恰为 ParamSpec 决定的那几个");

      check_str(pj.value("type", std::string()), p.type, w + ": type 一致");

      // description 必须存在且非空：空描述等于让模型猜参数语义，单靠 type 猜不出来
      const std::string pdesc = pj.value("description", std::string());
      check(!pdesc.empty(), w + ": description 非空（模型可用性的下限）");
      const std::string expected_desc = p.soft_enum && !p.enum_values.empty()
          ? p.desc + " Suggested values (not exhaustive): " + json(p.enum_values).dump() : p.desc;
      check_str(pdesc, expected_desc, w + ": description preserves declaration and advisory values");
      if (p.type == "array" && !p.items_type.empty())
        check_json(pj.value("items", json::object()), json{{"type", p.items_type}}, w + ": array items contract");

      // enum：顺序敏感。实现是按 enum_values 原样赋值数组的；顺序进线格式后模型对
      // 枚举的偏好会受影响（越靠前越容易被选中），所以"值集合相同但顺序不同"也算漂移。
      if (p.enum_values.empty() || p.soft_enum) {
        check(!pj.contains("enum"), w + ": 无 enum_values 时 schema 不出现 enum 键");
      } else {
        check(pj.contains("enum"), w + ": 有 enum_values 时 schema 带 enum");
        check_json(pj.value("enum", json::array()), json(p.enum_values),
                   w + ": enum 逐个一致（顺序敏感）");
      }

      // ---------- 4) 范围 ----------
      if (p.has_range) {
        check(pj.contains("minimum") && pj.contains("maximum"),
              w + ": has_range=true 时 schema 带 minimum/maximum");
        check_num(pj.value("minimum", -1e300), p.min, w + ": minimum == ParamSpec::min");
        check_num(pj.value("maximum", -1e300), p.max, w + ": maximum == ParamSpec::max");
      } else {
        // 实现口径：整键缺席，不是 null。null 在 JSON Schema 里是无意义值，而且会让
        // "有没有范围"这件事在模型侧变得模糊（null 与缺键在 function calling 里表现不同）
        check(!pj.contains("minimum") && !pj.contains("maximum"),
              w + ": has_range=false 时不出现 minimum/maximum（整键缺席，不是 null）");
      }
    }

    // ---------- 6) 反向：schema 里的参数必须在 ToolSpec 里有声明 ----------
    for (auto it = props.begin(); it != props.end(); ++it) {
      bool declared = false;
      for (const auto& p : specs[i].params) {
        if (p.name == it.key()) {
          declared = true;
          break;
        }
      }
      check(declared, where + " 参数 " + it.key() + ": 在 ToolSpec 里确有声明（反向）");
    }

    // ---------- 5) risk ----------
    // 按当前实现口径，risk **不进 schema**：审批是引擎侧的事（call_guarded 读 risk_of），
    // 模型看不到危险级别，也就无法据此挑"哪个工具看起来最无害"。
    // 所以"映射正确"在这里断言两件事：
    //   (a) schema 里没有任何风险字段 —— 若将来决定给模型看，必须同步改这条断言（有意为之）；
    //   (b) 引擎派发用的 risk_of(name) 与声明的 ToolSpec::risk 一致 —— specs_（vector）
    //       与 tools_（map）是两个容器，add() 里就出过一次因求值顺序把 name move 成空串
    //       而导致两边分叉的事故，这里锁的就是那类漂移。
    check(!fn.contains("risk"), where + ": schema 不向模型暴露 risk（审批口径在引擎侧）");
    check(!params.contains("risk"), where + ": parameters 不向模型暴露 risk");
    check(reg.risk_of(specs[i].name) == specs[i].risk,
          where + ": risk_of() 与 ToolSpec::risk 一致（Specs 与 tools_ 未分叉）");
  }
}

}  // namespace

int main() {
  // ---------- 真实注册表 ----------
  FakePipeline pipe;
  pipe.running_ = true;
  tool::Registry reg;
  pipeline::build_real_tools(reg, pipe, nullptr);

  // 数量本身也要断言：少了工具的话下面的逐项比对会"少比一项"而照样全绿。
  // 8 = FR-TOOL.1 的完整清单（生命周期三个由 add_lifecycle_tools 在 build_real_tools
  // 末尾注册，cue / cue-serve / cue-eval 拿到的是同一份）。
  check_size(reg.specs().size(), 8, "真实注册表恰好 8 个工具（FR-TOOL.1）");
  compare_registry(reg, "build_real_tools");

  // ---------- 比较器自检 ----------
  // 另造一个只含一个工具的小注册表，字段组合刻意刁钻（可选+enum、必填+range、
  // 无 enum 无 range 的裸 string）。目的不是测这个假工具，而是证明 compare_registry
  // 真的在**读 schema 的输出**：若 schema() 将来被改成返回一份硬编码的 tools 数组，
  // 或者比较器被写成空转，这条会立刻失败。期望值仍然只来自这份 ToolSpec。
  {
    tool::Registry tiny;
    tiny.add(tool::ToolSpec{.name = "n14_selftest",
                            .desc = "比较器自检用的工具（不参与真实注册表）",
                            .risk = tool::Risk::Mutating,
                            .params = {{.name = "mode",
                                        .type = "string",
                                        .desc = "模式",
                                        .required = true,
                                        .enum_values = {"a", "b"},
                                        .has_range = false,
                                        .min = 0,
                                        .max = 0},
                                       {.name = "limit",
                                        .type = "integer",
                                        .desc = "上限",
                                        .required = false,
                                        .enum_values = {},
                                        .has_range = true,
                                        .min = 7,
                                        .max = 99},
                                       {.name = "note",
                                        .type = "string",
                                        .desc = "备注",
                                        .required = false,
                                        .enum_values = {},
                                        .has_range = false,
                                        .min = 0,
                                        .max = 0}}},
              [](const json&) { return tool::ToolResult{}; });
    compare_registry(tiny, "自检注册表");
  }

  // Exercise actual validation, rather than only comparing schema with its source.
  {
    const auto* shell = reg.find_spec("run_shell");
    check(shell != nullptr, "real run_shell exists");
    if (shell) {
      check(bool(tool::validate_args(*shell, json{{"cmd", "/usr/bin/ls"}, {"args", {"-l"}}})),
            "absolute whitelist path remains allowed by advisory validation");
      check(!tool::validate_args(*shell, json{{"cmd", "ls"}, {"args", "-l"}}), "reject scalar args");
      check(!tool::validate_args(*shell, json{{"cmd", "ls"}, {"args", {"-l", 1}}}), "reject mixed args");
      check(!tool::validate_args(*shell, json{{"cmd", "ls"}, {"timeout_ms", 199}}), "reject below range");
      check(!tool::validate_args(*shell, json{{"cmd", "ls"}, {"unknown", 1}}), "reject unknown param");
      check(!tool::validate_args(*shell, json::object()), "reject missing required cmd");
      check(bool(tool::validate_args(*shell, json{{"cmd", "ls"}, {"timeout_ms", 5000.0}})), "integral float accepted");
      check(!tool::validate_args(*shell, json{{"cmd", "ls"}, {"timeout_ms", 5000.5}}), "fractional integer rejected");
      // Approval still precedes execution; no shell process is launched here.
      tool::CallContext deny;
      deny.mode = tool::ApprovalMode::Deny;
      check(reg.call_guarded("run_shell", json{{"cmd", "/usr/bin/ls"}}, deny).status ==
                tool::GuardedCall::Status::ApprovalDenied, "advisory schema does not bypass approval");
    }
    const auto* metrics = reg.find_spec("query_metrics");
    if (metrics) check(!tool::validate_args(*metrics, json{{"window_ms", "5000"}}), "scalar type checked");
    tool::ToolSpec scalar{.name = "bounds", .desc = "numeric edge contracts",
      .params = {{.name = "i", .type = "integer", .desc = "signed integer", .enum_values = {}},
                 {.name = "n", .type = "number", .desc = "finite number", .enum_values = {}}}};
    check(!tool::validate_args(scalar, json{{"i", UINT64_MAX}, {"n", 0}}), "unsigned int64 overflow rejected");
    check(!tool::validate_args(scalar, json{{"i", 0x1p63}, {"n", 0}}), "float int64 overflow rejected");
    check(bool(tool::validate_args(scalar, json{{"i", -0x1p63}, {"n", 0}})), "int64 minimum accepted");
    check(!tool::validate_args(scalar, json{{"i", 0}, {"n", std::numeric_limits<double>::quiet_NaN()}}), "NaN rejected");
    check(!tool::validate_args(scalar, json{{"i", 0}, {"n", std::numeric_limits<double>::infinity()}}), "infinity rejected");
    const auto* fault = reg.find_spec("inject_fault");
    if (fault) check(!tool::validate_args(*fault, json{{"kind", "invented"}, {"value", 0}}), "hard enum remains enforced");
  }

  {
    // Fresh cassette with the actual current registry schema. Public schema
    // changes legitimately change old keys; never rewrite the key algorithm.
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / ("cue-c12-schema-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()));
    cue::llm::ChatRequest request;
    request.model = "scripted";
    request.tools = reg.schema();
    cue::llm::ChatMessage message;
    message.role = "user"; message.content = "schema round trip";
    request.messages.push_back(message);
    cue::llm::ScriptedClient scripted;
    cue::llm::ChatResponse response;
    response.content = "recorded response";
    response.usage = {71, 19};
    scripted.push(response);
    const auto generated = scripted.chat(request);
    check(bool(generated), "scripted schema response generated");
    if (generated) {
      const auto written = cue::llm::cassette_append(dir.string(), request, "", *generated);
      check(bool(written), "actual schema cassette recorded");
      cue::llm::CassetteClient replay({.dir = dir.string(), .default_model = ""});
      const auto result = replay.chat(request);
      check(result && result->content == response.content && result->usage.prompt_tokens == 71 &&
          result->usage.completion_tokens == 19, "actual schema cassette replay preserves response and usage");
      check(replay.stats().hits == 1 && replay.stats().misses == 0, "actual schema cassette key hits");
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
    check(!ec && !fs::exists(dir), "temporary schema cassette cleaned up");
  }

  if (failures == 0) {
    std::printf("test_tool_schema: OK\n");
    return 0;
  }
  std::printf("test_tool_schema: %d failure(s)\n", failures);
  return 1;
}
