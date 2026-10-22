// FR-EVAL.5：case 生成器——注入矩阵 × 参数组合，批量生成**真能跑**的 case。
//
// 为什么单独成模块而不是塞进 eval.cpp：
//  1) 矩阵展开是纯函数（不碰文件、不起管线、不读时钟），单测可以把「展开数量 / id 唯一 /
//     非法组合被拒 / 继承字段正确」逐条钉死；
//  2) 合法性规则是**语义判断**（哪些轴组合物理上不成立），与运行期判分是两件事：
//     混在 2500 行的 eval.cpp 里，出了假绿灯时分不清是规则错还是判分错。
#pragma once

#include <cstddef>
#include <map>
#include <expected>
#include <string>

#include <nlohmann/json.hpp>

namespace cue::eval {

// 生成统计：CLI 的统计行与单测断言取同一份数据，不各算一遍。
struct GenStats {
  std::size_t combinations = 0;  // 笛卡尔积展开后的原始组合数（未过滤）
  std::size_t rejected = 0;      // 被合法性规则排除的组合数
  std::size_t generated = 0;     // 最终写入 suite 的 case 数
  // 按根因分布（统计行要回答"生成了多少、每类多少"）
  std::map<std::string, std::size_t> by_root_cause;
  // 拒绝原因 → 次数。被排除的组合必须能说清为什么，不允许静默丢弃。
  std::map<std::string, std::size_t> rejected_reasons;
};

struct GeneratedSuite {
  nlohmann::ordered_json suite;  // 完整 suite 文档，可直接写盘
  GenStats stats;
};

// 纯函数：matrix 与 base 都是已解析的 JSON。
// base 里每个根因取**第一个** case 当模板——prompt / expect.sla / evidence_candidates /
// allowed_actions / budget / repair 全部原样继承，只有轴上的参数被替换。
std::expected<GeneratedSuite, std::string> generate_cases(const nlohmann::json& matrix,
                                                          const nlohmann::json& base,
                                                          const std::string& suite_name);

// 文件入口：base 路径先按矩阵文件所在目录解析，再退回 cwd（两种都能用）。
// 返回的错误是给人看的中文句子（哪个文件、哪条规则），不是异常穿透。
std::expected<GeneratedSuite, std::string> generate_from_files(const std::string& matrix_path);

}  // namespace cue::eval
