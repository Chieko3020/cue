// cue-capture — 原生采集端：视频源 → libx264/ffmpeg 编码 → 上行 WS（与浏览器同一套协议）
//
// 三条后端：
//   lavfi   合成源（testsrc2 + sine），无需任何设备，`./scripts/run.sh demo-min` 用它
//   ffmpeg  dshow（Windows）/ avfoundation（macOS）子进程，设备名参数化
//   v4l2    Linux V4L2 摄像头（mmap + 双缓冲）
//
// 为什么要独立进程：浏览器侧（web/app.js）已经在产上行包，但"协议不分叉"这件事一直
// 没有第二个生产方来验证。本进程与浏览器**共用** include/cue/proto/wire.h 的 32 字节头与
// 分片语义（唯一实现见 proto/upstream_pack.h），cued 不需要任何"识别原生端"的分支。
//
// 视频编码只做一件事：x264（进程内 `--encode inproc`，默认）或让 ffmpeg 子进程编码
// （`--encode ffmpeg`，输出 Annex-B 裸流，由 AnnexBAuAssembler 按 AUD 分帧）。
// 默认 inproc 的理由：encode_ms 只有进程内编码才有真实值；子进程编码时这一项如实写 null。
// 音频固定进程内：PCM → swr_convert（libswresample）→ 48 kHz mono → libopus 20 ms 帧。
#if defined(_WIN32)
// Windows：winsock2 必须先于 windows.h（否则 winsock1 的头先被拉进来，socket 类型冲突）。
// ws2_32 由 CMake 的 target_link_libraries 负责。
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#include <fcntl.h>
#include <process.h>
#include <signal.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstdarg>
#include <ctime>
#include <functional>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

extern "C" {
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}
#if defined(__linux__)
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#endif

#include "cue/common/clock.h"
#include "cue/media/annexb.h"
#include "cue/media/audio_resampler.h"
#include "cue/media/avc_level.h"
#include "cue/media/encoder.h"
#include "cue/media/ffmpeg_args.h"
#include "cue/media/opus_encoder.h"
#include "cue/proto/upstream_pack.h"
#include "cue/proto/wire.h"

#include "capture_child_process.h"
#include "capture_pipe_budget.h"

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using json = nlohmann::json;
using cue::media::AnnexBAuAssembler;
using cue::media::AudioResampler;
using cue::media::CaptureSource;
using cue::media::DeviceKind;
using cue::media::FfmpegArgsSpec;
using cue::media::HostPlatform;
using cue::media::OpusAudioEncoder;
using cue::media::VideoEncodeMode;
using cue::media::X264Encoder;

namespace {

// 信号处理里只能碰 sig_atomic_t/volatile 这类类型；用 volatile sig_atomic_t 而不是
// std::atomic<bool>，避免在信号上下文里调用非 async-signal-safe 的实现。
volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

// ---------------------------------------------------------------- 日志/工具

void log_line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void log_line(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::fputs("[cue-capture] ", stderr);
  std::vfprintf(stderr, fmt, ap);
  std::fputc('\n', stderr);
  va_end(ap);
}

bool parse_int(std::string_view s, int& out) {
  const auto* b = s.data();
  const auto* e = s.data() + s.size();
  auto [p, ec] = std::from_chars(b, e, out);
  return ec == std::errc() && p == e;
}

bool parse_double(std::string_view s, double& out) {
  std::string tmp(s);
  char* end = nullptr;
  out = std::strtod(tmp.c_str(), &end);
  return end == tmp.c_str() + tmp.size() && !tmp.empty();
}

// ---------------------------------------------------------------- 选项

// 编码参数这一组默认值的取舍，全部来自"实时 + 丢包后能自愈"这两个约束。
//
// 注意：x264 参数的**拼装**不在本文件——进程内编码在 src/media/encoder.cpp
// （X264Encoder::open），子进程编码在 include/cue/media/ffmpeg_args.h
// （build_ffmpeg_argv）。本文件只决定"喂给它们什么"。下面每条结论都经过实测
// （ffmpeg 6.1 + libx264 164，看码流里 x264 写的 SEI options 行）：
//
// · deblock/cabac 是 preset 决定的，不是可选的画质旋钮。ultrafast 会同时把
//   deblock=0:0:0、cabac=0、8x8dct=0、aq=0、mbtree=0、analyse=0:0、subme=0 全关掉。
//   去块滤波是压制宏块边界的**唯一**工具，关掉之后平坦渐变区（testsrc2 的背景
//   正是）会露出整片 16×16 方块网格——就是那个"块状棋盘"。这不是码率不够：
//   给 ultrafast 更多码率只会把方块内部做得更准，方块边界照样跳变。所以本工具
//   在参数解析阶段就拒绝 ultrafast（见 check_preset），可用下限是 veryfast
//   （deblock/cabac/aq 都还在）。CPU 有余量时往上走 faster/medium：ref/subme/
//   trellis 提高，同码率下画质更好，代价是单帧编码耗时与端到端延迟。
//
// · B 帧：两条路径都已经是 0，这里不需要额外设置。进程内是 encoder.cpp 的
//   i_bframe=0 + tune=zerolatency；子进程是 ffmpeg 侧 max_b_frames 默认为 0 再加
//   -tune zerolatency（实测：medium 默认 bframes=3，加 zerolatency 后 0）。
//   代价是码率效率——B 帧在同质量下通常省 10~20% 码率，关掉后同码率画质略降。
//   换到的是不引入重排延迟：低延迟链路上这笔交易是确定的。
//   （tune=zerolatency 同时把 rc_lookahead/sync_lookahead 归零、mbtree 关掉，
//     实测 mbtree 1→0、bframes 3→0；sliced_threads=1 在 threads=1 时无实际影响。）
//
// · GOP：keyint 就是"丢一帧之后最多多久能自愈"。接收端丢包后必须等下一个自带
//   SPS/PPS 的 IDR 才能恢复，最坏情况是整整一个 GOP 的花屏。30 fps 下 gop=30 →
//   最坏 1 s，gop=60 → 2 s，x264 默认的 250 → 8 s（实时链路里等于"丢了就废了"）。
//   代价是 IDR 变密、同码率画质略降（1 s GOP 的 IDR 开销在 640×360/1.2 Mbps 上
//   约占 10~20% 码率）。实时优先，所以默认 30。
//
// · repeat-headers（每个 IDR 自带 SPS/PPS）是上面"自愈"成立的前提：下行一旦丢掉
//   开头的 CodecConfig，接收端会对**每一帧**都回 -1094995529（AVERROR_INVALIDDATA）
//   且永不恢复，画面永久冻住。这一项在两条路径上目前都**是开的**，但依据不同：
//   子进程路径靠 ffmpeg_args.h 的 -x264-params（实测 -f h264 裸流每帧前都有
//   SPS PPS：SPS PPS SEI IDR / SPS PPS IDR / …），进程内路径靠 libx264 的
//   b_repeat_headers 默认值（x264 之所以默认重发，是因为容器侧要它显式关掉才行）。
//   **注意 x264 的参数名是 repeat-headers（短横线）**，写成 repeat_headers 会被
//   静默忽略——ffmpeg_args.h 里目前就是下划线写法，靠默认值兜住了。这也说明
//   "参数写了"不等于"参数生效"：这一条必须靠看码流验证，不能靠读命令行。
struct Options {
  std::string backend = "lavfi";  // lavfi | ffmpeg | v4l2
  std::string ws_url = "ws://127.0.0.1:9000/";
  int width = 640;
  int height = 360;
  int fps = 30;
  int bitrate_kbps = 1200;
  int gop = 30;
  std::string preset = "veryfast";
  // x264 编码线程数。0 = 交给 x264 自己按核数决定（原来的实现把它写死成 1，
  // 在 1080p 上实测 encode p50 29.5 ms —— 33 ms 的帧预算只剩 10%，采集端直接
  // 掉到 28 fps，而这台机器上整条链路总共只用了 9% 的 CPU。
  // 低延迟不靠"少开线程"保证：tune=zerolatency 会把一帧切成多 slice 并行编，
  // 线程多不会引入帧级排队。真要在小机器上让路，显式传 --threads 1 即可。
  int threads = 0;
  std::string device = "/dev/video0";
  std::string audio_device;  // 空 = 同 device（dshow/avfoundation 的设备名）
  double duration_s = 0.0;   // 0 = 无限
  bool audio = true;
  std::string encode = "inproc";  // inproc | ffmpeg
  std::string input;              // 文件源（FR-CAP.3 的"文件"半边）
  int audio_rate = 44100;         // 源 PCM（故意不是 48k，让 swr 真的重采样）
  int audio_channels = 2;
  int avc_level = 0;              // 0 = 按面积自动（与浏览器 pickCodec 同一张表）
  // 上行 Opus 的弱网参数（N-01）。默认值来自 OpusAudioEncoder 的常量，那里写清了
  // 依据（loss perc=10 是"让 libopus 真的生成 LBRR 的最小值"；DTX 默认开，实测不动
  // 活动帧的码流）。
  int opus_loss_perc = OpusAudioEncoder::kDefaultPacketLossPerc;
  bool opus_dtx = true;
  std::string ffmpeg_bin = "ffmpeg";
  std::string dump_wire;  // 非空 = 只打印协议包样例（协议对照脚本用）
};

void print_usage() {
  std::printf(
      "cue-capture — 原生采集端（上行 WS 协议与浏览器共用）\n"
      "\n"
      "用法：cue-capture [选项]\n"
      "  --backend lavfi|ffmpeg|v4l2   采集后端（默认 lavfi：合成源，无需设备）\n"
      "  --ws URL                      cued 地址（默认 ws://127.0.0.1:9000/）\n"
      "  --size WxH                    采集/编码尺寸（默认 640x360）\n"
      "  --fps N                       帧率（默认 30）\n"
      "  --bitrate N                   视频码率 kbps（默认 1200）\n"
      "  --gop N                       GOP 帧数（默认 30：30 fps 下最坏 1 s 自愈；见源码注释）\n"
      "  --preset X                    x264 preset（默认 veryfast；ultrafast 被拒绝，见源码注释）\n"
      "  --threads N                   x264 编码线程数（默认 0 = 按核数自动；小机器可显式压到 1）\n"
      "  --device NAME                 v4l2 设备路径（默认 /dev/video0）/ dshow 设备名 /\n"
      "                                avfoundation 索引（如 0 或 0:1）\n"
      "  --audio-device NAME           dshow/avfoundation 音频设备（默认同 --device）\n"
      "  --audio on|off                是否采集/编码音频（默认 on；v4l2 后端无音频设备）\n"
      "  --encode inproc|ffmpeg        视频编码位置（默认 inproc：libx264 在进程内）\n"
      "  --input PATH                  用文件做源（否则 lavfi testsrc2/sine）\n"
      "  --duration S                  采集时长，0 = 无限（默认 0）\n"
      "  --avc-level IDC               AVC level ×10（默认按面积自动：≤720p L3.0、否则 L4.0）\n"
      "  --audio-format RATE:CH        源 PCM 格式（默认 44100:2，采集端负责重采样到 48k mono）\n"
      "  --opus-loss-perc N            Opus in-band FEC 的预期丢包率 0..100（默认 10）。\n"
      "                                实测：5 只有 4%% 的帧带 LBRR，10 才 100%% 带；\n"
      "                                0 = 完全不带 FEC。依据与实测数据见 opus_encoder.h\n"
      "  --opus-dtx on|off             Opus DTX（静音段发 1 字节帧，默认 on，依据见源码注释）\n"
      "  --ffmpeg PATH                 ffmpeg 可执行文件（默认 ffmpeg）\n"
      "  --dump-wire PATH              写出协议包样例（- = stdout）后退出\n"
      "  --help                        本帮助\n"
      "\n"
      "每条统计按秒打到 stdout（JSON 行，前缀 CAPTURE）：capture_fps / frames_in / frames_out /\n"
      "dropped / encode_ms_p50 / encode_ms_p99 / bytes_sent / audio_frames_out / audio_bytes_sent\n");
}

// x264 的十档 preset。白名单挡的是拼写错误：拼错的档位在进程内只是
// x264_param_default_preset 返回失败、在子进程只是 ffmpeg 起不来，两种都比在
// 参数解析阶段直接报错难查（用户看到的会是"编码器打不开"而不是"参数写错了"）。
constexpr std::string_view kX264Presets[] = {
    "ultrafast", "superfast", "veryfast", "faster", "fast",
    "medium",    "slow",      "slower",   "veryslow", "placebo"};

// 为什么把 ultrafast 直接判成参数错误，而不是"警告一下继续跑"。
// 本工具存在的意义是产出一条**有代表性的**上行流给 cued/probe 做链路验证；
// ultrafast 产出的画面不是这条链路的画面，而是"没开去块滤波的 x264"的画面。
// 实测（ffmpeg 6.1 + libx264 164）ultrafast 会同时关掉 deblock=0:0:0、cabac=0、
// 8x8dct=0、aq=0、mbtree=0、analyse=0:0、subme=0；平坦渐变区因此整片 16×16 方块
// 网格跳变，看起来就是"块状棋盘"。给更多码率也没用——方块内部的梯度会更准，
// 方块边界照样跳变。要压 CPU 请改 cued 侧的 --preset（转码链有自己的降级语义），
// 不要把采集端的画面质量当成 CPU 旋钮。
bool check_preset(std::string_view preset, std::string& err) {
  bool known = false;
  for (const auto p : kX264Presets) {
    if (p == preset) {
      known = true;
      break;
    }
  }
  if (!known) {
    err = "--preset 不是 x264 的档位：" + std::string(preset) +
          "（可选 ultrafast/superfast/veryfast/faster/fast/medium/slow/slower/veryslow/placebo）";
    return false;
  }
  if (preset == "ultrafast") {
    err = "--preset ultrafast 被拒绝：它关掉 deblock/cabac/aq/mbtree 与全部分区"
          "（实测 deblock=0:0:0 cabac=0 aq=0 analyse=0:0 subme=0），去块滤波一关，"
          "平坦渐变区就会露出 16×16 方块网格（块状棋盘）。可用下限是 veryfast；"
          "要压 CPU 请调 cued 侧的 --preset。";
    return false;
  }
  return true;
}

bool parse_options(int argc, char** argv, Options& o, std::string& err) {
  auto next = [&](int& i, const char* name) -> std::optional<std::string_view> {
    if (i + 1 >= argc) {
      err = std::string("选项 ") + name + " 缺少参数";
      return std::nullopt;
    }
    return std::string_view(argv[++i]);
  };
  for (int i = 1; i < argc; ++i) {
    const std::string_view a(argv[i]);
    if (a == "--help" || a == "-h") {
      print_usage();
      std::exit(0);
    } else if (a == "--backend") {
      auto v = next(i, "--backend");
      if (!v) return false;
      o.backend = std::string(*v);
    } else if (a == "--ws") {
      auto v = next(i, "--ws");
      if (!v) return false;
      o.ws_url = std::string(*v);
    } else if (a == "--size") {
      auto v = next(i, "--size");
      if (!v) return false;
      const std::size_t x = v->find('x');
      if (x == std::string_view::npos || !parse_int(v->substr(0, x), o.width) ||
          !parse_int(v->substr(x + 1), o.height) || o.width <= 0 || o.height <= 0) {
        err = "--size 需要 WxH（如 640x360）";
        return false;
      }
    } else if (a == "--fps") {
      auto v = next(i, "--fps");
      if (!v || !parse_int(*v, o.fps)) return false;
    } else if (a == "--bitrate") {
      auto v = next(i, "--bitrate");
      if (!v || !parse_int(*v, o.bitrate_kbps)) return false;
    } else if (a == "--gop") {
      auto v = next(i, "--gop");
      if (!v || !parse_int(*v, o.gop)) return false;
    } else if (a == "--preset") {
      auto v = next(i, "--preset");
      if (!v) return false;
      if (!check_preset(*v, err)) return false;
      o.preset = std::string(*v);
    } else if (a == "--threads") {
      auto v = next(i, "--threads");
      if (!v) return false;
      o.threads = std::clamp(std::atoi(std::string(*v).c_str()), 0, 64);
    } else if (a == "--device") {
      auto v = next(i, "--device");
      if (!v) return false;
      o.device = std::string(*v);
    } else if (a == "--audio-device") {
      auto v = next(i, "--audio-device");
      if (!v) return false;
      o.audio_device = std::string(*v);
    } else if (a == "--duration") {
      auto v = next(i, "--duration");
      if (!v || !parse_double(*v, o.duration_s) || o.duration_s < 0) {
        err = "--duration 需要非负秒数";
        return false;
      }
    } else if (a == "--audio") {
      auto v = next(i, "--audio");
      if (!v) return false;
      if (*v == "on") {
        o.audio = true;
      } else if (*v == "off") {
        o.audio = false;
      } else {
        err = "--audio 只接受 on|off";
        return false;
      }
    } else if (a == "--encode") {
      auto v = next(i, "--encode");
      if (!v) return false;
      if (*v != "inproc" && *v != "ffmpeg") {
        err = "--encode 只接受 inproc|ffmpeg";
        return false;
      }
      o.encode = std::string(*v);
    } else if (a == "--input") {
      auto v = next(i, "--input");
      if (!v) return false;
      o.input = std::string(*v);
    } else if (a == "--audio-format") {
      auto v = next(i, "--audio-format");
      if (!v) return false;
      const std::size_t c = v->find(':');
      if (c == std::string_view::npos || !parse_int(v->substr(0, c), o.audio_rate) ||
          !parse_int(v->substr(c + 1), o.audio_channels) || o.audio_rate <= 0 ||
          o.audio_channels <= 0) {
        err = "--audio-format 需要 RATE:CH（如 44100:2）";
        return false;
      }
    } else if (a == "--avc-level") {
      auto v = next(i, "--avc-level");
      if (!v || !parse_int(*v, o.avc_level) || o.avc_level < 0) {
        err = "--avc-level 需要非负整数（30 = L3.0、40 = L4.0）";
        return false;
      }
    } else if (a == "--opus-loss-perc") {
      auto v = next(i, "--opus-loss-perc");
      if (!v) return false;
      int perc = 0;
      // 范围与 libopus 的契约一致（0..100，见 OpusAudioEncoder::kMaxPacketLossPerc）。
      // 越界在解析阶段就拒：编码器那边的 set_packet_loss_perc 也会拒，两处都要硬，
      // 否则"参数写了但没生效"只会表现为音质悄悄变差。
      if (!parse_int(*v, perc) || perc < OpusAudioEncoder::kMinPacketLossPerc ||
          perc > OpusAudioEncoder::kMaxPacketLossPerc) {
        err = "--opus-loss-perc 需要 0..100 的整数（0 = 不带 in-band FEC）";
        return false;
      }
      o.opus_loss_perc = perc;
    } else if (a == "--opus-dtx") {
      auto v = next(i, "--opus-dtx");
      if (!v) return false;
      if (*v == "on") {
        o.opus_dtx = true;
      } else if (*v == "off") {
        o.opus_dtx = false;
      } else {
        err = "--opus-dtx 只接受 on|off";
        return false;
      }
    } else if (a == "--ffmpeg") {
      auto v = next(i, "--ffmpeg");
      if (!v) return false;
      o.ffmpeg_bin = std::string(*v);
    } else if (a == "--dump-wire") {
      auto v = next(i, "--dump-wire");
      if (!v) return false;
      o.dump_wire = std::string(*v);
    } else {
      err = std::string("未知选项：") + std::string(a);
      return false;
    }
  }
  if (o.fps <= 0 || o.bitrate_kbps <= 0 || o.gop <= 0) {
    err = "--fps/--bitrate/--gop 必须为正";
    return false;
  }
  if (o.width % 2 != 0 || o.height % 2 != 0) {
    err = "--size 的宽高必须是偶数（yuv420p）";
    return false;
  }
  return true;
}

HostPlatform host_platform() {
#if defined(_WIN32)
  return HostPlatform::Windows;
#elif defined(__APPLE__)
  return HostPlatform::macOS;
#else
  return HostPlatform::Linux;
#endif
}

// AVC level 的选择表在 include/cue/media/avc_level.h（media 层，N-09 从这里的匿名
// 命名空间搬出去）。理由有两条：这张表与浏览器侧 web/app.js 的 pickCodec 是两份
// 平行实现，必须对同一分辨率给出同一个 level，probe 侧对照编码参数才有意义；而写在
// app 里的函数 tests/ 链接不到，两个边界值（414720 / 2097152）只能靠人工跑管线验证。
// 面积超过 1080p 时浏览器会拒绝（pickCodec 返回 null），采集端不拒绝，拿到的 0 表示
// "交给 x264 自动推导"，调用点据此打一行提示。

// ---------------------------------------------------------------- 子进程

using cue::capture::ChildProcess;
using cue::capture::IoStatus;

// ---------------------------------------------------------------- WS 客户端

// 上行 WS 客户端。与浏览器使用同一套握手、同一个 ping/pong 时钟同步（§7.1）与
// 同一套二进制分片——cued 侧没有任何"这是浏览器还是原生端"的分支。
// 下行关键帧请求：cued 收到 probe 的请求后，会通过同一条 WS 发一个上游
// KeyframeReq(0x04) 回来。采集端平时只写不读，所以这个请求必须靠 ws.poll_control()
// 主动取；取到之后置位，由编码循环在**下一帧**插一个 IDR。
// 用全局标志而不是穿参数：采集端是单实例 CLI，多传一个指针要改三处签名，
// 而这个标志只被「发送线程置位 / 编码线程消费」两个地方碰到。
std::atomic<bool> g_want_idr{false};

class WsSender {
 public:
  explicit WsSender(std::string url) : url_(std::move(url)) {}
  ~WsSender() { close(); }

  bool connect(std::string& err) {
    close();
    if (!parse_url(err)) return false;
    ws_.emplace(ioc_.get_executor());
    beast::error_code ec;
    tcp::resolver resolver(ioc_);
    const auto results = resolver.resolve(host_, port_, ec);
    if (ec) {
      err = "解析 " + host_ + ":" + port_ + " 失败: " + ec.message();
      ws_.reset();
      return false;
    }
    ws_->next_layer().expires_after(std::chrono::seconds(3));
    ws_->next_layer().connect(results, ec);
    if (ec) {
      err = "连接 " + host_ + ":" + port_ + " 失败: " + ec.message();
      ws_.reset();
      return false;
    }
    const std::string host_header = port_ == "80" ? host_ : host_ + ":" + port_;
    ws_->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
    ws_->read_message_max(1 << 20);
    ws_->next_layer().expires_after(std::chrono::seconds(3));
    ws_->handshake(host_header, target_, ec);
    if (ec) {
      err = "WS 握手失败: " + ec.message();
      ws_.reset();
      return false;
    }
    ws_->binary(true);
    connected_.store(true, std::memory_order_relaxed);
    sync_clock();
    return true;
  }

  // 非阻塞地取一个已经到达的控制消息。
  //
  // 为什么要 available() 先探测：采集端的发送线程持着 mu_ 写帧，如果这里直接
  // ws_->read()，socket 上没有数据时会把锁连同发送一起阻塞住（帧就掉节奏了）。
  // 先问「有没有至少一个上行头那么长」可读，有才进去读，并用 50 ms 超时兜底，
  // 防止只到了一半的头把线程钉死。
  //
  // 目前只认一种控制消息：上游 KeyframeReq(0x04)。读完必须把流恢复成二进制模式，
  // 否则接下来的 send_binary 会撞上「上一条消息没读完」的错误。
  void poll_control() {
    if (!connected_.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (!ws_) return;
    beast::error_code ec;
    const std::size_t avail = ws_->next_layer().socket().available(ec);
    if (ec || avail < cue::proto::kMediaHeaderSize) return;
    ws_->next_layer().expires_after(std::chrono::milliseconds(50));
    beast::flat_buffer buf;
    ws_->read(buf, ec);
    ws_->binary(true);  // 恢复成二进制模式，供发送帧使用
    if (ec) return;
    const auto* raw = static_cast<const uint8_t*>(buf.data().data());
    if (buf.data().size() < cue::proto::kMediaHeaderSize) return;
    if (raw[0] != cue::proto::kMagicUp) return;
    if (raw[1] != static_cast<uint8_t>(cue::proto::MediaType::KeyframeReq)) return;
    g_want_idr.store(true, std::memory_order_relaxed);
  }

  bool send_binary(std::span<const uint8_t> data, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!ws_ || !connected_.load(std::memory_order_relaxed)) {
      err = "WS 未连接";
      return false;
    }
    beast::error_code ec;
    // 每次写前设超时：cued 卡死时阻塞写会把采集线程一起拖住，指标上看是"采集停了"，
    // 真正的原因却是对端没在 read。5 s 是这段链路里最大的合理单帧写耗时。
    ws_->next_layer().expires_after(std::chrono::seconds(5));
    ws_->write(net::buffer(data.data(), data.size()), ec);
    if (ec) {
      connected_.store(false, std::memory_order_relaxed);
      err = "WS 写失败: " + ec.message();
      return false;
    }
    bytes_sent_.fetch_add(data.size(), std::memory_order_relaxed);
    return true;
  }

  void close() {
    connected_.store(false, std::memory_order_relaxed);
    if (ws_) {
      beast::error_code ec;
      ws_->next_layer().socket().close(ec);
      ws_.reset();
    }
  }

  bool connected() const { return connected_.load(std::memory_order_relaxed); }
  int64_t offset_us() const { return offset_us_; }
  int64_t now_us() const { return cue::now_us() + offset_us_; }
  uint64_t bytes_sent() const { return bytes_sent_.load(std::memory_order_relaxed); }

 private:
  bool parse_url(std::string& err) {
    std::string_view u = url_;
    if (u.starts_with("ws://")) {
      u.remove_prefix(5);
    } else if (u.starts_with("wss://")) {
      err = "不支持 wss://（本机/内网采集走 ws://，TLS 由反代终止）";
      return false;
    } else {
      err = "--ws 需要 ws://host:port/path";
      return false;
    }
    const std::size_t slash = u.find('/');
    const std::string authority(u.substr(0, slash));
    target_ = slash == std::string_view::npos ? "/" : std::string(u.substr(slash));
    const std::size_t colon = authority.rfind(':');
    if (colon == std::string::npos) {
      host_ = authority;
      port_ = "80";
    } else {
      host_ = authority.substr(0, colon);
      port_ = authority.substr(colon + 1);
    }
    if (host_.empty() || port_.empty()) {
      err = "--ws 的 host/port 为空";
      return false;
    }
    return true;
  }

  // 与 web/app.js 完全同款：8 次 ping 取最小 RTT 的那次估 offset，
  // pts 由此落进 cued 的单调时钟域（probe 的端到端延迟靠这个域才可读）。
  void sync_clock() {
    int64_t best_rtt = std::numeric_limits<int64_t>::max();
    int64_t best_offset = 0;
    int ok = 0;
    beast::error_code ec;
    for (int i = 0; i < 8; ++i) {
      const int64_t t = cue::now_us();
      const json ping{{"op", "ping"}, {"t", t}};
      const std::string payload = ping.dump();
      ws_->text(true);
      ws_->next_layer().expires_after(std::chrono::seconds(2));
      ws_->write(net::buffer(payload), ec);
      if (ec) break;
      beast::flat_buffer buf;
      ws_->read(buf, ec);
      if (ec) break;
      if (!ws_->got_text()) {
        ws_->binary(true);
        continue;
      }
      const auto body = std::string_view(static_cast<const char*>(buf.data().data()),
                                         buf.data().size());
      const json j = json::parse(body, nullptr, false);
      if (!j.is_object() || j.value("op", "") != "pong") {
        ws_->binary(true);
        continue;
      }
      const int64_t rtt = cue::now_us() - t;
      if (rtt < best_rtt) {
        best_rtt = rtt;
        best_offset = j.value("server_us", static_cast<int64_t>(0)) - (t + rtt / 2);
      }
      ++ok;
    }
    ws_->binary(true);
    offset_us_ = ok > 0 ? best_offset : 0;
    log_line("clock offset %+.2f ms（%d 次 pong，最小 RTT %.2f ms）",
             static_cast<double>(offset_us_) / 1000.0, ok,
             best_rtt == std::numeric_limits<int64_t>::max()
                 ? 0.0
                 : static_cast<double>(best_rtt) / 1000.0);
  }

  std::string url_, host_, port_ = "80", target_ = "/";
  net::io_context ioc_;
  std::optional<websocket::stream<beast::tcp_stream>> ws_;
  std::mutex mu_;
  std::atomic<bool> connected_{false};
  std::atomic<uint64_t> bytes_sent_{0};
  int64_t offset_us_ = 0;
};

// ---------------------------------------------------------------- 已编码帧队列

struct EncodedFrame {
  std::vector<uint8_t> data;
  bool keyframe = false;
  uint64_t pts_us = 0;
  uint32_t frame_id = 0;
};

// 采集线程 → 发送线程的单生产者单消费者队列。满了丢最老的一帧：直播链路里
// 迟到的帧没有发送价值，积压只会把端到端延迟推高（cued 侧的抖动缓冲会照单全收）。
class FrameQueue {
 public:
  explicit FrameQueue(std::size_t max) : max_(max) {}
  // 返回 false 表示为了入队丢掉了一帧（调用方计 dropped）
  bool push(EncodedFrame f) {
    std::unique_lock<std::mutex> lk(mu_);
    bool dropped = false;
    if (q_.size() >= max_) {
      q_.pop_front();
      dropped = true;
    }
    q_.push_back(std::move(f));
    cv_.notify_one();
    return !dropped;
  }
  bool pop(EncodedFrame& out) {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [&] { return !q_.empty() || stopped_; });
    if (q_.empty()) return false;
    out = std::move(q_.front());
    q_.pop_front();
    return true;
  }
  void stop() {
    std::lock_guard<std::mutex> lk(mu_);
    stopped_ = true;
    cv_.notify_all();
  }

 private:
  std::size_t max_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<EncodedFrame> q_;
  bool stopped_ = false;
};

// ---------------------------------------------------------------- 采集节拍

// 合成源/文件源的播放时钟：第 n 帧的 pts = t0 + n/fps，且**读之前**先睡到该时刻。
//
// 为什么不能直接用"读到帧的时刻"当 pts：ffmpeg 的 -re 在启动时会先突发若干帧
// （实测前 10 帧的 PTS 增量是 3–12 ms，之后才稳定到 33 ms）。cued 的抖动缓冲用
// PTS 增量 EWMA 估计帧间隔，而它只在 dpts ≤ 1.5×估计值时才更新——前 10 个 3 ms 增量
// 会把估计值压到 ~19 ms 且此后再也升不回去（33 ms 的增量永远被判成"大空洞"），
// 结果每一帧都被记成 skipped、每帧白等 40 ms 预算。实测：不定拍时 cued 的
// jb_skipped 约等于 frames_in（≈1/帧），probe 侧同样。
//
// 摄像头（v4l2 / dshow / avfoundation）由设备定拍，用到达时刻当 pts（enabled=false）。
class FramePacer {
 public:
  FramePacer(int fps, bool enabled)
      : interval_us_(fps > 0 ? 1'000'000 / fps : 33'333), enabled_(enabled) {}

  bool enabled() const { return enabled_; }
  bool started() const { return started_; }

  // 在**读取之前**调用：睡到第 n 帧应当被采集的时刻。
  // 第一帧不睡：t0 必须取"第一帧真的到达"的时刻，否则 ffmpeg 子进程 0.3–0.5 s 的
  // 启动时间会变成第 0 帧到第 1 帧之间的一个假空洞（实测 cued 一口气记 14 个 skipped）。
  void before_frame() {
    if (!enabled_ || !started_) return;
    next_us_ += interval_us_;
    const int64_t now = cue::now_us();
    // 落后超过一帧：重新对齐到当前时刻。编码/发送偶尔拖慢时 pts 不该一路欠账
    // （那会让"端到端延迟"比实际小，指标失真）；代价是这次跳变会被下游记成空洞，
    // 这是真实发生的停顿，记下来比瞒过去好。
    if (next_us_ + interval_us_ < now) next_us_ = now;
    while (!g_stop) {
      const int64_t remain = next_us_ - cue::now_us();
      if (remain <= 0) break;
      std::this_thread::sleep_for(std::chrono::microseconds(std::min<int64_t>(remain, 5'000)));
    }
  }

  // 每读到一帧调用一次；第一帧用它定 t0，之后不影响时间轴。
  void note_frame_arrived(int64_t arrival_us) {
    if (!enabled_ || started_) return;
    started_ = true;
    next_us_ = arrival_us;
  }

  // 按"帧/访问单元"定拍：返回这一帧的 pts，必要时先睡到该时刻。
  // 用于子进程编码路径——那里的读取边界是字节流，不是帧边界，没法按"读之前睡"定拍。
  int64_t pace_next(int64_t now) {
    if (!enabled_) return now;
    if (!started_) {
      started_ = true;
      next_us_ = now;
      return now;
    }
    next_us_ += interval_us_;
    if (next_us_ + interval_us_ < now) next_us_ = now;
    while (!g_stop) {
      const int64_t remain = next_us_ - cue::now_us();
      if (remain <= 0) break;
      std::this_thread::sleep_for(std::chrono::microseconds(std::min<int64_t>(remain, 5'000)));
    }
    return next_us_;
  }

  int64_t pts() const { return next_us_; }

 private:
  int64_t interval_us_;
  int64_t next_us_ = 0;
  bool enabled_;
  bool started_ = false;
};

// ---------------------------------------------------------------- 统计

struct Stats {
  std::atomic<uint64_t> frames_captured{0};
  std::atomic<uint64_t> frames_sent{0};
  std::atomic<uint64_t> dropped{0};
  std::atomic<uint64_t> encode_errors{0};
  std::atomic<uint64_t> send_errors{0};
  std::atomic<uint64_t> oversized{0};
  std::atomic<uint64_t> audio_frames_sent{0};
  std::atomic<uint64_t> audio_bytes_sent{0};
  // 音频定拍：因为落后太多而重锚的次数（见 audio_loop 里的说明）。
  std::atomic<uint64_t> audio_pace_resets{0};
  std::atomic<uint64_t> audio_encode_errors{0};

  void add_encode_us(int64_t us) {
    std::lock_guard<std::mutex> lk(enc_mu);
    encode_us_.push_back(us);
    if (encode_us_.size() > kEncodeRing) encode_us_.pop_front();
  }
  std::vector<int64_t> encode_samples() {
    std::lock_guard<std::mutex> lk(enc_mu);
    return {encode_us_.begin(), encode_us_.end()};
  }

  static constexpr std::size_t kEncodeRing = 4096;

 private:
  std::mutex enc_mu;
  std::deque<int64_t> encode_us_;
};

double percentile_ms(std::vector<int64_t> v, double p) {
  if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
  std::sort(v.begin(), v.end());
  const std::size_t idx = static_cast<std::size_t>(
      std::min<double>(static_cast<double>(v.size() - 1), p * static_cast<double>(v.size() - 1)));
  return static_cast<double>(v[idx]) / 1000.0;
}

json pct_or_null(const std::vector<int64_t>& v, double p) {
  if (v.empty()) return json(nullptr);
  return json(percentile_ms(v, p));
}

// ---------------------------------------------------------------- 音频

struct AudioContext {
  ChildProcess* source = nullptr;
  AudioResampler* resampler = nullptr;
  OpusAudioEncoder* opus = nullptr;
  WsSender* sender = nullptr;
  Stats* stats = nullptr;
  uint32_t stream_id = 2;
  int channels = 2;
  std::atomic<bool>* audio_failed = nullptr;
};

// 音频线程：PCM → swr_convert → 960 样本/帧 → opus → 上行包。
// 20 ms 一帧的 pts 由首帧时间 + n*20 ms 递推（与浏览器 onAudioChunk 同一个口径），
// 不按"读取时刻"打戳，否则调度抖动会混进 20 ms 的节奏里。
void audio_loop(AudioContext ctx) {
  std::vector<uint8_t> raw(8192);
  std::vector<uint8_t> pending;
  std::vector<int16_t> resampled;
  std::vector<int16_t> mono;
  mono.reserve(48000);
  const std::size_t frame_bytes = sizeof(int16_t) * static_cast<std::size_t>(ctx.channels);
  uint32_t frame_id = 0;
  int64_t pts_base = 0;
  int64_t n_packets = 0;
  // 音频定拍的下一个发送时刻（见 send_frames 里的说明）。0 = 还没锚定。
  int64_t pace_next_us = 0;
  std::string err;

  auto send_frames = [&]() {
    while (mono.size() >= static_cast<std::size_t>(OpusAudioEncoder::kFrameSamples)) {
      auto pkt = ctx.opus->encode(mono.data(), OpusAudioEncoder::kFrameSamples, err);
      if (!pkt) {
        ctx.stats->audio_encode_errors.fetch_add(1, std::memory_order_relaxed);
        log_line("opus 编码失败（%s）：%s", cue::media::opus_error_name(pkt.error()), err.c_str());
        if (ctx.audio_failed) ctx.audio_failed->store(true, std::memory_order_relaxed);
        return;
      }
      if (pts_base == 0) pts_base = ctx.sender->now_us();
      const int64_t pts = pts_base + n_packets * 20000;
      ++n_packets;
      cue::proto::UpstreamMedia m;
      m.type = static_cast<uint8_t>(cue::proto::MediaType::Audio);
      m.codec = static_cast<uint8_t>(cue::proto::Codec::Opus);
      m.stream_id = ctx.stream_id;
      m.frame_id = frame_id++;
      m.pts_us = static_cast<uint64_t>(pts);
      m.keyframe = true;  // 每个 Opus 包都是独立可解帧（浏览器侧同样置位）
      // ---- 音频定拍（本函数最重要的几行）----
      //
      // 为什么必须在这里等：音频来自 ffmpeg 子进程读设备，而 DirectShow / 虚拟声卡是
      // **按缓冲块交付**的——实测一簇约 0.4 秒。原来"读到就发"，于是上行、下行、
      // 接收端播放队列全都跟着 0.4 秒一簇地脉动：接收端的播放队列深度在 0~500 ms 之间
      // 锯齿摆动（实测中位 240、极差 500），而**同一簇内最早与最晚样本的播放延迟本身
      // 就差 0.4 秒**——任何固定的音画补偿延迟都不可能全程同步。
      // 按 20 ms 一包匀速吐出去之后，接收端队列深度会稳定在几十毫秒，延迟成为常量，
      // 音画同步才是一个可解的固定值。
      //
      // pts 本来就是 t0 + n*20ms（不受调度抖动影响），这里补上"**发送时刻**也等距"。
      // 落后超过 200 ms 就重锚：宁可丢一段节奏，也不要永久处于追赶状态
      // （永久追赶意味着越积越多，最后连 pts 都和实际时间脱节）。
      {
        if (pace_next_us == 0) pace_next_us = ctx.sender->now_us();
        const int64_t due = pace_next_us;
        pace_next_us += 20000;
        for (;;) {
          const int64_t now = ctx.sender->now_us();
          if (now >= due) {
            if (now - due > 200'000) {
              // 落后太多（例如线程被调度走了、或设备一次给了很长的块）：重锚到当前时刻。
              pace_next_us = now;
              ctx.stats->audio_pace_resets.fetch_add(1, std::memory_order_relaxed);
            }
            break;
          }
          if (g_stop != 0) return;
          const int64_t remain = due - now;
          std::this_thread::sleep_for(std::chrono::microseconds(remain > 2000 ? 2000 : remain));
        }
      }
      const auto packets = cue::proto::pack_upstream(*pkt, m);
      bool ok = true;
      for (const auto& p : packets) {
        if (!ctx.sender->send_binary(p, err)) {
          ok = false;
          ctx.stats->send_errors.fetch_add(1, std::memory_order_relaxed);
          break;
        }
      }
      if (ok) {
        ctx.stats->audio_frames_sent.fetch_add(1, std::memory_order_relaxed);
        ctx.stats->audio_bytes_sent.fetch_add(pkt->size(), std::memory_order_relaxed);
      }
      mono.erase(mono.begin(), mono.begin() + OpusAudioEncoder::kFrameSamples);
    }
  };

  while (g_stop == 0) {
    std::size_t got = 0;
    const IoStatus st = ctx.source->read_some(raw.data(), raw.size(), got, 500);
    if (st == IoStatus::Timeout) continue;
    if (st == IoStatus::Error) {
      log_line("音频源读取失败");
      break;
    }
    if (st == IoStatus::Eof) break;
    pending.insert(pending.end(), raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(got));
    const std::size_t usable = pending.size() - pending.size() % frame_bytes;
    if (usable == 0) continue;
    const std::size_t in_frames = usable / frame_bytes;
    resampled.clear();
    const auto rc = ctx.resampler->convert(reinterpret_cast<const int16_t*>(pending.data()),
                                           in_frames, resampled, err);
    pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(usable));
    if (!rc) {
      log_line("重采样失败（%s）：%s", cue::media::resample_error_name(rc.error()), err.c_str());
      break;
    }
    mono.insert(mono.end(), resampled.begin(), resampled.end());
    send_frames();
  }
  // 收流结束：排空重采样器内部延迟，否则末尾几十个样本会丢（20 ms 帧拼不满就丢，可接受，
  // 但延迟样本本身不排空会让"重采样器到底转换了多少"无法对账）。
  resampled.clear();
  std::string ferr;
  if (ctx.resampler->flush(resampled, ferr)) {
    mono.insert(mono.end(), resampled.begin(), resampled.end());
    send_frames();
  }
  if (!mono.empty()) {
    log_line("音频尾部不足一帧（%llu 样本）未发送", static_cast<unsigned long long>(mono.size()));
  }
}

// ---------------------------------------------------------------- 发送线程

void sender_loop(FrameQueue& q, WsSender& ws, Stats& st, uint32_t stream_id,
                 std::string& err_out) {
  EncodedFrame f;
  while (q.pop(f)) {
    // 每帧之前顺手取一次控制消息（非阻塞，见 WsSender::poll_control）：
    // 30 帧/秒的节奏下对关键帧请求的响应延迟不超过一帧。
    ws.poll_control();
    if (f.data.empty()) continue;
    if (!cue::proto::upstream_frame_fits(f.data.size())) {
      // 超过 1024 分片上限的帧对端会拒（wire.h 的 FragCountTooLarge）：发送端先丢并计数，
      // 不让"被对端拒"混进网络丢包的证据里。
      st.oversized.fetch_add(1, std::memory_order_relaxed);
      log_line("帧 %u 超过协议分片上限（%llu 字节），已丢弃", f.frame_id,
               static_cast<unsigned long long>(f.data.size()));
      continue;
    }
    cue::proto::UpstreamMedia m;
    m.type = static_cast<uint8_t>(cue::proto::MediaType::Video);
    m.codec = static_cast<uint8_t>(cue::proto::Codec::H264);
    m.stream_id = stream_id;
    m.frame_id = f.frame_id;
    m.pts_us = f.pts_us;
    m.keyframe = f.keyframe;
    const auto packets = cue::proto::pack_upstream(f.data, m);
    bool ok = true;
    std::string err;
    for (const auto& p : packets) {
      if (!ws.send_binary(p, err)) {
        ok = false;
        st.send_errors.fetch_add(1, std::memory_order_relaxed);
        err_out = err;
        g_stop = 1;  // WS 断了就没有继续采集的意义：本地缓存没有消费者，继续编码只是空转
        break;
      }
    }
    if (ok) st.frames_sent.fetch_add(1, std::memory_order_relaxed);
  }
}

// ---------------------------------------------------------------- 统计输出

struct StatsOutput {
  Stats* stats = nullptr;
  WsSender* sender = nullptr;
  std::string backend;
  std::string encode_mode;
  std::string source;
  const char* audio_state = "off";
};

// 每秒一行 JSON（前缀 CAPTURE）。键名与 cued 的 METRICS 口径对齐：
//   frames_in/frames_out/dropped 同名；encode_ms_p50/p99 是 cued 的 encode 段的同名量
//   （cued 侧是 {p50,p95,p99} 的嵌套对象，这里为了"一行一个读数"拍平成 encode_ms_*）；
//   bytes_sent 对应 cued 的 bytes_in（采集端视角是"发出去的字节"）。
void emit_metrics(const StatsOutput& out, uint64_t& prev_captured, uint64_t& prev_sent,
                  int64_t& prev_us) {
  const int64_t now = cue::now_us();
  const uint64_t captured = out.stats->frames_captured.load(std::memory_order_relaxed);
  const uint64_t sent = out.stats->frames_sent.load(std::memory_order_relaxed);
  const double dt = prev_us > 0 ? static_cast<double>(now - prev_us) / 1e6 : 0.0;
  const double fps_capture =
      dt > 0 ? static_cast<double>(captured - prev_captured) / dt : 0.0;
  const double fps_send = dt > 0 ? static_cast<double>(sent - prev_sent) / dt : 0.0;
  prev_captured = captured;
  prev_sent = sent;
  prev_us = now;

  const auto enc = out.stats->encode_samples();
  json m{{"capture_fps", std::round(fps_capture * 10.0) / 10.0},
         {"send_fps", std::round(fps_send * 10.0) / 10.0},
         {"frames_in", captured},
         {"frames_out", sent},
         {"dropped", out.stats->dropped.load(std::memory_order_relaxed)},
         {"encode_errors", out.stats->encode_errors.load(std::memory_order_relaxed)},
         {"send_errors", out.stats->send_errors.load(std::memory_order_relaxed)},
         {"oversized", out.stats->oversized.load(std::memory_order_relaxed)},
         {"encode_ms_p50", pct_or_null(enc, 0.50)},
         {"encode_ms_p99", pct_or_null(enc, 0.99)},
         {"encode_samples", enc.size()},
         {"bytes_sent", out.sender->bytes_sent()},
         {"audio_frames_out", out.stats->audio_frames_sent.load(std::memory_order_relaxed)},
         {"audio_bytes_sent", out.stats->audio_bytes_sent.load(std::memory_order_relaxed)},
         // 音频定拍重锚次数：非 0 说明这一秒里发送节奏被打断过（设备交付块过大
         // 或线程被调度走），是"音画同步又开始漂"的早期信号。
         {"audio_pace_resets", out.stats->audio_pace_resets.load(std::memory_order_relaxed)},
         {"audio_encode_errors", out.stats->audio_encode_errors.load(std::memory_order_relaxed)},
         {"backend", out.backend},
         {"encode", out.encode_mode},
         {"source", out.source},
         {"audio", out.audio_state},
         {"ws", out.sender->connected() ? "connected" : "closed"}};
  const std::string line = "CAPTURE " + m.dump();
  std::fwrite(line.data(), 1, line.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

// ---------------------------------------------------------------- V4L2

#if defined(__linux__)
// Linux V4L2 采集（mmap + 双缓冲）。为什么要求 count>=2：只有一个缓冲时，驱动在
// 我们把数据拷走之前无法写入下一帧，采集速率会被编码耗时直接卡住；两个缓冲才能
// 做到"编码 A 的同时驱动填 B"。
class V4l2Source {
 public:
  ~V4l2Source() { close(); }
  V4l2Source() = default;
  V4l2Source(const V4l2Source&) = delete;
  V4l2Source& operator=(const V4l2Source&) = delete;

  bool open(const std::string& dev, int width, int height, std::string& err) {
    close();
    fd_ = ::open(dev.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
      err = "打开 " + dev + " 失败: " + std::strerror(errno);
      return false;
    }
    v4l2_capability cap{};
    if (::ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
      err = std::string("VIDIOC_QUERYCAP: ") + std::strerror(errno);
      close();
      return false;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
      err = dev + " 不是视频采集设备";
      close();
      return false;
    }
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
      err = dev + " 不支持 streaming I/O（mmap 模式不可用）";
      close();
      return false;
    }
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = static_cast<uint32_t>(width);
    fmt.fmt.pix.height = static_cast<uint32_t>(height);
    // 只支持 YUYV：MJPEG 需要额外解码器，而本项目的"采集端不做解码"是刻意边界。
    // 驱动改成别的格式时如实报错，不静默换格式。
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (::ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
      err = std::string("VIDIOC_S_FMT: ") + std::strerror(errno);
      close();
      return false;
    }
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
      err = "驱动拒绝了 YUYV（实际格式 " + std::to_string(fmt.fmt.pix.pixelformat) + "）";
      close();
      return false;
    }
    if (static_cast<int>(fmt.fmt.pix.width) != width ||
        static_cast<int>(fmt.fmt.pix.height) != height) {
      err = "驱动协商到 " + std::to_string(fmt.fmt.pix.width) + "x" +
            std::to_string(fmt.fmt.pix.height) + "，与请求尺寸不一致";
      close();
      return false;
    }
    w_ = width;
    h_ = height;

    v4l2_requestbuffers rb{};
    rb.count = 2;
    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    rb.memory = V4L2_MEMORY_MMAP;
    if (::ioctl(fd_, VIDIOC_REQBUFS, &rb) < 0) {
      err = std::string("VIDIOC_REQBUFS: ") + std::strerror(errno);
      close();
      return false;
    }
    if (rb.count < 2) {
      err = "驱动只给了 " + std::to_string(rb.count) + " 个缓冲（需要 ≥2 才谈得上双缓冲）";
      close();
      return false;
    }
    nbuf_ = static_cast<int>(rb.count);
    for (int i = 0; i < nbuf_; ++i) {
      v4l2_buffer b{};
      b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      b.memory = V4L2_MEMORY_MMAP;
      b.index = static_cast<uint32_t>(i);
      if (::ioctl(fd_, VIDIOC_QUERYBUF, &b) < 0) {
        err = std::string("VIDIOC_QUERYBUF: ") + std::strerror(errno);
        close();
        return false;
      }
      void* p = ::mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_,
                       static_cast<off_t>(b.m.offset));
      if (p == MAP_FAILED) {
        err = std::string("mmap: ") + std::strerror(errno);
        close();
        return false;
      }
      bufs_[i].start = p;
      bufs_[i].len = b.length;
      if (::ioctl(fd_, VIDIOC_QBUF, &b) < 0) {
        err = std::string("VIDIOC_QBUF: ") + std::strerror(errno);
        close();
        return false;
      }
    }
    sws_ = sws_getContext(w_, h_, AV_PIX_FMT_YUYV422, w_, h_, AV_PIX_FMT_YUV420P,
                          SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_) {
      err = "sws_getContext(YUYV422→YUV420P) 失败";
      close();
      return false;
    }
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (::ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
      err = std::string("VIDIOC_STREAMON: ") + std::strerror(errno);
      close();
      return false;
    }
    streaming_ = true;
    return true;
  }

  enum class Read : uint8_t { Frame = 0, Timeout, Error };

  // 取一帧并转成 YUV420P 写进 out（out 由调用方分配好 w*h 的缓冲）。
  Read read_frame(AVFrame* out, std::string& err, int timeout_ms = 500) {
    pollfd p{fd_, POLLIN, 0};
    const int pr = ::poll(&p, 1, timeout_ms);
    if (pr == 0) return Read::Timeout;
    if (pr < 0) {
      if (errno == EINTR) return Read::Timeout;
      err = std::string("poll: ") + std::strerror(errno);
      return Read::Error;
    }
    v4l2_buffer b{};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    if (::ioctl(fd_, VIDIOC_DQBUF, &b) < 0) {
      if (errno == EAGAIN) return Read::Timeout;
      err = std::string("VIDIOC_DQBUF: ") + std::strerror(errno);
      return Read::Error;
    }
    const uint8_t* src_data[4] = {static_cast<const uint8_t*>(bufs_[b.index].start), nullptr,
                                  nullptr, nullptr};
    const int src_linesize[4] = {w_ * 2, 0, 0, 0};
    sws_scale(sws_, src_data, src_linesize, 0, h_, out->data, out->linesize);
    // QBUF 必须无条件执行：丢了它这片缓冲就再也不回来，几帧之后 DQBUF 会永久超时。
    if (::ioctl(fd_, VIDIOC_QBUF, &b) < 0) {
      err = std::string("VIDIOC_QBUF(回队): ") + std::strerror(errno);
      return Read::Error;
    }
    return Read::Frame;
  }

  void close() {
    if (fd_ >= 0) {
      if (streaming_) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        (void)::ioctl(fd_, VIDIOC_STREAMOFF, &type);
        streaming_ = false;
      }
      for (int i = 0; i < nbuf_; ++i) {
        if (bufs_[i].start) {
          ::munmap(bufs_[i].start, bufs_[i].len);
          bufs_[i].start = nullptr;
        }
      }
      nbuf_ = 0;
      if (sws_) {
        sws_freeContext(sws_);
        sws_ = nullptr;
      }
      ::close(fd_);
      fd_ = -1;
    }
  }

  int width() const { return w_; }
  int height() const { return h_; }

 private:
  struct Buf {
    void* start = nullptr;
    std::size_t len = 0;
  };
  int fd_ = -1;
  Buf bufs_[8]{};
  int nbuf_ = 0;
  int w_ = 0;
  int h_ = 0;
  bool streaming_ = false;
  SwsContext* sws_ = nullptr;
};
#endif  // __linux__

// ---------------------------------------------------------------- 采集主循环

// 采集时长窗口：从**第一帧真正到达**开始计时，而不是从进程开始。
// ffmpeg 子进程启动要 0.3–0.5 s（进程创建 + 编码器初始化），把它算进 --duration
// 会让 "8 s 采到 226 帧" 看起来像掉帧；口径应该是"采到第一帧之后的 N 秒"。
struct DurationWindow {
  int64_t start_us = 0;
  double seconds = 0.0;
  void mark_started() {
    if (start_us == 0) start_us = cue::now_us();
  }
  bool elapsed() const {
    if (start_us == 0 || seconds <= 0) return false;
    return static_cast<double>(cue::now_us() - start_us) >= seconds * 1e6;
  }
};

// 把一帧交给发送队列。keyframe / 帧号 / pts 在这里统一分配，三条后端共用。
struct FrameSink {
  FrameQueue* queue = nullptr;
  Stats* stats = nullptr;
  uint32_t stream_id = 1;
  uint32_t next_frame_id = 0;
  // cued 的单调时钟域（WsSender 负责 ping/pong 标定）。pts 必须在**采集/成帧**
  // 时刻取，不能在发送时刻取，否则编码与排队耗时会被算进端到端延迟。
  std::function<int64_t()> now;
  // 非空且 enabled 时用定拍时钟给 pts（合成源/文件源）；摄像头用 now()。
  FramePacer* pacer = nullptr;

  int64_t frame_pts() const {
    return (pacer != nullptr && pacer->enabled() && pacer->started()) ? pacer->pts() : now();
  }

  // 子进程编码路径专用：按 AU 定拍并返回 pts（见 FramePacer::pace_next）。
  int64_t pace_frame() { return pacer != nullptr ? pacer->pace_next(now()) : now(); }

  void emit(std::vector<uint8_t> data, bool keyframe, int64_t pts_us, int64_t encode_us) {
    stats->frames_captured.fetch_add(1, std::memory_order_relaxed);
    if (encode_us > 0) stats->add_encode_us(encode_us);
    EncodedFrame f;
    f.data = std::move(data);
    f.keyframe = keyframe;
    f.pts_us = static_cast<uint64_t>(pts_us);
    f.frame_id = next_frame_id++;
    if (!queue->push(std::move(f))) stats->dropped.fetch_add(1, std::memory_order_relaxed);
  }
};

// ---- 进程内编码：从子进程/设备拿到 yuv420p 帧 → libx264 ----
int capture_inproc(ChildProcess& video_src, X264Encoder& enc, const Options& o,
                   FrameSink& sink, bool& child_encode_done) {
  const std::size_t raw_size =
      static_cast<std::size_t>(o.width) * static_cast<std::size_t>(o.height) * 3 / 2;
  std::vector<uint8_t> raw(raw_size);
  std::size_t raw_filled = 0;
  AVFrame* frame = av_frame_alloc();
  if (!frame) {
    log_line("av_frame_alloc 失败");
    return 1;
  }
  frame->format = AV_PIX_FMT_YUV420P;
  frame->width = o.width;
  frame->height = o.height;
  if (av_frame_get_buffer(frame, 32) < 0) {
    log_line("av_frame_get_buffer 失败");
    av_frame_free(&frame);
    return 1;
  }
  int rc = 0;
  DurationWindow window{0, o.duration_s};
  while (g_stop == 0 && !window.elapsed()) {
    if (sink.pacer) sink.pacer->before_frame();
    std::size_t got = 0;
    const IoStatus st = video_src.read_exact(raw.data() + raw_filled, raw_size - raw_filled, got, 500);
    raw_filled += got;
    if (st == IoStatus::Timeout) continue;
    if (st == IoStatus::Error) {
      log_line("视频源读取失败");
      rc = 1;
      break;
    }
    if (st == IoStatus::Eof) {
      if (raw_filled > 0) log_line("视频源 EOF（尾帧不完整：%lld/%lld 字节，丢弃）", static_cast<long long>(raw_filled),
               static_cast<long long>(raw_size));
      child_encode_done = true;
      break;
    }
    raw_filled = 0;
    window.mark_started();
    if (sink.pacer) sink.pacer->note_frame_arrived(cue::now_us());
    // rawvideo 是紧密排列的 yuv420p；AVFrame 的 planes 可能带行对齐填充，逐行拷。
    const std::size_t y_size = static_cast<std::size_t>(o.width) * o.height;
    const std::size_t uv_size = y_size / 4;
    const uint8_t* src_y = raw.data();
    const uint8_t* src_u = raw.data() + y_size;
    const uint8_t* src_v = raw.data() + y_size + uv_size;
    for (int y = 0; y < o.height; ++y) {
      std::memcpy(frame->data[0] + static_cast<std::ptrdiff_t>(y) * frame->linesize[0],
                  src_y + static_cast<std::size_t>(y) * o.width,
                  static_cast<std::size_t>(o.width));
    }
    for (int y = 0; y < o.height / 2; ++y) {
      std::memcpy(frame->data[1] + static_cast<std::ptrdiff_t>(y) * frame->linesize[1],
                  src_u + static_cast<std::size_t>(y) * (o.width / 2),
                  static_cast<std::size_t>(o.width / 2));
      std::memcpy(frame->data[2] + static_cast<std::ptrdiff_t>(y) * frame->linesize[2],
                  src_v + static_cast<std::size_t>(y) * (o.width / 2),
                  static_cast<std::size_t>(o.width / 2));
    }
    // pts 取「这一帧被采集的时刻」：合成源走定拍时钟（见 FramePacer），摄像头走到达时刻。
    // 两种情况都发生在编码之前，端到端延迟里因此包含编码耗时（与浏览器"送进编码器
    // 那一刻"的口径一致）。
    const int64_t pts = sink.frame_pts();
    int64_t enc_us = 0;
    std::string err;
    // 接收端请求了关键帧：在**这一帧**插 IDR（见 g_want_idr 的注释）。
    if (g_want_idr.exchange(false, std::memory_order_relaxed)) enc.force_keyframe();
    auto out = enc.encode(frame, pts, err, &enc_us);
    if (!out) {
      if (out.error() == cue::media::EncodeError::EncoderDelay) continue;
      sink.stats->encode_errors.fetch_add(1, std::memory_order_relaxed);
      log_line("编码失败（%s）：%s", cue::media::encode_error_name(out.error()), err.c_str());
      continue;
    }
    sink.emit(std::move(out->data), out->keyframe, pts, enc_us);
  }
  av_frame_free(&frame);
  return rc;
}

// ---- 子进程编码：读 Annex-B 裸流 → 按 AUD 分帧（encode_ms 如实写 null） ----
int capture_annexb(ChildProcess& video_src, const Options& o, FrameSink& sink,
                   bool& child_encode_done) {
  AnnexBAuAssembler assembler;
  std::vector<uint8_t> chunk(64 * 1024);
  auto handle_au = [&](std::vector<uint8_t> au) {
    if (au.empty()) return;
    bool keyframe = false;
    for (const auto& n : cue::media::split_annexb(au)) {
      if (n.type() == cue::media::kNalTypeIdr) {
        keyframe = true;
        break;
      }
    }
    // 子进程编码路径没有进程内编码耗时可测：encode_ms_* 如实写 null，
    // 而不是拿"解析耗时"冒充。pts 按 AU 定拍（见 FramePacer::pace_next）。
    sink.emit(std::move(au), keyframe, sink.pace_frame(), 0);
  };
  DurationWindow window{0, o.duration_s};
  while (g_stop == 0 && !window.elapsed()) {
    std::size_t got = 0;
    const IoStatus st = video_src.read_some(chunk.data(), chunk.size(), got, 500);
    if (st == IoStatus::Timeout) continue;
    if (st == IoStatus::Error) {
      log_line("视频源读取失败");
      return 1;
    }
    if (st == IoStatus::Eof) {
      child_encode_done = true;
      break;
    }
    window.mark_started();
    auto aus = assembler.feed(std::span<const uint8_t>(chunk.data(), got));
    if (!aus) {
      log_line("Annex-B 分帧失败（%s）：ffmpeg 输出里没有 AUD；--encode ffmpeg 需要 "
               "-x264-params aud=1（本程序已默认加）",
               cue::media::au_assembler_error_name(aus.error()));
      return 1;
    }
    for (auto& au : *aus) handle_au(std::move(au));
  }
  for (auto& au : assembler.flush()) handle_au(std::move(au));
  return 0;
}

#if defined(__linux__)
// ---- V4L2：mmap 双缓冲 → sws 转 yuv420p → libx264 ----
int capture_v4l2(V4l2Source& src, X264Encoder& enc, const Options& o, FrameSink& sink) {
  AVFrame* frame = av_frame_alloc();
  if (!frame) return 1;
  frame->format = AV_PIX_FMT_YUV420P;
  frame->width = o.width;
  frame->height = o.height;
  if (av_frame_get_buffer(frame, 32) < 0) {
    av_frame_free(&frame);
    return 1;
  }
  int rc = 0;
  DurationWindow window{0, o.duration_s};
  while (g_stop == 0 && !window.elapsed()) {
    std::string err;
    const auto st = src.read_frame(frame, err, 500);
    if (st == V4l2Source::Read::Timeout) continue;
    if (st == V4l2Source::Read::Error) {
      log_line("V4L2 读帧失败：%s", err.c_str());
      rc = 1;
      break;
    }
    window.mark_started();
    const int64_t pts = sink.frame_pts();
    int64_t enc_us = 0;
    // 接收端请求了关键帧：在**这一帧**插 IDR（见 g_want_idr 的注释）。
    if (g_want_idr.exchange(false, std::memory_order_relaxed)) enc.force_keyframe();
    auto out = enc.encode(frame, pts, err, &enc_us);
    if (!out) {
      if (out.error() == cue::media::EncodeError::EncoderDelay) continue;
      sink.stats->encode_errors.fetch_add(1, std::memory_order_relaxed);
      log_line("编码失败（%s）：%s", cue::media::encode_error_name(out.error()), err.c_str());
      continue;
    }
    sink.emit(std::move(out->data), out->keyframe, pts, enc_us);
  }
  av_frame_free(&frame);
  return rc;
}
#endif

// ---------------------------------------------------------------- 协议样例转储

// 给 scripts/compare-upstream-wire.mjs 用：用**生产代码**（proto::pack_upstream）
// 对固定输入打包，输出 hex，供与 JS 实现逐字节对照。meta 一并写出，作为对照的输入。
int dump_wire_examples(const std::string& path) {
  struct Case {
    const char* name;
    std::size_t payload_len;
    cue::proto::UpstreamMedia meta;
  };
  std::vector<Case> cases;
  auto video_meta = [](uint32_t frame_id, uint64_t pts, bool keyframe) {
    cue::proto::UpstreamMedia m;
    m.type = static_cast<uint8_t>(cue::proto::MediaType::Video);
    m.codec = static_cast<uint8_t>(cue::proto::Codec::H264);
    m.stream_id = 1;
    m.frame_id = frame_id;
    m.pts_us = pts;
    m.keyframe = keyframe;
    return m;
  };
  cases.push_back({"empty", 0, video_meta(7, 1234567890123ULL, true)});
  cases.push_back({"one-byte", 1, video_meta(7, 1234567890123ULL, true)});
  cases.push_back({"frag-1", 16383, video_meta(7, 1234567890123ULL, true)});
  cases.push_back({"frag-exact", 16384, video_meta(7, 1234567890123ULL, true)});
  cases.push_back({"frag-plus-one", 16385, video_meta(7, 1234567890123ULL, true)});
  cases.push_back({"frag-four", 49157, video_meta(7, 1234567890123ULL, true)});
  cases.push_back({"non-keyframe", 20000, video_meta(0xFFFFFFFFu, 0, false)});
  {
    cue::proto::UpstreamMedia m;
    m.type = static_cast<uint8_t>(cue::proto::MediaType::Audio);
    m.codec = static_cast<uint8_t>(cue::proto::Codec::Opus);
    m.stream_id = 2;
    m.frame_id = 91;
    m.pts_us = 42;
    m.keyframe = true;
    cases.push_back({"audio", 300, m});
  }

  std::string out;
  for (const auto& c : cases) {
    std::vector<uint8_t> payload(c.payload_len, 0xA5);
    const auto packets = cue::proto::pack_upstream(payload, c.meta);
    json head{{"case", c.name},
              {"payload_len", c.payload_len},
              {"payload_fill", "a5"},
              {"frag_count", packets.empty() ? 0 : packets.size()},
              {"meta", {{"type", static_cast<int>(c.meta.type)},
                        {"codec", static_cast<int>(c.meta.codec)},
                        {"stream_id", c.meta.stream_id},
                        {"frame_id", c.meta.frame_id},
                        {"pts_us", c.meta.pts_us},
                        {"keyframe", c.meta.keyframe}}}};
    json hexes = json::array();
    for (const auto& p : packets) {
      static const char* kHex = "0123456789abcdef";
      std::string h;
      h.reserve(p.size() * 2);
      for (uint8_t b : p) {
        h.push_back(kHex[b >> 4]);
        h.push_back(kHex[b & 0x0F]);
      }
      hexes.push_back(h);
    }
    head["packets"] = std::move(hexes);
    out += head.dump();
    out.push_back('\n');
  }
  if (path == "-") {
    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fflush(stdout);
    return 0;
  }
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    log_line("无法写 %s: %s", path.c_str(), std::strerror(errno));
    return 1;
  }
  std::fwrite(out.data(), 1, out.size(), f);
  std::fclose(f);
  log_line("协议样例已写入 %s（%llu 个 case）", path.c_str(),
           static_cast<unsigned long long>(cases.size()));
  return 0;
}

// ---------------------------------------------------------------- 主流程

int run(const Options& o) {
  if (o.backend != "lavfi" && o.backend != "ffmpeg" && o.backend != "v4l2") {
    log_line("未知后端：%s（可选 lavfi|ffmpeg|v4l2）", o.backend.c_str());
    return 2;
  }
  const HostPlatform platform = host_platform();
  if (o.backend == "ffmpeg" && platform == HostPlatform::Linux) {
    log_line("backend=ffmpeg（dshow/avfoundation）只在 Windows/macOS 有意义；"
             "Linux 请用 --backend v4l2 或 --backend lavfi");
    return 2;
  }
  if (o.backend == "v4l2" && platform != HostPlatform::Linux) {
    log_line("backend=v4l2 只在 Linux 上可用");
    return 2;
  }

  CaptureSource source = CaptureSource::Device;
  if (!o.input.empty()) {
    source = CaptureSource::File;
  } else if (o.backend == "lavfi") {
    source = CaptureSource::Lavfi;
  }
  const bool inproc = o.encode == "inproc";
  const int level_idc =
      o.avc_level > 0 ? o.avc_level : cue::media::pick_avc_level(o.width, o.height);
  if (level_idc == 0) {
    log_line("尺寸 %dx%d 超过 1080p：浏览器侧 pickCodec 会拒绝，采集端退回 x264 自动 level",
             o.width, o.height);
  }

  Stats stats;
  FrameQueue queue(8);
  WsSender sender(o.ws_url);
  std::string err;

  log_line("backend=%s source=%s encode=%s %dx%d@%d %dkbps gop=%d preset=%s level=%s",
           o.backend.c_str(),
           source == CaptureSource::Lavfi ? "lavfi"
                                         : (source == CaptureSource::File ? "file" : "device"),
           o.encode.c_str(), o.width, o.height, o.fps, o.bitrate_kbps, o.gop, o.preset.c_str(),
           cue::media::avc_level_name(level_idc).c_str());

  // superfast 不拒绝（它的 deblock/cabac/aq 都还开着，实测 deblock=1:0:0 cabac=1 aq=1），
  // 但 analyse=0x3:0x3 + me=dia + subme=1 + mbtree=0 仍然把码率效率压得很低：同码率下
  // 比 veryfast 更容易在平坦区看到块效应。留一行，免得把它当成"和 veryfast 差不多"。
  if (o.preset == "superfast") {
    log_line("preset=superfast 低于建议下限 veryfast（分区/运动搜索/子像素都降级、mbtree 关），"
             "同码率画质会明显差一档；只有这台机器跑不动 veryfast 时才用它");
  }

  // 连接重试：demo-min 里 cued 比采集端早 0.3 s 起，但启动顺序不是契约。
  {
    const int64_t deadline = cue::now_us() + 5'000'000;
    for (;;) {
      if (sender.connect(err)) break;
      if (cue::now_us() > deadline) {
        log_line("连接 cued 失败：%s", err.c_str());
        return 1;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
  }
  log_line("已连接 %s（clock offset %+.2f ms）", o.ws_url.c_str(),
           static_cast<double>(sender.offset_us()) / 1000.0);

  // 视频编码器：进程内编码才打开 x264；子进程编码路径由 ffmpeg 负责。
  X264Encoder encoder;
  if (inproc) {
    std::string eerr;
    if (!encoder.open(o.width, o.height, o.bitrate_kbps, o.fps, o.gop, o.preset, eerr, o.threads,
                      level_idc)) {
      log_line("x264 打开失败：%s", eerr.c_str());
      return 1;
    }
  }

  // 音频：只有 lavfi/ffmpeg 后端有音频源；v4l2 只有视频（服务器无 /dev/snd，
  // 摄像头音频在 Linux 上是另一条 ALSA 路径，本任务不假装采到）。
  const bool audio_wanted = o.audio;
  bool audio_available = audio_wanted && o.backend != "v4l2";
  if (audio_wanted && o.backend == "v4l2") {
    log_line("backend=v4l2 没有音频采集路径（V4L2 只出视频帧），本次只发视频");
  }
  AudioResampler resampler;
  OpusAudioEncoder opus;
  if (audio_available) {
    cue::media::ResampleSpec spec;
    spec.in_rate = o.audio_rate;
    spec.in_channels = o.audio_channels;
    spec.out_rate = OpusAudioEncoder::kSampleRate;
    spec.out_channels = 1;
    std::string aerr;
    if (!resampler.open(spec, aerr)) {
      log_line("重采样器初始化失败：%s", aerr.c_str());
      return 1;
    }
    if (!opus.open(aerr)) {
      log_line("Opus 编码器初始化失败：%s", aerr.c_str());
      return 1;
    }
    // 弱网参数在 open 之后设置（open 会把之前存的值一并下发，这里再设一次是为了
    // 让"CLI 值 → 编码器实际值"这一段在代码上显式可见，而不是靠成员初值隐式传递）。
    if (!opus.set_packet_loss_perc(o.opus_loss_perc) || !opus.set_dtx(o.opus_dtx)) {
      log_line("Opus 弱网参数非法（--opus-loss-perc 需 0..100）");
      return 1;
    }
    // 打印**实际生效值**而不是 CLI 值：fec_enabled() 是"开关打开且 loss perc > 0"，
    // 所以 --opus-loss-perc 0 会把 fec 显示成 off（libopus 在 loss=0 时不产生 LBRR，
    // 两条口径必须一致，否则日志会撒谎）。
    log_line("音频：%d Hz/%d ch s16 → swr_convert → 48 kHz mono → libopus %d ms %d bps"
             "（in-band fec=%s loss_perc=%d dtx=%s）",
             o.audio_rate, o.audio_channels, OpusAudioEncoder::kFrameMs,
             OpusAudioEncoder::kBitrateBps, opus.fec_enabled() ? "on" : "off",
             opus.packet_loss_perc(), opus.dtx_enabled() ? "on" : "off");
  }

  // 视频子进程（v4l2 不用子进程）
  ChildProcess video_src;
  ChildProcess audio_src;
  if (o.backend != "v4l2") {
    FfmpegArgsSpec vs;
    vs.source = source;
    vs.platform = platform;
    vs.kind = DeviceKind::Video;
    vs.encode = inproc ? VideoEncodeMode::InProcess : VideoEncodeMode::FfmpegChild;
    vs.ffmpeg_bin = o.ffmpeg_bin;
    vs.input = o.input;
    vs.device = o.device;
    vs.lavfi = "testsrc2=size=" + std::to_string(o.width) + "x" + std::to_string(o.height) +
               ":rate=" + std::to_string(o.fps);
    vs.width = o.width;
    vs.height = o.height;
    vs.fps = o.fps;
    vs.bitrate_kbps = o.bitrate_kbps;
    vs.gop = o.gop;
    vs.level_idc = level_idc;
    vs.preset = o.preset;
    const auto argv = cue::media::build_ffmpeg_argv(vs);
    if (argv.empty()) {
      log_line("无法为 backend=%s 构造 ffmpeg 命令行", o.backend.c_str());
      return 2;
    }
    const int pipe_bytes = cue::capture::video_pipe_bytes(platform, vs.encode, vs.width, vs.height);
    if (!video_src.start(argv, err, pipe_bytes)) {
      log_line("启动视频源失败：%s", err.c_str());
      return 1;
    }
    if (audio_available) {
      FfmpegArgsSpec as;
      as.source = source;
      as.platform = platform;
      as.kind = DeviceKind::Audio;
      as.ffmpeg_bin = o.ffmpeg_bin;
      as.input = o.input;
      as.device = o.audio_device.empty() ? o.device : o.audio_device;
      as.audio_rate = o.audio_rate;
      as.audio_channels = o.audio_channels;
      const auto aargv = cue::media::build_ffmpeg_argv(as);
      if (!aargv.empty() && audio_src.start(aargv, err)) {
        log_line("音频源子进程已启动");
      } else {
        // 没有音频源不是致命错误：视频主线照跑，但如实把状态写进指标。
        audio_available = false;
        log_line("音频源启动失败（%s），本次只发视频", err.c_str());
      }
    }
  }

  const char* audio_state = !o.audio ? "off" : (audio_available ? "on" : "skipped");
  StatsOutput out;
  out.stats = &stats;
  out.sender = &sender;
  out.backend = o.backend;
  out.encode_mode = o.encode;
  out.source = source == CaptureSource::Lavfi ? "lavfi"
                                              : (source == CaptureSource::File ? "file" : "device");
  out.audio_state = audio_state;

  std::atomic<bool> audio_failed{false};
  AudioContext actx;
  actx.source = &audio_src;
  actx.resampler = &resampler;
  actx.opus = &opus;
  actx.sender = &sender;
  actx.stats = &stats;
  actx.stream_id = 2;
  actx.channels = o.audio_channels;
  actx.audio_failed = &audio_failed;

  std::thread audio_thread;
  if (audio_available) audio_thread = std::thread(audio_loop, actx);

  std::string sender_err;
  std::thread send_thread(sender_loop, std::ref(queue), std::ref(sender), std::ref(stats),
                          uint32_t{1}, std::ref(sender_err));

  std::atomic<bool> stats_stop{false};
  std::thread stats_thread([&] {
    uint64_t prev_captured = 0;
    uint64_t prev_sent = 0;
    int64_t prev_us = cue::now_us();
    while (!stats_stop.load(std::memory_order_relaxed)) {
      for (int i = 0; i < 10 && !stats_stop.load(std::memory_order_relaxed); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (stats_stop.load(std::memory_order_relaxed)) break;
      emit_metrics(out, prev_captured, prev_sent, prev_us);
    }
  });

  // 合成源/文件源按 fps 定拍；摄像头由设备定拍（理由见 FramePacer）。
  FramePacer pacer(o.fps, source != CaptureSource::Device);
  FrameSink sink;
  sink.queue = &queue;
  sink.stats = &stats;
  sink.stream_id = 1;
  sink.now = [&sender] { return sender.now_us(); };
  sink.pacer = &pacer;
  bool child_done = false;
  int rc = 0;
#if defined(__linux__)
  V4l2Source v4l2;
  if (o.backend == "v4l2") {
    std::string verr;
    if (!v4l2.open(o.device, o.width, o.height, verr)) {
      log_line("V4L2 打开失败：%s", verr.c_str());
      g_stop = 1;
      rc = 1;
    } else {
      log_line("V4L2 已就绪：%s %dx%d mmap 双缓冲", o.device.c_str(), v4l2.width(),
               v4l2.height());
      rc = capture_v4l2(v4l2, encoder, o, sink);
    }
  }
#endif
  if (o.backend != "v4l2" && rc == 0) {
    if (inproc) {
      rc = capture_inproc(video_src, encoder, o, sink, child_done);
    } else {
      rc = capture_annexb(video_src, o, sink, child_done);
    }
  }

  // 收尾：先让队列排空，再停线程/子进程。顺序反了会丢最后几帧——那不是"网络问题"，
  // 是退出路径自己丢的。
  g_stop = 1;
  video_src.stop();
  // read_some is bounded, so the sole reader can finish before its handle is closed.
  audio_src.stop_after_reader(audio_thread);
  queue.stop();
  send_thread.join();
  stats_stop.store(true, std::memory_order_relaxed);
  stats_thread.join();
  encoder.close();

  {
    uint64_t prev_captured = 0;
    uint64_t prev_sent = 0;
    int64_t prev_us = 0;
    emit_metrics(out, prev_captured, prev_sent, prev_us);
  }

  const uint64_t captured = stats.frames_captured.load(std::memory_order_relaxed);
  const uint64_t sent = stats.frames_sent.load(std::memory_order_relaxed);
  log_line("结束：采集 %llu 帧，发出 %llu 帧，丢 %llu 帧，编码错误 %llu，发送错误 %llu，"
           "音频 %llu 包，发出 %llu 字节；%s",
           static_cast<unsigned long long>(captured), static_cast<unsigned long long>(sent),
           static_cast<unsigned long long>(stats.dropped.load(std::memory_order_relaxed)),
           static_cast<unsigned long long>(stats.encode_errors.load(std::memory_order_relaxed)),
           static_cast<unsigned long long>(stats.send_errors.load(std::memory_order_relaxed)),
           static_cast<unsigned long long>(stats.audio_frames_sent.load(std::memory_order_relaxed)),
           static_cast<unsigned long long>(sender.bytes_sent()),
           child_done ? "源已 EOF" : "到时/中断");
  if (!sender_err.empty()) log_line("发送线程错误：%s", sender_err.c_str());
  if (captured == 0) {
    log_line("一帧都没采到：检查后端参数与设备");
    return rc == 0 ? 1 : rc;
  }
  return rc;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
  // Windows 没有 SIGPIPE（写已关闭的 socket 由异步错误而不是信号体现），
  // 也没有 SIGTERM；Ctrl-C 用 CRT 的 signal 处理就够了（SetConsoleCtrlHandler 更完整，
  // 但这里只需要「能优雅停」）。
  // 用全局 ::signal 而不是 std::signal：MinGW 的 <signal.h> 只把符号放进全局命名空间。
  ::signal(SIGINT, on_signal);
#else
  ::signal(SIGINT, on_signal);
  ::signal(SIGTERM, on_signal);
  ::signal(SIGPIPE, SIG_IGN);
#endif

  Options o;
  std::string err;
  if (!parse_options(argc, argv, o, err)) {
    std::fprintf(stderr, "[cue-capture] %s\n\n", err.c_str());
    print_usage();
    return 2;
  }
  if (!o.dump_wire.empty()) return dump_wire_examples(o.dump_wire);
  return run(o);
}
