// 评测状态的持久化（M4b）
//
// state.jsonl 是 append-only 的：每行一个 case outcome，崩溃/中断后已完成的 case 不重跑。
// 每行带指纹（SPEC §7.7），续跑时只复用指纹一致的 outcome——改了 agent 源码后缓存必然失效。
#pragma once

#include <cstdint>
#include <expected>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "cue/eval/eval.h"

namespace cue::eval {

// ---- 指纹

// 基线指纹：可执行文件（/proc/self/exe）⊕ git describe ⊕ suite 文件内容
std::string base_fingerprint(const std::string& suite_path);
// case 级指纹 = 基线 ⊕ case.id ⊕ seed
std::string fingerprint_for(const std::string& suite_path, const Case& c);

// ---- outcome 序列化

json outcome_to_json(const CaseOutcome& o);
std::expected<CaseOutcome, std::string> outcome_from_json(const json& j);

// ---- state.jsonl

struct CachedOutcome {
  CaseOutcome outcome;
  std::string fingerprint;
};
// key = (case 序号, 第 k 次尝试)
using StateMap = std::map<std::pair<int, int>, CachedOutcome>;

bool append_state_header(const std::string& path, const json& header, std::string& err);
bool append_state(const std::string& path, int case_index, int k, const CaseOutcome& o,
                  const std::string& fingerprint, std::string& err);
// 缺失文件视为错误（续跑拼错 run_id 时必须报错，而不是静默从零开始）
bool load_state(const std::string& path, StateMap& out, std::string& err);
// run header（续跑用它恢复 suite/mode/repeat，而不是拿 CLI 默认值去撞指纹）
std::expected<json, std::string> read_state_header(const std::string& path);

}  // namespace cue::eval
