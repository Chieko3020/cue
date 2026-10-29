// Real socket readiness and actual best-effort reporter; no user devices or external peers.
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include "probe_network.h"
using namespace cue::probe;
namespace {
int failures=0;
void check(bool ok,const char* what){if(!ok){std::printf("FAIL: %s\n",what);++failures;}}
int64_t now_us(){return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
struct Socket {
  probe_socket_t fd=kProbeInvalidSocket;
  ~Socket(){reset();}
  void reset(){if(fd!=kProbeInvalidSocket)probe_close_socket(fd);fd=kProbeInvalidSocket;}
};
sockaddr_in address(uint16_t port){sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(port);::inet_pton(AF_INET,"127.0.0.1",&a.sin_addr);return a;}
uint16_t port_of(probe_socket_t fd){sockaddr_in a{};probe_socklen_t n=sizeof(a);check(::getsockname(fd,reinterpret_cast<sockaddr*>(&a),&n)==0,"getsockname");return ntohs(a.sin_port);}
bool ready(probe_socket_t fd, short events,int timeout=1000){probe_pollfd p{fd,events,0};return probe_poll(&p,1,timeout)>0&&(p.revents&events)!=0;}
// TCP is a byte stream: readiness does not promise that an entire line is available.
std::string read_line(probe_socket_t fd, int timeout_ms = 1000) {
  const int64_t deadline = now_us() + int64_t{timeout_ms} * 1000;
  std::string line;
  while (line.size() < 4096) {
    const int64_t remaining = deadline - now_us();
    if (remaining <= 0 || !ready(fd, POLLIN, static_cast<int>((remaining + 999) / 1000))) return {};
    char byte = 0;
    // A single reader consumes only bytes whose availability was just observed.
    if (::recv(fd, &byte, 1, 0) != 1) return {};
    line.push_back(byte);
    if (byte == '\n') return line;
  }
  return {};
}
void listen_at(Socket& socket,uint16_t port=0){
  socket.fd=::socket(AF_INET,SOCK_STREAM,0);int reuse=1;
  ::setsockopt(socket.fd,SOL_SOCKET,SO_REUSEADDR,reinterpret_cast<const char*>(&reuse),sizeof(reuse));
  const auto a=address(port);
  check(::bind(socket.fd,reinterpret_cast<const sockaddr*>(&a),sizeof(a))==0,"listener bind");
  check(::listen(socket.fd,4)==0,"listen");
}
}
int main(){
  SocketRuntime runtime;check(runtime.ok(),"socket runtime");
#if defined(_WIN32)
  static_assert(sizeof(probe_socket_t)==sizeof(SOCKET));
#endif
  Socket udp,sender;udp.fd=::socket(AF_INET,SOCK_DGRAM,0);sender.fd=::socket(AF_INET,SOCK_DGRAM,0);
  auto a=address(0);check(::bind(udp.fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0,"UDP bind");
  probe_pollfd p{udp.fd,POLLIN,POLLIN};
  check(probe_poll(&p,1,10)==0&&p.revents==0,"timeout clears old revents");
  a=address(port_of(udp.fd));check(::sendto(sender.fd,"udp",3,0,reinterpret_cast<sockaddr*>(&a),sizeof(a))==3,"UDP send");
  const bool udp_ready = probe_poll(&p,1,1000)>0&&(p.revents&POLLIN);
  check(udp_ready,"UDP read readiness");
  char bytes[256]{};
  if (udp_ready) check(::recv(udp.fd,bytes,sizeof(bytes),0)==3,"UDP receive");
#if defined(_WIN32)
  Socket invalid;invalid.fd=::socket(AF_INET,SOCK_DGRAM,0);const auto closed=invalid.fd;invalid.reset();
  probe_pollfd bad{closed,POLLIN,POLLIN};check(probe_poll(&bad,1,0)<0&&bad.revents==0,"select error clears old revents");
#endif
  Socket listener;listen_at(listener);const auto port=port_of(listener.fd);
  TcpReporters reporter({{"127.0.0.1",port}});
  int connections=0;
  for(int i=0;i<50&&connections==0;++i){const auto start=now_us();connections+=static_cast<int>(reporter.pump(start).size());check(now_us()-start<50'000,"pump never waits for connect");std::this_thread::sleep_for(std::chrono::milliseconds(5));}
  check(connections==1,"TCP connect/write readiness");
  Socket peer;if(ready(listener.fd,POLLIN))peer.fd=::accept(listener.fd,nullptr,nullptr);
  check(peer.fd!=kProbeInvalidSocket,"accept reporter");
  reporter.publish("{\"ev\":\"first\"}\n",now_us());
  check(read_line(peer.fd)=="{\"ev\":\"first\"}\n","complete JSON delivered before deadline");
  linger reset{1,0};::setsockopt(peer.fd,SOL_SOCKET,SO_LINGER,reinterpret_cast<const char*>(&reset),sizeof(reset));peer.reset();listener.reset();
  for(int i=0;i<20&&reporter.dropped()==0;++i){reporter.publish("{\"ev\":\"closed\"}\n",now_us());std::this_thread::sleep_for(std::chrono::milliseconds(10));}
  check(reporter.dropped()>0,"closed receiver counted");
  // Retry stays throttled; a refused attempt never announces a successful connection.
  check(reporter.pump(now_us()).empty(),"retry throttle");
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  check(reporter.pump(now_us()).empty(),"refused connect not announced");
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  check(reporter.pump(now_us()).empty(),"exception/error readiness rejected");
  listen_at(listener,port);
  connections=0;
  for(int i=0;i<300&&connections==0;++i){connections+=static_cast<int>(reporter.pump(now_us()).size());std::this_thread::sleep_for(std::chrono::milliseconds(5));}
  check(connections==1,"receiver restart reconnects");
  if(ready(listener.fd,POLLIN))peer.fd=::accept(listener.fd,nullptr,nullptr);
  reporter.publish("{\"ev\":\"recovered\"}\n",now_us());
  check(read_line(peer.fd)=="{\"ev\":\"recovered\"}\n","complete recovered JSON delivered before deadline");
  TcpReporters invalid_addr({{"not-an-ip",port}});check(invalid_addr.pump(now_us()).empty(),"invalid address cannot connect");invalid_addr.publish("x",now_us());check(invalid_addr.dropped()==1,"invalid address drops visible");
  std::printf("probe network failures=%d\n",failures);return failures?1:0;
}
