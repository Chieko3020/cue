// 沙箱层：白名单命令 + landlock + rlimit，在没有 shell 的前提下执行「只读诊断」命令。
//
// 为什么需要它（AC-11）：agent 要能看 `df -h` / `ps` / `ss` 这类系统状态，但把 shell
// 交给模型等于把整台机器的写权限交出去。这里把三件事同时钉死：
//   1) 白名单按「命令名」精确匹配，参数不参与匹配（模型不能靠参数绕过）；
//   2) 不经过 shell（execv 直投），`;` `|` `$()` 根本没有解释器去处理；
//   3) landlock 把文件系统写成默认拒绝，只有显式放行的只读路径可见，
//      rlimit 再兜一层 CPU/内存/进程数/文件大小。
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

struct Limits {
  int cpu_seconds = 5;               // RLIMIT_CPU：CPU 时间上限（防死循环）
  uint64_t address_space_mb = 256;   // RLIMIT_AS：虚拟内存上限（防 OOM 拖垮 2C/2G 机器）
  uint64_t file_size_mb = 0;         // 0 = RLIMIT_FSIZE=0，禁止写任何普通文件
  int max_procs = 16;                // RLIMIT_NPROC：防止命令自己 fork 炸弹
  int timeout_ms = 5000;             // 墙钟超时（父进程计时，超时 kill 整个进程组）
  std::size_t max_output_bytes = 8 * 1024;  // stdout/stderr 各截断到这个字节数
};

struct Result {
  bool ran = false;      // 真的执行了（未执行 = 被白名单/参数拒绝或启动失败）
  bool timed_out = false;
  bool cancelled = false;
  // landlock 不可用（ENOSYS/EOPNOTSUPP 或规则应用失败）时降级为「仅 rlimit」。
  // 降级绝不能静默：degraded=true + degraded_reason 会一路传到工具返回与审计行。
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
};

// 在 landlock + rlimit 沙箱里跑一条命令。
//   子进程顺序：setpgid → rlimit → landlock → execv（绝对路径，不走 PATH）。
//   父进程：poll 读 stdout/stderr（非阻塞），每 20ms 检查 cancel 与超时；
//           超时/取消 → kill(-pid, SIGKILL) 整个进程组，然后收尸。
// cancel 非空且置位时必须尽快返回（cancelled=true）——AC-8 的「SIGTERM 后 5 秒退出」
// 依赖这一点：一个 30 秒的命令不能把进程退出顶穿。
Result run(const std::string& cmd, const std::vector<std::string>& args, const Limits& limits,
           const std::atomic<bool>* cancel = nullptr);

// 把当前进程关进「只读放行 /usr /lib /lib64 /etc /proc /sys + 可写 /dev/null +
// 只读工作目录」的 landlock 规则集。返回 false 时 degraded=true 且 error 说明原因
// （调用方应继续执行，只是只剩 rlimit 这一层）。
// 公开给单测：验证「写被拒」最干净的方式是 test 里 fork 一个子进程、在同样规则下
// 直接 open(O_WRONLY)，而不是为测试往白名单里塞 tee/echo > 之类的命令。
bool apply_landlock(bool& degraded, std::string& error);

}  // namespace cue::sandbox
