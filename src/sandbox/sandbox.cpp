#include "cue/sandbox/sandbox.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
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
constexpr uint64_t kReadFs = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
constexpr uint64_t kReadExecFs = kReadFs | LANDLOCK_ACCESS_FS_EXECUTE;
// 文件规则能带的权限：内核只接受这几种（其余在非目录上 add_rule 会 EINVAL）。
constexpr uint64_t kFileRuleFs = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_READ_FILE |
                                 LANDLOCK_ACCESS_FS_WRITE_FILE;

struct DirRule {
  const char* path;
  uint64_t access;
};

// 只读放行清单：/usr /lib /lib64 是二进制与动态库（要 EXECUTE），
// /etc 是时区、主机名、ld.so.cache，/proc /sys 是进程与内核状态。
// /tmp 与工作目录**不在**这里：它们是写操作的落脚点，由 Limits 的 rw_paths 显式决定。
// 工作目录单独以只读方式放行（见 build_landlock_plan 内注释）。
constexpr DirRule kDirs[] = {
    {"/usr", kReadExecFs}, {"/lib", kReadExecFs}, {"/lib64", kReadExecFs},
    {"/etc", kReadFs},     {"/proc", kReadFs},    {"/sys", kReadFs},
};

// 目录可写规则：读 + 写 + 增删 + 改名引用。
// 为什么不授 MAKE_CHAR/MAKE_BLOCK：普通用户本来就建不出设备节点，多授一项只是把
// 授权面扩大；mknod 在这里被默认拒绝是刻意的。
uint64_t write_dir_access(long abi) {
  uint64_t a = kReadFs | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_MAKE_REG |
               LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_FIFO |
               LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_SYM |
               LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE;
  // REFER 从 ABI 2、TRUNCATE 从 ABI 3 起存在；内核不认识的位置一位就 EINVAL。
  if (abi >= 2) a |= LANDLOCK_ACCESS_FS_REFER;
  if (abi >= 3) a |= LANDLOCK_ACCESS_FS_TRUNCATE;  // `>` 重定向的 O_TRUNC 需要它
  return a;
}

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

// 路径规范化：相对路径按当前工作目录展开；不存在的尾部原样保留（weakly_canonical
// 只解析存在的前缀）。可写路径必须按规范化后的结果做策略判断，否则 `runs/../..`
// 这种写法会绕过危险清单。
std::string canonical_path(const std::string& p) {
  std::error_code ec;
  const std::filesystem::path abs = std::filesystem::absolute(p, ec);
  if (ec) return std::filesystem::path(p).lexically_normal().string();
  const std::filesystem::path canon = std::filesystem::weakly_canonical(abs, ec);
  return ec ? abs.lexically_normal().string() : canon.string();
}

// 临时根：FR-SANDBOX.2 点名要求可写的那些目录。cwd 落在它们下面时不能因此拒绝。
bool is_temp_root(const std::string& canon) {
  if (canon == "/tmp" || canon == "/var/tmp") return true;
  const char* tmpdir = std::getenv("TMPDIR");
  if (tmpdir != nullptr && *tmpdir != '\0') {
    std::error_code ec;
    const auto t = std::filesystem::weakly_canonical(std::filesystem::path(tmpdir), ec);
    if (!ec && t.string() == canon) return true;
  }
  return false;
}

// p 是否等于 prefix 或位于 prefix 之下（按路径分量比较，避免 /tmpx 被当成 /tmp）。
bool path_within(const std::string& p, const std::string& prefix) {
  if (prefix.empty()) return false;
  if (p == prefix) return true;
  if (p.size() <= prefix.size()) return false;
  if (p.compare(0, prefix.size(), prefix) != 0) return false;
  const char next = p[prefix.size()];
  return prefix.back() == '/' || next == '/';
}

// 危险可写路径：返回非空的拒绝原因。这里是「策略」而不是「尽力而为」：
// 命中即整个 run 被拒（见 run()），因为静默忽略一个被请求的可写路径会让调用方
// 以为它可写——那是比"明确拒绝"更难查的故障。
std::string dangerous_write_reason(const std::string& canon) {
  // 注意 "/" 要单独判：把它当"前缀"会让任何绝对路径都命中（/foo 也 within "/"）。
  if (canon == "/") return "不允许把根目录设为可写";
  static const char* kForbidden[] = {"/etc", "/usr", "/lib", "/lib64", "/bin",
                                     "/sbin", "/boot", "/root", "/proc", "/sys", "/dev"};

  for (const char* f : kForbidden) {
    if (path_within(canon, f)) {
      return std::string("系统目录不允许可写（") + f + "）";
    }
  }
  const char* home = std::getenv("HOME");
  if (home != nullptr && *home != '\0') {
    const std::string h(home);
    if (canon == h) return "不允许把 HOME 整个目录设为可写";
    if (path_within(canon, h + "/.ssh")) return "不允许把 ~/.ssh 设为可写";
    if (path_within(canon, h + "/.gnupg")) return "不允许把 ~/.gnupg 设为可写";
  }
  return {};
}

}  // namespace

LandlockPlan build_landlock_plan(const Limits& limits) {
  LandlockPlan plan;
  // 内核能力查询：create_ruleset(NULL, 0, VERSION) 返回 ABI 版本
  const long abi =
      ::syscall(SYS_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
  if (abi < 1) {
    plan.usable = false;
    plan.unavailable_reason =
        std::string("landlock 不可用（create_ruleset VERSION errno=") + std::to_string(errno) + "）";
    return plan;
  }
  plan.usable = true;
  plan.abi = abi;

  auto open_rule = [&](const std::string& path, uint64_t access, int& out_errno) -> bool {
    const int fd = ::open(path.c_str(), O_PATH | O_CLOEXEC);
    if (fd < 0) {
      out_errno = errno;
      return false;
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
      out_errno = errno;
      ::close(fd);
      return false;
    }
    // 文件规则不能带 READ_DIR/MAKE_*/REMOVE_*/REFER（内核在非目录上返回 EINVAL）：
    // 普通文件只保留 READ_FILE|WRITE_FILE|EXECUTE（+ TRUNCATE）。
    uint64_t a = access;
    if (!S_ISDIR(st.st_mode)) {
      uint64_t file_ok = kFileRuleFs;
      if (abi >= 3) file_ok |= LANDLOCK_ACCESS_FS_TRUNCATE;
      a &= file_ok;
    }
    plan.rules.push_back(LandlockRule{fd, a});
    return true;
  };

  // 1) 内置只读目录。ENOENT 跳过（某些发行版没有 /lib64），其余错误让整层降级：
  //    半套规则比没有规则更危险（调用方会以为隔离生效了）。
  for (const auto& rule : kDirs) {
    int e = 0;
    if (open_rule(rule.path, rule.access, e)) continue;
    if (e == ENOENT) continue;
    plan.usable = false;
    plan.unavailable_reason =
        std::string("landlock 放行目录打开失败 ") + rule.path + " errno=" + std::to_string(e);
    close_landlock_plan(plan);
    return plan;
  }

  // 2) 工作目录只读放行：诊断命令（ls/du/df/stat）常常直接看工作树，
  //    不给读会让裸 `ls` 直接 EACCES。写权限**没有**授予——写只来自 rw_paths。
  {
    int e = 0;
    if (!open_rule(".", kReadFs, e)) {
      plan.usable = false;
      plan.unavailable_reason =
          std::string("landlock 工作目录打开失败 errno=") + std::to_string(e);
      close_landlock_plan(plan);
      return plan;
    }
  }

  // 3) 调用方追加的只读路径（项目数据目录）。不存在的路径不让整层降级：它不影响
  //    「默认拒绝」的安全语义，只是少一块可见区域，如实报告即可。
  for (const auto& raw : limits.ro_paths) {
    const std::string canon = canonical_path(raw);
    int e = 0;
    if (open_rule(canon, kReadFs, e)) continue;
    plan.not_applied.push_back("ro " + canon + ": 打开失败 errno=" + std::to_string(e));
  }

  // 4) 可写路径（FR-SANDBOX.2）：先策略判定，再 mkdir -p，最后建规则。
  const std::string cwd = canonical_path(".");
  for (const auto& raw : limits.rw_paths) {
    const std::string canon = canonical_path(raw);
    std::string why = dangerous_write_reason(canon);
    if (why.empty() && path_within(cwd, canon) && !is_temp_root(canon)) {
      // rw 路径是工作目录本身或它的祖先 → 等于把项目根（或更上层）整体设为可写。
      // 临时根是例外：cwd 常常就在 /tmp/<run> 下跑（测试、一次性 run），
      // 而 FR-SANDBOX.2 明确要求临时目录可写，把 /tmp 判成"项目根的祖先"会自相矛盾。
      why = "不允许把项目根或其祖先目录设为可写";
    }
    if (!why.empty()) {
      plan.rejected.push_back("rw " + canon + ": " + why);
      continue;
    }
    std::error_code ec;
    std::filesystem::create_directories(canon, ec);
    if (ec) {
      plan.not_applied.push_back("rw " + canon + ": mkdir -p 失败 " + ec.message());
      continue;
    }
    int e = 0;
    if (!open_rule(canon, write_dir_access(abi), e)) {
      plan.not_applied.push_back("rw " + canon + ": 打开失败 errno=" + std::to_string(e));
      continue;
    }
    plan.rw_applied.push_back(canon);
  }

  // 5) /dev/null 必须例外地可写：子进程的 stdout/stderr 是本层的管道，但命令（及 glibc）
  //    常见地会打开 /dev/null 丢弃输出；一旦默认拒绝一切写，open("/dev/null", O_WRONLY)
  //    会 EACCES，用户看到的是"命令坏了"而不是"沙箱在限制"。
  //    只授这一个字符设备，且只授 READ_FILE|WRITE_FILE（文件规则不能带 READ_DIR）。
  {
    const int fd = ::open("/dev/null", O_PATH | O_CLOEXEC);
    if (fd < 0) {
      plan.usable = false;
      plan.unavailable_reason =
          std::string("landlock /dev/null 打开失败 errno=") + std::to_string(errno);
      close_landlock_plan(plan);
      return plan;
    }
    plan.rules.push_back(LandlockRule{
        fd, LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE});
  }
  return plan;
}

void close_landlock_plan(LandlockPlan& plan) {
  for (auto& rule : plan.rules) {
    if (rule.fd >= 0) {
      ::close(rule.fd);
      rule.fd = -1;
    }
  }
}

bool apply_landlock_plan(const LandlockPlan& plan, char* err, std::size_t err_len,
                         bool& degraded) {
  degraded = false;
  if (!plan.usable) {
    degraded = true;
    set_err(err, err_len, plan.unavailable_reason.c_str(), 0);
    return false;
  }

  // handled 里多写一个该内核不认识的位会让 create_ruleset 直接 EINVAL，
  // 所以按 ABI 版本裁剪（REFER 2+、TRUNCATE 3+）。
  uint64_t handled = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE |
                     LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR |
                     LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
                     LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR |
                     LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK |
                     LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
                     LANDLOCK_ACCESS_FS_MAKE_SYM;
  if (plan.abi >= 2) handled |= LANDLOCK_ACCESS_FS_REFER;
  if (plan.abi >= 3) handled |= LANDLOCK_ACCESS_FS_TRUNCATE;

  landlock_ruleset_attr attr{};
  attr.handled_access_fs = handled;
  const int ruleset = static_cast<int>(
      ::syscall(SYS_landlock_create_ruleset, &attr, sizeof(attr), 0));
  if (ruleset < 0) {
    degraded = true;
    set_err(err, err_len, "landlock_create_ruleset 失败", errno);
    return false;
  }

  for (const auto& rule : plan.rules) {
    int e = 0;
    if (add_path_beneath(ruleset, rule.fd, rule.access, &e) != 0) {
      ::close(ruleset);
      degraded = true;
      set_err(err, err_len, "landlock 规则失败", e);
      return false;
    }
  }

  // 无 root 运行 landlock 的前提：no_new_privs。它同时堵住 setuid 提权，
  // 是这一层「不需要 sudo」的代价交换。
  if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
    ::close(ruleset);
    degraded = true;
    set_err(err, err_len, "prctl(PR_SET_NO_NEW_PRIVS) 失败", errno);
    return false;
  }
  if (::syscall(SYS_landlock_restrict_self, ruleset, 0) != 0) {
    ::close(ruleset);
    degraded = true;
    set_err(err, err_len, "landlock_restrict_self 失败", errno);
    return false;
  }
  ::close(ruleset);
  return true;
}

namespace {

// 降权计划（FR-SANDBOX.4）。uid/gid 必须在 fork 前查好：
// getpwnam 会走 NSS（可能 dlopen/malloc），fork 之后不能调。
struct PrivPlan {
  bool do_drop = false;
  uid_t uid = 0;
  gid_t gid = 0;
  std::string note;
};

PrivPlan build_priv_plan(const Limits& limits) {
  PrivPlan p;
  if (::geteuid() != 0) {
    // 非 root 时"不降权"不是降级，而是没有权限可降。但仍要显式记录：
    // 用 `sudo` 跑起来的那一次和普通用户跑的那一次，审计里必须能分辨。
    p.note = "not_root(uid=" + std::to_string(::geteuid()) + ")";
    return p;
  }
  const std::string user = limits.sandbox_user.empty() ? "nobody" : limits.sandbox_user;
  struct passwd* pw = ::getpwnam(user.c_str());
  if (pw == nullptr) {
    p.note = "user_not_found:" + user;
    return p;
  }
  if (pw->pw_uid == 0) {
    p.note = "refused_root_user:" + user;
    return p;
  }
  p.do_drop = true;
  p.uid = pw->pw_uid;
  p.gid = pw->pw_gid;
  p.note = user + "(" + std::to_string(pw->pw_uid) + ")";
  return p;
}

bool set_limit(int resource, rlim_t soft, rlim_t hard) {
  struct rlimit rl {};
  rl.rlim_cur = soft;
  rl.rlim_max = hard;
  return ::setrlimit(resource, &rl) == 0;
}

// 子进程状态通道：父进程要能区分「landlock 已生效」「landlock 降级」「容器后端」
// 「初始化失败」，否则降级就是静默的。
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

// docker 二进制位置：不读 PATH（同 resolve_executable 的理由）。
bool resolve_docker_binary(std::string& out, std::string& error) {
  const std::string candidates[] = {"/usr/bin/docker", "/usr/local/bin/docker", "/bin/docker"};
  for (const auto& path : candidates) {
    struct stat st {};
    if (::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
      out = path;
      return true;
    }
  }
  error = "docker 后端找不到 docker 可执行文件（/usr/bin/docker）";
  return false;
}

// 子进程要 exec 的东西：landlock 后端是白名单命令的绝对路径，docker 后端是
// `docker run ...`（必要时前面加 `sudo -n`）。在这里一次性拼好，子进程里不再分配。
struct SpawnPlan {
  std::vector<std::string> args;  // argv（args[0] 是绝对路径），不含 nullptr
  bool landlock = false;          // 子进程是否在 execve 前 restrict_self
  std::string backend = "landlock";
};

bool build_spawn_plan(const std::string& cmd, const std::vector<std::string>& args,
                      const Limits& limits, SpawnPlan& plan, Result& r) {
  if (limits.backend == Backend::Landlock) {
    if (!is_allowed_command(cmd)) {
      r.error = "命令不在白名单: " + cmd;
      return false;
    }
    std::string exe;
    if (!resolve_executable(cmd, exe, r.error)) return false;
    plan.landlock = true;
    plan.backend = "landlock";
    plan.args.reserve(args.size() + 1);
    plan.args.push_back(exe);
    for (const auto& a : args) plan.args.push_back(a);
    return true;
  }

  // ---- docker 后端（FR-SANDBOX.8）----
  const std::string leaf = cmd.substr(cmd.find_last_of('/') + 1);
  if (!limits.docker_relax_whitelist) {
    // 默认口径：白名单仍然生效。docker 是白名单之外的第二层隔离，而不是它的替代，
    // 否则「换个后端」就等于悄悄放开了模型可达的命令面。
    if (!is_allowed_command(cmd)) {
      r.error = "命令不在白名单: " + cmd;
      return false;
    }
  } else {
    // 验证容器侧隔离时才放开：名字仍然必须是裸名（不带路径、不以 '-' 开头），
    // 否则「放开白名单」会顺手变成「允许传任意路径/选项给 docker」。
    if (leaf.empty() || leaf.find('/') != std::string::npos || leaf[0] == '-' ||
        leaf == "." || leaf == "..") {
      r.error = "docker 后端的命令名不合法: " + cmd;
      return false;
    }
  }

  std::string docker;
  if (!resolve_docker_binary(docker, r.error)) return false;
  plan.backend = "docker";
  plan.landlock = false;
  if (limits.docker_use_sudo && ::geteuid() != 0) {
    // 当前用户不在 docker 组（本机实况），必须借 sudo。`-n` 是硬要求：需要密码时
    // 立刻失败并返回 "sudo: a password is required"，不会把父进程挂在密码提示上。
    struct stat st {};
    if (::stat("/usr/bin/sudo", &st) != 0) {
      r.error = "docker 后端需要 sudo 提权，但找不到 /usr/bin/sudo";
      return false;
    }
    plan.args.push_back("/usr/bin/sudo");
    plan.args.push_back("-n");
  }
  plan.args.push_back(docker);
  plan.args.insert(plan.args.end(), {"run", "--rm", "--network=none", "--read-only",
                                     "--cap-drop=ALL", "--memory=128m",
                                     limits.docker_image, leaf});
  for (const auto& a : args) plan.args.push_back(a);
  return true;
}

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

const char* backend_name(Backend backend) {
  switch (backend) {
    case Backend::Landlock:
      return "landlock";
    case Backend::Docker:
      return "docker";
  }
  return "unknown";
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
  if (cmd.empty()) {
    r.error = "命令名不能为空";
    return r;
  }

  // 所有会分配内存的准备工作都在 fork 之前完成：拼 argv、建 landlock 规则（open fd）、
  // 查 passwd（NSS 可能 dlopen）。子进程里只允许系统调用。
  SpawnPlan spawn;
  if (!build_spawn_plan(cmd, args, limits, spawn, r)) return r;
  r.backend = spawn.backend;

  // 降权只对 landlock 后端有意义：docker 后端里子进程是 docker CLI（降权它没有用，
  // 容器内的身份由镜像/--user 决定）。这里如实写清楚，不留空白让人以为是漏了。
  const PrivPlan priv = spawn.landlock ? build_priv_plan(limits) : PrivPlan{};
  r.privilege_drop =
      spawn.landlock ? priv.note : std::string("docker:容器内镜像默认用户（宿主侧 setuid 不适用）");

  LandlockPlan landlock;
  if (spawn.landlock) {
    landlock = build_landlock_plan(limits);
    // 以 root 启动时会降到 nobody，而 runs/<id> 通常是 root 刚建的（0755）：不把属主
    // 交给沙箱用户，"可写 runs/<id>/" 在 root 场景下就是空的。只 chown 我们自己能管的
    // 目录——已经是「其他人可写」的（/tmp 1777）一律不动，避免改系统目录属主。
    if (priv.do_drop) {
      for (const auto& p : landlock.rw_applied) {
        struct stat st {};
        if (::stat(p.c_str(), &st) != 0) continue;
        if ((st.st_mode & S_IWOTH) != 0) continue;
        if (::chown(p.c_str(), priv.uid, priv.gid) != 0) {
          landlock.not_applied.push_back("rw " + p +
                                         ": chown 到沙箱用户失败 errno=" + std::to_string(errno));
        }
      }
    }
    if (!landlock.rejected.empty()) {
      // 被请求的可写路径命中危险清单 → 整个 run 拒绝。静默跳过会让调用方以为
      // 它拿到了写权限（审计里也只会看到"命令跑了"）。
      std::string why;
      for (const auto& item : landlock.rejected) {
        if (!why.empty()) why += "；";
        why += item;
      }
      r.error = "可写路径被策略拒绝，未执行：" + why;
      r.paths_not_applied = landlock.rejected;
      close_landlock_plan(landlock);
      return r;
    }
    r.rw_paths_applied = landlock.rw_applied;
    r.paths_not_applied = landlock.not_applied;
  } else {
    // 容器根是只读的（--read-only），docker 后端不提供可写路径。写进结果，
    // 不让"rw_paths 被静默忽略"变成调用方以后才发现的事故。
    r.paths_not_applied.push_back(
        "docker: 容器根只读（--read-only），不提供可写路径；rw_paths 未生效");
  }

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

  std::vector<char*> argv;
  argv.reserve(spawn.args.size() + 1);
  for (auto& s : spawn.args) argv.push_back(const_cast<char*>(s.c_str()));
  argv.push_back(nullptr);

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
    close_landlock_plan(landlock);
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
    close_landlock_plan(landlock);
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
    //
    // docker 后端只留 RLIMIT_CORE：exec 的是宿主上的 docker CLI（Go 程序），它启动时
    // 要预留大段虚拟地址空间并开线程池——实测 RLIMIT_AS=256MB 会让它 panic
    // （fatal error: failed to reserve page summary memory），RLIMIT_NPROC=16 也会把
    // Go 的线程算成超限。容器里的资源边界由 `--memory=128m` 与父进程超时负责。
    bool ok = spawn.landlock
                  ? (set_limit(RLIMIT_CPU, limits.cpu_seconds, limits.cpu_seconds) &&
                     set_limit(RLIMIT_AS, limits.address_space_mb * 1024ULL * 1024ULL,
                               limits.address_space_mb * 1024ULL * 1024ULL) &&
                     set_limit(RLIMIT_FSIZE, limits.file_size_mb * 1024ULL * 1024ULL,
                               limits.file_size_mb * 1024ULL * 1024ULL) &&
                     set_limit(RLIMIT_NPROC, limits.max_procs, limits.max_procs) &&
                     set_limit(RLIMIT_NOFILE, 64, 64) && set_limit(RLIMIT_CORE, 0, 0))
                  : set_limit(RLIMIT_CORE, 0, 0);
    if (!ok) {
      child_report(st_pipe[1], 'X', "setrlimit", errno);
      ::_exit(126);
    }

    if (spawn.landlock) {
      char lerr[224] = {0};
      bool degraded = false;
      if (apply_landlock_plan(landlock, lerr, sizeof(lerr), degraded)) {
        child_report(st_pipe[1], 'L', nullptr, 0);
      } else {
        // 降级：只保留 rlimit，但必须把原因报给父进程（不能静默）。
        // 注意白名单与「不经 shell」不受影响：那两道与 landlock 是否可用无关。
        child_report(st_pipe[1], 'D', lerr, 0);
      }
    } else {
      // 容器后端：文件系统隔离由容器负责（--read-only / --cap-drop=ALL），这里只报
      // "已进入容器路径"，不是降级。
      child_report(st_pipe[1], 'B', nullptr, 0);
    }

    // 降权必须在 landlock **之后**：
    //  - landlock_restrict_self 本身不需要特权，root 也能调；
    //  - 一旦 setuid 到 nobody，进程就再没有能力去改规则集（也不需要改），
    //    所以「先钉死文件系统边界、再丢特权」是唯一不会留下窗口的顺序。
    // 反过来（先降权、再 landlock）会失败：非 root 也能 restrict_self，但此时
    // 打开 /usr 等目录做 O_PATH 可能已经被权限挡住，规则集就不完整。
    if (priv.do_drop) {
      if (::setgroups(0, nullptr) != 0 || ::setgid(priv.gid) != 0 || ::setuid(priv.uid) != 0) {
        child_report(st_pipe[1], 'X', "降权失败（setgroups/setgid/setuid）", errno);
        ::_exit(126);
      }
      // 自证：降权之后真实的 uid/euid 都必须不再是 0。只信 setuid 的返回值是不够的
      // （例如被 seccomp 静默忽略时返回 0 但没生效）。
      if (::getuid() == 0 || ::geteuid() == 0) {
        child_report(st_pipe[1], 'X', "降权后 uid 仍为 0", 0);
        ::_exit(126);
      }
    }

    ::execve(argv[0], argv.data(), envp.data());
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
  // 规则 fd 只在子进程里用得到，父进程拿到的是同一批 fd：fork 之后立刻关掉，
  // 不让它们挂在长期驻留的 harness 进程上。
  close_landlock_plan(landlock);

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
    if (kind == 'L' || kind == 'B') {
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
