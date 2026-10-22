// 单一配置文件（FR-OBS.5）：解析、命令行覆盖、行号、互斥项。
//
// 为什么这些必须是单测而不是"起进程试一下"：
//  - 行号是验收条款（"非法配置拒绝启动并给出行号"），而人手数行号最容易数错；
//  - 优先级（命令行 > 配置文件 > 内置默认值）是语义，不是行为——起进程只能看到
//    "端口是 9999"，看不到"这个 9999 是不是覆盖来的"。
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "cue/cli/eval_options.h"
#include "cue/cli/serve_options.h"
#include "cue/common/config.h"

namespace {

int failures = 0;

void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

namespace fs = std::filesystem;

// 临时配置文件：析构即删，测试之间不共享状态。
class TempConfig {
 public:
  TempConfig(const std::string& name, const std::string& text) {
    path_ = fs::temp_directory_path() / ("cue-test-config-" + std::to_string(::getpid()) + "-" + name + ".json");
    std::ofstream out(path_);
    out << text;
  }
  ~TempConfig() {
    std::error_code ec;
    fs::remove(path_, ec);
  }
  const std::string path() const { return path_.string(); }

 private:
  fs::path path_;
};

std::vector<char*> to_argv(std::vector<std::string>& args) {
  std::vector<char*> out;
  out.reserve(args.size());
  for (auto& a : args) out.push_back(a.data());
  return out;
}

// 一行的配置文本（用于断言"第 N 行"）：第 5 行是端口超范围。
const char* kBadPortLine5 = R"({
  "serve": {
    "bind": "127.0.0.1",
    "web_dir": "",
    "port": 70000,
    "approval": "ask"
  }
}
)";

// 语法错误：第 5 行少了一个逗号。
const char* kSyntaxErrorLine5 = R"({
  "serve": {
    "bind": "127.0.0.1"
  }
  "log": {}
}
)";

// 互斥用例**故意不放 cued_bin**：它是相对路径（./build/cued），
// 而 CI 构建到 build-ci —— 本地能过、CI 必挂。get_existing_file 会先拒绝，
// 于是根本走不到互斥检查，两条断言一起失败（这个坑只在 CI 暴露过一次）。
// cued_bin 的存在性校验由下面的 nonexistent 用例单独覆盖，这里不需要它。
const char* kMutualExclusion = R"({
  "eval": {
    "record_tools_dir": "runs/tools-rec",
    "replay_tools_dir": "runs/tools-play"
  }
}
)";

}  // namespace

int main() {
  // ---------------------------------------------------------------- 行号：取值范围
  {
    TempConfig cfg("badport", kBadPortLine5);
    std::vector<std::string> args = {"cue-serve", "--config", cfg.path()};
    auto argv = to_argv(args);
    auto opt = cue::cli::parse_serve_options(static_cast<int>(argv.size()), argv.data());
    check(!opt, "端口 70000 → 拒绝启动");
    if (!opt) {
      check(opt.error().line == 5, "端口超范围报在第 5 行");
      const std::string text = opt.error().text();
      check(text.find(":5:") != std::string::npos, "错误文案里带 :5:（文件:行:列）");
      check(text.find("serve.port") != std::string::npos, "错误文案指出是 serve.port");
      check(text.find("65535") != std::string::npos, "错误文案给出合法范围");
    }
  }

  // ---------------------------------------------------------------- 行号：语法错误
  {
    TempConfig cfg("syntax", kSyntaxErrorLine5);
    std::vector<std::string> args = {"cue-serve", "--config", cfg.path()};
    auto argv = to_argv(args);
    auto opt = cue::cli::parse_serve_options(static_cast<int>(argv.size()), argv.data());
    check(!opt, "缺逗号 → 拒绝启动");
    if (!opt) {
      check(opt.error().line == 5, "JSON 语法错误报在第 5 行");
      check(opt.error().text().find("语法错误") != std::string::npos, "错误文案说明是语法错误");
    }
  }

  // ---------------------------------------------------------------- 互斥项（配置文件内部）
  {
    TempConfig cfg("mutex", kMutualExclusion);
    std::vector<std::string> args = {"cue-eval", "--config", cfg.path()};
    auto argv = to_argv(args);
    auto opt = cue::cli::parse_eval_options(static_cast<int>(argv.size()), argv.data());
    check(!opt, "record_tools_dir 与 replay_tools_dir 同时给 → 拒绝");
    if (!opt) {
      check(opt.error().line == 4, "互斥项报在后出现的那个键（第 4 行）");
      check(opt.error().text().find("互斥") != std::string::npos, "错误文案说明互斥");
    }
  }

  // ---------------------------------------------------------------- 未知键（拼错保护）
  {
    TempConfig cfg("unknown", R"({
  "serve": {
    "approvel": "auto"
  }
}
)");
    std::vector<std::string> args = {"cue-serve", "--config", cfg.path()};
    auto argv = to_argv(args);
    auto opt = cue::cli::parse_serve_options(static_cast<int>(argv.size()), argv.data());
    check(!opt, "未知键 approvel → 拒绝");
    if (!opt) {
      check(opt.error().line == 3, "未知键报在它自己那一行");
      check(opt.error().text().find("未知配置键") != std::string::npos, "错误文案说明是未知键");
    }
  }

  // ---------------------------------------------------------------- 优先级：命令行覆盖配置文件
  {
    TempConfig cfg("override", R"({
  "serve": {
    "port": 8081,
    "approval": "auto",
    "gop": 30
  },
  "eval": {
    "repeat": 5,
    "agent_mode": "real"
  },
  "log": {
    "level": "warn"
  }
}
)");
    {
      std::vector<std::string> args = {"cue-serve", "--config", cfg.path(), "--port", "9999",
                                       "--log-level", "debug"};
      auto argv = to_argv(args);
      auto opt = cue::cli::parse_serve_options(static_cast<int>(argv.size()), argv.data());
      check(static_cast<bool>(opt), "合法配置 + 命令行 → 正常解析");
      if (opt) {
        check(opt->port == 9999, "命令行 --port 覆盖配置文件里的 8081");
        check(opt->approval == "auto", "配置文件里的 approval 生效（命令行没给）");
        check(opt->gop == 30, "配置文件里的 gop 生效");
        check(opt->log.level == "debug", "命令行 --log-level 覆盖配置文件里的 warn");
        check(opt->bind == "127.0.0.1", "没被任何一方给的项保持内置默认值");
      }
    }
    {
      std::vector<std::string> args = {"cue-eval", "--config", cfg.path(), "--repeat", "2"};
      auto argv = to_argv(args);
      auto opt = cue::cli::parse_eval_options(static_cast<int>(argv.size()), argv.data());
      check(static_cast<bool>(opt), "评测：合法配置 + 命令行 → 正常解析");
      if (opt) {
        check(opt->cfg.repeat == 2, "命令行 --repeat 覆盖配置文件里的 5");
        check(opt->cfg.agent_mode == "real", "配置文件里的 agent_mode 生效");
        check(opt->mode_set, "配置文件给了 agent_mode → mode_set 为真（续跑校验依赖它）");
        check(opt->log.level == "warn", "配置文件里的 log.level 生效");
        check(!opt->suite_set, "配置文件没给 suite → suite_set 仍为假");
      }
    }
    {
      // 只给配置文件：字段全部来自文件，默认值不被改写。
      std::vector<std::string> args = {"cue-serve", "--config", cfg.path()};
      auto argv = to_argv(args);
      auto opt = cue::cli::parse_serve_options(static_cast<int>(argv.size()), argv.data());
      check(static_cast<bool>(opt), "只给配置文件也能解析");
      if (opt) {
        check(opt->port == 8081, "只给配置文件：端口来自配置");
        check(opt->bitrate_kbps == 1500, "只给配置文件：没写的项仍是内置默认值 1500");
      }
    }
  }

  // ---------------------------------------------------------------- 枚举与类型错误
  {
    TempConfig cfg("enum", R"({
  "serve": {
    "approval": "sometimes"
  },
  "log": {
    "level": "verbose"
  }
}
)");
    std::vector<std::string> args = {"cue-serve", "--config", cfg.path()};
    auto argv = to_argv(args);
    auto opt = cue::cli::parse_serve_options(static_cast<int>(argv.size()), argv.data());
    check(!opt, "approval 非法枚举 → 拒绝");
    if (opt) {
      check(false, "approval=auto|ask|deny 之外的取值不该被接受");
    } else {
      check(opt.error().line == 3, "枚举错误报在 approval 那一行");
    }

    // log.level 的枚举在 apply_log_options 里（同一个 Doc，同一套行号）
    TempConfig cfg2("loglevel", R"({
  "serve": {
    "port": 8080
  },
  "log": {
    "level": "verbose"
  }
}
)");
    std::vector<std::string> args2 = {"cue-serve", "--config", cfg2.path()};
    auto argv2 = to_argv(args2);
    auto opt2 = cue::cli::parse_serve_options(static_cast<int>(argv2.size()), argv2.data());
    check(!opt2, "log.level 非法 → 拒绝（不静默退回 info）");
    if (!opt2) check(opt2.error().line == 6, "log.level 错误报在它自己那一行");
  }

  // ---------------------------------------------------------------- 路径存在性
  {
    TempConfig cfg("missing-file", R"({
  "serve": {
    "cued_bin": "./definitely-not-here/cued"
  }
}
)");
    std::vector<std::string> args = {"cue-serve", "--config", cfg.path()};
    auto argv = to_argv(args);
    auto opt = cue::cli::parse_serve_options(static_cast<int>(argv.size()), argv.data());
    check(!opt, "cued_bin 指向不存在的文件 → 拒绝");
    if (!opt) {
      check(opt.error().line == 3, "路径错误报在 cued_bin 那一行");
      check(opt.error().text().find("不存在") != std::string::npos, "错误文案说明路径不存在");
    }
  }

  // ---------------------------------------------------------------- CLI-only 错误没有行号
  {
    std::vector<std::string> args = {"cue-serve", "--port", "70000"};
    auto argv = to_argv(args);
    auto opt = cue::cli::parse_serve_options(static_cast<int>(argv.size()), argv.data());
    check(!opt, "命令行端口超范围 → 拒绝");
    if (!opt) {
      check(opt.error().line == 0, "命令行错误没有行号（不编一个假行号）");
      check(opt.error().text().rfind("cue-serve: ", 0) == 0, "命令行错误以程序名开头");
    }
  }

  // ---------------------------------------------------------------- 配置文件不影响既有默认值
  {
    // 空对象配置：一个键都没写 → 与完全不给 --config 等价。
    TempConfig cfg("empty", "{}\n");
    std::vector<std::string> with_cfg = {"cue-serve", "--config", cfg.path()};
    std::vector<std::string> without = {"cue-serve"};
    auto argv_a = to_argv(with_cfg);
    auto argv_b = to_argv(without);
    auto a = cue::cli::parse_serve_options(static_cast<int>(argv_a.size()), argv_a.data());
    auto b = cue::cli::parse_serve_options(static_cast<int>(argv_b.size()), argv_b.data());
    check(a && b, "空配置与不给配置都能解析");
    if (a && b) {
      check(a->port == b->port, "空配置：端口默认值不变");
      check(a->ws_port == b->ws_port && a->probe_port == b->probe_port, "空配置：端口默认值不变（ws/probe）");
      check(a->approval == b->approval && a->preset == b->preset, "空配置：枚举/字符串默认值不变");
      check(a->bitrate_kbps == b->bitrate_kbps && a->gop == b->gop, "空配置：数值默认值不变");
      check(a->log.level == b->log.level && a->log.json == b->log.json, "空配置：日志默认值不变");
    }
  }

  // ---------------------------------------------------------------- position_of_byte（行列换算）
  {
    const std::string text = "{\n  \"a\": 1,\n  \"b\": bad\n}\n";
    const auto p = cue::config::position_of_byte(text, text.find("bad") + 1);
    check(p.line == 3, "字节偏移 → 行号 3");
    check(p.column == 8, "字节偏移 → 列号（按 1-based）");
  }

  if (failures == 0) {
    std::printf("test_config: OK\n");
    return 0;
  }
  std::printf("test_config: %d failure(s)\n", failures);
  return 1;
}
