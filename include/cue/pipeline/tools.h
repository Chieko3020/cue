// 真实管线 → 工具层：把 ProcessPipelineControl 包装成 agent 可调用的工具。
// 单独放一层的理由（M4b）：`cue`（交互）与 `cue-eval`（评测）必须用**同一套**工具语义，
// 否则「评测里 agent 能做的」和「交互里 agent 能做的」会分叉。
//
// 观测留痕：每个工具返回（成功或失败）都会追加到可选 trace，评测用它做证据校验——
// 只有真实发生过的观测才能被 agent 引用（SPEC §7.6 score.evidence）。
#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "cue/pipeline/process_control.h"
#include "cue/tool/tool.h"

namespace cue::pipeline {

// 一次工具调用的观测留痕（tool 名 + 回灌给模型的原文 + 结构化数据）
struct ToolTrace {
  std::string tool;
  std::string text;
  nlohmann::json data = nlohmann::json::object();
};

// 交互模式（cue run）与评测模式（cue-eval --mode real）共用的系统提示词。
// 放在同一处，避免两条入口的 agent 语义分叉。
const char* real_ops_system_prompt();

// 指标快照 → JSON（分位无样本输出 null，pid=null 表示当前没有 cued）。
// 导出给 M5 控制台后端复用：控制台与工具层必须看到同一份字段、同一套「没采到 ≠ 0」
// 口径，否则页面上会出现工具里查不到的伪读数。
nlohmann::json metrics_json(const PipelineMetrics& m, int64_t now_us);

// 注册 4 个真实工具：query_metrics / pipeline_set_param / inject_fault / fetch_observation。
// trace 可为空（交互模式不需要留痕）。
void build_real_tools(tool::Registry& reg, ProcessPipelineControl& pipe, std::vector<ToolTrace>* trace);

}  // namespace cue::pipeline
