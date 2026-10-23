// SDL2 渲染/播放实现。SDL2 不可用时整份是空实现（见文件末尾的 #else 分支）。
#include "probe_render.h"

#if defined(CUE_HAVE_SDL2) && CUE_HAVE_SDL2

#include <SDL2/SDL.h>

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
  // 播放队列的观测量：不暴露这几个数，就只能靠"听起来有没有咔咔"来猜。
  uint64_t audio_over_soft = 0;
  // 因时钟漂移而丢掉的 20 ms 包数（见 queue_audio 的说明）。
  uint64_t audio_dropped_drift = 0;  // 超过软上限（仍然放行，只记账）
  uint64_t audio_trimmed = 0;    // 超过硬上限被迫丢弃旧数据
  uint64_t audio_queued_pkts = 0;
  // 上一次整体纠正播放队列的时刻（毫秒）。见 queue_audio 里对限流的说明。
  Uint64 last_trim_ms = 0;
  // 周期性截图（见 enable_snapshot）：间隔、前缀、已存张数、上次存盘时刻。
  int snap_every_ms = 0;
  std::string snap_prefix;
  uint64_t snap_count = 0;
  Uint64 snap_last_ms = 0;
  uint64_t audio_push_errors = 0;  // SDL_QueueAudio 失败（设备被拔/后端异常）
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
      SDL_PauseAudioDevice(im->adev, 0);
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
  if (impl_ == nullptr || impl_->adev == 0 || pcm == nullptr || samples <= 0) return;
  // 播放侧的排队策略：**宁可多几百毫秒延迟，也不能丢包**。
  //
  // 旧实现是"队列一旦超过上限就把新包丢掉"，结果是：音频成串到达 → 队列超限 →
  // 之后每个新包都被静默丢弃 → 队列耗空 → 重新灌 → 再超限，形成持续的"咔咔"。
  // 而停流对账证明链路一个包都没丢（cued 发 7924、probe 收 7959），
  // 也就是说那些咔咔声完全是自己丢出来的。20 ms 的缺口远比 200 ms 额外延迟更刺耳，
  // 何况本地验证根本听不出 200 ms 延迟。
  //
  // 因此：软上限内只进不出（只记账），真的积压超过硬上限才丢，而且丢**旧**的——
  // 旧的继续播只是增加延迟，新的丢了却是一段空白。
  //
  // 两个取值为什么是 300 / 1000：
  //  · 300 ms 是"明显不对但还没到要动刀"的界线：20 ms 一帧，300 ms = 15 帧的积压，
  //    任何正常的到达抖动（几十 ms 量级）都够不着它，越过它就意味着有系统性的东西
  //    在推高延迟，必须让人看见（audio_over_soft 进指标，见 audio_stats()）。
  //  · 1000 ms 是止损点：50 帧积压 = 音画已经彻底不同步，再攒下去不如清空重来。
  //    清空的代价是"一下子跳过去"，但那是**一次**跳，比持续的咔咔好；而且健康链路
  //    永远到不了这里（48 kHz 设备晶振差 50 ppm 时，1000 ms 要几小时才积出来，
  //    那种情况宁可跳一次也不该让延迟无限涨）。
  //  SDL 没有"丢掉队首 N 字节"的接口（只有 Query/Clear），所以清空是唯一可用的
  //  修剪原语；这也是把硬上限做成"清空"而不是"截断"的原因。
  // 播放侧队列的纠正策略（第四版；前三版都被实测否掉，写清免得走回去）。
  //
  // v1 超过 200 ms 丢弃**新**包 → 稳定在阈值上，几乎每个包都丢 → 持续咔咔。
  // v2 超过 250 ms 用 SDL_DequeueAudio 丢队首 → Windows 后端返回 0，什么都没删，
  //    计数却 +1（"每次都在修剪、深度一动不动"）。
  // v3 超过 250 ms 用 SDL_ClearQueuedAudio 清到 80 ms → 实测 295 次清空，
  //    每次丢掉约 380 ms 真实音频；而且**对正在播放的设备清空会让它彻底哑掉**，
  //    用户听到的"一段时间后完全没声音"就是它。
  //
  // 现在这一版基于实测的成因：音频到达速率是 1.002 秒/秒（几乎正好实时），
  // 而声卡的消费速率与源有约 0.5% 的时钟漂移 ⇒ 队列会以 ~6 ms/s 缓慢上涨。
  // 真实播放器用自适应重采样解决；这里没有重采样器，就用**最小代价的丢包**：
  // 每越过上限只丢**当前这一个 20 ms 包**，丢完深度立刻回落 20 ms，
  // 要再涨回上限需要好几秒 —— 于是听感是"每几秒漏一帧 20 ms"，
  // 而不是"每隔一两分钟硬跳一次"。250 ms 的上限让延迟固定在可接受范围。
  //
  // 上限必须**高于源的一簇长度**，否则会周期性抽干。
  //
  // 实测线索：用户报告"音频几乎以每 0.4 秒规律性地卡一次"。0.4 s 这个固定周期不是
  // 随机的——它正是音频**成簇到达**的间隔（DirectShow/虚拟声卡按缓冲块交付，
  // 一次给约 0.4 秒的量）。而原来把上限设在 250 ms，比一簇还短：队列刚攒到 250 就被
  // 丢包压下去，两簇之间的空档里设备立刻把队列抽干 ⇒ 每个簇周期响一次"咔"。
  //
  // 所以上限取 800 ms（两倍于一簇，留出余量），让队列始终装得下一簇还不至于抽干；
  // 代价是音频延迟约 0.4~0.8 s——诊断用接收端完全可以接受，而周期性咔咔不能。
  // 判断依据是可观测的：如果听感仍然每 0.4 s 一咔，说明簇更长，这个值还要往上提。
  // 上限的取值逻辑（这一条是本轮音画同步的关键）：
  //   下限由"音频成簇到达"决定 —— 实测一簇约 0.4 s，缓冲低于它就必然在簇间抽干，
  //   表现为每 0.4 s 一声"咔"（这个已经用 250 ms 上限实测复现过）；
  //   上限由"能否被视频对齐"决定 —— 视频侧只能给一个**固定**延迟，而队列深度会在
  //   [0, 上限] 之间漂移，上限越大，能对齐的相同时段越短。之前设 800 ms 时实测深度
  //   在 160 / 320 / 520 / 660 之间来回跳，任何固定延迟都只能偶然对上。
  // 取 500：刚好高于一簇（不断音），又足够窄（漂移带只有 ~100 ms，可被固定延迟覆盖）。
  constexpr double kUpperMs = 500.0;
  constexpr double kPanicMs = 2000.0;
  const double q = double(queued_ms());
  if (q > kPanicMs) {
    SDL_ClearQueuedAudio(impl_->adev);
    ++impl_->audio_trimmed;
    impl_->last_trim_ms = SDL_GetTicks64();
  } else if (q > kUpperMs) {
    // 时钟漂移的代价：丢这一个包（20 ms 静音），换取整体不越积越深。
    ++impl_->audio_over_soft;
    ++impl_->audio_dropped_drift;
    return;
  }
  // samples 是"样本数"（不是帧数）：单声道的 s16 一个样本 2 字节，与 queued_ms()
  // 的换算口径必须一致（那里除以 2 字节/样本/声道）。
  const auto bytes = static_cast<Uint32>(samples) * static_cast<Uint32>(sizeof(int16_t));
  if (SDL_QueueAudio(impl_->adev, pcm, bytes) != 0) {
    ++impl_->audio_push_errors;
    if (!impl_->warned_audio) {
      impl_->warned_audio = true;
      std::fprintf(stderr, "[probe] SDL_QueueAudio 失败（只报一次）：%s\n", SDL_GetError());
    }
    return;
  }
  ++impl_->audio_queued_pkts;
}

int Renderer::queued_ms() const {
  if (impl_ == nullptr || impl_->adev == 0) return -1;
  const Uint32 bytes = SDL_GetQueuedAudioSize(impl_->adev);
  // 用设备**实际**参数（open 时校验过 == 我们要的 48 kHz / 单声道 / s16）换算：
  // 写死 48000 会让"设备其实跑在别的采样率"这种失效变成一条看起来正常的曲线。
  const double bytes_per_sec =
      double(impl_->audio_hz) * double(impl_->audio_ch) * double(sizeof(int16_t));
  if (bytes_per_sec <= 0.0) return -1;  // 设备没真正打开（audio_hz=0）
  return static_cast<int>(double(bytes) * 1000.0 / bytes_per_sec);
}

void Renderer::reset_audio() {
  if (impl_ == nullptr || impl_->adev == 0) return;
  // 换代/重连必须清：不清的话上一轮已经解码进队列的 PCM 会继续播完，
  // 用户听到的就是"重启后重复的那段其实是上一轮的旧数据"。
  SDL_ClearQueuedAudio(impl_->adev);
}

Renderer::AudioStats Renderer::audio_stats() const {
  AudioStats out;
  if (impl_ == nullptr) return out;
  out.over_soft = impl_->audio_over_soft;
  out.dropped_drift = impl_->audio_dropped_drift;
  out.trimmed = impl_->audio_trimmed;
  out.queued = impl_->audio_queued_pkts;
  out.push_errors = impl_->audio_push_errors;
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
bool Renderer::open(const RenderConfig&, std::string& err) {
  err = "本构建没有 SDL2 支持（配置时未找到 sdl2）：--render 不可用";
  return false;
}
void Renderer::draw_yuv(const void*) {}
void Renderer::queue_audio(const int16_t*, int) {}
// 这两个必须带 const，与头文件里的声明一致：漏了 const 的定义不是"另一个重载"，
// 而是直接编译不过（no declaration matches）——而没有 SDL2 的构建正好是
// "CI/服务器环境"这一支，最容易一直没人编到它。
int Renderer::queued_ms() const { return -1; }
void Renderer::reset_audio() {}
void Renderer::enable_snapshot(int, std::string) {}
Renderer::AudioStats Renderer::audio_stats() const { return AudioStats{}; }
void Renderer::enable_snapshot(int every_ms, std::string prefix) {
  if (impl_ == nullptr) return;
  impl_->snap_every_ms = every_ms > 0 ? every_ms : 0;
  impl_->snap_prefix = std::move(prefix);
  impl_->snap_last_ms = 0;
  impl_->snap_count = 0;
}

bool Renderer::pump_events() { return false; }

}  // namespace cue::probe

#endif
