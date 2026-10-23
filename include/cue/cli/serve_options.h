// cue-serve 的启动选项：命令行 > 配置文件 > 内置默认值
//
// 为什么单独一个模块而不是留在 apps/cue-serve.cpp 里：优先级规则（FR-OBS.5）要能被
// 单测直接驱动——"配置文件写 8080、命令行写 9999，结果必须是 9999"这条，如果解析代码
// 待在 main 文件里，就只能靠起进程验，测不到。
#pragma once

#include <expected>
#include <string>

#include "cue/common/config.h"
#include "cue/common/log.h"

namespace cue::cli {

struct ServeOptions {
  std::string bind = "127.0.0.1";
  int port = 8080;
  std::string web_dir;  // 空 = 自动探测 web/dist → web
  // 推流页（原生 JS，不参与 Vite 构建）的根目录。空 = 由 web_dir 推导：
  // web/dist → web（dist 的父目录），web → web。干净 clone 与构建后行为一致。
  std::string stream_dir;
  std::string cued_bin = "./build/cued";
  std::string runs_dir = "./runs";
  std::string approval = "ask";  // ask | auto | deny
  int ws_port = 9000;
  int probe_port = 9001;
  int approval_timeout_s = 30;  // 超时自动拒绝
  bool no_pipeline = false;
  int bitrate_kbps = 1500;
  int gop = 60;
  std::string preset = "veryfast";
  bool transcode = false;
  double inject_drop = 0.0;
  cue::log::Options log;
  std::string config_path;  // 非空 = 用了 --config
  bool help = false;
};

// 解析 argc/argv，先应用 --config 指向的配置文件，再用命令行覆盖。
// 配置文件的错误带"文件:行:列"；命令行错误带程序名（没有行号可言）。
std::expected<ServeOptions, config::Error> parse_serve_options(int argc, char** argv);

void print_serve_usage();

}  // namespace cue::cli
