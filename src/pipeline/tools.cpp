#include "cue/pipeline/tools.h"

#include <utility>

namespace cue::pipeline {

using json = nlohmann::json;

namespace {

// 统一的留痕包装：工具 handler 返回前把结果记进 trace（失败也要记，证据校验只认真实观测）
tool::ToolResult record(std::vector<ToolTrace>* trace, const char* name, tool::ToolResult r) {
  if (trace) trace->push_back(ToolTrace{name, r.content, r.data});
  return r;
}

}  // namespace

const char* real_ops_system_prompt() {
  return "你是 Cue 的媒体管线运维 agent。你只能通过工具观测与调整管线。"
         "每轮只能调用工具；结论必须引用真实观测作为证据；每个阶段结束调用 stage_complete。"
         "根因必须使用枚举值：network_loss / network_delay / bandwidth_insufficient / "
         "encoder_too_slow / resolution_too_high / queue_backlog / keyframe_storm / clock_drift。"
         "若要取消已有的丢包注入，用 inject_fault 且 value=0。";
}

void build_real_tools(tool::Registry& reg, ProcessPipelineControl& pipe, std::vector<ToolTrace>* trace) {
  reg.add(tool::ToolSpec{.name = "query_metrics",
                         .desc = "查询媒体管线指标（六段分位 + 帧统计，预聚合）",
                         .risk = tool::Risk::ReadOnly,
                         .params = {{.name = "window_ms",
                                     .type = "integer",
                                     .desc = "窗口毫秒（当前实现固定 1s 采样）",
                                     .required = false,
                                     .enum_values = {},
                                     .has_range = true,
                                     .min = 200,
                                     .max = 60000}}},
          [&pipe, trace](const json&) {
            const auto m = pipe.latest();
            if (m.samples == 0) {
              return record(trace, "query_metrics",
                            tool::ToolResult{false, "尚无采样（cued 未收到媒体流）", json::object()});
            }
            json j{{"fps", m.fps},
                   {"frames_in", m.frames_in},
                   {"frames_out", m.frames_out},
                   {"dropped", m.dropped},
                   {"injected", m.injected},
                   {"decoded", m.decoded},
                   {"decode_errors", m.decode_errors},
                   {"recv_p50_ms", m.recv_p50},
                   {"buffer_p50_ms", m.buffer_p50},
                   {"decode_p50_ms", m.decode_p50},
                   {"decode_p99_ms", m.decode_p99},
                   {"op_p50_ms", m.op_p50},
                   {"op_p99_ms", m.op_p99},
                   {"send_p50_ms", m.send_p50}};
            return record(trace, "query_metrics", tool::ToolResult{true, "metrics=" + j.dump(), j});
          });

  reg.add(tool::ToolSpec{.name = "pipeline_set_param",
                         .desc = "修改媒体管线参数（生效方式为重启管线，会造成短暂断流）",
                         .risk = tool::Risk::Mutating,
                         .params = {{.name = "key",
                                     .type = "string",
                                     .desc = "参数名",
                                     .required = true,
                                     .enum_values = {"bitrate_kbps", "gop", "preset", "transcode"}},
                                    {.name = "value",
                                     .type = "string",
                                     .desc = "目标值",
                                     .required = true,
                                     .enum_values = {},
                                     .has_range = false,
                                     .min = 0,
                                     .max = 0}}},
          [&pipe, trace](const json& a) {
            std::string err;
            const std::string k = a["key"].get<std::string>();
            const std::string v = a["value"].get<std::string>();
            if (!pipe.set_param(k, v, err)) {
              return record(trace, "pipeline_set_param",
                            tool::ToolResult{false, "设置失败: " + err, json::object()});
            }
            return record(trace, "pipeline_set_param",
                          tool::ToolResult{true, "已设置 " + k + "=" + v + "（管线已重启）", a});
          });

  reg.add(tool::ToolSpec{.name = "inject_fault",
                         .desc = "注入网络损伤（危险动作；取消注入用 value=0）",
                         .risk = tool::Risk::Dangerous,
                         .params = {{.name = "kind",
                                     .type = "string",
                                     .desc = "类型",
                                     .required = true,
                                     .enum_values = {"drop"}},
                                    {.name = "value",
                                     .type = "number",
                                     .desc = "丢包百分比 0-100",
                                     .required = true,
                                     .enum_values = {},
                                     .has_range = true,
                                     .min = 0,
                                     .max = 100}}},
          [&pipe, trace](const json& a) {
            std::string err;
            const std::string k = a["kind"].get<std::string>();
            const double v = a["value"].get<double>();
            if (!pipe.inject_fault(k, v, err)) {
              return record(trace, "inject_fault", tool::ToolResult{false, "注入失败: " + err, json::object()});
            }
            return record(trace, "inject_fault",
                          tool::ToolResult{true, "已注入 " + k + "=" + std::to_string(v) + "%", a});
          });

  reg.add(tool::ToolSpec{.name = "fetch_observation",
                         .desc = "获取观测（日志尾部 / 进程状态 / 帧统计）",
                         .risk = tool::Risk::ReadOnly,
                         .params = {{.name = "source",
                                     .type = "string",
                                     .desc = "来源",
                                     .required = true,
                                     .enum_values = {"logs", "procs", "frames"}},
                                    {.name = "tail",
                                     .type = "integer",
                                     .desc = "日志行数",
                                     .required = false,
                                     .enum_values = {},
                                     .has_range = true,
                                     .min = 1,
                                     .max = 200}}},
          [&pipe, trace](const json& a) {
            std::string err;
            const std::string src = a["source"].get<std::string>();
            const int tail = a.contains("tail") ? a["tail"].get<int>() : 20;
            const std::string obs = pipe.observation(src, tail, err);
            if (!err.empty()) return record(trace, "fetch_observation", tool::ToolResult{false, err, json::object()});
            return record(trace, "fetch_observation", tool::ToolResult{true, obs, json{{"source", src}}});
          });
}

}  // namespace cue::pipeline
