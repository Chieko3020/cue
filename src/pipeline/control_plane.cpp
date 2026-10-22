#include "cue/pipeline/control_plane.h"

#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <exception>
#include <string_view>
#include <utility>

namespace cue::pipeline {

namespace {

using json = nlohmann::json;

// 数值解析一律走 from_chars：非法值必须返回错误，而不是让 std::stoi 抛异常穿过 op 边界
// （cued 是长期运行的进程，一条坏请求绝不能带走它）。
bool parse_int(std::string_view s, long long lo, long long hi, int& out) {
  if (s.empty()) return false;
  long long v = 0;
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || p != s.data() + s.size()) return false;
  if (v < lo || v > hi) return false;
  out = static_cast<int>(v);
  return true;
}

bool parse_bool_flag(std::string_view s, bool& out) {
  if (s == "on" || s == "true" || s == "1") {
    out = true;
    return true;
  }
  if (s == "off" || s == "false" || s == "0") {
    out = false;
    return true;
  }
  return false;
}

// x264 的 preset 是闭集：非法名字让 x264_encoder_open 失败，表现为「编码器起不来、
// 指标突然变成 0 帧」——比当场拒绝难查得多，所以在这里拦。
bool valid_preset(std::string_view s) {
  static constexpr std::string_view kPresets[] = {"ultrafast", "superfast", "veryfast",
                                                  "faster",    "fast",      "medium",
                                                  "slow",      "slower",    "veryslow",
                                                  "placebo"};
  for (std::string_view p : kPresets) {
    if (s == p) return true;
  }
  return false;
}

std::string preset_list_text() {
  return "ultrafast/superfast/veryfast/faster/fast/medium/slow/slower/veryslow/placebo";
}

// 一个 json 值 → observation 里的文本形态（字符串原样，其余用 JSON 字面量）
std::string field_text(const json& v) {
  if (v.is_string()) return v.get<std::string>();
  if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
  if (v.is_null()) return "null";
  return v.dump();
}

// {"fps":30,"alive":true} → "fps=30 alive=true"（按插入序：ordered_json 保留字段顺序）
template <typename JsonT>
std::string render_fields(const JsonT& obj) {
  std::string out;
  for (auto it = obj.begin(); it != obj.end(); ++it) {
    if (!out.empty()) out.push_back(' ');
    out += it.key();
    out.push_back('=');
    out += field_text(it.value());
  }
  return out;
}

std::size_t count_lines(const std::string& s) {
  return static_cast<std::size_t>(std::count(s.begin(), s.end(), '\n'));
}

}  // namespace

bool same_params(const RuntimeParams& a, const RuntimeParams& b) {
  return a.transcode == b.transcode && a.width == b.width && a.height == b.height &&
         a.bitrate_kbps == b.bitrate_kbps && a.fps == b.fps && a.gop == b.gop &&
         a.threads == b.threads && a.preset == b.preset && a.send_mbps == b.send_mbps &&
         a.queue_fps == b.queue_fps && a.fec_group == b.fec_group && a.aimd == b.aimd &&
         a.inject_drop == b.inject_drop &&
         a.inject_ingress_drop == b.inject_ingress_drop &&
         a.inject_delay_ms == b.inject_delay_ms && a.inject_jitter_ms == b.inject_jitter_ms &&
         a.inject_seed == b.inject_seed;
}

json encoder_params_json(const RuntimeParams& p) {
  // 名字沿用 encoder_params_json（调用方/录制格式已依赖），但里面不只有"编码档位"：
  // fec_group/aimd 是发送与拥塞控制参数，同样属于「agent 可观测、不可被注入值污染」的
  // 运行配置。三者都**不**进 encoder_relevant_key：改它们不重建编码器。
  return json{{"transcode", p.transcode ? "on" : "off"},
              {"width", p.width},
              {"height", p.height},
              {"size", p.width > 0 ? std::to_string(p.width) + "x" + std::to_string(p.height)
                                   : std::string("follow")},
              {"bitrate_kbps", p.bitrate_kbps},
              {"fps", p.fps},
              {"gop", p.gop},
              {"preset", p.preset},
              {"threads", p.threads},
              {"fec_group", p.fec_group},
              {"aimd", p.aimd ? "on" : "off"}};
}

json runtime_params_json(const RuntimeParams& p) {
  json j = encoder_params_json(p);
  j["send_mbps"] = p.send_mbps;
  j["queue_fps"] = p.queue_fps;
  j["inject_drop"] = p.inject_drop;
  j["inject_ingress_drop"] = p.inject_ingress_drop;
  j["inject_delay_ms"] = p.inject_delay_ms;
  j["inject_jitter_ms"] = p.inject_jitter_ms;
  j["inject_seed"] = p.inject_seed;
  j["fec_group"] = p.fec_group;
  j["aimd"] = p.aimd ? "on" : "off";
  return j;
}

bool encoder_relevant_key(const std::string& key) {
  return key == "transcode" || key == "width" || key == "height" || key == "bitrate_kbps" ||
         key == "gop" || key == "preset" || key == "fps";
}

bool apply_param(const std::string& key, const std::string& value, RuntimeParams& draft,
                 std::string& err) {
  RuntimeParams next = draft;
  if (key == "bitrate_kbps") {
    int v = 0;
    if (!parse_int(value, 100, 20000, v)) {
      err = "bitrate_kbps 超出范围 [100, 20000] 或不是整数: \"" + value + "\"";
      return false;
    }
    next.bitrate_kbps = v;
  } else if (key == "gop") {
    int v = 0;
    // 上限 300 而不是 cued --gop 的 600：SPEC §7.4 的档位表按 300 校准，
    // 运行期改档若允许 600 而评测档位只到 300，两边就出现"工具能设、评测不认"的分叉。
    if (!parse_int(value, 1, 300, v)) {
      err = "gop 超出范围 [1, 300] 或不是整数: \"" + value + "\"";
      return false;
    }
    next.gop = v;
  } else if (key == "preset") {
    if (!valid_preset(value)) {
      err = "preset 不在 x264 预设闭集内: \"" + value + "\"（可用：" + preset_list_text() + "）";
      return false;
    }
    next.preset = value;
  } else if (key == "transcode") {
    bool on = false;
    if (!parse_bool_flag(value, on)) {
      err = "transcode 只接受 on/off（或 true/false/1/0）: \"" + value + "\"";
      return false;
    }
    next.transcode = on;
  } else if (key == "width" || key == "height") {
    int v = 0;
    // 0 = 沿用解码尺寸；只给一个维度没有意义（cued 无法推断另一边）
    if (!parse_int(value, 0, 4096, v)) {
      err = key + " 超出范围 [0, 4096] 或不是整数: \"" + value + "\"";
      return false;
    }
    if (key == "width") {
      next.width = v;
    } else {
      next.height = v;
    }
    if ((next.width == 0) != (next.height == 0)) {
      err = "width/height 必须成对设置（0/0 = 沿用解码尺寸），当前会得到 " +
            std::to_string(next.width) + "x" + std::to_string(next.height);
      return false;
    }
    if (next.width > 0) {
      if (next.width < 16 || next.height < 16) {
        err = "width/height 下限是 16: " + std::to_string(next.width) + "x" +
              std::to_string(next.height);
        return false;
      }
      if (next.width % 2 != 0 || next.height % 2 != 0) {
        err = "width/height 必须是偶数（x264 I420 要求）: " + std::to_string(next.width) + "x" +
              std::to_string(next.height);
        return false;
      }
    }
  } else if (key == "fps") {
    int v = 0;
    if (!parse_int(value, 1, 240, v)) {
      err = "fps 超出范围 [1, 240] 或不是整数: \"" + value + "\"";
      return false;
    }
    next.fps = v;
  } else if (key == "fec_group") {
    // FR-PIPE.9：每 N 个数据包一组。改它只影响下一帧的分组，不重建编码器。
    int v = 0;
    if (!parse_int(value, 1, 64, v)) {
      err = "fec_group 超出范围 [1, 64] 或不是整数: \"" + value + "\"";
      return false;
    }
    next.fec_group = v;
  } else if (key == "aimd") {
    bool on = false;
    if (!parse_bool_flag(value, on)) {
      err = "aimd 只接受 on/off（或 true/false/1/0）: \"" + value + "\"";
      return false;
    }
    next.aimd = on;
  } else {
    // 结构性参数（ws_port / bind / probe_* / jitter_* / fec）仍然必须重启：
    // 这里明确拒绝，而不是接受之后"看起来生效了"。
    err = "unsupported key: \"" + key +
          "\"（运行期可改：bitrate_kbps / gop / preset / transcode / width / height / fps / "
          "fec_group / aimd；ws_port / bind / probe_* / jitter_* / fec(开关) 属于结构性参数，"
          "需要重启 cued）";
    return false;
  }
  draft = next;
  return true;
}

bool apply_fault(const std::string& kind, double value, RuntimeParams& draft, std::string& err) {
  static constexpr double kMaxDelayMs = 2000.0;  // 延迟注入上限（见下面的注释）
  const bool is_delay = kind == "delay" || kind == "jitter";
  if (kind != "drop" && kind != "ingress_drop" && kind != "rate" && kind != "backlog" &&
      !is_delay) {
    err = "不支持的注入类型 \"" + kind +
          "\"（可用：drop / ingress_drop / rate / backlog / delay / jitter）";
    return false;
  }
  // 单位不能混：delay/jitter 是**毫秒**，其余是百分比/Mbps/帧每秒。
  // 两种口径放在同一个 [0,100] 里会让「delay=100」既可能是 100 ms 又可能是 100%，
  // 所以范围按 kind 分开，而不是一刀切。
  if (!std::isfinite(value) || value < 0.0) {
    err = "注入值必须是非负有限数（0 = 清除全部注入）";
    return false;
  }
  if (is_delay) {
    if (value > kMaxDelayMs) {
      err = kind + " 注入值必须在 [0, " + std::to_string(static_cast<int>(kMaxDelayMs)) +
            "] 毫秒（单位是毫秒，不是百分比）";
      return false;
    }
  } else if (value > 100.0) {
    err = "注入值超出范围 [0, 100]（0 = 清除全部注入）";
    return false;
  }
  if (kind == "rate" && value > 0.0 && value < 0.1) {
    err = "rate 注入值必须在 [0.1, 100] Mbps（cued 的 --send-mbps 下限是 0.1）";
    return false;
  }
  RuntimeParams next = draft;
  // value=0 = 清除**全部**注入：工具面只有一个「注入丢包计数」读数，agent 不该被要求
  // 先猜是哪一路注入才能撤销。
  if (value <= 0.0) {
    next.inject_drop = 0.0;
    next.inject_ingress_drop = 0.0;
    next.send_mbps = 10.0;  // SPEC §7.2 的默认链路：10 Mbps / 64 包突发
    next.queue_fps = 0.0;
    next.inject_delay_ms = 0.0;
    next.inject_jitter_ms = 0.0;
  } else if (kind == "drop") {
    next.inject_drop = value;
  } else if (kind == "ingress_drop") {
    next.inject_ingress_drop = value;
  } else if (kind == "rate") {
    next.send_mbps = value;
  } else if (kind == "backlog") {
    next.queue_fps = value;
  } else if (kind == "delay") {
    next.inject_delay_ms = value;
  } else {
    next.inject_jitter_ms = value;
  }
  draft = next;
  return true;
}

// ---------------------------------------------------------------- ParamStore

ParamStore::ParamStore(RuntimeParams init) : p_(std::move(init)) {}

ParamStore::Snapshot ParamStore::snapshot() const {
  std::lock_guard<std::mutex> lk(mu_);
  return Snapshot{p_, gen_, enc_gen_};
}

bool ParamStore::set_param(const std::string& key, const std::string& value, bool& changed,
                           std::string& err) {
  std::lock_guard<std::mutex> lk(mu_);
  RuntimeParams draft = p_;
  if (!apply_param(key, value, draft, err)) return false;
  changed = !same_params(draft, p_);
  if (!changed) return true;  // 幂等：不推进 generation，也就不会白重建一次编码器
  p_ = std::move(draft);
  ++gen_;
  if (encoder_relevant_key(key)) ++enc_gen_;
  return true;
}

bool ParamStore::set_fault(const std::string& kind, double value, bool& changed,
                           std::string& err) {
  std::lock_guard<std::mutex> lk(mu_);
  RuntimeParams draft = p_;
  if (!apply_fault(kind, value, draft, err)) return false;
  changed = !same_params(draft, p_);
  if (!changed) return true;
  p_ = std::move(draft);
  ++gen_;
  // 注入只改发送侧/丢包判定，不动编码器：enc_gen_ 不变 → 不重建编码器 → 不断流。
  return true;
}

bool ParamStore::apply_batch(const json& args, bool& changed, std::string& err) {
  std::lock_guard<std::mutex> lk(mu_);
  RuntimeParams draft = p_;
  bool any_encoder_key = false;
  for (auto it = args.begin(); it != args.end(); ++it) {
    std::string value;
    if (it.value().is_string()) {
      value = it.value().get<std::string>();
    } else if (it.value().is_number() || it.value().is_boolean()) {
      value = it.value().dump();
    } else {
      err = "args." + it.key() + " 必须是字符串或数字";
      return false;
    }
    if (!apply_param(it.key(), value, draft, err)) {
      // 一个字段非法 → 整批不应用：避免「width 改了 height 没改」这种半截状态
      err = "args." + it.key() + ": " + err;
      return false;
    }
    if (encoder_relevant_key(it.key())) any_encoder_key = true;
  }
  changed = !same_params(draft, p_);
  if (!changed) return true;
  p_ = std::move(draft);
  ++gen_;
  if (any_encoder_key) ++enc_gen_;
  return true;
}

json ParamStore::encoder_json() const {
  std::lock_guard<std::mutex> lk(mu_);
  return encoder_params_json(p_);
}

json ParamStore::full_json() const {
  std::lock_guard<std::mutex> lk(mu_);
  return runtime_params_json(p_);
}

// ---------------------------------------------------------------- ControlPlane

ControlPlane::ControlPlane(ParamStore& store, Providers providers)
    : store_(store), prov_(std::move(providers)) {}

ControlPlane::~ControlPlane() { stop(); }

bool ControlPlane::start(const std::string& socket_path, std::string& err) {
  if (server_) {
    err = "控制面已启动";
    return false;
  }
  auto srv = std::make_unique<ControlServer>(
      socket_path, [this](const json& req, ControlSession& ses) { return handle(req, ses); });
  std::string serr;
  if (!srv->start(serr)) {
    // 降级不是"悄悄换成 stdout"：原因要能一路传到指标与 observation 里
    unavailable_reason_ = "UDS 控制通道建立失败：" + serr;
    channel_ = ControlChannelKind::None;
    err = unavailable_reason_;
    return false;
  }
  server_ = std::move(srv);
  channel_ = server_->kind();
  unavailable_reason_.clear();
  // 退到 STREAM 也要留痕：协议一样、分帧方式不同，不能只活在代码注释里
  if (channel_ == ControlChannelKind::Stream && !server_->fallback_reason().empty()) {
    push_log("[cued] 控制通道：" + server_->fallback_reason());
  }
  return true;
}

void ControlPlane::stop() {
  if (server_) {
    server_->stop();
    server_.reset();
  }
  channel_ = ControlChannelKind::None;
}

void ControlPlane::publish(const json& body) {
  if (!body.is_object()) return;
  const auto ev_it = body.find("ev");
  if (ev_it == body.end() || !ev_it->is_string()) return;
  json msg{{"event", ev_it->get<std::string>()},
           {"data", body.contains("data") && body["data"].is_object() ? body["data"]
                                                                     : json::object()}};
  if (body.contains("t")) msg["t"] = body["t"];
  {
    std::lock_guard<std::mutex> lk(ring_mu_);
    events_.push_back(body);
    while (events_.size() > kControlEventMax) events_.pop_front();
  }
  if (server_) server_->publish(msg);
}

void ControlPlane::push_log(std::string line) {
  std::lock_guard<std::mutex> lk(ring_mu_);
  logs_.push_back(std::move(line));
  while (logs_.size() > kControlLogMax) logs_.pop_front();
}

std::string ControlPlane::tail_of_logs(int tail) const {
  std::lock_guard<std::mutex> lk(ring_mu_);
  if (logs_.empty()) return {};
  const std::size_t want = static_cast<std::size_t>(std::max(tail, 1));
  const std::size_t n = std::min(want, logs_.size());
  std::string out;
  for (std::size_t i = logs_.size() - n; i < logs_.size(); ++i) {
    out += logs_[i];
    out.push_back('\n');
  }
  return out;
}

std::string ControlPlane::tail_of_events(int tail) const {
  std::lock_guard<std::mutex> lk(ring_mu_);
  if (events_.empty()) return {};
  const std::size_t want = static_cast<std::size_t>(std::max(tail, 1));
  const std::size_t n = std::min(want, events_.size());
  std::string out;
  for (std::size_t i = events_.size() - n; i < events_.size(); ++i) {
    const json& e = events_[i];
    out += "ev=" + (e.contains("ev") && e["ev"].is_string() ? e["ev"].get<std::string>()
                                                            : std::string("?")) +
           " t_us=" +
           (e.contains("t") && e["t"].is_number_integer() ? std::to_string(e["t"].get<int64_t>())
                                                          : std::string("0")) +
           " data=" + (e.contains("data") ? e["data"].dump() : std::string("{}")) + '\n';
  }
  return out;
}

json ControlPlane::handle(const json& request, ControlSession& session) {
  std::int64_t id = 0;
  std::string op;
  json args;
  std::string perr;
  if (!parse_request(request, id, op, args, perr)) return make_error(id, perr);
  ops_.fetch_add(1, std::memory_order_relaxed);
  try {
    if (op == "pipeline.start") return op_pipeline_start(args, id);
    if (op == "pipeline.stop") return op_pipeline_stop(id);
    if (op == "pipeline.set_param") return op_set_param(args, id);
    if (op == "metrics.query") return op_metrics(args, id);
    if (op == "obs.fetch") return op_obs(args, id);
    if (op == "fault.set") return op_fault(args, id);
    if (op == "subscribe") return op_subscribe(args, session, id);
    // 这两个 op 依赖多设备会话（本批不做）：必须回明确的 not implemented，
    // 不能假装成功，也不能静默不回包（客户端会一直等到超时）。
    if (op == "session.list" || op == "session.kill") {
      return make_error(id, "not implemented: " + op + "（依赖多设备会话，本批未实现）");
    }
  } catch (const std::exception& e) {
    // 客户端可以发任何形态的 args：一条坏请求换来一条 ok:false，进程不能倒。
    return make_error(id, std::string("处理 ") + op + " 时异常: " + e.what());
  }
  return make_error(id, "unknown op: " + op);
}

json ControlPlane::op_pipeline_start(const json& args, std::int64_t id) {
  bool changed = false;
  std::string err;
  if (!args.empty() && !store_.apply_batch(args, changed, err)) {
    return make_error(id, "参数下发失败：" + err);
  }
  if (changed && prov_.emit) {
    prov_.emit("pipeline.param",
               json{{"key", "*"}, {"batch", true}, {"restart", false},
                    {"params", store_.encoder_json()}});
  }
  // already_running 是事实而不是偷懒：cued 的"管线"就是进程本身（WS 监听已就绪），
  // 不存在"进程在、管线没启动"的中间态。真要停只有 pipeline.stop（= 进程退出）。
  return make_ok(id, json{{"running", true},
                          {"already_running", true},
                          {"pid", static_cast<std::int64_t>(::getpid())},
                          {"control_channel", channel_tag()},
                          {"applied", changed},
                          {"params", store_.encoder_json()}});
}

json ControlPlane::op_pipeline_stop(std::int64_t id) {
  if (!prov_.on_stop) return make_error(id, "pipeline.stop 未被后端启用（没有注册停止回调）");
  auto cb = prov_.on_stop;  // 拷贝：延迟线程不能碰已析构的 ControlPlane
  const std::int64_t pid = static_cast<std::int64_t>(::getpid());
  // 响应必须先落到 socket 上，客户端才能按 id 配对；而"停止 cued"就是进程退出。
  // 50 ms 远小于任何客户端超时，又足够让这条响应离开内核缓冲。
  std::thread([cb] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    cb();
  }).detach();
  return make_ok(id, json{{"stopping", true}, {"pid", pid}});
}

json ControlPlane::op_set_param(const json& args, std::int64_t id) {
  if (!args.contains("key") || !args["key"].is_string()) {
    return make_error(id, "缺 args.key（字符串）");
  }
  if (!args.contains("value")) return make_error(id, "缺 args.value");
  const std::string key = args["key"].get<std::string>();
  std::string value;
  if (args["value"].is_string()) {
    value = args["value"].get<std::string>();
  } else if (args["value"].is_number() || args["value"].is_boolean()) {
    // 脚本/手测常直接传数字；工具层传字符串。两种都收。
    value = args["value"].dump();
  } else {
    return make_error(id, "args.value 必须是字符串或数字");
  }
  bool changed = false;
  std::string err;
  if (!store_.set_param(key, value, changed, err)) return make_error(id, err);
  if (changed && prov_.emit) {
    prov_.emit("pipeline.param", json{{"key", key},
                                      {"value", value},
                                      {"restart", false},
                                      {"encoder_rebuild", encoder_relevant_key(key)}});
  }
  // 这里没有重启：进程、WS 会话、解码器都不动；只有编码参数变了才会在下一帧
  // 重建编码器（媒体线程按 encoder_generation 懒执行）。
  return make_ok(id, json{{"key", key},
                          {"value", value},
                          {"changed", changed},
                          {"params", store_.encoder_json()}});
}

json ControlPlane::op_metrics(const json& args, std::int64_t id) {
  int window_ms = 5000;
  if (const auto it = args.find("window_ms"); it != args.end() && it->is_number_integer()) {
    window_ms = it->get<int>();
  }
  window_ms = std::clamp(window_ms, 100, 600'000);
  json out = prov_.metrics ? prov_.metrics(window_ms) : json::object();
  if (!out.is_object()) out = json::object();
  out["window_ms"] = window_ms;
  out["params"] = store_.encoder_json();
  out["control_channel"] = channel_tag();
  out["control_ops"] = ops_.load(std::memory_order_relaxed);
  out["pid"] = static_cast<std::int64_t>(::getpid());
  return make_ok(id, out);
}

json ControlPlane::op_obs(const json& args, std::int64_t id) {
  if (!args.contains("source") || !args["source"].is_string()) {
    return make_error(id, "缺 args.source（logs / procs / frames / events）");
  }
  const std::string source = args["source"].get<std::string>();
  int tail = 20;
  if (const auto it = args.find("tail"); it != args.end() && it->is_number_integer()) {
    tail = it->get<int>();
  }
  tail = std::clamp(tail, 1, 1000);

  std::string text;
  if (source == "logs") {
    text = tail_of_logs(tail);
  } else if (source == "events") {
    text = tail_of_events(tail);
  } else if (source == "frames") {
    text = prov_.frames ? render_fields(prov_.frames()) : std::string();
  } else if (source == "procs") {
    text = prov_.procs ? render_fields(prov_.procs()) : std::string();
  } else {
    // 枚举之外的 source 是调用方的错，回明确原因而不是空字符串（空字符串会被读成
    // 「没有日志」，掩盖真正的参数错误）。
    return make_error(id, "unsupported source: \"" + source +
                              "\"（可用：logs / procs / frames / events）");
  }
  if (text.size() > kMaxObservationText) {
    // 响应必须装得进一条 64 KiB 消息：在行边界截断并明确标注，不静默截半行
    std::size_t cut = text.rfind('\n', kMaxObservationText);
    if (cut == std::string::npos || cut == 0) cut = kMaxObservationText;
    const std::size_t dropped = text.size() - cut;
    text.resize(cut);
    text += "\n...(truncated: " + std::to_string(dropped) + " bytes 未显示)\n";
  }
  return make_ok(id, json{{"source", source},
                          {"tail", tail},
                          {"text", text},
                          {"lines", static_cast<std::int64_t>(count_lines(text))}});
}

json ControlPlane::op_fault(const json& args, std::int64_t id) {
  if (!args.contains("kind") || !args["kind"].is_string()) {
    return make_error(id,
                      "缺 args.kind（drop / ingress_drop / rate / backlog / delay / jitter）");
  }
  if (!args.contains("value") || !args["value"].is_number()) {
    return make_error(id, "缺 args.value（数字；0 = 清除全部注入）");
  }
  const std::string kind = args["kind"].get<std::string>();
  const double value = args["value"].get<double>();
  bool changed = false;
  std::string err;
  if (!store_.set_fault(kind, value, changed, err)) return make_error(id, err);
  if (changed && prov_.emit) {
    if (value <= 0.0) {
      // 清除必须由控制面补一条事件：cued 自己只是"不再注入"，没有任何内部状态翻转，
      // 不补的话事件流里只会看到故障施加、看不到故障撤销。
      prov_.emit("fault.cleared", json{{"kind", kind}, {"value", 0.0}, {"scope", "control"},
                                       {"pid", static_cast<std::int64_t>(::getpid())}});
    } else {
      prov_.emit("fault.applied", json{{"kind", kind}, {"value", value}, {"scope", "control"},
                                       {"pid", static_cast<std::int64_t>(::getpid())}});
    }
  }
  // respawn 只给控制面用（父进程镜像启动参数）：它刚刚设过这些值，不构成额外泄露。
  const json full = store_.full_json();
  return make_ok(id, json{{"kind", kind},
                          {"value", value},
                          {"changed", changed},
                          {"respawn", json{{"inject_drop", full["inject_drop"]},
                                           {"inject_ingress_drop", full["inject_ingress_drop"]},
                                           {"send_mbps", full["send_mbps"]},
                                           {"queue_fps", full["queue_fps"]},
                                           {"inject_delay_ms", full["inject_delay_ms"]},
                                           {"inject_jitter_ms", full["inject_jitter_ms"]}}},
                          {"params", store_.encoder_json()}});
}

json ControlPlane::op_subscribe(const json& args, ControlSession& session, std::int64_t id) {
  std::vector<std::string> events;
  if (const auto it = args.find("events"); it != args.end()) {
    if (!it->is_array()) return make_error(id, "args.events 必须是字符串数组");
    for (const auto& e : *it) {
      if (!e.is_string()) return make_error(id, "args.events 里的元素必须是字符串");
      events.push_back(e.get<std::string>());
    }
  }
  session.subscribe(std::move(events));
  return make_ok(id, json{{"subscribed", true},
                          {"events", args.contains("events") ? args["events"] : json::array()}});
}

}  // namespace cue::pipeline
