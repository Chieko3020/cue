// 配置文件解析与校验（FR-OBS.5）
#include "cue/common/config.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace cue::config {
namespace {

// UTF-8 续接字节不单独占一列：否则 "第 5 行第 30 列"里的列号会和编辑器对不上。
bool is_utf8_continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

std::vector<std::string> split_dots(std::string_view key) {
  std::vector<std::string> out;
  std::string cur;
  for (const char ch : key) {
    if (ch == '.') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(ch);
    }
  }
  if (!cur.empty()) out.push_back(std::move(cur));
  return out;
}

// 逐段下钻；任何一段不是对象或不存在都返回 nullptr。
const nlohmann::json* walk(const nlohmann::json& root, std::string_view key) {
  const nlohmann::json* cur = &root;
  for (const auto& seg : split_dots(key)) {
    if (!cur->is_object()) return nullptr;
    const auto it = cur->find(seg);
    if (it == cur->end()) return nullptr;
    cur = &*it;
  }
  return cur;
}

std::string join_enum(std::span<const std::string_view> allowed) {
  std::string s;
  for (std::size_t i = 0; i < allowed.size(); ++i) s += (i == 0 ? "" : "|") + std::string(allowed[i]);
  return s;
}

}  // namespace

std::string Error::text() const {
  std::string out = path.empty() ? std::string("cue") : path;
  if (line > 0) {
    out += ":" + std::to_string(line);
    if (column > 0) out += ":" + std::to_string(column);
  }
  out += ": ";
  out += message;
  return out;
}

Position position_of_byte(std::string_view text, std::size_t byte_offset) {
  // nlohmann 给的 byte 是 1-based：第 1 个字节 = 行 1 列 1。
  const std::size_t end = byte_offset == 0 ? 0 : std::min(byte_offset - 1, text.size());
  Position p{1, 1};
  for (std::size_t i = 0; i < end; ++i) {
    const char c = text[i];
    if (c == '\n') {
      ++p.line;
      p.column = 1;
    } else if (!is_utf8_continuation(c)) {
      ++p.column;
    }
  }
  return p;
}

std::vector<std::string> Doc::split_lines(const std::string& text) {
  std::vector<std::string> out;
  std::string cur;
  for (const char ch : text) {
    if (ch == '\n') {
      out.push_back(cur);
      cur.clear();
    } else if (ch != '\r') {
      cur.push_back(ch);
    }
  }
  out.push_back(cur);
  return out;
}

std::expected<Doc, Error> Doc::load(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::unexpected(Error{path, 0, 0,
                                 std::string("无法打开配置文件：") + std::strerror(errno)});
  }
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return from_text(std::move(text), path);
}

std::expected<Doc, Error> Doc::from_text(std::string text, std::string path) {
  Doc doc;
  doc.path_ = std::move(path);
  doc.text_ = std::move(text);
  doc.lines_ = split_lines(doc.text_);
  try {
    doc.root_ = nlohmann::json::parse(doc.text_);
  } catch (const nlohmann::json::parse_error& e) {
    const Position pos = position_of_byte(doc.text_, e.byte);
    return std::unexpected(
        Error{doc.path_, pos.line, pos.column, std::string("JSON 语法错误：") + e.what()});
  }
  if (!doc.root_.is_object()) {
    // 顶层必须是对象：否则 "serve.port" 这种点号路径全都不成立。
    return std::unexpected(Error{doc.path_, 1, 1, "配置文件顶层必须是 JSON 对象"});
  }
  return doc;
}

const nlohmann::json* Doc::find(std::string_view key) const { return walk(root_, key); }

bool Doc::contains(std::string_view key) const { return find(key) != nullptr; }

const nlohmann::json& Doc::at(std::string_view key) const { return *find(key); }

int Doc::line_of(std::string_view key) const {
  // 沿点号路径逐段定位：先找到 "serve" 所在行，再从它之后找 "port"。同名的嵌套键
  // 不会互相串（"eval.cued_bin" 不会命中 "serve.cued_bin" 那一段）。
  std::size_t start = 0;
  int line = 0;
  for (const auto& seg : split_dots(key)) {
    const std::string needle = "\"" + seg + "\"";
    line = 0;
    for (std::size_t i = start; i < lines_.size(); ++i) {
      if (lines_[i].find(needle) != std::string::npos) {
        line = static_cast<int>(i + 1);
        start = i + 1;
        break;
      }
    }
    if (line == 0) return 0;  // 这一层没在原文里出现（键由代码合成 / 文件被别的工具改写）
  }
  return line;
}

Error Doc::error(std::string_view key, std::string message) const {
  return Error{path_, line_of(key), 0, std::move(message)};
}

std::expected<std::string, Error> Doc::get_string(std::string_view key) const {
  const auto* n = find(key);
  if (n == nullptr) return std::unexpected(error(key, "缺少键 " + std::string(key)));
  if (!n->is_string()) {
    return std::unexpected(
        error(key, std::string(key) + " 必须是字符串（收到 " + n->type_name() + "）"));
  }
  return n->get<std::string>();
}

std::expected<int64_t, Error> Doc::get_int(std::string_view key, int64_t lo, int64_t hi) const {
  const auto* n = find(key);
  if (n == nullptr) return std::unexpected(error(key, "缺少键 " + std::string(key)));
  if (!n->is_number_integer()) {
    return std::unexpected(
        error(key, std::string(key) + " 必须是整数（收到 " + n->type_name() + "）"));
  }
  const int64_t v = n->get<int64_t>();
  if (v < lo || v > hi) {
    return std::unexpected(error(key, std::string(key) + " 超出范围 " + std::to_string(lo) + ".." +
                                      std::to_string(hi) + "，收到 " + std::to_string(v)));
  }
  return v;
}

std::expected<double, Error> Doc::get_double(std::string_view key, double lo, double hi) const {
  const auto* n = find(key);
  if (n == nullptr) return std::unexpected(error(key, "缺少键 " + std::string(key)));
  if (!n->is_number()) {
    return std::unexpected(
        error(key, std::string(key) + " 必须是数字（收到 " + n->type_name() + "）"));
  }
  const double v = n->get<double>();
  if (!(v >= lo) || !(v <= hi)) {
    return std::unexpected(error(key, std::string(key) + " 超出范围 " + std::to_string(lo) + ".." +
                                      std::to_string(hi) + "，收到 " + std::to_string(v)));
  }
  return v;
}

std::expected<bool, Error> Doc::get_bool(std::string_view key) const {
  const auto* n = find(key);
  if (n == nullptr) return std::unexpected(error(key, "缺少键 " + std::string(key)));
  if (!n->is_boolean()) {
    return std::unexpected(
        error(key, std::string(key) + " 必须是布尔值（收到 " + n->type_name() + "）"));
  }
  return n->get<bool>();
}

std::expected<std::string, Error> Doc::get_enum(
    std::string_view key, std::span<const std::string_view> allowed) const {
  auto v = get_string(key);
  if (!v) return v;
  for (const auto& a : allowed) {
    if (*v == a) return v;
  }
  return std::unexpected(error(key, std::string(key) + " 只能是 " + join_enum(allowed) +
                                    "，收到 " + *v));
}

std::expected<std::string, Error> Doc::get_existing_file(std::string_view key) const {
  auto v = get_string(key);
  if (!v) return v;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(*v, ec)) {
    return std::unexpected(error(key, std::string(key) + " 指向的文件不存在：" + *v +
                                      (ec ? "（" + ec.message() + "）" : "")));
  }
  return v;
}

std::expected<std::string, Error> Doc::get_existing_dir(std::string_view key) const {
  auto v = get_string(key);
  if (!v) return v;
  std::error_code ec;
  if (!std::filesystem::is_directory(*v, ec)) {
    return std::unexpected(error(key, std::string(key) + " 指向的目录不存在：" + *v +
                                      (ec ? "（" + ec.message() + "）" : "")));
  }
  return v;
}

std::expected<void, Error> Doc::reject_unknown_keys(
    std::string_view prefix, std::span<const std::string_view> known) const {
  const nlohmann::json* node = prefix.empty() ? &root_ : find(prefix);
  if (node == nullptr || !node->is_object()) return {};
  for (const auto& [k, _v] : node->items()) {
    if (!k.empty() && k.front() == '_') continue;  // 注释键
    if (std::find(known.begin(), known.end(), k) != known.end()) continue;
    const std::string full = prefix.empty() ? k : std::string(prefix) + "." + k;
    return std::unexpected(error(full, "未知配置键 " + full + "（拼写检查；`_` 开头的键当注释）"));
  }
  return {};
}

std::expected<void, Error> Doc::forbid_together(std::string_view a, std::string_view b) const {
  if (!contains(a) || !contains(b)) return {};
  const auto& va = at(a);
  const auto& vb = at(b);
  const bool a_given = !va.is_null() && !(va.is_string() && va.get<std::string>().empty());
  const bool b_given = !vb.is_null() && !(vb.is_string() && vb.get<std::string>().empty());
  if (!a_given || !b_given) return {};
  const int line_b = line_of(b);
  const int line_a = line_of(a);
  return std::unexpected(Error{path_, line_b != 0 ? line_b : line_a, 0,
                               std::string(a) + " 与 " + std::string(b) +
                                   " 互斥，不能同时给出（配置里同时出现）"});
}

std::expected<void, Error> apply_log_options(const Doc& doc, cue::log::Options& out) {
  if (!doc.contains("log")) return {};
  // 未知键直接拒绝：日志配置写错（例如 "level": "verbose"）时静默退回 info，
  // 会让人以为"debug 打开了但没日志"，查半天。
  static constexpr std::string_view kKnown[] = {"file",    "level", "json", "stderr",
                                                "rotate_mb", "rotate_files", "queue_size"};
  if (auto ok = doc.reject_unknown_keys("log", kKnown); !ok) return ok;

  if (doc.contains("log.file")) {
    auto v = doc.get_string("log.file");
    if (!v) return std::unexpected(v.error());
    if (!v->empty()) {
      // 文件本身可以先不存在（由 sink 创建），但**目录**必须存在：
      // rotating sink 不会替你 mkdir，路径写错时应该在这里就拒绝启动。
      std::error_code ec;
      const auto parent = std::filesystem::path(*v).parent_path();
      if (!parent.empty() && !std::filesystem::is_directory(parent, ec)) {
        return std::unexpected(doc.error("log.file", "log.file 的目录不存在：" + parent.string()));
      }
    }
    out.file = *v;
  }
  if (doc.contains("log.level")) {
    auto v = doc.get_string("log.level");
    if (!v) return std::unexpected(v.error());
    cue::log::Level lvl{};
    if (!cue::log::parse_level(*v, lvl)) {
      return std::unexpected(doc.error("log.level",
                                       "log.level 只能是 debug|info|warn|error，收到 " + *v));
    }
    out.level = *v;
  }
  if (doc.contains("log.json")) {
    auto v = doc.get_bool("log.json");
    if (!v) return std::unexpected(v.error());
    out.json = *v;
  }
  if (doc.contains("log.stderr")) {
    auto v = doc.get_bool("log.stderr");
    if (!v) return std::unexpected(v.error());
    out.stderr_sink = *v;
  }
  if (doc.contains("log.rotate_mb")) {
    auto v = doc.get_int("log.rotate_mb", 1, 4096);
    if (!v) return std::unexpected(v.error());
    out.rotate_mb = static_cast<int>(*v);
  }
  if (doc.contains("log.rotate_files")) {
    auto v = doc.get_int("log.rotate_files", 1, 100);
    if (!v) return std::unexpected(v.error());
    out.rotate_files = static_cast<int>(*v);
  }
  if (doc.contains("log.queue_size")) {
    auto v = doc.get_int("log.queue_size", 16, 1 << 20);
    if (!v) return std::unexpected(v.error());
    out.queue_size = static_cast<std::size_t>(*v);
  }
  return {};
}

}  // namespace cue::config
