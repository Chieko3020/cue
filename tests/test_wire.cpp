// proto 编解码往返与边界校验（M1 最小测试；不依赖测试框架，用 ctest 的退出码）
#include <cstdio>
#include <vector>

#include "cue/proto/wire.h"

namespace {

int failures = 0;

void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

}  // namespace

int main() {
  using namespace cue::proto;

  // ---- 上行媒体头往返 ----
  MediaHeader h;
  h.type = static_cast<uint8_t>(MediaType::Video);
  h.codec = static_cast<uint8_t>(Codec::H264);
  h.flags = kUpFlagKeyframe | kUpFlagLastFrag;
  h.stream_id = 1;
  h.frame_id = 42;
  h.frag_index = 3;
  h.frag_count = 7;
  h.pts_us = 1234567890123ULL;
  h.payload_len = 16;

  // 构造「32 字节头 + 16 字节 payload」；payload_len 必须与实际长度一致，否则 decode 会拒绝
  const auto hdr = encode(h);
  std::vector<uint8_t> up(hdr.begin(), hdr.end());
  up.resize(kMediaHeaderSize + 16, 0x5A);

  auto decoded = decode_media({up.data(), up.size()});
  check(decoded.has_value(), "media decode ok");
  if (decoded) {
    check(decoded->magic == kMagicUp, "media magic");
    check(decoded->frame_id == 42, "media frame_id");
    check(decoded->frag_index == 3, "media frag_index");
    check(decoded->frag_count == 7, "media frag_count");
    check(decoded->pts_us == 1234567890123ULL, "media pts_us");
    check(decoded->payload_len == 16, "media payload_len");
    check(decoded->flags == (kUpFlagKeyframe | kUpFlagLastFrag), "media flags");
  }

  // ---- 下行数据报头往返 ----
  DatagramHeader dh;
  dh.type = static_cast<uint8_t>(DownType::Video);
  dh.flags = kDownFlagKeyframe;
  dh.session_id = 9;
  dh.frame_id = 1000;
  dh.frag_index = 5;
  dh.frag_count = 9;
  dh.pts_us = 987654321ULL;

  auto down = encode(dh);
  auto ddec = decode_datagram({down.data(), down.size()});
  check(ddec.has_value(), "datagram decode ok");
  if (ddec) {
    check(ddec->session_id == 9, "datagram session_id");
    check(ddec->frame_id == 1000, "datagram frame_id");
    check(ddec->frag_index == 5, "datagram frag_index");
    check(ddec->frag_count == 9, "datagram frag_count");
    check(ddec->pts_us == 987654321ULL, "datagram pts_us");
    check((ddec->flags & kDownFlagKeyframe) != 0, "datagram keyframe flag");
  }

  // ---- 边界：截断 / 错 magic 必须被拒绝 ----
  check(!decode_media({up.data(), kMediaHeaderSize - 1}).has_value(), "media truncated rejected");
  check(!decode_datagram({down.data(), kDatagramHeaderSize - 1}).has_value(), "datagram truncated rejected");

  auto bad = up;
  bad[0] = 0xD1;
  check(!decode_media({bad.data(), bad.size()}).has_value(), "media wrong magic rejected");

  // payload_len 超出实际长度必须拒绝
  auto lying = up;
  lying[28] = 0xFF;
  lying[29] = 0xFF;
  lying[30] = 0x00;
  lying[31] = 0x00;
  check(!decode_media({lying.data(), lying.size()}).has_value(), "media payload_len overflow rejected");

  // ---- 分片：一帧切成多包，头字段自洽 ----
  std::vector<uint8_t> frame(kMaxUdpPayload * 2 + 10, 0xAB);
  auto pkts = split_into_datagrams(frame, /*session*/ 3, /*frame*/ 77, /*pts*/ 555, /*key*/ true);
  check(pkts.size() == 3, "split into 3 datagrams");
  for (std::size_t i = 0; i < pkts.size(); ++i) {
    auto ph = decode_datagram({pkts[i].data(), pkts[i].size()});
    check(ph.has_value(), "split header decodes");
    if (!ph) continue;
    check(ph->frag_index == i, "split frag_index order");
    check(ph->frag_count == 3, "split frag_count");
    check(ph->frame_id == 77, "split frame_id");
    check(ph->session_id == 3, "split session_id");
    check((ph->flags & kDownFlagKeyframe) != 0, "split keyframe flag");
  }

  // 空帧不产生分片
  check(split_into_datagrams({}, 1, 1, 0, false).empty(), "empty frame -> no datagrams");

  if (failures == 0) {
    std::printf("test_wire: OK\n");
    return 0;
  }
  std::printf("test_wire: %d failure(s)\n", failures);
  return 1;
}
