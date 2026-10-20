// Spike-C：自研 UDP 下行协议的最小实现（SPEC §7.2 的 24 字节头）。
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace cue_spike::proto {

constexpr uint8_t kMagicDown = 0xD1;
constexpr std::size_t kMaxPayload = 1200 - 24;  // 1176

enum class Type : uint8_t {
  kVideo = 0x01,
  kAudio = 0x02,
  kFec = 0x03,
  kNack = 0x04,
  kStatsReq = 0x05,
  kFeedback = 0x06,
};

enum Flags : uint8_t {
  kFlagKeyframe = 1u << 0,
  kFlagRetransmit = 1u << 1,
  kFlagFecRepaired = 1u << 2,
};

#pragma pack(push, 1)
struct Header {
  uint8_t magic;
  uint8_t type;
  uint8_t flags;
  uint8_t reserved;
  uint32_t session_id;
  uint32_t frame_id;
  uint16_t frag_index;
  uint16_t frag_count;
  uint64_t pts_us;  // 生产环境为媒体 PTS；本 spike 直接写发送端 steady_clock 微秒（同机可比）
};
static_assert(sizeof(Header) == 24, "SPEC §7.2 要求 24 字节头");

struct FecPayload {
  uint32_t base_frame_id;
  uint16_t group_index;
  uint16_t member_count;
};

struct NackEntry {
  uint32_t frame_id;
  uint16_t frag_index;
};
static_assert(sizeof(NackEntry) == 6);
#pragma pack(pop)

// 帧内容的确定性生成函数：两端用同一函数校验重组正确性。
inline uint8_t payload_byte(uint32_t frame_id, uint32_t offset) {
  uint32_t x = frame_id * 2654435761u + offset * 2246822519u;
  x ^= x >> 15;
  x *= 0x2545F491u;
  x ^= x >> 13;
  return static_cast<uint8_t>(x & 0xFF);
}

// 整帧 FNV-1a，用于发现错拼/丢片（组错位会被抓到）。
inline uint32_t frame_hash(uint32_t frame_id, uint32_t frame_bytes) {
  uint32_t h = 2166136261u;
  for (uint32_t i = 0; i < frame_bytes; ++i) {
    h ^= payload_byte(frame_id, i);
    h *= 16777619u;
  }
  return h;
}

struct FrameSpec {
  uint32_t frame_id{0};
  uint32_t frame_bytes{0};
  uint16_t frag_count{0};
  uint16_t frag_payload{0};
};

}  // namespace cue_spike::proto
