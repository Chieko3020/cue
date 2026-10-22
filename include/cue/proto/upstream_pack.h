// 上行媒体包的分片打包：把「一帧已编码载荷」切成 wire.h 的 32 字节头 + ≤16 KiB 载荷。
//
// 为什么单独一个头文件而不是散在发送端里：浏览器（web/app.js）、Node 模拟端
// （scripts/lib/upstream-wire.mjs）与原生采集端（apps/cue-capture.cpp）三处必须产出
// **逐字节相同**的包，否则「协议不分叉」只是口号。三处各自手写一遍分片循环，
// 迟早有一处把 last_frag 写成「第一片」或者把 keyframe 位只放在首片。
// 这里给出 C++ 唯一实现，tests/test_capture.cpp 用 wire.h 的 validate_media 反向校验，
// scripts/compare-upstream-wire.mjs 与 JS 实现逐字节对照。
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "cue/proto/wire.h"

namespace cue::proto {

// 一帧的上行元信息，字段与 web/app.js 的 sendMediaPacket(meta, bytes) 一一对应。
// flags 不在入参里：keyframe/last_frag 由打包函数按协议规则填（见下），
// 调用方不该有机会把「最后一片没打 last_frag」这种错误带进来。
struct UpstreamMedia {
  uint8_t type = static_cast<uint8_t>(MediaType::Video);
  uint8_t codec = 0;
  uint32_t stream_id = 0;
  uint32_t frame_id = 0;
  uint64_t pts_us = 0;
  bool keyframe = false;
};

// 打包规则（与 web/app.js:192-209、scripts/lib/upstream-wire.mjs、test-client.mjs 一致）：
//   frag_count = max(1, ceil(len / kMaxWsFrag))，空载荷也算 1 片（payload_len=0）
//   每片都带 keyframe 位；只有最后一片带 last_frag 位
//   分片按顺序负载填充，第 i 片载荷 = bytes[i*16KiB, min((i+1)*16KiB, len))
//
// 空载荷在这里是**合法**的（协议层没有禁止 0 字节帧；浏览器端 JS 的 Math.max(1, ...)
// 也会产出「1 片 0 载荷」）。但 0 字节帧解不出任何东西，所以发送端（cue-capture）在
// 调用前就把空帧丢掉并计入 dropped —— 打包函数不替调用方做这个策略决定。
inline std::vector<std::vector<uint8_t>> pack_upstream(std::span<const uint8_t> payload,
                                                       const UpstreamMedia& m) {
  const std::size_t frag_count =
      payload.empty() ? 1 : (payload.size() + kMaxWsFrag - 1) / kMaxWsFrag;
  std::vector<std::vector<uint8_t>> out;
  out.reserve(frag_count);
  for (std::size_t i = 0; i < frag_count; ++i) {
    const std::size_t off = i * kMaxWsFrag;
    const std::size_t len = off < payload.size() ? std::min(kMaxWsFrag, payload.size() - off) : 0;
    MediaHeader h;
    h.type = m.type;
    h.codec = m.codec;
    h.stream_id = m.stream_id;
    h.frame_id = m.frame_id;
    h.frag_index = static_cast<uint32_t>(i);
    h.frag_count = static_cast<uint32_t>(frag_count);
    h.pts_us = m.pts_us;
    h.flags = static_cast<uint8_t>((m.keyframe ? kUpFlagKeyframe : 0) |
                                   (i + 1 == frag_count ? kUpFlagLastFrag : 0));
    h.payload_len = static_cast<uint32_t>(len);
    const auto hdr = encode(h);
    std::vector<uint8_t> pkt;
    pkt.reserve(kMediaHeaderSize + len);
    pkt.insert(pkt.end(), hdr.begin(), hdr.end());
    pkt.insert(pkt.end(), payload.begin() + static_cast<std::ptrdiff_t>(off),
               payload.begin() + static_cast<std::ptrdiff_t>(off + len));
    out.push_back(std::move(pkt));
  }
  return out;
}

// 单帧可打包上界的协议判据：分片数超过 kMaxFragPerFrame 的帧，接收端会以
// FragCountTooLarge 直接拒掉。发送端据此在打包前丢帧并计数，而不是让对端去拒。
inline constexpr std::size_t kMaxUpstreamFrameBytes =
    static_cast<std::size_t>(kMaxFragPerFrame) * kMaxWsFrag;

inline bool upstream_frame_fits(std::size_t payload_bytes) {
  return payload_bytes <= kMaxUpstreamFrameBytes;
}

}  // namespace cue::proto
