// UDS 控制通道单测（技术设计文档 §4.3 / T-05）。
//
// 覆盖的是**协议与控制语义**，不是媒体管线：真 cued 的"改参数不断流"要在
// scripts 下用 probe + test-client 实测（见 commit message），这里钉死的是
// 「换一种 socket 类型，请求-响应语义不变」「坏请求不能带走服务端」「超长消息
// 被拒但连接还在」「重连后 id 从 1 重新开始也能配对」这些可自动化验证的部分。
//
// 为什么用真 socket（socketpair 会被替身糊弄过去）：分帧方式的差异（SEQPACKET 保留
// 消息边界 / STREAM 靠换行）恰恰是这个测试要证明的东西，替身会把它藏起来。
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "cue/pipeline/control_channel.h"
#include "cue/pipeline/control_plane.h"

namespace {

using json = nlohmann::json;
using cue::pipeline::ControlChannelKind;
using cue::pipeline::ControlClient;
using cue::pipeline::ControlPlane;
using cue::pipeline::ControlServer;
using cue::pipeline::ParamStore;
using cue::pipeline::RuntimeParams;
using cue::pipeline::kMaxControlMessage;
using cue::pipeline::make_error;
using cue::pipeline::make_ok;
using cue::pipeline::make_request;

int failures = 0;

void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

// 每个测试块一个独立目录：/tmp 下不能残留上一轮的状态（socket 文件、0700 目录）。
// 名字带 pid，重复跑不会互相踩。
std::string socket_path(const char* tag) {
  static int seq = 0;
  return "/tmp/cue-ctl-test-" + std::to_string(::getpid()) + "-" + tag + "-" +
         std::to_string(seq++) + "/control.sock";
}

RuntimeParams base_params() {
  RuntimeParams p;
  p.bitrate_kbps = 1500;
  p.gop = 60;
  p.preset = "veryfast";
  p.inject_seed = 42;
  return p;
}

// 一个只回显 id 的 handler：专门验证「id 原样回填」与传输层分帧，
// 不掺任何 op 语义。
ControlServer::Handler echo_handler() {
  return [](const json& req, cue::pipeline::ControlSession&) {
    const std::int64_t id = req.contains("id") && req["id"].is_number_integer()
                                ? req["id"].get<std::int64_t>()
                                : -1;
    return make_ok(id, json{{"echo_id", id},
                            {"op", req.value("op", std::string())},
                            {"args", req.contains("args") ? req["args"] : json::object()}});
  };
}

// ---- 裸 socket 辅助：要能发出"客户端不肯发"的东西（超长消息/非 JSON 行）----
int raw_connect(const std::string& path, ControlChannelKind kind, std::string& err) {
  const int type = (kind == ControlChannelKind::SeqPacket ? SOCK_SEQPACKET : SOCK_STREAM) |
                   SOCK_CLOEXEC;
  const int fd = ::socket(AF_UNIX, type, 0);
  if (fd < 0) {
    err = std::string("socket: ") + std::strerror(errno);
    return -1;
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    err = "路径过长";
    ::close(fd);
    return -1;
  }
  std::memcpy(addr.sun_path, path.data(), path.size());
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    err = std::string("connect: ") + std::strerror(errno);
    ::close(fd);
    return -1;
  }
  return fd;
}

bool raw_send(int fd, const std::string& data) {
  std::size_t off = 0;
  while (off < data.size()) {
    const ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
    if (n > 0) {
      off += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

// 读一条完整消息（seqpacket 一条 recv；stream 读到换行）。超时返回空串。
std::string raw_recv(int fd, ControlChannelKind kind, int timeout_ms) {
  const int64_t deadline = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count() +
                           timeout_ms;
  std::string out;
  for (;;) {
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    if (now >= deadline) return out;
    pollfd pfd{fd, POLLIN, 0};
    if (::poll(&pfd, 1, static_cast<int>(deadline - now)) <= 0) return out;
    char buf[8192];
    const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
    if (n <= 0) return out;
    out.append(buf, static_cast<std::size_t>(n));
    if (kind == ControlChannelKind::SeqPacket) return out;
    const std::size_t nl = out.find('\n');
    if (nl != std::string::npos) {
      out.resize(nl);
      return out;
    }
    if (out.size() > kMaxControlMessage * 2) return out;
  }
}

// 归一化 JSON 响应：解析失败时返回带哨兵的对象，让断言能看懂失败原因
json parse_reply(const std::string& raw) {
  json j = json::parse(raw.begin(), raw.end(), nullptr, false);
  if (j.is_discarded() || !j.is_object()) return json{{"ok", false}, {"error", "非 JSON 对象"}};
  return j;
}

}  // namespace

int main() {
  using namespace cue::pipeline;

  // ---------------------------------------------------------------- 1) 两种 socket 类型的
  // 往返（含 id 回填）。SEQPACKET 与 STREAM 必须走**同一套**请求-响应语义：
  // 换分帧方式不该改变协议。
  for (const auto kind : {ControlChannelKind::SeqPacket, ControlChannelKind::Stream}) {
    const std::string path = socket_path(kind == ControlChannelKind::SeqPacket ? "seq" : "stream");
    ControlServer srv(path, echo_handler(), kind);
    std::string err;
    if (!srv.start(err)) {
      std::printf("FAIL: server.start(%s): %s\n", control_channel_detail(kind), err.c_str());
      ++failures;
      continue;
    }
    check(srv.kind() == kind, "传输：服务端按要求的 socket 类型启动");

    ControlClient cli;
    check(cli.connect(path, 500, err, kind), "传输：客户端连上 UDS");
    check(cli.kind() == kind, "传输：客户端 socket 类型与服务端一致");

    json resp;
    check(cli.request(make_request(7, "ping", json{{"n", 1}}), resp, err),
          "传输：请求-响应往返成功");
    check(resp.contains("id") && resp["id"] == 7, "传输：响应原样回填 id=7");
    check(resp.contains("ok") && resp["ok"] == true, "传输：响应 ok=true");
    check(resp["result"]["echo_id"] == 7, "传输：服务端看到的就是 id=7");

    // id 每个请求递增，客户端按 id 配对——这里用一个非连续大 id 再验一次
    check(cli.request(make_request(1234567, "ping", json::object()), resp, err),
          "传输：第二个请求成功");
    check(resp["id"] == 1234567, "传输：大 id 也原样回填");

    // 非 JSON / 缺 id 的请求：必须有 ok:false 的回应，而不是无声无息
    {
      json bad{{"op", "ping"}};  // 缺 id
      json r2;
      check(cli.request(bad, r2, err), "传输：缺 id 的请求仍拿到一条回应");
      check(!r2["ok"].get<bool>(), "传输：缺 id → ok=false");
      check(r2["error"].get<std::string>().find("id") != std::string::npos,
            "传输：缺 id 的错误文案点明 id");
    }

    cli.close();
    srv.stop();
  }

  // ---------------------------------------------------------------- 2) 坏请求不能带走服务端
  // 未知 op / not implemented / 缺 args / 非法 key / 参数形态不对，全部 ok:false 且
  // 服务端继续服务。最后那条"还能继续"的断言是关键：只看 error 文案无法证明进程还活着。
  {
    ParamStore store(base_params());
    ControlPlane::Providers provs;
    provs.metrics = [](int window_ms) {
      return json{{"samples", 3}, {"fps", 30.0}, {"t_us", 4242}, {"window_ms", window_ms}};
    };
    provs.frames = [] { return nlohmann::ordered_json{{"frames_in", 10}, {"frames_out", 9}}; };
    provs.procs = [] { return nlohmann::ordered_json{{"pid", 1234}, {"running", true}}; };
    ControlPlane plane(store, provs);
    const std::string path = socket_path("errors");
    std::string err;
    check(plane.start(path, err), "坏请求：控制面启动");

    ControlClient cli;
    check(cli.connect(path, 500, err), "坏请求：客户端连上");
    json res;
    std::string cerr;

    check(!cli.call("pipeline.nope", json::object(), res, cerr), "坏请求：未知 op 失败");
    check(cerr.find("unknown op") != std::string::npos, "坏请求：未知 op 的 error 指明 unknown op");

    cerr.clear();
    check(!cli.call("session.list", json::object(), res, cerr), "坏请求：未实现的 op 返回失败");
    check(cerr.find("not implemented") != std::string::npos,
          "坏请求：未实现的 op 必须显式 not implemented（不能静默）");

    cerr.clear();
    check(!cli.call("obs.fetch", json::object(), res, cerr), "坏请求：obs.fetch 缺 source 失败");
    check(cerr.find("source") != std::string::npos, "坏请求：缺 source 的 error 点名字段");

    cerr.clear();
    check(!cli.call("obs.fetch", json{{"source", "pids"}}, res, cerr),
          "坏请求：obs.fetch 非法 source 失败");
    check(cerr.find("unsupported source") != std::string::npos,
          "坏请求：非法 source 的 error 说明可选值");

    cerr.clear();
    check(!cli.call("pipeline.set_param", json{{"key", "ws_port"}, {"value", "9000"}}, res, cerr),
          "坏请求：结构性参数被拒绝");
    check(cerr.find("unsupported key") != std::string::npos,
          "坏请求：结构性参数的 error 说明只接受哪些键");

    cerr.clear();
    check(!cli.call("pipeline.set_param", json{{"key", "bitrate_kbps"}, {"value", "abc"}}, res,
                    cerr),
          "坏请求：非法数值被拒绝");
    cerr.clear();
    check(!cli.call("pipeline.set_param", json{{"key", "width"}, {"value", "-1"}}, res, cerr),
          "坏请求：负数宽度被拒绝");
    cerr.clear();
    check(!cli.call("fault.set", json{{"kind", "explode"}, {"value", 1}}, res, cerr),
          "坏请求：未知注入类型被拒绝");
    cerr.clear();
    check(!cli.call("fault.set", json{{"kind", "drop"}, {"value", 200}}, res, cerr),
          "坏请求：超范围注入值被拒绝");

    // 一串坏请求之后，正常请求仍然成功 —— 服务端没被带走
    cerr.clear();
    check(cli.call("metrics.query", json::object(), res, cerr), "坏请求之后：metrics.query 仍成功");
    check(res["samples"] == 3, "坏请求之后：metrics 快照仍是提供方的真实值");
    cli.close();
    plane.stop();
    (void)plane;
  }

  // ---------------------------------------------------------------- 3) set_param 不重启就生效
  // 这条是 FR-PIPE.5 / FR-PIPE.10 在单测层面的判据：
  //  - 改 bitrate → metrics.query 立刻看到新值（同一进程、同一连接）；
  //  - 幂等（改成同一个值）不推进 generation（否则会白重建一次编码器）；
  //  - 注入类改动不推进 encoder_generation（否则「注入故障」会变成一次真实断流）。
  {
    ParamStore store(base_params());
    ControlPlane::Providers provs;
    provs.metrics = [](int) {
      return json{{"samples", 5}, {"fps", 29.9}, {"t_us", 7777}};
    };
    ControlPlane plane(store, provs);
    const std::string path = socket_path("setparam");
    std::string err;
    check(plane.start(path, err), "set_param：控制面启动");
    ControlClient cli;
    check(cli.connect(path, 500, err), "set_param：客户端连上");

    json res;
    std::string cerr;
    check(cli.call("metrics.query", json{{"window_ms", 5000}}, res, cerr),
          "set_param：改之前先查一次指标");
    check(res["params"]["bitrate_kbps"] == 1500, "set_param：改之前是 1500 kbps");
    check(res["control_channel"] == "uds", "set_param：指标里 control_channel=uds");

    const auto gen0 = store.snapshot();
    check(cli.call("pipeline.set_param", json{{"key", "bitrate_kbps"}, {"value", "2000"}}, res,
                   cerr),
          "set_param：改 bitrate_kbps=2000 成功");
    check(res["changed"] == true, "set_param：changed=true");
    const auto gen1 = store.snapshot();
    check(gen1.params.bitrate_kbps == 2000, "set_param：参数存储里的值已变");
    check(gen1.generation == gen0.generation + 1, "set_param：generation 推进 1");
    check(gen1.encoder_generation == gen0.encoder_generation + 1,
          "set_param：编码器 generation 推进（下一帧会重建编码器）");

    cerr.clear();
    check(cli.call("metrics.query", json::object(), res, cerr), "set_param：改之后再查一次指标");
    check(res["params"]["bitrate_kbps"] == 2000, "set_param：metrics.query 能看到新值 2000");
    check(res["control_ops"].get<std::uint64_t>() >= 3, "set_param：control_ops 在累计");

    // 幂等：同值再设一次，不该推进任何 generation
    cerr.clear();
    check(cli.call("pipeline.set_param", json{{"key", "bitrate_kbps"}, {"value", "2000"}}, res,
                   cerr),
          "set_param：同值再设一次成功");
    check(res["changed"] == false, "set_param：同值 → changed=false（幂等）");
    check(store.snapshot().generation == gen1.generation, "set_param：幂等不推进 generation");

    // 注入只改发送侧：不重建编码器（这是"注入不断流"的机械证明）
    const auto fen0 = store.snapshot();
    cerr.clear();
    check(cli.call("fault.set", json{{"kind", "drop"}, {"value", 10}}, res, cerr),
          "fault.set：注入 drop=10 成功");
    const auto fen1 = store.snapshot();
    check(fen1.params.inject_drop == 10.0, "fault.set：注入值写进参数存储");
    check(fen1.encoder_generation == fen0.encoder_generation,
          "fault.set：编码器 generation 不动（不重建编码器 → 不断流）");
    check(fen1.generation == fen0.generation + 1, "fault.set：普通 generation 推进");

    // 清除：value=0 清掉全部注入（工具面只有一个"撤销"语义）
    cerr.clear();
    check(cli.call("fault.set", json{{"kind", "drop"}, {"value", 0}}, res, cerr),
          "fault.set：value=0 清除");
    const auto fen2 = store.snapshot();
    check(fen2.params.inject_drop == 0.0 && fen2.params.send_mbps == 10.0 &&
              fen2.params.queue_fps == 0.0,
          "fault.set：value=0 清除全部注入（含 rate/backlog）");

    // T-09：fec_group / aimd 是**普通运行期参数**——改了不重建编码器，下一帧发送侧生效。
    const auto fg0 = store.snapshot();
    cerr.clear();
    check(cli.call("pipeline.set_param", json{{"key", "fec_group"}, {"value", "16"}}, res, cerr),
          "set_param：fec_group=16 成功（运行期可改）");
    const auto fg1 = store.snapshot();
    check(fg1.params.fec_group == 16, "set_param：fec_group 写进参数存储");
    check(fg1.encoder_generation == fg0.encoder_generation,
          "set_param：fec_group 不推进编码器 generation（不重建编码器）");
    cerr.clear();
    check(!cli.call("pipeline.set_param", json{{"key", "fec_group"}, {"value", "65"}}, res, cerr),
          "set_param：fec_group 越界被拒绝");
    check(cerr.find("fec_group 超出范围") != std::string::npos,
          "set_param：fec_group 越界给出明确原因");
    cerr.clear();
    check(cli.call("pipeline.set_param", json{{"key", "aimd"}, {"value", "off"}}, res, cerr),
          "set_param：aimd=off 成功");
    check(store.snapshot().params.aimd == false, "set_param：AIMD 可以被关掉（评测确定性）");

    // 订阅：之后服务端主动推 {"event":...}（无 id），客户端不该把它当成响应
    std::atomic<int> pushed{0};
    std::string pushed_ev;
    cli.set_event_callback([&](const json& m) {
      pushed.fetch_add(1);
      pushed_ev = m.value("event", std::string());
    });
    cerr.clear();
    check(cli.call("subscribe", json{{"events", json::array({"metric.sample"})}}, res, cerr),
          "subscribe：订阅成功");
    plane.publish(json{{"ev", "metric.sample"}, {"t", 99}, {"data", json{{"fps", 30.0}}}});
    // 下一次请求会在等响应之前先读到那条推送
    cerr.clear();
    check(cli.call("metrics.query", json::object(), res, cerr), "subscribe：推送之后的请求成功");
    check(pushed.load() == 1, "subscribe：收到一条主动推送");
    check(pushed_ev == "metric.sample", "subscribe：推送的 event 名正确");

    cli.close();
    plane.stop();
  }

  // ---------------------------------------------------------------- 4) 超长消息被拒且连接保留
  // 两种 socket 类型各测一遍：> 64 KiB 必须得到明确错误，然后**同一个连接**还能
  // 正常收发。这条最容易做成"悄悄截断"，所以断言里同时检查错误文案与后续可用性。
  for (const auto kind : {ControlChannelKind::SeqPacket, ControlChannelKind::Stream}) {
    const std::string path = socket_path(kind == ControlChannelKind::SeqPacket ? "big-seq"
                                                                             : "big-stream");
    ControlServer srv(path, echo_handler(), kind);
    std::string err;
    if (!srv.start(err)) {
      std::printf("FAIL: 超长消息：server.start: %s\n", err.c_str());
      ++failures;
      continue;
    }
    const int fd = raw_connect(path, kind, err);
    if (fd < 0) {
      std::printf("FAIL: 超长消息：raw_connect: %s\n", err.c_str());
      ++failures;
      srv.stop();
      continue;
    }
    // 64 KiB + 1 字节：刚好越界，比"随便发个 1 MB"更能验到边界
    std::string big(kMaxControlMessage + 1, 'x');
    if (kind == ControlChannelKind::Stream) big.push_back('\n');
    check(raw_send(fd, big), "超长消息：发送端成功写出超长消息");

    // 读回应：STREAM 下服务端会先回一条 too large，再对残留半条回一条坏 JSON，
    // 两条都算"拒绝"，其中至少一条必须点明 too large。
    bool saw_too_large = false;
    for (int i = 0; i < 4; ++i) {
      const std::string raw = raw_recv(fd, kind, 300);
      if (raw.empty()) break;
      const json j = parse_reply(raw);
      if (j.contains("error") && j["error"].is_string() &&
          j["error"].get<std::string>().find("too large") != std::string::npos) {
        saw_too_large = true;
      }
      if (j.value("ok", false)) break;  // 读到后续正常响应就停
    }
    check(saw_too_large, "超长消息：被明确拒绝（too large）");

    // 连接还活着：同一条 fd 上再发一条合法请求，必须拿到 ok
    const json req = make_request(11, "ping", json::object());
    std::string line = req.dump();
    if (kind == ControlChannelKind::Stream) line.push_back('\n');
    check(raw_send(fd, line), "超长消息：同一条连接还能写");
    bool got_ok = false;
    for (int i = 0; i < 4 && !got_ok; ++i) {
      const std::string raw = raw_recv(fd, kind, 500);
      if (raw.empty()) break;
      const json j = parse_reply(raw);
      got_ok = j.value("ok", false) && j.value("id", static_cast<std::int64_t>(0)) == 11;
    }
    check(got_ok, "超长消息：被拒之后同一条连接仍能完成请求-响应");
    ::close(fd);
    srv.stop();
  }

  // ---------------------------------------------------------------- 5) 断线重连，id 从 1 重来
  // 客户端断开后新连接是全新的一代会话：服务端不认识前一连接的任何状态，
  // 所以 id 从 1 重新开始也必须能配对（服务端只做回填，不做去重）。
  {
    const std::string path = socket_path("reconnect");
    ControlServer srv(path, echo_handler());
    std::string err;
    check(srv.start(err), "重连：server.start");

    {
      ControlClient c1;
      check(c1.connect(path, 500, err), "重连：第一个客户端连上");
      json res;
      std::string cerr;
      check(c1.call("ping", json{{"k", 1}}, res, cerr), "重连：第一个客户端的第一次请求");
      check(res["echo_id"] == 1, "重连：第一个客户端的第一条请求 id=1");
      check(c1.call("ping", json{{"k", 2}}, res, cerr), "重连：第一个客户端的第二次请求");
      check(res["echo_id"] == 2, "重连：id 递增到 2");
      c1.close();
    }
    {
      ControlClient c2;
      check(c2.connect(path, 500, err), "重连：第二个客户端连上");
      json res;
      std::string cerr;
      check(c2.call("ping", json{{"k", 3}}, res, cerr), "重连：新客户端的第一次请求");
      check(res["echo_id"] == 1, "重连：新连接 id 从 1 重新开始，仍能配对");
      check(c2.sent() == 1, "重连：新客户端的请求计数独立");
      c2.close();
    }
    srv.stop();
  }

  // ---------------------------------------------------------------- 6) 目录私有性
  // 一个 group/other 可写的目录不能放控制 socket（否则别人能抢绑/替换）。
  // 这条用真实文件系统验：把目录改成 0777 之后开始必须失败并说明原因。
  {
    const std::string path = socket_path("private");
    const std::string dir = path.substr(0, path.find_last_of('/'));
    ::mkdir(dir.c_str(), 0700);
    ::chmod(dir.c_str(), 0777);
    ControlServer srv(path, echo_handler());
    std::string err;
    check(!srv.start(err), "私有目录：0777 的目录必须拒绝");
    check(err.find("权限过宽") != std::string::npos, "私有目录：错误文案点明权限过宽");
    ::rmdir(dir.c_str());
  }

  if (failures == 0) {
    std::printf("test_control_channel: OK\n");
    return 0;
  }
  std::printf("test_control_channel: %d failure(s)\n", failures);
  return 1;
}
