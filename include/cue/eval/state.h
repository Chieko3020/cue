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

// 基线指纹：可执行文件（/proc/self/exe）⊕ git describe ⊕ suite 文件内容 ⊕ 生效配置
std::string base_fingerprint(const std::string& suite_path, const std::string& config_hash);
// case 级指纹 = 基线 ⊕ case.id ⊕ seed。基线只算一次（每个 case 重算会重读 suite
// 并 popen("git describe")，30 case × repeat 就是几十个子进程）。
std::string fingerprint_from_base(const std::string& base, const Case& c);
// 等价于 fingerprint_from_base(base_fingerprint(...), c) 的便捷版（单测/一次性用）
std::string fingerprint_for(const std::string& suite_path, const Case& c, const std::string& config_hash);

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

// 同一 run 的独占锁（SPEC §7.7 的续跑语义需要：并发 resume 会双份计费、双份行）
class RunLock {
 public:
  RunLock() = default;
  ~RunLock();
  RunLock(const RunLock&) = delete;
  RunLock& operator=(const RunLock&) = delete;

  // dir 下建 run.lock 并 flock(LOCK_EX|LOCK_NB)；已被占用返回错误
  std::expected<void, std::string> acquire(const std::string& dir);

 private:
  int fd_ = -1;
};

// 一行 JSON 追加（header 与 case 行共用；写失败必须显式报错，不能静默）
std::expected<void, std::string> append_line(const std::string& path, const json& line);
std::expected<void, std::string> append_run_header(const std::string& path, const json& header);
std::expected<void, std::string> append_state(const std::string& path, int case_index, int k,
                                              const CaseOutcome& o, const std::string& fingerprint);
// 缺失文件视为错误（续跑拼错 run_id 时必须报错，而不是静默从零开始）。
// 崩溃截断的最后一行按"无害丢弃"处理（只容忍最后一行），否则整个 resume 直接报废。
std::expected<StateMap, std::string> load_state(const std::string& path);
// run header（续跑用它恢复 suite/mode/repeat，而不是拿 CLI 默认值去撞指纹）
std::expected<json, std::string> read_state_header(const std::string& path);

}  // namespace cue::eval
