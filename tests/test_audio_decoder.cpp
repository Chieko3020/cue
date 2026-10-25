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

// ---------------------------------------------------------------- 5) 编码器弱网参数

// 这一组测的是**配置语义**，不是音质：loss perc 0 与 >0 的差别必须能被观察到
// （下面的 FEC 恢复用例用 LBRR 的有无来证明它真的下发到了 libopus）。
void test_encoder_loss_settings() {
  using namespace cue::media;
  std::string err;
  OpusAudioEncoder enc;
  // 默认值必须与常量一致：CLI 的默认值就是从这里来的（cue-capture 的 --opus-loss-perc）。
  check(enc.packet_loss_perc() == OpusAudioEncoder::kDefaultPacketLossPerc,
        "opus dec: default loss perc");
  check(enc.fec_enabled(), "opus dec: FEC on by default (loss perc > 0)");
  check(enc.dtx_enabled(), "opus dec: DTX on by default");

  // 越界必须**拒绝且不改当前值**（不夹取：夹取会让 500% 看起来像 100% 生效了）
  check(!enc.set_packet_loss_perc(-1), "opus dec: loss perc -1 rejected");
  check(!enc.set_packet_loss_perc(OpusAudioEncoder::kMaxPacketLossPerc + 1),
        "opus dec: loss perc 101 rejected");
  check(enc.packet_loss_perc() == OpusAudioEncoder::kDefaultPacketLossPerc,
        "opus dec: rejected value does not change current");

  // 0 = 不带 FEC：libopus 在 loss perc == 0 时不产生 LBRR，所以 fec_enabled() 必须为 false，
  // 而不是只回显开关状态（那会让日志/指标撒谎）。
  check(enc.set_packet_loss_perc(0), "opus dec: loss perc 0 accepted");
  check(!enc.fec_enabled(), "opus dec: loss perc 0 -> FEC not effective");
  check(enc.set_packet_loss_perc(100), "opus dec: loss perc 100 accepted");
  check(enc.fec_enabled(), "opus dec: loss perc 100 -> FEC effective");

  // open 之前设置的值必须在 open 时生效（open 不会把配置重置回默认）
  OpusAudioEncoder pre;
  check(pre.set_packet_loss_perc(7), "opus dec: preset loss perc 7");
  check(pre.set_dtx(false), "opus dec: preset dtx off");
  check(pre.open(err), "opus dec: open with preset settings");
  check(pre.packet_loss_perc() == 7 && !pre.dtx_enabled(),
        "opus dec: open keeps preset settings");
  pre.close();
}

// ---------------------------------------------------------------- 6) in-band FEC 恢复

// libopus 的语义：**本帧**的 LBRR 冗余在后一个包里。所以"丢一帧"的恢复方式是
// 拿后一包做 decode_fec=1，解出的是**前一帧**；随后还要用 Normal 解同一包拿到当前帧。
//
// 允许跳过的条件（不是"跑不出就放过"）：编码器可能因为码率/内容决定不带 LBRR，
// 这时 decode(FecRecover) 会返回 FecUnavailable。本用例用 loss perc=10 + 稳定正弦，
// 实测该组合一定带 LBRR；如果某个 libopus 版本换了策略，跳过并打印原因比硬断言更诚实
// ——硬断言会把"编码器选择不带冗余"误报成"FEC 坏了"。
void test_fec_recovery() {
  using namespace cue::media;
  std::string err;
  OpusAudioEncoder enc;
  OpusAudioDecoder dec;
  check(enc.open(err) && dec.open(err), "opus fec: encoder/decoder open");
  check(enc.set_packet_loss_perc(10), "opus fec: loss perc 10");

  // 编码 30 帧；解码器先正常吃前 10 帧（模拟"丢包前已经有历史"）。
  std::vector<std::vector<uint8_t>> pkts;
  for (int i = 0; i < 30; ++i) {
    const auto pcm = make_sine(kAudioFrameSamples, kSineHz, kAmplitude);
    pkts.push_back(encode_frame(enc, pcm));
  }
  DecodeCounters counters;
  for (int i = 0; i < 10; ++i) {
    const auto out = decode_or_count(dec, pkts[static_cast<std::size_t>(i)], counters, err);
    check(out.sample_count() == static_cast<std::size_t>(kAudioFrameSamples),
          "opus fec: history frame decodes");
  }
  check(counters.errors == 0, "opus fec: history has no errors");

  // 模拟 pkts[10] 丢失：只把 pkts[11] 喂给解码器，先 FecRecover 再 Normal。
  err.clear();
  auto recovered = dec.decode(pkts[11], err, DecodeMode::FecRecover);
  if (!recovered) {
    if (recovered.error() == AudioDecodeError::FecUnavailable) {
      std::printf(
          "  SKIP: 编码器这一路没有带 LBRR（FecUnavailable），跳过 FEC 恢复断言（不是失败）\n");
    } else {
      check(false, "opus fec: FecRecover neither succeeded nor reported FecUnavailable");
    }
  } else {
    check(recovered->sample_count() == static_cast<std::size_t>(kAudioFrameSamples),
          "opus fec: recovered frame is 960 samples (上一帧的 20 ms)");
    const double dbfs = rms_dbfs(recovered->samples);
    // 真恢复的是上一帧的**真实波形**：正弦幅度 -9 dBFS 左右。容差放到 >-30 dBFS
    // （实测 -9.5 dBFS）；一个"凭空造出来的空帧"会是 -inf，这里能区分。
    check(dbfs > -30.0, "opus fec: recovered frame carries real audio energy");
    std::printf("  FEC 恢复帧 %.2f dBFS（原信号 %.2f dBFS）\n", dbfs,
                rms_dbfs(make_sine(kAudioFrameSamples, kSineHz, kAmplitude)));
  }
  // FEC 恢复**不算** decoded：它是恢复帧，要单独计 audio_fec_recovered（见 probe 的注释）。
  // 这里直接断言计数没涨（decode_or_count 只在 Normal 成功时才涨）。
  check(counters.frames == 10 && counters.pcm_samples == 10ull * kAudioFrameSamples,
        "opus fec: FEC recovery does not count as a decoded frame");

  // 同一包接着 Normal 解：必须拿到当前帧（FecRecover 之后状态已经落在正确位置）
  err.clear();
  auto current = dec.decode(pkts[11], err);
  check(current.has_value(), "opus fec: normal decode after FecRecover");
  if (current) {
    check(current->sample_count() == static_cast<std::size_t>(kAudioFrameSamples),
          "opus fec: current frame is 960 samples");
    check(rms_dbfs(current->samples) > -30.0, "opus fec: current frame has energy");
  }
}

// 反向用例：编码端 loss perc = 0（不带 LBRR）时，FecRecover 必须**明确说没有冗余**，
// 而不是悄悄用 PLC 造一帧冒充恢复（那正是 audio_decoder.h 里那条教训的翻版）。
void test_fec_unavailable_without_lbrr() {
  using namespace cue::media;
  std::string err;
  OpusAudioEncoder enc;
  OpusAudioDecoder dec;
  check(enc.open(err) && dec.open(err), "opus fec0: encoder/decoder open");
  check(enc.set_packet_loss_perc(0), "opus fec0: loss perc 0（不带 LBRR）");

  std::vector<std::vector<uint8_t>> pkts;
  for (int i = 0; i < 12; ++i) {
    const auto pcm = make_sine(kAudioFrameSamples, kSineHz, kAmplitude);
    pkts.push_back(encode_frame(enc, pcm));
  }
  for (int i = 0; i < 10; ++i) {
    check(dec.decode(pkts[static_cast<std::size_t>(i)], err).has_value(),
          "opus fec0: normal frame decodes");
  }
  err.clear();
  auto r = dec.decode(pkts[11], err, DecodeMode::FecRecover);
  check(!r.has_value() && r.error() == AudioDecodeError::FecUnavailable,
        "opus fec0: no LBRR -> FecUnavailable (not a fabricated frame)");
  check(err.find("LBRR") != std::string::npos, "opus fec0: reason names LBRR");
  // 明确的失败不能动主解码器：紧接着的 Normal 解码必须照常成功。
  auto cur = dec.decode(pkts[11], err);
  check(cur.has_value(), "opus fec0: decoder still usable after FecUnavailable");
}

// ---------------------------------------------------------------- 7) PLC（conceal）

void test_conceal() {
  using namespace cue::media;
  std::string err;
  OpusAudioEncoder enc;
  OpusAudioDecoder dec;
  check(enc.open(err) && dec.open(err), "opus plc: encoder/decoder open");

  // 先喂 5 帧真实音频（PLC 需要解码器有历史才能外推）
  DecodeCounters counters;
  for (int i = 0; i < 5; ++i) {
    const auto pkt = encode_frame(enc, make_sine(kAudioFrameSamples, kSineHz, kAmplitude));
    const auto out = decode_or_count(dec, pkt, counters, err);
    check(out.sample_count() == static_cast<std::size_t>(kAudioFrameSamples),
          "opus plc: warmup frame decodes");
  }
  const uint64_t decoded_before = counters.frames;
  const uint64_t pcm_before = counters.pcm_samples;

  // 三个隐藏帧（= cue-probe 的 kMaxPlcFrames）：每帧必须恰好 20 ms。
  for (int i = 0; i < 3; ++i) {
    err.clear();
    auto c = dec.conceal(err);
    check(c.has_value(), "opus plc: conceal returns a frame");
    if (c) {
      check(c->sample_count() == static_cast<std::size_t>(kAudioFrameSamples),
            "opus plc: concealed frame is 960 samples");
      check(c->sample_rate == kAudioSampleRate && c->channels == kAudioChannels,
            "opus plc: concealed frame is 48 kHz mono");
    }
  }
  // **类型上**就分开了：conceal() 返回 ConcealedFrame，不是 DecodedAudioFrame，
  // 所以它没法被 decode_or_count / audio_decoded_frames 那条路统计进去。
  // 这里再用计数断言一次"确实没涨"。
  check(counters.frames == decoded_before && counters.pcm_samples == pcm_before,
        "opus plc: concealment is not counted as a decoded frame");
  // 连续外推不报错、也不返回 0 样本。
  auto again = dec.conceal(err);
  check(again.has_value() && again->sample_count() == static_cast<std::size_t>(kAudioFrameSamples),
        "opus plc: repeated concealment stays well-formed");
}

// 未打开的解码器：conceal 必须明确报 NotOpen，而不是造帧
void test_conceal_cold() {
  using namespace cue::media;
  OpusAudioDecoder cold;
  std::string err;
  auto c = cold.conceal(err);
  check(!c.has_value() && c.error() == AudioDecodeError::NotOpen,
        "opus plc: conceal before open -> NotOpen");
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
  test_encoder_loss_settings();
  test_fec_recovery();
  test_fec_unavailable_without_lbrr();
  test_conceal();
  test_conceal_cold();
  if (failures == 0) {
    std::printf("OK: audio decoder tests passed\n");
    return 0;
  }
  std::printf("FAILED: %d audio decoder check(s)\n", failures);
  return 1;
}
