// Annex-B 字节流的起始码切分与「访问单元（AU）」分帧。
//
// 为什么需要：`cue-capture` 的 ffmpeg 后端（以及 `lavfi --encode ffmpeg`）拿到的是
// x264 直接写进管道的 H.264 裸流，里面没有容器层告诉我们在哪里切帧。上行协议要求
// 一帧一个（或一组）包，所以必须按 Annex-B 语法自己切 NAL、再按 AU 边界成帧。
//
// 两种起始码都要认：
//   3 字节  00 00 01        —— 编码片（slice）前常用
//   4 字节  00 00 00 01     —— x264 在 AUD/SPS/PPS/SEI 前常用
// 只认一种的解析器会在本项目的实测里把下一帧 NAL 头的首字节吃掉（把 4 字节起始码的
// 最后一个 00 当载荷）或把上一帧载荷尾部的 00 当起始码的一部分。
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace cue::media {

// H.264 nal_unit_type（ITU-T H.264 §7.3.1）
inline constexpr uint8_t kNalTypeSliceNonIdr = 1;
inline constexpr uint8_t kNalTypeSlicePartA = 2;
inline constexpr uint8_t kNalTypeIdr = 5;
inline constexpr uint8_t kNalTypeSei = 6;
inline constexpr uint8_t kNalTypeSps = 7;
inline constexpr uint8_t kNalTypePps = 8;
inline constexpr uint8_t kNalTypeAud = 9;

// 一个 NAL 单元在输入缓冲里的位置。payload 不含起始码，含 NAL 头字节。
struct NalUnit {
  std::size_t offset = 0;          // 起始码首字节在输入缓冲里的偏移
  std::size_t start_code_len = 0;  // 3 或 4
  std::span<const uint8_t> payload;

  uint8_t type() const { return payload.empty() ? 0 : static_cast<uint8_t>(payload[0] & 0x1F); }
  bool is_vcl() const { return type() >= 1 && type() <= 5; }
  bool is_keyframe() const { return type() == kNalTypeIdr; }
};

// 切分一个（完整或部分）Annex-B 缓冲。最后一个 NAL 结束于缓冲末尾——它可能还没收全，
// 但在 NAL 层面这是唯一可能的切法；是否构成完整 AU 由 AnnexBAuAssembler 决定。
inline std::vector<NalUnit> split_annexb(std::span<const uint8_t> bytes) {
  std::vector<NalUnit> out;
  // 起始码只可能出现在 00 00 01（可能前面还有一个 00）。先找全位置，再按位置切，
  // 避免边扫边切时指针失效。
  std::vector<std::pair<std::size_t, std::size_t>> scs;  // (offset, len)
  for (std::size_t i = 0; i + 3 <= bytes.size();) {
    if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1) {
      if (i > 0 && bytes[i - 1] == 0) {
        // 4 字节起始码：前一个 00 属于起始码，不属上一帧载荷（Annex-B 的 trailing 0
        // 与 leading 0 在语法上等价，按 4 字节吃掉是 x264/ffmpeg 的通行做法）。
        scs.emplace_back(i - 1, 4);
      } else {
        scs.emplace_back(i, 3);
      }
      i += 3;
      continue;
    }
    ++i;
  }
  for (std::size_t k = 0; k < scs.size(); ++k) {
    const std::size_t start = scs[k].first + scs[k].second;
    const std::size_t end = (k + 1 < scs.size()) ? scs[k + 1].first : bytes.size();
    if (start >= end) continue;  // 连续起始码：空 NAL 不是合法 NAL，跳过
    out.push_back(NalUnit{scs[k].first, scs[k].second, bytes.subspan(start, end - start)});
  }
  return out;
}

enum class AuAssemblerError : uint8_t {
  None = 0,
  // 流里一直没有 AUD(9)。x264 的 aud=1 会让每个 AU 以 AUD 开头，这是本项目所有
  // ffmpeg 子进程参数都显式打开的；没有 AUD 说明有人换了参数或喂了别的裸流。
  // 这里**不**猜分帧（猜错会把多 slice 帧切成多帧发出去），如实报错。
  NoAud,
};

inline const char* au_assembler_error_name(AuAssemblerError e) {
  switch (e) {
    case AuAssemblerError::None: return "ok";
    case AuAssemblerError::NoAud: return "no_aud";
  }
  return "unknown";
}

// 从管道到帧的流式组装：feed 任意长度的字节块，返回其中已经完整的 AU。
// AU 的字节范围是 [AUD_k, AUD_{k+1})：含本帧的 AUD/SPS/PPS/SEI/slice 全部 NAL，
// 起始码原样保留（下游 cued 的 libavcodec 按 Annex-B 解析，需要它们）。
//
// 为什么以 AUD 为界：一个 AU = 一个编码帧，AUD 就是 H.264 里专门为此存在的分隔符；
// 靠「VCL NAL 数量/首片标志」猜多 slice 帧的分界需要逐位解析 Exp-Golomb，且一旦
// x264 换成多 slice 编码就会把一帧拆成多帧发出去（接收端表现为解码错误或花屏）。
class AnnexBAuAssembler {
 public:
  // 头 64 个 NAL 里都没有 AUD 就判 NoAud：容忍管道前几 KB 只收到 SPS/PPS 的情况，
  // 又不至于把一整条没有 AUD 的流静默按错误规则切完。
  static constexpr std::size_t kAudProbeNals = 64;

  std::expected<std::vector<std::vector<uint8_t>>, AuAssemblerError> feed(
      std::span<const uint8_t> bytes);

  // 收流结束：把缓冲区里最后一个 AU 交出去（它在流内没有「下一个 AUD」来界定末尾）。
  std::vector<std::vector<uint8_t>> flush();

  bool audio_detected() const { return aud_found_; }
  // 已见到的 NAL 数（仅用于报错信息与测试断言，不影响分帧）
  std::size_t nals_seen() const { return nals_seen_; }

 private:
  std::vector<uint8_t> buf_;
  bool aud_found_ = false;
  bool no_aud_ = false;
  std::size_t nals_seen_ = 0;
};

inline std::expected<std::vector<std::vector<uint8_t>>, AuAssemblerError>
AnnexBAuAssembler::feed(std::span<const uint8_t> bytes) {
  buf_.insert(buf_.end(), bytes.begin(), bytes.end());
  std::vector<std::vector<uint8_t>> out;

  for (;;) {
    const auto nals = split_annexb(buf_);
    if (!aud_found_ && !no_aud_) {
      for (const auto& n : nals) {
        if (n.type() == kNalTypeAud) {
          aud_found_ = true;
          break;
        }
      }
      nals_seen_ = nals.size();
      if (!aud_found_ && nals.size() >= kAudProbeNals) no_aud_ = true;
      if (no_aud_) return std::unexpected(AuAssemblerError::NoAud);
      if (!aud_found_) return out;  // 还没见到 AUD：先攒着，不做任何切分
    }
    nals_seen_ = nals.size();

    // 找前两个 AUD：第一个 AUD 到第二个 AUD 之间就是一个完整 AU。
    constexpr std::size_t kNoOffset = static_cast<std::size_t>(-1);
    std::size_t first = kNoOffset;
    std::size_t second = kNoOffset;
    for (const auto& n : nals) {
      if (n.type() != kNalTypeAud) continue;
      if (first == kNoOffset) {
        first = n.offset;
      } else {
        second = n.offset;
        break;
      }
    }
    if (second == kNoOffset) break;  // 还没有下一个 AU 的边界

    out.emplace_back(buf_.begin() + static_cast<std::ptrdiff_t>(first),
                     buf_.begin() + static_cast<std::ptrdiff_t>(second));
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(second));
  }
  return out;
}

inline std::vector<std::vector<uint8_t>> AnnexBAuAssembler::flush() {
  std::vector<std::vector<uint8_t>> out;
  if (!aud_found_ || buf_.empty()) return out;
  const auto nals = split_annexb(buf_);
  constexpr std::size_t kNoOffset = static_cast<std::size_t>(-1);
  std::size_t first = kNoOffset;
  for (const auto& n : nals) {
    if (n.type() == kNalTypeAud) {
      first = n.offset;
      break;
    }
  }
  if (first == kNoOffset) first = 0;
  if (first < buf_.size()) {
    out.emplace_back(buf_.begin() + static_cast<std::ptrdiff_t>(first), buf_.end());
  }
  buf_.clear();
  return out;
}

}  // namespace cue::media
