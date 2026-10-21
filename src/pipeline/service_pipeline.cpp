// 管线后端工厂：把「端点描述」落成具体实现。
// 这里是唯一允许 new 具体后端的地方——工具层/评测层只拿 IServicePipeline。
#include "cue/pipeline/service_pipeline.h"

#include <memory>
#include <string>

#include "cue/pipeline/process_control.h"

namespace cue::pipeline {

std::unique_ptr<IServicePipeline> make_pipeline(const PipelineEndpoint& ep, std::string& err) {
  switch (ep.kind) {
    case PipelineEndpoint::Kind::Process:
      return std::make_unique<ProcessPipelineControl>(ep);
    case PipelineEndpoint::Kind::Http:
      // 明确失败而不是返回空实现：调用方拿到 nullptr 必须查 err，不会把「什么也没做」
      // 当成一条正常管线（HTTP 后端落地于 M7 后续）。
      err = "Http 管线后端尚未实现（PipelineEndpoint::Kind::Http；http_base=" + ep.http_base + "）";
      return nullptr;
  }
  err = "未知的管线端点类型（PipelineEndpoint::Kind=" +
        std::to_string(static_cast<int>(ep.kind)) + "）";
  return nullptr;
}

}  // namespace cue::pipeline
