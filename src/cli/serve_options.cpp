// cue-serve 选项解析：配置文件 → 命令行覆盖（FR-OBS.5）
#include "cue/cli/serve_options.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <vector>

namespace cue::cli {
namespace {

constexpr const char* kProg = "cue-serve";

config::Error cli_error(std::string message) { return config::Error{kProg, 0, 0, std::move(message)}; }

bool parse_int(std::string_view s, int& out, int lo, int hi) {
  if (s.empty()) return false;
  std::string tmp(s);
  char* end = nullptr;
  errno = 0;
  const long v = std::strtol(tmp.c_str(), &end, 10);
  if (errno != 0 || end == nullptr || *end != '\0' || v < lo || v > hi) return false;
  out = static_cast<int>(v);
  return true;
}

bool parse_double(std::string_view s, double& out, double lo, double hi) {
  if (s.empty()) return false;
  std::string tmp(s);
  char* end = nullptr;
  errno = 0;
  const double v = std::strtod(tmp.c_str(), &end);
  if (errno != 0 || end == nullptr || *end != '\0' || !(v >= lo) || !(v <= hi)) return false;
  out = v;
  return true;
}

// ---- 配置文件里的 serve 段 ----

std::expected<void, config::Error> apply_serve_config(const config::Doc& doc, ServeOptions& o) {
  static constexpr std::string_view kKnown[] = {
      "bind",     "port",        "web_dir",    "stream_dir", "cued_bin", "runs_dir", "approval",
      "ws_port",  "probe_port",  "approval_timeout_s", "no_pipeline", "bitrate_kbps",
      "gop",      "preset",      "transcode",  "inject_drop"};
  if (auto ok = doc.reject_unknown_keys("serve", kKnown); !ok) {
    return std::unexpected(ok.error());
  }
  if (doc.contains("serve.bind")) {
    auto v = doc.get_string("serve.bind");
    if (!v) return std::unexpected(v.error());
    o.bind = *v;
  }
  if (doc.contains("serve.port")) {
    auto v = doc.get_int("serve.port", 1, 65535);
    if (!v) return std::unexpected(v.error());
    o.port = static_cast<int>(*v);
  }
  if (doc.contains("serve.web_dir")) {
    auto v = doc.get_string("serve.web_dir");
    if (!v) return std::unexpected(v.error());
    if (!v->empty()) {
      // 给了静态目录就必须存在：否则控制台起来后每个请求都是 404，而日志里只有
      // "静态目录：web"，看不出是配置写错了。
      std::error_code ec;
      if (!std::filesystem::is_directory(*v, ec)) {
        return std::unexpected(doc.error("serve.web_dir", "serve.web_dir 指向的目录不存在：" + *v));
      }
    }
    o.web_dir = *v;
  }
  if (doc.contains("serve.stream_dir")) {
    auto v = doc.get_string("serve.stream_dir");
    if (!v) return std::unexpected(v.error());
    if (!v->empty()) {
      std::error_code ec;
      if (!std::filesystem::is_directory(*v, ec)) {
        return std::unexpected(
            doc.error("serve.stream_dir", "serve.stream_dir 指向的目录不存在：" + *v));
      }
    }
    o.stream_dir = *v;
  }
  if (doc.contains("serve.cued_bin")) {
    auto v = doc.get_existing_file("serve.cued_bin");
    if (!v) return std::unexpected(v.error());
    o.cued_bin = *v;
  }
  if (doc.contains("serve.runs_dir")) {
    auto v = doc.get_string("serve.runs_dir");
    if (!v) return std::unexpected(v.error());
    o.runs_dir = *v;  // 目录可以先不存在（评测会创建），不校验存在性
  }
  if (doc.contains("serve.approval")) {
    static constexpr std::string_view kModes[] = {"ask", "auto", "deny"};
    auto v = doc.get_enum("serve.approval", kModes);
    if (!v) return std::unexpected(v.error());
    o.approval = *v;
  }
  if (doc.contains("serve.ws_port")) {
    auto v = doc.get_int("serve.ws_port", 1, 65535);
    if (!v) return std::unexpected(v.error());
    o.ws_port = static_cast<int>(*v);
  }
  if (doc.contains("serve.probe_port")) {
    auto v = doc.get_int("serve.probe_port", 1, 65535);
    if (!v) return std::unexpected(v.error());
    o.probe_port = static_cast<int>(*v);
  }
  if (doc.contains("serve.approval_timeout_s")) {
    auto v = doc.get_int("serve.approval_timeout_s", 1, 600);
    if (!v) return std::unexpected(v.error());
    o.approval_timeout_s = static_cast<int>(*v);
  }
  if (doc.contains("serve.no_pipeline")) {
    auto v = doc.get_bool("serve.no_pipeline");
    if (!v) return std::unexpected(v.error());
    o.no_pipeline = *v;
  }
  if (doc.contains("serve.bitrate_kbps")) {
    auto v = doc.get_int("serve.bitrate_kbps", 100, 20000);
    if (!v) return std::unexpected(v.error());
    o.bitrate_kbps = static_cast<int>(*v);
  }
  if (doc.contains("serve.gop")) {
    auto v = doc.get_int("serve.gop", 1, 300);
    if (!v) return std::unexpected(v.error());
    o.gop = static_cast<int>(*v);
  }
  if (doc.contains("serve.preset")) {
    auto v = doc.get_string("serve.preset");
    if (!v) return std::unexpected(v.error());
    o.preset = *v;
  }
  if (doc.contains("serve.transcode")) {
    auto v = doc.get_bool("serve.transcode");
    if (!v) return std::unexpected(v.error());
    o.transcode = *v;
  }
  if (doc.contains("serve.inject_drop")) {
    auto v = doc.get_double("serve.inject_drop", 0.0, 100.0);
    if (!v) return std::unexpected(v.error());
    o.inject_drop = *v;
  }
  return {};
}

// 命令行取值：`--opt value` 与 `--opt=value` 都认（配置文件里的路径常带等号式习惯）。
bool take_value(int argc, char** argv, int& i, std::string& out) {
  const std::string a = argv[i];
  const auto eq = a.find('=');
  if (eq != std::string::npos) {
    out = a.substr(eq + 1);
    return true;
  }
  if (i + 1 >= argc) return false;
  out = argv[++i];
  return true;
}

// 参数名（去掉 = 之后的部分），用于查表。
std::string_view opt_name(const char* arg) {
  std::string_view a(arg);
  const auto eq = a.find('=');
  return eq == std::string_view::npos ? a : a.substr(0, eq);
}

}  // namespace

void print_serve_usage() {
  std::printf(
      "usage: cue-serve [options]\n"
      "  --config PATH          读 JSON 配置文件（命令行覆盖配置文件；非法配置带行号拒绝启动）\n"
      "  --bind ADDR            监听地址（默认 127.0.0.1，仅回环）\n"
      "  --port N               监听端口（默认 8080）\n"
      "  --web DIR              静态目录（默认自动：web/dist 存在则用它，否则 web/）\n"
      "  --stream-dir DIR       推流页根（默认由 --web 推导：dist 的父目录；/stream 走它）\n"
      "  --cued BIN             cued 可执行文件（默认 ./build/cued）\n"
      "  --runs DIR             评测产物目录（默认 ./runs）\n"
      "  --approval MODE        ask（默认，审批 UI 决定）/ auto（自动批准）/ deny（拒绝非只读）\n"
      "  --ws-port N            cued 的 WS 端口（默认 9000）\n"
      "  --probe-port N         cued 的 UDP 目标端口（默认 9001）\n"
      "  --approval-timeout S   审批超时秒数（默认 30，超时自动拒绝）\n"
      "  --no-pipeline          不启动 cued（只用控制台看状态/评测）\n"
      "  --bitrate-kbps N       初始码率（默认 1500）\n"
      "  --gop N                初始 GOP（默认 60）\n"
      "  --preset NAME          初始 x264 preset（默认 veryfast）\n"
      "  --transcode            初始打开转码\n"
      "  --inject-drop PCT      启动时注入丢包百分比（默认 0）\n"
      "  --log-file PATH        结构化日志文件（可轮转；默认只写 stderr）\n"
      "  --log-level LEVEL      debug|info|warn|error（默认 info）\n"
      "  --log-text             人读格式（默认 JSON-line；FR-OBS.1 的口径是 JSON）\n"
      "  --log-json             强制 JSON-line（默认即是）\n"
      "  --help                 打印本帮助\n"
      "\n"
      "退出：Ctrl-C / SIGTERM。会先停 cued 再关闭监听（不留孤儿进程）。\n");
}

std::expected<ServeOptions, config::Error> parse_serve_options(int argc, char** argv) {
  ServeOptions o;

  // 第一遍：只找 --config。命令行因此天然覆盖配置文件（第二遍最后写）。
  for (int i = 1; i < argc; ++i) {
    const auto name = opt_name(argv[i]);
    if (name == "--config") {
      std::string v;
      if (!take_value(argc, argv, i, v)) return std::unexpected(cli_error("--config 缺少取值"));
      o.config_path = v;
    }
  }
  if (!o.config_path.empty()) {
    auto doc = config::Doc::load(o.config_path);
    if (!doc) return std::unexpected(doc.error());
    if (auto ok = apply_serve_config(*doc, o); !ok) return std::unexpected(ok.error());
    if (auto ok = config::apply_log_options(*doc, o.log); !ok) return std::unexpected(ok.error());
  }

  // 第二遍：命令行。
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const std::string_view name = opt_name(argv[i]);
    std::string v;
    if (name == "--help" || name == "-h") {
      o.help = true;
      return o;
    } else if (name == "--config") {
      if (!take_value(argc, argv, i, v)) return std::unexpected(cli_error("--config 缺少取值"));
    } else if (name == "--bind") {
      if (!take_value(argc, argv, i, v)) return std::unexpected(cli_error("--bind 缺少取值"));
      o.bind = v;
    } else if (name == "--port") {
      if (!take_value(argc, argv, i, v)) return std::unexpected(cli_error("--port 缺少取值"));
      if (!parse_int(v, o.port, 1, 65535)) {
        return std::unexpected(cli_error("--port 取值非法：" + v));
      }
    } else if (name == "--web") {
      if (!take_value(argc, argv, i, v)) return std::unexpected(cli_error("--web 缺少取值"));
      o.web_dir = v;
    } else if (name == "--stream-dir") {
      if (!take_value(argc, argv, i, v)) return std::unexpected(cli_error("--stream-dir 缺少取值"));
      o.stream_dir = v;
    } else if (name == "--cued") {
      if (!take_value(argc, argv, i, v)) return std::unexpected(cli_error("--cued 缺少取值"));
      o.cued_bin = v;
    } else if (name == "--runs") {
      if (!take_value(argc, argv, i, v)) return std::unexpected(cli_error("--runs 缺少取值"));
      o.runs_dir = v;
    } else if (name == "--approval") {
      if (!take_value(argc, argv, i, v)) return std::unexpected(cli_error("--approval 缺少取值"));
      if (v != "ask" && v != "auto" && v != "deny") {
        return std::unexpected(cli_error("--approval 只能是 ask|auto|deny，收到：" + v));
      }
      o.approval = v;
    } else if (name == "--ws-port") {
      if (!take_value(argc, argv, i, v)) return std::unexpected(cli_error("--ws-port 缺少取值"));
      if (!parse_int(v, o.ws_port, 1, 65535)) {
        return std::unexpected(cli_error("--ws-port 取值非法：" + v));
      }
    } else if (name == "--probe-port") {
      if (!take_value(argc, argv, i, v)) return std::unexpected(cli_error("--probe-port 缺少取值"));
      if (!parse_int(v, o.probe_port, 1, 65535)) {
        return std::unexpected(cli_error("--probe-port 取值非法：" + v));
      }
    } else if (name == "--approval-timeout") {
      if (!take_value(argc, argv, i, v)) {
        return std::unexpected(cli_error("--approval-timeout 缺少取值"));
      }
      if (!parse_int(v, o.approval_timeout_s, 1, 600)) {
        return std::unexpected(cli_error("--approval-timeout 取值非法：" + v));
      }
    } else if (name == "--no-pipeline") {
      o.no_pipeline = true;
    } else if (name == "--bitrate-kbps") {
      if (!take_value(argc, argv, i, v)) {
        return std::unexpected(cli_error("--bitrate-kbps 缺少取值"));
      }
      if (!parse_int(v, o.bitrate_kbps, 100, 20000)) {
        return std::unexpected(cli_error("--bitrate-kbps 取值非法：" + v));
      }
    } else if (name == "--gop") {
      if (!take_value(argc, argv, i, v)) return std::unexpected(cli_error("--gop 缺少取值"));
      if (!parse_int(v, o.gop, 1, 300)) return std::unexpected(cli_error("--gop 取值非法：" + v));
    } else if (name == "--preset") {
      if (!take_value(argc, argv, i, v)) return std::unexpected(cli_error("--preset 缺少取值"));
      o.preset = v;
    } else if (name == "--transcode") {
      o.transcode = true;
    } else if (name == "--inject-drop") {
      if (!take_value(argc, argv, i, v)) return std::unexpected(cli_error("--inject-drop 缺少取值"));
      if (!parse_double(v, o.inject_drop, 0.0, 100.0)) {
        return std::unexpected(cli_error("--inject-drop 取值非法：" + v));
      }
    } else if (name == "--log-file") {
      if (!take_value(argc, argv, i, v)) return std::unexpected(cli_error("--log-file 缺少取值"));
      o.log.file = v;
    } else if (name == "--log-level") {
      if (!take_value(argc, argv, i, v)) return std::unexpected(cli_error("--log-level 缺少取值"));
      cue::log::Level lvl{};
      if (!cue::log::parse_level(v, lvl)) {
        return std::unexpected(cli_error("--log-level 只能是 debug|info|warn|error，收到：" + v));
      }
      o.log.level = v;
    } else if (name == "--log-json") {
      o.log.json = true;
    } else if (name == "--log-text") {
      o.log.json = false;
    } else {
      return std::unexpected(cli_error("未知参数：" + a));
    }
  }
  return o;
}

}  // namespace cue::cli
