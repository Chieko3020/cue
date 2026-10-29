// App-private socket readiness and best-effort JSON-line reporting.
#pragma once
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
namespace cue::probe {
// ---------------------------------------------------------------- 平台包装
//
// probe 只用两件 POSIX 设施：poll 与 close(fd)。Windows 的同义物是 WSAPoll 与
// closesocket，且两者的结构体名/参数类型都不同，所以在这里各收一个薄包装，
// 而不是在调用点撒 #ifdef（六处 poll + 四处 close，很容易漏一处）。
#if defined(_WIN32)
using probe_pollfd = WSAPOLLFD;
// Windows 的 SOCKET 是 UINT_PTR（64 位），用 int 装会截断句柄。
using probe_socket_t = SOCKET;
using probe_socklen_t = int;
constexpr int kProbeSendFlags = 0;  // MSG_NOSIGNAL 是 Linux 的；Windows 的 send 不产生 SIGPIPE
constexpr probe_socket_t kProbeInvalidSocket = INVALID_SOCKET;
// 用 select 而不是 WSAPoll：实测 WSAPoll 对 UDP 的 POLLIN 在本机不触发
// （bind 成功、发包端用 PowerShell 的 UdpClient 对照能收到，但 probe 的 pkts 恒 0、
// 且不报任何错）。select 是 winsock 上最老的接口，语义与 POSIX 一致。
inline int probe_poll(probe_pollfd* fds, std::size_t n, int timeout_ms) {
  fd_set rs, ws, es;
  FD_ZERO(&rs);
  FD_ZERO(&ws);
  FD_ZERO(&es);
  if (n > FD_SETSIZE) {
    for (std::size_t i = 0; i < n; ++i) fds[i].revents = 0;
    ::WSASetLastError(WSAENOBUFS);
    return SOCKET_ERROR;
  }
  for (std::size_t i = 0; i < n; ++i) {
    fds[i].revents = 0;
    if (fds[i].fd == INVALID_SOCKET) continue;
    if ((fds[i].events & POLLIN) != 0) {
      FD_SET(fds[i].fd, &rs);

    }
    if ((fds[i].events & POLLOUT) != 0) FD_SET(fds[i].fd, &ws);
    FD_SET(fds[i].fd, &es);
  }
  if (rs.fd_count == 0 && ws.fd_count == 0 && es.fd_count == 0) {
    ::WSASetLastError(WSAEINVAL);
    return SOCKET_ERROR;
  }
  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  const int r = ::select(0, rs.fd_count ? &rs : nullptr, ws.fd_count ? &ws : nullptr,
                         es.fd_count ? &es : nullptr, timeout_ms < 0 ? nullptr : &tv);
  if (r > 0) {
    for (std::size_t i = 0; i < n; ++i) {
      if (fds[i].fd == INVALID_SOCKET) continue;
      if (FD_ISSET(fds[i].fd, &rs)) fds[i].revents |= POLLIN;
      if (FD_ISSET(fds[i].fd, &ws)) fds[i].revents |= POLLOUT;
      if (FD_ISSET(fds[i].fd, &es)) fds[i].revents |= POLLERR;
    }
  }
  return r;
}
inline void probe_close_socket(probe_socket_t fd) { ::closesocket(fd); }
#else
using probe_pollfd = struct pollfd;
inline int probe_poll(probe_pollfd* fds, std::size_t n, int timeout_ms) {
  return ::poll(fds, static_cast<nfds_t>(n), timeout_ms);
}
using probe_socket_t = int;
using probe_socklen_t = socklen_t;
constexpr int kProbeSendFlags = MSG_NOSIGNAL;
constexpr probe_socket_t kProbeInvalidSocket = -1;
inline void probe_close_socket(probe_socket_t fd) { ::close(fd); }
#endif


// Explicit Winsock lifetime; POSIX needs no process-wide initialization.
class SocketRuntime {
 public:
  SocketRuntime() {
#if defined(_WIN32)
    WSADATA data{};
    ok_ = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif
  }
  ~SocketRuntime() {
#if defined(_WIN32)
    if (ok_) ::WSACleanup();
#endif
  }
  bool ok() const { return ok_; }
  SocketRuntime(const SocketRuntime&) = delete;
  SocketRuntime& operator=(const SocketRuntime&) = delete;
 private:
  bool ok_ = true;
};

inline bool probe_connect_pending() {
#if defined(_WIN32)
  const int error = ::WSAGetLastError();
  return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEALREADY;
#else
  return errno == EINPROGRESS || errno == EALREADY;
#endif
}
class TcpReporters {
 public:
  struct Addr {
    std::string host;
    uint16_t port = 0;
  };

  explicit TcpReporters(std::vector<Addr> addrs) {
    for (auto& a : addrs) sinks_.push_back(Sink{std::move(a.host), a.port});
  }
  ~TcpReporters() {
    for (auto& s : sinks_) {
      if (s.fd != kProbeInvalidSocket) probe_close_socket(s.fd);
    }
  }
  TcpReporters(const TcpReporters&) = delete;
  TcpReporters& operator=(const TcpReporters&) = delete;

  bool empty() const { return sinks_.empty(); }

  // 尝试推进连接（非阻塞）；返回本轮新连上的地址（调用方据此记一条事件）。
  std::vector<std::string> pump(int64_t now_us) {
    std::vector<std::string> fresh;
    for (auto& s : sinks_) {
      if (s.up) continue;
      if (s.fd == kProbeInvalidSocket) {
        if (now_us < s.next_try_us) continue;
        if (!begin_connect(s, now_us)) continue;
        if (s.up) fresh.push_back(s.host + ":" + std::to_string(s.port));
        continue;
      }
      // 连接进行中：用 poll(0) 看它是否已经完成
      probe_pollfd pfd{static_cast<decltype(probe_pollfd::fd)>(s.fd), POLLOUT, 0};
      const int pr = probe_poll(&pfd, 1, 0);
      if (pr < 0 || (pr == 0 && now_us >= s.connect_deadline_us)) {
        drop(s, now_us);
        continue;
      }
      if (pr == 0) continue;
      int soerr = 0;
      probe_socklen_t len = sizeof(soerr);
      if (::getsockopt(s.fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &len) != 0 ||
          soerr != 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        drop(s, now_us);
        continue;
      }
      s.up = true;
      fresh.push_back(s.host + ":" + std::to_string(s.port));
    }
    return fresh;
  }

  void publish(std::string_view line, int64_t now_us) {
    if (sinks_.empty()) return;
    for (auto& s : sinks_) {
      if (!s.up) {
        ++s.dropped;  // 未连上期间的指标行确实没送到，必须计数
        continue;
      }
      // MSG_NOSIGNAL：对端关闭时不能把整个 probe 用 SIGPIPE 打死
      const auto n = ::send(s.fd, line.data(), static_cast<int>(line.size()), kProbeSendFlags);
      if (n < 0 || static_cast<std::size_t>(n) != line.size()) {
        ++s.dropped;
        drop(s, now_us);
        continue;
      }
      ++s.sent;
    }
  }

  uint64_t sent() const {
    uint64_t n = 0;
    for (const auto& s : sinks_) n += s.sent;
    return n;
  }
  uint64_t dropped() const {
    uint64_t n = 0;
    for (const auto& s : sinks_) n += s.dropped;
    return n;
  }

 private:
  struct Sink {
    std::string host;
    uint16_t port = 0;
    probe_socket_t fd = kProbeInvalidSocket;
    bool up = false;
    int64_t next_try_us = 0;
    int64_t connect_deadline_us = 0;
    uint64_t sent = 0;
    uint64_t dropped = 0;
  };

  static constexpr int64_t kRetryUs = 1'000'000;
  static constexpr int64_t kConnectBudgetUs = 2'000'000;

  bool begin_connect(Sink& s, int64_t now_us) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(s.port);
    if (::inet_pton(AF_INET, s.host.c_str(), &addr.sin_addr) != 1) {
      s.next_try_us = now_us + kRetryUs;  // 解析不了就按域名失败处理，节流重试
      return false;
    }
#if defined(_WIN32)
    const probe_socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd != INVALID_SOCKET) {
      u_long nb = 1;
      if (::ioctlsocket(fd, FIONBIO, &nb) != 0) {
        probe_close_socket(fd);
        s.next_try_us = now_us + kRetryUs;
        return false;
      }
    }
#else
    const probe_socket_t fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
#endif
    if (fd == kProbeInvalidSocket) {
      s.next_try_us = now_us + kRetryUs;
      return false;
    }
    const int r = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (r == 0) {
      s.fd = fd;
      s.up = true;
      return true;
    }
    if (!probe_connect_pending()) {
      probe_close_socket(fd);
      s.next_try_us = now_us + kRetryUs;
      return false;
    }
    s.fd = fd;
    s.connect_deadline_us = now_us + kConnectBudgetUs;
    return true;
  }

  void drop(Sink& s, int64_t now_us) {
    if (s.fd != kProbeInvalidSocket) probe_close_socket(s.fd);
    s.fd = kProbeInvalidSocket;
    s.up = false;
    s.next_try_us = now_us + kRetryUs;
  }

  std::vector<Sink> sinks_;
};


}  // namespace cue::probe
