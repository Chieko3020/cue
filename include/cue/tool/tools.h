// 真实管线 → 工具层：把 IServicePipeline 包装成 agent 可调用的工具。
// 单独放一层的理由（M4b）：`cue`（交互）与 `cue-eval`（评测）必须用**同一套**工具语义，
// 否则「评测里 agent 能做的」和「交互里 agent 能做的」会分叉。
//
// 观测留痕：每个工具返回（成功或失败）都会追加到可选 trace，评测用它做证据校验——
// 只有真实发生过的观测才能被 agent 引用（SPEC §7.6 score.evidence）。
#pragma once

#include <atomic>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "cue/common/service_pipeline.h"
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

// build_real_tools 的可选项（默认值保持既有调用点不变）。
struct RealToolsOptions {
  // run_shell 的审计文件。空 = 不写审计；交互模式默认 "runs/audit.jsonl"，
  // 评测模式用 run 目录（runs/<run_id>/audit.jsonl），两者都不进 git。
  std::string audit_log_path;
  // AC-8：工具执行中也要能被取消（run_shell 是唯一会长时间阻塞的工具，
  // 一次 30 秒的 shell 会顶穿「SIGTERM 后 5 秒退出」）。透传到 sandbox::run。
  const std::atomic<bool>* cancel = nullptr;
  // FR-SANDBOX.8：run_shell 使用的沙箱后端（"landlock" 默认 / "docker"）。
  // 这里用字符串而不是 sandbox::Backend：本头文件属于工具层，而枚举逼每个调用方
  // include 沙箱头文件（工具层对沙箱实现保持零依赖）。非法值不会被静默当成默认值——
  // 会打一行 stderr，并由审计里的实际后端字段兜底。
  std::string sandbox_backend = "landlock";
  // docker 后端镜像（本机已有：alpine:latest）。
  std::string docker_image = "alpine:latest";
};

// 注册 8 个真实工具（FR-TOOL.1 的完整清单）：
//   query_metrics / pipeline_set_param / inject_fault / fetch_observation / run_shell
//   + pipeline_start / pipeline_stop / pipeline_restart（见 add_lifecycle_tools）
// trace 可为空（交互模式不需要留痕）。
void build_real_tools(tool::Registry& reg, IServicePipeline& pipe, std::vector<ToolTrace>* trace,
                      const RealToolsOptions& opts = {});

// 三个 lifecycle 工具的唯一实现（SPEC §7.4 的 mutating 动作）。
// cue-serve 与 build_real_tools 都调它：schema、风险级、profile 档位表只允许有一份，
// 两份拷贝迟早漂移（"控制台能做的 agent 不能做"就是这么长出来的）。
// profile 只作用于 start/restart，且真的映射成一串 set_param（不是仅记录）。
// trace 可为空：与 build_real_tools 同一套留痕约定（评测用它做证据校验）。
void add_lifecycle_tools(tool::Registry& reg, IServicePipeline& pipe,
                         std::vector<ToolTrace>* trace = nullptr);

}  // namespace cue::pipeline
