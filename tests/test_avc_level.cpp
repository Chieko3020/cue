// 分辨率 → AVC level 单测（技术设计文档 §13.1「media/avc_level」那行，N-09）。
//
// 为什么要有这条测试：这张表有两份平行实现——C++（include/cue/media/avc_level.h，
// 采集端 cue-capture 用）与浏览器 JavaScript（web/app.js 的 pickCodec）。两份必须对
// 同一分辨率给出同一个 level，否则同一分辨率在两条产线上编码参数就不一致，probe 侧
// "对照编码参数"这件事失去意义。C++ 这份原来写在 apps/cue-capture.cpp 里，tests/
// 链接不到 app，所以两个边界值一直只有人工证据；N-09 把它搬到 media 层后在这里钉死。
// 浏览器那份不在这里测（它不在 C++ 里），由 scripts/check-avc-level.mjs 做一致性检查。
//
// 断言口径是**闭区间**：area 恰好等于上限仍然合格。依据是 M0 实测的浏览器报错原文
// "coded area 2088960 exceeds maximum 414720 (AVC level 3.0)" —— 2088960 被拒是因为
// 它大于 414720，而不是大于等于；2048×1024 恰好 2097152 的那条断言同理钉住另一侧。
#include <climits>
#include <cstdio>

#include "cue/media/avc_level.h"
#include "cue/media/ffmpeg_args.h"

namespace {

int failures = 0;

void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

using cue::media::avc_level_name;
using cue::media::kAvcLevel30MaxArea;
using cue::media::kAvcLevel40MaxArea;
using cue::media::pick_avc_level;

// 常量本身也是断言对象：这两个数是 AVC 附录 A 的 MaxFS 换算来的（宏块数 × 256），
// 有人"顺手"把它们改成 415000 这种整数时，下面那批用例会一起红，但先在这里失败能
// 直接指出是常量被动过。
static_assert(kAvcLevel30MaxArea == 1620 * 256, "L3.0 上限应为 1620 宏块 × 256");
static_assert(kAvcLevel40MaxArea == 8192 * 256, "L4.0 上限应为 8192 宏块 × 256");

// pick_avc_level 是 constexpr 的，编译期就该算出同样的结果——这也顺带证明它没有
// 静态状态、没有依赖运行期环境。这四条只覆盖常量表达式走得到的路径。
static_assert(pick_avc_level(720, 576) == 30);
static_assert(pick_avc_level(2048, 1024) == 40);
static_assert(pick_avc_level(3840, 2160) == 0);
static_assert(pick_avc_level(0, 0) == 30);  // 见下方第 4 组的说明：现行为，不改语义

}  // namespace

int main() {
  // ---- 1) L3.0 边界：414720 恰好合格，再多一个样本就升到 L4.0 ----
  // 720×576 = 414720 是能凑出这个面积的常见尺寸之一（PAL D1）。
  check(720 * 576 == kAvcLevel30MaxArea, "premise: 720x576 == 414720");
  check(pick_avc_level(720, 576) == 30, "1a area == 414720 -> 30 (闭区间)");
  // 721×576 = 415296：比上限多 576（真实采集尺寸请见第 3 组；721 是奇数，yuv420p 用不了，
  // 这里只借它构造"刚过线"的面积）。
  check(721 * 576 == 415296, "premise: 721x576 == 415296");
  check(pick_avc_level(721, 576) == 40, "1b area == 415296 (> 414720) -> 40");
  // 恰好 +1 的那个面积用 1×414721 精确构造：这不只是等差一格的数学玩具，它证明判定
  // 用的是 `<=` 而不是 `<`——写成 `<` 时 414721 会掉到 40 是"碰巧也对"，但 414720
  // 会掉到 40，1a 会先红。
  check(pick_avc_level(1, kAvcLevel30MaxArea + 1) == 40, "1c area == 414721 -> 40");

  // ---- 2) L4.0 边界：2097152 恰好合格，超出即 0（交给 x264 自动）----
  // 2048×1024 是 1080p 之上的常见采集尺寸（2K）。
  check(2048 * 1024 == kAvcLevel40MaxArea, "premise: 2048x1024 == 2097152");
  check(pick_avc_level(2048, 1024) == 40, "2a area == 2097152 -> 40 (闭区间)");
  check(2048 * 1025 == 2099200, "premise: 2048x1025 == 2099200");
  check(pick_avc_level(2048, 1025) == 0, "2b area == 2099200 (> 2097152) -> 0");
  // 恰好 +1：1×2097153 精确构造出边界外的最小面积。
  check(pick_avc_level(1, kAvcLevel40MaxArea + 1) == 0, "2c area == 2097153 -> 0");

  // ---- 3) 常见档位（p360p15 / p480p30 / p720p30 / p1080p30s）----
  check(pick_avc_level(640, 360) == 30, "3a 640x360 = 230400 -> 30");
  check(pick_avc_level(1280, 720) == 40, "3b 1280x720 = 921600 -> 40");
  check(pick_avc_level(1920, 1080) == 40, "3c 1920x1080 = 2073600 -> 40");
  check(pick_avc_level(3840, 2160) == 0, "3d 3840x2160 = 8294400 -> 0");

  // ---- 4) 0/负尺寸：断言**现行为**，本任务不改语义 ----
  // 现行为：area 为 0 或负数时 `area <= 414720` 成立 → 返回 30（L3.0）。
  // 这不构成线上缺陷：宽高在命令行解析处就要求 > 0（apps/cue-capture.cpp
  // 的 `o.width <= 0 || o.height <= 0` 分支），这两条用例是给"将来有人把
  // pick_avc_level 拿去接别的输入"时留的护栏，不是为某个可复现的线上路径。
  check(pick_avc_level(0, 0) == 30, "4a (0,0) -> 30（现行为）");
  check(pick_avc_level(-1, 1080) == 30, "4b (-1,1080) -> 30（现行为）");
  check(pick_avc_level(1920, -1) == 30, "4c (1920,-1) -> 30（现行为）");

  // ---- 5) 溢出：int 相乘会溢出，long long 不会 ----
  // 真实面积 2147483647² = 4611686014132420609，int64 上限是 9223372036854775807，
  // 还差一半余量。若实现写的是 `w * h`（int×int）则是有符号溢出 UB，
  // UBSan 构建下会直接报错，而不是"碰巧给出一个数"。
  check(LLONG_MAX == 9223372036854775807LL, "premise: int64 上限");
  check(static_cast<long long>(INT_MAX) * INT_MAX == 4611686014132420609LL,
        "premise: INT_MAX² 放得进 int64");
  check(pick_avc_level(INT_MAX, INT_MAX) == 0, "5a (INT_MAX, INT_MAX) -> 0（不溢出）");
  // 负×负得正：两个 INT_MAX 都是正数已覆盖；这里补一个"负负得正后仍然超大"的组合，
  // 证明实现没有靠"负数必小"这种巧合。
  check(pick_avc_level(INT_MIN, INT_MIN) == 0, "5b (INT_MIN, INT_MIN) -> 面积正且超大 -> 0");

  // ---- 6) 与 avc_level_name 的衔接：level idc 要能被写成 ffmpeg 的 -level 参数 ----
  // 这两处拆开写就迟早对不上（30 -> "3.0"、40 -> "4.0"、0 -> "auto"），所以这里
  // 直接把 pick_avc_level 的输出喂进去，断言端到端文案。
  check(avc_level_name(pick_avc_level(720, 576)) == "3.0", "6a 720x576 -> \"3.0\"");
  check(avc_level_name(pick_avc_level(1920, 1080)) == "4.0", "6b 1920x1080 -> \"4.0\"");
  check(avc_level_name(pick_avc_level(3840, 2160)) == "auto", "6c 3840x2160 -> \"auto\"");

  if (failures == 0) {
    std::printf("avc_level: 全部断言通过\n");
    return 0;
  }
  std::printf("avc_level: %d 条断言失败\n", failures);
  return 1;
}
