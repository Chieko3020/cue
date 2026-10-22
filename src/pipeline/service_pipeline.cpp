// 管线后端工厂：把「端点描述」落成具体实现。
// 这里是唯一允许 new 具体后端的地方——工具层/评测层只拿 IServicePipeline。
#include "cue/common/service_pipeline.h"

#include <memory>
#include <string>

#include "cue/pipeline/process_control.h"
#include "cue/pipeline/http_pipeline.h"

namespace cue::pipeline {

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
