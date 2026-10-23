// ffmpeg 子进程的命令行构造（纯函数，不起进程）。
//
// 为什么要单独抽出来：`dshow` / `avfoundation` 两条后端只能在 Windows / macOS 上跑，
// 本仓库的 CI 是 Linux，服务器也没有摄像头。如果把这两条后端的参数拼装散落在
// `#ifdef _WIN32` / `#ifdef __APPLE__` 分支里，Linux 上连语法都不会被检查到。
// 这里按 HostPlatform 枚举拼参数，Linux 上照样编译、照样被 tests/test_capture.cpp
// 断言「dshow 的命令行里出现了 video=<设备名>」—— 能验证的部分就不再是"只有代码"。
//
// 仍然无法验证的是：目标平台上 ffmpeg 是否真的这么认设备名、权限/驱动是否就绪。
// 这一层必须如实标注为未在目标平台验证。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cue::media {

enum class HostPlatform : uint8_t { Linux = 0, Windows, macOS };
enum class DeviceKind : uint8_t { Video = 0, Audio };
enum class CaptureSource : uint8_t { Lavfi = 0, File, Device };
enum class VideoEncodeMode : uint8_t { InProcess = 0, FfmpegChild };

// 构造 ffmpeg argv 需要的全部输入。字段与 apps/cue-capture.cpp 的 Options 对齐，
// 但刻意不依赖它（否则测试要为了一个参数拼装把整个 app 的头文件拖进来）。
struct FfmpegArgsSpec {
  CaptureSource source = CaptureSource::Lavfi;
  HostPlatform platform = HostPlatform::Linux;
  DeviceKind kind = DeviceKind::Video;
  VideoEncodeMode encode = VideoEncodeMode::InProcess;

  std::string ffmpeg_bin = "ffmpeg";
  std::string input;   // source == File
  std::string device;  // source == Device（dshow 设备名 / avfoundation 索引）
  std::string lavfi;   // source == Lavfi 的视频源，默认 testsrc2

  int width = 640;
  int height = 360;
  int fps = 30;
  int bitrate_kbps = 1200;
  int gop = 60;
  int level_idc = 30;  // AVC level ×10：30 = L3.0，40 = L4.0
  std::string preset = "veryfast";
  int audio_rate = 44100;  // 源 PCM 采样率：故意不一定等于 48000，让 swr 真的做重采样
  int audio_channels = 2;
};

// 把整数 AVC level 写成 ffmpeg 的 -level 参数（30 → "3.0"）。
inline std::string avc_level_name(int level_idc) {
  if (level_idc <= 0) return "auto";
  const int major = level_idc / 10;
  const int minor = level_idc % 10;
  return std::to_string(major) + "." + std::to_string(minor);
}

// 返回空 vector 表示「这个平台/来源组合不支持」，调用方据此给出人读错误，
// 而不是喂给 execvp 一个半截命令。
inline std::vector<std::string> build_ffmpeg_argv(const FfmpegArgsSpec& s) {
  std::vector<std::string> a;
  a.push_back(s.ffmpeg_bin);
  a.push_back("-hide_banner");
  a.push_back("-loglevel");
  a.push_back("error");
  a.push_back("-nostdin");

  // ---- 输入 ----
  if (s.kind == DeviceKind::Video) {
    if (s.source == CaptureSource::Lavfi) {
      // -re 按源帧率放送：合成源不节流的话管道里会瞬间灌满整段，测试就变成"批量转码"，
      // 既测不到采集节奏也测不到实时链路的背压。
      a.push_back("-re");
      a.push_back("-f");
      a.push_back("lavfi");
      a.push_back("-i");
      a.push_back(s.lavfi.empty() ? "testsrc2" : s.lavfi);
    } else if (s.source == CaptureSource::File) {
      if (s.input.empty()) return {};
      a.push_back("-re");
      a.push_back("-i");
      a.push_back(s.input);
    } else {
      if (s.device.empty()) return {};
      if (s.platform == HostPlatform::Windows) {
        a.push_back("-f");
        a.push_back("dshow");
        a.push_back("-framerate");
        a.push_back(std::to_string(s.fps));
        a.push_back("-video_size");
        a.push_back(std::to_string(s.width) + "x" + std::to_string(s.height));
        a.push_back("-i");
        a.push_back("video=" + s.device);
      } else if (s.platform == HostPlatform::macOS) {
        a.push_back("-f");
        a.push_back("avfoundation");
        a.push_back("-framerate");
        a.push_back(std::to_string(s.fps));
        a.push_back("-video_size");
        a.push_back(std::to_string(s.width) + "x" + std::to_string(s.height));
        a.push_back("-i");
        // avfoundation 的视频/音频用 "video:audio" 索引；只采视频时是 "N"。
        a.push_back(s.device);
      } else {
        return {};  // Linux 用 --backend v4l2，不走 dshow/avfoundation
      }
    }
    // ---- 输出：进程内编码（rawvideo）或交给子进程编码（h264 裸流）----
    a.push_back("-an");
    if (s.encode == VideoEncodeMode::FfmpegChild) {
      a.push_back("-c:v");
      a.push_back("libx264");
      a.push_back("-preset");
      a.push_back(s.preset.empty() ? "veryfast" : s.preset);
      a.push_back("-tune");
      a.push_back("zerolatency");
      a.push_back("-b:v");
      a.push_back(std::to_string(s.bitrate_kbps) + "k");
      a.push_back("-g");
      a.push_back(std::to_string(s.gop));
      a.push_back("-pix_fmt");
      a.push_back("yuv420p");
      a.push_back("-profile:v");
      a.push_back("high");
      if (s.level_idc > 0) {
        a.push_back("-level");
        a.push_back(avc_level_name(s.level_idc));
      }
      // aud=1：每个访问单元以 AUD(9) 开头，采集端据此分帧（AnnexBAuAssembler 的唯一判据）
      // repeat-headers=1：每个 IDR 前重发 SPS/PPS。**这是丢包后能否自愈的唯一依赖**：
      //   接收端一旦错过参数集，就再也解不出任何一帧（实测：probe 每帧报
      //   send_packet failed: -1094995529，画面永久冻住）。有了它，最坏也只需等到
      //   下一个 IDR（由 -g 决定，默认 30 → 1 秒）就能恢复。
      //   注意拼写：x264 只认 `repeat-headers`（短横线）。写成 `repeat_headers`
      //   **不会报错、会被静默忽略**，看起来"已经开了"，实际生效的是 x264 默认值——
      //   把这种关键能力建立在别人的默认值上，等于靠运气。
      // scenecut=0：GOP 严格按 -g 走，probe 侧的"关键帧间隔"才可预测
      a.push_back("-x264-params");
      a.push_back("aud=1:repeat-headers=1:scenecut=0");
      a.push_back("-f");
      a.push_back("h264");
    } else {
      a.push_back("-f");
      a.push_back("rawvideo");
      a.push_back("-pix_fmt");
      a.push_back("yuv420p");
      // -s 与设备/文件尺寸不一致时 ffmpeg 会自动插 scale；rawvideo 没有容器记尺寸，
      // 采集端按 w*h*3/2 定长读，所以这里必须显式写死输出尺寸。
      a.push_back("-s");
      a.push_back(std::to_string(s.width) + "x" + std::to_string(s.height));
    }
    a.push_back("pipe:1");
    return a;
  }

  // ---- 音频：一律输出 s16le 交错原始 PCM，重采样在采集端用 libswresample 做 ----
  if (s.source == CaptureSource::Lavfi) {
    // 音频同样要 -re：合成源不节流的话，PCM 管道会被瞬间灌满，采集端会以"越快越好"
    // 的节奏发出 50 包/秒的 Opus，pts 与真实时间脱钩（端到端延迟指标直接失效）。
    a.push_back("-re");
    a.push_back("-f");
    a.push_back("lavfi");
    a.push_back("-i");
    a.push_back("sine=frequency=440:sample_rate=" + std::to_string(s.audio_rate));
  } else if (s.source == CaptureSource::File) {
    if (s.input.empty()) return {};
    a.push_back("-re");
    a.push_back("-i");
    a.push_back(s.input);
    a.push_back("-vn");
    // 可选映射：文件里没有音轨时不报错，采集端从 EOF 得到"没有音频"这个事实
    a.push_back("-map");
    a.push_back("0:a:0?");
  } else {
    if (s.device.empty()) return {};
    if (s.platform == HostPlatform::Windows) {
      a.push_back("-f");
      a.push_back("dshow");
      a.push_back("-i");
      a.push_back("audio=" + s.device);
    } else if (s.platform == HostPlatform::macOS) {
      a.push_back("-f");
      a.push_back("avfoundation");
      a.push_back("-i");
      // "0:1" → 音频索引 1；"0" → 音频索引 0（":0"）。
      const std::size_t colon = s.device.find(':');
      const std::string audio_index =
          colon == std::string::npos ? s.device : s.device.substr(colon + 1);
      a.push_back(":" + audio_index);
    } else {
      return {};
    }
  }
  a.push_back("-ac");
  a.push_back(std::to_string(s.audio_channels));
  a.push_back("-ar");
  a.push_back(std::to_string(s.audio_rate));
  a.push_back("-f");
  a.push_back("s16le");
  a.push_back("pipe:1");
  return a;
}

// Windows 的 spawn 走 _popen(命令串)，需要把 argv 拼成命令行；引号规则按 MSVC CRT：
// 含空格或引号的参数整体加双引号，内部的引号/反斜杠按 CRT 的解析规则转义。
inline std::string build_windows_command_line(const std::vector<std::string>& argv) {
  std::string out;
  for (const auto& arg : argv) {
    if (!out.empty()) out.push_back(' ');
    const bool need_quote = arg.empty() || arg.find_first_of(" \t\"") != std::string::npos;
    if (!need_quote) {
      out += arg;
      continue;
    }
    out.push_back('"');
    std::size_t backslashes = 0;
    for (char c : arg) {
      if (c == '\\') {
        ++backslashes;
        continue;
      }
      if (c == '"') {
        out.append(backslashes * 2 + 1, '\\');
        out.push_back('"');
        backslashes = 0;
        continue;
      }
      out.append(backslashes, '\\');
      backslashes = 0;
      out.push_back(c);
    }
    out.append(backslashes * 2, '\\');  // 引号前的反斜杠也要翻倍
    out.push_back('"');
  }
  return out;
}

}  // namespace cue::media
