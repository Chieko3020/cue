// Cue 线协议：上行（浏览器 → cued，WebSocket 二进制）与下行（cued → probe，UDP）。
// 所有多字节整数为小端；时间戳单位微秒。
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
enum class DownType : uint8_t {
  Video = 0x01,
  Audio = 0x02,
  Fec = 0x03,
  Nack = 0x04,
  StatsReq = 0x05,
  Feedback = 0x06,
  // 接收端请求一个关键帧（对应 RTP/RTCP 的 PLI）。丢包打断 H.264 参考链之后，
  // 接收端除了"等下一个 IDR"之外没有任何恢复手段，而 IDR 间隔由发送端的 GOP 决定
  // （默认 30 帧 = 1 秒）。有了这条请求，恢复时间从"最坏一个 GOP"变成"一个 RTT"。
  // 载荷为空：接收端只表达"我需要一个能重新起步的点"，具体怎么做由发送端决定。
  KeyframeReq = 0x07,
};

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

// 上行包头的公共解包 + 尺寸类边界校验（分片语义由调用方决定，见下面两个入口）。
namespace detail {

inline MediaReject decode_media_header(std::span<const uint8_t> b, MediaHeader& out) {
  if (b.size() < kMediaHeaderSize) return MediaReject::Truncated;
  if (b[0] != kMagicUp) return MediaReject::BadMagic;
  MediaHeader h;
  h.magic = b[0];
  h.type = b[1];
  h.codec = b[2];
  h.flags = b[3];
  h.stream_id = get_u32(&b[4]);
  h.frame_id = get_u32(&b[8]);
  h.frag_index = get_u32(&b[12]);
  h.frag_count = get_u32(&b[16]);
  h.pts_us = get_u64(&b[20]);
  h.payload_len = get_u32(&b[28]);
  if (h.payload_len > b.size() - kMediaHeaderSize) return MediaReject::PayloadOverflow;
  if (h.payload_len > kMaxWsFrag) return MediaReject::FragmentTooLarge;
  out = h;
  return MediaReject::None;
}

}  // namespace detail

// 上行媒体包的协议边界校验，集中在这里（下游两个进程不再各自"补"）。
// 成功时 out 为解出的头；失败时返回具体原因，out 不可用。
inline MediaReject validate_media(std::span<const uint8_t> b, MediaHeader& out) {
  const MediaReject r = detail::decode_media_header(b, out);
  if (r != MediaReject::None) return r;
  if (out.frag_count == 0) return MediaReject::FragCountZero;
  if (out.frag_count > kMaxFragPerFrame) return MediaReject::FragCountTooLarge;
  if (out.frag_index >= out.frag_count) return MediaReject::FragIndexOutOfRange;
  return MediaReject::None;
}

// 控制类上行包（codec_config 0x03 / keyframe_req 0x04）不使用分片语义：
// 发送端常把 frag_count/frag_index 留 0，按媒体包规则会被 FragCountZero 直接拒掉。
// 这里只放宽分片三项，尺寸/magic/越界仍然严格（超大 payload 照样拒，不额外分配内存）。
inline MediaReject validate_control(std::span<const uint8_t> b, MediaHeader& out) {
  return detail::decode_media_header(b, out);
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
  // 载荷的 codec（Codec 枚举；0 = 发送端未标注）。原来是保留字节、恒为 0，
  // 现在由 cued 填真实值：probe 只有知道 codec 才能决定「解码」还是「跳过并记事件」，
  // 靠载荷猜（嗅 AVC 起始码）会把 VP8 当 H264 喂进解码器。字节位置与长度不变，
  // 旧 probe 读到非 0 也只当它保留位，线格式兼容。
  uint8_t codec = 0;
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
  b[3] = h.codec;
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
  h.codec = b[3];
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
  // FR-PIPE.9：每 N 个数据报一组，group_index 从 0 递增（同一帧内唯一）。
  // 组的**起始 frag 下标**写在数据报头的 frag_index 里，member_count 是本组成员数；
  // probe 用 (frag_index, member_count) 定位组员。旧的一帧一组格式
  // （frag_index=0、member_count=frag_count）因此仍然可解释，不需要协议版本位。
  uint16_t group_index = 0;
  uint16_t member_count = 0;  // 本组成员数；必须与「frag_count − frag_index」相容
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

// ---------------------------------------------------------------- FEC 分组（FR-PIPE.9）
// 分组规则与 XOR 运算收在头文件里：cued 生成、probe 恢复、单测验证用的是同一份实现，
// 否则「发送端按 8 分组、接收端按整帧恢复」这种不对称只能靠联调发现。
inline constexpr std::size_t kMaxFecGroupSize = 64;
inline constexpr std::size_t kDefaultFecGroupSize = 8;

// 一组在帧内的位置：offset = 第一个成员的 frag_index，member_count = 成员数。
struct FecGroupSpan {
  uint16_t offset = 0;
  uint16_t member_count = 0;
};

inline std::vector<FecGroupSpan> fec_group_spans(std::size_t frag_count, std::size_t group_size) {
  std::vector<FecGroupSpan> out;
  if (frag_count == 0) return out;
  const std::size_t n = std::clamp<std::size_t>(group_size, 1, kMaxFecGroupSize);
  for (std::size_t off = 0; off < frag_count; off += n) {
    const std::size_t len = std::min(n, frag_count - off);
    out.push_back(FecGroupSpan{static_cast<uint16_t>(off), static_cast<uint16_t>(len)});
  }
  return out;
}

// 一组载荷的 XOR 冗余：各成员按最长者补零后逐字节异或。
inline std::vector<uint8_t> fec_xor_parity(std::span<const std::vector<uint8_t>> members) {
  std::size_t maxlen = 0;
  for (const auto& m : members) maxlen = std::max(maxlen, m.size());
  std::vector<uint8_t> parity(maxlen, 0);
  for (const auto& m : members) {
    for (std::size_t i = 0; i < m.size(); ++i) parity[i] ^= m[i];
  }
  return parity;
}

// 用 parity + 其余成员恢复唯一缺失的那一片。恰好缺 1 片才成功（缺 0 片无从判断长度，
// 缺 ≥2 片是欠定方程）。members 里的空向量视为「缺失」。
//
// 恢复出的长度 = parity 长度（该组最长载荷）：若丢的是较短的一片，尾部多出的 0 是
// 补零异或的产物。Annex-B 允许 trailing_zero_8bits，所以不影响解码；协议本身不携带
// 每片长度，这是 XOR-FEC 的固有限制（与实现无关）。
inline bool fec_recover_one(std::span<const std::vector<uint8_t>> members,
                            std::span<const uint8_t> parity, std::size_t& missing_index,
                            std::vector<uint8_t>& recovered) {
  std::size_t miss = members.size();
  std::size_t n_missing = 0;
  for (std::size_t i = 0; i < members.size(); ++i) {
    if (members[i].empty()) {
      miss = i;
      ++n_missing;
    }
  }
  if (n_missing != 1 || parity.empty()) return false;
  recovered.assign(parity.begin(), parity.end());
  for (std::size_t i = 0; i < members.size(); ++i) {
    if (i == miss) continue;
    const auto& m = members[i];
    for (std::size_t k = 0; k < m.size() && k < recovered.size(); ++k) recovered[k] ^= m[k];
  }
  missing_index = miss;
  return true;
}

// ---------------------------------------------------------------- feedback（技术设计 §4.2）
// probe → cued，type=0x06，载荷 4 个 f32（小端）：
//   loss_rate | recv_bitrate_mbps | jitter_ms | rtt_ms
// 「没有可靠值」写 -1，不写 0：0 是合法读数（零丢包/零时延），拿它冒充缺失会让
// cued 的 AIMD 以为链路完美而升码率。f32 而不是 f64：这是控制反馈，1% 精度足够，
// 且 16 字节载荷可以塞进任何 MTU。
inline constexpr std::size_t kFeedbackPayloadSize = 16;

struct FeedbackPayload {
  float loss_rate = 0.0f;
  float recv_bitrate_mbps = -1.0f;
  float jitter_ms = -1.0f;
  float rtt_ms = -1.0f;
};

inline std::array<uint8_t, kFeedbackPayloadSize> encode(const FeedbackPayload& h) {
  std::array<uint8_t, kFeedbackPayloadSize> b{};
  detail::put_u32(&b[0], std::bit_cast<uint32_t>(h.loss_rate));
  detail::put_u32(&b[4], std::bit_cast<uint32_t>(h.recv_bitrate_mbps));
  detail::put_u32(&b[8], std::bit_cast<uint32_t>(h.jitter_ms));
  detail::put_u32(&b[12], std::bit_cast<uint32_t>(h.rtt_ms));
  return b;
}

inline std::optional<FeedbackPayload> decode_feedback(std::span<const uint8_t> b) {
  if (b.size() < kFeedbackPayloadSize) return std::nullopt;
  FeedbackPayload h;
  h.loss_rate = std::bit_cast<float>(detail::get_u32(&b[0]));
  h.recv_bitrate_mbps = std::bit_cast<float>(detail::get_u32(&b[4]));
  h.jitter_ms = std::bit_cast<float>(detail::get_u32(&b[8]));
  h.rtt_ms = std::bit_cast<float>(detail::get_u32(&b[12]));
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

// 载荷 codec 未标注（旧发送端 / 非媒体包）：probe 见到它只能记 skipped，不能猜着解码
inline constexpr uint8_t kDownCodecUnspecified = 0;

// 把一帧切成 ≤kMaxUdpPayload 的分片，返回每个分片的完整数据报（头 + payload）。
// why 非空时写回失败原因（成功写 None）。codec 由调用方给载荷的真实 codec（见 DatagramHeader）。
inline std::vector<std::vector<uint8_t>> split_into_datagrams(
    std::span<const uint8_t> frame, uint32_t session_id, uint32_t frame_id,
    uint64_t pts_us, bool keyframe, SplitError* why = nullptr,
    uint8_t codec = kDownCodecUnspecified) {
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
    h.codec = codec;
    auto hdr = encode(h);
    std::vector<uint8_t> pkt(hdr.begin(), hdr.end());
    pkt.insert(pkt.end(), frame.data() + off, frame.data() + off + len);
    out.push_back(std::move(pkt));
  }
  return out;
}

// ---------------------------------------------------------------- 结构化事件行（stdout）
// cued / cue-probe 在关键节点往 stdout 写一行事件，供 harness 的 reader 线程逐行解析
// （`ProcessPipelineControl` 只认整行的 kEventTag 前缀，与 `[cued]` / `[probe]` 人读行共存）。
// 格式固定，且**必须**是合法 JSON 单行：
//   [cue-event] {"ev":"<type>","t":<unix_us>,"data":{...}}
// 这里只收敛「标签 + 落盘方式」，JSON 体由调用方序列化——wire.h 不引 JSON 依赖。
inline constexpr std::string_view kEventTag = "[cue-event] ";

// 整行一次 fwrite + fflush：行不会被 stdio 的分块写入或别的线程截成两半。
// body 必须是已序列化的 JSON 对象（调用方负责合法性与单行性）。
inline void emit_event_line(std::string_view body) {
  std::string line;
  line.reserve(kEventTag.size() + body.size() + 1);
  line.append(kEventTag);
  line.append(body);
  line.push_back('\n');
  (void)std::fwrite(line.data(), 1, line.size(), stdout);
  (void)std::fflush(stdout);
}

}  // namespace cue::proto
