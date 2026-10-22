#include "cue/pipeline/control_channel.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <utility>

namespace cue::pipeline {

namespace {

using json = nlohmann::json;

// 读超时用单调时钟：控制通道的超时不能用墙钟（NTP 回拨会让 deadline 永远不到）。
int64_t mono_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int remaining_ms(int64_t deadline) {
  const int64_t left = deadline - mono_ms();
  if (left <= 0) return 0;
  return static_cast<int>(std::min<int64_t>(left, 60'000));
}

std::string errno_text(const char* what) {
  return std::string(what) + ": " + std::strerror(errno);
}

// 目录私有性：控制 socket 的父目录不能被别人写，否则别人可以换掉 socket 文件
// （哪怕只是 rename 掉再绑自己的），控制面就等于对全机器开放。
//
// 规则：
//  - 缺的层级按 0700 建，并显式 chmod（umask 会把 mkdir 的模式再削一刀，
//    「只有我能进」不能依赖调用方 umask）；
//  - 必须是我拥有的目录，且 group/other 位全为 0；
//  - 父目录（例如 /tmp）如果 group/other 可写，必须带 sticky 位（1777 的 /tmp 合格）；
//    没有 sticky 的可写父目录意味着谁都能把我们的目录改名挪走。
bool ensure_private_dir(const std::string& dir, std::string& err) {
  if (dir.empty()) {
    err = "控制 socket 目录为空";
    return false;
  }
  // 逐级创建（不依赖 std::filesystem：这里要在失败时拿到精确的 errno 与层级）
  for (std::size_t i = 1; i <= dir.size(); ++i) {
    if (i != dir.size() && dir[i] != '/') continue;
    const std::string prefix = dir.substr(0, i);
    struct stat st {};
    if (::lstat(prefix.c_str(), &st) == 0) {
      if (S_ISLNK(st.st_mode)) {
        err = "控制 socket 路径上的 " + prefix + " 是符号链接（拒绝：链接可被替换）";
        return false;
      }
      if (!S_ISDIR(st.st_mode)) {
        err = "控制 socket 路径上的 " + prefix + " 不是目录";
        return false;
      }
      continue;
    }
    if (errno != ENOENT) {
      err = errno_text(("lstat " + prefix).c_str());
      return false;
    }
    if (::mkdir(prefix.c_str(), 0700) != 0) {
      err = errno_text(("mkdir " + prefix).c_str());
      return false;
    }
    // mkdir 的模式会被 umask 削，这里补一刀明确的权限
    if (::chmod(prefix.c_str(), 0700) != 0) {
      err = errno_text(("chmod " + prefix).c_str());
      return false;
    }
  }
  struct stat st {};
  if (::lstat(dir.c_str(), &st) != 0) {
    err = errno_text(("lstat " + dir).c_str());
    return false;
  }
  if (st.st_uid != ::geteuid()) {
    err = "控制 socket 目录 " + dir + " 属于 uid " + std::to_string(st.st_uid) +
          "（不是当前用户 " + std::to_string(::geteuid()) + "），拒绝在其中放控制通道";
    return false;
  }
  if ((st.st_mode & 077) != 0) {
    err = "控制 socket 目录 " + dir + " 权限过宽（" + std::to_string(st.st_mode & 0777) +
          "）：必须 0700，否则其他用户可以抢绑/替换控制 socket";
    return false;
  }
  // 父目录：只需要「不能被别人随意改名我们的目录」
  const std::size_t slash = dir.find_last_of('/');
  if (slash != std::string::npos && slash > 0) {
    const std::string parent = dir.substr(0, slash);
    struct stat pst {};
    if (::lstat(parent.c_str(), &pst) == 0 && S_ISDIR(pst.st_mode)) {
      const bool writable_by_others = (pst.st_mode & 0022) != 0;
      const bool sticky = (pst.st_mode & 01000) != 0;
      if (writable_by_others && !sticky) {
        err = "控制 socket 的父目录 " + parent +
              " 可被其他用户写且没有 sticky 位（拒绝：别人能把目录挪走）";
        return false;
      }
    }
  }
  return true;
}

std::string dirname_of(const std::string& path) {
  const std::size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) return ".";
  if (slash == 0) return "/";
  return path.substr(0, slash);
}

bool is_seqpacket(ControlChannelKind k) { return k == ControlChannelKind::SeqPacket; }

// 建 socket：优先 SEQPACKET，拿不到就退 STREAM 并把原因交给调用方（不能无声降级）。
int make_socket(ControlChannelKind preferred, ControlChannelKind& got, std::string& fallback) {
  const int base = SOCK_CLOEXEC;
  if (preferred == ControlChannelKind::Stream) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | base, 0);
    if (fd < 0) return -1;
    got = ControlChannelKind::Stream;
    return fd;
  }
  int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | base, 0);
  if (fd >= 0) {
    got = ControlChannelKind::SeqPacket;
    return fd;
  }
  const std::string why = errno_text("socket(AF_UNIX, SOCK_SEQPACKET)");
  fd = ::socket(AF_UNIX, SOCK_STREAM | base, 0);
  if (fd < 0) return -1;
  got = ControlChannelKind::Stream;
  fallback = why + " → 退 SOCK_STREAM + JSON-line";
  return fd;
}

bool fill_addr(sockaddr_un& addr, const std::string& path, std::string& err) {
  addr = sockaddr_un{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    err = "控制 socket 路径过长（" + std::to_string(path.size()) + " > " +
          std::to_string(sizeof(addr.sun_path) - 1) + "）：" + path;
    return false;
  }
  std::memcpy(addr.sun_path, path.data(), path.size());
  return true;
}

}  // namespace

const char* control_channel_tag(ControlChannelKind kind) {
  switch (kind) {
    case ControlChannelKind::SeqPacket:
    case ControlChannelKind::Stream:
      return "uds";
    case ControlChannelKind::None:
      break;
  }
  return "stdout";
}

const char* control_channel_detail(ControlChannelKind kind) {
  switch (kind) {
    case ControlChannelKind::SeqPacket: return "uds:seqpacket";
    case ControlChannelKind::Stream: return "uds:stream";
    case ControlChannelKind::None: break;
  }
  return "stdout";
}

json make_request(std::int64_t id, std::string_view op, const json& args) {
  // 键序固定 id/op/args：人读日志与抓包对照时不用猜顺序
  nlohmann::ordered_json j{{"id", id}, {"op", std::string(op)}};
  j["args"] = args.is_object() ? args : json::object();
  return j;
}

json make_ok(std::int64_t id, const json& result) {
  return json{{"id", id}, {"ok", true}, {"result", result.is_object() ? result : json::object()}};
}

json make_error(std::int64_t id, std::string_view error) {
  return json{{"id", id}, {"ok", false}, {"error", std::string(error)}};
}

bool parse_request(const json& j, std::int64_t& id, std::string& op, json& args,
                   std::string& err) {
  id = 0;
  args = json::object();
  if (!j.is_object()) {
    err = "请求不是 JSON 对象";
    return false;
  }
  const auto id_it = j.find("id");
  if (id_it == j.end()) {
    err = "请求缺 id（响应无法配对）";
    return false;
  }
  if (id_it->is_number_integer()) {
    id = id_it->get<std::int64_t>();
  } else if (!id_it->is_null()) {
    err = "请求的 id 不是整数";
    return false;
  }
  const auto op_it = j.find("op");
  if (op_it == j.end() || !op_it->is_string()) {
    err = "请求缺 op（字符串）";
    return false;
  }
  op = op_it->get<std::string>();
  if (op.empty()) {
    err = "请求的 op 为空";
    return false;
  }
  if (const auto a = j.find("args"); a != j.end()) {
    if (!a->is_object()) {
      err = "请求的 args 不是对象";
      return false;
    }
    args = *a;
  }
  return true;
}

// ---------------------------------------------------------------- 连接

struct ControlServer::Conn final : ControlSession {
  ControlServer* server = nullptr;
  int fd = -1;
  ControlChannelKind kind = ControlChannelKind::None;
  std::mutex write_mu;  // 响应（读线程）与订阅推送（媒体线程）共用一个 fd
  std::atomic<bool> closed{false};
  // 订阅状态：只被连接自己的读线程与 publish（媒体线程）访问
  mutable std::mutex sub_mu;
  bool sub_active = false;
  std::vector<std::string> sub_events;
  // stream 模式的分帧残留
  std::string rbuf;

  void shutdown() {
    if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
  }

  void subscribe(std::vector<std::string> events) override {
    std::lock_guard<std::mutex> lk(sub_mu);
    sub_active = true;
    sub_events = std::move(events);
  }
  void unsubscribe() override {
    std::lock_guard<std::mutex> lk(sub_mu);
    sub_active = false;
    sub_events.clear();
  }
  bool subscribed() const override {
    std::lock_guard<std::mutex> lk(sub_mu);
    return sub_active;
  }
  bool push(const json& msg) override;
};

bool ControlServer::Conn::push(const json& msg) {
  if (closed.load()) return false;
  std::string body = msg.dump();
  if (body.size() > kMaxControlMessage) return false;  // 推送超限：丢掉而不是发半条
  std::lock_guard<std::mutex> lk(write_mu);
  if (kind == ControlChannelKind::Stream) body.push_back('\n');
  std::size_t off = 0;
  while (off < body.size()) {
    const ssize_t n = ::send(fd, body.data() + off, body.size() - off, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n > 0) {
      off += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    // EAGAIN/EPIPE 都算「这条推送没送到」：订阅者跟不上时丢事件，
    // 媒体线程绝不能因为一个慢客户端停在这里。
    closed.store(true);
    return false;
  }
  return true;
}

bool ControlServer::write_reply(const std::shared_ptr<Conn>& conn, const json& j) {
  std::string body = j.dump();
  if (body.size() > kMaxControlMessage) {
    // 响应超限：换成一条明确的错误（保留 id，客户端还能配对）
    body = make_error(j.value("id", static_cast<std::int64_t>(0)),
                      "response too large: " + std::to_string(body.size()) + " bytes > " +
                          std::to_string(kMaxControlMessage))
               .dump();
  }
  if (is_seqpacket(conn->kind)) {
    // 一条消息一次 send（不含换行）：SEQPACKET 的消息边界就是帧边界
  } else {
    body.push_back('\n');
  }
  std::lock_guard<std::mutex> lk(conn->write_mu);
  std::size_t off = 0;
  while (off < body.size()) {
    const ssize_t n = ::send(conn->fd, body.data() + off, body.size() - off, MSG_NOSIGNAL);
    if (n > 0) {
      off += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

ControlServer::ControlServer(std::string socket_path, Handler handler,
                            ControlChannelKind preferred)
    : path_(std::move(socket_path)), handler_(std::move(handler)), preferred_(preferred) {}

bool ControlServer::start(std::string& err) {
  if (fd_ >= 0) {
    err = "控制服务端已启动";
    return false;
  }
  if (path_.empty()) {
    err = "控制 socket 路径为空";
    return false;
  }
  if (!ensure_private_dir(dirname_of(path_), err)) return false;

  ControlChannelKind got = ControlChannelKind::None;
  const int fd = make_socket(preferred_, got, fallback_reason_);
  if (fd < 0) {
    err = errno_text("socket(AF_UNIX)");
    return false;
  }
  sockaddr_un addr{};
  if (!fill_addr(addr, path_, err)) {
    ::close(fd);
    return false;
  }
  // 上一代同路径 socket（正常不会发生：目录按 pid 分），先清掉再绑，避免 EADDRINUSE
  ::unlink(path_.c_str());
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    err = errno_text(("bind " + path_).c_str());
    ::close(fd);
    return false;
  }
  // 只允许当前用户连：目录 0700 已经挡住了其他用户，socket 自己再收一道
  if (::chmod(path_.c_str(), 0600) != 0) {
    err = errno_text(("chmod " + path_).c_str());
    ::close(fd);
    ::unlink(path_.c_str());
    return false;
  }
  if (::listen(fd, 16) != 0) {
    err = errno_text(("listen " + path_).c_str());
    ::close(fd);
    ::unlink(path_.c_str());
    return false;
  }
  fd_ = fd;
  kind_ = got;
  stop_.store(false);
  accept_thread_ = std::thread([this] { accept_loop(); });
  return true;
}

void ControlServer::accept_loop() {
  for (;;) {
    sockaddr_un peer{};
    socklen_t plen = sizeof(peer);
    const int cfd = ::accept4(fd_, reinterpret_cast<sockaddr*>(&peer), &plen, SOCK_CLOEXEC);
    if (cfd < 0) {
      if (errno == EINTR) continue;
      // stop() 用 shutdown 叫醒 accept：此刻必须退出，而不是把它当成错误重试
      if (stop_.load()) break;
      if (errno == EBADF || errno == EINVAL) break;
      continue;
    }
    auto conn = std::make_shared<Conn>();
    conn->server = this;
    conn->fd = cfd;
    conn->kind = kind_;
    active_.fetch_add(1);
    {
      std::lock_guard<std::mutex> lk(conns_mu_);
      conns_.push_back(conn);
    }
    // 每连接一个线程：连接数是个位数，换掉「事件循环 + 每连接状态机」的复杂度
    std::thread([this, conn] {
      serve(conn);
      detach_conn(conn);
      if (active_.fetch_sub(1) == 1) {
        std::lock_guard<std::mutex> lk(idle_mu_);
        idle_cv_.notify_all();
      }
    }).detach();
  }
}

void ControlServer::detach_conn(const std::shared_ptr<Conn>& conn) {
  conn->closed.store(true);
  {
    std::lock_guard<std::mutex> lk(conns_mu_);
    std::erase(conns_, conn);
  }
  if (conn->fd >= 0) {
    ::close(conn->fd);
    conn->fd = -1;
  }
}

void ControlServer::serve(const std::shared_ptr<Conn>& conn) {
  std::vector<char> buf(kMaxControlMessage + 1);
  for (;;) {
    if (stop_.load()) break;
    ssize_t n = 0;
    if (is_seqpacket(conn->kind)) {
      // MSG_TRUNC：消息比缓冲区大时也返回真实长度（本机内核实测支持），
      // 这样才能把「超 64 KiB」与「恰好 64 KiB」区分开并明确拒绝。
      n = ::recv(conn->fd, buf.data(), buf.size(), MSG_TRUNC);
      if (n < 0) {
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
        break;
      }
      if (n == 0) break;  // 对端关闭
      if (static_cast<std::size_t>(n) > kMaxControlMessage) {
        // 超限：拒绝这一条，但**不断连接**（断开会让客户端把「消息太大」
        // 误读成「控制通道挂了」）。id 无从得知，回 0。
        write_reply(conn, make_error(0, "message too large: " + std::to_string(n) +
                                            " bytes > " + std::to_string(kMaxControlMessage) +
                                            "（连接保留，可继续发合法请求）"));
        continue;
      }
      handle_message(conn, std::string_view(buf.data(), static_cast<std::size_t>(n)));
      continue;
    }
    // STREAM：换行分帧。单行超限 → 回报错误并丢弃到下一个换行（重新对齐帧边界）
    n = ::recv(conn->fd, buf.data(), std::min<std::size_t>(buf.size(), 4096), 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
      break;
    }
    if (n == 0) break;  // EOF
    if (conn->rbuf.size() + static_cast<std::size_t>(n) > kMaxControlMessage &&
        conn->rbuf.find('\n') == std::string::npos) {
      conn->rbuf.clear();
      write_reply(conn, make_error(0, "message too large: > " +
                                          std::to_string(kMaxControlMessage) +
                                          " bytes（连接保留，已丢弃到下一个换行）"));
      // 后面的半条请求会作为一条坏 JSON 再被拒一次——这是有意的：宁可多拒，
      // 也不能把半条请求拼给下一条。
      continue;
    }
    conn->rbuf.append(buf.data(), static_cast<std::size_t>(n));
    std::size_t pos = 0;
    while ((pos = conn->rbuf.find('\n')) != std::string::npos) {
      const std::string line = conn->rbuf.substr(0, pos);
      conn->rbuf.erase(0, pos + 1);
      if (line.empty()) continue;  // 容忍空行（不是帧）
      if (line.size() > kMaxControlMessage) {
        write_reply(conn, make_error(0, "message too large: " + std::to_string(line.size()) +
                                            " bytes > " + std::to_string(kMaxControlMessage)));
        continue;
      }
      handle_message(conn, line);
    }
  }
  conn->closed.store(true);
  conn->shutdown();
}

void ControlServer::handle_message(const std::shared_ptr<Conn>& conn, std::string_view raw) {
  json reply;
  json req = json::parse(raw.begin(), raw.end(), nullptr, false);
  if (req.is_discarded()) {
    reply = make_error(0, "请求不是合法 JSON");
    write_reply(conn, reply);
    return;
  }
  std::int64_t id = 0;
  std::string op;
  json args;
  std::string perr;
  if (!parse_request(req, id, op, args, perr)) {
    write_reply(conn, make_error(id, perr));
    return;
  }
  try {
    reply = handler_ ? handler_(req, *conn) : make_error(id, "控制服务端没有 handler");
  } catch (const std::exception& e) {
    // handler 里的任何异常都不能带走连接：一条坏请求换来一条 ok:false 才对。
    reply = make_error(id, std::string("处理请求时异常: ") + e.what());
  }
  write_reply(conn, reply);
}

void ControlServer::stop() {
  const bool owned = fd_ >= 0;  // 只有真的 bind 成功过才清理 socket 文件
  stop_.store(true);
  if (fd_ >= 0) {
    ::shutdown(fd_, SHUT_RDWR);  // 叫醒 accept
  }
  if (accept_thread_.joinable()) accept_thread_.join();
  {
    std::lock_guard<std::mutex> lk(conns_mu_);
    for (auto& c : conns_) c->shutdown();  // 叫醒每条连接的 recv
  }
  // 等连接线程收尾。这里**无限等**而不是超时：连接线程持有 conns_mu_ 与 this，
  // 超时返回等于把 ControlServer 的内存交给一个还在跑的线程（UAF）。
  // 它们的 recv 已被 shutdown 叫醒，handler 都是纯内存操作，所以等的是毫秒级。
  {
    std::unique_lock<std::mutex> lk(idle_mu_);
    idle_cv_.wait(lk, [this] { return active_.load() == 0; });
  }
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  if (owned) {
    // 自己建的 socket 自己清：/tmp/cue/<pid>/ 里只留这一个文件，顺手把目录删掉。
    // 删不掉（还有别的文件/别人在用）也不报错——pid 目录天然是一次性的。
    ::unlink(path_.c_str());
    ::rmdir(dirname_of(path_).c_str());
  }
  kind_ = ControlChannelKind::None;
}

std::size_t ControlServer::publish(const json& body) {
  if (stop_.load()) return 0;
  if (!body.is_object() || !body.contains("event")) return 0;
  std::vector<std::shared_ptr<Conn>> targets;
  {
    std::lock_guard<std::mutex> lk(conns_mu_);
    targets = conns_;
  }
  const std::string ev = body["event"].is_string() ? body["event"].get<std::string>() : "";
  std::size_t sent = 0;
  std::vector<std::shared_ptr<Conn>> dead;
  for (auto& c : targets) {
    if (!c->subscribed()) continue;
    // 订阅过滤：sub_events 非空时只推列出的 ev（客户端只关心 metric.sample 时
    // 不必被 codec.config 之类刷屏）
    bool want = true;
    {
      std::lock_guard<std::mutex> lk(c->sub_mu);
      if (!c->sub_events.empty()) {
        want = std::find(c->sub_events.begin(), c->sub_events.end(), ev) != c->sub_events.end();
      }
    }
    if (!want) continue;
    if (c->push(body)) {
      ++sent;
    } else {
      dead.push_back(c);
    }
  }
  // 推失败（EPIPE / EAGAIN）＝ 这个订阅者跟不上：叫醒它的读线程收尾，
  // 但绝不在这里等它——发布者是媒体线程。
  for (auto& c : dead) c->shutdown();
  return sent;
}

ControlServer::~ControlServer() { stop(); }

// ---------------------------------------------------------------- 客户端

ControlClient::~ControlClient() { close(); }

bool ControlClient::connect(const std::string& path, int timeout_ms, std::string& err,
                            ControlChannelKind preferred) {
  close();
  // 一次尝试：成功则接管 fd_/kind_ 并返回 0；失败返回 errno（供上层判断要不要换类型）。
  auto attempt = [&](ControlChannelKind want, std::string& aerr) -> int {
    ControlChannelKind got = ControlChannelKind::None;
    std::string fallback;
    const int fd = make_socket(want, got, fallback);
    if (fd < 0) {
      aerr = errno_text("socket(AF_UNIX)");
      return errno;
    }
    sockaddr_un addr{};
    if (!fill_addr(addr, path, aerr)) {
      ::close(fd);
      return EINVAL;
    }
    // 非阻塞 connect + poll：阻塞 connect 在 UDS 上通常瞬间返回，
    // 但没有监听者时也可能等到超时才失败，控制面不能被这种等待吃掉预算。
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    int e = rc == 0 ? 0 : errno;
    if (rc != 0 && e == EINPROGRESS) {
      pollfd pfd{fd, POLLOUT, 0};
      const int pr = ::poll(&pfd, 1, std::max(timeout_ms, 1));
      if (pr == 0) {
        e = ETIMEDOUT;
      } else if (pr < 0) {
        e = errno;
      } else {
        int soerr = 0;
        socklen_t sl = sizeof(soerr);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) != 0) {
          e = errno;
        } else {
          e = soerr;
        }
      }
    }
    if (e != 0) {
      aerr = std::string("connect ") + path + ": " + std::strerror(e);
      ::close(fd);
      return e;
    }
    if (flags >= 0) (void)::fcntl(fd, F_SETFL, flags);
    fd_ = fd;
    kind_ = got;
    broken_reason_.clear();
    rbuf_.clear();
    return 0;
  };

  // 先试首选类型；对端用的是另一种时 connect 返回 EPROTOTYPE——这时换一种再试一次。
  // cued 在 SEQPACKET 不可用时会降级到 STREAM，客户端不能因为"猜错了类型"就
  // 报"控制通道不可用"（那会把一次可恢复的降级变成永远不可用）。
  const ControlChannelKind kinds[2] = {
      preferred, preferred == ControlChannelKind::SeqPacket ? ControlChannelKind::Stream
                                                            : ControlChannelKind::SeqPacket};
  std::string first_err;
  for (const auto kind : kinds) {
    std::string aerr;
    const int e = attempt(kind, aerr);
    if (e == 0) return true;
    if (first_err.empty()) first_err = aerr;
    if (e != EPROTOTYPE && e != ESOCKTNOSUPPORT) break;
  }
  err = first_err;
  return false;
}

void ControlClient::close() {
  std::lock_guard<std::mutex> lk(mu_);
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  kind_ = ControlChannelKind::None;
  rbuf_.clear();
}

void ControlClient::mark_broken(const std::string& why) {
  if (!broken_reason_.empty()) return;
  broken_reason_ = why;
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  kind_ = ControlChannelKind::None;
  rbuf_.clear();
}

void ControlClient::set_event_callback(std::function<void(const json&)> cb) {
  std::lock_guard<std::mutex> lk(mu_);
  on_event_ = std::move(cb);
}

bool ControlClient::write_msg(const json& j, int timeout_ms, std::string& err) {
  std::string body = j.dump();
  if (body.size() > kMaxControlMessage) {
    err = "请求超长（" + std::to_string(body.size()) + " > " + std::to_string(kMaxControlMessage) +
          "）：控制面不允许发这么大的请求";
    return false;
  }
  if (!is_seqpacket(kind_)) body.push_back('\n');
  const int64_t deadline = mono_ms() + std::max(timeout_ms, 1);
  std::size_t off = 0;
  while (off < body.size()) {
    if (remaining_ms(deadline) == 0) {
      err = "写控制请求超时";
      return false;
    }
    pollfd pfd{fd_, POLLOUT, 0};
    const int pr = ::poll(&pfd, 1, remaining_ms(deadline));
    if (pr == 0) {
      err = "写控制请求超时";
      return false;
    }
    if (pr < 0) {
      if (errno == EINTR) continue;
      err = errno_text("poll(POLLOUT)");
      return false;
    }
    const ssize_t n = ::send(fd_, body.data() + off, body.size() - off, MSG_NOSIGNAL);
    if (n > 0) {
      off += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && (errno == EINTR)) continue;
    err = errno_text("send");
    return false;
  }
  return true;
}

bool ControlClient::read_msg(std::string& out, int timeout_ms, std::string& err) {
  const int64_t deadline = mono_ms() + std::max(timeout_ms, 1);
  std::vector<char> buf(kMaxControlMessage + 1);
  for (;;) {
    if (is_seqpacket(kind_)) {
      // 已有完整消息在缓冲里？SEQPACKET 一次 recv 就是一条，不需要拼接。
      const int pr = remaining_ms(deadline);
      if (pr == 0) {
        err = "等控制响应超时";
        return false;
      }
      pollfd pfd{fd_, POLLIN, 0};
      const int r = ::poll(&pfd, 1, pr);
      if (r == 0) {
        err = "等控制响应超时";
        return false;
      }
      if (r < 0) {
        if (errno == EINTR) continue;
        err = errno_text("poll(POLLIN)");
        return false;
      }
      const ssize_t n = ::recv(fd_, buf.data(), buf.size(), MSG_TRUNC);
      if (n < 0) {
        if (errno == EINTR) continue;
        err = errno_text("recv");
        return false;
      }
      if (n == 0) {
        err = "控制连接被对端关闭";
        return false;
      }
      if (static_cast<std::size_t>(n) > kMaxControlMessage) {
        err = "控制响应超长（" + std::to_string(n) + " > " + std::to_string(kMaxControlMessage) +
              "）";
        return false;
      }
      out.assign(buf.data(), static_cast<std::size_t>(n));
      return true;
    }
    // STREAM：先看缓冲里有没有现成的整行，没有就去读
    const std::size_t pos = rbuf_.find('\n');
    if (pos != std::string::npos) {
      out = rbuf_.substr(0, pos);
      rbuf_.erase(0, pos + 1);
      if (out.empty()) continue;
      return true;
    }
    const int pr = remaining_ms(deadline);
    if (pr == 0) {
      err = "等控制响应超时";
      return false;
    }
    pollfd pfd{fd_, POLLIN, 0};
    const int r = ::poll(&pfd, 1, pr);
    if (r == 0) {
      err = "等控制响应超时";
      return false;
    }
    if (r < 0) {
      if (errno == EINTR) continue;
      err = errno_text("poll(POLLIN)");
      return false;
    }
    const ssize_t n = ::recv(fd_, buf.data(), std::min<std::size_t>(buf.size(), 4096), 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      err = errno_text("recv");
      return false;
    }
    if (n == 0) {
      err = "控制连接被对端关闭";
      return false;
    }
    rbuf_.append(buf.data(), static_cast<std::size_t>(n));
    if (rbuf_.size() > kMaxControlMessage && rbuf_.find('\n') == std::string::npos) {
      err = "控制响应超长（单行 > " + std::to_string(kMaxControlMessage) + " 字节）";
      return false;
    }
  }
}

bool ControlClient::request(const json& req, json& resp, std::string& err, int timeout_ms) {
  std::lock_guard<std::mutex> lk(mu_);
  if (fd_ < 0) {
    err = broken_reason_.empty() ? "控制连接未建立" : "控制连接不可用：" + broken_reason_;
    return false;
  }
  const std::int64_t want = req.contains("id") && req["id"].is_number_integer()
                                ? req["id"].get<std::int64_t>()
                                : 0;
  sent_.fetch_add(1);
  if (!write_msg(req, timeout_ms, err)) {
    mark_broken(err);
    return false;
  }
  const int64_t deadline = mono_ms() + std::max(timeout_ms, 1);
  for (;;) {
    const int left = remaining_ms(deadline);
    if (left == 0) {
      err = "等控制响应超时（" + std::to_string(timeout_ms) + " ms）";
      return false;
    }
    std::string raw;
    if (!read_msg(raw, left, err)) {
      // 读失败（EOF / 超时 / 超长）一律视为通道不可用：请求-响应是严格配对的，
      // 这一条响应没拿到，下一条就可能与迟到的响应错配。
      mark_broken(err);
      return false;
    }
    json msg = json::parse(raw.begin(), raw.end(), nullptr, false);
    if (msg.is_discarded() || !msg.is_object()) {
      // 坏报文不能当成"这次请求失败"就完事：它让 id 配对失准，连接不可信了
      mark_broken("控制响应不是合法 JSON 对象");
      err = broken_reason_;
      return false;
    }
    // 订阅推送：没有 id 键 = 服务端主动事件，交给回调，继续等自己的响应
    if (!msg.contains("id")) {
      if (msg.contains("event") && on_event_) on_event_(msg);
      continue;
    }
    const std::int64_t got = msg["id"].is_number_integer() ? msg["id"].get<std::int64_t>() : 0;
    if (got != want) {
      // id 不配对说明这条响应属于别的请求（或者服务端回错了）：协议层面失步
      mark_broken("控制响应 id 不配对（期望 " + std::to_string(want) + "，收到 " +
                  std::to_string(got) + "）");
      err = broken_reason_;
      return false;
    }
    resp = std::move(msg);
    return true;
  }
}

bool ControlClient::call(std::string_view op, const json& args, json& result, std::string& err,
                         int timeout_ms) {
  std::int64_t id = 0;
  {
    std::lock_guard<std::mutex> lk(mu_);
    id = next_id_++;
  }
  json resp;
  if (!request(make_request(id, op, args), resp, err, timeout_ms)) return false;
  const auto ok_it = resp.find("ok");
  if (ok_it == resp.end() || !ok_it->is_boolean()) {
    err = "控制响应缺 ok 字段";
    return false;
  }
  if (!ok_it->get<bool>()) {
    const auto e = resp.find("error");
    err = (e != resp.end() && e->is_string()) ? e->get<std::string>() : "控制请求失败（无 error 文案）";
    return false;
  }
  const auto r = resp.find("result");
  result = (r != resp.end() && r->is_object()) ? *r : json::object();
  return true;
}

}  // namespace cue::pipeline
