// 设备会话表（FR-PIPE.12）：设备注册、能力协商、租约、多设备并发。
//
// 为什么单独成库而不是塞进 cued.cpp：
//  1) 设备表的全部规则（能力 → 档位、心跳续租、超时回收、同 device_id 的重入）都是
//     **纯状态机**，没有 socket、没有 FFmpeg、没有线程。把它放在库里，单测才能在没有
//     媒体管线的情况下逐条钉死「15 s 没心跳必须被回收」「回收之后同名设备能重新注册」
//     这类判据；放进 cued.cpp 就只能起真进程 + 真等待去验。
//  2) `session.list` / `session.kill` 是控制面的数据源（T-21 控制台设备页）。控制面
//     只认识这张表，不认识 cued 的媒体循环。
//
// 线程模型：表本身一把锁。媒体线程（每会话一根）在帧处理路径上只调 touch()/publish_stats()，
// 都是短临界区；控制线程（UDS）读 sessions_json()；回收由 cued 的定时线程调 expire_due()。
// **回收动作（shutdown）在锁内完成**：见 SessionRegistry::kill 的注释（fd 复用竞态）。
#pragma once

#include <cstdint>
#include <deque>
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cue/pipeline/control_plane.h"  // RuntimeParams：档位沿用同一份参数定义

namespace cue::pipeline {

// 租约默认 15 s（FR-PIPE.12）。取值理由：30 fps 的推流端即使卡顿也能在 15 s 内恢复出包；
// 而半开连接（客户端崩溃、内核还替它 ACK）在 15 s 内一定被判死。
inline constexpr int64_t kDefaultLeaseMs = 15'000;
// 并发上限的硬上界：2 vCPU 的实测上限远小于它（README「已知限制」给了实测数字），
// 这里只挡「--max-sessions 99999」这种把内存吃光的配置错误。
inline constexpr int kMaxSessionsHardLimit = 8;
// 已关闭会话留在表里的条数：设备页要能显示「刚断开的那台」，但不能无限涨。
inline constexpr std::size_t kClosedHistory = 16;

// ---------------------------------------------------------------- 能力协商
//
// 上行 0x05（MediaType::Stats）的载荷是自描述 JSON，cued 只读它认识的键：
//   {"kind":"caps",
//    "device_id":"cam-living",            // 可选；缺省由会话号派生
//    "name":"lavfi testsrc2",             // 可选；设备名
//    "capture_path":"lavfi",              // 采集路径：canvas/mstp/rvfc/lavfi/v4l2...
//    "source":"camera",
//    "video":{"h264":true,"vp8":false,"chosen":"avc1.42E01E","degraded":false,
//             "max":{"width":1280,"height":720,"fps":30},
//             "native":{"width":1280,"height":720}},
//    "audio":{"opus":true,"sample_rate":48000,"channels":1},
//    "display":{"width":1280,"height":720,"fps":30,"bitrate":2000000}}
//
// 兼容既有浏览器（web/app.js 只报 display + video.{h264,vp8,chosen,degraded} + devices）：
// 没有 max/native 时，**display 就是原生尺寸**（浏览器把"现在采多大"说清楚了，没说的
// 上限不能猜）。所有缺省字段保持"未知"而不是 0——0 会被读成"只能采 0 像素"。
struct DeviceCaps {
  bool declared = false;       // 收到过能解析的能力上报
  int native_width = 0;        // 采集侧原生宽（未知 = 0）
  int native_height = 0;
  int max_width = 0;           // 设备声明的能力上限（未知 = 0，不做分辨率夹取）
  int max_height = 0;
  int max_fps = 0;
  bool h264 = false;           // 编码器支持情况（上行方向；对下发档位只做记录与提示）
  bool vp8 = false;
  bool opus = false;
  std::string device_id;       // 客户端自报（可空）
  std::string name;            // 设备名 / UA
  std::string capture_path;
  std::string source;
  std::string codec;           // 实际选中的上行 codec
  bool degraded = false;       // 发送端自己说"降级了"（例如 avc1 不可用退回 vp8）
  int devices_total = 0;       // enumerateDevices() 条目数（浏览器才有）
  std::string note;            // 解析过程中被忽略的部分（不静默丢弃，进设备表可查）
};

// 从 0x05 载荷里的 JSON 解析能力。只认认识的键，缺省保持未知；不抛异常。
DeviceCaps parse_device_caps(const nlohmann::json& caps);
nlohmann::json device_caps_json(const DeviceCaps& caps);

// 服务端按能力下发的档位（FR-PIPE.12）。
//
// **沿用现有 --width/--height/--fps/--bitrate/--gop/--preset/--threads 的逻辑**，不另造
// 一张档位表：下发值 = 配置值，只在"配置值超过设备能力"时夹取。理由：这个项目里
// 「档位」已经是 ParamStore 的一份完整参数（tools.cpp 的 profile 也是映射成一串
// set_param），再引入一套独立的 ladder 会让「控制面改的」和「设备拿到的」分叉。
struct AssignedProfile {
  bool transcode = false;
  int width = 0;   // 0/0 = 沿用解码尺寸（与 RuntimeParams 同义）
  int height = 0;
  int fps = 30;
  int bitrate_kbps = 1500;
  int gop = 60;
  int threads = 1;
  std::string preset = "veryfast";
  bool clamped = false;        // 有没有因为设备能力被夹取
  std::string basis;           // no_caps | caps_ok | caps_clamped | passthrough
  nlohmann::json detail;       // configured / caps / 夹取原因（写进 device.profile_assigned）
};

AssignedProfile assign_profile(const RuntimeParams& params, const DeviceCaps& caps);
bool same_profile(const AssignedProfile& a, const AssignedProfile& b);

// ---------------------------------------------------------------- 会话表
//
// 共享给会话线程之外的东西（NACK 线程要按会话号找重传缓存、报告线程要读每会话读数）。
// 库里只认识"有一个不透明载荷"；具体字段在 cued.cpp 里派生。
class SessionPayload {
 public:
  virtual ~SessionPayload() = default;
};

class SessionRegistry {
 public:
  struct Options {
    int64_t lease_us = kDefaultLeaseMs * 1000;
    int max_sessions = 2;
    uint16_t probe_port_base = 9001;
  };

  struct RegisterRequest {
    std::string device_id;   // 空 = 派生 dev-s<session_id>
    std::string peer;        // "ip:port"，设备页/排查用
    int fd = -1;             // WS 的 socket fd；kill/回收时 shutdown 它
    std::shared_ptr<SessionPayload> payload;
    AssignedProfile profile;
    nlohmann::json profile_detail;  // 下发理由（profile_assigned 事件的 data）
  };

  struct RegisterResult {
    uint32_t session_id = 0;
    std::string device_id;
    int slot = 0;
    uint16_t probe_port = 0;
    int64_t lease_us = 0;
    // 同一 device_id 的旧会话：注册方负责发事件并断开它们（表里已被标成 replaced）。
    std::vector<uint32_t> replaced;
  };

  explicit SessionRegistry(Options options);

  SessionRegistry(const SessionRegistry&) = delete;
  SessionRegistry& operator=(const SessionRegistry&) = delete;

  // 注册一个会话（= 设备表里加一行）。并发满了返回错误（调用方负责拒绝连接）。
  // **同 device_id 的最新接入优先**：旧会话被标成 replaced 并 shutdown，id 由 replaced 返回。
  // 这样"客户端崩溃后立刻重连"不必等旧租约到期，代价是没有任何鉴权的 cued 允许新连接
  // 顶掉旧连接（cued 本来就不鉴权，见 --bind 的警告）。
  std::expected<RegisterResult, std::string> register_session(RegisterRequest req,
                                                             int64_t now_us);

  // 会话线程退出时调用：从表里摘掉，进最近关闭列表。
  void unregister(uint32_t session_id);

  // 会话被回收/被杀的原因（"lease_expired" / "session.kill" / "device_replaced"）；
  // 空 = 正常断开（客户端自己关的）。会话线程在 unregister 之前读它来定事件文案。
  std::string close_reason(uint32_t session_id) const;

  // 心跳续租：任何一条成功读到的 WS 报文都算（见 cued.cpp 的 touch 调用点）。
  bool touch(uint32_t session_id, int64_t now_us);

  // 到期回收：返回被回收的会话号，并在锁内 shutdown 它们的 socket。
  std::vector<uint32_t> expire_due(int64_t now_us);

  // 主动断开（session.kill / device_replaced）。reason 会写进 device.unregistered。
  bool kill(uint32_t session_id, const std::string& reason);

  // 能力上报到达：更新能力快照。设备自报的 device_id 与当前不同时改表里那一行
  // （relabeled=true，old_device_id 供调用方发 device.relabeled 事件）。
  // 同一 device_id 已被别的活会话占着时，那个会话被标 replaced 并 shutdown（返回其 id）。
  bool set_caps(uint32_t session_id, const DeviceCaps& caps, std::vector<uint32_t>& replaced,
                bool& relabeled, std::string& old_device_id);

  void set_profile(uint32_t session_id, const AssignedProfile& profile);
  // 会话线程每秒发布一次自己的读数（键名与 METRICS 行一致，便于聚合读）。
  void publish_stats(uint32_t session_id, nlohmann::json values);

  int live_count() const;
  int max_sessions() const;
  std::shared_ptr<SessionPayload> payload(uint32_t session_id) const;
  // 会话的探针端口（NACK 线程据此确认「这个包确实来自那个探针端口」）。0 = 未知会话。
  uint16_t probe_port_of(uint32_t session_id) const;

  // session.list / /api/state 设备页的数据源。
  nlohmann::json sessions_json(int64_t now_us, bool include_closed) const;
  // 聚合：计数器求和 + 每会话读数数组（METRICS 行的 per_session 用它）。
  // 加法类计数器按会话求和；最坏值类（loss/rtt/delayed_max_us）取 max——一个设备的
  // 坏链路不能被另一台的好数字平均掉（报告里说"这条链路没问题"必须对每台都成立）。
  nlohmann::json aggregate(int64_t now_us) const;

 private:
  struct Record {
    uint32_t session_id = 0;
    std::string device_id;
    std::string peer;
    int fd = -1;
    int slot = 0;
    uint16_t probe_port = 0;
    int64_t connected_us = 0;
    int64_t last_seen_us = 0;
    int64_t lease_us = 0;
    uint64_t heartbeats = 0;   // 续租次数（只用于排查"到底有没有心跳"）
    std::string state;         // live | closing | closed
    std::string reason;        // 非空 = 被回收/被杀的原因
    DeviceCaps caps;
    AssignedProfile profile;
    nlohmann::json profile_detail = nlohmann::json::object();
    nlohmann::json stats = nlohmann::json::object();
    std::shared_ptr<SessionPayload> payload;
  };

  nlohmann::json record_json(const Record& r, int64_t now_us) const;
  Record* find(uint32_t session_id);              // 调用方持锁
  const Record* find(uint32_t session_id) const;  // 调用方持锁
  void mark_and_shutdown(Record& r, const std::string& reason);  // 调用方持锁
  int alloc_slot_locked() const;                  // 调用方持锁

  mutable std::mutex mu_;
  Options opt_;
  uint32_t next_session_id_ = 1;
  std::map<uint32_t, Record> live_;
  std::deque<Record> closed_;
};

}  // namespace cue::pipeline
