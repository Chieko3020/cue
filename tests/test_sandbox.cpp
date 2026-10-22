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
#include <sys/stat.h>
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

bool contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }
bool contains(const std::string& hay, const std::string& needle) {
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

// 在 apply_landlock_plan() 之后 fork 出来的子进程里验证写白名单（FR-SANDBOX.2）：
//   exit 0  = /tmp 与 runs/sandbox 可写、项目源码不可写、/dev/null 可写（符合预期）
//   exit 1  = /tmp 写不进（临时目录没放行）
//   exit 2  = runs/sandbox 写不进（FR-SANDBOX.2 的后半句没做到）
//   exit 3  = 项目源码竟然可写（默认拒绝失效，最严重）
//   exit 4  = /dev/null 打不开（例外规则漏了，会让正常命令的重定向失败）
//   exit 5  = 写源码失败但不是 EACCES（例如路径写错，属于假绿，必须重跑）
//   exit 42 = landlock 不可用（旧内核），跳过而不是假绿
//
// 为什么不用 shell 验证 `echo x > runs/id/f`：这一层的全部前提就是"不经过 shell"，
// run_shell 也没有重定向语法（没有解释器）。用同一份 Limits 在子进程里 open() 等价，
// 而且能精确区分"哪一条路径失败"。
void test_landlock_write_paths() {
  sb::Limits limits;  // 默认：ro=工作目录（项目根），rw=/tmp + runs/sandbox
  auto plan = sb::build_landlock_plan(limits);
  if (!plan.usable) {
    std::printf("SKIP: landlock 不可用（%s），写路径未验证\n", plan.unavailable_reason.c_str());
    return;
  }
  check(plan.rejected.empty(), "默认可写路径不应被策略拒绝");
  check(plan.not_applied.empty(), "默认可写路径应当都生效（/tmp 与 runs/sandbox）");

  const pid_t pid = ::fork();
  if (pid == 0) {
    char err[224] = {0};
    bool deg = false;
    if (!sb::apply_landlock_plan(plan, err, sizeof(err), deg)) ::_exit(42);
    const std::string tmp_path = "/tmp/cue-ac11-rw-" + std::to_string(::getpid());
    const int t = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (t < 0) ::_exit(1);
    ::close(t);
    ::unlink(tmp_path.c_str());
    const int rd = ::open("runs/sandbox/probe.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (rd < 0) ::_exit(2);
    ::close(rd);
    ::unlink("runs/sandbox/probe.txt");
    // 项目源码只读：O_WRONLY 就够（landlock 先看 WRITE_FILE，不需要 O_TRUNC）。
    // 必须断言 EACCES 而不是"open 失败就行"：路径不存在也会失败，那是假绿。
    const int src = ::open("src/sandbox/sandbox.cpp", O_WRONLY);
    if (src >= 0) {
      ::close(src);
      ::_exit(3);
    }
    if (errno != EACCES) ::_exit(5);
    const int nfd = ::open("/dev/null", O_WRONLY);
    if (nfd < 0) ::_exit(4);
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
    std::printf("SKIP: landlock 不可用（降级路径），写路径未验证\n");
    return;
  }
  if (code != 0) std::printf("landlock 子进程退出码 %d（1=/tmp 2=runs 3=源码可写 4=/dev/null 5=非权限失败）\n", code);
  check(code == 0, "landlock 必须放行 /tmp 与 runs/<id>、拒绝项目源码、放行 /dev/null");
}

// 危险可写路径必须被**显式拒绝**（整个 run 不执行），而不是静默忽略：
// 静默忽略会让调用方以为它拿到了写权限。
void test_dangerous_rw_path_refused() {
  const char* bad[] = {"/", "/etc", "/usr", "/tmp/..", "/proc"};
  for (const char* p : bad) {
    sb::Limits limits;
    limits.rw_paths = {p};
    const auto r = sb::run("echo", {"x"}, limits, nullptr);
    check(!r.ran, "危险可写路径必须让 run 直接拒绝（不执行）");
    check(contains(r.error, "可写路径被策略拒绝"), "拒绝原因必须写明是可写路径策略");
    check(!r.paths_not_applied.empty(), "被拒绝的路径必须如实列在结果里（审计要用）");
  }
}

// 不存在/建不出来的可写路径：不阻断执行，但必须如实上报（"以为能写"最难查）。
void test_missing_rw_path_reported() {
  // "建不出来"必须对 root 也成立：权限位挡不住 root（CAP_DAC_OVERRIDE），
  // 但用普通文件当父目录会 ENOTDIR——这条与 uid 无关。
  const std::string blocker = "/tmp/cue-ac11-blocker-" + std::to_string(::getpid());
  {
    std::ofstream out(blocker);
    out << "not a directory\n";
  }
  sb::Limits limits;
  limits.rw_paths = {"/tmp/cue-ac11-rw-ok-" + std::to_string(::getpid()), blocker + "/runs"};
  const auto r = sb::run("echo", {"ok"}, limits, nullptr);
  check(r.ran && r.exit_code == 0, "一条路径建不出来不应让命令不执行");
  check(r.rw_paths_applied.size() == 1, "只有建得出来的那条路径应生效");
  check(r.paths_not_applied.size() == 1, "建不出来的路径必须出现在 paths_not_applied 里");
  if (!r.paths_not_applied.empty()) {
    check(contains(r.paths_not_applied[0], blocker + "/runs"),
          "未生效路径要带具体路径，便于审计定位");
  }
  // 该测试自己建的东西自己收掉（/tmp 不该因为跑过单测而积垃圾）
  for (const auto& p : r.rw_paths_applied) ::rmdir(p.c_str());
  ::unlink(blocker.c_str());
}

// FR-SANDBOX.4：root 下子进程必须降到 nobody；非 root 下必须显式记录 not_root。
// 用 `id` 验证真实 uid（只看 Result 字段等于自证）。
void test_privilege_drop() {
  sb::Limits limits;
  const auto r = sb::run("id", {}, limits, nullptr);
  check(r.ran && r.exit_code == 0, "id 应执行成功");
  std::printf("[privdrop] privilege=%s  id: %s", r.privilege_drop.c_str(), r.stdout_text.c_str());
  check(!r.privilege_drop.empty(), "降权结果必须显式记录（不能静默）");
  if (::geteuid() != 0) {
    check(contains(r.privilege_drop, "not_root("), "非 root 时降权结果应为 not_root(uid=…)");
    check(contains(r.stdout_text, "uid=" + std::to_string(::geteuid()) + "("),
          "非 root 时子进程 uid 应当就是当前 uid（没有降权）");
  } else {
    check(contains(r.privilege_drop, "nobody("), "root 时降权结果应为 nobody(65534)");
    check(contains(r.stdout_text, "uid=65534("), "root 启动时子进程 uid 必须是 nobody(65534)");
    check(!contains(r.stdout_text, "uid=0("), "root 启动时子进程绝不能仍是 uid=0");
  }
}

// FR-SANDBOX.8：docker 后端（本机镜像 alpine:latest，不 pull）。
// 三条断言：命令跑得通 / --network=none 真的断网（按 IP，排除 DNS 干扰）/ 根只读。
// 没有 docker（CI 容器里常见）时打印 SKIP，而不是假绿。
void test_docker_backend() {
  struct stat st {};
  if (::stat("/usr/bin/docker", &st) != 0) {
    std::printf("SKIP: 本机没有 /usr/bin/docker，docker 后端未验证\n");
    return;
  }
  sb::Limits limits;
  limits.backend = sb::Backend::Docker;
  limits.docker_relax_whitelist = true;  // 只为验证容器侧隔离；模型可达的调用不走这条
  limits.timeout_ms = 30000;

  const auto r = sb::run("cat", {"/etc/hostname"}, limits, nullptr);
  if (!r.ran) {
    std::printf("SKIP: docker 后端跑不起来（%s）\n", r.error.c_str());
    return;
  }
  check(r.exit_code == 0, "docker 后端应能执行 cat /etc/hostname");
  check(!r.stdout_text.empty(), "docker 后端应拿到容器内的 /etc/hostname");
  std::printf("[docker] cat /etc/hostname → exit=%d stdout=%s", r.exit_code,
              r.stdout_text.c_str());
  check(r.backend == "docker", "结果里必须标明实际后端是 docker");

  // --network=none：用 IP 而不是域名，避免把"没有 DNS"误判成"没有网络"
  const auto w = sb::run("wget", {"-q", "-O-", "http://1.1.1.1/"}, limits, nullptr);
  check(w.ran, "wget 应进入容器执行");
  check(w.exit_code != 0, "--network=none 下 wget 连外部地址必须失败");
  std::printf("[docker] wget http://1.1.1.1/ → exit=%d stderr=%s", w.exit_code,
              w.stderr_text.c_str());
  check(contains(w.stderr_text, "unreachable") || contains(w.stderr_text, "Network"),
        "失败原因应指向网络不可达（而不是参数/镜像问题）");

  // --read-only：根文件系统不可写
  const auto t = sb::run("touch", {"/tmp/cue-ac11-docker"}, limits, nullptr);
  check(t.ran && t.exit_code != 0, "--read-only 下 touch 必须失败");
  std::printf("[docker] touch /tmp/x → exit=%d stderr=%s", t.exit_code, t.stderr_text.c_str());
  check(contains(t.stderr_text, "Read-only") || contains(t.stderr_text, "read-only"),
        "失败原因应指向只读文件系统");

  // 白名单仍然生效：不放宽时 docker 后端也不接受 wget
  sb::Limits strict;
  strict.backend = sb::Backend::Docker;
  strict.timeout_ms = 30000;
  const auto s2 = sb::run("wget", {"-q", "-O-", "http://1.1.1.1/"}, strict, nullptr);
  check(!s2.ran && contains(s2.error, "白名单"),
        "docker 后端默认不改白名单语义（越权命令仍被拒）");
}


// FR-SANDBOX.2 的端到端验收：**通过 run() 执行一条命令**去写 runs/<id>/，
// 而不是只在测试里 fork+open。白名单里唯一能写文件的诊断命令是 `sort -o`：
//   sort -o runs/sandbox/run_shell_write.txt /etc/hostname   → 必须成功
//   sort -o src/sandbox/sandbox.cpp       /etc/hostname      → 必须 EACCES 且源码不变
// 这比 `echo x > file` 更贴近实际：run_shell 不经 shell，重定向语法根本不存在。
void test_write_via_command() {
  sb::Limits limits;
  const std::string ok_path = "runs/sandbox/run_shell_write.txt";
  ::unlink(ok_path.c_str());
  const auto ok = sb::run("sort", {"-o", ok_path, "/etc/hostname"}, limits, nullptr);
  check(ok.ran && ok.exit_code == 0, "sort -o 写 runs/sandbox 应成功（可写路径放行）");
  check(::access(ok_path.c_str(), F_OK) == 0, "run() 必须真的把文件写出来");
  check(read_text(ok_path) == read_text("/etc/hostname"), "写入内容应与 /etc/hostname 一致");
  ::unlink(ok_path.c_str());

  const std::string src = "src/sandbox/sandbox.cpp";
  const std::string before = read_text(src);
  const auto bad = sb::run("sort", {"-o", src, "/etc/hostname"}, limits, nullptr);
  check(bad.ran, "命令本身应被执行（拒绝来自 landlock，不是白名单）");
  check(bad.exit_code != 0, "写项目源码必须失败");
  check(contains(bad.stderr_text, "denied") || contains(bad.stderr_text, "Permission") ||
            contains(bad.stderr_text, "权限"),
        "失败原因应指向权限拒绝（landlock EACCES）");
  check(read_text(src) == before, "源码文件绝不能被改写");

  // 项目根本身不可写：这是「不允许把 /（项目根）设为可写」的运行期证据
  const std::string repo_marker = "sandbox_should_not_write_here.txt";
  ::unlink(repo_marker.c_str());
  const auto bad2 = sb::run("sort", {"-o", repo_marker, "/etc/hostname"}, limits, nullptr);
  check(bad2.exit_code != 0, "在项目根新建文件必须失败");
  check(::access(repo_marker.c_str(), F_OK) != 0, "项目根不能被沙箱命令写入");
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
  test_landlock_write_paths();
  test_dangerous_rw_path_refused();
  test_missing_rw_path_reported();
  test_privilege_drop();
  test_docker_backend();
  test_write_via_command();
  test_timeout();
  test_cancel();
  test_output_truncation();

  // root 跑过之后 runs/sandbox 的属主已经被交给 nobody（沙箱用户），别把它留给
  // 普通用户的后续运行。空目录才删得掉：本测试自己的产物都清过了。
  if (::geteuid() == 0) ::rmdir("runs/sandbox");

  if (failures != 0) {
    std::printf("test_sandbox: %d 个断言失败\n", failures);
    return 1;
  }
  std::printf("test_sandbox: 全部通过\n");
  return 0;
}
