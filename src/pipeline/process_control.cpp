#include "cue/pipeline/process_control.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
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

bool extract_double(const std::string& line, const char* key, double& out) {
  const std::string pat = std::string(key) + "=";
  auto pos = line.find(pat);
  if (pos == std::string::npos) return false;
  pos += pat.size();
  try {
    out = std::stod(line.substr(pos));
  } catch (...) {
    return false;
  }
  return true;
}

bool extract_u64(const std::string& line, const char* key, uint64_t& out) {
  const std::string pat = std::string(key) + "=";
  auto pos = line.find(pat);
  if (pos == std::string::npos) return false;
  pos += pat.size();
  try {
    out = std::stoull(line.substr(pos));
  } catch (...) {
    return false;
  }
  return true;
}

// 解析 "decode=1.030/1.441/13.940" 这类三分位
bool extract_triplet(const std::string& line, const char* key, double* p50, double* p95, double* p99) {
  const std::string pat = std::string(key) + "=";
  auto pos = line.find(pat);
  if (pos == std::string::npos) return false;
  pos += pat.size();
  const auto slash1 = line.find('/', pos);
  if (slash1 == std::string::npos) return false;
  const auto slash2 = line.find('/', slash1 + 1);
  if (slash2 == std::string::npos) return false;
  try {
    if (p50) *p50 = std::stod(line.substr(pos, slash1 - pos));
    if (p95) *p95 = std::stod(line.substr(slash1 + 1, slash2 - slash1 - 1));
    if (p99) *p99 = std::stod(line.substr(slash2 + 1));
  } catch (...) {
    return false;
  }
  return true;
}

}  // namespace

ProcessPipelineControl::ProcessPipelineControl(Config cfg) : cfg_(std::move(cfg)) {}
ProcessPipelineControl::~ProcessPipelineControl() { stop(); }

bool ProcessPipelineControl::spawn(std::string& err) {
  int fds[2];
  if (::pipe(fds) != 0) {
    err = std::string("pipe: ") + std::strerror(errno);
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
    return false;
  }
  if (pid == 0) {
    ::dup2(fds[1], STDOUT_FILENO);
    ::dup2(fds[1], STDERR_FILENO);
    ::close(fds[0]);
    ::close(fds[1]);
    ::execv(cfg_.cued_bin.c_str(), argv.data());
    _exit(127);
  }
  ::close(fds[1]);
  log_fd_ = fds[0];
  pid_ = pid;
  stop_reader_.store(false);
  reader_ = std::thread([this] { reader_loop(); });
  return true;
}

bool ProcessPipelineControl::start(std::string& err) {
  if (pid_ != 0) return true;
  return spawn(err);
}

void ProcessPipelineControl::stop() {
  if (pid_ == 0) return;
  ::kill(pid_, SIGTERM);
  for (int i = 0; i < 50; ++i) {  // 最多等 500ms
    int status = 0;
    const pid_t r = ::waitpid(pid_, &status, WNOHANG);
    if (r == pid_) break;
    ::usleep(10 * 1000);
  }
  if (::waitpid(pid_, nullptr, WNOHANG) == 0) {
    ::kill(pid_, SIGKILL);
    ::waitpid(pid_, nullptr, 0);
  }
  pid_ = 0;
  stop_reader_.store(true);
  if (log_fd_ >= 0) {
    ::close(log_fd_);
    log_fd_ = -1;
  }
  if (reader_.joinable()) reader_.join();
}

void ProcessPipelineControl::reader_loop() {
  std::string buf;
  char tmp[1024];
  while (!stop_reader_.load()) {
    const ssize_t n = ::read(log_fd_, tmp, sizeof(tmp));
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
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
  if (line.find("[cued] fps=") != std::string::npos) {
    PipelineMetrics m;
    extract_double(line, "fps", m.fps);
    extract_u64(line, "in", m.frames_in);
    extract_u64(line, "out", m.frames_out);
    extract_u64(line, "drop", m.dropped);
    extract_u64(line, "inj", m.injected);
    extract_u64(line, "dec", m.decoded);
    extract_u64(line, "dec_err", m.decode_errors);
    extract_triplet(line, "recv", &m.recv_p50, nullptr, nullptr);
    extract_triplet(line, "buffer", &m.buffer_p50, nullptr, nullptr);
    extract_triplet(line, "decode", &m.decode_p50, nullptr, &m.decode_p99);
    extract_triplet(line, "op", &m.op_p50, nullptr, &m.op_p99);
    extract_triplet(line, "send", &m.send_p50, nullptr, nullptr);
    m.updated_us = now_us();
    std::lock_guard<std::mutex> lk(mu_);
    m.samples = metrics_.samples + 1;
    metrics_ = m;
    return;
  }
  std::lock_guard<std::mutex> lk(mu_);
  log_tail_.push_back(line);
  if (log_tail_.size() > 200) log_tail_.erase(log_tail_.begin());
}

PipelineMetrics ProcessPipelineControl::latest() const {
  std::lock_guard<std::mutex> lk(mu_);
  return metrics_;
}

bool ProcessPipelineControl::set_param(const std::string& key, const std::string& value, std::string& err) {
  Config next = cfg_;
  try {
    if (key == "bitrate_kbps") next.bitrate_kbps = std::stoi(value);
    else if (key == "gop") next.gop = std::stoi(value);
    else if (key == "preset") next.preset = value;
    else if (key == "transcode") next.transcode = (value == "on" || value == "true" || value == "1");
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
    oss << "cued pid=" << pid_ << " running=" << (pid_ != 0 ? "true" : "false")
        << " transcode=" << (cfg_.transcode ? "on" : "off") << " bitrate=" << cfg_.bitrate_kbps
        << "kbps gop=" << cfg_.gop;
    return oss.str();
  }
  if (source == "frames") {
    oss << "frames_in=" << metrics_.frames_in << " frames_out=" << metrics_.frames_out
        << " dropped=" << metrics_.dropped << " injected=" << metrics_.injected
        << " decoded=" << metrics_.decoded << " decode_errors=" << metrics_.decode_errors;
    return oss.str();
  }
  const int n = std::max(1, std::min<int>(tail <= 0 ? 20 : tail, static_cast<int>(log_tail_.size())));
  const int start = static_cast<int>(log_tail_.size()) - n;
  for (int i = start; i < static_cast<int>(log_tail_.size()); ++i) oss << log_tail_[i] << '\n';
  return oss.str();
}

}  // namespace cue::pipeline
