// Cue 线协议：上行（浏览器 → cued，WebSocket 二进制）与下行（cued → probe，UDP）。
// 所有多字节整数为小端；时间戳单位微秒。
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace cue::proto {

inline constexpr uint8_t kMagicUp = 0xC1;
inline constexpr uint8_t kMagicDown = 0xD1;
inline constexpr std::size_t kMediaHeaderSize = 32;
inline constexpr std::size_t kDatagramHeaderSize = 24;
inline constexpr std::size_t kMaxWsFrag = 16 * 1024;   // 上行单分片载荷上限
inline constexpr std::size_t kMaxUdpPayload = 1200;    // 下行单包载荷上限（避免 IP 分片）
// 单帧最大分片数：与下行 split_into_datagrams 的 1024 上限保持一致。
// 这同时是 frag_count 的协议上界（伪造 u32 会被 decode_media 直接拒绝，不再分配内存）。
inline constexpr uint32_t kMaxFragPerFrame = 1024;

enum class MediaType : uint8_t { Video = 0x01, Audio = 0x02, CodecConfig = 0x03, KeyframeReq = 0x04, Stats = 0x05 };
enum class Codec : uint8_t { H264 = 0x01, VP8 = 0x02, Opus = 0x03 };
enum class DownType : uint8_t { Video = 0x01, Audio = 0x02, Fec = 0x03, Nack = 0x04, StatsReq = 0x05, Feedback = 0x06 };

// 上行 flags
inline constexpr uint8_t kUpFlagKeyframe = 0x01;
inline constexpr uint8_t kUpFlagConfig = 0x02;
inline constexpr uint8_t kUpFlagLastFrag = 0x04;

// 下行 flags
inline constexpr uint8_t kDownFlagKeyframe = 0x01;
inline constexpr uint8_t kDownFlagRetransmit = 0x02;
inline constexpr uint8_t kDownFlagFecRepaired = 0x04;

namespace detail {

inline void put_u16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
inline void put_u32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
inline void put_u64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
inline uint16_t get_u16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
inline uint32_t get_u32(const uint8_t* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
  return v;
}
inline uint64_t get_u64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}

}  // namespace detail

// ---------------------------------------------------------------- 上行媒体包
struct MediaHeader {
  uint8_t magic = kMagicUp;
  uint8_t type = 0;   // MediaType
  uint8_t codec = 0;  // Codec
  uint8_t flags = 0;
  uint32_t stream_id = 0;
  uint32_t frame_id = 0;
  uint32_t frag_index = 0;
  uint32_t frag_count = 0;
  uint64_t pts_us = 0;
  uint32_t payload_len = 0;
};

inline std::array<uint8_t, kMediaHeaderSize> encode(const MediaHeader& h) {
  std::array<uint8_t, kMediaHeaderSize> b{};
  b[0] = h.magic;
  b[1] = h.type;
  b[2] = h.codec;
  b[3] = h.flags;
  detail::put_u32(&b[4], h.stream_id);
  detail::put_u32(&b[8], h.frame_id);
  detail::put_u32(&b[12], h.frag_index);
  detail::put_u32(&b[16], h.frag_count);
  detail::put_u64(&b[20], h.pts_us);
  detail::put_u32(&b[28], h.payload_len);
  return b;
}

// decode_media 的拒绝原因：调用方（cued）可以据此打限流日志，而不是静默 continue
enum class MediaReject : uint8_t {
  None = 0,
  Truncated,        // 短于 32 字节头
  BadMagic,         // b[0] != 0xC1
  PayloadOverflow,  // payload_len > 实际剩余字节
  FragmentTooLarge, // payload_len > kMaxWsFrag
  FragCountZero,    // frag_count == 0（会凭空推一帧）
  FragCountTooLarge,// frag_count > kMaxFragPerFrame
  FragIndexOutOfRange,  // frag_index >= frag_count
};

inline const char* media_reject_reason(MediaReject r) {
  switch (r) {
    case MediaReject::None: return "ok";
    case MediaReject::Truncated: return "truncated";
    case MediaReject::BadMagic: return "bad_magic";
    case MediaReject::PayloadOverflow: return "payload_overflow";
    case MediaReject::FragmentTooLarge: return "fragment_too_large";
    case MediaReject::FragCountZero: return "frag_count_zero";
    case MediaReject::FragCountTooLarge: return "frag_count_too_large";
    case MediaReject::FragIndexOutOfRange: return "frag_index_out_of_range";
  }
  return "unknown";
}

// 上行媒体包的协议边界校验，集中在这里（下游两个进程不再各自"补"）。
// 成功时 out 为解出的头；失败时返回具体原因，out 不可用。
inline MediaReject validate_media(std::span<const uint8_t> b, MediaHeader& out) {
  if (b.size() < kMediaHeaderSize) return MediaReject::Truncated;
  if (b[0] != kMagicUp) return MediaReject::BadMagic;
  MediaHeader h;
  h.magic = b[0];
  h.type = b[1];
  h.codec = b[2];
  h.flags = b[3];
  h.stream_id = detail::get_u32(&b[4]);
  h.frame_id = detail::get_u32(&b[8]);
  h.frag_index = detail::get_u32(&b[12]);
  h.frag_count = detail::get_u32(&b[16]);
  h.pts_us = detail::get_u64(&b[20]);
  h.payload_len = detail::get_u32(&b[28]);
  if (h.payload_len > b.size() - kMediaHeaderSize) return MediaReject::PayloadOverflow;
  if (h.payload_len > kMaxWsFrag) return MediaReject::FragmentTooLarge;
  if (h.frag_count == 0) return MediaReject::FragCountZero;
  if (h.frag_count > kMaxFragPerFrame) return MediaReject::FragCountTooLarge;
  if (h.frag_index >= h.frag_count) return MediaReject::FragIndexOutOfRange;
  out = h;
  return MediaReject::None;
}

inline std::optional<MediaHeader> decode_media(std::span<const uint8_t> b) {
  MediaHeader h;
  if (validate_media(b, h) != MediaReject::None) return std::nullopt;
  return h;
}

// ---------------------------------------------------------------- 下行数据报
struct DatagramHeader {
  uint8_t magic = kMagicDown;
  uint8_t type = 0;  // DownType
  uint8_t flags = 0;
  uint8_t reserved = 0;
  uint32_t session_id = 0;
  uint32_t frame_id = 0;
  uint16_t frag_index = 0;
  uint16_t frag_count = 0;
  uint64_t pts_us = 0;
};

inline std::array<uint8_t, kDatagramHeaderSize> encode(const DatagramHeader& h) {
  std::array<uint8_t, kDatagramHeaderSize> b{};
  b[0] = h.magic;
  b[1] = h.type;
  b[2] = h.flags;
  b[3] = h.reserved;
  detail::put_u32(&b[4], h.session_id);
  detail::put_u32(&b[8], h.frame_id);
  detail::put_u16(&b[12], h.frag_index);
  detail::put_u16(&b[14], h.frag_count);
  detail::put_u64(&b[16], h.pts_us);
  return b;
}

inline std::optional<DatagramHeader> decode_datagram(std::span<const uint8_t> b) {
  if (b.size() < kDatagramHeaderSize) return std::nullopt;
  if (b[0] != kMagicDown) return std::nullopt;
  DatagramHeader h;
  h.magic = b[0];
  h.type = b[1];
  h.flags = b[2];
  h.reserved = b[3];
  h.session_id = detail::get_u32(&b[4]);
  h.frame_id = detail::get_u32(&b[8]);
  h.frag_index = detail::get_u16(&b[12]);
  h.frag_count = detail::get_u16(&b[14]);
  h.pts_us = detail::get_u64(&b[16]);
  return h;
}

// ---------------------------------------------------------------- NACK / FEC 载荷
// NACK 条目与 FEC 载荷统一在这里编解码：两端各写一份 memcpy 的旧写法，谁先把一边
// 改成小端谁就静默不兼容（review #18）。
inline constexpr std::size_t kNackEntrySize = 6;
inline constexpr std::size_t kMaxNackEntries = 64;  // SPEC §7.2：单包 ≤ 64 条

struct NackEntry {
  uint32_t frame_id = 0;
  uint16_t frag_index = 0;
};

inline void append_nack_entry(std::vector<uint8_t>& out, const NackEntry& e) {
  const std::size_t off = out.size();
  out.resize(off + kNackEntrySize);
  detail::put_u32(&out[off], e.frame_id);
  detail::put_u16(&out[off + 4], e.frag_index);
}

inline std::vector<NackEntry> decode_nack_entries(std::span<const uint8_t> b) {
  const std::size_t n = b.size() / kNackEntrySize;
  std::vector<NackEntry> out;
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const uint8_t* p = b.data() + i * kNackEntrySize;
    out.push_back(NackEntry{detail::get_u32(p), detail::get_u16(p + 4)});
  }
  return out;
}

// FEC 载荷：u32 base_frame_id | u16 group_index | u16 member_count | data（SPEC §7.2）
inline constexpr std::size_t kFecPayloadHeaderSize = 8;

struct FecPayloadHeader {
  uint32_t base_frame_id = 0;
  uint16_t group_index = 0;   // 一帧一组：显式写 0，不再靠"整段零填充"隐式约定
  uint16_t member_count = 0;  // 必须与数据报头的 frag_count 一致（probe 侧交叉校验）
};

inline std::array<uint8_t, kFecPayloadHeaderSize> encode(const FecPayloadHeader& h) {
  std::array<uint8_t, kFecPayloadHeaderSize> b{};
  detail::put_u32(&b[0], h.base_frame_id);
  detail::put_u16(&b[4], h.group_index);
  detail::put_u16(&b[6], h.member_count);
  return b;
}

inline std::optional<FecPayloadHeader> decode_fec_payload_header(std::span<const uint8_t> b) {
  if (b.size() < kFecPayloadHeaderSize) return std::nullopt;
  FecPayloadHeader h;
  h.base_frame_id = detail::get_u32(&b[0]);
  h.group_index = detail::get_u16(&b[4]);
  h.member_count = detail::get_u16(&b[6]);
  return h;
}

// ---------------------------------------------------------------- 下行分片
// 切片失败的原因：调用方（cued）要能区分"空帧"与"超协议上限"，而不是一律当丢帧（review #34）
enum class SplitError : uint8_t { None = 0, EmptyFrame, TooManyFragments };

inline const char* split_error_reason(SplitError e) {
  switch (e) {
    case SplitError::None: return "ok";
    case SplitError::EmptyFrame: return "empty_frame";
    case SplitError::TooManyFragments: return "too_many_fragments";
  }
  return "unknown";
}

// 把一帧切成 ≤kMaxUdpPayload 的分片，返回每个分片的完整数据报（头 + payload）。
// why 非空时写回失败原因（成功写 None）。
inline std::vector<std::vector<uint8_t>> split_into_datagrams(
    std::span<const uint8_t> frame, uint32_t session_id, uint32_t frame_id,
    uint64_t pts_us, bool keyframe, SplitError* why = nullptr) {
  if (why) *why = SplitError::None;
  std::vector<std::vector<uint8_t>> out;
  if (frame.empty()) {
    if (why) *why = SplitError::EmptyFrame;
    return out;
  }
  const std::size_t total = (frame.size() + kMaxUdpPayload - 1) / kMaxUdpPayload;
  if (total > kMaxFragPerFrame) {
    if (why) *why = SplitError::TooManyFragments;  // 超限帧不再静默丢弃
    return out;
  }
  out.reserve(total);
  for (std::size_t i = 0; i < total; ++i) {
    const std::size_t off = i * kMaxUdpPayload;
    const std::size_t len = std::min(kMaxUdpPayload, frame.size() - off);
    DatagramHeader h;
    h.type = static_cast<uint8_t>(DownType::Video);
    h.flags = static_cast<uint8_t>(keyframe ? kDownFlagKeyframe : 0);
    h.session_id = session_id;
    h.frame_id = frame_id;
    h.frag_index = static_cast<uint16_t>(i);
    h.frag_count = static_cast<uint16_t>(total);
    h.pts_us = pts_us;
    auto hdr = encode(h);
    std::vector<uint8_t> pkt(hdr.begin(), hdr.end());
    pkt.insert(pkt.end(), frame.data() + off, frame.data() + off + len);
    out.push_back(std::move(pkt));
  }
  return out;
}

}  // namespace cue::proto
