// AC-11 沙箱层单测：白名单 / 真实执行 / landlock 写拒绝 / 超时 / 取消 / 输出截断。
//
// 为什么不用 shell 构造测试（`cat > file`、`echo x | sort`）：
// 这一层的全部前提就是"不经过 shell"，用 shell 去测它等于绕开被测对象。
// 写拒绝直接 fork 一个子进程、在 apply_landlock() 之后 open(O_WRONLY)，
// 这样既不需要往白名单里加 tee/echo 之类的命令，也不依赖参数语法。
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cue/sandbox/sandbox.h"

namespace sb = cue::sandbox;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

bool contains(const std::string& hay, const char* needle) {
  return hay.find(needle) != std::string::npos;
}

std::string read_text(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return s;
}

std::vector<std::string> split_nul(const std::string& raw) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : raw) {
    if (c == '\0') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

// 扫 /proc/<pid>/cmdline 找"这个命令还在不在"。用于验证超时/取消后进程组真的被清掉：
// 只检查 run() 的返回值无法证明孙进程没留在后台。
int count_processes_running(const std::vector<std::string>& args) {
  DIR* d = ::opendir("/proc");
  if (d == nullptr) return -1;
  int hits = 0;
  while (dirent* e = ::readdir(d)) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    const std::string path = std::string("/proc/") + e->d_name + "/cmdline";
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) continue;
    char buf[4096];
    const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) continue;
    buf[n] = '\0';
    const auto argv = split_nul(std::string(buf, static_cast<std::size_t>(n)));
    if (argv.size() < args.size()) continue;
    // argv[0] 可能是 /usr/bin/ls：比 basename 就够，参数要逐字一致
    std::string base = argv[0];
    const auto slash = base.find_last_of('/');
    if (slash != std::string::npos) base = base.substr(slash + 1);
    if (base != args[0]) continue;
    bool same = true;
    for (std::size_t i = 1; i < args.size(); ++i) {
      if (argv[i] != args[i]) {
        same = false;
        break;
      }
    }
    if (same) ++hits;
  }
  ::closedir(d);
  return hits;
}

void test_whitelist() {
  const auto& names = sb::allowed_commands();
  check(!names.empty(), "白名单不能为空");
  // 二分查找要求字典序：新增条目忘排序会让 is_allowed_command 静默漏判
  for (std::size_t i = 1; i < names.size(); ++i) {
    check(names[i - 1] < names[i], "白名单必须严格升序（binary_search 的前提）");
  }
  for (const auto& n : names) check(sb::is_allowed_command(n), "白名单内命令应被接受");
  for (const auto& n : names) {
    check(sb::is_allowed_command("/usr/bin/" + n), "/usr/bin/<白名单名> 应被接受");
    check(sb::is_allowed_command("/bin/" + n), "/bin/<白名单名> 应被接受");
  }

  const char* rejected[] = {"rm",          "sh",          "bash",       "sudo",
                            "chmod",       "dd",          "tee",        "mv",
                            "cp",          "python",      "python3",    "nc",
                            "curl",        "wget",        "perl",       "env ",
                            "/usr/bin/rm", "/bin/rm",     "/bin/sh",    "/bin/bash",
                            "../../bin/ls", "/usr/bin/../bin/ls", "/usr/local/bin/ls",
                            "/usr/bin/ls/", "ls/",        "",           ".",
                            ".."};
  for (const char* c : rejected) {
    check(!sb::is_allowed_command(c), "白名单外命令/非法路径必须拒绝");
  }
}

void test_rejected_commands_never_run() {
  // 用「真的会删文件」的命令做哨兵：如果白名单判定漏了、fork/exec 发生，
  // 这个文件就会消失。比只看返回值更能证明"没有子进程被创建"。
  const std::string tmp = "/tmp/cue-ac11-sentinel-" + std::to_string(::getpid());
  {
    std::ofstream out(tmp);
    out << "sentinel\n";
  }
  check(::access(tmp.c_str(), F_OK) == 0, "哨兵文件应创建成功");

  struct Attempt {
    std::string cmd;
    std::vector<std::string> args;
  };
  const Attempt attempts[] = {
      {"rm", {"-f", tmp}},
      {"/bin/rm", {"-f", tmp}},
      {"sh", {"-c", "rm -f " + tmp}},
      {"bash", {"-c", "rm -f " + tmp}},
      {"../../bin/ls", {}},
  };
  for (const auto& a : attempts) {
    sb::Limits limits;
    limits.timeout_ms = 500;
    const auto r = sb::run(a.cmd, a.args, limits, nullptr);
    check(!r.ran, "白名单外命令不得执行（ran=false）");
    check(contains(r.error, "白名单"), "拒绝原因必须说明白名单");
    check(r.duration_ms < 100, "拒绝必须在 fork 之前完成（无子进程、不耗时）");
    check(::access(tmp.c_str(), F_OK) == 0, "白名单外命令绝不能真的执行（哨兵文件必须还在）");
  }
  ::unlink(tmp.c_str());
}

void test_allowed_command_runs() {
  {
    sb::Limits limits;
    const auto r = sb::run("echo", {"hello-ac11"}, limits, nullptr);
    check(r.ran, "echo 应执行");
    check(!r.timed_out && !r.cancelled, "echo 不应超时/取消");
    check(r.exit_code == 0, "echo 退出码应为 0");
    check(r.stdout_text == "hello-ac11\n", "echo 输出应逐字匹配");
    check(r.stderr_text.empty(), "echo 不应有 stderr");
  }
  {
    // 绝对路径形式同样要走通：白名单允许的是名字，execv 用的是解析后的绝对路径
    sb::Limits limits;
    const auto r = sb::run("/usr/bin/echo", {"abs"}, limits, nullptr);
    check(r.ran && r.exit_code == 0 && r.stdout_text == "abs\n", "/usr/bin/echo 应正常执行");
  }
  {
    // 读一个 landlock 明确放行的路径（/etc），验证只读规则没把正常读取一起掐掉
    sb::Limits limits;
    const auto r = sb::run("cat", {"/etc/hostname"}, limits, nullptr);
    check(r.ran, "cat 应执行");
    check(r.exit_code == 0, "cat /etc/hostname 退出码应为 0");
    const std::string expect = read_text("/etc/hostname");
    check(!expect.empty() && r.stdout_text == expect, "cat 应读到与直接读一致的内容");
  }
  {
    // 裸 ls 列工作目录：工作目录只给读不给写，否则诊断命令一上来就是 EACCES
    sb::Limits limits;
    const auto r = sb::run("ls", {}, limits, nullptr);
    check(r.ran, "ls 应执行");
    check(r.exit_code == 0, "ls（工作目录只读放行）应成功");
  }
}

// 在 apply_landlock() 之后 fork 出来的子进程里验证写拒绝：
//   exit 0  = 写 /tmp 被拒、/dev/null 可写（符合预期）
//   exit 1  = 写 /tmp 竟然成功（landlock 没生效）
//   exit 3  = /dev/null 打不开（例外规则漏了，会让正常命令的重定向失败）
//   exit 42 = landlock 不可用（旧内核），跳过而不是假绿
void test_landlock_write_denied() {
  bool degraded = false;
  std::string error;
  // 先在当前进程之外的环境验证一次能力（不在测试进程里 restrict，否则后续用例全被限死）
  const pid_t pid = ::fork();
  if (pid == 0) {
    bool deg = false;
    std::string err;
    if (!sb::apply_landlock(deg, err)) ::_exit(42);
    const std::string path = "/tmp/cue-ac11-write-" + std::to_string(::getpid());
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
      ::close(fd);
      ::unlink(path.c_str());
      ::_exit(1);
    }
    const int nfd = ::open("/dev/null", O_WRONLY);
    if (nfd < 0) ::_exit(3);
    ::close(nfd);
    ::_exit(0);
  }
  check(pid > 0, "fork 失败");
  int status = 0;
  check(::waitpid(pid, &status, 0) == pid, "waitpid 失败");
  if (!WIFEXITED(status)) {
    check(false, "landlock 子进程异常退出");
    return;
  }
  const int code = WEXITSTATUS(status);
  if (code == 42) {
    std::printf("SKIP: landlock 不可用（降级路径），写拒绝未验证\n");
    return;
  }
  check(code == 0, "landlock 必须拒绝写 /tmp 且放行写 /dev/null");
  (void)degraded;
  (void)error;
}

void test_timeout() {
  sb::Limits limits;
  limits.timeout_ms = 50;
  const auto t0 = std::chrono::steady_clock::now();
  const auto r = sb::run("ls", {"-R", "/usr"}, limits, nullptr);
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
          .count();
  check(r.ran, "超时的命令确实执行过（ran=true）");
  check(r.timed_out, "ls -R /usr 应在 50ms 内被判定超时");
  check(!r.cancelled, "超时不是取消");
  check(elapsed < 3000, "超时后必须尽快返回（不能等命令自己结束）");
  // 进程组清理：SIGKILL 之后不应该还有 ls -R /usr 在跑
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  check(count_processes_running({"ls", "-R", "/usr"}) == 0, "超时后进程组必须被清理（无残留）");
}

void test_cancel() {
  sb::Limits limits;
  limits.timeout_ms = 30000;  // 足够长：如果 cancel 没生效，这个用例会卡 30 秒
  std::atomic<bool> cancel{false};
  std::thread killer([&cancel] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    cancel.store(true);
  });
  const auto t0 = std::chrono::steady_clock::now();
  const auto r = sb::run("ls", {"-R", "/usr"}, limits, &cancel);
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
          .count();
  killer.join();
  check(r.ran, "取消前命令已开始执行");
  check(r.cancelled, "置位 cancel 后应返回 cancelled=true");
  check(!r.timed_out, "取消优先于超时");
  check(elapsed < 200, "取消后 200ms 内必须返回（AC-8 的 5 秒预算）");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  check(count_processes_running({"ls", "-R", "/usr"}) == 0, "取消后进程组必须被清理（无残留）");
}

void test_output_truncation() {
  sb::Limits limits;
  limits.max_output_bytes = 64;
  const auto r = sb::run("ls", {"/usr/bin"}, limits, nullptr);
  check(r.ran && r.exit_code == 0, "ls /usr/bin 应成功");
  check(r.stdout_truncated, "64 字节上限下 ls /usr/bin 应标记截断");
  check(r.stdout_bytes > 64, "审计用的 stdout_bytes 必须是截断前的真实字节数");
  // 截断后最多是「上限 + 标记」，不能把整个输出攒在内存里
  check(r.stdout_text.size() <= 64 + 32, "stdout_text 必须不超过上限 + 截断标记");
  check(contains(r.stdout_text, "[truncated]"), "截断必须有可见标记");
}

}  // namespace

int main() {
  test_whitelist();
  test_rejected_commands_never_run();
  test_allowed_command_runs();
  test_landlock_write_denied();
  test_timeout();
  test_cancel();
  test_output_truncation();

  if (failures != 0) {
    std::printf("test_sandbox: %d 个断言失败\n", failures);
    return 1;
  }
  std::printf("test_sandbox: 全部通过\n");
  return 0;
}
