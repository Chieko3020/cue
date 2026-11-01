// 设备会话表的实现。规则集中在这里，cued 只负责"什么时候调"：
//   · 注册/注销、同一 device_id 的接入语义（最新接入优先）；
//   · 租约（touch / expire_due）与回收时对 socket 的 shutdown；
//   · 能力 → 档位的夹取（assign_profile）；
//   · session.list / METRICS.per_session 的 JSON 形状（控制面与 /api/state 共用）。
#include "cue/pipeline/session_registry.h"

#include <algorithm>
#include <cmath>
#include <string_view>

#include <sys/socket.h>
#include <unistd.h>

namespace cue::pipeline {

using json = nlohmann::json;

namespace {

// 设备自报的 device_id 是外部输入：只允许可读字符、长度有上限。
// 不做这一步的话，一个 4 KiB 的 device_id 会被原样塞进事件流和指标行。
constexpr std::size_t kMaxDeviceIdLen = 64;

std::string sanitize_device_id(std::string_view in) {
  std::string out;
  out.reserve(std::min(in.size(), kMaxDeviceIdLen));
  for (const char ch : in) {
    if (out.size() >= kMaxDeviceIdLen) break;
    const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                    (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == ':';
    out.push_back(ok ? ch : '_');
  }
  return out;
}

int json_int(const nlohmann::json& j, const char* key, int fallback = 0) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_number()) return fallback;
  const double v = it->get<double>();
  if (!std::isfinite(v) || v < 0.0) return fallback;
  return static_cast<int>(std::min(100000.0, static_cast<double>(std::llround(v))));
}

int json_int_at(const nlohmann::json& j, const char* a, const char* b, int fallback = 0) {
  const auto ia = j.find(a);
  if (ia == j.end() || !ia->is_object()) return fallback;
  return json_int(*ia, b, fallback);
}

std::string json_str(const nlohmann::json& j, const char* key) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_string()) return {};
  return it->get<std::string>();
}

bool json_bool_at(const nlohmann::json& j, const char* a, const char* b, bool fallback = false) {
  const auto ia = j.find(a);
  if (ia == j.end() || !ia->is_object()) return fallback;
  const auto ib = ia->find(b);
  if (ib == ia->end() || !ib->is_boolean()) return fallback;
  return ib->get<bool>();
}

std::string json_str_at(const nlohmann::json& j, const char* a, const char* b) {
  const auto ia = j.find(a);
  if (ia == j.end() || !ia->is_object()) return {};
  return json_str(*ia, b);
}

// yuv420p / x264 要求偶数边长，且不小于 16（x264 的最小尺寸）。
int even_dim(int v) {
  if (v <= 0) return 0;
  v -= v % 2;
  if (v < 16) v = 16;
  return v;
}

// 加法类计数器：聚合口径（多路求和）与 METRICS 行同一批键名。
const char* const kAdditiveCounters[] = {
    "frames_in",       "frames_out",      "dropped",         "injected",
    "decoded",         "decode_errors",   "rejected",        "budget_drops",
    "send_errors",     "encode_errors",   "encode_delays",   "decode_drops",
    "queue_dropped",   "delayed_pkts",    "fec_groups",      "ingress_late",
    "audio_frames_in", "audio_bytes_in",  "audio_multi_frag", "bytes_in",
    // 音频下行：真正发出去的包数、被出向注入吃掉的包数（N-01）。两个都要在聚合白名单里
    // ——聚合只认这份名单，漏一个的后果是 METRICS 顶层恒 0，而 probe 侧明明收到了音频
    // （实测踩到：audio_frames_out 从来没进过 METRICS，于是"注入了多少音频包"看起来永远是 0）。
    "audio_frames_out", "audio_injected_drops",
    // 音频解码：成功帧数（audio_decoded 的判据）、累计 PCM 样本数、失败帧数。
    // 多路求和天然成立——"整条管线解出了多少样本"就是各路之和。
    "audio_decoded_frames", "audio_pcm_frames", "audio_decode_errors",
    "jb_depth",        "jb_skipped",      "jb_late",         "jb_switches",
    "fb_rx",           "fb_recv_mbps",
    "upstream_pending_frames", "upstream_pending_bytes", "upstream_budget_rejected"};
// 取 max 的读数：最坏链路不能被另一台的好数字平均掉。
// audio_decode_p99_us 取最慢的一路（性能问题自然是取最坏）；
// audio_rms_dbfs 取最响的一路——聚合后的含义是"至少有一路在这个电平上"，
// 不把不同设备的声压平均成一个数（平均会把静音的那一路藏起来）。
const char* const kMaxFields[] = {"delayed_max_us", "audio_last_pts_us", "jb_target_frames",
                                  "fb_loss_rate", "fb_rtt_ms", "audio_decode_p99_us",
                                  "audio_rms_dbfs"};

nlohmann::json profile_json(const AssignedProfile& p) {
  return nlohmann::json{{"transcode", p.transcode},
                        {"width", p.width},
                        {"height", p.height},
                        {"fps", p.fps},
                        {"bitrate_kbps", p.bitrate_kbps},
                        {"gop", p.gop},
                        {"threads", p.threads},
                        {"preset", p.preset},
                        {"clamped", p.clamped},
                        {"basis", p.basis}};
}

}  // namespace

// ---------------------------------------------------------------- 能力协商

DeviceCaps parse_device_caps(const nlohmann::json& caps) {
  DeviceCaps out;
  if (!caps.is_object()) {
    out.note = "载荷不是 JSON 对象";
    return out;
  }
  out.declared = true;
  out.device_id = sanitize_device_id(json_str(caps, "device_id"));
  out.name = json_str(caps, "name");
  if (out.name.empty()) out.name = json_str(caps, "ua");
  out.capture_path = json_str(caps, "capture_path");
  out.source = json_str(caps, "source");
  out.codec = json_str_at(caps, "video", "chosen");
  out.degraded = json_bool_at(caps, "video", "degraded", false);
  out.h264 = json_bool_at(caps, "video", "h264", false);
  out.vp8 = json_bool_at(caps, "video", "vp8", false);
  out.opus = json_bool_at(caps, "audio", "opus", false);
  out.devices_total = json_int_at(caps, "devices", "total", 0);

  // 上限：优先 video.max，其次 video.native，最后 display。
  // display 是浏览器"现在采多大"——没有更明确的上限时它就是原生尺寸，
  // 拿它当上限是诚实的（服务端不该把 480p 源上采样成 720p 下发）。
  const int vw = json_int_at(caps, "video", "width", 0);
  const int vh = json_int_at(caps, "video", "height", 0);
  const int dw = json_int_at(caps, "display", "width", vw);
  const int dh = json_int_at(caps, "display", "height", vh);
  const int nfps = json_int_at(caps, "display", "fps", json_int_at(caps, "video", "fps", 0));

  // 显式上限（video.max.width/height/fps）：优先于原生尺寸。
  const auto vmax = caps.find("video");
  if (vmax != caps.end() && vmax->is_object()) {
    const auto it = vmax->find("max");
    if (it != vmax->end() && it->is_object()) {
      out.max_width = json_int(*it, "width", 0);
      out.max_height = json_int(*it, "height", 0);
      out.max_fps = json_int(*it, "fps", 0);
    }
    const auto inat = vmax->find("native");
    if (inat != vmax->end() && inat->is_object()) {
      out.native_width = json_int(*inat, "width", 0);
      out.native_height = json_int(*inat, "height", 0);
      if (out.native_width <= 0) out.native_width = dw;
      if (out.native_height <= 0) out.native_height = dh;
    }
  }
  if (out.native_width <= 0) out.native_width = dw;
  if (out.native_height <= 0) out.native_height = dh;
  if (out.max_width <= 0) out.max_width = out.native_width;
  if (out.max_height <= 0) out.max_height = out.native_height;
  if (out.max_fps <= 0) out.max_fps = nfps;

  if (out.max_width <= 0 || out.max_height <= 0) {
    out.note = "没有给出可用的分辨率（video.max / video.native / display 都缺）→ 不做分辨率夹取";
  }
  if (out.h264 == false && out.vp8 == false) {
    // 两个都没声明：可能只是没报，不能据此把设备当"没有编码器"
    out.note += (out.note.empty() ? "" : "；");
    out.note += "没有声明 codec 支持（video.h264/vp8 都缺）→ 只记录，不影响下发档位";
  }
  return out;
}

nlohmann::json device_caps_json(const DeviceCaps& c) {
  nlohmann::json out{{"declared", c.declared},
                     {"device_id", c.device_id.empty() ? json(nullptr) : json(c.device_id)},
                     {"native", {{"width", c.native_width}, {"height", c.native_height}}},
                     {"max", {{"width", c.max_width}, {"height", c.max_height}, {"fps", c.max_fps}}},
                     {"video", {{"h264", c.h264}, {"vp8", c.vp8}, {"degraded", c.degraded}}},
                     {"audio", {{"opus", c.opus}}},
                     {"devices_total", c.devices_total}};
  out["name"] = c.name.empty() ? json(nullptr) : json(c.name);
  out["capture_path"] = c.capture_path.empty() ? json(nullptr) : json(c.capture_path);
  out["source"] = c.source.empty() ? json(nullptr) : json(c.source);
  out["codec"] = c.codec.empty() ? json(nullptr) : json(c.codec);
  if (!c.note.empty()) out["note"] = c.note;
  return out;
}

AssignedProfile assign_profile(const RuntimeParams& rp, const DeviceCaps& caps) {
  AssignedProfile p;
  p.transcode = rp.transcode;
  p.width = rp.width;
  p.height = rp.height;
  p.fps = rp.fps;
  p.bitrate_kbps = rp.bitrate_kbps;
  p.gop = rp.gop;
  p.threads = rp.threads;
  p.preset = rp.preset;
  p.basis = rp.transcode ? "no_caps" : "passthrough";
  p.detail = nlohmann::json{{"configured", {{"transcode", rp.transcode},
                                            {"width", rp.width},
                                            {"height", rp.height},
                                            {"fps", rp.fps},
                                            {"bitrate_kbps", rp.bitrate_kbps},
                                            {"gop", rp.gop},
                                            {"preset", rp.preset}}},
                            {"caps", caps.declared ? device_caps_json(caps) : json(nullptr)}};
  if (!caps.declared) return p;  // 没有能力上报：按配置下发（与 T-22 之前逐字节一致）

  std::string why;
  // ---- 分辨率：只在"配置值 > 设备上限"时夹取（0/0 = 沿用解码尺寸，天然不超上限）----
  const bool has_target = p.width > 0 && p.height > 0;
  if (has_target && caps.max_width > 0 && caps.max_height > 0 &&
      (p.width > caps.max_width || p.height > caps.max_height)) {
    const double sw = double(caps.max_width) / double(p.width);
    const double sh = double(caps.max_height) / double(p.height);
    const double scale = std::min(sw, sh);
    const int nw = even_dim(static_cast<int>(std::floor(double(p.width) * scale)));
    const int nh = even_dim(static_cast<int>(std::floor(double(p.height) * scale)));
    // 码率按像素比例缩放：480p 的设备拿到 720p 的码率不是"档位更高"，是浪费带宽。
    // 下限 200 kbps 与 control_plane.cpp 的 AIMD 下限一致，上限与 --bitrate 的上界一致。
    const double ratio = double(nw * nh) / double(p.width * p.height);
    const int nb = std::clamp(static_cast<int>(std::llround(double(p.bitrate_kbps) * ratio)),
                              200, 20000);
    p.detail["clamped"] = {{"width", nw},
                           {"height", nh},
                           {"bitrate_kbps", nb},
                           {"from", {{"width", p.width}, {"height", p.height},
                                     {"bitrate_kbps", p.bitrate_kbps}}}};
    p.width = nw;
    p.height = nh;
    p.bitrate_kbps = nb;
    p.clamped = true;
    why = "配置 " + std::to_string(rp.width) + "x" + std::to_string(rp.height) +
          " 超过设备上限 " + std::to_string(caps.max_width) + "x" +
          std::to_string(caps.max_height) + " → 夹到 " + std::to_string(nw) + "x" +
          std::to_string(nh);
  }
  // ---- 帧率：超过设备声明的上限就压到上限 ----
  if (caps.max_fps > 0 && p.fps > caps.max_fps) {
    p.detail["fps_clamped"] = {{"from", p.fps}, {"to", caps.max_fps}};
    p.fps = caps.max_fps;
    p.clamped = true;
    if (!why.empty()) why += "；";
    why += "帧率 " + std::to_string(rp.fps) + " > 设备上限 " + std::to_string(caps.max_fps);
  }
  if (p.clamped) {
    p.basis = "caps_clamped";
  } else {
    p.basis = rp.transcode ? "caps_ok" : "passthrough";
  }
  if (!why.empty()) p.detail["why"] = why;
  return p;
}

bool same_profile(const AssignedProfile& a, const AssignedProfile& b) {
  // 只比影响编码器输出的字段。basis/detail/clamped 是"为什么"，改措辞不该触发重建。
  return a.transcode == b.transcode && a.width == b.width && a.height == b.height &&
         a.fps == b.fps && a.bitrate_kbps == b.bitrate_kbps && a.gop == b.gop &&
         a.threads == b.threads && a.preset == b.preset;
}

// ---------------------------------------------------------------- 会话表

SessionRegistry::SessionRegistry(Options options) : opt_(std::move(options)) {
  // 配置是外部输入（命令行/配置文件）：在构造函数里夹到合法区间，别让一个 0 秒租约
  // 把每个会话都立刻回收、或者一个 99999 的并发上限把 2 GB 内存吃光。
  opt_.lease_us = std::clamp<int64_t>(opt_.lease_us, 200'000, 600'000'000);
  opt_.max_sessions = std::clamp(opt_.max_sessions, 1, kMaxSessionsHardLimit);
}

SessionRegistry::Record* SessionRegistry::find(uint32_t session_id) {
  const auto it = live_.find(session_id);
  return it == live_.end() ? nullptr : &it->second;
}

const SessionRegistry::Record* SessionRegistry::find(uint32_t session_id) const {
  const auto it = live_.find(session_id);
  return it == live_.end() ? nullptr : &it->second;
}

int SessionRegistry::alloc_slot_locked() const {
  for (int slot = 0; slot < opt_.max_sessions; ++slot) {
    bool used = false;
    for (const auto& [id, r] : live_) {
      if (r.slot == slot) {
        used = true;
        break;
      }
    }
    if (!used) return slot;
  }
  return -1;
}

// 回收/杀掉一个会话：标记原因 + **在锁内** shutdown socket。
//
// 为什么 shutdown 必须在锁内：会话线程在退出路径上是「unregister（持锁）→ 关闭 fd」。
// 如果 kill 方在释放锁之后再 shutdown，这中间会话线程可能已经 unregister 并 close，
// 而新连接 accept 出的 socket 可能复用同一个 fd 号 —— 那时 shutdown 打中的是**别人**。
// 持锁期间会话线程进不了 unregister，fd 只能是这一条的。shutdown 是纯 syscall，不会回调本表。
void SessionRegistry::mark_and_shutdown(Record& r, const std::string& reason) {
  if (r.state != "live") return;
  r.state = "closing";
  r.reason = reason;
  if (r.fd >= 0) {
    ::shutdown(r.fd, SHUT_RDWR);
    r.fd = -1;  // 清掉：重复 kill / 到期扫描不会再碰这个号
  }
}

std::expected<SessionRegistry::RegisterResult, std::string> SessionRegistry::register_session(
    RegisterRequest req, int64_t now_us) {
  std::lock_guard<std::mutex> lk(mu_);
  if (static_cast<int>(live_.size()) >= opt_.max_sessions) {
    return std::unexpected("并发上限 " + std::to_string(opt_.max_sessions) +
                           " 路已满（--max-sessions；先断一路，或按实测上限调大）");
  }
  const int slot = alloc_slot_locked();
  if (slot < 0) {
    return std::unexpected("没有空闲会话槽位（--max-sessions=" +
                           std::to_string(opt_.max_sessions) + "）");
  }
  RegisterResult out;
  out.session_id = next_session_id_++;
  out.slot = slot;
  out.probe_port = static_cast<uint16_t>(opt_.probe_port_base + slot);
  out.lease_us = opt_.lease_us;
  out.device_id = sanitize_device_id(req.device_id);
  if (out.device_id.empty()) out.device_id = "dev-s" + std::to_string(out.session_id);

  // 同一 device_id 的最新接入优先：旧会话标 replaced 并 shutdown，调用方负责发事件。
  for (auto& [id, r] : live_) {
    if (r.state == "live" && r.device_id == out.device_id) {
      mark_and_shutdown(r, "device_replaced");
      out.replaced.push_back(id);
    }
  }

  Record rec;
  rec.session_id = out.session_id;
  rec.device_id = out.device_id;
  rec.peer = std::move(req.peer);
  rec.fd = req.fd;
  rec.slot = slot;
  rec.probe_port = out.probe_port;
  rec.connected_us = now_us;
  rec.last_seen_us = now_us;
  rec.lease_us = opt_.lease_us;
  rec.state = "live";
  rec.profile = std::move(req.profile);
  rec.profile_detail = std::move(req.profile_detail);
  rec.payload = std::move(req.payload);
  live_.emplace(rec.session_id, std::move(rec));
  return out;
}

void SessionRegistry::unregister(uint32_t session_id) {
  std::lock_guard<std::mutex> lk(mu_);
  Record* r = find(session_id);
  if (r == nullptr) return;
  r->state = "closed";
  r->fd = -1;  // 从这里起谁都不能再 shutdown 这个 fd（会话线程马上会关它）
  closed_.push_back(*r);
  while (closed_.size() > kClosedHistory) closed_.pop_front();
  live_.erase(session_id);
}

std::string SessionRegistry::close_reason(uint32_t session_id) const {
  std::lock_guard<std::mutex> lk(mu_);
  const Record* r = find(session_id);
  return r == nullptr ? std::string() : r->reason;
}

bool SessionRegistry::touch(uint32_t session_id, int64_t now_us) {
  std::lock_guard<std::mutex> lk(mu_);
  Record* r = find(session_id);
  if (r == nullptr || r->state != "live") return false;
  r->last_seen_us = now_us;
  r->heartbeats += 1;
  return true;
}

std::vector<uint32_t> SessionRegistry::expire_due(int64_t now_us) {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<uint32_t> expired;
  for (auto& [id, r] : live_) {
    if (r.state != "live") continue;
    if (now_us < r.last_seen_us + r.lease_us) continue;
    mark_and_shutdown(r, "lease_expired");
    expired.push_back(id);
  }
  return expired;
}

bool SessionRegistry::kill(uint32_t session_id, const std::string& reason) {
  std::lock_guard<std::mutex> lk(mu_);
  Record* r = find(session_id);
  if (r == nullptr || r->state != "live") return false;
  mark_and_shutdown(*r, reason);
  return true;
}

bool SessionRegistry::set_caps(uint32_t session_id, const DeviceCaps& caps,
                               std::vector<uint32_t>& replaced, bool& relabeled,
                               std::string& old_device_id) {
  std::lock_guard<std::mutex> lk(mu_);
  Record* r = find(session_id);
  if (r == nullptr) return false;
  r->caps = caps;
  replaced.clear();
  relabeled = false;
  old_device_id.clear();
  if (caps.device_id.empty() || caps.device_id == r->device_id) return true;
  // 客户端自报的 id 与派生 id 不同：改表里这一行。已被别的活会话占着时，
  // 按"最新接入优先"顶掉旧的（与 register_session 同一套规则）。
  for (auto& [id, other] : live_) {
    if (id == session_id || other.state != "live" || other.device_id != caps.device_id) continue;
    mark_and_shutdown(other, "device_replaced");
    replaced.push_back(id);
  }
  relabeled = true;
  old_device_id = r->device_id;
  r->device_id = caps.device_id;
  return true;
}

void SessionRegistry::set_profile(uint32_t session_id, const AssignedProfile& profile) {
  std::lock_guard<std::mutex> lk(mu_);
  if (Record* r = find(session_id)) r->profile = profile;
}

void SessionRegistry::publish_stats(uint32_t session_id, nlohmann::json values) {
  std::lock_guard<std::mutex> lk(mu_);
  if (Record* r = find(session_id)) r->stats = std::move(values);
}

int SessionRegistry::live_count() const {
  std::lock_guard<std::mutex> lk(mu_);
  int n = 0;
  for (const auto& [id, r] : live_) {
    (void)id;
    if (r.state == "live") ++n;
  }
  return n;
}

int SessionRegistry::max_sessions() const { return opt_.max_sessions; }

std::shared_ptr<SessionPayload> SessionRegistry::payload(uint32_t session_id) const {
  std::lock_guard<std::mutex> lk(mu_);
  const Record* r = find(session_id);
  return r == nullptr ? nullptr : r->payload;
}

uint16_t SessionRegistry::probe_port_of(uint32_t session_id) const {
  std::lock_guard<std::mutex> lk(mu_);
  const Record* r = find(session_id);
  return r == nullptr ? 0 : r->probe_port;
}

nlohmann::json SessionRegistry::record_json(const Record& r, int64_t now_us) const {
  const int64_t expire_at = r.last_seen_us + r.lease_us;
  const int64_t left_ms = r.state == "live" ? std::max<int64_t>(0, expire_at - now_us) / 1000 : 0;
  nlohmann::json out{
      {"session_id", r.session_id},
      {"device_id", r.device_id},
      {"state", r.state},
      {"peer", r.peer},
      {"slot", r.slot},
      {"probe_port", r.probe_port},
      {"connected_us", r.connected_us},
      {"last_seen_us", r.last_seen_us},
      {"lease_ms", r.lease_us / 1000},
      {"expires_in_ms", left_ms},
      {"heartbeats", r.heartbeats},
      {"caps", device_caps_json(r.caps)},
      {"profile", profile_json(r.profile)},
      {"profile_basis", r.profile_detail},
      {"stats", r.stats},
  };
  if (!r.reason.empty()) out["reason"] = r.reason;
  return out;
}

nlohmann::json SessionRegistry::sessions_json(int64_t now_us, bool include_closed) const {
  std::lock_guard<std::mutex> lk(mu_);
  nlohmann::json list = nlohmann::json::array();
  int live_n = 0;
  for (const auto& [id, r] : live_) {
    (void)id;
    if (r.state == "live") ++live_n;
    list.push_back(record_json(r, now_us));
  }
  if (include_closed) {
    for (const auto& r : closed_) list.push_back(record_json(r, now_us));
  }
  return nlohmann::json{{"sessions", std::move(list)},
                        {"count", live_.size()},
                        {"live", live_n},
                        {"max_sessions", opt_.max_sessions},
                        {"lease_ms", opt_.lease_us / 1000}};
}

nlohmann::json SessionRegistry::aggregate(int64_t now_us) const {
  std::lock_guard<std::mutex> lk(mu_);
  nlohmann::json counters = nlohmann::json::object();
  for (const char* key : kAdditiveCounters) counters[key] = 0;
  for (const char* key : kMaxFields) counters[key] = 0;
  counters["fb_loss_rate"] = nullptr;  // 没有会话报过 feedback 时必须是 null，不是 0
  counters["fb_rtt_ms"] = nullptr;
  // 同理：没有音频流的会话不能把 audio_rms_dbfs 写成 0（0 dBFS 是满幅！），
  // 也不能把 p99 写成 0 µs。null 才是"没有这个读数"。
  counters["audio_decode_p99_us"] = nullptr;
  counters["audio_rms_dbfs"] = nullptr;
  double fps_sum = 0.0;
  int live_n = 0;
  nlohmann::json per_session = nlohmann::json::array();
  for (const auto& [id, r] : live_) {
    if (r.state != "live") continue;
    ++live_n;
    nlohmann::json entry{{"session_id", id},
                         {"device_id", r.device_id},
                         {"slot", r.slot},
                         {"probe_port", r.probe_port},
                         {"state", r.state},
                         {"expires_in_ms",
                          std::max<int64_t>(0, r.last_seen_us + r.lease_us - now_us) / 1000},
                         {"profile", profile_json(r.profile)}};
    for (auto it = r.stats.begin(); it != r.stats.end(); ++it) entry[it.key()] = it.value();
    per_session.push_back(std::move(entry));
    // 求和
    for (const char* key : kAdditiveCounters) {
      const auto it = r.stats.find(key);
      if (it == r.stats.end() || !it->is_number()) continue;
      const double v = it->get<double>();
      counters[key] = counters[key].get<double>() + v;
    }
    // 取最大（null 与其他值比较时跳过 null）
    for (const char* key : kMaxFields) {
      const auto it = r.stats.find(key);
      if (it == r.stats.end()) continue;
      if (it->is_null()) continue;
      if (!it->is_number()) continue;
      const double v = it->get<double>();
      if (counters[key].is_null() || v > counters[key].get<double>()) counters[key] = v;
    }
    const auto fit = r.stats.find("fps");
    if (fit != r.stats.end() && fit->is_number()) fps_sum += fit->get<double>();
  }
  return nlohmann::json{{"live", live_n},
                        {"fps", fps_sum},
                        {"counters", std::move(counters)},
                        {"per_session", std::move(per_session)}};
}

}  // namespace cue::pipeline
