// 管线后端工厂：把「端点描述」落成具体实现。
// 这里是唯一允许 new 具体后端的地方——工具层/评测层只拿 IServicePipeline。
#include "cue/common/service_pipeline.h"

#include <memory>
#include <string>

#include "cue/pipeline/process_control.h"
#include "cue/pipeline/http_pipeline.h"

namespace cue::pipeline {

nlohmann::json empty_session_table() {
  return nlohmann::json{{"sessions", nlohmann::json::array()}, {"count", 0}, {"live", 0}};
}

nlohmann::json no_session_table(const char* backend, std::string& err) {
  // 文案写「没有会话表」而不是「没有设备」：前者是能力缺失，后者是「事实读作空」。
  // 前端与工具层只有靠这句话才能把「设备都掉线了」与「后端不支持设备表」分开。
  err = std::string(backend) +
        " 后端没有会话表：设备注册、能力、租约只存在于 cued 的 SessionRegistry，"
        "该后端既不 spawn cued，也不维护多设备会话";
  return empty_session_table();
}

nlohmann::json IServicePipeline::sessions_json(std::string& err) const {
  // 默认实现同样只表达「我不知道」：空表 + 原因，不假装查过。
  err = "该管线后端没有实现会话表（没有设备/租约概念）";
  return empty_session_table();
}

std::unique_ptr<IServicePipeline> make_pipeline(const PipelineEndpoint& ep, std::string& err) {
  switch (ep.kind) {
    case PipelineEndpoint::Kind::Process:
      // kind 在这里被显式忽略：分支本身已经决定了后端类型（ProcessPipelineControl 的
      // 构造函数只读 cued 启动参数，不看 kind）。不加运行时校验是刻意的——那是行为变更，
      // 会让"Http 端点却交给 Process 后端"从"能跑但指错了"变成启动失败。
      return std::make_unique<ProcessPipelineControl>(ep);
    case PipelineEndpoint::Kind::Http:
      return std::make_unique<HttpServicePipeline>(ep);
  }
  err = "未知的管线端点类型（PipelineEndpoint::Kind=" +
        std::to_string(static_cast<int>(ep.kind)) + "）";
  return nullptr;
}

}  // namespace cue::pipeline
