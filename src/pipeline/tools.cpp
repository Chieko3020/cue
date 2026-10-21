#include "cue/pipeline/tools.h"

#include <utility>

#include "cue/common/clock.h"

namespace cue::pipeline {

using json = nlohmann::json;

// data 只放"我看到了什么"（真实测量值），不放"我做了什么"（请求参数）。
// 否则 agent 可以引用自己的动作当观测证据（review-eval.md #26）。
// age_ms / samples 让 agent 能判断这条是不是 cued 重启前的旧指标（#27）。
json metrics_json(const PipelineMetrics& m, int64_t now) {
  const int64_t age_us = m.updated_us == 0 ? -1 : now - m.updated_us;
  // 分位没有样本时输出 null 而不是 0：agent 引用 0 当证据就是伪证
  // （transcode 关闭时 encode 段本来就没有样本，写 0 等于宣称"编码 0 ms"）。
  auto pct = [](double v) { return has_sample(v) ? json(v) : json(nullptr); };
  return json{{"fps", m.fps},
              {"frames_in", m.frames_in},
              {"frames_out", m.frames_out},
              {"dropped", m.dropped},
              {"injected", m.injected},
              {"decoded", m.decoded},
              {"decode_errors", m.decode_errors},
              {"recv_p50_ms", pct(m.recv_p50)},
              {"buffer_p50_ms", pct(m.buffer_p50)},
              {"decode_p50_ms", pct(m.decode_p50)},
              {"decode_p99_ms", pct(m.decode_p99)},
              {"op_p50_ms", pct(m.op_p50)},
              {"op_p99_ms", pct(m.op_p99)},
              {"encode_p50_ms", pct(m.encode_p50)},
              {"encode_p99_ms", pct(m.encode_p99)},
              {"send_p50_ms", pct(m.send_p50)},
              // 进程身份/存活：pid=null 表示当前没有 cued（不是 pid 0 的真进程）
              {"pid", m.pid == 0 ? json(nullptr) : json(static_cast<int64_t>(m.pid))},
              {"alive", m.alive},
              // 协议/发送/编码/预算侧计数：过去只在 cued 的 METRICS 行里，agent 看不到
              {"rejected", m.rejected},
              {"send_errors", m.send_errors},
              {"encode_errors", m.encode_errors},
              {"decode_drops", m.decode_drops},
              {"budget_drops", m.budget_drops},
              {"samples", m.samples},
              {"age_ms", age_us < 0 ? -1 : age_us / 1000}};
}

namespace {

// 统一的留痕包装：工具 handler 返回前把结果记进 trace（失败也要记，证据校验只认真实观测）
tool::ToolResult record(std::vector<ToolTrace>* trace, const char* name, tool::ToolResult r) {
  if (trace) trace->push_back(ToolTrace{name, r.content, r.data});
  return r;
}

// 动作类工具的返回：动作写进 text，data 只带动作之后重新测到的指标
tool::ToolResult action_result(std::vector<ToolTrace>* trace, const char* name, const std::string& text,
                               IServicePipeline& pipe) {
  const auto m = pipe.latest();
  if (m.samples == 0) {
    return record(trace, name,
                  tool::ToolResult{true, text + "（暂无媒体流采样）", json{{"samples", 0}}});
  }
  auto j = metrics_json(m, now_us());
  return record(trace, name, tool::ToolResult{true, text + " 最新观测: " + j.dump(), j});
}

}  // namespace

const char* real_ops_system_prompt() {
  return "你是 Cue 的媒体管线运维 agent。你只能通过工具观测与调整管线。"
         "每轮只能调用工具；结论必须引用真实观测作为证据；每个阶段结束调用 stage_complete。"
         "根因必须使用枚举值：network_loss / network_delay / bandwidth_insufficient / "
         "encoder_too_slow / resolution_too_high / queue_backlog / keyframe_storm / clock_drift。"
         "若要取消已有的丢包注入，用 inject_fault 且 value=0。"
         "diagnose 阶段提交 evidence 时，每条只从某次工具返回里逐字摘抄 key=value 片段"
         "（例如 injected=59、fps=30.3、decode_p99_ms=3.199），最多 4 条；"
         "严格禁止解释性文字、中文句子、根因说明或把多次观测合并成一条——"
         "判分会逐条校验：只要出现引用之外的文字，该条证据即判伪。";
}

void build_real_tools(tool::Registry& reg, IServicePipeline& pipe, std::vector<ToolTrace>* trace) {
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
            const json j = metrics_json(m, now_us());
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
            return action_result(trace, "pipeline_set_param",
                                 "已设置 " + k + "=" + v + "（管线已重启）", pipe);
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
            return action_result(trace, "inject_fault", "已注入 " + k + "=" + std::to_string(v) + "%",
                                 pipe);
          });

  reg.add(tool::ToolSpec{.name = "fetch_observation",
                         .desc = "获取观测（日志尾部 / 进程状态 / 帧统计）",
                         .risk = tool::Risk::ReadOnly,
                         .params = {{.name = "source",
                                     .type = "string",
                                     .desc = "来源",
                                     .required = true,
                                     .enum_values = {"logs", "procs", "frames", "events"}},
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
            // data 只带真实读数；source 是请求参数，放 text 里就够
            return record(trace, "fetch_observation",
                          tool::ToolResult{true, obs, json::object()});
          });
}

}  // namespace cue::pipeline
