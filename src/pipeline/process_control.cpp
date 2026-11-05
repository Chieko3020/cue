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
#include <functional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "cue/common/clock.h"
#include "cue/proto/wire.h"

namespace cue::pipeline {

namespace {

using json = nlohmann::json;

// log_tail_ 的上限：fetch_observation(logs) 的字节预算靠它兜底
constexpr std::size_t kLogTailMax = 200;
// 崩溃重启预算与退避：AC-2 要求至少能重启成功一次，但崩溃循环不能无限刷
constexpr int kMaxRestarts = 3;
constexpr auto kRestartBackoff = std::chrono::milliseconds(200);
constexpr auto kReapPoll = std::chrono::milliseconds(50);
// 等 cued 的 UDS socket 出现（spawn 之后）的预算：cued 起来到 bind 控制 socket 只做
// 解码器 open 与配置校验，正常是几十 ms；3 s 足够覆盖 2 vCPU 上被抢占的情况，
// 又不至于让一次 start() 在降级路径上卡 10 s。
constexpr int kControlConnectTimeoutMs = 3000;

// cued 的每秒指标行前缀。严格整行前缀匹配（不是子串），cued 哪天多打一个
// frames_in= 也不会串味（review #29）。
constexpr std::string_view kMetricsTag = "METRICS ";
// 事件缓存上限：observation(events) 与将来的 SSE 都吃这一份。5000 条是 cue 侧
// SSE 环形缓冲的口径（SPEC §5），这里只保留「最近 N 条」够 agent 看即可。
constexpr std::size_t kEventMax = 256;

// 事件时间戳要 unix 微秒（跨进程/墙钟展示）；延迟测量仍只准用 cue::now_us()。
int64_t unix_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// 把 cued 回来的编码档位镜像进 cfg_：cfg_ 是「下一次 spawn 的命令行参数」，
// 不是运行期真值（真值在 cued 的 ParamStore 里，metrics.query 的 params 是权威）。
// 只镜像存在的键：cued 少给一个字段时保留本地值，而不是把它写成默认值。
void apply_encoder_params_json(const nlohmann::json& p, PipelineEndpoint& cfg) {
  if (p.contains("transcode") && p["transcode"].is_string()) {
    cfg.transcode = p["transcode"].get<std::string>() == "on";
  }
  if (p.contains("width") && p["width"].is_number_integer()) {
    cfg.width = p["width"].get<int>();
  }
  if (p.contains("height") && p["height"].is_number_integer()) {
    cfg.height = p["height"].get<int>();
  }
  if (p.contains("bitrate_kbps") && p["bitrate_kbps"].is_number_integer()) {
    cfg.bitrate_kbps = p["bitrate_kbps"].get<int>();
  }
  if (p.contains("fps") && p["fps"].is_number_integer()) {
    cfg.fps = p["fps"].get<int>();
  }
  if (p.contains("gop") && p["gop"].is_number_integer()) {
    cfg.gop = p["gop"].get<int>();
  }
  if (p.contains("preset") && p["preset"].is_string()) {
    cfg.preset = p["preset"].get<std::string>();
  }
  // fec_group/aimd 不是编码参数，但同样是「运行期真值」：镜像进 cfg_ 之后，
  // cued 崩溃被监督线程重启时会带着用户最后设定的值，而不是回到启动默认。
  if (p.contains("fec_group") && p["fec_group"].is_number_integer()) {
    cfg.fec_group = p["fec_group"].get<int>();
  }
  if (p.contains("aimd") && p["aimd"].is_string()) {
    cfg.aimd = p["aimd"].get<std::string>() == "on";
  }
}

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

// fork + exec 的公共部分（cued 与 probe 共用）：
//  - pipe2(O_CLOEXEC) 收子进程的 stdout+stderr；
//  - handshake 管道把 execv 失败的 errno 交回父进程——启动失败必须如实报，不能"假装成功"（review #10）；
//  - 子进程自成进程组并挂 PDEATHSIG：cue 被 SIGKILL 时它拉起的进程不会留成孤儿（AC-8）。
struct Child {
  pid_t pid = 0;  // 0 = 失败（err 有原因）
  int fd = -1;    // 读端：子进程的 stdout+stderr
  std::string err;
};

Child spawn_child(const std::vector<std::string>& args) {
  Child out;
  if (args.empty() || args.front().empty()) {
    out.err = "可执行文件路径为空";
    return out;
  }
  int fds[2];
  if (::pipe2(fds, O_CLOEXEC) != 0) {
    out.err = std::string("pipe2: ") + std::strerror(errno);
    return out;
  }
  int hs[2];
  if (::pipe2(hs, O_CLOEXEC) != 0) {
    out.err = std::string("pipe2(handshake): ") + std::strerror(errno);
    ::close(fds[0]);
    ::close(fds[1]);
    return out;
  }
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);

  const pid_t pid = ::fork();
  if (pid < 0) {
    out.err = std::string("fork: ") + std::strerror(errno);
    ::close(fds[0]);
    ::close(fds[1]);
    ::close(hs[0]);
    ::close(hs[1]);
    return out;
  }
  if (pid == 0) {
    ::setpgid(0, 0);
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (::getppid() == 1) _exit(127);  // 父进程在 prctl 之前就没了
    ::dup2(fds[1], STDOUT_FILENO);
    ::dup2(fds[1], STDERR_FILENO);
    ::close(fds[0]);
    ::close(fds[1]);
    ::close(hs[0]);
    mark_inherited_fds_cloexec();  // hs[1] 也已被标 CLOEXEC（本来就有）
    ::execv(args.front().c_str(), argv.data());
    const int exec_errno = errno;
    // 唯一的出口：把 errno 交给父进程；_exit 不走 atexit/析构。
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
    out.err = "exec " + args.front() + ": " + std::strerror(child_errno);
    return out;
  }
  out.pid = pid;
  out.fd = fds[0];
  return out;
}

}  // namespace

ProcessPipelineControl::ProcessPipelineControl(Config cfg) : cfg_(std::move(cfg)) {
  // 回调单独存一份：cfg_ 会被 set_param/inject_fault 在别的线程整体赋值，
  // reader 线程每次读 cfg_ 都会与它竞争。
  on_event_ = cfg_.on_event;
  // 是否托管探针在构造时定死：probe_bin 不是 set_param 能改的键，所以不需要加锁读 cfg_
  probe_enabled_ = !cfg_.probe_bin.empty();
  supervisor_ = std::thread([this] { supervisor_loop(); });
}

ProcessPipelineControl::~ProcessPipelineControl() {
  stop();
  stop_super_.store(true);
  if (supervisor_.joinable()) supervisor_.join();
}

// ---------------------------------------------------------------- 控制通道（UDS）

std::string ProcessPipelineControl::control_socket_path(pid_t pid) {
  // 路径由 cued 决定（/tmp/cue/<pid>/control.sock）：按 pid 分目录，同机多个 cued 不互踩。
  // 不走命令行参数：控制 socket 的位置必须是"可推导的"，否则两端又多一个要同步的配置项。
  return "/tmp/cue/" + std::to_string(static_cast<long long>(pid)) + "/control.sock";
}

ProcessPipelineControl::Config ProcessPipelineControl::cfg_snapshot() const {
  std::lock_guard<std::mutex> lk(cfg_mu_);
  return cfg_;
}

ProcessPipelineControl::Config ProcessPipelineControl::config() const { return cfg_snapshot(); }

std::string ProcessPipelineControl::control_channel() const {
  return control_channel_tag(ctl_kind_.load());
}

std::string ProcessPipelineControl::last_control_error() const {
  std::lock_guard<std::mutex> lk(ctl_mu_);
  return last_control_error_;
}

void ProcessPipelineControl::close_control_locked() {
  std::lock_guard<std::mutex> lk(ctl_mu_);
  if (ctl_) {
    ctl_->close();
    ctl_.reset();
  }
  ctl_kind_.store(ControlChannelKind::None);
}

bool ProcessPipelineControl::connect_control_locked(std::string& err) {
  close_control_locked();
  const pid_t pid = pid_.load();
  if (pid == 0) {
    err = "cued 未运行（没有 pid，推不出控制 socket 路径）";
    return false;
  }
  const std::string path = control_socket_path(pid);
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(kControlConnectTimeoutMs);
  std::string last = "socket 未出现";
  for (;;) {
    if (pid_.load() != pid) {
      last = "cued 在等控制 socket 时退出了";
      break;
    }
    if (::access(path.c_str(), F_OK) == 0) {
      auto client = std::make_unique<ControlClient>();
      std::string cerr;
      if (client->connect(path, 300, cerr)) {
        {
          std::lock_guard<std::mutex> lk(ctl_mu_);
          ctl_ = std::move(client);
          last_control_error_.clear();
          ctl_kind_.store(ctl_->kind());
        }
        // 启动参数下发：一次 pipeline.start 的 args 批量应用。
        // 为什么不是一组 set_param：
        //  1) set_param 一次只改一个键，中间态（width 改了 height 还没改）会被正在跑的
        //     媒体循环看到并重建一次编码器；pipeline.start 的 args 是原子的一批；
        //  2) 命令行参数仍然原样下传（评测脚本依赖 --inject-drop 的"启动即生效"语义），
        //     这里是"命令行为初值、控制面确认一次"，同值幂等（不推进 generation）。
        const ProcessPipelineControl::Config cfg = cfg_snapshot();
        json args{{"bitrate_kbps", cfg.bitrate_kbps},
                  {"gop", cfg.gop},
                  {"preset", cfg.preset},
                  {"transcode", cfg.transcode ? "on" : "off"},
                  {"fps", cfg.fps},
                  {"fec_group", cfg.fec_group},
                  {"aimd", cfg.aimd ? "on" : "off"}};
        // width/height 只在成对有效时下发：单给一维本身非法，不该让整批失败。
        if (cfg.width > 0 && cfg.height > 0) {
          args["width"] = cfg.width;
          args["height"] = cfg.height;
        } else if (cfg.width == 0 && cfg.height == 0) {
          args["width"] = 0;
          args["height"] = 0;
        }
        json res;
        std::string serr;
        if (!control_call("pipeline.start", args, res, serr)) {
          // 控制通道通了但参数确认失败：不假装成功，留一条日志与事件（参数的实际来源
          // 仍是命令行，所以不把 start() 判成失败）。
          {
            std::lock_guard<std::mutex> lk(mu_);
            push_log_locked("[control] 启动参数下发失败：" + serr);
          }
          emit_local_event("control.error", json{{"op", "pipeline.start"}, {"reason", serr}});
        }
        return true;
      }
      last = cerr;
    }
    if (std::chrono::steady_clock::now() >= deadline) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  err = "等待控制 socket " + path + " 超时（" + std::to_string(kControlConnectTimeoutMs) +
        " ms）：" + last;
  {
    std::lock_guard<std::mutex> lk(ctl_mu_);
    last_control_error_ = "控制连接不可用：" + err;
    ctl_kind_.store(ControlChannelKind::None);
  }
  // 降级不是静默的：事件 + 日志 + (指标里的) control_channel=stdout 三处都要能看见
  emit_local_event("control.degraded", json{{"reason", err}, {"channel", "stdout"}});
  return false;
}

bool ProcessPipelineControl::control_call(const char* op, const json& args, json& result,
                                          std::string& err) const {
  std::lock_guard<std::mutex> lk(ctl_mu_);
  if (!ctl_) {
    // 懒重连：只在 socket 还在（cued 还活着）时试一次。这恢复的是**控制连接本身**，
    // 进程、会话、媒体流都不动——与"重启 cued 让它重新读命令行"是两回事。
    const pid_t pid = pid_.load();
    if (pid != 0) {
      const std::string path = control_socket_path(pid);
      if (::access(path.c_str(), F_OK) == 0) {
        auto client = std::make_unique<ControlClient>();
        std::string cerr;
        if (client->connect(path, 200, cerr)) {
          ctl_ = std::move(client);
          ctl_kind_.store(ctl_->kind());
          last_control_error_.clear();
        }
      }
    }
  }
  if (!ctl_) {
    err = last_control_error_.empty() ? "控制连接不可用（UDS 未建立）" : last_control_error_;
    err += "；降级到 stdout 模式，改参数需要重启 cued（本版不会为此自动重启）";
    return false;
  }
  json resp;
  if (!ctl_->call(op, args, resp, err)) {
    if (!ctl_->connected()) {
      // 断了：丢掉客户端并留下原因；下一次调用会先试一次重连
      last_control_error_ = "控制连接不可用：" + err;
      ctl_.reset();
      ctl_kind_.store(ControlChannelKind::None);
    }
    return false;
  }
  result = std::move(resp);
  return true;
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
  // 先起 probe（cued 的下游）：probe 绑定端口之前 cued 发的包会被内核直接丢掉，
  // 端到端指标就会从第一个窗口起缺样本。probe 起不来也照样起 cued——否则"探针没起来"
  // 会被报成 pipeline_start_failed，掩盖真正的原因。失败原因留给 probe_latest()。
  if (probe_enabled_ && probe_pid_.load() == 0) {
    std::string perr;
    if (!spawn_probe_locked(perr)) {
      {
        std::lock_guard<std::mutex> lk(mu_);
        probe_unavailable_ = "probe 启动失败：" + perr;
        push_log_locked("[supervisor] " + probe_unavailable_);
      }
      emit_local_event("probe.unavailable", json{{"reason", perr}});
    }
  }
  if (spawn_cued_locked(err)) return true;
  // cued 起不来：这一次拉起的探针不能留成孤儿。stopping_ 也要挡住监督线程——
  // 否则它可能把这次"控制面收尾"当成探针崩溃，再拉一代起来占着端口。
  stopping_.store(true);
  kill_probe_locked();
  stopping_.store(false);
  return false;
}

bool ProcessPipelineControl::spawn_cued_locked(std::string& err) {
  // 自动重启 / 改档重启都不会经过 stop()：上一代的读线程与日志 fd 必须在这里收干净。
  // 否则给 joinable 的 std::thread 重新赋值会直接 std::terminate（实测踩过）。
  if (reader_.joinable()) {
    stop_reader_.store(true);
    reader_.join();
  }
  if (log_fd_ >= 0) {
    ::close(log_fd_);
    log_fd_ = -1;
  }

  // 快照一次：cfg_ 会被控制线程改（set_param / inject_fault 成功后镜像），
  // 边读边被改会拼出"新码率 + 旧 GOP"这种命令行。
  const ProcessPipelineControl::Config cfg = cfg_snapshot();
  std::vector<std::string> args{
      cfg.cued_bin, "--ws-port", std::to_string(cfg.ws_port),
      "--probe-port", std::to_string(cfg.probe_port),
      "--bitrate", std::to_string(cfg.bitrate_kbps),
      "--fps", std::to_string(cfg.fps),
      "--gop", std::to_string(cfg.gop),
      "--preset", cfg.preset,
      "--send-mbps", std::to_string(cfg.send_mbps),
      "--inject-drop", std::to_string(cfg.inject_drop),
      "--inject-seed", std::to_string(cfg.inject_seed),
      "--fec", cfg.fec ? "on" : "off",
      "--fec-group", std::to_string(cfg.fec_group),
      "--aimd", cfg.aimd ? "on" : "off",
  };
  // 上行注入与发送排队只在开启时下传：不带这两个参数时 cued 的行为与 M2 完全一致
  if (cfg.inject_ingress_drop > 0.0) {
    args.push_back("--inject-ingress-drop");
    args.push_back(std::to_string(cfg.inject_ingress_drop));
  }
  if (cfg.queue_fps > 0.0) {
    args.push_back("--queue-fps");
    args.push_back(std::to_string(cfg.queue_fps));
  }
  // 延迟注入同样只在开启时下传：不带参数时 cued 的发送路径与 M2 逐字节一致
  // （多一个默认 0 的开关会让人以为「默认就在压包」）。
  if (cfg.inject_delay_ms > 0.0) {
    args.push_back("--inject-delay-ms");
    args.push_back(std::to_string(cfg.inject_delay_ms));
  }
  if (cfg.inject_jitter_ms > 0.0) {
    args.push_back("--inject-jitter-ms");
    args.push_back(std::to_string(cfg.inject_jitter_ms));
  }
  // 注入 trace 同样只在显式给了路径时下传（空 = cued 不落 trace）：它是复现证据，
  // 不是行为开关，无条件给一个默认路径会让每次起 cued 都写一份没人清理的文件。
  // 判定仍然由 `should_drop_pkt(seed, permille, seq)` 决定，trace 只记录判决结果。
  if (!cfg.inject_trace.empty()) {
    args.push_back("--inject-trace");
    args.push_back(cfg.inject_trace);
  }
  if (cfg.transcode) {
    args.push_back("--transcode");
    args.push_back("on");
    if (cfg.width > 0 && cfg.height > 0) {
      args.push_back("--width");
      args.push_back(std::to_string(cfg.width));
      args.push_back("--height");
      args.push_back(std::to_string(cfg.height));
    }
  }

  const Child cued = spawn_child(args);
  if (cued.pid == 0) {
    err = cued.err;
    return false;
  }
  log_fd_ = cued.fd;
  {
    std::lock_guard<std::mutex> lk(mu_);
    // 新会话的指标必须清零：否则重启后 1 s 内 latest() 返回上一个 cued 的数字（#25）
    metrics_ = PipelineMetrics{};
    metrics_.pid = cued.pid;
    // 去重水位属于「这一代 cued」，换代必须一起清零：水位是"同一代之内响应单调
    // 递增"的假设，跨代不成立。当前 cued 的 t_us 是同一台机器的 steady_clock，
    // 换代后天然更大，所以不清零今天也不会误判；但一代一代地比下去，等于把
    // "旧进程的数"当成新响应的下界——一旦 t_us 换成进程相对时钟（或 cued 挪到
    // 另一台机器/容器里），新进程的 t_us 会从 0 重新开始，不清零就会把新一代的
    // 每一份响应都判成"不新"，params 再次永远进不来。
    uds_wm_.reset();
    push_log_locked("[supervisor] cued started pid=" + std::to_string(cued.pid));
  }
  pid_.store(cued.pid);
  stop_reader_.store(false);
  reader_ = std::thread([this] { reader_loop(); });
  // 控制通道：cued 起来之后才有 socket。这里等（轮询 + 超时），失败**不阻断启动**——
  // 「控制面没建起来」不等于「管线不能用」，但降级事实必须一路传出去：
  // last_control_error_ + 日志 + control.degraded 事件，且 set_param 会明确拒绝。
  std::string cerr;
  if (!connect_control_locked(cerr)) {
    std::lock_guard<std::mutex> lk(mu_);
    push_log_locked("[control] 降级到 stdout：" + cerr);
  }
  return true;
}

bool ProcessPipelineControl::spawn_probe_locked(std::string& err) {
  if (probe_pid_.load() != 0) {
    err = "probe already running";
    return false;
  }
  if (probe_reader_.joinable()) {
    stop_probe_reader_.store(true);
    probe_reader_.join();
  }
  if (probe_fd_ >= 0) {
    ::close(probe_fd_);
    probe_fd_ = -1;
  }
  const ProcessPipelineControl::Config cfg = cfg_snapshot();
  std::vector<std::string> args{
      cfg.probe_bin, "--port", std::to_string(cfg.probe_port),
      "--bind", cfg.probe_host,
      // 标称帧率只用来推卡顿阈值（stall_factor × 1/fps），不是测量值
      "--fps", std::to_string(cfg.fps),
      // 接收端缓冲深度必须与 cued 侧的 --jitter-target-frames 对齐：
      // probe 的历史默认是 2 帧（30 fps 下 66 ms），而 case 的 SLA 门限是
      // 端到端 p50 <= 60 ms —— 默认值单独就能顶穿它，且失败原因看起来像"管线慢"。
      // 这里显式传 1，与 cued 的零驻留口径一致。
      "--jitter-target-frames", "1",
  };
  if (!cfg.probe_report_addrs.empty()) {
    // 跨机模式：harness 与 probe 不在同一台机器上时，指标走 TCP JSON-line 上报；
    // 本机模式下这条通道为空，probe 只打 stdout（我们直接读管道）。
    args.push_back("--report-addrs");
    args.push_back(cfg.probe_report_addrs);
  }
  const Child probe = spawn_child(args);
  if (probe.pid == 0) {
    err = probe.err;
    return false;
  }
  probe_fd_ = probe.fd;
  {
    std::lock_guard<std::mutex> lk(mu_);
    // 新一代探针：上一代的读数必须清零，否则 probe_latest() 会拿旧指标冒充当前状态
    probe_metrics_ = ProbeMetrics{};
    probe_metrics_.pid = probe.pid;
    probe_unavailable_.clear();
    probe_last_log_.clear();
    push_log_locked("[supervisor] probe started pid=" + std::to_string(probe.pid));
  }
  probe_pid_.store(probe.pid);
  stop_probe_reader_.store(false);
  probe_reader_ = std::thread([this] { probe_reader_loop(); });
  emit_local_event("probe.spawned",
                   json{{"pid", probe.pid}, {"port", cfg.probe_port}, {"bin", cfg.probe_bin}});
  return true;
}

void ProcessPipelineControl::kill_probe_locked() {
  const pid_t pid = probe_pid_.load();
  if (pid != 0) {
    ::kill(pid, SIGTERM);
    int status = 0;
    bool gone = false;
    for (int i = 0; i < 50; ++i) {  // 最多等 500ms
      const pid_t r = ::waitpid(pid, &status, WNOHANG);
      if (r == pid || (r < 0 && errno == ECHILD)) {
        gone = true;
        break;
      }
      ::usleep(10 * 1000);
    }
    if (!gone) {
      ::kill(pid, SIGKILL);
      while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
    }
    // 只清自己那一代：监督线程可能已经 reaped 并重启了一代，直接清零会把新探针
    // 从账上抹掉（它还在跑、还占着端口，之后再也回收不到）。
    if (probe_pid_.load() == pid) probe_pid_.store(0);
    emit_local_event("probe.stopped", json{{"reason", "control"}, {"pid", pid}});
  }
  // 先让读线程退出再关 fd（与 cued 侧同一个理由：fd 号被复用后可能读到别人的数据）
  stop_probe_reader_.store(true);
  if (probe_reader_.joinable()) probe_reader_.join();
  if (probe_fd_ >= 0) {
    ::close(probe_fd_);
    probe_fd_ = -1;
  }
}

bool ProcessPipelineControl::start(std::string& err) {
  if (pid_.load() != 0) return true;
  return spawn(err);
}

void ProcessPipelineControl::stop() {
  std::lock_guard<std::mutex> lk(spawn_mu_);
  stopping_.store(true);
  kill_cued_locked();
  // 倒序收下游：cued 停了以后 probe 已经没有数据来源，先停 cued 再停它，
  // 避免"探针先死、cued 还在往死端口发包"这段窗口里的假计数。
  kill_probe_locked();
  close_control_locked();
  restarts_ = 0;
  probe_restarts_ = 0;
  stopping_.store(false);
}

void ProcessPipelineControl::kill_cued_locked() {
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
    // 控制面主动停：与 cued 自己被 SIGTERM 时的 pipeline.stopped(origin=process) 区分开
    emit_local_event("pipeline.stopped", json{{"reason", "control"}, {"pid", pid}});
  }
  // 控制连接属于这一代 cued：进程没了，fd 立刻失效，留着只会让下一次请求
  // 拿到 EPIPE 才发现。这里主动断开，并保留原因（observation 会带出去）。
  close_control_locked();
  // 先让读线程醒过来退出，再关 fd：旧顺序 close 在前，读线程可能正阻塞在同一 fd 上，
  // 拿到 EBADF 或（fd 号被复用后）读到别人的数据（review #24）
  stop_reader_.store(true);
  if (reader_.joinable()) reader_.join();
  if (log_fd_ >= 0) {
    ::close(log_fd_);
    log_fd_ = -1;
  }
}

void ProcessPipelineControl::supervisor_loop() {
  // cued 与 probe 各自有独立的 pid 与重启预算：探针崩溃不该重启 cued（那会断流），
  // cued 崩溃也不该把探针的累计计数清零（端到端样本是接收端视角）。
  while (!stop_super_.load()) {
    const pid_t cued = pid_.load();
    if (cued != 0) supervise_cued_once(cued);
    const pid_t probe = probe_pid_.load();
    if (probe != 0) supervise_probe_once(probe);
    std::this_thread::sleep_for(kReapPoll);
  }
}

void ProcessPipelineControl::supervise_cued_once(pid_t pid) {
  int status = 0;
  const pid_t r = ::waitpid(pid, &status, WNOHANG);
  if (r == pid) {
    pid_.store(0);
    const bool was_stopping = stopping_.load();
    std::string msg = "[supervisor] cued pid=" + std::to_string(pid) + " exited " +
                      describe_status(status);
    if (was_stopping) {
      push_log(msg);
      return;
    }
    bool restarted = false;
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
          restarted = true;
          msg += " → 已自动重启 pid=" + std::to_string(pid_.load());
        } else {
          msg += " → 重启失败: " + rerr;
        }
      }
    }
    // 崩溃/被 kill：cued 自己来不及报（SIGKILL 更是没机会），由监督方补事件。
    // 走到这里 was_stopping 必为 false（true 的分支上面已经 return）。
    emit_local_event("pipeline.stopped", json{{"reason", "exit"},
                                              {"pid", pid},
                                              {"status", describe_status(status)},
                                              {"restarting", restarted}});
    if (restarted) {
      emit_local_event("pipeline.restart", json{{"reason", "auto_restart"},
                                                {"old_pid", pid},
                                                {"new_pid", pid_.load()}});
    }
    push_log(msg);
    std::fprintf(stderr, "%s\n", msg.c_str());
    return;
  }
  if (r < 0 && errno != EINTR && errno != EAGAIN) {
    // ECHILD：这个 pid 已经被别人回收（或本就是僵尸），保守地认为进程已不在
    pid_.store(0);
    push_log("[supervisor] waitpid(" + std::to_string(pid) + ") failed: " + std::strerror(errno));
  }
}

void ProcessPipelineControl::supervise_probe_once(pid_t pid) {
  int status = 0;
  const pid_t r = ::waitpid(pid, &status, WNOHANG);
  if (r == pid) {
    probe_pid_.store(0);
    const bool was_stopping = stopping_.load();
    std::string msg = "[supervisor] probe pid=" + std::to_string(pid) + " exited " +
                      describe_status(status);
    if (was_stopping) {
      push_log(msg);
      return;
    }
    bool restarted = false;
    {
      std::lock_guard<std::mutex> slk(spawn_mu_);
      if (stopping_.load()) {
        // stop() 抢到了锁：它会把这一代收干净，不再重启
      } else if (probe_restarts_ >= kMaxRestarts) {
        msg += "（probe 自动重启预算 " + std::to_string(kMaxRestarts) + " 次已耗尽，"
               "端到端指标将不可用）";
        std::lock_guard<std::mutex> lk(mu_);
        probe_unavailable_ = "probe 反复退出且自动重启预算已耗尽";
      } else {
        ++probe_restarts_;
        std::this_thread::sleep_for(kRestartBackoff);
        std::string perr;
        if (spawn_probe_locked(perr)) {
          restarted = true;
          msg += " → 已自动重启 pid=" + std::to_string(probe_pid_.load());
        } else {
          msg += " → 重启失败: " + perr;
          std::lock_guard<std::mutex> lk(mu_);
          probe_unavailable_ = "probe 重启失败：" + perr;
        }
      }
    }
    emit_local_event("probe.exited", json{{"reason", "exit"},
                                          {"pid", pid},
                                          {"status", describe_status(status)},
                                          {"restarting", restarted}});
    push_log(msg);
    std::fprintf(stderr, "%s\n", msg.c_str());
    return;
  }
  if (r < 0 && errno != EINTR && errno != EAGAIN) {
    probe_pid_.store(0);
    push_log("[supervisor] waitpid(probe " + std::to_string(pid) +
             ") failed: " + std::strerror(errno));
  }
}

// cued 与 probe 的读线程只差一个 fd 和一个停止标志：解析逻辑完全共用
// （两者都按 wire.h 的 [cue-event] 行格式输出，人读行也都只进 log_tail_）。
void ProcessPipelineControl::read_loop(int fd, std::atomic<bool>& stop_flag) {
  std::string buf;
  char tmp[4096];
  while (!stop_flag.load()) {
    pollfd pfd{fd, POLLIN, 0};
    const int pr = ::poll(&pfd, 1, 200);  // 200ms 超时：读线程天然可中断（#24）
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (pr == 0) continue;
    const ssize_t n = ::read(fd, tmp, sizeof(tmp));
    if (n == 0) break;  // EOF：子进程已关掉写端
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

void ProcessPipelineControl::reader_loop() { read_loop(log_fd_, stop_reader_); }

void ProcessPipelineControl::probe_reader_loop() { read_loop(probe_fd_, stop_probe_reader_); }

// METRICS 行与 UDS metrics.query 的响应共用这一份解析：同一个 PipelineMetrics 口径，
// 两个数据源。分开写两份的后果是"stdout 读数"与"UDS 读数"迟早对不上，而它们本应
// 是同一份 report() 的产物。
//
// from_uds=true 时额外做两件事：
//  1) 用独立的去重水位 uds_wm_（只记 cued 自报的 t_us）判断「这份响应是不是更新」，
//     不比上次采纳的新就整份丢弃；判断**不**碰 metrics_.updated_us——后者是本机时钟
//     口径（age_ms = now_us() - updated_us），拿它当去重基准等于拿「数据产生时刻」
//     和「我收到它的时刻」比大小：stdout 每秒那行已经把 updated_us 顶到"刚刚"，
//     于是每一份 UDS 响应都"不比缓存新"，params（编码档位真值）永远进不了缓存——
//     真管线下 encode_width/height 恒 null、m2 的 avc 证据不成立，就是这么来的。
//  2) samples 不递增（那是"解析到几条 stdout METRICS 行"的计数），只在缓存为空时
//     采纳 cued 自报的 samples。
// 两条路径的 updated_us 一律写本机 now_us()（也顺带修掉了 stdout 路径把 t_us 混进
// 时间戳的隐患）。
bool ProcessPipelineControl::apply_metrics_json(const json& j, bool from_uds) {
  if (!j.is_object()) return false;
  PipelineMetrics m;  // 分位默认 NaN：cued 打 null 的段保持"无样本"
  // 计数一律按"键存在且是非负整数"解析：value() 遇到 null/类型不符会抛异常，
  // 而 METRICS 行来自子进程，任何字段形态都可能出现（不让它穿透 reader 线程）。
  auto count = [&j](const char* key) -> uint64_t {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return 0;
    const int64_t v = it->get<int64_t>();
    return v > 0 ? static_cast<uint64_t>(v) : 0;
  };
  const auto fps_it = j.find("fps");
  m.fps = (fps_it != j.end() && fps_it->is_number()) ? fps_it->get<double>() : 0.0;
  m.frames_in = count("frames_in");
  m.frames_out = count("frames_out");
  m.dropped = count("dropped");
  m.injected_observed = read_observed_counter(j, "injected", m.injected);
  m.decoded = count("decoded");
  m.decode_errors = count("decode_errors");
  m.rejected = count("rejected");
  m.send_errors = count("send_errors");
  m.encode_errors = count("encode_errors");
  m.decode_drops = count("decode_drops");
  m.budget_drops_observed = read_observed_counter(j, "budget_drops", m.budget_drops);
  m.queue_dropped = count("queue_dropped");
  m.delayed_pkts = count("delayed_pkts");
  m.delayed_max_us = count("delayed_max_us");
  m.ingress_late = count("ingress_late");
  // 段分位：cued 写 null（本秒没有样本）或整段缺失 → 保持 NaN，绝不写成 0。
  // p95 过去被塞进一个丢弃用的 sink（"没有消费者"）——这正是报告 p95 恒 n/a 的根因：
  // 生产端一直在发，消费端解析出来就扔了（N-05）。现在六段各收各的三分位，
  // recv/buffer 的 p99 也补上了字段（否则与 p95 一样白丢）。解析语义见
  // apply_stage_percentiles：整段缺失/坏字段都只让对应位保持 NaN，不把整行判坏。
  apply_stage_percentiles(j, "recv", m.recv_p50, m.recv_p95, m.recv_p99);
  apply_stage_percentiles(j, "buffer", m.buffer_p50, m.buffer_p95, m.buffer_p99);
  apply_stage_percentiles(j, "decode", m.decode_p50, m.decode_p95, m.decode_p99);
  apply_stage_percentiles(j, "op", m.op_p50, m.op_p95, m.op_p99);
  apply_stage_percentiles(j, "encode", m.encode_p50, m.encode_p95, m.encode_p99);
  // send 段：延迟注入的主证据（send 段涨、计数不涨）在 p99 上，p95 同样保留。
  apply_stage_percentiles(j, "send", m.send_p50, m.send_p95, m.send_p99);
  // 控制面字段（T-05）：通道类型与 cued 累计处理的控制请求数
  if (const auto it = j.find("control_channel"); it != j.end() && it->is_string()) {
    m.control_channel = it->get<std::string>();
  }
  m.control_ops = count("control_ops");
  // 编码档位（N-02）：只有 UDS 的 metrics.query 响应带 params（cued 的 ParamStore 真值）。
  // stdout 的 METRICS 行没有这一段 → 保持 encoder_known=false，观测面输出 null（未知），
  // 绝不把"没拿到"写成 0（0 会让"1080p 没在转码"和"不知道尺寸"长得一模一样）。
  if (const auto pit = j.find("params"); pit != j.end() && pit->is_object()) {
    auto pnum = [&pit](const char* key, int& out) {
      const auto f = pit->find(key);
      if (f == pit->end() || !f->is_number_integer()) return;
      out = f->get<int>();
    };
    auto pstr = [&pit](const char* key, std::string& out) {
      const auto f = pit->find(key);
      if (f == pit->end() || !f->is_string()) return;
      out = f->get<std::string>();
    };
    m.encoder_known = true;
    pnum("width", m.enc_width);
    pnum("height", m.enc_height);
    pnum("bitrate_kbps", m.enc_bitrate_kbps);
    pnum("gop", m.enc_gop);
    pstr("preset", m.enc_preset);
    pstr("transcode", m.enc_transcode);
  }
  const int64_t remote_t = (j.contains("t_us") && j["t_us"].is_number_integer())
                               ? j["t_us"].get<int64_t>()
                               : 0;
  m.pid = pid_.load();
  m.alive = true;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (from_uds) {
      // 去重只看「上一次采纳的 cued t_us」（与 metrics_ 同一临界区：判定 + 推进 +
      // 写缓存必须原子）。判成不新就原样保留缓存——连 age_ms 都不刷新。
      if (!uds_wm_.accept(remote_t)) return true;
      m.samples = metrics_.samples;
      if (m.samples == 0) {
        const auto s_it = j.find("samples");
        if (s_it != j.end() && s_it->is_number_integer()) {
          const int64_t v = s_it->get<int64_t>();
          if (v > 0) m.samples = static_cast<uint64_t>(v);
        }
      }
    } else {
      m.samples = metrics_.samples + 1;
    }
    // 时间戳只写本机时钟：age_ms 是 now_us() - updated_us，写 cued 的 t_us 会让它
    // 变成负几十万小时（两个数语义不同）。去重与时间戳是两件事，别合并。
    m.updated_us = now_us();
    metrics_ = m;
  }
  return true;
}

void ProcessPipelineControl::parse_line(const std::string& line) {
  // 结构化事件行：整行标签匹配，与 [cued] 人读行 / METRICS 行互不干扰
  if (line.rfind(cue::proto::kEventTag, 0) == 0) {
    parse_event_line(line.substr(cue::proto::kEventTag.size()));
    return;
  }
  if (line.rfind(kMetricsTag, 0) == 0) {
    const json j = json::parse(line.substr(kMetricsTag.size()), nullptr, false);
    if (!j.is_object()) {
      push_log("[pipeline] METRICS 行不是合法 JSON: " + line);
      return;
    }
    if (!apply_metrics_json(j, /*from_uds=*/false)) {
      push_log("[pipeline] METRICS 行不是对象: " + line);
    }
    return;
  }
  // 人的可读行留在 log_tail_，但每秒的 fps 摘要行不再重复堆进去（指标已结构化）
  if (line.rfind("[cued] fps=", 0) == 0) return;
  if (line.rfind("[probe] fps=", 0) == 0) return;
  // probe 的人读行单独留一份：它起不来/绑不上端口时，原因就在这几行里
  if (line.rfind("[probe]", 0) == 0) {
    std::lock_guard<std::mutex> lk(mu_);
    probe_last_log_ = line;
  }
  push_log(line);
}

void ProcessPipelineControl::parse_event_line(const std::string& body) {
  const json j = json::parse(body, nullptr, false);
  if (!j.is_object()) {
    push_log("[pipeline] 事件行不是合法 JSON: " + body);
    return;
  }
  const auto ev_it = j.find("ev");
  if (ev_it == j.end() || !ev_it->is_string()) {
    push_log("[pipeline] 事件行缺 ev 字段: " + body);
    return;
  }
  PipelineEvent ev;
  ev.type = ev_it->get<std::string>();
  if (const auto d = j.find("data"); d != j.end() && d->is_object()) ev.data = *d;
  // t 可能缺失（合成事件/旧版本）或类型不对：不让 value() 抛异常，缺了就记 0
  if (const auto t = j.find("t"); t != j.end() && t->is_number_integer()) {
    ev.t_us = t->get<int64_t>();
  }
  ev.seen_us = now_us();
  // probe 的每秒指标行同时是「事件」（进缓存/SSE）和「结构化快照」（probe_latest 的数据源）。
  // 只把它当事件缓存起来是不够的：消费方要的是"当前值"，不是自己去事件流里倒推。
  if (ev.type == "probe.metric") parse_probe_metric(ev.data);
  record_event(std::move(ev));
}

void ProcessPipelineControl::parse_probe_metric(const json& data) {
  ProbeMetrics p;
  std::string perr;
  if (!probe_metrics_from_json(data, p, perr)) {
    push_log("[pipeline] probe.metric 解析失败：" + perr);
    return;
  }
  std::lock_guard<std::mutex> lk(mu_);
  const pid_t pid = probe_pid_.load();
  // samples 由 harness 数「收到了几条指标行」，不是从数值里猜的：
  // 评测侧的「等新采样」逻辑依赖它单调增长。
  p.samples = probe_metrics_.samples + 1;
  p.updated_us = now_us();
  p.pid = pid;
  p.alive = pid != 0;
  probe_metrics_ = std::move(p);
}

ProbeMetrics ProcessPipelineControl::probe_latest() const {
  std::lock_guard<std::mutex> lk(mu_);
  ProbeMetrics p = probe_metrics_;
  p.alive = probe_pid_.load() != 0;
  // 不可用要**说清是哪一种**：进程没起、起不来、起来了但一条指标都没有，
  // 三者对使用者意味着不同的处置（换路径 / 查端口 / 查媒体流）。
  if (!probe_enabled_) {
    p.unavailable_reason = "probe 未启用（probe_bin 为空）";
  } else if (!probe_unavailable_.empty()) {
    p.unavailable_reason = probe_unavailable_;
    if (!probe_last_log_.empty()) p.unavailable_reason += "；probe 日志：" + probe_last_log_;
  } else if (!p.alive) {
    p.unavailable_reason = "probe 进程未运行（起不来或刚退出）";
  } else if (p.samples == 0) {
    p.unavailable_reason = "probe 已启动但还没有指标上报（不足 1 s 或没有媒体流）";
    if (!probe_last_log_.empty()) p.unavailable_reason += "；probe 日志：" + probe_last_log_;
  }
  return p;
}

void ProcessPipelineControl::emit_local_event(std::string type, json data) {
  // 合成的控制面事件：origin=control 与子进程来的事件区分开（子进程的不带 origin）
  if (data.is_object()) data["origin"] = "control";
  PipelineEvent ev;
  ev.type = std::move(type);
  ev.data = std::move(data);
  ev.t_us = unix_us();
  ev.seen_us = now_us();
  record_event(std::move(ev));
}

void ProcessPipelineControl::record_event(PipelineEvent ev) {
  std::function<void(const PipelineEvent&)> cb;
  {
    std::lock_guard<std::mutex> lk(mu_);
    // codec_config 的 SPS/PPS 单独缓存：它是"媒体元信息"，不属于每秒指标
    if (ev.type == "codec.config" && ev.data.is_object()) {
      auto str = [&ev](const char* key, std::string& out) {
        const auto it = ev.data.find(key);
        if (it == ev.data.end() || !it->is_string()) return;
        out = it->get<std::string>();
      };
      str("codec", codec_name_);
      str("sps", sps_hex_);
      str("pps", pps_hex_);
      str("raw", codec_raw_hex_);
    }
    events_.push_back(ev);
    while (events_.size() > kEventMax) events_.pop_front();
    cb = on_event_;
  }
  // 回调在锁外调用：消费者（SSE/前端）可能回头调 latest()/recent_events()
  if (cb) cb(ev);
}

void ProcessPipelineControl::set_event_callback(std::function<void(const PipelineEvent&)> cb) {
  std::lock_guard<std::mutex> lk(mu_);
  on_event_ = std::move(cb);
}

std::vector<PipelineEvent> ProcessPipelineControl::recent_events(std::size_t n) const {
  std::lock_guard<std::mutex> lk(mu_);
  if (n == 0 || n >= events_.size()) return {events_.begin(), events_.end()};
  return {events_.end() - static_cast<std::ptrdiff_t>(n), events_.end()};
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
  // 旧版 cued 的 METRICS 行没有 control_channel：按控制面实际状态填。
  // 留空会被读成"没有控制面"，而事实上它只是"这版没上报"。
  if (m.control_channel.empty()) m.control_channel = control_channel_tag(ctl_kind_.load());
  return m;
}

bool ProcessPipelineControl::refresh_metrics(std::string& err) {
  json result;
  if (!control_call("metrics.query", json{{"window_ms", 1000}}, result, err)) return false;
  if (!apply_metrics_json(result, /*from_uds=*/true)) {
    err = "metrics.query 的响应不是 JSON 对象";
    return false;
  }
  return true;
}

nlohmann::json ProcessPipelineControl::sessions_json(std::string& err) const {
  json result;
  if (!control_call("session.list", json::object(), result, err)) {
    // 取不到 ≠ 没有设备。err 由 control_call 写好（"控制连接不可用…"或 cued 的原文），
    // 这里只补一句"设备表因此未知"，避免上游把空表当成"设备全掉线"。
    if (!err.empty() && err.find("设备表") == std::string::npos) {
      err += "；设备表因此未知（不是「没有设备」）";
    }
    return empty_session_table();
  }
  // 形状不对也不猜：cued 与控制台版本不匹配时，宁可报错也不要渲染一张假表。
  if (!result.is_object() || !result.contains("sessions") || !result["sessions"].is_array()) {
    err = "session.list 的响应里没有 sessions 数组（cued 版本与控制台不匹配？）";
    return empty_session_table();
  }
  return result;
}

bool ProcessPipelineControl::set_param(const std::string& key, const std::string& value,
                                      std::string& err) {
  json result;
  if (!control_call("pipeline.set_param", json{{"key", key}, {"value", value}}, result, err)) {
    // 失败原因原样透出：cued 的校验文案（"gop 超出范围…"）比"内部错误"有用得多；
    // 控制面不可用时 control_call 已经在 err 里写明"降级 → 需要重启 cued"。
    return false;
  }
  // 成功：把生效后的编码档位镜像进 cfg_（下一次 spawn 的命令行参数）。
  if (result.contains("params") && result["params"].is_object()) {
    std::lock_guard<std::mutex> lk(cfg_mu_);
    apply_encoder_params_json(result["params"], cfg_);
  }
  // 这里**不发** pipeline.restart 事件：cued 侧会发 pipeline.param（带 encoder_rebuild），
  // 两处各发一条会让 agent 在事件流里看到两次"参数变了"。
  return true;
}

bool ProcessPipelineControl::inject_fault(const std::string& kind, double value,
                                          std::string& err) {
  // 四种注入对应四种不同的可观测量（real-faults 的五类里四类由此而来）：
  //   drop          出向丢包（%）：只涨 injected；丢弃发生在 frames_out 结算之后 → fps 不动
  //   ingress_drop  上行丢包（%）：帧重组超时 → dropped 涨、frames_out 掉
  //   rate          发送速率上限（Mbps）：令牌桶不足 → budget_drops 涨、帧被丢
  //   backlog       发送排队节奏（帧/s）：不丢包，队列积压 → frames_in−frames_out 涨
  //   delay / jitter 延迟注入（毫秒）：不丢包，delayed_pkts 涨、send 段分位涨，
  //                  injected / budget_drops 都不动（这是与丢包最直接的区分点）
  json result;
  if (!control_call("fault.set", json{{"kind", kind}, {"value", value}}, result, err)) {
    return false;
  }
  // respawn 是 cued 回传的"下次重启该用的档位"：注入是全局的（value=0 会一并清掉
  // rate/backlog），清除规则只允许有一份，所以镜像它而不是在这里重写一遍。
  if (result.contains("respawn") && result["respawn"].is_object()) {
    const json& r = result["respawn"];
    std::lock_guard<std::mutex> lk(cfg_mu_);
    if (r.contains("inject_drop") && r["inject_drop"].is_number()) {
      cfg_.inject_drop = r["inject_drop"].get<double>();
    }
    if (r.contains("inject_ingress_drop") && r["inject_ingress_drop"].is_number()) {
      cfg_.inject_ingress_drop = r["inject_ingress_drop"].get<double>();
    }
    if (r.contains("send_mbps") && r["send_mbps"].is_number()) {
      cfg_.send_mbps = r["send_mbps"].get<double>();
    }
    if (r.contains("queue_fps") && r["queue_fps"].is_number()) {
      cfg_.queue_fps = r["queue_fps"].get<double>();
    }
    if (r.contains("inject_delay_ms") && r["inject_delay_ms"].is_number()) {
      cfg_.inject_delay_ms = r["inject_delay_ms"].get<double>();
    }
    if (r.contains("inject_jitter_ms") && r["inject_jitter_ms"].is_number()) {
      cfg_.inject_jitter_ms = r["inject_jitter_ms"].get<double>();
    }
  }
  // fault.applied / fault.cleared 由 cued 自己发（它才知道注入到底生效了没有）；
  // 父进程不再补发，否则事件流里同一件事出现两次。
  return true;
}

std::string ProcessPipelineControl::observation(const std::string& source, int tail,
                                                std::string& err) const {
  if (source != "logs" && source != "procs" && source != "frames" && source != "events") {
    err = "unsupported source: " + source;
    return {};
  }
  json result;
  std::string cerr;
  const bool via_uds =
      control_call("obs.fetch", json{{"source", source}, {"tail", tail}}, result, cerr);
  if (via_uds && result.contains("text") && result["text"].is_string()) {
    return result["text"].get<std::string>();
  }
  // 降级：控制通道不可用时用 stdout 解析出来的本地缓存（数据仍然真实，只是不是实时的）。
  // 第一行必须写明"这一份走的是 stdout"——把缓存当实时快照是最容易骗过自己的那种报告，
  // 但**不能**因此返回空：运维在看进程还在不在的时候，旧数据也比没有强。
  std::string text = local_observation(source, tail);
  const std::string why =
      cerr.empty() ? std::string("obs.fetch 响应缺 text 字段") : cerr;
  return "control_channel=stdout reason=" + why + "\n" + text;
}

std::string ProcessPipelineControl::local_observation(const std::string& source, int tail) const {
  std::lock_guard<std::mutex> lk(mu_);
  const ProcessPipelineControl::Config cfg = cfg_snapshot();
  std::ostringstream oss;
  if (source == "procs") {
    // 注意：不暴露真实注入值——那是"标准答案"，agent 必须从指标推断
    // running=false + pid=0 是 cued 已不在的可靠信号（监督线程负责回收）
    const pid_t pid = pid_.load();
    oss << "cued pid=" << pid << " running=" << (pid != 0 ? "true" : "false")
        << " restarts=" << restarts_ << " transcode=" << (cfg.transcode ? "on" : "off")
        << " bitrate=" << cfg.bitrate_kbps << "kbps gop=" << cfg.gop;
    // 编码器实际档位：agent 判断「编码段慢」到底是 preset 太慢还是输出像素太多，
    // 必须能读到这两项（它们与 bitrate/gop 同属配置面，不是答案）。
    oss << " size="
        << (cfg.width > 0 ? std::to_string(cfg.width) + "x" + std::to_string(cfg.height)
                          : std::string("follow"))
        << " preset=" << cfg.preset;
    return oss.str();
  }
  if (source == "events") {
    if (events_.empty()) return {};
    const std::size_t want = static_cast<std::size_t>(tail <= 0 ? 20 : tail);
    const std::size_t n = std::min(want, events_.size());
    for (std::size_t i = events_.size() - n; i < events_.size(); ++i) {
      const auto& e = events_[i];
      oss << "ev=" << e.type << " t_us=" << e.t_us << " data=" << e.data.dump() << '\n';
    }
    return oss.str();
  }
  if (source == "frames") {
    oss << "frames_in=" << metrics_.frames_in << " frames_out=" << metrics_.frames_out
        << " dropped=" << metrics_.dropped << " injected=" << metrics_.injected
        << " decoded=" << metrics_.decoded << " decode_errors=" << metrics_.decode_errors;
    // media 元信息：来自 cued 缓存的 codec_config（0x03）。没有收到过 0x03 时整段不出现，
    // 而不是编一个空 SPS。
    if (!sps_hex_.empty() || !pps_hex_.empty() || !codec_raw_hex_.empty()) {
      oss << " codec=" << codec_name_;
      if (!sps_hex_.empty()) oss << " sps=" << sps_hex_;
      if (!pps_hex_.empty()) oss << " pps=" << pps_hex_;
      if (!codec_raw_hex_.empty()) oss << " codec_raw=" << codec_raw_hex_;
    }
    return oss.str();
  }
  if (log_tail_.empty()) return {};
  const int n = std::max(1, std::min<int>(tail <= 0 ? 20 : tail, static_cast<int>(log_tail_.size())));
  // 空 log_tail_ 时 size-n 会变成 -1（越界读，ASan 实测 SEGV）
  const int begin = std::max(0, static_cast<int>(log_tail_.size()) - n);
  for (int i = begin; i < static_cast<int>(log_tail_.size()); ++i) {
    oss << log_tail_[static_cast<std::size_t>(i)] << '\n';
  }
  return oss.str();
}

}  // namespace cue::pipeline
