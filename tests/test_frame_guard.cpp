// 幽灵帧守卫的单测（技术设计文档 §13.1「proto/frame_guard」那行）：N-08 把
// apps/cue-probe.cpp 里两处**逐字相同**的守卫条件抽成 proto 层纯函数之后补上的六条。
//
// 为什么值得单测：这两个条件在 M0 踩过坑，而且它们藏在 2700 行的 main() 里——要复现
// "已完成帧又收到迟到的 FEC 包 / 分片"这种时序，得真跑 8 s 管线再注入丢包；而本函数
// 的四个入参（包里的帧号、完成水位、是否已有完成帧、槽位在不在）是有限的，能逐条钉死。
// 函数无副作用（计数器留在 probe 本地），所以这里连时钟、socket 都不需要，毫秒级跑完。
//
// 六条锁定的是：
//   1) 已完成帧 + 无槽位 → 丢弃（幽灵帧守卫的核心；FEC 与分片两种调用各测一次）；
//   2) 已完成帧 + 仍有槽位 → 不丢弃（槽位还在 = 帧尚未结算，允许继续收片）；
//   3) frame_id > max_completed → 不丢弃（迟到的"未来"帧，正常路径）；
//   4) have_completed == false → 永不丢弃（首帧保护：frame_id=0、max_completed=0 必须
//      放行，这是"别把第一个 IDR 挡掉"的回归锁）；
//   5) 边界：frame_id == max_completed 且无槽位 → 丢弃（闭区间，写成 `<` 就会漏）；
//   6) 回绕：frame_id 很大而水位很小 → 不丢弃；水位很大而 frame_id=0 → 丢弃
//      （断言函数按无符号比较，两个操作数没有写反）。
#include <cstdint>
#include <cstdio>

#include "cue/proto/frame_guard.h"

namespace {

int failures = 0;

void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

using cue::proto::would_rebuild_completed_frame;

}  // namespace

int main() {
  // 1) 核心：该帧已判定完成、pending 里没有它的槽位 → 必须丢弃这个包，否则会重建
  //    空槽位，随后被 finish_if_complete 计成"第二次完成"（幽灵帧）。
  //    两个调用点用的是同一个函数，这里按两处的实际语义各测一次：FEC 分支传的是
  //    FEC 包的 base_frame_id（校验包总在整帧之后到，帧号小于水位）；分片/重传分支
  //    传的是视频包的 frame_id（通常正好落在水位上）。
  check(would_rebuild_completed_frame(240, 241, true, false),
        "1a FEC 校验包为已完成帧重建槽位 → 丢弃");
  check(would_rebuild_completed_frame(241, 241, true, false),
        "1b 迟到的视频分片/重传为已完成帧重建槽位 → 丢弃");

  // 2) 槽位还在 = 这一帧尚未结算（还在收片），此时不能丢包。
  check(!would_rebuild_completed_frame(240, 241, true, true),
        "2a 已完成水位之内但槽位仍在 → 不丢弃（继续收片）");
  check(!would_rebuild_completed_frame(241, 241, true, true),
        "2b 帧号等于水位但槽位仍在 → 不丢弃");

  // 3) 帧号高于水位：这是正常路径（新帧、乱序但还没完成过），决不能挡。
  check(!would_rebuild_completed_frame(242, 241, true, false),
        "3a frame_id > max_completed → 不丢弃（未来帧）");
  check(!would_rebuild_completed_frame(1000, 5, true, false),
        "3b 帧号远高于水位 → 不丢弃");

  // 4) 首帧保护：max_completed 初值 0、frame_id 从 0 开始，没有 have_completed 就会把
  //    第一个 IDR 当成"已完成帧的迟到尾巴"丢掉（解码器拿不到 SPS/PPS）。
  check(!would_rebuild_completed_frame(0, 0, false, false),
        "4a 首帧保护：frame_id=0/max_completed=0/无槽位 → 放行（别挡 IDR）");
  check(!would_rebuild_completed_frame(0, 0, false, true),
        "4b 首帧保护：槽位未建时也放行");
  check(!would_rebuild_completed_frame(7, 100, false, false),
        "4c 还没有任何完成帧时水位不生效 → 不丢弃");
  check(!would_rebuild_completed_frame(0xFFFFFFFFu, 0, false, false),
        "4d 首帧保护优先于回绕判断 → 不丢弃");
  {
    bool any_discarded = false;
    for (std::uint32_t fid = 0; fid <= 8; ++fid) {
      for (std::uint32_t mc = 0; mc <= 8; ++mc) {
        for (int in_pending = 0; in_pending < 2; ++in_pending) {
          if (would_rebuild_completed_frame(fid, mc, false, in_pending != 0)) any_discarded = true;
        }
      }
    }
    check(!any_discarded, "4e have_completed=false 的 162 种组合全部放行");
  }

  // 5) 边界：闭区间。写成 `frame_id < max_completed` 会让"正好等于水位"的那一帧逃过守卫，
  //    也就是最常见的迟到分片（它的帧号通常就等于水位）。
  check(would_rebuild_completed_frame(7, 7, true, false),
        "5a frame_id == max_completed 且无槽位 → 丢弃（闭区间）");
  check(!would_rebuild_completed_frame(7, 6, true, false),
        "5b frame_id 比水位大 1 → 不丢弃");

  // 6) 回绕：uint32_t 的 frame_id 会绕回来，但**比较方向**不能反。这里钉住"谁大谁小"
  //    用的是无符号语义，而不是某种回绕距离算法（当前实现就是直接 <=，探针靠会话换代
  //    重置水位来兜住真实的回绕，见 apps/cue-probe.cpp 的 reset_stream）。
  check(!would_rebuild_completed_frame(0xFFFFFFFFu, 1000, true, false),
        "6a frame_id 很大、水位很小 → 不丢弃（无符号比较没写反）");
  check(would_rebuild_completed_frame(0, 0xFFFFFFFFu, true, false),
        "6b 水位很大、frame_id=0 → 丢弃（无符号比较没写反）");
  check(!would_rebuild_completed_frame(0xFFFFFFFEu, 0xFFFFFFFFu, true, true),
        "6c 回绕附近但槽位仍在 → 不丢弃");

  if (failures == 0) std::printf("test_frame_guard: OK\n");
  return failures == 0 ? 0 : 1;
}
