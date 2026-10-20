#include "cue/pipeline/process_control.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "cue/common/clock.h"

namespace cue::pipeline {

namespace {

using json = nlohmann::json;

// log_tail_ 的上限：fetch_observation(logs) 的字节预算靠它兜底
constexpr std::size_t kLogTailMax = 200;
// 崩溃重启预算与退避：AC-2 要求至少能重启成功一次，但崩溃循环不能无限刷
constexpr int kMaxRestarts = 3;
constexpr auto kRestartBackoff = std::chrono::milliseconds(200);
constexpr auto kReapPoll = std::chrono::milliseconds(50);

// cued 的每秒指标行前缀。严格整行前缀匹配（不是子串），cued 哪天多打一个
// frames_in= 也不会串味（review #29）。
constexpr std::string_view kMetricsTag = "METRICS ";

std::string describe_status(int status) {
  if (WIFSIGNALED(status)) {
    return "signal=" + std::to_string(WTERMSIG(status)) +
           (WCOREDUMP(status) ? "(core)" : "");
  }
  if (WIFEXITED(status)) return "exit=" + std::to_string(WEXITSTATUS(status));
  return "status=" + std::to_string(status);
}

// 给 fd 打上 CLOEXEC：fork 后 exec 前把父进程的其它 fd 全部关掉，
// 否则 cue 的监听 socket 会被 cued 白持有（review #23）。
void mark_inherited_fds_cloexec() {
  long maxfd = ::sysconf(_SC_OPEN_MAX);
  if (maxfd < 0 || maxfd > 4096) maxfd = 4096;
  for (int fd = 3; fd < static_cast<int>(maxfd); ++fd) {
    (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);
  }
}

}  // namespace

ProcessPipelineControl::ProcessPipelineControl(Config cfg) : cfg_(std::move(cfg)) {
  supervisor_ = std::thread([this] { supervisor_loop(); });
}

ProcessPipelineControl::~ProcessPipelineControl() {
  stop();
  stop_super_.store(true);
  if (supervisor_.joinable()) supervisor_.join();
}

bool ProcessPipelineControl::spawn(std::string& err) {
  std::lock_guard<std::mutex> lk(spawn_mu_);
  return spawn_locked(err);
}

bool ProcessPipelineControl::spawn_locked(std::string& err) {
  if (pid_.load() != 0) {
    err = "cued already running";
    return false;
  }
  // 自动重启路径不会经过 stop()：上一代的读线程与日志 fd 必须在这里收干净。
  // 否则给 joinable 的 std::thread 重新赋值会直接 std::terminate（实测踩过）。
  if (reader_.joinable()) {
    stop_reader_.store(true);
    reader_.join();
  }
  if (log_fd_ >= 0) {
    ::close(log_fd_);
    log_fd_ = -1;
  }

  // pipe2(O_CLOEXEC)：两个管道都不会被 cued 继承（review #23）
  int fds[2];
  if (::pipe2(fds, O_CLOEXEC) != 0) {
    err = std::string("pipe2: ") + std::strerror(errno);
    return false;
  }
  // exec 握手管道：execv 成功后写端被 CLOEXEC 关掉（父读到 EOF = 成功）；
  // 失败则子进程写入 errno，父进程据此报"启动失败"而不是假装成功（review #10）
  int hs[2];
  if (::pipe2(hs, O_CLOEXEC) != 0) {
    err = std::string("pipe2(handshake): ") + std::strerror(errno);
    ::close(fds[0]);
    ::close(fds[1]);
    return false;
  }

  std::vector<std::string> args{
      cfg_.cued_bin, "--ws-port", std::to_string(cfg_.ws_port),
      "--probe-port", std::to_string(cfg_.probe_port),
      "--bitrate", std::to_string(cfg_.bitrate_kbps),
      "--fps", std::to_string(cfg_.fps),
      "--gop", std::to_string(cfg_.gop),
      "--preset", cfg_.preset,
      "--send-mbps", std::to_string(cfg_.send_mbps),
      "--inject-drop", std::to_string(cfg_.inject_drop),
      "--inject-seed", std::to_string(cfg_.inject_seed),
      "--fec", cfg_.fec ? "on" : "off",
  };
  if (cfg_.transcode) {
    args.push_back("--transcode");
    args.push_back("on");
    if (cfg_.width > 0 && cfg_.height > 0) {
      args.push_back("--width");
      args.push_back(std::to_string(cfg_.width));
      args.push_back("--height");
      args.push_back(std::to_string(cfg_.height));
    }
  }
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);

  const pid_t pid = ::fork();
  if (pid < 0) {
    err = std::string("fork: ") + std::strerror(errno);
    ::close(fds[0]);
    ::close(fds[1]);
    ::close(hs[0]);
    ::close(hs[1]);
    return false;
  }
  if (pid == 0) {
    // 自己是进程组长：stop() 可以整组回收，probe 之类的孙进程不会成孤儿（AC-8）
    ::setpgid(0, 0);
    // cue 被 SIGKILL/崩溃时也要带走 cued：否则 ps 里会留下没人管的 cued（AC-8）
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (::getppid() == 1) _exit(127);  // 父进程在 prctl 之前就没了
    ::dup2(fds[1], STDOUT_FILENO);
    ::dup2(fds[1], STDERR_FILENO);
    ::close(fds[0]);
    ::close(fds[1]);
    ::close(hs[0]);
    mark_inherited_fds_cloexec();  // hs[1] 也已被标 CLOEXEC（本来就有）
    ::execv(cfg_.cued_bin.c_str(), argv.data());
    const int exec_errno = errno;
    // 唯一的出口：把 errno 交给父进程；_exit 不走 atexit/析构。
    // 写失败只可能是父进程已经不在了，此时无从上报，直接退出。
    const ssize_t ignored = ::write(hs[1], &exec_errno, sizeof(exec_errno));
    (void)ignored;
    _exit(127);
  }

  ::close(fds[1]);
  ::close(hs[1]);
  int child_errno = 0;
  ssize_t hn = 0;
  do {
    hn = ::read(hs[0], &child_errno, sizeof(child_errno));
  } while (hn < 0 && errno == EINTR);
  ::close(hs[0]);

  if (hn == static_cast<ssize_t>(sizeof(child_errno))) {
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    ::close(fds[0]);
    err = "exec " + cfg_.cued_bin + ": " + std::strerror(child_errno);
    return false;
  }

  log_fd_ = fds[0];
  {
    std::lock_guard<std::mutex> lk(mu_);
    // 新会话的指标必须清零：否则重启后 1 s 内 latest() 返回上一个 cued 的数字（#25）
    metrics_ = PipelineMetrics{};
    metrics_.pid = pid;
    push_log_locked("[supervisor] cued started pid=" + std::to_string(pid));
  }
  pid_.store(pid);
  stop_reader_.store(false);
  reader_ = std::thread([this] { reader_loop(); });
  return true;
}

bool ProcessPipelineControl::start(std::string& err) {
  if (pid_.load() != 0) return true;
  return spawn(err);
}

void ProcessPipelineControl::stop() {
  std::lock_guard<std::mutex> lk(spawn_mu_);
  stopping_.store(true);
  const pid_t pid = pid_.load();
  if (pid != 0) {
    // cued 是进程组长（forked 后 setpgid(0,0)），kill(-pid) 连它拉起的子进程一起收；
    // 再补一枪直接 kill(pid)：万一 setpgid 没生效（子进程已换组），只打组会 ESRCH，
    // 后面的 waitpid 就会永久阻塞。
    ::kill(-pid, SIGTERM);
    ::kill(pid, SIGTERM);
    bool gone = false;
    for (int i = 0; i < 50; ++i) {  // 最多等 500ms
      if (pid_.load() != pid) {
        gone = true;
        break;
      }
      ::usleep(10 * 1000);
    }
    if (!gone) {
      ::kill(-pid, SIGKILL);
      ::kill(pid, SIGKILL);
      for (int i = 0; i < 50; ++i) {
        if (pid_.load() != pid) break;
        ::usleep(10 * 1000);
      }
    }
    // 兜底回收：监督线程可能还没轮询到。它若已回收，这里拿到 ECHILD，无副作用。
    int status = 0;
    if (::waitpid(pid, &status, WNOHANG) == 0) {
      while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
    }
    pid_.store(0);
  }
  // 先让读线程醒过来退出，再关 fd：旧顺序 close 在前，读线程可能正阻塞在同一 fd 上，
  // 拿到 EBADF 或（fd 号被复用后）读到别人的数据（review #24）
  stop_reader_.store(true);
  if (reader_.joinable()) reader_.join();
  if (log_fd_ >= 0) {
    ::close(log_fd_);
    log_fd_ = -1;
  }
  restarts_ = 0;
  stopping_.store(false);
}

void ProcessPipelineControl::supervisor_loop() {
  while (!stop_super_.load()) {
    const pid_t pid = pid_.load();
    if (pid == 0) {
      std::this_thread::sleep_for(kReapPoll);
      continue;
    }
    int status = 0;
    const pid_t r = ::waitpid(pid, &status, WNOHANG);
    if (r == pid) {
      pid_.store(0);
      const bool was_stopping = stopping_.load();
      std::string msg = "[supervisor] cued pid=" + std::to_string(pid) + " exited " +
                        describe_status(status);
      if (was_stopping) {
        push_log(msg);
        continue;
      }
      {
        std::lock_guard<std::mutex> slk(spawn_mu_);
        if (stopping_.load()) {
          // stop() 抢到了锁：它会把这一代收干净，不再重启
        } else if (restarts_ >= kMaxRestarts) {
          msg += "（自动重启预算 " + std::to_string(kMaxRestarts) + " 次已耗尽，不再重启）";
        } else {
          ++restarts_;
          std::this_thread::sleep_for(kRestartBackoff);
          std::string rerr;
          if (spawn_locked(rerr)) {
            msg += " → 已自动重启 pid=" + std::to_string(pid_.load());
          } else {
            msg += " → 重启失败: " + rerr;
          }
        }
      }
      push_log(msg);
      std::fprintf(stderr, "%s\n", msg.c_str());
      continue;
    }
    if (r < 0 && errno != EINTR && errno != EAGAIN) {
      // ECHILD：这个 pid 已经被别人回收（或本就是僵尸），保守地认为进程已不在
      pid_.store(0);
      push_log("[supervisor] waitpid(" + std::to_string(pid) + ") failed: " +
               std::strerror(errno));
      continue;
    }
    std::this_thread::sleep_for(kReapPoll);
  }
}

void ProcessPipelineControl::reader_loop() {
  std::string buf;
  char tmp[4096];
  while (!stop_reader_.load()) {
    pollfd pfd{log_fd_, POLLIN, 0};
    const int pr = ::poll(&pfd, 1, 200);  // 200ms 超时：读线程天然可中断（#24）
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (pr == 0) continue;
    const ssize_t n = ::read(log_fd_, tmp, sizeof(tmp));
    if (n == 0) break;  // EOF：cued 已关掉写端
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
      break;
    }
    buf.append(tmp, static_cast<std::size_t>(n));
    std::size_t pos;
    while ((pos = buf.find('\n')) != std::string::npos) {
      std::string line = buf.substr(0, pos);
      buf.erase(0, pos + 1);
      parse_line(line);
    }
  }
}

void ProcessPipelineControl::parse_line(const std::string& line) {
  if (line.rfind(kMetricsTag, 0) == 0) {
    const json j = json::parse(line.substr(kMetricsTag.size()), nullptr, false);
    if (!j.is_object()) {
      push_log("[pipeline] METRICS 行不是合法 JSON: " + line);
      return;
    }
    PipelineMetrics m;
    m.fps = j.value("fps", 0.0);
    m.frames_in = j.value("frames_in", static_cast<uint64_t>(0));
    m.frames_out = j.value("frames_out", static_cast<uint64_t>(0));
    m.dropped = j.value("dropped", static_cast<uint64_t>(0));
    m.injected = j.value("injected", static_cast<uint64_t>(0));
    m.decoded = j.value("decoded", static_cast<uint64_t>(0));
    m.decode_errors = j.value("decode_errors", static_cast<uint64_t>(0));
    m.rejected = j.value("rejected", static_cast<uint64_t>(0));
    auto stage = [&j](const char* key, double& p50, double& p95, double& p99) {
      if (!j.contains(key)) return;
      const auto& s = j.at(key);
      if (!s.is_object()) return;
      p50 = s.value("p50", 0.0);
      p95 = s.value("p95", 0.0);
      p99 = s.value("p99", 0.0);
    };
    double sink_p95 = 0;
    stage("recv", m.recv_p50, sink_p95, sink_p95);
    stage("buffer", m.buffer_p50, sink_p95, sink_p95);
    stage("decode", m.decode_p50, sink_p95, m.decode_p99);
    stage("op", m.op_p50, sink_p95, m.op_p99);
    stage("encode", m.encode_p50, sink_p95, m.encode_p99);
    stage("send", m.send_p50, sink_p95, sink_p95);
    m.updated_us = now_us();
    m.pid = pid_.load();
    std::lock_guard<std::mutex> lk(mu_);
    m.samples = metrics_.samples + 1;
    m.alive = true;
    metrics_ = m;
    return;
  }
  // 人的可读行留在 log_tail_，但每秒的 fps 摘要行不再重复堆进去（指标已结构化）
  if (line.rfind("[cued] fps=", 0) == 0) return;
  push_log(line);
}

void ProcessPipelineControl::push_log(std::string line) {
  std::lock_guard<std::mutex> lk(mu_);
  push_log_locked(std::move(line));
}

void ProcessPipelineControl::push_log_locked(std::string line) {
  log_tail_.push_back(std::move(line));
  while (log_tail_.size() > kLogTailMax) log_tail_.pop_front();
}

PipelineMetrics ProcessPipelineControl::latest() const {
  std::lock_guard<std::mutex> lk(mu_);
  PipelineMetrics m = metrics_;
  m.alive = pid_.load() != 0;
  return m;
}

bool ProcessPipelineControl::set_param(const std::string& key, const std::string& value, std::string& err) {
  Config next = cfg_;
  try {
    if (key == "bitrate_kbps") next.bitrate_kbps = std::stoi(value);
    else if (key == "gop") next.gop = std::stoi(value);
    else if (key == "preset") next.preset = value;
    else if (key == "transcode") next.transcode = (value == "on" || value == "true" || value == "1");
    // SPEC §7.4 的 key 枚举含 width/height：档位表驱动的「降分辨率」必须能执行
    else if (key == "width") next.width = std::stoi(value);
    else if (key == "height") next.height = std::stoi(value);
    else {
      err = "unsupported key: " + key;
      return false;
    }
  } catch (const std::exception&) {
    err = "invalid value for " + key + ": " + value;
    return false;
  }
  // 语义校验
  if (next.bitrate_kbps < 100 || next.bitrate_kbps > 20000) {
    err = "bitrate_kbps out of range [100, 20000]";
    return false;
  }
  if (next.gop < 1 || next.gop > 300) {
    err = "gop out of range [1, 300]";
    return false;
  }
  // 0/0 表示「沿用解码尺寸」；只给一个维度没有意义（cued 侧无法推断另一边）
  if ((next.width == 0) != (next.height == 0)) {
    err = "width/height must be set together (0/0 = follow decoded size)";
    return false;
  }
  if (next.width > 0) {
    if (next.width < 16 || next.width > 4096 || next.height < 16 || next.height > 4096) {
      err = "width/height out of range [16, 4096]";
      return false;
    }
    if (next.width % 2 != 0 || next.height % 2 != 0) {
      err = "width/height must be even (x264 I420 requirement)";
      return false;
    }
  }
  // 幂等：配置没变就不重启（重启会断流，agent 反复 set 同值会空转）
  if (next.bitrate_kbps == cfg_.bitrate_kbps && next.gop == cfg_.gop && next.preset == cfg_.preset &&
      next.transcode == cfg_.transcode && next.width == cfg_.width && next.height == cfg_.height) {
    return true;
  }
  cfg_ = next;
  stop();
  return spawn(err);
}

bool ProcessPipelineControl::inject_fault(const std::string& kind, double value, std::string& err) {
  if (kind != "drop") {
    err = "M3 仅支持 kind=drop（delay/jitter/rate 见 RECEIPT 未实现项）";
    return false;
  }
  if (value < 0 || value > 100) {
    err = "drop percent out of range [0, 100]";
    return false;
  }
  if (std::fabs(cfg_.inject_drop - value) < 1e-9) return true;  // 幂等：注入值没变不重启
  cfg_.inject_drop = value;
  stop();
  return spawn(err);
}

std::string ProcessPipelineControl::observation(const std::string& source, int tail, std::string& err) const {
  if (source != "logs" && source != "procs" && source != "frames") {
    err = "unsupported source: " + source;
    return {};
  }
  std::lock_guard<std::mutex> lk(mu_);
  std::ostringstream oss;
  if (source == "procs") {
    // 注意：不暴露真实注入值——那是"标准答案"，agent 必须从指标推断
    // running=false + pid=0 是 cued 已不在的可靠信号（监督线程负责回收）
    const pid_t pid = pid_.load();
    oss << "cued pid=" << pid << " running=" << (pid != 0 ? "true" : "false")
        << " restarts=" << restarts_ << " transcode=" << (cfg_.transcode ? "on" : "off")
        << " bitrate=" << cfg_.bitrate_kbps << "kbps gop=" << cfg_.gop;
    return oss.str();
  }
  if (source == "frames") {
    oss << "frames_in=" << metrics_.frames_in << " frames_out=" << metrics_.frames_out
        << " dropped=" << metrics_.dropped << " injected=" << metrics_.injected
        << " decoded=" << metrics_.decoded << " decode_errors=" << metrics_.decode_errors;
    return oss.str();
  }
  if (log_tail_.empty()) return {};
  const int n = std::max(1, std::min<int>(tail <= 0 ? 20 : tail, static_cast<int>(log_tail_.size())));
  // 空 log_tail_ 时 size-n 会变成 -1（越界读，ASan 实测 SEGV）
  const int start = std::max(0, static_cast<int>(log_tail_.size()) - n);
  for (int i = start; i < static_cast<int>(log_tail_.size()); ++i) oss << log_tail_[static_cast<std::size_t>(i)] << '\n';
  return oss.str();
}

}  // namespace cue::pipeline
