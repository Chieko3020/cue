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

inline std::optional<MediaHeader> decode_media(std::span<const uint8_t> b) {
  if (b.size() < kMediaHeaderSize) return std::nullopt;
  if (b[0] != kMagicUp) return std::nullopt;
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
  if (h.payload_len > b.size() - kMediaHeaderSize) return std::nullopt;
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

// 把一帧切成 ≤kMaxUdpPayload 的分片，返回每个分片的完整数据报（头 + payload）。
inline std::vector<std::vector<uint8_t>> split_into_datagrams(
    std::span<const uint8_t> frame, uint32_t session_id, uint32_t frame_id,
    uint64_t pts_us, bool keyframe) {
  std::vector<std::vector<uint8_t>> out;
  if (frame.empty()) return out;
  const std::size_t total = (frame.size() + kMaxUdpPayload - 1) / kMaxUdpPayload;
  if (total > 1024) return out;  // 超限帧直接丢弃（M1 不处理）
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
