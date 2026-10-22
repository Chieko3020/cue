#include "cue/sandbox/sandbox.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <linux/landlock.h>

// 老内核头文件里可能没有这几个号（landlock 5.13 引入，close_range 5.9 引入）。
// 显式给出兜底值，编译期不依赖 <asm/unistd.h> 的具体版本。
#ifndef SYS_landlock_create_ruleset
#define SYS_landlock_create_ruleset 444
#endif
#ifndef SYS_landlock_add_rule
#define SYS_landlock_add_rule 445
#endif
#ifndef SYS_landlock_restrict_self
#define SYS_landlock_restrict_self 446
#endif

namespace cue::sandbox {
namespace {

using Clock = std::chrono::steady_clock;

int64_t now_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch())
      .count();
}

// 白名单：只放「诊断类只读」命令。按命令名精确匹配（不看参数），
// 因此模型无法通过参数构造注入——真正的隔离还有 landlock 与「不经 shell」两道。
// 排序用于 binary_search；新增条目必须保持字典序（单测会检查）。
constexpr std::string_view kAllowed[] = {
    "cat",  "cut",   "date", "df",   "du",    "echo", "env",  "file", "free",
    "grep", "head",  "id",   "ls",   "pgrep", "ps",   "sort", "ss",   "stat",
    "tail", "tr",    "uname", "uniq", "uptime", "wc",  "which"};

bool leaf_allowed(std::string_view leaf) {
  if (leaf.empty()) return false;
  if (leaf.find('/') != std::string_view::npos) return false;
  return std::binary_search(std::begin(kAllowed), std::end(kAllowed), leaf);
}

// 把白名单名解析成绝对路径。只查 /usr/bin 与 /bin（Ubuntu 上 /bin 是 /usr/bin 的
// 符号链接），**不读 PATH**：PATH 可被调用方或工作目录污染，一旦 execvp 走了 PATH，
// 白名单就变成「名字对就行」，同名的恶意二进制会顶替。
bool resolve_executable(const std::string& cmd, std::string& out, std::string& error) {
  const std::string leaf = cmd.substr(cmd.find_last_of('/') + 1);
  const std::string candidates[] = {"/usr/bin/" + leaf, "/bin/" + leaf};
  for (const auto& path : candidates) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) continue;
    if (!S_ISREG(st.st_mode)) continue;
    if (::access(path.c_str(), X_OK) != 0) continue;
    out = path;
    return true;
  }
  error = "白名单命令找不到可执行文件: " + cmd;
  return false;
}

// ---------------------------------------------------------------- landlock

// 只读：允许读文件/列目录；可执行：再允许 EXECUTE（只对 /usr /lib /lib64 授予）。
constexpr uint64_t kReadFs =
    LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
constexpr uint64_t kReadExecFs = kReadFs | LANDLOCK_ACCESS_FS_EXECUTE;

struct DirRule {
  const char* path;
  uint64_t access;
};

// 只读放行清单：/usr /lib /lib64 是二进制与动态库（要 EXECUTE），
// /etc 是时区、主机名、ld.so.cache，/proc /sys 是进程与内核状态。
// /tmp 与工作目录**不在**这里：它们是写操作的落脚点，默认拒绝。
// 工作目录单独以只读方式放行（见 apply_landlock 内注释）。
constexpr DirRule kDirs[] = {
    {"/usr", kReadExecFs}, {"/lib", kReadExecFs}, {"/lib64", kReadExecFs},
    {"/etc", kReadFs},     {"/proc", kReadFs},    {"/sys", kReadFs},
};

int add_path_beneath(int ruleset_fd, int parent_fd, uint64_t access, int* out_errno) {
  landlock_path_beneath_attr attr{};
  attr.allowed_access = access;
  attr.parent_fd = parent_fd;
  if (::syscall(SYS_landlock_add_rule, ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &attr, 0) != 0) {
    *out_errno = errno;
    return -1;
  }
  return 0;
}

// fork 之后的子进程里不能调用任何会分配内存的函数（另一个线程可能正握着 malloc 锁），
// 因此错误信息用固定缓冲 + 手写拼接，错误码只记 errno 数字（strerror 不是
// async-signal-safe 的）。
void set_err(char* buf, std::size_t len, const char* msg, int sys_errno) {
  if (len == 0) return;
  std::size_t off = 0;
  for (const char* p = msg; *p != '\0' && off + 1 < len; ++p) buf[off++] = *p;
  if (sys_errno != 0) {
    const char kPrefix[] = " errno=";
    for (const char* p = kPrefix; *p != '\0' && off + 1 < len; ++p) buf[off++] = *p;
    char digits[12];
    int n = 0;
    int v = sys_errno < 0 ? -sys_errno : sys_errno;
    do {
      digits[n++] = static_cast<char>('0' + v % 10);
      v /= 10;
    } while (v != 0 && n < 12);
    while (n > 0 && off + 1 < len) buf[off++] = digits[--n];
  }
  buf[off] = '\0';
}

// 真正的规则应用（在子进程里调用）。返回 true=已限制；false=降级，原因写进 err。
bool apply_landlock_raw(char* err, std::size_t err_len, bool* degraded) {
  *degraded = false;
  // 内核能力查询：create_ruleset(NULL, 0, VERSION) 返回 ABI 版本
  const long abi =
      ::syscall(SYS_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
  if (abi < 1) {
    const int e = errno;
    *degraded = true;
    set_err(err, err_len, "landlock 不可用（create_ruleset VERSION）", e);
    return false;
  }

  // 按 ABI 取舍：REFER 从 2 起、TRUNCATE 从 3 起。handled 里多写一个该内核不认识的
  // 位会让 create_ruleset 直接 EINVAL，所以必须按版本裁剪。
  uint64_t handled = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE |
                     LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR |
                     LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
                     LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR |
                     LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK |
                     LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
                     LANDLOCK_ACCESS_FS_MAKE_SYM;
  if (abi >= 2) handled |= LANDLOCK_ACCESS_FS_REFER;
  if (abi >= 3) handled |= LANDLOCK_ACCESS_FS_TRUNCATE;

  landlock_ruleset_attr attr{};
  attr.handled_access_fs = handled;
  const int ruleset = static_cast<int>(
      ::syscall(SYS_landlock_create_ruleset, &attr, sizeof(attr), 0));
  if (ruleset < 0) {
    const int e = errno;
    *degraded = true;
    set_err(err, err_len, "landlock_create_ruleset 失败", e);
    return false;
  }

  auto fail = [&](const char* msg, int e) {
    ::close(ruleset);
    *degraded = true;
    set_err(err, err_len, msg, e);
    return false;
  };

  for (const auto& rule : kDirs) {
    const int fd = ::open(rule.path, O_PATH | O_CLOEXEC);
    if (fd < 0) {
      // 某些发行版没有 /lib64；缺失的放行目录不是错误，跳过即可。
      if (errno == ENOENT) continue;
      return fail("landlock 放行目录打开失败", errno);
    }
    int e = 0;
    const int rc = add_path_beneath(ruleset, fd, rule.access, &e);
    ::close(fd);
    if (rc != 0) return fail("landlock 放行目录规则失败", e);
  }

  // 工作目录只读放行：诊断命令（ls/du/df/stat）常常直接看工作树，
  // 不给读会让裸 `ls` 直接 EACCES。写权限**没有**授予——这里只加 READ_FILE|READ_DIR。
  {
    const int cwd = ::open(".", O_PATH | O_CLOEXEC);
    if (cwd < 0) return fail("landlock 工作目录打开失败", errno);
    int e = 0;
    const int rc = add_path_beneath(ruleset, cwd, kReadFs, &e);
    ::close(cwd);
    if (rc != 0) return fail("landlock 工作目录规则失败", e);
  }

  // /dev/null 必须例外地可写：子进程的 stdout/stderr 是本层的管道，但命令（及 glibc）
  // 常见地会打开 /dev/null 丢弃输出；一旦默认拒绝一切写，open("/dev/null", O_WRONLY)
  // 会 EACCES，用户看到的是"命令坏了"而不是"沙箱在限制"。
  // 只授这一个字符设备，且只授 READ_FILE|WRITE_FILE（文件规则不能带 READ_DIR）。
  {
    const int devnull = ::open("/dev/null", O_PATH | O_CLOEXEC);
    if (devnull < 0) return fail("landlock /dev/null 打开失败", errno);
    int e = 0;
    // 文件规则不能带 READ_DIR（内核对非目录返回 EINVAL）：只授 READ_FILE|WRITE_FILE
    const int rc = add_path_beneath(
        ruleset, devnull, LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE, &e);
    ::close(devnull);
    if (rc != 0) return fail("landlock /dev/null 规则失败", e);
  }

  // 无 root 运行 landlock 的前提：no_new_privs。它同时堵住 setuid 提权，
  // 是这一层「不需要 sudo」的代价交换。
  if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
    return fail("prctl(PR_SET_NO_NEW_PRIVS) 失败", errno);
  }
  if (::syscall(SYS_landlock_restrict_self, ruleset, 0) != 0) {
    return fail("landlock_restrict_self 失败", errno);
  }
  ::close(ruleset);
  return true;
}

bool set_limit(int resource, rlim_t soft, rlim_t hard) {
  struct rlimit rl {};
  rl.rlim_cur = soft;
  rl.rlim_max = hard;
  return ::setrlimit(resource, &rl) == 0;
}

// 子进程状态通道：父进程要能区分「landlock 已生效」「landlock 降级」「初始化失败」，
// 否则降级就是静默的。
void child_report(int fd, char kind, const char* msg, int sys_errno) {
  char buf[224];
  std::size_t n = 0;
  auto put = [&](char c) {
    if (n + 1 < sizeof(buf)) buf[n++] = c;
  };
  put(kind);
  if (msg != nullptr && *msg != '\0') {
    put(':');
    for (const char* p = msg; *p != '\0' && n + 1 < sizeof(buf); ++p) put(*p);
  }
  if (sys_errno != 0) {
    const char kPrefix[] = " errno=";
    for (const char* p = kPrefix; *p != '\0' && n + 1 < sizeof(buf); ++p) put(*p);
    char digits[12];
    int d = 0;
    int v = sys_errno < 0 ? -sys_errno : sys_errno;
    do {
      digits[d++] = static_cast<char>('0' + v % 10);
      v /= 10;
    } while (v != 0 && d < 12);
    while (d > 0) put(digits[--d]);
  }
  put('\n');
  ssize_t ignored = ::write(fd, buf, n);
  (void)ignored;
}

// 读到的字节数可能远大于需要保留的（审计要真实字节数），这里统计总量、只留前 max_bytes。
struct Capture {
  int fd = -1;
  std::string data;
  uint64_t total = 0;
  bool eof = false;
  bool truncated = false;
};

void drain(Capture& c, std::size_t max_bytes) {
  char buf[4096];
  for (;;) {
    if (c.fd < 0) return;
    const ssize_t n = ::read(c.fd, buf, sizeof(buf));
    if (n > 0) {
      c.total += static_cast<uint64_t>(n);
      if (c.data.size() < max_bytes) {
        const std::size_t room = max_bytes - c.data.size();
        c.data.append(buf, std::min(static_cast<std::size_t>(n), room));
      }
      continue;
    }
    if (n == 0) {
      ::close(c.fd);
      c.fd = -1;
      c.eof = true;
      return;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return;
    // 读出错（例如子进程被 kill 后管道裂开）：按 EOF 处理，不把父进程卡住。
    ::close(c.fd);
    c.fd = -1;
    c.eof = true;
    return;
  }
}

void close_capture(Capture& c) {
  if (c.fd >= 0) {
    ::close(c.fd);
    c.fd = -1;
  }
}

constexpr char kTruncMarker[] = "\n[truncated]";

}  // namespace

const std::vector<std::string>& allowed_commands() {
  static const std::vector<std::string> kNames = [] {
    std::vector<std::string> v;
    v.reserve(std::size(kAllowed));
    for (const auto& s : kAllowed) v.emplace_back(s);
    return v;
  }();
  return kNames;
}

bool is_allowed_command(const std::string& cmd) {
  if (cmd.empty()) return false;
  if (cmd.find('/') == std::string::npos) return leaf_allowed(cmd);
  // 绝对路径只接受两种形式：/usr/bin/<名>、/bin/<名>。
  // `../../bin/ls`、`/bin/../../bin/ls` 之类的穿越在叶子名里带 '/'，会在这里被拒。
  constexpr std::string_view kUsrBin = "/usr/bin/";
  constexpr std::string_view kBin = "/bin/";
  std::string_view leaf;
  if (cmd.compare(0, kUsrBin.size(), kUsrBin) == 0) {
    leaf = std::string_view(cmd).substr(kUsrBin.size());
  } else if (cmd.compare(0, kBin.size(), kBin) == 0) {
    leaf = std::string_view(cmd).substr(kBin.size());
  } else {
    return false;
  }
  return leaf_allowed(leaf);
}

bool apply_landlock(bool& degraded, std::string& error) {
  char err[224] = {0};
  bool deg = false;
  const bool ok = apply_landlock_raw(err, sizeof(err), &deg);
  degraded = deg;
  error = ok ? "" : std::string(err);
  return ok;
}

Result run(const std::string& cmd, const std::vector<std::string>& args, const Limits& limits,
           const std::atomic<bool>* cancel) {
  Result r;
  const int64_t start_us = now_us();

  // 取消先于一切：已经被取消的 run 不再 fork（也就不会留下需要收尸的进程组）。
  if (cancel != nullptr && cancel->load()) {
    r.cancelled = true;
    r.error = "取消标志已置位，未执行";
    return r;
  }
  if (!is_allowed_command(cmd)) {
    r.error = "命令不在白名单: " + cmd;
    return r;
  }
  std::string exe;
  if (!resolve_executable(cmd, exe, r.error)) return r;

  // argv 在 fork 前构造好：子进程里不能做内存分配。
  std::vector<char*> argv;
  argv.reserve(args.size() + 2);
  argv.push_back(const_cast<char*>(exe.c_str()));
  for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);

  // 环境变量同样在 fork 前构造：沙箱子进程拿到的是**一份白名单环境**，不继承 harness 的。
  // 两个理由，缺一都不成立：
  //  ① 安全：harness 的环境里有 CUE_API_KEY 之类的凭据，而命令白名单里就有 `env`。
  //     继承等于把密钥直接交给模型（sandbox 要防的"误泄露"正是这一条）。
  //  ② 正确性：本机 /etc/ld.so.preload 预加载了 libonion，sanitizer 构建的进程一旦把
  //     LD_PRELOAD=libasan 继承给子进程，子进程会在 main 之前因 "ASan runtime does not
  //     come first" 直接 abort，或者撞上 RLIMIT_AS=256MB 分不出 shadow 而崩。
  // 用 execve 而不是 fork 后 unsetenv：unsetenv 在 glibc 里可能 malloc，而 fork 之后
  // 子进程只允许做系统调用（父进程是多线程的，malloc 有死锁风险）。
  // PATH 给沙箱自己用（部分是绝对路径调用，但 `which` 之类要查 PATH）；locale 固定，
  // 免得子进程的排序/大小写行为随宿主环境漂移。
  std::vector<std::string> env_storage = {
      "PATH=/usr/local/bin:/usr/bin:/bin",
      "LANG=C.UTF-8",
      "LC_ALL=C.UTF-8",
  };
  std::vector<char*> envp;
  envp.reserve(env_storage.size() + 1);
  for (auto& s : env_storage) envp.push_back(const_cast<char*>(s.c_str()));
  envp.push_back(nullptr);

  int out_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};
  int st_pipe[2] = {-1, -1};
  if (::pipe2(out_pipe, O_CLOEXEC) != 0 || ::pipe2(err_pipe, O_CLOEXEC) != 0 ||
      ::pipe2(st_pipe, O_CLOEXEC) != 0) {
    r.error = std::string("pipe2 失败 errno=") + std::to_string(errno);
    for (int* p : {out_pipe, err_pipe, st_pipe}) {
      if (p[0] >= 0) ::close(p[0]);
      if (p[1] >= 0) ::close(p[1]);
    }
    return r;
  }
  const int devnull = ::open("/dev/null", O_RDONLY | O_CLOEXEC);

  const pid_t pid = ::fork();
  if (pid < 0) {
    r.error = std::string("fork 失败 errno=") + std::to_string(errno);
    for (int* p : {out_pipe, err_pipe, st_pipe}) {
      ::close(p[0]);
      ::close(p[1]);
    }
    if (devnull >= 0) ::close(devnull);
    return r;
  }

  if (pid == 0) {
    // ---- 子进程：只做系统调用，不分配内存 ----
    // 自己成组：父进程超时/取消时 kill(-pid) 能连它的子进程一起收掉。
    ::setpgid(0, 0);
    if (devnull >= 0) ::dup2(devnull, STDIN_FILENO);  // 不继承交互式 stdin，防止 cat 挂住
    ::dup2(out_pipe[1], STDOUT_FILENO);
    ::dup2(err_pipe[1], STDERR_FILENO);

    // rlimit 在 landlock 之前：限制是「资源」边界，landlock 是「可见性」边界，
    // 两者顺序不影响语义，但先设 limit 可以让 landlock 自身的内存消耗也受约束。
    bool ok = set_limit(RLIMIT_CPU, limits.cpu_seconds, limits.cpu_seconds) &&
              set_limit(RLIMIT_AS, limits.address_space_mb * 1024ULL * 1024ULL,
                        limits.address_space_mb * 1024ULL * 1024ULL) &&
              set_limit(RLIMIT_FSIZE, limits.file_size_mb * 1024ULL * 1024ULL,
                        limits.file_size_mb * 1024ULL * 1024ULL) &&
              set_limit(RLIMIT_NPROC, limits.max_procs, limits.max_procs) &&
              set_limit(RLIMIT_NOFILE, 64, 64) && set_limit(RLIMIT_CORE, 0, 0);
    if (!ok) {
      child_report(st_pipe[1], 'X', "setrlimit", errno);
      ::_exit(126);
    }

    char lerr[224] = {0};
    bool degraded = false;
    if (apply_landlock_raw(lerr, sizeof(lerr), &degraded)) {
      child_report(st_pipe[1], 'L', nullptr, 0);
    } else {
      // 降级：只保留 rlimit，但必须把原因报给父进程（不能静默）。
      // 注意白名单与「不经 shell」不受影响：那两道与 landlock 是否可用无关。
      child_report(st_pipe[1], 'D', lerr, 0);
    }

    ::execve(exe.c_str(), argv.data(), envp.data());
    child_report(st_pipe[1], 'X', "execve", errno);
    ::_exit(127);
  }

  // ---- 父进程 ----
  // 与子进程竞争 setpgid：父进程也设一次，保证在子进程还没跑到 setpgid 时
  // 取消/超时也能按组 kill。子进程已 exec 时这里会 EACCES，忽略即可。
  if (::setpgid(pid, pid) != 0) {
    // 忽略：真正的兜底是子进程自己的 setpgid(0,0)
  }
  ::close(out_pipe[1]);
  ::close(err_pipe[1]);
  ::close(st_pipe[1]);
  if (devnull >= 0) ::close(devnull);

  // 读子进程状态行。要读到 EOF 而不是读到第一个换行就停：exec 失败时子进程会在
  // "L\n"/"D:...\n" 之后再写一行 "X:..."，只看首行会把「没跑起来」记成 ran=true。
  // 正常路径里 exec 会因 CLOEXEC 关掉写端，EOF 立刻到来，不会多等。
  std::string status_line;
  {
    const int64_t deadline = now_us() + 2'000'000;
    char buf[256];
    for (;;) {
      if (now_us() >= deadline) break;
      struct pollfd pfd {
        st_pipe[0], POLLIN, 0
      };
      const int rc = ::poll(&pfd, 1, 100);
      if (rc < 0 && errno != EINTR) break;
      if (rc <= 0) continue;
      const ssize_t n = ::read(st_pipe[0], buf, sizeof(buf));
      if (n > 0) {
        status_line.append(buf, static_cast<std::size_t>(n));
        continue;
      }
      break;  // EOF：exec 成功（CLOEXEC 关掉写端）或子进程已退出
    }
  }
  ::close(st_pipe[0]);

  if (!status_line.empty()) {
    const char kind = status_line[0];
    const std::string rest = status_line.size() > 1 ? status_line.substr(1) : std::string();
    auto strip = [](const std::string& s) {
      std::string v = s;
      while (!v.empty() && (v.front() == ':' || v.front() == ' ')) v.erase(v.begin());
      while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
      return v;
    };
    if (kind == 'L') {
      r.ran = true;
    } else if (kind == 'D') {
      r.ran = true;
      r.degraded = true;
      r.degraded_reason = strip(rest);
    } else {  // 'X'
      r.error = "沙箱初始化失败: " + strip(rest);
    }
    // exec 失败会在沙箱状态之后再追加一行：这时沙箱本身初始化成功过，
    // 但命令并没有执行，必须是 ran=false + failed，不能拿 exit=127 冒充执行结果。
    const std::size_t x = status_line.find("\nX:");
    if (x != std::string::npos) {
      r.ran = false;
      r.error = "沙箱初始化失败: " + strip(status_line.substr(x + 2));
    }
  }

  // 非阻塞读：poll 说可读就一次性读空，避免阻塞在「写端还开着但没数据」的读上。
  for (int fd : {out_pipe[0], err_pipe[0]}) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }

  Capture out;
  out.fd = out_pipe[0];
  Capture err;
  err.fd = err_pipe[0];

  const int timeout_ms = std::max(1, limits.timeout_ms);
  const int64_t deadline_us = start_us + static_cast<int64_t>(timeout_ms) * 1000;
  bool killed = false;

  for (;;) {
    if (cancel != nullptr && cancel->load()) {
      r.cancelled = true;
      ::kill(-pid, SIGKILL);
      killed = true;
      break;
    }
    const int64_t now = now_us();
    if (now >= deadline_us) {
      r.timed_out = true;
      ::kill(-pid, SIGKILL);
      killed = true;
      break;
    }
    int wait_ms = 20;
    const int64_t remain_ms = (deadline_us - now) / 1000;
    if (remain_ms < wait_ms) wait_ms = static_cast<int>(std::max<int64_t>(1, remain_ms));

    struct pollfd pfd[2] = {{out.fd, POLLIN, 0}, {err.fd, POLLIN, 0}};
    int rc = ::poll(pfd, 2, wait_ms);
    if (rc < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (rc == 0) {
      if (out.eof && err.eof) break;
      continue;
    }
    if ((pfd[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) drain(out, limits.max_output_bytes);
    if ((pfd[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) drain(err, limits.max_output_bytes);
    if (out.eof && err.eof) break;
  }

  // 被 kill 的进程组要主动把管道读干净（否则孙进程还握着写端时，我们只能等超时）。
  if (killed) {
    const int64_t drain_deadline = now_us() + 500'000;
    while (!(out.eof && err.eof) && now_us() < drain_deadline) {
      struct pollfd pfd[2] = {{out.fd, POLLIN, 0}, {err.fd, POLLIN, 0}};
      if (::poll(pfd, 2, 20) > 0) {
        if ((pfd[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) drain(out, limits.max_output_bytes);
        if ((pfd[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) drain(err, limits.max_output_bytes);
      }
    }
  }
  close_capture(out);
  close_capture(err);

  // 收尸：先 WNOHANG 轮询 2 秒；仍有残留（极端情况：孙进程脱离进程组）就再补一刀。
  int status = 0;
  pid_t waited = 0;
  for (int i = 0; i < 1000; ++i) {
    waited = ::waitpid(pid, &status, WNOHANG);
    if (waited == pid) break;
    if (waited < 0 && errno != EINTR) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  if (waited != pid) {
    ::kill(-pid, SIGKILL);
    ::kill(pid, SIGKILL);
    waited = ::waitpid(pid, &status, 0);
  }

  if (WIFEXITED(status)) {
    r.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    r.signal = WTERMSIG(status);
  }

  // 没有状态行 + 退出码 127/126 = 子进程在 exec/初始化时死了（正常失败路径都会写状态行，
  // 走到这里说明连 write 都没发生）。其余无状态行的情况也要给原因，不能留空的 error。
  if (!r.ran && r.error.empty()) {
    if (r.exit_code == 127) {
      r.error = "execve 失败（子进程退出 127：可执行文件可能在 fork 后被移走）";
    } else if (r.exit_code == 126) {
      r.error = "子进程沙箱初始化失败（退出 126）";
    } else if (r.timed_out || r.cancelled) {
      r.error = "在 exec 完成前被终止";
    } else {
      r.error = "子进程未报告沙箱状态（可能在 exec 前异常退出）";
    }
  }

  out.truncated = out.total > out.data.size();
  err.truncated = err.total > err.data.size();
  if (out.truncated) out.data += kTruncMarker;
  if (err.truncated) err.data += kTruncMarker;

  r.stdout_text = std::move(out.data);
  r.stderr_text = std::move(err.data);
  r.stdout_bytes = out.total;
  r.stderr_bytes = err.total;
  r.stdout_truncated = out.truncated;
  r.stderr_truncated = err.truncated;
  r.duration_ms = (now_us() - start_us) / 1000;
  return r;
}

}  // namespace cue::sandbox
