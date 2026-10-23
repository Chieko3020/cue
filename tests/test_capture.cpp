// T-11 采集端单测：上行包分片边界、Annex-B 起始码/访问单元分帧、swr 重采样样本数、
// ffmpeg 子进程命令行（含只能在目标平台运行的 dshow/avfoundation 参数）、Opus 20 ms 帧。
//
// 全部是纯函数/纯组件：不起 ffmpeg、不需要摄像头、毫秒级跑完。
// 断言口径与生产代码一致（proto::validate_media 反向校验），不给"为了让测试过"放宽。
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "cue/media/annexb.h"
#include "cue/media/audio_resampler.h"
#include "cue/media/ffmpeg_args.h"
#include "cue/media/opus_encoder.h"
#include "cue/proto/upstream_pack.h"
#include "cue/proto/wire.h"

namespace {

int failures = 0;

void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

std::string join(const std::vector<std::string>& v, char sep = ' ') {
  std::string out;
  for (const auto& s : v) {
    if (!out.empty()) out.push_back(sep);
    out += s;
  }
  return out;
}

bool contains(const std::vector<std::string>& argv, const std::string& needle) {
  return std::find(argv.begin(), argv.end(), needle) != argv.end();
}

// ---------------------------------------------------------------- 上行包

// 手动按小端读 u32/u64：这是对 wire.h 字节布局的独立复核，不调用 wire.h 的 detail::get_*。
uint32_t le_u32(const uint8_t* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
  return v;
}
uint64_t le_u64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}

void check_one_pack_case(std::size_t payload_len, cue::proto::UpstreamMedia meta,
                         const char* label) {
  std::vector<uint8_t> payload(payload_len);
  for (std::size_t i = 0; i < payload_len; ++i) payload[i] = static_cast<uint8_t>(i * 31 + 7);
  const auto packets = cue::proto::pack_upstream(payload, meta);
  const std::size_t expect_frags =
      payload_len == 0 ? 1 : (payload_len + cue::proto::kMaxWsFrag - 1) / cue::proto::kMaxWsFrag;
  bool ok = packets.size() == expect_frags;
  std::vector<uint8_t> reassembled;
  for (std::size_t i = 0; i < packets.size(); ++i) {
    const auto& pkt = packets[i];
    cue::proto::MediaHeader h;
    // 反向校验用生产代码的入口：能被 cued 收下的包才算协议合法
    if (cue::proto::validate_media({pkt.data(), pkt.size()}, h) != cue::proto::MediaReject::None) {
      ok = false;
      continue;
    }
    if (h.type != meta.type || h.codec != meta.codec) ok = false;
    if (h.stream_id != meta.stream_id || h.frame_id != meta.frame_id) ok = false;
    if (h.pts_us != meta.pts_us) ok = false;
    if (h.frag_index != i || h.frag_count != packets.size()) ok = false;
    const uint8_t want_flags = static_cast<uint8_t>(
        (meta.keyframe ? cue::proto::kUpFlagKeyframe : 0) |
        (i + 1 == packets.size() ? cue::proto::kUpFlagLastFrag : 0));
    if (h.flags != want_flags) ok = false;
    if (pkt.size() != cue::proto::kMediaHeaderSize + h.payload_len) ok = false;
    reassembled.insert(reassembled.end(), pkt.begin() + cue::proto::kMediaHeaderSize, pkt.end());
  }
  if (reassembled != payload) ok = false;
  if (!ok) {
    std::printf("FAIL: pack case %s (payload_len=%zu, frags=%zu)\n", label, payload_len,
                packets.size());
    ++failures;
  }
}

void test_upstream_pack() {
  using namespace cue::proto;
  UpstreamMedia video;
  video.type = static_cast<uint8_t>(MediaType::Video);
  video.codec = static_cast<uint8_t>(Codec::H264);
  video.stream_id = 1;
  video.frame_id = 42;
  video.pts_us = 1234567890123ULL;
  video.keyframe = true;

  // 分片边界：0 字节、1 字节、刚不满一片、正好一片、多 1 字节、四片
  check_one_pack_case(0, video, "empty");
  check_one_pack_case(1, video, "one-byte");
  check_one_pack_case(kMaxWsFrag - 1, video, "frag-minus-one");
  check_one_pack_case(kMaxWsFrag, video, "frag-exact");
  check_one_pack_case(kMaxWsFrag + 1, video, "frag-plus-one");
  check_one_pack_case(3 * kMaxWsFrag + 5, video, "four-frags");

  // 逐字节核对 32 字节头的偏移/字节序（与 web/app.js、test-client.mjs 同一布局）
  {
    std::vector<uint8_t> payload(10, 0x5A);
    UpstreamMedia m;
    m.type = static_cast<uint8_t>(MediaType::Video);
    m.codec = static_cast<uint8_t>(Codec::H264);
    m.stream_id = 0x01020304;
    m.frame_id = 0x0A0B0C0D;
    m.pts_us = 0x1112131415161718ULL;
    m.keyframe = true;
    const auto packets = pack_upstream(payload, m);
    check(packets.size() == 1, "single fragment for small payload");
    const auto& b = packets[0];
    check(b.size() == kMediaHeaderSize + payload.size(), "packet size = 32 + payload");
    check(b[0] == kMagicUp, "byte0 magic 0xC1");
    check(b[1] == 0x01, "byte1 type video");
    check(b[2] == 0x01, "byte2 codec h264");
    check(b[3] == (kUpFlagKeyframe | kUpFlagLastFrag), "byte3 flags keyframe|last_frag");
    check(le_u32(&b[4]) == 0x01020304, "stream_id little-endian @4");
    check(le_u32(&b[8]) == 0x0A0B0C0D, "frame_id little-endian @8");
    check(le_u32(&b[12]) == 0, "frag_index @12");
    check(le_u32(&b[16]) == 1, "frag_count @16");
    check(le_u64(&b[20]) == 0x1112131415161718ULL, "pts_us little-endian @20");
    check(le_u32(&b[28]) == payload.size(), "payload_len @28");
  }

  // 非关键帧：关键帧位必须全程为 0（否则 cued/probe 会把 P 帧当 IDR 处理）
  {
    UpstreamMedia m = video;
    m.keyframe = false;
    const auto packets = pack_upstream(std::vector<uint8_t>(20000, 0), m);
    check(packets.size() == 2, "20000 bytes -> 2 fragments");
    check((packets[0][3] & kUpFlagKeyframe) == 0, "non-keyframe frag0 no keyframe bit");
    check((packets[0][3] & kUpFlagLastFrag) == 0, "non-keyframe frag0 no last_frag");
    check((packets[1][3] & kUpFlagLastFrag) != 0, "last fragment has last_frag");
  }

  // 空载荷：协议层合法（1 片、payload_len=0），但不可解码——发送端负责不制造它
  {
    const auto packets = pack_upstream({}, video);
    check(packets.size() == 1, "empty payload -> exactly 1 fragment");
    MediaHeader h;
    check(validate_media({packets[0].data(), packets[0].size()}, h) == MediaReject::None,
          "empty payload passes validate_media");
    check(h.payload_len == 0, "empty payload_len = 0");
  }

  // 协议上界：恰好 1024 片合法，1025 片被 cued 以 FragCountTooLarge 拒
  {
    check(upstream_frame_fits(kMaxUpstreamFrameBytes), "frame fits at 1024 fragments");
    check(!upstream_frame_fits(kMaxUpstreamFrameBytes + 1), "frame oversize at 1024+1");
    std::vector<uint8_t> at_limit(kMaxUpstreamFrameBytes, 0x11);
    const auto ok_packets = pack_upstream(at_limit, video);
    check(ok_packets.size() == kMaxFragPerFrame, "16 MiB -> exactly 1024 fragments");
    MediaHeader h;
    check(validate_media({ok_packets.back().data(), ok_packets.back().size()}, h) ==
              MediaReject::None,
          "1024-fragment frame accepted");
    std::vector<uint8_t> over(kMaxUpstreamFrameBytes + 1, 0x22);
    const auto over_packets = pack_upstream(over, video);
    check(over_packets.size() == kMaxFragPerFrame + 1, "16 MiB + 1 -> 1025 fragments");
    check(validate_media({over_packets.back().data(), over_packets.back().size()}, h) ==
              MediaReject::FragCountTooLarge,
          "1025 fragments rejected with frag_count_too_large");
  }
}

// ---------------------------------------------------------------- Annex-B

std::vector<uint8_t> make_nal(bool four_byte, std::initializer_list<uint8_t> payload) {
  std::vector<uint8_t> out;
  if (four_byte) out.push_back(0);
  out.push_back(0);
  out.push_back(0);
  out.push_back(1);
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

void append(std::vector<uint8_t>& dst, const std::vector<uint8_t>& nal) {
  dst.insert(dst.end(), nal.begin(), nal.end());
}

void test_annexb_split() {
  using namespace cue::media;
  // SPS(4 字节起始码) + PPS(3 字节) + IDR(4 字节) + 一个非 IDR slice(3 字节)
  std::vector<uint8_t> stream;
  append(stream, make_nal(true, {kNalTypeSps, 0x42, 0xC0, 0x1E}));
  append(stream, make_nal(false, {kNalTypePps, 0xCE, 0x06}));
  append(stream, make_nal(true, {kNalTypeIdr, 0x88, 0x84, 0x21}));
  append(stream, make_nal(false, {kNalTypeSliceNonIdr, 0x9A, 0x01}));

  const auto nals = split_annexb(stream);
  check(nals.size() == 4, "split: four NAL units");
  if (nals.size() == 4) {
    check(nals[0].start_code_len == 4, "split: first start code is 4 bytes");
    check(nals[0].type() == kNalTypeSps, "split: NAL0 is SPS");
    check(nals[0].offset == 0, "split: NAL0 offset 0");
    check(nals[1].start_code_len == 3, "split: second start code is 3 bytes");
    check(nals[1].type() == kNalTypePps, "split: NAL1 is PPS");
    check(nals[2].start_code_len == 4, "split: third start code is 4 bytes");
    check(nals[2].type() == kNalTypeIdr && nals[2].is_keyframe(), "split: NAL2 is IDR");
    check(nals[3].type() == kNalTypeSliceNonIdr && !nals[3].is_keyframe(),
          "split: NAL3 is non-IDR slice");
    check(nals[3].payload.size() == 3, "split: NAL3 payload size");
    check(nals[3].payload[0] == kNalTypeSliceNonIdr && nals[3].payload[2] == 0x01,
          "split: NAL3 payload bytes");
    check(nals[0].payload.size() == 4, "split: NAL0 payload size");
  }

  // 3 字节起始码在缓冲起始处
  {
    const auto s = make_nal(false, {0x41, 0x01});
    const auto parsed = split_annexb(s);
    check(parsed.size() == 1 && parsed[0].start_code_len == 3, "split: leading 3-byte start code");
  }
  // 防竞争字节 00 00 03 01 不是起始码
  {
    const auto s = make_nal(false, {0x41, 0x00, 0x00, 0x03, 0x01, 0x22});
    const auto parsed = split_annexb(s);
    check(parsed.size() == 1, "split: 00 00 03 01 is not a start code");
    check(parsed.size() == 1 && parsed[0].payload.size() == 6, "split: emulation bytes kept");
  }
  // 没有起始码 → 不是 Annex-B
  {
    const std::vector<uint8_t> junk{0x65, 0x88, 0x84};
    check(split_annexb(junk).empty(), "split: no start code -> no NAL");
  }
}

std::vector<uint8_t> make_au(uint8_t slice_header) {
  using namespace cue::media;
  std::vector<uint8_t> au;
  append(au, make_nal(true, {kNalTypeAud, 0xF0}));
  append(au, make_nal(true, {kNalTypeSps, 0x42, 0xC0, 0x1E}));
  append(au, make_nal(true, {kNalTypePps, 0xCE, 0x06}));
  append(au, make_nal(true, {slice_header, 0x88, 0x84}));
  return au;
}

void test_au_assembler() {
  using namespace cue::media;
  const std::vector<uint8_t> au1 = make_au(kNalTypeIdr);
  const std::vector<uint8_t> au2 = make_au(kNalTypeSliceNonIdr);
  const std::vector<uint8_t> au3 = make_au(kNalTypeSliceNonIdr);
  std::vector<uint8_t> stream;
  append(stream, au1);
  append(stream, au2);
  append(stream, au3);

  // 一次性喂入：只有前两个 AU 完整（第三个还在等"下一个 AUD"）
  {
    AnnexBAuAssembler a;
    auto got = a.feed(stream);
    check(got.has_value(), "assembler: feed ok");
    check(got && got->size() == 2, "assembler: whole-buffer feed yields 2 complete AUs");
    if (got && got->size() == 2) {
      check((*got)[0] == au1, "assembler: AU0 bytes (AUD+SPS+PPS+IDR) exact");
      check((*got)[1] == au2, "assembler: AU1 bytes exact");
    }
    const auto tail = a.flush();
    check(tail.size() == 1 && tail[0] == au3, "assembler: flush yields last AU");
    check(a.audio_detected(), "assembler: AUD detected");
  }

  // 按 1 字节喂：起始码被切成任意碎片也必须还原
  {
    AnnexBAuAssembler a;
    std::vector<std::vector<uint8_t>> got;
    for (uint8_t b : stream) {
      const std::vector<uint8_t> one{b};
      auto part = a.feed(one);
      check(part.has_value(), "assembler: byte-wise feed ok");
      if (!part) break;
      for (auto& au : *part) got.push_back(std::move(au));
    }
    check(got.size() == 2, "assembler: byte-wise feed yields 2 AUs before flush");
    if (got.size() == 2) {
      check(got[0] == au1, "assembler: byte-wise AU0 exact");
      check(got[1] == au2, "assembler: byte-wise AU1 exact");
    }
    auto tail = a.flush();
    check(tail.size() == 1 && tail[0] == au3, "assembler: byte-wise flush exact");
  }

  // 没有 AUD 的流：如实报 NoAud，不猜分帧
  {
    AnnexBAuAssembler a;
    std::vector<uint8_t> no_aud;
    for (int i = 0; i < 64; ++i) append(no_aud, make_nal(false, {kNalTypeSliceNonIdr, 0x9A}));
    auto got = a.feed(no_aud);
    check(!got.has_value(), "assembler: no AUD -> error");
    check(!got && got.error() == AuAssemblerError::NoAud, "assembler: error is NoAud");
  }
}

// ---------------------------------------------------------------- 重采样

void test_resampler() {
  using namespace cue::media;
  AudioResampler rs;
  std::string err;
  ResampleSpec spec;
  spec.in_rate = 44100;
  spec.in_channels = 2;
  spec.out_rate = 48000;
  spec.out_channels = 1;
  check(rs.open(spec, err), "resampler: open 44100/2 -> 48000/1");

  // 100 ms 立体声正弦（441 Hz，44.1k 下正好 100 样本/周期）
  const std::size_t in_frames = 4410;
  std::vector<int16_t> in(in_frames * 2);
  for (std::size_t i = 0; i < in_frames; ++i) {
    const int16_t s = static_cast<int16_t>(10000 * std::sin(2.0 * M_PI * 441.0 *
                                                             static_cast<double>(i) / 44100.0));
    in[i * 2] = s;
    in[i * 2 + 1] = s;
  }

  auto feed_all = [&](std::size_t chunk, std::size_t& out_samples) {
    std::vector<int16_t> out;
    std::size_t fed = 0;
    while (fed < in_frames) {
      const std::size_t n = std::min(chunk, in_frames - fed);
      const auto rc = rs.convert(in.data() + fed * 2, n, out, err);
      check(rc.has_value(), "resampler: convert ok");
      if (!rc) break;
      fed += n;
    }
    const auto fr = rs.flush(out, err);
    check(fr.has_value(), "resampler: flush ok");
    out_samples = out.size();
  };

  // 一次性喂 100 ms：swr 的输出（含尾部 flush）必须精确等于 4800。
  // 这是 libswresample 多相滤波器的延迟补偿；如果这里出现差几个样本，
  // 长跑会累积成音频时钟偏移，所以断言精确值而不是"够接近"。
  std::size_t samples_one_shot = 0;
  feed_all(in_frames, samples_one_shot);
  check(samples_one_shot == 4800, "resampler: 4410 stereo frames -> 4800 mono samples (one shot)");

  // 分块喂入必须得到同样的总样本数（流式路径与一次性路径等价）
  rs.close();
  check(rs.open(spec, err), "resampler: reopen");
  std::size_t samples_chunked = 0;
  feed_all(1024, samples_chunked);
  check(samples_chunked == 4800, "resampler: chunked feed -> 4800 samples");

  // 每帧样本数换算（Opus 20 ms 打包用）
  check(AudioResampler::output_frames_for(4410, 44100, 48000) == 4800,
        "output_frames_for: 100 ms @44.1k -> 4800");
  check(AudioResampler::output_frames_for(441, 44100, 48000) == 480,
        "output_frames_for: 10 ms @44.1k -> 480");
  check(AudioResampler::output_frames_for(800, 8000, 48000) == 4800,
        "output_frames_for: 100 ms @8k -> 4800");

  // 8 kHz 单声道 → 48 kHz 单声道（6 倍升采样）
  AudioResampler wide;
  std::string werr;
  ResampleSpec wspec;
  wspec.in_rate = 8000;
  wspec.in_channels = 1;
  wspec.out_rate = 48000;
  wspec.out_channels = 1;
  check(wide.open(wspec, werr), "resampler: open 8000/1 -> 48000/1");
  {
    std::vector<int16_t> out;
    std::vector<int16_t> pcm(800, 1000);
    check(wide.convert(pcm.data(), pcm.size(), out, werr).has_value(), "resampler: 8k convert");
    check(wide.flush(out, werr).has_value(), "resampler: 8k flush");
    check(out.size() == 4800, "resampler: 800 @8k -> 4800 @48k");
  }

  // 非法规格必须被拒，而不是静默产出垃圾
  {
    AudioResampler bad;
    std::string berr;
    ResampleSpec bspec;
    bspec.in_rate = 0;
    check(!bad.open(bspec, berr), "resampler: zero rate rejected");
  }
}

// ---------------------------------------------------------------- ffmpeg 命令行

void test_ffmpeg_args() {
  using namespace cue::media;
  FfmpegArgsSpec s;
  s.source = CaptureSource::Device;
  s.platform = HostPlatform::Windows;
  s.kind = DeviceKind::Video;
  s.encode = VideoEncodeMode::InProcess;
  s.device = "USB Camera";
  s.width = 640;
  s.height = 360;
  s.fps = 30;

  const auto dshow = build_ffmpeg_argv(s);
  check(!dshow.empty(), "dshow video: argv not empty");
  const std::string dshow_line = join(dshow);
  check(dshow_line.find("-f dshow") != std::string::npos, "dshow video: input format");
  check(dshow_line.find("-video_size 640x360") != std::string::npos, "dshow video: size");
  check(dshow_line.find("-framerate 30") != std::string::npos, "dshow video: framerate");
  check(dshow_line.find("-i video=USB Camera") != std::string::npos, "dshow video: device name");
  check(dshow_line.find("-f rawvideo") != std::string::npos, "dshow video: rawvideo output");
  check(dshow_line.find("-pix_fmt yuv420p") != std::string::npos, "dshow video: pix_fmt");
  check(dshow_line.find("-s 640x360") != std::string::npos, "dshow video: raw size");
  check(contains(dshow, "-nostdin"), "dshow video: -nostdin present");
  check(contains(dshow, "-an"), "dshow video: audio disabled on video child");
  check(dshow.back() == "pipe:1", "dshow video: writes to stdout pipe");

  // dshow 音频：同一台设备名字带空格/括号是最常见的情况
  {
    FfmpegArgsSpec a = s;
    a.kind = DeviceKind::Audio;
    a.device = "Microphone (USB Audio)";
    const auto argv = build_ffmpeg_argv(a);
    const std::string line = join(argv);
    check(line.find("-f dshow") != std::string::npos, "dshow audio: input format");
    check(line.find("-i audio=Microphone (USB Audio)") != std::string::npos,
          "dshow audio: audio=<device>");
    check(line.find("-ac 2 -ar 44100 -f s16le pipe:1") != std::string::npos,
          "dshow audio: s16le interleaved output");
  }

  // avfoundation：视频索引直接给，音频索引从同一串里取
  {
    FfmpegArgsSpec v = s;
    v.platform = HostPlatform::macOS;
    v.device = "0";
    const auto argv = build_ffmpeg_argv(v);
    const std::string line = join(argv);
    check(line.find("-f avfoundation") != std::string::npos, "avfoundation video: input format");
    check(line.find("-i 0") != std::string::npos, "avfoundation video: device index");
    check(line.find("-video_size 640x360") != std::string::npos, "avfoundation video: size");

    FfmpegArgsSpec a = s;
    a.platform = HostPlatform::macOS;
    a.kind = DeviceKind::Audio;
    a.device = "0";
    const auto audio0 = build_ffmpeg_argv(a);
    check(join(audio0).find("-i :0") != std::string::npos,
          "avfoundation audio: '0' -> audio index 0");
    a.device = "0:1";
    const auto audio1 = build_ffmpeg_argv(a);
    check(join(audio1).find("-i :1") != std::string::npos,
          "avfoundation audio: '0:1' -> audio index 1");
    // 子进程编码的 AVC level 也要跟着 -level 走（音频规格不影响视频）
    FfmpegArgsSpec c = s;
    c.platform = HostPlatform::macOS;
    c.encode = VideoEncodeMode::FfmpegChild;
    c.device = "0";
    c.level_idc = 40;
    const std::string child = join(build_ffmpeg_argv(c));
    check(child.find("-level 4.0") != std::string::npos, "avfoundation child encode: -level 4.0");
  }

  // 子进程编码：Annex-B 裸流 + aud=1（采集端分帧的唯一判据）
  {
    FfmpegArgsSpec c = s;
    c.platform = HostPlatform::Windows;
    c.encode = VideoEncodeMode::FfmpegChild;
    c.level_idc = 30;
    const std::string line = join(build_ffmpeg_argv(c));
    check(line.find("-c:v libx264") != std::string::npos, "child encode: libx264");
    check(line.find("-preset veryfast") != std::string::npos, "child encode: preset");
    check(line.find("-b:v 1200k") != std::string::npos, "child encode: bitrate");
    check(line.find("-g 60") != std::string::npos, "child encode: gop");
    check(line.find("-tune zerolatency") != std::string::npos, "child encode: zerolatency");
    check(line.find("aud=1") != std::string::npos, "child encode: aud=1");
    // 断言的是**短横线**写法：x264 只认 `repeat-headers`，写成下划线会被静默忽略，
    // 而"每个 IDR 自带 SPS/PPS"是丢包后能自愈的关键依赖，不能靠默认值。
    check(line.find("repeat-headers=1") != std::string::npos,
          "child encode: repeat headers（短横线，x264 才认）");
    check(line.find("-level 3.0") != std::string::npos, "child encode: -level 3.0");
    check(line.find("-f h264") != std::string::npos, "child encode: h264 elementary stream");
  }

  // lavfi：合成源 + 裸帧/裸流
  {
    FfmpegArgsSpec l = s;
    l.source = CaptureSource::Lavfi;
    l.platform = HostPlatform::Linux;
    l.lavfi = "testsrc2=size=640x360:rate=30";
    const std::string line = join(build_ffmpeg_argv(l));
    check(line.find("-re -f lavfi -i testsrc2=size=640x360:rate=30") != std::string::npos,
          "lavfi: -re paced synthetic source");
    check(line.find("-f rawvideo") != std::string::npos, "lavfi: rawvideo output for inproc");

    FfmpegArgsSpec a = l;
    a.kind = DeviceKind::Audio;
    const std::string aline = join(build_ffmpeg_argv(a));
    check(aline.find("-re -f lavfi -i sine=frequency=440:sample_rate=44100") != std::string::npos,
          "lavfi audio: -re paced sine at native 44.1 kHz");
    check(aline.find("-f s16le pipe:1") != std::string::npos, "lavfi audio: s16le stdout");
  }

  // 文件源（FR-CAP.3 的"文件"半边）
  {
    FfmpegArgsSpec f = s;
    f.source = CaptureSource::File;
    f.platform = HostPlatform::Linux;
    f.input = "/tmp/clip.mp4";
    const std::string line = join(build_ffmpeg_argv(f));
    check(line.find("-re -i /tmp/clip.mp4") != std::string::npos, "file: -re input");
    check(line.find("-an -f rawvideo") != std::string::npos, "file: video only");
  }

  // Linux 上的设备后端必须返回空（由 app 给出"请用 v4l2/lavfi"的错误）
  {
    FfmpegArgsSpec d = s;
    d.platform = HostPlatform::Linux;
    check(build_ffmpeg_argv(d).empty(), "linux device backend: unsupported -> empty argv");
  }
  // 缺必填字段时返回空，而不是半截命令
  {
    FfmpegArgsSpec f = s;
    f.source = CaptureSource::File;
    check(build_ffmpeg_argv(f).empty(), "file source without --input -> empty argv");
    FfmpegArgsSpec d = s;
    d.device.clear();
    check(build_ffmpeg_argv(d).empty(), "device source without --device -> empty argv");
  }

  check(avc_level_name(30) == "3.0", "avc_level_name(30)");
  check(avc_level_name(40) == "4.0", "avc_level_name(40)");
  check(avc_level_name(0) == "auto", "avc_level_name(0)");

  // Windows 命令行拼装：dshow 的设备名里空格/引号都很常见
  check(build_windows_command_line({"ffmpeg", "-i", "video=USB Camera"}) ==
            "ffmpeg -i \"video=USB Camera\"",
        "windows cmdline: quoted device name");
  check(build_windows_command_line({"a", "b\\", "c"}) == "a b\\ c",
        "windows cmdline: plain backslash untouched");
  check(build_windows_command_line({"a", "b c\\"}) == "a \"b c\\\\\"",
        "windows cmdline: trailing backslash doubled before closing quote");
  check(build_windows_command_line({"a", "q\"q"}) == "a \"q\\\"q\"",
        "windows cmdline: embedded quote escaped");
}

// ---------------------------------------------------------------- Opus

void test_opus() {
  using namespace cue::media;
  OpusAudioEncoder enc;
  std::string err;
  check(enc.open(err), "opus: open 48k mono");
  check(enc.frame_samples() == 960, "opus: 20 ms = 960 samples");
  std::vector<int16_t> pcm(OpusAudioEncoder::kFrameSamples, 0);
  auto pkt = enc.encode(pcm.data(), pcm.size(), err);
  check(pkt.has_value(), "opus: encode 960 samples");
  if (pkt) {
    check(!pkt->empty(), "opus: packet non-empty");
    check(pkt->size() < 1200, "opus: 48 kbps 20 ms packet is small");
  }
  auto bad = enc.encode(pcm.data(), 480, err);
  check(!bad.has_value() && bad.error() == OpusError::InvalidFrame,
        "opus: wrong frame size rejected");
}

}  // namespace

int main() {
  test_upstream_pack();
  test_annexb_split();
  test_au_assembler();
  test_resampler();
  test_ffmpeg_args();
  test_opus();
  if (failures == 0) {
    std::printf("OK: capture tests passed\n");
    return 0;
  }
  std::printf("FAILED: %d capture check(s)\n", failures);
  return 1;
}
