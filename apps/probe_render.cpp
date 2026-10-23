// SDL2 渲染/播放实现。SDL2 不可用时整份是空实现（见文件末尾的 #else 分支）。
#include "probe_render.h"

#if defined(CUE_HAVE_SDL2) && CUE_HAVE_SDL2

#include <SDL2/SDL.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <cstdio>
#include <cstring>
#include <vector>

namespace cue::probe {

struct Renderer::Impl {
  SDL_Window* win = nullptr;
  SDL_Renderer* ren = nullptr;
  SDL_Texture* tex = nullptr;
  SDL_AudioDeviceID adev = 0;
  SwsContext* sws = nullptr;
  std::vector<uint8_t> rgb;  // 复用缓冲，避免每帧 malloc
  int w = 0;
  int h = 0;
  std::size_t rgb_size = 0;
  int tex_w = 0;
  int tex_h = 0;

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

bool Renderer::available() { return true; }
const char* Renderer::backend_name() { return "sdl2"; }

bool Renderer::open(const RenderConfig& cfg, std::string& err) {
  if (impl_ != nullptr) return true;
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
    err = std::string("SDL_Init: ") + SDL_GetError();
    return false;
  }
  auto* im = new Impl();
  im->w = cfg.width > 0 ? cfg.width : 640;
  im->h = cfg.height > 0 ? cfg.height : 360;
  im->win = SDL_CreateWindow("cue-probe", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             im->w, im->h, SDL_WINDOW_RESIZABLE);
  if (im->win == nullptr) {
    err = std::string("SDL_CreateWindow: ") + SDL_GetError();
    delete im;
    return false;
  }
  im->ren = SDL_CreateRenderer(im->win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
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
  im->adev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
  if (im->adev != 0) {
    SDL_PauseAudioDevice(im->adev, 0);
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
  // 尺寸变了就重建：
  //   · sws 上下文在尺寸/格式变化时必须重建（否则 sws_scale 返回错误码，画面黑屏）；
  //   · SDL texture 同理。
  if (im.sws == nullptr || im.w != f->width || im.h != f->height) {
    if (im.sws) sws_freeContext(im.sws);
    im.sws = sws_getContext(f->width, f->height, static_cast<AVPixelFormat>(f->format),
                            f->width, f->height, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr,
                            nullptr, nullptr);
    im.w = f->width;
    im.h = f->height;
    im.rgb_size = static_cast<std::size_t>(f->width) * static_cast<std::size_t>(f->height) * 3;
    im.rgb.assign(im.rgb_size, 0);
    if (im.tex) SDL_DestroyTexture(im.tex);
    im.tex = SDL_CreateTexture(im.ren, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
                               im.w, im.h);
    im.tex_w = im.w;
    im.tex_h = im.h;
    SDL_SetWindowSize(im.win, im.w, im.h);
  }
  if (im.sws == nullptr || im.tex == nullptr) return;
  uint8_t* dst[4] = {im.rgb.data(), nullptr, nullptr, nullptr};
  int dst_stride[4] = {im.w * 3, 0, 0, 0};
  sws_scale(im.sws, f->data, f->linesize, 0, f->height, dst, dst_stride);
  SDL_UpdateTexture(im.tex, nullptr, im.rgb.data(), im.w * 3);
  SDL_RenderClear(im.ren);
  SDL_RenderCopy(im.ren, im.tex, nullptr, nullptr);
  SDL_RenderPresent(im.ren);
}

void Renderer::queue_audio(const int16_t* pcm, int samples) {
  if (impl_ == nullptr || impl_->adev == 0 || pcm == nullptr || samples <= 0) return;
  // 队列上限 200 ms：网络卡一下不该让音频无限堆积（越积越延迟）。
  const Uint32 queued = SDL_GetQueuedAudioSize(impl_->adev);
  const Uint32 cap = static_cast<Uint32>(impl_->adev ? 19200 * 2 : 0);  // 200 ms @48k mono s16
  if (queued > cap) return;
  SDL_QueueAudio(impl_->adev, pcm, static_cast<Uint32>(samples * sizeof(int16_t)));
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
bool Renderer::open(const RenderConfig&, std::string& err) {
  err = "本构建没有 SDL2 支持（配置时未找到 sdl2）：--render 不可用";
  return false;
}
void Renderer::draw_yuv(const void*) {}
void Renderer::queue_audio(const int16_t*, int) {}
bool Renderer::pump_events() { return false; }

}  // namespace cue::probe

#endif
