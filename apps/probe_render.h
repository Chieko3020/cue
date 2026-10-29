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
#include <vector>

namespace cue::probe {

struct RenderConfig {
  int width = 0;   // 0 = 用第一帧的实际尺寸
  int height = 0;
  int audio_rate = 48000;
  int audio_channels = 1;
  // 播放输出设备名（子串匹配，忽略大小写）。空 = 用系统默认设备。
  //
  // 为什么一定要能指定：诊断为的是"听清楚链路送来的声音"，而默认输出设备在
  // 虚拟声卡环境里往往是**被采集的那一路**（例如 VoiceMeeter 的 Input VAIO）。
  // 那样 probe 的输出会立刻被采集端再收一遍，形成回环/回音，
  // 听感上会让人误判成"链路有回声"。
  std::string audio_out;
  // 播放前的预缓冲（毫秒）。0 = 立即开始播（旧行为）。
  //
  // 为什么需要它：音频在发送侧已经定拍（20 ms 一包匀速发出），但公网上仍会有几十毫秒
  // 的到达抖动。若设备一开始就播，队列长期在 0 附近，任何一次抖动都会让设备**饿着**，
  // 听感就是断音/卡顿。先攒够一小段再开始播，等于给播放侧一个固定余量。
  //
  // 这个值和"延迟"直接冲突：预缓冲越大越不易断，但音画同步的补偿量要跟着变。
  // 所以默认 0，由调用方按链路质量显式给。合法范围 0..2000ms。
  // 预缓冲阶段不应用250ms播放软限；reset/硬清后按同一目标重新启动。
  int audio_prebuffer_ms = 0;
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

  // 当前播放队列深度（毫秒）。设备没打开返回 -1。
  // 没有这个观测就只能靠"听起来有没有咔咔"来猜，而咔咔的原因很可能在自己这边
  // （实测过：队列超上限就把新包丢掉，等于自己制造了丢包）。
  int queued_ms() const;

  // 清空播放队列。流换代/重连时必须调用：否则上一轮已解码的 PCM 会继续播完，
  // 听起来就是"重启后重复播放的还是上一轮的旧数据"。
  void reset_audio();

  // 播放队列的累计账。只有把它放进指标行，"是链路丢了还是播放侧自己丢了/自己积压了"
  // 这个问题才有证据可查——只听声音只能判断"有咔咔"，判断不了责任在哪一侧。
  struct AudioStats {
    uint64_t over_soft = 0;    // 队列越过软上限的次数
    // 因时钟漂移/成簇到达而丢掉的 20 ms 包数。**这是听感变差最直接的嫌疑人**：
    // 丢得越多，声音越断续；不暴露它就只能靠耳朵猜。
    uint64_t dropped_drift = 0;
    // 已开始播放但队列被抽干的次数：设备饿着的直接证据，对应听感上的断音/卡顿。
    uint64_t starved = 0;
    uint64_t trimmed = 0;      // 越过硬上限、被迫清空的次数（听感上是一次跳变）
    uint64_t queued = 0;       // 成功入队的 PCM 段数
    uint64_t push_errors = 0;  // SDL_QueueAudio 失败次数（设备被拔/后端异常）
  };
  AudioStats audio_stats() const;

  // 周期性把**渲染出来的画面**存成 BMP（0 = 关闭）。
  //
  // 为什么不在外面截桌面：桌面截屏受会话/窗口焦点/DPI 影响，从 SSH 起还要绕
  // schtasks /it 进会话 1，实测在这台机器上任务会在跑但脚本不执行（连重定向日志
  // 都不生成），排查成本远高于收益。而"把渲染结果存盘"只需要 SDL_RenderReadPixels，
  // 抓到的就是实际显示的那一帧，并且窗口被挡住、最小化都不影响。
  // 用 BMP 而不是 PNG：SDL 内置 SaveBMP，不引入 libpng 依赖；需要 PNG 时本地转一次即可。
  void enable_snapshot(int every_ms, std::string prefix);

  // 处理退出事件；返回 false 表示用户关了窗口（调用方应退出主循环）。
  // 注意：渲染器**没打开**时这个函数恒返回 false（"没有窗口"与"窗口被关"在返回值上
  // 无法区分）。所以调用方必须先记住 open() 成功与否，再决定要不要拿它当退出信号——
  // 否则 --render 在无 DISPLAY/无 GPU 的环境打不开一次，probe 就会立刻退出，
  // 而 open() 的约定明明是"失败就降级成只算指标"。
  bool pump_events();

  // 列出可用的**播放**设备名（用于让用户挑 --audio-out 的字符串）。
  // 没有 SDL2 时返回空表。
  static std::vector<std::string> audio_outputs();

  // 这个构建是否带 SDL2 支持。
  static bool available();
  // 人读的后端描述（"sdl2" 或 "none"）。
  static const char* backend_name();

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace cue::probe
