// FR-EVAL.5：case 生成器的负向/正向判据。
//
// 这不是"跑一遍看它生成了几个"的自证：每条断言对应生成合同里的一条承诺——
//   * 矩阵展开数量 = 笛卡尔积（选项顺序无关、确定性）；
//   * id 可读且唯一（重复 id 会让 pass^k 与成本按 id 合并，全错）；
//   * 非法组合被**拒绝并说明原因**（静默丢弃会让人以为矩阵生效了）；
//   * prompt / sla / evidence_candidates / allowed_actions / budget / repair 原样继承；
//   * 生成物能被 load_suite 接受（用同一套 parse_case 当最后一道门）。
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cue/eval/eval.h"
#include "cue/eval/generator.h"

namespace eval = cue::eval;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

int failures = 0;
void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

// 模板集：四类根因，覆盖三种轴（丢包 / 限速 / 时延 / 管线参数）。
// 数值与 suites/real-faults.json 同口径，但不依赖它——生成器单测不能因为
// 真实的 case 集改参数而变红。
const char* kBase = R"json({
  "suite": "gen-test-base",
  "version": 1,
  "cases": [
    {
      "id": "loss-tpl", "seed": 41,
      "fault": {"kind": "ingress_drop", "params": {"inject_ingress_pct": 50}},
      "pipeline": {"bitrate_kbps": 1500, "gop": 60},
      "prompt": "loss prompt",
      "expect": {
        "root_cause": "network_loss",
        "evidence_candidates": ["injected", "fps"],
        "allowed_actions": ["inject_fault", "pipeline_set_param", "query_metrics", "fetch_observation"],
        "sla": {"fps_min": 25}
      },
      "budget": {"max_turns": 16, "max_yuan": 0.3, "max_wall_s": 180}
    },
    {
      "id": "bw-tpl", "seed": 42,
      "fault": {"kind": "rate", "params": {"rate_mbps": 0.1}},
      "pipeline": {"transcode": true, "width": 1280, "height": 720,
                   "bitrate_kbps": 4000, "gop": 60, "preset": "veryfast"},
      "prompt": "bw prompt",
      "expect": {
        "root_cause": "bandwidth_insufficient",
        "evidence_candidates": ["budget_drops", "dropped", "fps"],
        "allowed_actions": ["inject_fault", "pipeline_set_param", "query_metrics", "fetch_observation"],
        "sla": {"fps_min": 25}
      },
      "budget": {"max_turns": 16, "max_yuan": 0.3, "max_wall_s": 180}
    },
    {
      "id": "delay-tpl", "seed": 43,
      "fault": {"kind": "delay", "params": {"delay_ms": 200}},
      "pipeline": {"bitrate_kbps": 1500, "gop": 60},
      "prompt": "delay prompt",
      "expect": {
        "root_cause": "network_delay",
        "evidence_candidates": ["send_p99_ms", "delayed_pkts", "injected"],
        "allowed_actions": ["inject_fault", "pipeline_set_param", "query_metrics", "fetch_observation"],
        "sla": {"fps_min": 25, "lat_p99_ms": 120}
      },
      "budget": {"max_turns": 16, "max_yuan": 0.3, "max_wall_s": 180}
    },
    {
      "id": "enc-tpl", "seed": 44,
      "fault": {"kind": "none"},
      "pipeline": {"transcode": true, "width": 1280, "height": 720,
                   "bitrate_kbps": 1500, "gop": 60, "preset": "placebo"},
      "repair": {"preset": "ultrafast"},
      "prompt": "enc prompt",
      "expect": {
        "root_cause": "encoder_too_slow",
        "evidence_candidates": ["encode_p50_ms", "encode_p99_ms", "fps"],
        "allowed_actions": ["pipeline_set_param", "query_metrics", "fetch_observation"],
        "sla": {"fps_min": 25, "lat_p99_ms": 100}
      },
      "budget": {"max_turns": 16, "max_yuan": 0.3, "max_wall_s": 180}
    }
  ]
})json";

json base() { return json::parse(kBase); }

bool has_reason(const eval::GenStats& st, const std::string& needle) {
  for (const auto& [why, n] : st.rejected_reasons) {
    (void)n;
    if (why.find(needle) != std::string::npos) return true;
  }
  return false;
}

const json* template_of(const json& b, const std::string& root) {
  for (const auto& c : b["cases"]) {
    if (c["expect"]["root_cause"] == root) return &c;
  }
  return nullptr;
}

}  // namespace

int main() {
  const json b = base();
  const std::string suite_name = "gen-test";

  // 1) 展开 + id 唯一 + 继承字段 + 轴替换
  {
    const json m = json::parse(R"json({
      "base": "unused.json",
      "seed_range": [40, 80],
      "axes": {
        "fault.kind": ["ingress_drop", "rate"],
        "fault.value": [30, 0.1],
        "pipeline.transcode": [true],
        "pipeline.preset": ["veryfast"]
      },
      "expect": {"network_loss": ["ingress_drop"], "bandwidth_insufficient": ["rate"]}
    })json");
    auto g = eval::generate_cases(m, b, suite_name);
    check(g.has_value(), "case1 展开成功");
    if (!g) {
      std::printf("  error: %s\n", g.error().c_str());
    } else {
      check(g->stats.combinations == 4, "case1 组合数 = 2×2×1×1");
      check(g->stats.generated == 2, "case1 只有 30% 丢包与 0.1Mbps 限速合法");
      check(g->stats.rejected == 2, "case1 0.1% 丢包与 30Mbps 限速被拒");
      check(g->stats.by_root_cause.at("network_loss") == 1, "case1 丢包类 1 个");
      check(g->stats.by_root_cause.at("bandwidth_insufficient") == 1, "case1 限速类 1 个");
      std::set<std::string> ids;
      for (const auto& c : g->suite["cases"]) {
        const std::string id = c["id"].get<std::string>();
        check(ids.insert(id).second, "case1 id 唯一");
        check(id.rfind("gen-", 0) == 0, "case1 id 以 gen- 开头");
        const std::string root = c["expect"]["root_cause"].get<std::string>();
        const json* tpl = template_of(b, root);
        check(tpl != nullptr, "case1 模板存在");
        if (tpl != nullptr) {
          check(c["prompt"] == (*tpl)["prompt"], "case1 prompt 继承");
          check(c["expect"]["sla"] == (*tpl)["expect"]["sla"], "case1 sla 继承");
          check(c["expect"]["evidence_candidates"] == (*tpl)["expect"]["evidence_candidates"],
                "case1 evidence_candidates 继承");
          check(c["expect"]["allowed_actions"] == (*tpl)["expect"]["allowed_actions"],
                "case1 allowed_actions 继承");
          check(c["budget"] == (*tpl)["budget"], "case1 budget 继承");
          if (tpl->contains("repair")) {
            check(c.contains("repair") && c["repair"] == (*tpl)["repair"], "case1 repair 继承");
          } else {
            check(!c.contains("repair"), "case1 无 repair 时也不凭空造");
          }
        }
        const uint64_t seed = c["seed"].get<uint64_t>();
        check(seed >= 40 && seed <= 80, "case1 seed 落在 seed_range");
        check(id.find("-s" + std::to_string(seed)) != std::string::npos,
              "case1 id 含 seed");
        if (root == "network_loss") {
          check(c["fault"]["kind"] == "ingress_drop", "case1 丢包 kind 正确");
          check(c["fault"]["params"]["inject_ingress_pct"] == 30, "case1 丢包值被轴替换");
        } else {
          check(c["fault"]["kind"] == "rate", "case1 限速 kind 正确");
          check(c["fault"]["params"]["rate_mbps"] == 0.1, "case1 限速值被轴替换");
        }
        check(c["pipeline"]["transcode"] == true, "case1 transcode 轴生效");
        check(c["pipeline"]["preset"] == "veryfast", "case1 preset 轴生效");
      }
      // 生成物必须能被 load_suite 直接吃下（与运行期同一套 parse_case）
      const fs::path out = fs::temp_directory_path() / "cue-test-generator-suite.json";
      {
        std::ofstream f(out);
        f << g->suite.dump(2);
      }
      auto loaded = eval::load_suite(out.string());
      check(loaded.has_value(), "case1 生成物可被 load_suite 加载");
      if (loaded) {
        check(loaded->size() == g->stats.generated, "case1 加载后 case 数一致");
        std::set<std::string> loaded_ids;
        for (const auto& c : *loaded) loaded_ids.insert(c.id);
        check(loaded_ids.size() == loaded->size(), "case1 加载后 id 仍唯一");
      } else {
        std::printf("  load error: %s\n", loaded.error().c_str());
      }
      fs::remove(out);
    }
  }

  // 2) count 截断按根因轮流取：小样本不能被展开顺序带偏
  {
    const json m = json::parse(R"json({
      "base": "unused.json",
      "count": 2,
      "groups": [
        {"axes": {"fault.kind": ["ingress_drop"], "fault.value": [30, 40, 50]}},
        {"axes": {"fault.kind": ["rate"], "fault.value": [0.05, 0.1, 0.15]}}
      ],
      "expect": {"network_loss": ["ingress_drop"], "bandwidth_insufficient": ["rate"]}
    })json");
    auto g = eval::generate_cases(m, b, suite_name);
    check(g.has_value(), "case2 展开成功");
    if (g) {
      check(g->stats.generated == 2, "case2 count 截断到 2");
      check(g->stats.by_root_cause.size() == 2, "case2 两类各占一个");
      for (const auto& [root, n] : g->stats.by_root_cause) {
        (void)root;
        check(n == 1, "case2 类别分布均衡（轮流取）");
      }
    } else {
      std::printf("  error: %s\n", g.error().c_str());
    }
  }

  // 3) 非法组合：逐条核对拒绝原因（不是静默丢弃）
  {
    // 3a transcode=off 时编码器轴不生效
    {
      const json m = json::parse(R"json({
        "base": "x", "axes": {"fault.kind": ["ingress_drop"], "fault.value": [50],
        "pipeline.transcode": [false], "pipeline.preset": ["veryfast"]},
        "expect": {"network_loss": ["ingress_drop"]}})json");
      auto g = eval::generate_cases(m, b, suite_name);
      check(g.has_value() && g->stats.generated == 0, "case3a 组合被拒");
      if (g) check(has_reason(g->stats, "transcode=off"), "case3a 拒绝原因含 transcode=off");
    }
    // 3b drop 不改 fps：fps 类 SLA 在故障态就成立
    {
      const json m = json::parse(R"json({
        "base": "x", "axes": {"fault.kind": ["drop"], "fault.value": [20]},
        "expect": {"network_loss": ["drop"]}})json");
      auto g = eval::generate_cases(m, b, suite_name);
      check(g.has_value() && g->stats.generated == 0, "case3b drop 被拒");
      if (g) check(has_reason(g->stats, "只涨 injected"), "case3b 拒绝原因说明 drop 不改 fps");
    }
    // 3c kind=none 且没改管线 = 干净基线
    {
      const json m = json::parse(R"json({
        "base": "x", "axes": {"fault.kind": ["none"]},
        "expect": {"encoder_too_slow": ["none"]}})json");
      auto g = eval::generate_cases(m, b, suite_name);
      check(g.has_value() && g->stats.generated == 0, "case3c 干净基线被拒");
      if (g) check(has_reason(g->stats, "干净基线"), "case3c 拒绝原因说明是基线");
    }
    // 3d kind=none 与 fault.value 互斥
    {
      const json m = json::parse(R"json({
        "base": "x", "axes": {"fault.kind": ["none"], "fault.value": [30],
        "expect.root_cause": ["encoder_too_slow"]},
        "expect": {"encoder_too_slow": ["none"]}})json");
      auto g = eval::generate_cases(m, b, suite_name);
      check(g.has_value() && g->stats.generated == 0, "case3d none+value 被拒");
      if (g) check(has_reason(g->stats, "fault.kind=none 却给了 fault.value"),
                   "case3d 拒绝原因含 none+value");
    }
    // 3e 延迟不足以越过 p99 阈值
    {
      const json m = json::parse(R"json({
        "base": "x", "axes": {"fault.kind": ["delay"], "fault.value": [50]},
        "expect": {"network_delay": ["delay"]}})json");
      auto g = eval::generate_cases(m, b, suite_name);
      check(g.has_value() && g->stats.generated == 0, "case3e 50ms 延迟被拒");
      if (g) check(has_reason(g->stats, "不足以越过"), "case3e 拒绝原因说明余量不足");
    }
    // 3f 同一 kind 对应多个根因却没用 expect.root_cause 消歧
    {
      const json m = json::parse(R"json({
        "base": "x", "axes": {"fault.kind": ["rate"], "fault.value": [0.1]},
        "expect": {"bandwidth_insufficient": ["rate"], "network_loss": ["ingress_drop", "rate"]}})json");
      auto g = eval::generate_cases(m, b, suite_name);
      check(g.has_value() && g->stats.generated == 0, "case3f 歧义 kind 被拒");
      if (g) check(has_reason(g->stats, "对应多个根因"), "case3f 拒绝原因指出需消歧");
    }
    // 3g 轴上的根因与 expect 声明的允许 kind 不符
    {
      const json m = json::parse(R"json({
        "base": "x", "axes": {"expect.root_cause": ["network_loss"],
        "fault.kind": ["rate"], "fault.value": [0.1]},
        "expect": {"network_loss": ["ingress_drop"], "bandwidth_insufficient": ["rate"]}})json");
      auto g = eval::generate_cases(m, b, suite_name);
      check(g.has_value() && g->stats.generated == 0, "case3g 根因/kind 不匹配被拒");
      if (g) check(has_reason(g->stats, "未声明允许 fault.kind=rate"),
                   "case3g 拒绝原因指出 expect 不匹配");
    }
    // 3i 限速低于 cued 参数下限：进程起不来，拿不到任何媒体流
    {
      const json m = json::parse(R"json({
        "base": "x", "axes": {"fault.kind": ["rate"], "fault.value": [0.05]},
        "expect": {"bandwidth_insufficient": ["rate"]}})json");
      auto g = eval::generate_cases(m, b, suite_name);
      check(g.has_value() && g->stats.generated == 0, "case3i 0.05Mbps 限速被拒");
      if (g) check(has_reason(g->stats, "低于 cued --send-mbps 下限"),
                   "case3i 拒绝原因指出进程参数下限");
    }
    // 3h 故障类底子用了慢档：清除注入也修不回来
    {
      const json m = json::parse(R"json({
        "base": "x", "axes": {"fault.kind": ["ingress_drop"], "fault.value": [50],
        "pipeline.transcode": [true], "pipeline.preset": ["placebo"]},
        "expect": {"network_loss": ["ingress_drop"]}})json");
      auto g = eval::generate_cases(m, b, suite_name);
      check(g.has_value() && g->stats.generated == 0, "case3h 慢档底子被拒");
      if (g) check(has_reason(g->stats, "修不回来"), "case3h 拒绝原因说明修复不可达");
    }
  }

  // 3j/3k 管线参数类根因：没有 repair、或没开转码时都不可判
  {
    json b2 = base();
    for (auto& c : b2["cases"]) {
      if (c["expect"]["root_cause"] == "encoder_too_slow") c.erase("repair");
    }
    const json m = json::parse(R"json({
      "base": "x", "axes": {"fault.kind": ["none"], "pipeline.transcode": [true],
      "pipeline.preset": ["placebo"]},
      "expect": {"encoder_too_slow": ["none"]}})json");
    auto g = eval::generate_cases(m, b2, suite_name);
    check(g.has_value() && g->stats.generated == 0, "case3j 无 repair 模板被拒");
    if (g) check(has_reason(g->stats, "没有 repair"), "case3j 拒绝原因指出修复不可达");
  }
  {
    const json m = json::parse(R"json({
      "base": "x", "axes": {"fault.kind": ["none"], "pipeline.transcode": [false]},
      "expect": {"encoder_too_slow": ["none"]}})json");
    auto g = eval::generate_cases(m, b, suite_name);
    check(g.has_value() && g->stats.generated == 0, "case3k 未开转码的编码根因被拒");
    if (g) check(has_reason(g->stats, "需要 transcode=on"),
                 "case3k 拒绝原因指出 passthrough 不跑编码器");
  }

  // 3l 时延类不允许开转码：故障窗口里会混入编码负载，诊断面不唯一
  {
    const json m = json::parse(R"json({
      "base": "x", "axes": {"fault.kind": ["delay"], "fault.value": [300],
      "pipeline.transcode": [true], "pipeline.preset": ["veryfast"]},
      "expect": {"network_delay": ["delay"]}})json");
    auto g = eval::generate_cases(m, b, suite_name);
    check(g.has_value() && g->stats.generated == 0, "case3l 时延+转码被拒");
    if (g) check(has_reason(g->stats, "时延类 case 必须 passthrough"),
                 "case3l 拒绝原因说明诊断歧义");
  }

  // 4) 重复 id 必须报错（不能靠 seed 碰巧不同掩盖）
  {
    const json m = json::parse(R"json({
      "base": "x", "seed_range": [40, 40],
      "groups": [
        {"axes": {"fault.kind": ["ingress_drop"], "fault.value": [50]}},
        {"axes": {"fault.kind": ["ingress_drop"], "fault.value": [50]}}
      ],
      "expect": {"network_loss": ["ingress_drop"]}})json");
    auto g = eval::generate_cases(m, b, suite_name);
    check(!g.has_value(), "case4 重复 id 生成失败");
    if (!g) {
      check(g.error().find("重复 id") != std::string::npos, "case4 错误信息含「重复 id」");
    }
  }

  // 5) 矩阵写错 → 硬错误（不是生成 0 个 case 却返回成功）
  {
    const json m = json::parse(R"json({
      "base": "x", "axes": {"pipeline.width": [1280]},
      "expect": {"network_loss": ["ingress_drop"]}})json");
    auto g = eval::generate_cases(m, b, suite_name);
    check(!g.has_value(), "case5a 宽高单轴被拒");
    if (!g) check(g.error().find("pipeline.size") != std::string::npos,
                  "case5a 错误信息指向 pipeline.size");
  }
  {
    const json m = json::parse(R"json({
      "base": "x", "axes": {"fault.kind": ["delay"], "fault.value": [200]},
      "expect": {"clock_drift": ["delay"]}})json");
    auto g = eval::generate_cases(m, b, suite_name);
    check(!g.has_value(), "case5b 缺模板的根因是硬错误");
    if (!g) check(g.error().find("没有根因 clock_drift") != std::string::npos,
                  "case5b 错误信息指出缺模板");
  }
  {
    const json m = json::parse(R"json({
      "base": "x", "axes": {"fault.kind": ["not_a_kind"]},
      "expect": {"network_loss": ["ingress_drop"]}})json");
    auto g = eval::generate_cases(m, b, suite_name);
    check(!g.has_value(), "case5c 未知 fault.kind 被拒");
  }
  {
    const json m = json::parse(R"json({"base": "x", "count": 0, "axes": {"fault.kind": ["drop"]}})json");
    auto g = eval::generate_cases(m, b, suite_name);
    check(!g.has_value(), "case5d count=0 被拒");
  }
  {
    const json m = json::parse(R"json({
      "base": "x", "seed_range": [80, 40], "axes": {"fault.kind": ["drop"]}})json");
    auto g = eval::generate_cases(m, b, suite_name);
    check(!g.has_value(), "case5e 倒置 seed_range 被拒");
  }
  // manual_only 的模板不能被拿来生成（生成物会被判分器跳过）
  {
    json b2 = base();
    b2["cases"][0]["manual_only"] = true;
    const json m = json::parse(R"json({
      "base": "x", "axes": {"fault.kind": ["ingress_drop"], "fault.value": [50]},
      "expect": {"network_loss": ["ingress_drop"]}})json");
    auto g = eval::generate_cases(m, b2, suite_name);
    check(!g.has_value(), "case5f manual_only 模板被拒");
  }

  // 6) generate_from_files：base 相对矩阵目录解析
  {
    const fs::path dir = fs::temp_directory_path() / "cue-test-generator-dir";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
      std::ofstream f(dir / "base.json");
      f << kBase;
    }
    {
      std::ofstream f(dir / "matrix.json");
      f << R"json({
        "suite": "from-file-suite",
        "base": "./base.json",
        "axes": {"fault.kind": ["delay"], "fault.value": [250]},
        "expect": {"network_delay": ["delay"]}
      })json";
    }
    auto g = eval::generate_from_files((dir / "matrix.json").string());
    check(g.has_value(), "case6 从文件生成成功");
    if (g) {
      check(g->suite["suite"] == "from-file-suite", "case6 输出 suite 名来自矩阵");
      check(g->stats.generated == 1, "case6 生成一个时延 case");
      check(g->suite["note"].get<std::string>().find("cue-eval --gen") != std::string::npos,
            "case6 生成物带 --gen 标记（防覆盖手写 suite）");
    } else {
      std::printf("  error: %s\n", g.error().c_str());
    }
    fs::remove_all(dir);
  }

  // 7) 相同矩阵重复生成 → 逐字节相同（确定性：id/seed/顺序都不能抖）
  {
    const json m = json::parse(R"json({
      "base": "x", "axes": {"fault.kind": ["ingress_drop", "delay"],
      "fault.value": [30, 40, 50, 200], "pipeline.transcode": [false]},
      "expect": {"network_loss": ["ingress_drop"], "network_delay": ["delay"]}})json");
    auto g1 = eval::generate_cases(m, b, suite_name);
    auto g2 = eval::generate_cases(m, b, suite_name);
    check(g1.has_value() && g2.has_value(), "case7 两次生成都成功");
    if (g1 && g2) {
      check(g1->suite.dump() == g2->suite.dump(), "case7 两次生成逐字节相同");
      check(g1->stats.generated > 0, "case7 至少生成一个");
    }
  }

  if (failures == 0) {
    std::printf("test_generator: OK (7 groups)\n");
    return 0;
  }
  std::printf("test_generator: %d failure(s)\n", failures);
  return 1;
}
