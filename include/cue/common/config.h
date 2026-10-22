// 单一配置文件（FR-OBS.5）：JSON + 启动时校验 + **报行号**
//
// 为什么选 JSON 而不是 TOML：nlohmann 已经是硬依赖（指标、事件、suite、report 全走它），
// 选 TOML 就等于为了配置文件再引一个解析器；而 FR-OBS.5 只要求"单一 TOML/JSON 文件"。
// 代价是 JSON 不能写注释——所以本模块把 `_` 开头的键当注释放行（见 reject_unknown_keys）。
//
// 行号怎么来（这是本文件存在的核心理由）：
//  - **语法错误**：nlohmann 的 parse_error 带 byte 偏移（1-based），position_of_byte 把它
//    换算成行列；
//  - **语义错误**（范围/枚举/路径/互斥）：JSON 解析器不会告诉你"这个值在第几行"，所以
//    按点号路径逐段在原文里定位（`serve.port` → 先找 "serve" 那段、再从它之后找 "port"）。
//    多层同名键不会串行；代价是值写得特别分散时要按段名去认，这一点在 README 里写明。
//
// 校验只在"配置文件显式写了这个键"时发生：没写就不校验、不覆盖默认值。
// 所以配置文件**不会**改变任何既有命令行默认语义（FR-OBS.5 之外的另一条硬要求）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "cue/common/log.h"

namespace cue::config {

// 配置/校验错误。line == 0 表示没有对应行（命令行参数错误、文件打不开）。
struct Error {
  std::string path;  // 配置文件路径；空 = 不是配置文件的错误（例如命令行）
  int line = 0;
  int column = 0;
  std::string message;

  // "cue.json:5:12: 消息" / "cue.json: 消息" / "cue-serve: 消息"
  std::string text() const;
};

struct Position {
  int line = 0;
  int column = 0;
};

// nlohmann parse_error::byte（1-based）→ 1-based 行列。列按 UTF-8 码点计（不是字节）。
Position position_of_byte(std::string_view text, std::size_t byte_offset);

class Doc {
 public:
  // 读文件 + 解析。语法错误带行列；文件打不开带路径（line=0）。
  static std::expected<Doc, Error> load(const std::string& path);

  // 也可以直接用一段文本构造（单测与"配置内嵌"场景）：path 只用于报错文案。
  static std::expected<Doc, Error> from_text(std::string text, std::string path);

  const nlohmann::json& root() const { return root_; }
  const std::string& path() const { return path_; }
  const std::string& text() const { return text_; }

  // 点号路径是否存在（"serve.port"）。路径中途不是对象也返回 false。
  bool contains(std::string_view key) const;
  // 取子节点；调用前必须先 contains（否则抛 out_of_range）。
  const nlohmann::json& at(std::string_view key) const;
  // 键所在行：按段名沿原文向下定位；找不到返回 0。
  int line_of(std::string_view key) const;
  // 以某键为上下文构造错误（自动补 path 与行号）。
  Error error(std::string_view key, std::string message) const;

  // ---- 类型 / 范围 / 枚举 / 路径 / 互斥：失败一律带行号 ----
  std::expected<std::string, Error> get_string(std::string_view key) const;
  std::expected<int64_t, Error> get_int(std::string_view key, int64_t lo, int64_t hi) const;
  std::expected<double, Error> get_double(std::string_view key, double lo, double hi) const;
  std::expected<bool, Error> get_bool(std::string_view key) const;
  std::expected<std::string, Error> get_enum(std::string_view key,
                                             std::span<const std::string_view> allowed) const;
  std::expected<std::string, Error> get_existing_file(std::string_view key) const;
  std::expected<std::string, Error> get_existing_dir(std::string_view key) const;
  // 对象里出现未知键 → 报错。拼错的键静默无效比报错更难查；`_` 开头的键当注释放行。
  std::expected<void, Error> reject_unknown_keys(std::string_view prefix,
                                                 std::span<const std::string_view> known) const;
  // 两个键互斥（例如 record_tools_dir 与 replay_tools_dir）。同时给出时，错误报在
  // **后出现**的那个键的行上——通常是后来加的那一行把两件事放在了一起。
  std::expected<void, Error> forbid_together(std::string_view a, std::string_view b) const;

 private:
  Doc() = default;
  const nlohmann::json* find(std::string_view key) const;
  static std::vector<std::string> split_lines(const std::string& text);

  std::string path_;
  std::string text_;
  std::vector<std::string> lines_;
  nlohmann::json root_;
};

// "log" 段 → 日志选项（file / level / json / stderr / rotate_mb / rotate_files）。
// 未出现的键保持默认值；非法值带行号返回。
std::expected<void, Error> apply_log_options(const Doc& doc, cue::log::Options& out);

}  // namespace cue::config
