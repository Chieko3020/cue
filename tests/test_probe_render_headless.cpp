// 实际编译 probe_render.cpp 的无 SDL 分支，验证公开降级契约。
#include <cstdio>
#include <string>

#include "probe_render.h"

namespace {
int failures = 0;
void check(bool condition, const char* message) {
  if (!condition) {
    std::printf("FAIL: %s\n", message);
    ++failures;
  }
}
}  // namespace

int main() {
  using cue::probe::Renderer;
  check(!Renderer::available(), "无 SDL 构建不可渲染");
  check(std::string(Renderer::backend_name()) == "none", "后端明确为 none");
  check(Renderer::audio_outputs().empty(), "无 SDL 构建没有播放设备");

  Renderer renderer;
  std::string error;
  check(!renderer.open({}, error), "打开无 SDL 渲染器失败");
  check(error.find("SDL2") != std::string::npos && error.find("--render") != std::string::npos,
        "打开失败明确说明 SDL2 和 --render 不可用");
  // 调用方降级后仍可安全使用接口；无设备的队列不能伪报为 0 ms。
  renderer.draw_yuv(nullptr);
  renderer.queue_audio(nullptr, 0);
  renderer.enable_snapshot(100, "unused");
  renderer.reset_audio();
  check(renderer.queued_ms() == -1, "无设备队列返回不可用值");
  check(!renderer.pump_events(), "无窗口时没有事件循环");
  const auto stats = renderer.audio_stats();
  check(stats.over_soft == 0 && stats.dropped_drift == 0 && stats.starved == 0 &&
            stats.trimmed == 0 && stats.queued == 0 && stats.push_errors == 0,
        "无设备时播放统计为空");
  return failures == 0 ? 0 : 1;
}
