// SDL2 渲染/播放实现。SDL2 不可用时整份是空实现（见文件末尾的 #else 分支）。
#include "probe_render.h"
#include "audio_playback.h"

#if defined(CUE_HAVE_SDL2) && CUE_HAVE_SDL2

#include <SDL2/SDL.h>
#include "audio_drift.h"
#include <algorithm>
#include <deque>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

namespace cue::probe {

struct Renderer::Impl : AudioQueueDevice {
  SDL_Window* win = nullptr;
  SDL_Renderer* ren = nullptr;
  SDL_Texture* tex = nullptr;
  SDL_AudioDeviceID adev = 0;
  SwsContext* sws = nullptr;
  std::vector<uint8_t> rgb;  // 复用缓冲，避免每帧 malloc
  int w = 0;
  int h = 0;
  std::size_t rgb_size = 0;
  // sws 上下文在创建时就把源像素格式绑死了，所以它是重建判据的一部分，不是装饰。
  int sws_src_fmt = -1;
  // 音频设备的**实际**参数（取自 SDL_OpenAudioDevice 的 have）。队列字节数要靠它
  // 换算成毫秒；拿写死的 48 kHz 去算，设备一旦协商成别的值，读数与实际播放速度
  // 一起错，而且错得很像"链路延迟抖动"。
  int audio_hz = 0;
  int audio_ch = 0;
  // 窗口尺寸只在第一帧决定一次：之后用户拖过窗口，再被流的分辨率变化抢回去会很难用。
  bool window_sized = false;
  // 是否"按第一帧实际尺寸开窗"。--render-size 显式给了就不该被覆盖：那个参数的
  // 用途正是"我就要这么大的窗口"（RenderConfig 里 0 才表示"用第一帧的"）。
  bool window_auto = true;
  AudioPlaybackQueue playback{*this};
  AudioDrift drift;
  std::vector<int16_t> converted;
  std::deque<int> queue_depths;
  uint64_t queue_samples = 0;
  Uint64 last_sample_ms = 0, last_quantile_ms = 0;
  int queue_p50_ms = -1, queue_p95_ms = -1, queue_p99_ms = -1;
  int snap_every_ms = 0;
  std::string snap_prefix;
  uint64_t snap_count = 0;
  Uint64 snap_last_ms = 0;
  // 每类失败只往 stderr 报一次：渲染路径的失败是每帧都发生的，不限流会把
  // "真正的第一句原因"冲掉，日志上看起来像刷屏噪声。
  bool warned_draw = false;
  bool warned_audio = false;

  void warn_draw(const char* what) {
    if (warned_draw) return;
    warned_draw = true;
    const char* se = SDL_GetError();
    const bool has_err = se != nullptr && *se != '\0';
    std::fprintf(stderr, "[probe] 渲染失败（只报一次）：%s%s%s\n", what, has_err ? "：" : "",
                 has_err ? se : "");
  }

  bool available() const override { return adev != 0; }
  int queued_ms() const override {
    if (!available() || audio_hz <= 0 || audio_ch <= 0) return -1;
    const double bytes_per_sec = double(audio_hz) * audio_ch * sizeof(int16_t);
    return static_cast<int>(double(SDL_GetQueuedAudioSize(adev)) * 1000.0 / bytes_per_sec);
  }
  bool empty() const override { return adev != 0 && SDL_GetQueuedAudioSize(adev) == 0; }
  void reset_processing() override { drift.reset(); }
  void pause(bool paused) override { SDL_PauseAudioDevice(adev, paused ? 1 : 0); }
  void clear() override { SDL_ClearQueuedAudio(adev); }
  AudioQueueResult queue(const int16_t* pcm, uint32_t bytes) override {
    drift.update(queued_ms(), static_cast<int64_t>(SDL_GetTicks64()) * 1000, playback.started());
    if (!drift.convert(pcm, static_cast<int>(bytes / sizeof(int16_t)), converted)) {
      if (!warned_audio) {
        warned_audio = true;
        std::fprintf(stderr, "[probe] playback resampler failed: %s\n", drift.last_error().c_str());
      }
      return AudioQueueResult::Failed;
    }
    if (converted.empty()) return AudioQueueResult::Buffered;
    if (SDL_QueueAudio(adev, converted.data(), static_cast<Uint32>(converted.size() * sizeof(int16_t))) == 0) return AudioQueueResult::Queued;
    if (!warned_audio) {
      warned_audio = true;
      std::fprintf(stderr, "[probe] SDL_QueueAudio 失败（只报一次）：%s\n", SDL_GetError());
    }
    return AudioQueueResult::Failed;
  }

  ~Impl() {
    if (sws) sws_freeContext(sws);
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    if (adev) SDL_CloseAudioDevice(adev);
    SDL_Quit();
  }
};

Renderer::Renderer() = default;
Renderer::~Renderer() { delete impl_; }

std::vector<std::string> Renderer::audio_outputs() {
  std::vector<std::string> out;
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return out;
  const int n = SDL_GetNumAudioDevices(0);
  for (int i = 0; i < n; ++i) {
    const char* nm = SDL_GetAudioDeviceName(i, 0);
    if (nm != nullptr) out.emplace_back(nm);
  }
  return out;
}

bool Renderer::available() { return true; }
const char* Renderer::backend_name() { return "sdl2"; }

bool Renderer::open(const RenderConfig& cfg, std::string& err) {
  if (!AudioPlaybackQueue::valid_prebuffer(cfg.audio_prebuffer_ms)) {
    err = "audio_prebuffer_ms must be between 0 and 2000";
    return false;
  }
  if (impl_ != nullptr) return true;
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
    err = std::string("SDL_Init: ") + SDL_GetError();
    return false;
  }
  auto* im = new Impl();
  im->w = cfg.width > 0 ? cfg.width : 640;
  im->h = cfg.height > 0 ? cfg.height : 360;
  im->window_auto = !(cfg.width > 0 && cfg.height > 0);
  im->win = SDL_CreateWindow("cue-probe", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             im->w, im->h, SDL_WINDOW_RESIZABLE);
  if (im->win == nullptr) {
    err = std::string("SDL_CreateWindow: ") + SDL_GetError();
    delete im;
    return false;
  }
  // 刻意**不要** SDL_RENDERER_PRESENTVSYNC：带 vsync 时 SDL_RenderPresent 会阻塞到下一个
  // 垂直同步，最长一个刷新周期（60 Hz 就是 16 ms）。这 16 ms 是在收包主循环里打盹——
  // 而音频是"到达即入队"的实时队列（没有预缓冲，深度常年只有 0~20 ms 的量级），
  // 主循环停 16 ms 就等于让设备把队列耗空一次，听感上正是"咔"。
  // 检测工具要的是"不干扰被测对象"，画面轻微撕裂比篡改测量结果划算得多。
  // （这不是"渲染与接收应该解耦"的替代方案——那是更大的改动，见本次审查的结论。）
  im->ren = SDL_CreateRenderer(im->win, -1, SDL_RENDERER_ACCELERATED);
  if (im->ren == nullptr) {
    // 无 GPU/远程桌面时退回软件渲染：只是"看得到"，不追求性能。
    im->ren = SDL_CreateRenderer(im->win, -1, SDL_RENDERER_SOFTWARE);
  }
  if (im->ren == nullptr) {
    err = std::string("SDL_CreateRenderer: ") + SDL_GetError();
    delete im;
    return false;
  }
  // 音频：设备打不开不算致命（没有声卡的环境很常见），视频照常。
  SDL_AudioSpec want{};
  want.freq = cfg.audio_rate;
  want.format = AUDIO_S16SYS;
  want.channels = static_cast<Uint8>(cfg.audio_channels > 0 ? cfg.audio_channels : 1);
  want.samples = 960;  // 20 ms @48k，与 Opus 帧对齐
  want.callback = nullptr;  // 用 SDL_QueueAudio，不要回调
  SDL_AudioSpec have{};
  // 选输出设备：cfg.audio_out 为空才用系统默认。
  //
  // 为什么不做"永远用默认"：在虚拟声卡环境里默认输出设备常常就是**被采集的那一路**
  // （VoiceMeeter 的 Input VAIO 之类），于是 probe 播出来的声音立刻又被采集端收进去，
  // 形成回环/回音——听起来像链路有回声，实际是本地自己的环。必须能显式指定。
  std::string chosen;
  const char* open_name = nullptr;
  if (!cfg.audio_out.empty()) {
    const int n = SDL_GetNumAudioDevices(0);  // 0 = 播放设备
    std::string want_lower = cfg.audio_out;
    for (auto& c : want_lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    int hit = -1;
    for (int i = 0; i < n; ++i) {
      const char* nm = SDL_GetAudioDeviceName(i, 0);
      if (nm == nullptr) continue;
      std::string low(nm);
      for (auto& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (low.find(want_lower) != std::string::npos) { hit = i; break; }
    }
    if (hit < 0) {
      std::fprintf(stderr, "[probe] 找不到输出设备 \"%s\"，退回系统默认。可用设备见 --list-audio\n",
                   cfg.audio_out.c_str());
    } else {
      open_name = SDL_GetAudioDeviceName(hit, 0);
      chosen = open_name ? open_name : "";
    }
  }
  im->adev = SDL_OpenAudioDevice(open_name, 0, &want, &have, 0);
  if (im->adev != 0) {
    // have 必须**真的**等于 want，不能只是"没报错"。
    //
    // 队列里放的是 48 kHz 单声道 s16 的裸 PCM，而这个数同时决定 queued_ms() 的换算：
    // 设备把 48000 协商成 44100（或 s16 协商成 float32）时，SDL 自己会插一层转换，
    // 播放**速度**不一定错，但队列深度的毫秒数一定按错误的比例算，于是"队列稳定在
    // 40 ms"这类读数全是假的；反过来若后端直接以别的格式吃数据，那就是真的播错速度
    // （听起来像慢放/快进），而症状完全不会指向"设备协商"。
    // 这个组件的作用是证明链路通不通，不是做设备兼容层：宁可明确报错、只留视频，
    // 也不能静默播一个听不懂的声音、报一个错的延迟。
    const bool spec_ok = have.freq == want.freq && have.channels == want.channels &&
                         have.format == want.format;
    if (!spec_ok) {
      std::fprintf(stderr,
                   "[probe] 音频设备协商失败（只渲染视频，避免播错速度/算错队列深度）："
                   "要 %d Hz/%d ch/fmt=0x%x，设备给 %d Hz/%d ch/fmt=0x%x\n",
                   static_cast<int>(want.freq), static_cast<int>(want.channels),
                   static_cast<unsigned>(want.format), static_cast<int>(have.freq),
                   static_cast<int>(have.channels), static_cast<unsigned>(have.format));
      SDL_CloseAudioDevice(im->adev);
      im->adev = 0;
    } else {
      im->audio_hz = static_cast<int>(have.freq);
      im->audio_ch = static_cast<int>(have.channels);
      std::fprintf(stderr, "[probe] 音频输出设备：%s%s（%d Hz / %d ch / s16）\n",
                   chosen.empty() ? "(系统默认)" : chosen.c_str(),
                   chosen.empty() ? "" : " ", im->audio_hz, im->audio_ch);
      std::fprintf(stderr, "[probe] audio buffer=%u samples, nominal_period_ms=%.6f\n",
                   static_cast<unsigned>(have.samples), 1000.0 * have.samples / have.freq);
      if (!im->drift.configure(im->audio_hz, im->audio_ch, cfg.audio_prebuffer_ms,
                               cfg.audio_drift_compensation)) {
        std::fprintf(stderr, "[probe] playback resampler initialization failed; audio disabled\n");
        SDL_CloseAudioDevice(im->adev);
        im->adev = 0;
      }
      im->playback.configure(cfg.audio_prebuffer_ms);
      if (cfg.audio_prebuffer_ms > 0) {
        std::fprintf(stderr, "[probe] 音频预缓冲 %d ms（攒够再开始播放）\n",
                     cfg.audio_prebuffer_ms);
      }
    }
  } else {
    std::fprintf(stderr, "[probe] 音频设备打不开（只渲染视频）：%s\n", SDL_GetError());
  }
  impl_ = im;
  return true;
}

void Renderer::draw_yuv(const void* av_frame) {
  if (impl_ == nullptr) return;
  Impl& im = *impl_;
  if (av_frame == nullptr) {
    SDL_RenderPresent(im.ren);
    return;
  }
  const auto* f = static_cast<const AVFrame*>(av_frame);
  if (f->width <= 0 || f->height <= 0) return;
  const int src_fmt = f->format;
  // 重建条件里三个"看不见的"项各自对应一种只会表现成黑屏的故障：
  //   · sws / tex 为空：上一次创建失败。不重试就永久黑屏，而尺寸看起来一切正常。
  //   · sws_src_fmt 变了：sws 上下文绑死了源格式，上游换 pix_fmt（yuv420p → nv12、
  //     软解换硬解）后 sws_scale 直接失败。只比宽高的话这个分支永远不会进。
  //   · 宽高变了：纹理、行跨度、rgb 缓冲都得跟着换。
  if (im.sws == nullptr || im.tex == nullptr || im.w != f->width || im.h != f->height ||
      im.sws_src_fmt != src_fmt) {
    // 是否要连纹理一起重做，只看尺寸：换格式不影响 RGB24 的纹理，白重建一次反而
    // 多丢一帧（纹理销毁到重建之间那次 present 是黑的）。
    const bool reseed = im.tex == nullptr || im.w != f->width || im.h != f->height;
    if (im.sws != nullptr && (reseed || im.sws_src_fmt != src_fmt)) {
      sws_freeContext(im.sws);
      im.sws = nullptr;
    }
    if (im.sws == nullptr) {
      im.sws = sws_getContext(f->width, f->height, static_cast<AVPixelFormat>(src_fmt), f->width,
                              f->height, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr,
                              nullptr);
      if (im.sws == nullptr) {
        im.warn_draw("sws_getContext（尺寸/格式组合建不出缩放上下文）");
        return;  // 不更新 im.w/im.h：下一个重建判据还得靠它们
      }
      im.sws_src_fmt = src_fmt;
    }
    if (reseed) {
      im.w = f->width;
      im.h = f->height;
      im.rgb_size = static_cast<std::size_t>(f->width) * static_cast<std::size_t>(f->height) * 3;
      im.rgb.assign(im.rgb_size, 0);
      if (im.tex != nullptr) {
        SDL_DestroyTexture(im.tex);
        im.tex = nullptr;
      }
      im.tex = SDL_CreateTexture(im.ren, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, im.w,
                                 im.h);
      if (im.tex == nullptr) {
        im.warn_draw("SDL_CreateTexture");
        return;
      }
      // 逻辑尺寸 = 视频尺寸：RenderCopy 的目标矩形传 nullptr 时，SDL 会把画面铺满
      // 整个窗口。窗口比例和视频比例不一致时（16:9 的画面塞进用户拖出来的方窗口，
      // 或者 --render-size 与流尺寸不同）画面就被**拉伸**——看起来像编码器把画质
      // 弄坏了，实际只是没做等比。设了逻辑尺寸，SDL 自己按比例缩放并加黑边，
      // 窗口 resize 也就不需要这里再算任何东西（tex_w/tex_h 这套手动换算因此去掉）。
      if (SDL_RenderSetLogicalSize(im.ren, im.w, im.h) != 0) {
        im.warn_draw("SDL_RenderSetLogicalSize");
      }
      // 只在第一帧、且用户没有显式指定 --render-size 时按视频尺寸开窗。
      // 之后分辨率再变只改纹理与逻辑尺寸：用户拖出来的窗口不该被抢回去，
      // 等比缩放到窗口是 SDL 按逻辑尺寸做的，不需要这里参与。
      if (!im.window_sized) {
        im.window_sized = true;
        if (im.window_auto) SDL_SetWindowSize(im.win, im.w, im.h);
      }
    }
  }
  if (im.sws == nullptr || im.tex == nullptr) return;
  uint8_t* dst[4] = {im.rgb.data(), nullptr, nullptr, nullptr};
  int dst_stride[4] = {im.w * 3, 0, 0, 0};
  if (sws_scale(im.sws, f->data, f->linesize, 0, f->height, dst, dst_stride) < 0) {
    im.warn_draw("sws_scale");
    return;
  }
  // pitch 就是 w*3：rgb 是紧排连续缓冲（assign 出来的），没有行末填充，
  // 所以它既是行跨度也是总长度/高。
  if (SDL_UpdateTexture(im.tex, nullptr, im.rgb.data(), im.w * 3) != 0) {
    im.warn_draw("SDL_UpdateTexture");
    return;
  }
  SDL_RenderClear(im.ren);
  if (SDL_RenderCopy(im.ren, im.tex, nullptr, nullptr) != 0) {
    im.warn_draw("SDL_RenderCopy");
    return;
  }
  SDL_RenderPresent(im.ren);

  // 周期性把渲染结果存盘。放在 Present 之后读，读到的是**刚显示的那一帧**。
  if (im.snap_every_ms > 0 && !im.snap_prefix.empty()) {
    const Uint64 now_ms = SDL_GetTicks64();
    if (im.snap_last_ms == 0 || now_ms - im.snap_last_ms >= static_cast<Uint64>(im.snap_every_ms)) {
      im.snap_last_ms = now_ms;
      // 读窗口而不是纹理：ReadPixels 拿到的是渲染目标的最终像素（含 LogicalSize 的
      // 等比缩放与黑边），与用户眼睛看到的一致。
      int w = 0;
      int h = 0;
      SDL_GetRendererOutputSize(im.ren, &w, &h);
      if (w > 0 && h > 0) {
        SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
        if (surf != nullptr) {
          if (SDL_RenderReadPixels(im.ren, nullptr, SDL_PIXELFORMAT_ARGB8888, surf->pixels,
                                   surf->pitch) == 0) {
            char path[512];
            std::snprintf(path, sizeof(path), "%s%05llu.bmp", im.snap_prefix.c_str(),
                          static_cast<unsigned long long>(im.snap_count));
            if (SDL_SaveBMP(surf, path) == 0) ++im.snap_count;
          }
          SDL_FreeSurface(surf);
        }
      }
    }
  }
}

void Renderer::queue_audio(const int16_t* pcm, int samples) {
  if (impl_) impl_->playback.push(pcm, samples);
}

int Renderer::queued_ms() const {
  return impl_ ? impl_->queued_ms() : -1;
}

int Renderer::queued_samples() const {
  return impl_ && impl_->available() ? static_cast<int>(SDL_GetQueuedAudioSize(impl_->adev) / sizeof(int16_t)) : -1;
}

void Renderer::sample_audio() {
  if (!impl_ || !impl_->available()) return;
  impl_->playback.observe();
  const auto now = SDL_GetTicks64();
  if (impl_->last_sample_ms != 0 && now - impl_->last_sample_ms < 10) return;
  impl_->last_sample_ms = now;
  ++impl_->queue_samples;
  impl_->queue_depths.push_back(impl_->queued_ms());
  if (impl_->queue_depths.size() > 6000) impl_->queue_depths.pop_front();
  if (impl_->last_quantile_ms == 0 || now - impl_->last_quantile_ms >= 1000) {
    impl_->last_quantile_ms = now;
    std::vector<int> depths(impl_->queue_depths.begin(), impl_->queue_depths.end());
    std::sort(depths.begin(), depths.end());
    auto pct = [&](double p) { return depths[static_cast<std::size_t>(p * double(depths.size()-1))]; };
    impl_->queue_p50_ms = pct(.5); impl_->queue_p95_ms = pct(.95); impl_->queue_p99_ms = pct(.99);
  }

}

void Renderer::reset_audio() {
  if (impl_) impl_->playback.reset();
}

Renderer::AudioStats Renderer::audio_stats() const {
  if (!impl_) return {};
  const auto& stats = impl_->playback.stats();
  AudioStats out;
  out.over_soft = stats.over_soft; out.dropped_drift = stats.dropped_drift;
  out.starved = stats.starved; out.trimmed = stats.trimmed;
  out.queued = stats.queued; out.push_errors = stats.push_errors;
  out.rebuffered = stats.rebuffered;
  const auto& ds = impl_->drift.stats();
  out.compensation_updates = ds.updates; out.compensation_errors = ds.errors;
  out.compensation_ppm = ds.ppm;
  out.input_samples = ds.input_samples; out.output_samples = ds.output_samples;
  out.queue_samples = impl_->queue_samples;
  out.queue_p50_ms = impl_->queue_p50_ms;
  out.queue_p95_ms = impl_->queue_p95_ms;
  out.queue_p99_ms = impl_->queue_p99_ms;
  return out;
}

void Renderer::enable_snapshot(int every_ms, std::string prefix) {
  if (impl_ == nullptr) return;
  impl_->snap_every_ms = every_ms > 0 ? every_ms : 0;
  impl_->snap_prefix = std::move(prefix);
  impl_->snap_last_ms = 0;
  impl_->snap_count = 0;
}

bool Renderer::pump_events() {
  if (impl_ == nullptr) return false;
  SDL_Event e;
  while (SDL_PollEvent(&e) != 0) {
    if (e.type == SDL_QUIT) return false;
    if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) return false;
  }
  return true;
}

}  // namespace cue::probe

#else  // 没有 SDL2

namespace cue::probe {

struct Renderer::Impl {};

Renderer::Renderer() = default;
Renderer::~Renderer() = default;
bool Renderer::available() { return false; }
const char* Renderer::backend_name() { return "none"; }
std::vector<std::string> Renderer::audio_outputs() { return {}; }
bool Renderer::open(const RenderConfig&, std::string& err) {
  err = "本构建没有 SDL2 支持（配置时禁用或未找到 sdl2）：--render 不可用";
  return false;
}
void Renderer::draw_yuv(const void*) {}
void Renderer::queue_audio(const int16_t*, int) {}
// 这两个必须带 const，与头文件里的声明一致：漏了 const 的定义不是"另一个重载"，
// 而是直接编译不过（no declaration matches）——而没有 SDL2 的构建正好是
// "CI/服务器环境"这一支，最容易一直没人编到它。
int Renderer::queued_ms() const { return -1; }
int Renderer::queued_samples() const { return -1; }
void Renderer::reset_audio() {}
void Renderer::sample_audio() {}
void Renderer::enable_snapshot(int, std::string) {}
Renderer::AudioStats Renderer::audio_stats() const { return AudioStats{}; }

bool Renderer::pump_events() { return false; }

}  // namespace cue::probe

#endif
