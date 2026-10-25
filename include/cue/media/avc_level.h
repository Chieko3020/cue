// 分辨率 → AVC level 的选择表（纯函数，只依赖标准库，不碰 FFmpeg）。
//
// 为什么把它抽到 media 层：这张表有两份平行实现——采集端（C++，本头文件）与浏览器端
// （web/app.js 的 pickCodec）。两份必须对同一分辨率给出同一个 level，否则"同一分辨率
// 在两条产线上得到同一个编码参数、probe 侧才能对照"这个前提就不成立。而 C++ 这份原来
// 写在 apps/cue-capture.cpp 的匿名命名空间里，tests/ 链接不到 app，于是它只能靠人工
// 跑管线验证（N-09 的证据）。抽出来之后，两个边界值由 tests/test_avc_level.cpp 钉住；
// 浏览器那份由 scripts/check-avc-level.mjs 做静态一致性检查（不是浏览器行为测试）。
#pragma once

namespace cue::media {

// coded area 上限（样本数，即宽 × 高，不是宏块数）。AVC 附录 A 的 MaxFS 以宏块计，
// 一个宏块 16×16 = 256 样本，于是：
//   L3.0：1620 MB × 256 = 414720
//   L4.0：8192 MB × 256 = 2097152
// 这两个数是**闭区间上界**：area 恰好等于上限仍然合格（M0 实测的报错是
// "coded area 2088960 exceeds maximum 414720"，说明判定用的是严格大于）。
inline constexpr int kAvcLevel30MaxArea = 414720;
inline constexpr int kAvcLevel40MaxArea = 2097152;

// 返回 AVC level ×10（30 = L3.0、40 = L4.0、0 = 交给 x264 自动推导），与
// ffmpeg_args.h 的 avc_level_name() 同一口径（那边把 30 写成 "-level 3.0"）。
//
// 返回 0 **不是错误码**：它的语义是"面积超出这张已知表，不要替 x264 做主"。
// 两个消费点都是这么用的：ffmpeg 子进程分支只在 level_idc > 0 时才追加 -level
// （ffmpeg_args.h），进程内 x264 分支也只在 level_idc > 0 时才写 i_level_idc
// （encoder.cpp，注释写明 0 保持 x264 的自动推导）。浏览器端没有"auto"，同名逻辑
// 在那种情况下只能返回 null 拒绝采集。
constexpr int pick_avc_level(int w, int h) {
  // 先扩成 long long 再乘：1920×1080 当然在 int 内，但 (INT_MAX, INT_MAX) 的真实面积
  // 是 2147483647² = 4611686014132420609，放不进 int32，有符号溢出是 UB。
  const long long area = static_cast<long long>(w) * h;
  if (area <= kAvcLevel30MaxArea) return 30;
  if (area <= kAvcLevel40MaxArea) return 40;
  return 0;
}

}  // namespace cue::media
