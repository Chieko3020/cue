// proto 编解码往返与边界校验（M1 最小测试；不依赖测试框架，用 ctest 的退出码）
// N-14 起还包含"多字节字段的线上字节序（golden bytes）"一组：见文件末尾那段注释。
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
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

// 一串字节的十六进制表示（大写、空格分隔），失败信息里打出来给人看。
std::string hex_range(const uint8_t* p, std::size_t n) {
  std::string s;
  char buf[4];
  for (std::size_t i = 0; i < n; ++i) {
    std::snprintf(buf, sizeof buf, "%02X", p[i]);
    if (i != 0) s.push_back(' ');
    s += buf;
  }
  return s;
}

// 逐字节比对：失败时打出「期望字节 / 实际字节」两行。
// 比 `a == b` 那种断言多出来的价值就在这里——线格式错的时候，"差在哪个字节、
// 变成了什么"是唯一能立刻定位问题的信息（端序反了会整段倒过来，位置挪了只差几项）。
void check_bytes(const std::string& what, const uint8_t* got,
                 std::initializer_list<uint8_t> want) {
  const std::vector<uint8_t> w(want);
  if (std::equal(w.begin(), w.end(), got)) return;
  std::printf("FAIL: %s\n  期望字节: %s\n  实际字节: %s\n", what.c_str(),
              hex_range(w.data(), w.size()).c_str(), hex_range(got, w.size()).c_str());
  ++failures;
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

  // ---- N-14：多字节字段的**线上字节序**（golden bytes）----
  //
  // 这条测试锁定的是**线上字节序**，不是宿主字节序：wire.h 的 detail::put_u16/u32/u64 是
  // 显式移位拼装（p[i] = v >> (8*i)），与宿主端序无关——换到一台大端机器上，这段代码
  // 发出的字节仍然和这里写的一模一样，所以这些断言在大端机器上也成立，它们描述的是"发
  // 出去的字节永远长这样"，而不是"本机是小端"。反过来，任何把 put_* 改成
  // memcpy/强转指针、或把移位方向改反的改动，都会被这几行以「期望字节 / 实际字节」抓住。
  //
  // 取值刻意选成各字节互不相同的模式（0x01020304、0x11223344、0xA1B2…）：
  // 用 0x00000001 这类值，大部分端序错法会"碰巧通过"；只有每个字节都不同，反序、位移、
  // 高字节被吞这几种错法才都会被区分出来。
  {
    // ---- 上行媒体头：u32 若干 + u64 一个 ----
    MediaHeader h;
    h.type = static_cast<uint8_t>(MediaType::Video);
    h.codec = static_cast<uint8_t>(Codec::H264);
    h.flags = kUpFlagKeyframe;
    h.stream_id = 0x01020304u;
    h.frame_id = 0x11223344u;
    h.frag_index = 0x000001B2u;  // 434：既要各字节互不相同，又要是合法分片（< frag_count）
    h.frag_count = 0x000001B3u;  // 435
    h.pts_us = 0x1122334455667788ull;
    h.payload_len = 16;
    const auto hb = encode(h);
    check_bytes("media: magic/type/codec/flags 位于 0..3", hb.data(), {0xC1, 0x01, 0x01, 0x01});
    check_bytes("media: stream_id(u32)=0x01020304 → LE 04 03 02 01", &hb[4],
                {0x04, 0x03, 0x02, 0x01});
    check_bytes("media: frame_id(u32)=0x11223344 → LE 44 33 22 11", &hb[8],
                {0x44, 0x33, 0x22, 0x11});
    check_bytes("media: frag_index(u32)=0x000001B2 → LE B2 01 00 00", &hb[12],
                {0xB2, 0x01, 0x00, 0x00});
    check_bytes("media: frag_count(u32)=0x000001B3 → LE B3 01 00 00", &hb[16],
                {0xB3, 0x01, 0x00, 0x00});
    check_bytes("media: pts_us(u64)=0x1122334455667788 → LE 88 77 66 55 44 33 22 11", &hb[20],
                {0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11});
    check_bytes("media: payload_len(u32)=16 → LE 10 00 00 00", &hb[28], {0x10, 0x00, 0x00, 0x00});
    // 字节对了还要能解回来（只对一半的实现——比如写对了读反了——在这里会暴露）
    {
      std::vector<uint8_t> pkt(hb.begin(), hb.end());
      pkt.resize(kMediaHeaderSize + 16, 0x5A);
      const auto m = decode_media({pkt.data(), pkt.size()});
      check(m.has_value(), "media: golden 字节可解");
      if (m) {
        check(m->stream_id == 0x01020304u, "media: stream_id 往返");
        check(m->frame_id == 0x11223344u, "media: frame_id 往返");
        check(m->frag_index == 0x000001B2u, "media: frag_index 往返");
        check(m->frag_count == 0x000001B3u, "media: frag_count 往返");
        check(m->pts_us == 0x1122334455667788ull, "media: pts_us 往返");
      }
    }

    // ---- 下行数据报头：u16 两个 + u32 两个 + u64 一个 ----
    DatagramHeader d;
    d.type = static_cast<uint8_t>(DownType::Video);
    d.flags = kDownFlagKeyframe;
    d.codec = static_cast<uint8_t>(Codec::H264);
    d.session_id = 0x0A0B0C0Du;
    d.frame_id = 0x11223344u;
    d.frag_index = 0x1234u;
    d.frag_count = 0xABCDu;
    d.pts_us = 0x0102030405060708ull;
    const auto db = encode(d);
    check_bytes("datagram: magic/type/flags/codec 位于 0..3", db.data(),
                {0xD1, 0x01, 0x01, 0x01});
    check_bytes("datagram: session_id(u32)=0x0A0B0C0D → LE 0D 0C 0B 0A", &db[4],
                {0x0D, 0x0C, 0x0B, 0x0A});
    check_bytes("datagram: frame_id(u32)=0x11223344 → LE 44 33 22 11", &db[8],
                {0x44, 0x33, 0x22, 0x11});
    check_bytes("datagram: frag_index(u16)=0x1234 → LE 34 12", &db[12], {0x34, 0x12});
    check_bytes("datagram: frag_count(u16)=0xABCD → LE CD AB", &db[14], {0xCD, 0xAB});
    check_bytes("datagram: pts_us(u64)=0x0102030405060708 → LE 08 07 06 05 04 03 02 01",
                &db[16], {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01});
    {
      const auto back = decode_datagram({db.data(), db.size()});
      check(back.has_value(), "datagram: golden 字节可解");
      if (back) {
        check(back->session_id == 0x0A0B0C0Du, "datagram: session_id 往返");
        check(back->frame_id == 0x11223344u, "datagram: frame_id 往返");
        check(back->frag_index == 0x1234u, "datagram: frag_index 往返");
        check(back->frag_count == 0xABCDu, "datagram: frag_count 往返");
        check(back->pts_us == 0x0102030405060708ull, "datagram: pts_us 往返");
      }
    }

    // ---- FEC 载荷头：u32 + u16 + u16（SPEC §7.2 的 8 字节）----
    FecPayloadHeader f;
    f.base_frame_id = 0xCAFEBABEu;
    f.group_index = 0x00FFu;
    f.member_count = 0x0800u;
    const auto fb = encode(f);
    check(fb.size() == kFecPayloadHeaderSize && kFecPayloadHeaderSize == 8,
          "fec: 载荷头 8 字节");
    check_bytes("fec: base_frame_id(u32)=0xCAFEBABE → LE BE BA FE CA", &fb[0],
                {0xBE, 0xBA, 0xFE, 0xCA});
    check_bytes("fec: group_index(u16)=0x00FF → LE FF 00", &fb[4], {0xFF, 0x00});
    check_bytes("fec: member_count(u16)=0x0800 → LE 00 08", &fb[6], {0x00, 0x08});
    {
      const auto back = decode_fec_payload_header({fb.data(), fb.size()});
      check(back.has_value(), "fec: golden 字节可解");
      if (back) {
        check(back->base_frame_id == 0xCAFEBABEu, "fec: base_frame_id 往返");
        check(back->group_index == 0x00FFu, "fec: group_index 往返");
        check(back->member_count == 0x0800u, "fec: member_count 往返");
      }
    }

    // ---- feedback 载荷（wire.h 里叫 FeedbackPayload，没有 FeedbackHeader 这个类型）----
    // 四个 f32 按位当作 u32 走 put_u32，所以字节序规则与整数完全一致；
    // 位模式固定写出来，证的是"f32 的位模式也按小端摆"，而不是"这台机器的浮点布局"。
    FeedbackPayload fbk;
    fbk.loss_rate = 0.5f;           // 0x3F000000
    fbk.recv_bitrate_mbps = -1.0f;  // 0xBF800000（"没有可靠值"）
    fbk.jitter_ms = 12.5f;          // 0x41480000
    fbk.rtt_ms = -1.0f;
    const auto fbb = encode(fbk);
    check(fbb.size() == kFeedbackPayloadSize && kFeedbackPayloadSize == 16,
          "feedback: 载荷 16 字节");
    check_bytes("feedback: loss_rate(f32)=0.5 → 0x3F000000 → LE 00 00 00 3F", &fbb[0],
                {0x00, 0x00, 0x00, 0x3F});
    check_bytes("feedback: recv_bitrate_mbps(f32)=-1 → 0xBF800000 → LE 00 00 80 BF", &fbb[4],
                {0x00, 0x00, 0x80, 0xBF});
    check_bytes("feedback: jitter_ms(f32)=12.5 → 0x41480000 → LE 00 00 48 41", &fbb[8],
                {0x00, 0x00, 0x48, 0x41});
    {
      const auto back = decode_feedback({fbb.data(), fbb.size()});
      check(back.has_value(), "feedback: golden 字节可解");
      if (back) {
        check(back->loss_rate == 0.5f, "feedback: loss_rate 往返");
        check(back->recv_bitrate_mbps == -1.0f, "feedback: recv_bitrate_mbps 往返");
        check(back->jitter_ms == 12.5f, "feedback: jitter_ms 往返");
        check(back->rtt_ms == -1.0f, "feedback: rtt_ms 往返");
      }
    }

    // ---- NACK 条目：u32 frame_id + u16 frag_index（变长载荷，逐条拼接）----
    {
      std::vector<uint8_t> entries;
      append_nack_entry(entries, NackEntry{0x0A0B0C0Du, 0x1234u});
      check(entries.size() == kNackEntrySize && kNackEntrySize == 6, "nack: 条目 6 字节");
      check_bytes("nack: frame_id(u32)=0x0A0B0C0D | frag_index(u16)=0x1234 → LE 0D 0C 0B 0A 34 12",
                  entries.data(), {0x0D, 0x0C, 0x0B, 0x0A, 0x34, 0x12});
      const auto back = decode_nack_entries({entries.data(), entries.size()});
      check(back.size() == 1 && back[0].frame_id == 0x0A0B0C0Du && back[0].frag_index == 0x1234u,
            "nack: 条目往返");
    }
  }

  if (failures == 0) {
    std::printf("test_wire: OK\n");
    return 0;
  }
  std::printf("test_wire: %d failure(s)\n", failures);
  return 1;
}
