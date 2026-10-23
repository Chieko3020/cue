// cue-probe 的渲染/播放（"接收端能看能听"）。
//
// 为什么用 SDL2 而不是各平台原生 API：视频要窗口 + 纹理，音频要一个 PCM 队列，
// SDL2 一套接口同时给两者（SDL_UpdateTexture / SDL_QueueAudio），Windows 与 Linux
// 同一份代码。原生方案（Windows 的 D3D + WASAPI、Linux 的 X11 + ALSA）要写两份
// 且各自几百行——而这个组件的作用只是"证明链路送来的画面和声音是通的"，
// 不值得为它写两套平台代码。
//
// SDL2 是**可选依赖**：找不到时不编译这块，--render 会在运行期明确报错，
// 而不是让无头环境（CI、服务器）因为缺一个图形库就编不过。
#pragma once

#include <cstdint>
#include <string>

namespace cue::probe {

struct RenderConfig {
  int width = 0;   // 0 = 用第一帧的实际尺寸
  int height = 0;
  int audio_rate = 48000;
  int audio_channels = 1;
};

// 只有声明；实现在 probe_render.cpp（SDL2 不可用时是空实现）。
class Renderer {
 public:
  Renderer();
  ~Renderer();
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  // 打开窗口与音频设备。失败返回 false 并写 err（调用方据此降级为"只算指标"）。
  bool open(const RenderConfig& cfg, std::string& err);

  // 送一帧 YUV（AVFrame*，可以是任意 pixel format）；内部 sws_scale 到 RGB 后上屏。
  // frame 为 nullptr 时只 pump 事件（保证窗口不假死）。
  void draw_yuv(const void* av_frame);

  // 送一段 PCM（int16 交错）。音频设备打开失败时静默忽略（不影响视频）。
  void queue_audio(const int16_t* pcm, int samples);

  // 处理退出事件；返回 false 表示用户关了窗口（调用方应退出主循环）。
  bool pump_events();

  // 这个构建是否带 SDL2 支持。
  static bool available();
  // 人读的后端描述（"sdl2" 或 "none"）。
  static const char* backend_name();

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace cue::probe
