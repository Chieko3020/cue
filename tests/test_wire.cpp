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

  // ---- 控制包（codec_config 0x03 / keyframe_req 0x04）：frag_count=0 合法 ----
  // 发送端常把控制包的 frag_count 留 0；按媒体包规则会被 FragCountZero 拒掉（M5 前置的坑）
  auto ctl = up;
  ctl[1] = static_cast<uint8_t>(MediaType::CodecConfig);
  detail::put_u32(&ctl[16], 0);  // frag_count
  detail::put_u32(&ctl[12], 0);  // frag_index
  MediaHeader cm;
  check(validate_media({ctl.data(), ctl.size()}, cm) == MediaReject::FragCountZero,
        "media 规则：frag_count=0 仍被拒");
  check(validate_control({ctl.data(), ctl.size()}, cm) == MediaReject::None,
        "control 规则：frag_count=0 放行");
  check(cm.type == static_cast<uint8_t>(MediaType::CodecConfig), "control 包类型正确");
  check(cm.payload_len == 16, "control payload_len 正确");
  // 放宽的只有分片三项：尺寸/magic/越界仍然严格
  check(validate_control({ctl.data(), kMediaHeaderSize - 1}, cm) == MediaReject::Truncated,
        "control truncated rejected");
  auto ctl_bad_magic = ctl;
  ctl_bad_magic[0] = 0xD1;
  check(validate_control({ctl_bad_magic.data(), ctl_bad_magic.size()}, cm) == MediaReject::BadMagic,
        "control wrong magic rejected");
  auto ctl_over = ctl;
  detail::put_u32(&ctl_over[28], static_cast<uint32_t>(kMaxWsFrag + 1));
  check(validate_control({ctl_over.data(), ctl_over.size()}, cm) == MediaReject::PayloadOverflow,
        "control 声明超大 payload 被拒（不给伪造长度分配内存）");
  auto ctl_big_frag = ctl;
  detail::put_u32(&ctl_big_frag[16], kMaxFragPerFrame + 1);  // 控制包不看 frag_count
  check(validate_control({ctl_big_frag.data(), ctl_big_frag.size()}, cm) == MediaReject::None,
        "control 不受 frag_count 上界约束");

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
