// T-31：H.264 解码器的内存契约单测（FR-DEV.4 / FR-PROBE.9 的回归防线）。
//
// 这一组存在的理由是一个真实事故：H264Decoder::decode 旧实现把上一张输出帧的引用
// 一直留到**下一次 drain** 才释放，而 avcodec_send_packet 在这之前就会对"调用方仍持有
// 的帧"再取一次引用，解码器内部的帧池因此永远回收不到那批缓冲。泄漏量正好是一帧 YUV：
//   · 1080p ≈ 3.1 MB/帧 → 30 fps 下 92 MB/s，cued 在 1970 MB 的机器上 7 秒涨 666 MB；
//   · 后果是 cgroup OOM 杀掉 cued（dmesg: anon-rss 1.35 GB）→ WS 断开 → 采集端退出 →
//     用户看到的是"画面和声音突然冻住"。
// 只断言"解出了帧、没有错误"是抓不到这个缺陷的：出问题时 decoded 一直在涨、
// decode_errors 一直是 0，指标看起来完全健康。所以这里直接量 RSS 增长。
//
// 用反复解码同一张 IDR 的方式复现：每帧都是关键帧，解码器每包必出一帧，
// 帧数可控、不依赖编码器、不需要外部样本文件。
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/frame.h>  // 断言要用到 AVFrame 的 width/height/data 字段
}

#include "cue/media/h264_decoder.h"

namespace {

int failures = 0;

void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

// 64x64 单帧 Annex-B IDR（SPS/PPS 内联），由下面这条命令产出（行尾反斜杠在这种注释里
// 会触发 -Wcomment，所以分行写成一行）：
//   ffmpeg -f lavfi -i color=c=blue:s=64x64:d=0.04 -frames:v 1 -c:v libx264 -profile:v
//   baseline -pix_fmt yuv420p -x264-params keyint=1:min-keyint=1:scenecut=0 -f h264
// 与 scripts/test-client.mjs 里浏览器模拟端发送的是同一段码流。
const char* kIdrBase64 =
    "AAAAAWdCwArcQmwEQAAAAwBAAAAMg8SJ4AAAAAFozg8sgAAAAQYF//9Z3EXpvebZSLeWLNgg2SPu73gyNjQgLSBj"
    "b3JlIDE2NCByMzEwOCAzMWUxOWY5IC0gSC4yNjQvTVBFRy00IEFWQyBjb2RlYyAtIENvcHlsZWZ0IDIwMDMtMjAyMy"
    "AtIGh0dHA6Ly93d3cudmlkZW9sYW4ub3JnL3gyNjQuaHRtbCAtIG9wdGlvbnM6IGNhYmFjPTAgcmVmPTEgZGVibG9j"
    "az0xOjA6MCBhbmFseXNlPTB4MToweDExMSBtZT1oZXggc3VibWU9NyBwc3k9MSBwc3lfcmQ9MS4wMDowLjAwIG1peGVk"
    "X3JlZj0wIG1lX3JhbmdlPTE2IGNocm9tYV9tZT0xIHRyZWxsaXM9MSA4eDhkY3Q9MCBjcW09MCBkZWFkem9uZT0yMSwx"
    "MSBmYXN0X3Bza2lwPTEgY2hyb21hX3FwX29mZnNldD0tMiB0aHJlYWRzPTIgbG9va2FoZWFkX3RocmVhZHM9MSBzbGlj"
    "ZWRfdGhyZWFkcz0wIG5yPTAgZGVjaW1hdGU9MSBpbnRlcmxhY2VkPTAgYmx1cmF5X2NvbXBhdD0wIGNvbnN0cmFpbmVk"
    "X2ludHJhPTAgYmZyYW1lcz0wIHdlaWdodHA9MCBrZXlpbnQ9MSBrZXlpbnRfbWluPTEgc2NlbmVjdXQ9MCBpbnRyYV9y"
    "ZWZyZXNoPTAgcmM9Y3JmIG1idHJlZT0wIGNyZj0yMy4wIHFjb21wPTAuNjAgcXBtaW49MCBxcG1heD02OSBxcHN0ZXA9"
    "NCBpcF9yYXRpbz0xLjQwIGFxPTE6MS4wMACAAAABZYiEBLxGKAAMxMcAAX1o4AAiQycnJ1111111111114A=";

int b64_val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

std::vector<uint8_t> base64_decode(const char* s) {
  std::vector<uint8_t> out;
  int acc = 0;
  int bits = 0;
  for (const char* p = s; *p; ++p) {
    const int v = b64_val(*p);
    if (v < 0) continue;  // '=' 与换行
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>((acc >> bits) & 0xff));
    }
  }
  return out;
}

// 读本进程的驻留集（kB）。只在 Linux 上量得准；其它平台返回 -1，调用方跳过内存断言。
long rss_kb() {
#if defined(__linux__)
  std::FILE* f = std::fopen("/proc/self/status", "r");
  if (!f) return -1;
  char line[256];
  long v = -1;
  while (std::fgets(line, sizeof line, f)) {
    if (std::strncmp(line, "VmRSS:", 6) == 0) {
      v = std::strtol(line + 6, nullptr, 10);
      break;
    }
  }
  std::fclose(f);
  return v;
#else
  return -1;
#endif
}

// ---------------------------------------------------------------- 1) 基本契约

void test_decode_basic() {
  const auto idr = base64_decode(kIdrBase64);
  check(idr.size() > 100, "h264 dec: 内嵌 IDR 解出来不是空的");

  cue::media::H264Decoder dec;
  std::string err;
  check(dec.open(err), "h264 dec: open");
  check(dec.is_open(), "h264 dec: is_open after open");

  AVFrame* f = dec.decode(idr, err);
  check(f != nullptr, "h264 dec: 首帧（IDR）必须有输出");
  if (f) {
    check(f->width == 64 && f->height == 64, "h264 dec: 解出尺寸 64x64");
    check(f->data[0] != nullptr && f->data[1] != nullptr && f->data[2] != nullptr,
          "h264 dec: 三个平面都有数据");
  }
  check(err.empty(), "h264 dec: 正常帧不产生错误串");
  check(dec.stats().frames >= 1, "h264 dec: frames 计数递增");
  check(dec.stats().errors == 0, "h264 dec: 正常码流 decode_errors 为 0");
}

// 空输入不能崩，也不能被算成一次错误（调用方按 err 非空计 errors）。
void test_empty_payload() {
  const auto idr = base64_decode(kIdrBase64);
  cue::media::H264Decoder dec;
  std::string err;
  check(dec.open(err), "h264 dec: open for empty test");
  err.clear();
  check(dec.decode({}, err) == nullptr, "h264 dec: 空载荷返回 nullptr");
  check(err.empty(), "h264 dec: 空载荷不算错误");
  // 空载荷之后同一个解码器必须还能工作（长跑会话不重建解码器）
  AVFrame* f = dec.decode(idr, err);
  check(f != nullptr, "h264 dec: 空载荷之后仍能解码");
}

// ---------------------------------------------------------------- 2) 内存契约（本次事故的回归点）

void test_frame_released_across_send_packet() {
  const auto idr = base64_decode(kIdrBase64);
  cue::media::H264Decoder dec;
  std::string err;
  check(dec.open(err), "h264 dec: open for leak test");

  const long rss0 = rss_kb();
  if (rss0 < 0) {
    std::printf("SKIP: 平台不支持读 RSS，跳过内存断言\n");
    return;
  }

  // 预热：解码器内部帧池、libc arena 的首次分配不算泄漏。
  constexpr int kWarmup = 500;
  uint64_t frames = 0;
  for (int i = 0; i < kWarmup; ++i) {
    if (dec.decode(idr, err)) ++frames;
  }
  const long rss_warm = rss_kb();
  check(frames == static_cast<uint64_t>(kWarmup),
        "h264 dec: 预热阶段每个 IDR 都应解出一帧（否则下面的内存断言没有意义）");

  // 正式测量：64x64 一帧 YUV420 = 6144 B。旧实现每帧漏一份，
  // 6000 帧 ≈ 36 MB；修好后应基本不涨。阈值取 8 MB，两侧都有足够余量。
  constexpr int kIters = 6000;
  frames = 0;
  for (int i = 0; i < kIters; ++i) {
    if (dec.decode(idr, err)) ++frames;
  }
  const long rss_after = rss_kb();
  check(frames == static_cast<uint64_t>(kIters),
        "h264 dec: 测量阶段每个 IDR 都应解出一帧");

  const long grown_kb = rss_after - rss_warm;
  std::printf("  内存：预热后 %ld kB → %d 帧后 %ld kB（增长 %ld kB）\n", rss_warm, kIters,
              rss_after, grown_kb);
  check(grown_kb < 8 * 1024,
        "h264 dec: 连续解码不得让 RSS 线性增长（上一帧须在下一次 send_packet 之前释放）");
}

// ---------------------------------------------------------------- 3) flush / 重开

void test_flush_and_reopen() {
  const auto idr = base64_decode(kIdrBase64);
  cue::media::H264Decoder dec;
  std::string err;
  check(dec.open(err), "h264 dec: open for flush test");
  for (int i = 0; i < 10; ++i) dec.decode(idr, err);
  dec.flush();
  check(dec.stats().flushes == 1, "h264 dec: flush 计数");
  AVFrame* f = dec.decode(idr, err);
  check(f != nullptr, "h264 dec: flush 之后新 IDR 仍能解出");
  dec.close();
  check(!dec.is_open(), "h264 dec: close 之后 not open");
  check(dec.decode(idr, err) == nullptr, "h264 dec: close 之后 decode 返回 nullptr");
  check(dec.open(err), "h264 dec: 可以重新 open");
  check(dec.decode(idr, err) != nullptr, "h264 dec: 重开之后能解码");
}

}  // namespace

int main() {
  test_decode_basic();
  test_empty_payload();
  test_frame_released_across_send_packet();
  test_flush_and_reopen();
  if (failures == 0) {
    std::printf("OK: h264 decoder tests passed\n");
    return 0;
  }
  std::printf("FAILED: %d h264 decoder check(s)\n", failures);
  return 1;
}
