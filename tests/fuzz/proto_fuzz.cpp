// proto 解析入口的 libFuzzer 目标（技术设计 §13.4 / T-18b）。
//
// 为什么单测不够：tests/test_wire.cpp 覆盖的是"边界样例"（0/16 KiB/16 KiB+1/超大），
// 它证明的是"我知道的那几个边界处理对了"。解析器面对的是任意字节，真正的风险在
// "两个字段的组合"上（frag_index < frag_count 且 payload_len 刚好等于剩余字节……）。
// libFuzzer 用覆盖反馈去找这些组合，比人穷举可靠。
//
// 三个入口各编一个可执行文件（见 CMakeLists.txt 的 CUE_FUZZ）：
//   CUE_FUZZ_TARGET_DECODE_MEDIA      decode_media（上行 32 字节头 + 分片校验）
//   CUE_FUZZ_TARGET_DECODE_DATAGRAM   decode_datagram（下行 24 字节 DatagramHeader）
//   CUE_FUZZ_TARGET_FEC_HEADER        decode_fec_payload_header（FEC 载荷 8 字节头）
// 一个 libFuzzer 进程只驱动一个 LLVMFuzzerTestOneInput，所以在编译期用宏选定入口，
// 而不是在运行时 if/else——后者漏写一个分支时，覆盖数字看起来仍然"有增长"。
//
// 解析成功时的断言是**协议后置条件**，不是重复实现：只要解析器返回 optional 有值，
// 就必须满足这些条件；任何一条不成立都是真 bug（越界读、字段丢失、约束没校验），
// 直接 abort 让 libFuzzer 记成 crash。
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>

#include "cue/proto/wire.h"

namespace {

// abort 而不是 assert：RelWithDebInfo 下 NDEBUG 会把 assert 编掉，fuzz 就失去了判据。
[[noreturn]] void fail(const char* what) {
  std::fprintf(stderr, "FUZZ INVARIANT VIOLATED: %s\n", what);
  std::fflush(stderr);
  std::abort();
}

// 头字段的逐字节往返：解出来的结构体重新编码必须等于输入的头。
// 它同时覆盖"某个字段没被赋默认值""多字节整数端序读错"这两类静默错误。
void check_roundtrip(std::span<const uint8_t> input, std::span<const uint8_t> reencoded) {
  if (input.size() < reencoded.size()) fail("输入短于重新编码后的头");
  for (std::size_t i = 0; i < reencoded.size(); ++i) {
    if (input[i] != reencoded[i]) fail("头字段往返不一致（解码/编码不对称）");
  }
}

}  // namespace

#if defined(CUE_FUZZ_TARGET_DECODE_MEDIA)
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size) {
  using namespace cue::proto;
  const std::span<const uint8_t> bytes(data, size);
  const auto header = decode_media(bytes);
  if (!header) return 0;  // 被拒是合法结果：拒绝路径本身也在被 fuzz（覆盖不加分但不崩）

  if (header->magic != kMagicUp) fail("magic 不是 kMagicUp");
  if (header->frag_count == 0 || header->frag_count > kMaxFragPerFrame) fail("frag_count 越界");
  if (header->frag_index >= header->frag_count) fail("frag_index >= frag_count");
  if (header->payload_len > kMaxWsFrag) fail("payload_len 超过单分片上限");
  if (header->payload_len > bytes.size() - kMediaHeaderSize) fail("payload_len 超过剩余字节");

  const auto reencoded = encode(*header);
  check_roundtrip(bytes, reencoded);

  // 同一字节串走控制类入口：它只应放宽分片三项，尺寸/magic/越界必须同样严格。
  // 反过来说，decode_media 接受的东西 validate_control 也必须接受（否则同一包在两个
  // 入口之间会得到不一致的结论）。
  MediaHeader ctl{};
  if (validate_control(bytes, ctl) != MediaReject::None) fail("控制入口拒绝了媒体入口接受的包");
  if (ctl.payload_len != header->payload_len || ctl.frame_id != header->frame_id) {
    fail("两个入口解出的头不一致");
  }
  return 0;
}
#elif defined(CUE_FUZZ_TARGET_DECODE_DATAGRAM)
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size) {
  using namespace cue::proto;
  const std::span<const uint8_t> bytes(data, size);
  const auto header = decode_datagram(bytes);
  if (!header) return 0;

  if (header->magic != kMagicDown) fail("magic 不是 kMagicDown");
  if (bytes.size() < kDatagramHeaderSize) fail("短包却解出了 DatagramHeader");
  const auto reencoded = encode(*header);
  check_roundtrip(bytes, reencoded);
  return 0;
}
#elif defined(CUE_FUZZ_TARGET_FEC_HEADER)
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size) {
  using namespace cue::proto;
  const std::span<const uint8_t> bytes(data, size);
  const auto header = decode_fec_payload_header(bytes);
  if (!header) {
    // 唯一允许的拒绝理由是长度不足；长度够却返回 null 就是解析器的 bug
    if (bytes.size() >= kFecPayloadHeaderSize) fail("长度足够却拒绝了 FEC 载荷头");
    return 0;
  }
  const auto reencoded = encode(*header);
  check_roundtrip(bytes, reencoded);

  // 往返后再解一次：三个字段必须完全一致。u16 极大值被钳位、成员被符号扩展这类
  // 错误都会在这里现形（member_count 可以大于分组上限，解析器只负责原样返回）。
  const auto again = decode_fec_payload_header(reencoded);
  if (!again) fail("重新编码的 FEC 头解析失败");
  if (again->base_frame_id != header->base_frame_id ||
      again->group_index != header->group_index ||
      again->member_count != header->member_count) {
    fail("FEC 头字段在往返中被改写");
  }
  return 0;
}
#else
#error "必须定义一个 CUE_FUZZ_TARGET_* 宏（见 CMakeLists.txt 的 CUE_FUZZ 段）"
#endif
