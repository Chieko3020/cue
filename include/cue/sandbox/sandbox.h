// 沙箱层：白名单命令 + landlock（或 docker）+ rlimit + 降权，在没有 shell 的前提下执行
// 「诊断类」命令。
//
// 为什么需要它（AC-11）：agent 要能看 `df -h` / `ps` / `ss` 这类系统状态，但把 shell
// 交给模型等于把整台机器的写权限交出去。这里把四件事同时钉死：
//   1) 白名单按「命令名」精确匹配，参数不参与匹配（模型不能靠参数绕过）；
//   2) 不经过 shell（execve 直投），`;` `|` `$()` 根本没有解释器去处理；
//   3) 文件系统默认拒绝，只有显式放行的路径可见；FR-SANDBOX.2 要求**可写**的只有
//      `runs/<id>/` 与临时目录，项目根保持只读（`Limits::rw_paths` / `ro_paths`）；
//   4) 以 root 启动时子进程降权到无特权用户（FR-SANDBOX.4），非 root 时如实记录
//      「没有降权」的原因，不静默。
// 另有可选容器后端（FR-SANDBOX.8）：同样的白名单语义跑在
// `docker run --rm --network=none --read-only --cap-drop=ALL --memory=128m` 里。
//
// 威胁模型（FR-SANDBOX.7）：默认的 landlock 后端防**误操作**，不防恶意代码——
// 它由被限制的进程自己施加（子进程在 execve 前 restrict_self），一个愿意自己再 exec
// 其它二进制的进程当然也能绕过白名单；真正需要防恶意代码时用 docker 后端。
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace cue::sandbox {

// 白名单命令名（升序）。工具 schema 的 enum 直接引用它，模型能看到可选范围；
// 这里**不含**任何写文件、提权、解释器类命令（rm/sh/bash/sudo/python/...）。
const std::vector<std::string>& allowed_commands();

// 命令名是否在白名单内。只接受三种形式：
//   <裸名>、/usr/bin/<白名单名>、/bin/<白名单名>
// 其余（绝对路径带子目录、相对路径、`../` 之类）一律拒绝。
// 注意：是否在 PATH 里可执行由 run() 解析，本函数只做名字判定。
bool is_allowed_command(const std::string& cmd);

// 隔离后端。默认 landlock：零依赖、~1ms，够防误操作；docker 用于「放开白名单后的
// 强隔离」（技术与需求文档 §5.7 的定位），代价是每个命令一次容器启动（实测 ~1.5 s）。
enum class Backend { Landlock, Docker };

// 后端名字（审计/结果用）："landlock" / "docker"。
const char* backend_name(Backend backend);

struct Limits {
  int cpu_seconds = 5;               // RLIMIT_CPU：CPU 时间上限（防死循环）
  uint64_t address_space_mb = 256;   // RLIMIT_AS：虚拟内存上限（防 OOM 拖垮 2C/2G 机器）
  // RLIMIT_FSIZE：单个普通文件的大小上限。
  // 0 会禁止写任何普通文件——那样「landlock 放行 runs/<id>/」就是一句空话（写会 EFBIG），
  // 所以默认 8 MiB（报告/探针转储够用），而不是 0。设备文件与管道不受 RLIMIT_FSIZE 限制。
  uint64_t file_size_mb = 8;
  int max_procs = 16;                // RLIMIT_NPROC：防止命令自己 fork 炸弹
  int timeout_ms = 5000;             // 墙钟超时（父进程计时，超时 kill 整个进程组）
  std::size_t max_output_bytes = 8 * 1024;  // stdout/stderr 各截断到这个字节数

  // ---- 文件系统规则（只对 landlock 后端有效；docker 后端由容器自己的只读根负责）----
  // 只读放行清单。空 = 只用内置的 /usr /lib /lib64 /etc /proc /sys 加当前工作目录；
  // 非空 = 在它们之外**追加**这些只读路径（项目数据目录就是这么给的）。
  // 内置清单与工作目录永远保留：拿掉 /usr 会让命令连动态库都加载不了。
  std::vector<std::string> ro_paths;
  // 可写放行清单。默认 `/tmp` 与 `runs/sandbox`（相对当前工作目录）。
  // 默认值就是需求文档的「可写临时目录 + runs/<id>」；没有 run id 时落在 runs/sandbox。
  // 调用方有 run id 时必须给具体目录（例如 `runs/<run_id>`），否则所有 run 共用一个小目录。
  // 建立规则前会先 mkdir -p；建不出来的路径不会让 run() 失败，但会如实出现在
  // Result::paths_not_applied 里（「以为能写」比「明确不能写」难查得多）。
  // 命中危险清单（`/`、`/etc`、`/usr`、`~/.ssh` …）的可写路径是**硬错误**：
  // 整个 run 在 fork 之前被拒绝，不会被静默忽略。
  std::vector<std::string> rw_paths = {"/tmp", "runs/sandbox"};

  // ---- 后端 ----
  Backend backend = Backend::Landlock;
  // docker 后端使用的镜像。必须本地已有（本机：`sudo docker images` → alpine:latest）。
  // 默认 alpine：自带 busybox 的 cat/ls/wget 等，白名单命令在容器里同样能找到。
  std::string docker_image = "alpine:latest";
  // docker 后端是否给 docker CLI 加 `-n`（非交互）前缀：以 root 启动时直接用 docker，
  // 否则走 `sudo -n`。`-n` 是关键——需要密码时 sudo 立刻失败，不会把父进程挂住。
  bool docker_use_sudo = true;
  // 仅用于验证容器侧隔离（网络/只读根）：放开命令白名单。
  // 默认 false——白名单是模型可达的门，docker 是在它之上的第二层，不应该因为换了后端就消失。
  bool docker_relax_whitelist = false;

  // ---- 降权（FR-SANDBOX.4）----
  // 以 root 启动时降到的用户。空 = nobody。非 root 启动时这个字段被忽略，
  // 结果里记 not_root（不是静默跳过）。
  std::string sandbox_user = "nobody";
};

struct Result {
  bool ran = false;      // 真的执行了（未执行 = 被白名单/参数/策略拒绝或启动失败）
  bool timed_out = false;
  bool cancelled = false;
  // landlock 不可用（ENOSYS/EOPNOTSUPP 或规则应用失败）时降级为「仅 rlimit」。
  // 降级绝不能静默：degraded=true + degraded_reason 会一路传到工具返回与审计行。
  // 注意它只描述**文件系统隔离**；「没有降权」是另一件事，见 privilege_drop。
  bool degraded = false;
  std::string degraded_reason;
  bool stdout_truncated = false;
  bool stderr_truncated = false;
  int exit_code = -1;    // 正常退出时的 WEXITSTATUS；被信号杀死时保持 -1
  int signal = 0;        // WTERMSIG；0 = 未被信号杀死
  std::string stdout_text;  // 已按 max_output_bytes 截断
  std::string stderr_text;  // 已按 max_output_bytes 截断
  std::string error;        // ran=false 的原因
  int64_t duration_ms = 0;
  uint64_t stdout_bytes = 0;  // 实际读到的总字节数（截断前，审计用）
  uint64_t stderr_bytes = 0;

  // 实际生效的后端（"landlock" / "docker"；未执行时为空）
  std::string backend;
  // 实际生效的可写路径（规范化后的绝对路径；审计要能回答"它到底被允许写哪里"）
  std::vector<std::string> rw_paths_applied;
  // 请求了但没生效的路径，每条是 "rw <path>: <原因>"。不存在/建不出/被策略拒绝都在这里，
  // 不会被静默跳过。
  std::vector<std::string> paths_not_applied;
  // 降权结果：root 下是 "nobody(65534)"，非 root 是 "not_root(uid=1000)"，
  // 用户不存在是 "user_not_found:xxx"（此时仍以 root 跑，必须是显式记录而不是静默）。
  std::string privilege_drop;
};

// 一条 landlock 规则。fd 由父进程 open（O_PATH|O_CLOEXEC），子进程只读它做 add_rule；
// 之所以"先建后 fork"，是因为 fork 之后的子进程不允许分配内存（父进程可能是多线程的）。
struct LandlockRule {
  int fd = -1;
  uint64_t access = 0;
};

struct LandlockPlan {
  bool usable = false;               // false = 内核不支持 landlock（降级，不只是跳过）
  std::string unavailable_reason;
  long abi = 0;                      // 内核 landlock ABI 版本（按它裁剪 handled_access_fs）
  std::vector<LandlockRule> rules;
  std::vector<std::string> rw_applied;      // 真正加了可写规则的路径（规范化后的绝对路径）
  std::vector<std::string> not_applied;     // "rw <path>: <原因>"
  std::vector<std::string> rejected;        // 危险可写路径 → run() 直接拒绝执行
};

// 父进程侧：解析路径、mkdir -p 可写目录、open 规则 fd，并查询内核 ABI。
// 会分配内存、会建目录，**必须在 fork 之前**调用（fork 之后只能走 apply_landlock_plan）。
LandlockPlan build_landlock_plan(const Limits& limits);
// 关闭 plan 里所有 fd（父进程在 fork 之后调用）。
void close_landlock_plan(LandlockPlan& plan);

// 子进程侧：只做系统调用（prctl + landlock_create_ruleset/add_rule/restrict_self），
// 不分配内存、不 strerror。返回 false 时 degraded=true 且 err 说明原因，调用方应继续执行
// （只是只剩 rlimit 这一层）。
bool apply_landlock_plan(const LandlockPlan& plan, char* err, std::size_t err_len, bool& degraded);

// 在 landlock（或 docker）+ rlimit + 降权下跑一条命令。
//   子进程顺序：setpgid → rlimit → landlock → setgid/setuid → execve（绝对路径，不走 PATH）。
//   父进程：poll 读 stdout/stderr（非阻塞），每 20ms 检查 cancel 与超时；
//           超时/取消 → kill(-pid, SIGKILL) 整个进程组，然后收尸。
// cancel 非空且置位时必须尽快返回（cancelled=true）——AC-8 的「SIGTERM 后 5 秒退出」
// 依赖这一点：一个 30 秒的命令不能把进程退出顶穿。
Result run(const std::string& cmd, const std::vector<std::string>& args, const Limits& limits,
           const std::atomic<bool>* cancel = nullptr);

}  // namespace cue::sandbox
