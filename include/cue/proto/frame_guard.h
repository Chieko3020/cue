// 幽灵帧守卫（M0 教训，N-08 抽成 proto 层纯函数）。
//
// **防的是什么**：下行接收端用 pending 表按 frame_id 攒分片。一帧被判定完成、槽位从
// pending 里删掉之后，它的"尾巴"仍可能到达，有两种：
//   - FEC 校验包：XOR 组的校验包本来就排在整帧数据之后，几乎必然晚于帧结算；
//   - 迟到的视频分片 / NACK 重传补回来的那一片。
// 这两种包若在 pending 里找不到槽位就顺手 `pending[fid]` 建一个空槽，会让
// finish_if_complete 把同一帧**再结算一次**：完成帧数多于真正解出的帧数，就是"幽灵帧"。
// 守卫的答案是：该帧已判定完成 + 槽位不在 → 丢弃这个包（调用方 continue）。
//
// **have_completed 为什么必需**（首帧保护）：max_completed 的初值是 0，而 frame_id
// 从 0 开始，只看 `frame_id <= max_completed` 会把**第一个 IDR** 当成"已完成帧的迟到
// 尾巴"丢掉；代价是解码器拿不到 SPS/PPS，接下来一整个 GOP 都解不出帧（实测：转码
// 链路 decoded=0、decode_errors 每秒 30）。所以"还没有任何已完成帧"时守卫必须整体失效。
//
// **两处调用点共用这一个判断，但各自计数**（计数留在 probe 本地，本函数无副作用）：
//   - fec_ghost_discarded 是**常态**——FEC 校验包总在整帧之后到，零丢包实测
//     8 s / 241 帧 → 250 次（约每完成帧 1 次）；它只证明"守卫在跑、这条路径有账"；
//   - frag_ghost_discarded 才是**异常**——正常链路不该出现（零丢包 0、5% 丢包 2~3）。
// 合并成一个计数会让"每完成帧一次的常态"淹没真正的异常，判读方式也不同（比率 vs 是否为 0），
// 所以这里只管"该不该丢"，数目字由调用点各自记。
#pragma once

#include <cstdint>

namespace cue::proto {

// 迟到的协议包（FEC 校验包 / 视频分片 / NACK 重传）会不会为"已完成的帧"重建槽位。
// 返回 true = 应当丢弃这个包。
//   frame_id       包携带的帧号（FEC 分支传 base_frame_id）
//   max_completed  已判定完成的最高 frame_id 水位（无符号比较，回绕按 uint32_t 语义走）
//   have_completed 是否已经有过至少一个完成帧（首帧保护，见文件头）
//   in_pending     pending 里是否已有这一帧的槽位（有槽位 = 尚未结算，允许继续收片）
[[nodiscard]] inline bool would_rebuild_completed_frame(std::uint32_t frame_id,
                                                        std::uint32_t max_completed,
                                                        bool have_completed, bool in_pending) {
  if (!have_completed) return false;  // 首帧保护：别把第一个 IDR 挡掉
  if (in_pending) return false;       // 槽位还在 = 帧尚未结算，继续收片
  return frame_id <= max_completed;   // 闭区间：等于水位的也算已完成
}

}  // namespace cue::proto
