// T-30 / FR-DEV.6：Opus 解码单测。
//
// 为什么必须有「正弦 vs 静音」这一组：解码函数返回成功、样本数也对得上，但全是零，
// 等于链路没通。只断言"解出了 960 个样本"会让这种失败看起来像成功。
//
// 全部是纯组件：libopus 编 → libopus 解，不起 ffmpeg、不需要声卡，毫秒级跑完。
// 断言与生产代码同口径（cued 用同一个 OpusAudioDecoder、同一个 rms_dbfs）。
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "cue/media/audio_decoder.h"
#include "cue/media/opus_encoder.h"

namespace {

int failures = 0;

void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

// 幅度按 int16 满量程算：amp = 16384 ≈ 0.5 FS，理论 RMS 读 -9.03 dBFS。
constexpr double kAmplitude = 16384.0;
constexpr double kSineHz = 440.0;
constexpr double kPi = 3.14159265358979323846;

std::vector<int16_t> make_sine(std::size_t samples, double freq_hz, double amp) {
  std::vector<int16_t> pcm(samples);
  for (std::size_t i = 0; i < samples; ++i) {
    const double t = static_cast<double>(i) / cue::media::kAudioSampleRate;
    pcm[i] = static_cast<int16_t>(std::lround(amp * std::sin(2.0 * kPi * freq_hz * t)));
  }
  return pcm;
}

// 确定性伪随机（LCG）：不用 <random> 是为了同一份字节在任何标准库上都一样。
std::vector<uint8_t> pseudo_random_bytes(std::size_t n, uint32_t seed) {
  std::vector<uint8_t> out(n);
  uint32_t x = seed;
  for (std::size_t i = 0; i < n; ++i) {
    x = x * 1664525u + 1013904223u;
    out[i] = static_cast<uint8_t>(x >> 24);
  }
  return out;
}

// cued 的计数口径：失败就 audio_decode_errors += 1，成功才累计 PCM 样本数。
struct DecodeCounters {
  uint64_t errors = 0;
  uint64_t pcm_samples = 0;
  uint64_t frames = 0;
};

cue::media::DecodedAudioFrame decode_or_count(cue::media::OpusAudioDecoder& dec,
                                              std::span<const uint8_t> packet,
                                              DecodeCounters& counters, std::string& err) {
  auto f = dec.decode(packet, err);
  if (!f) {
    ++counters.errors;
    return {};
  }
  ++counters.frames;
  counters.pcm_samples += f->sample_count();
  return std::move(*f);
}

std::vector<uint8_t> encode_frame(cue::media::OpusAudioEncoder& enc,
                                  const std::vector<int16_t>& pcm) {
  std::string err;
  auto pkt = enc.encode(pcm.data(), pcm.size(), err);
  if (!pkt) {
    std::printf("FAIL: opus encode: %s\n", err.c_str());
    ++failures;
    return {};
  }
  return std::move(*pkt);
}

// ---------------------------------------------------------------- 1) 正弦 vs 静音

void test_sine_vs_silence_roundtrip() {
  using namespace cue::media;
  OpusAudioEncoder enc;
  OpusAudioDecoder dec;
  std::string err;
  check(enc.open(err), "opus dec: encoder open");
  check(dec.open(err), "opus dec: decoder open");

  DecodeCounters counters;
  const std::vector<int16_t> silence(kAudioFrameSamples, 0);
  const std::vector<int16_t> sine = make_sine(kAudioFrameSamples, kSineHz, kAmplitude);

  // 先各编一帧，再各自解码：编码器状态会影响码流，但正弦/静音的差别不是它能抹掉的。
  const auto sine_pkt = encode_frame(enc, sine);
  check(!sine_pkt.empty(), "opus dec: sine packet non-empty");
  check(sine_pkt.size() <= kMaxOpusPacketBytes, "opus dec: sine packet within RFC limit");

  const auto sine_out = decode_or_count(dec, sine_pkt, counters, err);
  check(counters.errors == 0, "opus dec: sine decodes without error");
  check(sine_out.sample_count() == static_cast<std::size_t>(kAudioFrameSamples),
        "opus dec: 20 ms @48k -> 960 samples");
  check(sine_out.sample_rate == kAudioSampleRate && sine_out.channels == kAudioChannels,
        "opus dec: output is 48 kHz mono");

  const double sine_dbfs = rms_dbfs(sine_out.samples);
  check(sine_dbfs > -20.0 && sine_dbfs < 0.0, "opus dec: sine RMS is a real level (-20..0 dBFS)");

  // 静音单独用一个编码器 + 一个**全新的解码器**：Opus 解码器是带状态的，
  // 刚解完一帧响的正弦，再解 3 字节的静音包，重叠相加会带出上一帧的尾巴
  // （实测同一个解码器读 -20 dBFS）。要测"静音流"就必须从静音开始，
  // 否则测到的是上一段流的残留，不是静音。
  OpusAudioEncoder enc2;
  OpusAudioDecoder dec2;
  check(enc2.open(err) && dec2.open(err), "opus dec: silence encoder/decoder open");
  const auto silence_pkt = encode_frame(enc2, silence);
  check(!silence_pkt.empty(), "opus dec: silence packet non-empty");
  const auto silence_out = decode_or_count(dec2, silence_pkt, counters, err);
  check(counters.errors == 0, "opus dec: silence decodes without error");
  check(silence_out.sample_count() == static_cast<std::size_t>(kAudioFrameSamples),
        "opus dec: silence frame is 960 samples");

  const double silence_dbfs = rms_dbfs(silence_out.samples);
  check(silence_dbfs < -60.0, "opus dec: silence RMS is far below sine");
  // 这是「静音 ≠ 通」的硬判据：两者差至少 30 dB，不是"都解成功了"就放过。
  check(sine_dbfs - silence_dbfs > 30.0, "opus dec: sine vs silence differ by > 30 dB");
  check(std::isfinite(dbfs_for_metric(silence_dbfs)), "opus dec: silence dBFS is JSON-representable");
  check(dbfs_for_metric(silence_dbfs) >= kSilenceFloorDbfs, "opus dec: floor respected");
  // 数字静音的真值是 -inf；写指标时必须落到下限，而不是 0 或 NaN。
  check(dbfs_for_metric(-std::numeric_limits<double>::infinity()) == kSilenceFloorDbfs,
        "opus dec: -inf clamps to kSilenceFloorDbfs");

  std::printf("  sine=%.2f dBFS silence=%.2f dBFS (metric=%.2f) sine-metric Δ=%.2f dB\n", sine_dbfs,
              silence_dbfs, dbfs_for_metric(silence_dbfs),
              sine_dbfs - dbfs_for_metric(silence_dbfs));
}

// ---------------------------------------------------------------- 2) 损坏包

void test_damaged_packet() {
  using namespace cue::media;
  OpusAudioDecoder dec;
  std::string err;
  check(dec.open(err), "opus dec: open for damaged test");
  DecodeCounters counters;

  const auto random = pseudo_random_bytes(64, 0xC0FFEEu);
  err.clear();
  auto bad = dec.decode(random, err);
  ++counters.errors;  // 调用方（cued）在 !f 时必须计一次
  check(!bad.has_value(), "opus dec: random bytes rejected, not decoded");
  check(!err.empty(), "opus dec: damaged packet keeps a reason string");
  if (bad) {
    check(false, "opus dec: random bytes produced a frame");
  } else {
    check(bad.error() == AudioDecodeError::InvalidPacket ||
              bad.error() == AudioDecodeError::DecodeFailed,
          "opus dec: damaged packet error kind");
  }

  // 全 0xFF：TOC 的帧数编码合法但帧结构不存在，libopus 报 OPUS_INVALID_PACKET。
  const std::vector<uint8_t> ff(8, 0xFF);
  const auto ff_bad = dec.decode(ff, err);
  check(!ff_bad.has_value(), "opus dec: 0xFF packet rejected");
  ++counters.errors;  // 每一个解不出来的包都要计一次，不能只计第一种

  // 出错之后同一个解码器必须还能解正常包（长跑会话不重建解码器）。
  OpusAudioEncoder enc;
  check(enc.open(err), "opus dec: encoder for recovery test");
  const auto pcm = make_sine(kAudioFrameSamples, kSineHz, kAmplitude);
  const auto pkt = encode_frame(enc, pcm);
  const auto out = decode_or_count(dec, pkt, counters, err);
  check(counters.frames == 1 && out.sample_count() == static_cast<std::size_t>(kAudioFrameSamples),
        "opus dec: decoder recovers after a damaged packet");
  check(counters.errors == 2, "opus dec: every failed packet is counted");
}

// ---------------------------------------------------------------- 3) 空包 / 超长包

void test_packet_bounds() {
  using namespace cue::media;
  OpusAudioDecoder dec;
  std::string err;
  check(dec.open(err), "opus dec: open for bounds test");
  DecodeCounters counters;

  err.clear();
  auto empty = dec.decode({}, err);
  check(!empty.has_value() && empty.error() == AudioDecodeError::EmptyPacket,
        "opus dec: empty packet rejected (no PLC fabrication)");
  check(err.find("PLC") != std::string::npos, "opus dec: empty packet reason explains PLC");
  ++counters.errors;

  // RFC 上限 + 1，以及一个"像分片那样大"的载荷：都必须在进解码器之前被拒。
  const std::vector<uint8_t> too_big(kMaxOpusPacketBytes + 1, 0);
  err.clear();
  auto big = dec.decode(too_big, err);
  check(!big.has_value() && big.error() == AudioDecodeError::PacketTooLarge,
        "opus dec: 1276-byte packet rejected");
  check(err.find("1275") != std::string::npos, "opus dec: oversize reason names the limit");
  ++counters.errors;

  const std::vector<uint8_t> ws_frag(16000, 0x80);
  const auto huge = dec.decode(ws_frag, err);
  check(!huge.has_value() && huge.error() == AudioDecodeError::PacketTooLarge,
        "opus dec: 16 KiB payload rejected");
  ++counters.errors;

  // 边界内的最大包不该因为长度被拒（能不能解看内容，这里是"没有尺寸误判"）。
  OpusAudioEncoder enc;
  check(enc.open(err), "opus dec: encoder for bound test");
  const auto pcm = make_sine(kAudioFrameSamples, kSineHz, kAmplitude);
  const auto pkt = encode_frame(enc, pcm);
  check(pkt.size() <= kMaxOpusPacketBytes, "opus dec: normal packet below limit");
  check(counters.errors == 3, "opus dec: three rejected packets counted");
}

// ---------------------------------------------------------------- 4) 连续帧 + 样本数

void test_sample_count_contract() {
  using namespace cue::media;
  OpusAudioEncoder enc;
  OpusAudioDecoder dec;
  std::string err;
  check(enc.open(err) && dec.open(err), "opus dec: open for sample-count test");
  DecodeCounters counters;

  // 50 帧 = 1 s：解码器连续工作，样本数必须严格可对账（960 × 50）。
  for (int i = 0; i < 50; ++i) {
    const auto pcm = make_sine(kAudioFrameSamples, kSineHz + i, kAmplitude);
    const auto pkt = encode_frame(enc, pcm);
    const auto out = decode_or_count(dec, pkt, counters, err);
    if (out.sample_count() != static_cast<std::size_t>(kAudioFrameSamples)) {
      check(false, "opus dec: every frame is exactly 960 samples");
      break;
    }
  }
  check(counters.errors == 0, "opus dec: 50-frame run has no errors");
  check(counters.pcm_samples == 50ull * static_cast<uint64_t>(kAudioFrameSamples),
        "opus dec: 50 frames = 48000 PCM samples");

  // rms_dbfs 的数学：满幅正弦（amp 32767、整周期数）应读 -3.01 dBFS。
  const std::vector<int16_t> full = make_sine(48000, 100.0, 32767.0);
  const double full_dbfs = rms_dbfs(full);
  check(std::fabs(full_dbfs + 3.01) < 0.1, "opus dec: full-scale sine reads -3.01 dBFS");
  const std::vector<int16_t> zeros(960, 0);
  check(std::isinf(rms_dbfs(zeros)), "opus dec: digital silence is -inf");
  check(std::isinf(rms_dbfs(std::span<const int16_t>{})), "opus dec: no samples is -inf");
}

}  // namespace

int main() {
  using namespace cue::media;
  // 没打开的解码器必须明确报 not_open，而不是空指针崩溃。
  {
    OpusAudioDecoder cold;
    std::string err;
    const std::vector<uint8_t> pkt{0x00};
    const auto r = cold.decode(pkt, err);
    check(!r.has_value() && r.error() == AudioDecodeError::NotOpen,
          "opus dec: decode before open -> NotOpen");
  }
  test_sine_vs_silence_roundtrip();
  test_damaged_packet();
  test_packet_bounds();
  test_sample_count_contract();
  if (failures == 0) {
    std::printf("OK: audio decoder tests passed\n");
    return 0;
  }
  std::printf("FAILED: %d audio decoder check(s)\n", failures);
  return 1;
}
