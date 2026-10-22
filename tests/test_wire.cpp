// proto 编解码往返与边界校验（M1 最小测试；不依赖测试框架，用 ctest 的退出码）
#include <algorithm>
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

  // ---- FEC 分组（FR-PIPE.9）：每 N 个数据包一组，group_index 递增 ----
  {
    const auto g8 = fec_group_spans(17, 8);
    check(g8.size() == 3, "17 片按 8 分组 = 3 组");
    if (g8.size() == 3) {
      check(g8[0].offset == 0 && g8[0].member_count == 8, "第 0 组 [0,8)");
      check(g8[1].offset == 8 && g8[1].member_count == 8, "第 1 组 [8,16)");
      check(g8[2].offset == 16 && g8[2].member_count == 1, "第 2 组只剩 1 片");
    }
    check(fec_group_spans(8, 8).size() == 1, "恰好 8 片 = 1 组");
    check(fec_group_spans(8, 64).size() == 1, "组大小 ≥ 帧分片数 = 整帧一组（旧行为）");
    check(fec_group_spans(0, 8).empty(), "0 片没有组");
    // 组大小被夹在 [1,64]：0 当 1、100 当 64
    check(fec_group_spans(3, 0).size() == 3, "组大小 0 当作 1");
    check(fec_group_spans(100, 100).size() == 2 &&
              fec_group_spans(100, 100)[0].member_count == 64,
          "组大小上限 64");
  }

  // ---- XOR 恢复：一组丢 1 片可恢复、丢 2 片不可恢复 ----
  {
    std::vector<std::vector<uint8_t>> members;
    for (std::size_t i = 0; i < 8; ++i) {
      members.push_back(std::vector<uint8_t>(100 + i, static_cast<uint8_t>(i * 17 + 3)));
    }
    const auto parity = fec_xor_parity(members);
    // 丢第 3 片（不是最长的那片：恢复长度 = parity 长度，尾部补 0 是协议固有限制）
    auto damaged = members;
    damaged[3].clear();
    std::size_t missing = 99;
    std::vector<uint8_t> rec;
    check(fec_recover_one(damaged, parity, missing, rec), "组内丢 1 片可恢复");
    check(missing == 3, "恢复的是缺失的那一片");
    check(rec.size() >= members[3].size() &&
              std::equal(members[3].begin(), members[3].end(), rec.begin()),
          "恢复内容与原件一致（尾部补 0 是 XOR-FEC 的固有长度限制）");
    // 丢 2 片：欠定，必须明确失败（不能返回半截数据）
    auto two = members;
    two[1].clear();
    two[5].clear();
    rec.clear();
    check(!fec_recover_one(two, parity, missing, rec), "组内丢 2 片不可恢复");
    // 丢 0 片：没有"唯一未知数"，同样不当成恢复
    check(!fec_recover_one(members, parity, missing, rec), "没有缺失时不做恢复");
    // 一组全在、另一组丢 1 片：多组场景下逐组恢复
    const auto spans = fec_group_spans(17, 8);
    std::vector<std::vector<uint8_t>> frame(17, std::vector<uint8_t>(32, 0x5A));
    frame[10].clear();  // 第 1 组的第 2 个成员
    std::size_t recovered = 0;
    for (const auto& s : spans) {
      const std::span<const std::vector<uint8_t>> group(frame.data() + s.offset, s.member_count);
      std::vector<uint8_t> par;
      if (s.offset == 8) {
        // 先拿完整的组算 parity，再让成员缺失（模拟"parity 先到、数据后丢"）
        std::vector<std::vector<uint8_t>> full(frame.begin() + s.offset,
                                               frame.begin() + s.offset + s.member_count);
        full[2] = std::vector<uint8_t>(32, 0x5A);
        par = fec_xor_parity(full);
      } else {
        par = fec_xor_parity(group);
      }
      std::size_t mi = 0;
      std::vector<uint8_t> r;
      if (cue::proto::fec_recover_one(group, par, mi, r)) {
        frame[s.offset + mi] = std::move(r);
        ++recovered;
      }
    }
    check(recovered == 1, "只恢复第 1 组缺的那一片");
    check(frame[10] == std::vector<uint8_t>(32, 0x5A), "恢复出的分片内容正确");
  }

  // ---- FEC 载荷头 + 数据报头里的组几何往返 ----
  {
    FecPayloadHeader ph;
    ph.base_frame_id = 900;
    ph.group_index = 2;
    ph.member_count = 8;
    const auto b = encode(ph);
    const auto back = decode_fec_payload_header({b.data(), b.size()});
    check(back.has_value(), "FEC 载荷头可解");
    if (back) {
      check(back->base_frame_id == 900, "FEC base_frame_id 往返");
      check(back->group_index == 2, "FEC group_index 往返（不再是恒 0）");
      check(back->member_count == 8, "FEC member_count 往返");
    }
    check(!decode_fec_payload_header({b.data(), kFecPayloadHeaderSize - 1}).has_value(),
          "截断的 FEC 载荷头被拒");
    DatagramHeader dh2;
    dh2.type = static_cast<uint8_t>(DownType::Fec);
    dh2.frag_index = 16;  // 组起始片号
    dh2.frag_count = 17;  // 整帧片数
    const auto db = encode(dh2);
    const auto dback = decode_datagram({db.data(), db.size()});
    check(dback.has_value() && dback->frag_index == 16 && dback->frag_count == 17,
          "FEC 组的起始片号/整帧片数在数据报头里往返");
  }

  // ---- feedback（0x06）编解码往返：4 个 f32 小端，-1 表示"没有可靠值" ----
  {
    FeedbackPayload f;
    f.loss_rate = 0.2f;
    f.recv_bitrate_mbps = 3.5f;
    f.jitter_ms = 12.5f;
    f.rtt_ms = -1.0f;
    const auto b = encode(f);
    check(b.size() == kFeedbackPayloadSize && kFeedbackPayloadSize == 16, "feedback 载荷 16 字节");
    const auto back = decode_feedback({b.data(), b.size()});
    check(back.has_value(), "feedback 可解");
    if (back) {
      check(back->loss_rate == 0.2f, "feedback loss_rate 往返");
      check(back->recv_bitrate_mbps == 3.5f, "feedback recv_bitrate_mbps 往返");
      check(back->jitter_ms == 12.5f, "feedback jitter_ms 往返");
      check(back->rtt_ms == -1.0f, "feedback rtt_ms=-1 原样往返（不是 0）");
    }
    // 小端：0.5f = 0x3F000000 → 字节 00 00 00 3F
    FeedbackPayload half;
    half.loss_rate = 0.5f;
    const auto hb = encode(half);
    check(hb[0] == 0x00 && hb[1] == 0x00 && hb[2] == 0x00 && hb[3] == 0x3F,
          "f32 按小端编码");
    check(!decode_feedback({hb.data(), kFeedbackPayloadSize - 1}).has_value(),
          "截断的 feedback 被拒");
    // 数据报头 type=0x06 与载荷一起走线格式
    DatagramHeader fh;
    fh.type = static_cast<uint8_t>(DownType::Feedback);
    const auto fhd = encode(fh);
    check(fhd[1] == 0x06, "feedback 的 DownType=0x06");
  }

  if (failures == 0) {
    std::printf("test_wire: OK\n");
    return 0;
  }
  std::printf("test_wire: %d failure(s)\n", failures);
  return 1;
}
