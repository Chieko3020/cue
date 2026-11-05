// Offline grading of explicitly supplied records. This validates a file's shape,
// not the truth of the source declared by its author.
#pragma once
#include <expected>
#include <map>
#include <string>
#include <vector>
#include "cue/eval/eval.h"

namespace cue::eval {
struct ManualInput {
  std::map<std::string, CaseOutcome> records;
  std::string sha256;
};
std::expected<ManualInput, std::string> load_manual_input(
    const std::string& path, const std::vector<Case>& cases);
CaseOutcome grade_manual_record(const Case& c, const CaseOutcome* record,
                               const std::string& path, const std::string& hash);
} // namespace cue::eval
