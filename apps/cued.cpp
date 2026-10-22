// cued — 媒体管线（M2a：解码 + 六段埋点，转码仍为 passthrough）
// 浏览器 --WS--> cued --UDP--> cue-probe
//
// 六段埋点（SPEC §5.3）：
//   recv   首片到达 → 本帧重组完成（帧内首片到齐时间）
//   buffer 帧在抖动缓冲里的真实等待（进队 → 放行）；T-04 之前这里记的是重组耗时，
//          与 recv 同口径，等于没有抖动缓冲这一段
//   decode libavcodec 解码耗时
//   op     ProcessorChain 耗时（M2a 为 passthrough，恒 ~0）
//   encode 编码耗时（M2b 转码后才非 0）
//   send   UDP 分片与发送耗时
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iomanip>
#include <mutex>
#include <span>
#include <sstream>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

#include "cue/common/clock.h"
#include "cue/media/encoder.h"
#include "cue/pipeline/control_plane.h"
#include "cue/media/h264_decoder.h"
#include "cue/media/jitter_buffer.h"
#include "cue/proto/wire.h"

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using udp = net::ip::udp;
using json = nlohmann::json;

namespace {

// 事件里的 t 是 unix 微秒（SSE/报告按墙钟展示）；延迟测量仍然只准用 cue::now_us()
// 的单调时钟（common/clock.h）。两者不可混用，这里只用于「事件发生时刻」。
int64_t unix_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// 控制面指针（T-05）。为什么用全局而不是层层传参：emit_event 在媒体循环、NACK 线程、
// 控制线程三处都被调用，而 UDS 订阅者看到的必须是**同一份**事件流。全局只在 main 里
// 赋一次值，之后只读；读到的 nullptr 表示控制通道没建起来（stdout 降级模式）。
std::atomic<cue::pipeline::ControlPlane*> g_control{nullptr};
// pipeline.stop（UDS 控制）与 SIGTERM 走同一条退出路径，这个标记只用于把事件里的
// reason 写成 control 而不是 signal——两种退出在报告里必须能分开。
std::atomic<bool> g_stop_from_control{false};
// 帧水位（进程级、单调）：每秒的 metric.sample 看不出 200 ms 的停顿，而"改了参数有没有
// 断流"恰恰要用 10 ms 级的分辨率才量得出来。这两个计数每帧推进，metrics.query 原样带出。
std::atomic<uint64_t> g_frames_in_total{0};
std::atomic<uint64_t> g_frames_out_total{0};

// 单行结构化事件： [cue-event] {"ev":"<type>","t":<unix_us>,"data":{...}}
// 与 [cued]/[probe] 人读行共存：现有解析器只认整行前缀，互不干扰。
// 外层用 ordered_json：键序固定为 ev/t/data（SPEC §5 的格式），不随字段名排序漂移。
//
// 一个出口写两处（stdout 行 + UDS 订阅推送）：两处各写一遍必然漂移，
// 而事件流是 agent/SSE 的权威来源——订阅者看到的必须与 harness 读到的同源。
void emit_event(std::string_view ev, const json& data) {
  nlohmann::ordered_json body{{"ev", std::string(ev)}, {"t", unix_us()}, {"data", data}};
  cue::proto::emit_event_line(body.dump());
  if (auto* ctl = g_control.load(std::memory_order_acquire)) {
    ctl->publish(json{{"ev", std::string(ev)}, {"t", body["t"]}, {"data", data}});
  }
}

// ---------------------------------------------------------------- codec_config(0x03)
// 前端用 annexb 编码（web/app.js: avc.format='annexb'），所以按 Annex-B 起始码切 NAL，
// 取 SPS(nal_unit_type=7) / PPS(8)。载荷不是 Annex-B 时整段按 raw hex 留底，不静默丢。
constexpr std::size_t kMaxConfigNalBytes = 1024;   // SPS/PPS 实际只有几十字节，超了就是异常
constexpr std::size_t kMaxConfigRawBytes = 2048;   // raw 兜底的十六进制长度上限（2×字节数）

struct CodecConfig {
  bool have = false;
  std::string codec = "h264";
  std::string sps_hex;
  std::string pps_hex;
  std::string raw_hex;  // 非 Annex-B 载荷（例如 avcC 描述）时的兜底
  std::size_t sps_bytes = 0;
  std::size_t pps_bytes = 0;
  uint32_t stream_id = 0;

  json as_json(bool replayed) const {
    return json{{"codec", codec},
                {"sps", sps_hex.empty() ? json(nullptr) : json(sps_hex)},
                {"pps", pps_hex.empty() ? json(nullptr) : json(pps_hex)},
                {"sps_bytes", sps_bytes},
                {"pps_bytes", pps_bytes},
                {"raw", raw_hex.empty() ? json(nullptr) : json(raw_hex)},
                {"stream_id", stream_id},
                {"replayed", replayed}};
  }
};

std::string to_hex(const uint8_t* p, std::size_t n) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(n * 2, '0');
  for (std::size_t i = 0; i < n; ++i) {
    out[2 * i] = kHex[p[i] >> 4];
    out[2 * i + 1] = kHex[p[i] & 0x0F];
  }
  return out;
}

// 返回是否找到 SPS/PPS（两者之一即可）。找不到时 out 保持不变，由调用方走 raw 兜底。
bool parse_annexb_sps_pps(std::span<const uint8_t> in, CodecConfig& out) {
  // 先定位起始码（4 字节优先，避免 00 00 00 01 被当成 00 00 01 + 一个多余 0）
  struct Nal {
    std::size_t sc_pos;  // 起始码首字节
    std::size_t begin;   // NAL 首字节
  };
  std::vector<Nal> nals;
  for (std::size_t i = 0; i + 3 <= in.size();) {
    if (i + 4 <= in.size() && in[i] == 0 && in[i + 1] == 0 && in[i + 2] == 0 && in[i + 3] == 1) {
      nals.push_back(Nal{i, i + 4});
      i += 4;
      continue;
    }
    if (in[i] == 0 && in[i + 1] == 0 && in[i + 2] == 1) {
      nals.push_back(Nal{i, i + 3});
      i += 3;
      continue;
    }
    ++i;
  }
  bool found = false;
  for (std::size_t k = 0; k < nals.size(); ++k) {
    const std::size_t begin = nals[k].begin;
    const std::size_t end = (k + 1 < nals.size()) ? nals[k + 1].sc_pos : in.size();
    if (end <= begin) continue;
    const std::size_t len = end - begin;
    if (len > kMaxConfigNalBytes) continue;
    const uint8_t nal_type = static_cast<uint8_t>(in[begin] & 0x1F);
    if (nal_type == 7 && out.sps_hex.empty()) {
      out.sps_hex = to_hex(in.data() + begin, len);
      out.sps_bytes = len;
      found = true;
    } else if (nal_type == 8 && out.pps_hex.empty()) {
      out.pps_hex = to_hex(in.data() + begin, len);
      out.pps_bytes = len;
      found = true;
    }
  }
  return found;
}

class Ring {
 public:
  void push(int64_t v) {
    if (buf_.size() < kCap) {
      buf_.push_back(v);
    } else {
      buf_[head_] = v;
      head_ = (head_ + 1) % kCap;
    }
  }
  int64_t pct(double p) const {
    if (buf_.empty()) return 0;
    std::vector<int64_t> v = buf_;
    std::sort(v.begin(), v.end());
    return v[static_cast<std::size_t>(p * double(v.size() - 1))];
  }
  std::size_t size() const { return buf_.size(); }

 private:
  static constexpr std::size_t kCap = 4096;
  std::vector<int64_t> buf_;
  std::size_t head_ = 0;
};

// splitmix64：由包序号直接决定是否丢包，保证同一 (seed, seq) 结果完全一致
inline uint64_t splitmix64(uint64_t x) {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

// 注入判定是纯函数：(seed, seq, 千分比) → 是否丢，所以不留可变状态。
// 千分比每帧从参数快照里取，控制面改值立刻生效——旧实现把 permille 存进 Injector，
// 于是「改一次注入」只能重启进程，那正是 T-05 要删掉的行为。
// 换算保持 `pct * 10` 的截断语义不变：--inject-trace 的逐包判定必须与旧实现逐位一致。
inline uint64_t permille_from_pct(double pct) {
  const double clamped = std::clamp(pct, 0.0, 100.0);
  return static_cast<uint64_t>(clamped * 10.0);
}

inline bool should_drop_pkt(uint64_t seed, uint64_t permille, uint64_t seq) {
  if (permille == 0) return false;
  return (splitmix64(seed * 0x9E3779B97F4A7C15ULL + seq) % 1000) < permille;
}

// 最近 N 帧的分片缓存，供 NACK 重传
class RetransmitCache {
 public:
  void put(uint32_t frame_id, const std::vector<std::vector<uint8_t>>& dg) {
    std::lock_guard<std::mutex> lk(mu_);
    frames_.emplace_back(frame_id, dg);
    while (frames_.size() > 256) frames_.pop_front();
  }
  std::optional<std::vector<uint8_t>> find(uint32_t frame_id, uint16_t frag_index) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
      if (it->first != frame_id) continue;
      if (frag_index < it->second.size()) return it->second[frag_index];
      return std::nullopt;
    }
    return std::nullopt;
  }

 private:
  std::mutex mu_;
  std::deque<std::pair<uint32_t, std::vector<std::vector<uint8_t>>>> frames_;
};

// 令牌桶：SPEC §7.2「10 Mbps / 64 包突发」。
// 口径按**包**计费（包大小按 kMaxUdpPayload + 24 B 头折算成包速率），这样 24 B/包的
// 头开销不会被漏算；突发是 64 包常量而不是"5% 窗口字节数"。
// 不足时先丢非关键帧：关键帧允许透支，但透支上限也是 64 包，关键帧风暴不能无限超发。
class TokenBucket {
 public:
  static constexpr double kBurstPackets = 64.0;
  static constexpr double kPacketBytes =
      double(cue::proto::kMaxUdpPayload + cue::proto::kDatagramHeaderSize);

  explicit TokenBucket(double bits_per_sec) { set_rate(bits_per_sec); }

  // 运行期改速率上限（fault.set kind=rate）：速率是令牌桶唯一的可变量，
  // 原地改不会丢令牌、也不打乱突发——所以这一步不需要重建任何东西。
  void set_rate(double bits_per_sec) {
    rate_pkts_.store(bits_per_sec / 8.0 / kPacketBytes, std::memory_order_relaxed);
  }

  // 非关键帧：令牌不够就丢（不排队）
  bool take_nonkey(std::size_t pkts, int64_t now_us) {
    refill(now_us);
    if (tokens_ < double(pkts)) return false;
    tokens_ -= double(pkts);
    return true;
  }
  // 关键帧：允许透支到 -burst，保证"先丢非关键帧"这条语义真的成立
  bool take_key(std::size_t pkts, int64_t now_us) {
    refill(now_us);
    if (tokens_ - double(pkts) < -kBurstPackets) return false;
    tokens_ -= double(pkts);
    return true;
  }

 private:
  void refill(int64_t now_us) {
    if (last_us_ == 0) last_us_ = now_us;
    // 时间回拨（NTP/CLOCK_MONOTONIC 不会，但参数注入的假时钟可能）不能让 dt 变负
    const double dt = now_us > last_us_ ? double(now_us - last_us_) / 1e6 : 0.0;
    last_us_ = now_us;
    tokens_ = std::min(kBurstPackets, tokens_ + dt * rate_pkts_.load(std::memory_order_relaxed));
  }

  // 控制线程写、媒体线程读：atomic<double> 在 x86-64 上无锁，读到的要么是旧速率
  // 要么是新速率，不会出现"半个 double"（那会让令牌桶按乱码速率灌令牌）。
  std::atomic<double> rate_pkts_{0.0};
  double tokens_ = kBurstPackets;
  int64_t last_us_ = 0;
};

// 限流日志：同一类错误 1 s 内只打一行（弱网/配置错误不能把 stderr 刷爆）
struct RateLimitedLog {
  void log(int64_t now_us, std::string msg) {
    if (msg == last && now_us - t_last_us < 1'000'000) return;
    std::fprintf(stderr, "[cued] %s\n", msg.c_str());
    last = std::move(msg);
    t_last_us = now_us;
  }
  int64_t t_last_us = 0;
  std::string last;
};

struct FrameAcc {
  uint32_t count = 0;
  uint64_t pts_us = 0;
  bool keyframe = false;
  int64_t first_seen_us = 0;
  std::vector<std::vector<uint8_t>> parts;
};

struct Counters {
  uint64_t frames_in = 0;
  uint64_t frames_out = 0;
  uint64_t frames_dropped = 0;
  uint64_t bytes_in = 0;
  uint64_t frames_decoded = 0;
  uint64_t decode_errors = 0;
  uint64_t injected_drops = 0;
  uint64_t rejected = 0;      // 协议边界拒绝（wire.h 的 validate_media 给出具体原因）
  uint64_t budget_drops = 0;  // 令牌桶不足丢的帧（先丢非关键帧）
  uint64_t send_errors = 0;   // sendto 失败的包（ENOBUFS/EMSGSIZE 不再被当成"已发送"）
  uint64_t encode_errors = 0;  // 编码失败（尺寸/格式/编码器错误），不再混进"轻丢帧"
  uint64_t encode_delays = 0;  // 编码器内部延迟：正常路径，无可发数据
  uint64_t decode_drops = 0;   // 解码器未消费的包（H264Decoder::stats().dropped）
  uint64_t queue_dropped = 0;  // 排队策略切换时被丢弃的积压帧（见 process_frame 里的注释）
  // 音频只计数：上行 type=0x02 的包被接收、被计数、出现在指标行里，但不进解码、
  // 不写下行。audio_decoded 恒 false 是这条链路的真实状态（原因见 METRICS 行的
  // audio_decode_reason 与处理分支处注释），不是没填的占位值。
  uint64_t audio_frames_in = 0;
  uint64_t audio_bytes_in = 0;
  // 多分片音频不做重组，单独计数：少计会让「音频到底有没有在流」看不出来
  uint64_t audio_multi_frag = 0;
  int64_t audio_last_pts_us = 0;
};

void cleanup_stale(std::map<uint32_t, FrameAcc>& pending, int64_t now_us, Counters& c) {
  for (auto it = pending.begin(); it != pending.end();) {
    if (now_us - it->second.first_seen_us > 2'000'000) {
      ++c.frames_in;  // 帧在"完成或超时"处结算，重复首片/缺首片都不会再算错
      g_frames_in_total.fetch_add(1, std::memory_order_relaxed);
      ++c.frames_dropped;
      it = pending.erase(it);
    } else {
      ++it;
    }
  }
}

const char* kStageNames[6] = {"recv", "buffer", "decode", "op", "encode", "send"};

// 某一段的 p 分位（ms）。没有样本时返回 JSON null：事件里不能拿 0 冒充
// 「这一段真的花了 0 ms」（transcode 关闭时 encode 段本来就没被采样）。
json stage_ms(const Ring& r, double p) {
  if (r.size() == 0) return nullptr;
  return double(r.pct(p)) / 1000.0;
}

// 进程启动时刻（obs.fetch procs 的 uptime 用）。由 main 第一行写入：
// 过去用函数内 static，结果"第一次问 uptime"的时候才初始化 → 恒为 0，
// 一个假读数比没有读数更坏。
int64_t g_process_start_us = 0;

int64_t process_uptime_ms() {
  if (g_process_start_us <= 0) return 0;
  return (cue::now_us() - g_process_start_us) / 1000;
}

// 控制面读的指标快照（metrics.query / obs.fetch frames 的数据源）。
//
// 与 METRICS 行是**同一份 JSON**（键名一字不改）：UDS 查到的数与 stdout 每秒那行
// 必须同口径，否则"同一时刻两个读数"迟早对不上。没有样本时写 samples=0 + 原因，
// 绝不拿默认值 0 冒充"fps=0"。
class MetricsView {
 public:
  void set(json metrics, nlohmann::ordered_json counters) {
    std::lock_guard<std::mutex> lk(mu_);
    metrics_ = std::move(metrics);
    counters_ = std::move(counters);
    updated_us_ = cue::now_us();
    ++samples_;
    times_.push_back(updated_us_);
    while (times_.size() > kHist) times_.pop_front();
  }

  json query(int window_ms) const {
    std::lock_guard<std::mutex> lk(mu_);
    json out = metrics_.is_object() ? metrics_ : json::object();
    const int64_t window_us = int64_t(window_ms) * 1000;
    std::size_t in_window = 0;
    for (const int64_t t : times_) {
      if (updated_us_ - t <= window_us) ++in_window;
    }
    out["samples"] = samples_;
    out["t_us"] = updated_us_;
    out["window_ms"] = window_ms;
    // samples_in_window 是从**报点时刻**数的，不是从计数器的取值区间算的：
    // cued 的计数器是"本次会话累计"，窗口内能做的是"这段窗口里采过几个点"。
    out["samples_in_window"] = in_window;
    out["available_window_ms"] = times_.empty() ? 0 : (updated_us_ - times_.front()) / 1000;
    // 帧水位：不是"窗口内的值"，是进程累计值。调用方自己差分（高频轮询时它比
    // samples_in_window 细得多）。
    out["frames_in_total"] = g_frames_in_total.load(std::memory_order_relaxed);
    out["frames_out_total"] = g_frames_out_total.load(std::memory_order_relaxed);
    if (samples_ == 0) {
      out["unavailable_reason"] = "还没有任何 metric.sample（启动不足 1 s 或没有媒体流）";
    }
    return out;
  }

  nlohmann::ordered_json counters() const {
    std::lock_guard<std::mutex> lk(mu_);
    return counters_;
  }

 private:
  static constexpr std::size_t kHist = 60;  // 最近 60 个报点（≈60 s）
  mutable std::mutex mu_;
  json metrics_ = json::object();
  nlohmann::ordered_json counters_{};
  int64_t updated_us_ = 0;
  uint64_t samples_ = 0;
  std::deque<int64_t> times_;
};

// 进程自述（obs.fetch procs）：pid / RSS / 线程数 / 运行时长 + 编码档位。
// 注入值（inject_drop / inject_ingress_drop / send_mbps / queue_fps）**故意不出现**：
// agent 必须从指标推断根因，把"标准答案"写进观测等于给它抄（沿用旧 observation(procs) 的约定）。
nlohmann::ordered_json procs_json(const cue::pipeline::ParamStore& params) {
  nlohmann::ordered_json out;
  out["pid"] = static_cast<long long>(::getpid());
  out["running"] = true;
  long rss_pages = -1;
  if (FILE* f = std::fopen("/proc/self/statm", "r")) {
    long total_pages = 0;
    if (std::fscanf(f, "%ld %ld", &total_pages, &rss_pages) != 2) rss_pages = -1;
    std::fclose(f);
  }
  if (rss_pages >= 0) {
    const long page_kb = ::sysconf(_SC_PAGESIZE) / 1024;
    out["rss_kb"] = static_cast<long long>(rss_pages) * page_kb;
  } else {
    out["rss_kb"] = nullptr;  // 读不到写 null：0 KB 会被读成"没有内存"
  }
  long threads = -1;
  if (FILE* f = std::fopen("/proc/self/status", "r")) {
    char line[256];
    while (std::fgets(line, sizeof line, f) != nullptr) {
      if (std::strncmp(line, "Threads:", 8) == 0) {
        threads = std::strtol(line + 8, nullptr, 10);
        break;
      }
    }
    std::fclose(f);
  }
  if (threads >= 0) {
    out["threads"] = threads;
  } else {
    out["threads"] = nullptr;
  }
  out["uptime_ms"] = process_uptime_ms();
  const json enc = params.encoder_json();
  for (auto it = enc.begin(); it != enc.end(); ++it) out[it.key()] = it.value();
  if (auto* ctl = g_control.load(std::memory_order_acquire)) {
    out["control_channel"] = ctl->channel_tag();
    out["control_ops"] = ctl->ops();
  } else {
    // 控制通道没建起来：如实写 stdout，而不是留空让人以为没这个概念
    out["control_channel"] = "stdout";
    out["control_ops"] = 0;
  }
  return out;
}

void report(const Ring rings[6], Counters& c, int64_t now_us, int64_t& t_last,
            uint64_t& frames_prev, const cue::media::JitterBufferStats& jb, MetricsView& view) {
  const double dt = double(now_us - t_last) / 1e6;
  const uint64_t df = c.frames_out - frames_prev;
  const double fps = double(df) / dt;
  // 人读行先拼成字符串再一次性写出：同一份文本还要喂给 obs.fetch(logs)，
  // 各 printf 一次的话两处格式必然漂移（改了一处忘了另一处）。
  std::ostringstream human;
  human.setf(std::ios::fixed);
  human << "[cued] fps=" << std::setprecision(1) << fps
        << " in=" << c.frames_in << " out=" << c.frames_out << " drop=" << c.frames_dropped
        << " inj=" << c.injected_drops << " bkt=" << c.budget_drops
        << " serr=" << c.send_errors << " enc_err=" << c.encode_errors
        << " dec=" << c.frames_decoded << " dec_err=" << c.decode_errors
        << " dec_drop=" << c.decode_drops << " rej=" << c.rejected
        << " aud=" << c.audio_frames_in << " MB=" << std::setprecision(2)
        << double(c.bytes_in) / (1024.0 * 1024.0);
  for (int i = 0; i < 6; ++i) {
    if (rings[i].size() == 0) {
      human << "  " << kStageNames[i] << "=-";
    } else {
      human << std::setprecision(3) << "  " << kStageNames[i] << "="
            << double(rings[i].pct(0.50)) / 1000.0 << "/" << double(rings[i].pct(0.95)) / 1000.0
            << "/" << double(rings[i].pct(0.99)) / 1000.0;
    }
  }
  human << " ms(p50/p95/p99)";
  // 抖动缓冲的状态单列：jb_depth 是"现在压着几帧"，jb_skipped/jb_late 是累计计数，
  // 与六段一样每秒一行，agent 才能把「buffer 段涨」和「队列在涨」关联起来。
  human << "  jb depth=" << jb.depth << " lvl=" << jb.level << " tgt=" << jb.target_frames
        << " sw=" << jb.switches << " skip=" << jb.skipped << " late=" << jb.late_dropped;
  const std::string human_line = human.str();
  std::fwrite(human_line.data(), 1, human_line.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
  if (auto* ctl_log = g_control.load(std::memory_order_acquire)) ctl_log->push_log(human_line);
  // 结构化指标行：process_control 只认这一行（严格整行前缀 "METRICS "），不再靠裸子串
  // 匹配 "in="/"dec="（cued 多打一个 frames_in= 就会串味，review #29）。
  // 六段里未被采样的段写 null：报告里显示 n/a，不能被 0 冒充实测。
  json m{{"fps", double(df) / dt},
         {"frames_in", c.frames_in},
         {"frames_out", c.frames_out},
         {"dropped", c.frames_dropped},
         {"injected", c.injected_drops},
         {"decoded", c.frames_decoded},
         {"decode_errors", c.decode_errors},
         {"rejected", c.rejected},
         {"budget_drops", c.budget_drops},
         {"send_errors", c.send_errors},
         {"encode_errors", c.encode_errors},
         {"encode_delays", c.encode_delays},
         {"decode_drops", c.decode_drops},
         {"queue_dropped", c.queue_dropped},
         // 音频（FR-DEV.6）：收包侧只计数。audio_decoded=false 与原因一起进指标行，
         // 因为「音频包在流」和「音频被解出来能播」是两件事，混成一句就是谎报。
         // 注意 MB 仍然只算视频载荷（bytes_in），音频字节单列，两类带宽不混在一个数里。
         {"audio_frames_in", c.audio_frames_in},
         {"audio_bytes_in", c.audio_bytes_in},
         {"audio_multi_frag", c.audio_multi_frag},
         {"audio_last_pts_us", c.audio_last_pts_us},
         {"audio_decoded", false},
         // 真解码 = libavcodec opus 解码器 + 下行音频数据报类型 + 播放侧，是独立的一条链路；
         // 本批只做「收、计、可见」，原因显式写进指标，不做静默降级。
         {"audio_decode_reason", "opus_decode_not_implemented"},
         // 抖动缓冲（FR-PIPE.2）：cued 侧是固定档，所以 jb_level 恒为 0、jb_switches 恒为 0；
         // 深度/跳帧/迟到是真实计数。它们与 buffer 段一起构成"抖动缓冲到底在干什么"的证据。
         {"jb_depth", jb.depth},
         {"jb_level", jb.level},
         {"jb_switches", jb.switches},
         {"jb_target_frames", jb.target_frames},
         {"jb_skipped", jb.skipped},
         {"jb_late", jb.late_dropped}};
  for (int i = 0; i < 6; ++i) {
    if (rings[i].size() == 0) {
      m[kStageNames[i]] = nullptr;
    } else {
      m[kStageNames[i]] = json{{"p50", double(rings[i].pct(0.50)) / 1000.0},
                               {"p95", double(rings[i].pct(0.95)) / 1000.0},
                               {"p99", double(rings[i].pct(0.99)) / 1000.0}};
    }
  }
  // 控制面可观测字段（FR-PIPE.5）：通道类型与累计处理的控制请求数。
  // 降级时必须写 stdout——「看起来有控制面、其实没有」是最坏的一种谎报。
  const auto* ctl = g_control.load(std::memory_order_acquire);
  const char* ctl_tag = ctl != nullptr ? ctl->channel_tag() : "stdout";
  const uint64_t ctl_ops = ctl != nullptr ? ctl->ops() : 0;
  m["control_channel"] = ctl_tag;
  m["control_ops"] = ctl_ops;
  const std::string line = "METRICS " + m.dump();
  std::fwrite(line.data(), 1, line.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
  // 控制面读同一份快照（metrics.query / obs.fetch frames）：UDS 与 stdout 两个读数
  // 因此永远是同一次 report() 的产物，不会出现"刚改完参数，两边数字不一样"。
  view.set(m, nlohmann::ordered_json{{"frames_in", c.frames_in},
                                     {"frames_out", c.frames_out},
                                     {"dropped", c.frames_dropped},
                                     {"injected", c.injected_drops},
                                     {"decoded", c.frames_decoded},
                                     {"decode_errors", c.decode_errors},
                                     {"rejected", c.rejected},
                                     {"budget_drops", c.budget_drops},
                                     {"send_errors", c.send_errors},
                                     {"encode_errors", c.encode_errors},
                                     {"decode_drops", c.decode_drops},
                                     {"audio_frames_in", c.audio_frames_in}});
  // 事件流里的同一份数字（SSE / 前端 / agent 事件缓存用）。未采样的段必须是 null。
  json sample{{"fps", fps},
              {"frames_in", c.frames_in},
              {"frames_out", c.frames_out},
              {"dropped", c.frames_dropped},
              {"injected", c.injected_drops},
              {"rejected", c.rejected},
              {"serr", c.send_errors},
              {"audio_frames_in", c.audio_frames_in},
              {"audio_decoded", false},
              // recv 段：数据报到达 → 本帧重组完成。此前 rings[0] 一直在算却没进 METRICS，
              // 六段里只有五段可读（FR-PIPE.6 要求六段都可读）。
              // 每段都给 p50/p95/p99 三档：FR-PIPE.6 的原文就是三档，
              // 只给 p50/p99 会让"长尾"和"中等偏慢"分不开。
              {"recv_p50", stage_ms(rings[0], 0.50)},
              {"recv_p95", stage_ms(rings[0], 0.95)},
              {"recv_p99", stage_ms(rings[0], 0.99)},
              {"buffer_p50", stage_ms(rings[1], 0.50)},
              {"buffer_p95", stage_ms(rings[1], 0.95)},
              {"buffer_p99", stage_ms(rings[1], 0.99)},
              {"jb_depth", jb.depth},
              {"jb_skipped", jb.skipped},
              {"jb_late", jb.late_dropped},
              {"jb_level", jb.level},
              {"decode_p50", stage_ms(rings[2], 0.50)},
              {"decode_p95", stage_ms(rings[2], 0.95)},
              {"decode_p99", stage_ms(rings[2], 0.99)},
              {"op_p50", stage_ms(rings[3], 0.50)},
              {"op_p95", stage_ms(rings[3], 0.95)},
              {"op_p99", stage_ms(rings[3], 0.99)},
              {"encode_p50", stage_ms(rings[4], 0.50)},
              {"encode_p95", stage_ms(rings[4], 0.95)},
              {"encode_p99", stage_ms(rings[4], 0.99)},
              {"send_p50", stage_ms(rings[5], 0.50)},
              {"send_p95", stage_ms(rings[5], 0.95)},
              {"send_p99", stage_ms(rings[5], 0.99)}};
  emit_event("metric.sample", sample);
  frames_prev = c.frames_out;
  t_last = now_us;
}

// CLI 解析：不用 std::stoi/std::stod —— 非法值抛出的异常会穿透 main（SPEC §9 禁止
// 异常穿透模块边界）。from_chars + 范围校验，失败打一行原因并以退出码 2 结束（review #22）。
bool parse_int(std::string_view s, long long lo, long long hi, long long& out) {
  if (s.empty()) return false;
  long long v = 0;
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || p != s.data() + s.size()) return false;
  if (v < lo || v > hi) return false;
  out = v;
  return true;
}

bool parse_double(std::string_view s, double lo, double hi, double& out) {
  if (s.empty()) return false;
  double v = 0;
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || p != s.data() + s.size()) return false;
  if (!(v >= lo && v <= hi)) return false;  // 同时挡掉 NaN
  out = v;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  g_process_start_us = cue::now_us();
  uint16_t ws_port = 9000;
  // WS 监听地址默认只回环：cued 没有鉴权，默认对外等于把解码/转码入口交给整个网络。
  // 跨机演示必须显式 --bind 0.0.0.0（并在报告/README 里说明）。
  std::string bind_addr = "127.0.0.1";
  std::string probe_host = "127.0.0.1";
  uint16_t probe_port = 9001;

  bool transcode_enabled = false;
  int out_w = 0;
  int out_h = 0;
  int out_kbps = 1500;
  int out_fps = 30;
  int out_gop = 60;
  int out_threads = 1;  // x264 线程数：2 vCPU 下默认 1，给解码与网络留核（review #33）
  std::string preset = "veryfast";
  double send_mbps = 10.0;
  double inject_drop = 0.0;
  // 上行（浏览器 → cued）丢包：模拟上行链路的包丢失。与出向注入分开，是因为两者在
  // cued 的指标上留下的是**不同**的证据（出向丢包只涨 injected；上行丢包还会让帧在
  // 重组超时处被结算成 dropped，frames_out 因此掉下来）。
  double inject_ingress_drop = 0.0;
  // 发送侧排队：>0 时帧不立即发出，而是按这个节奏排空（丢包策略换成了排队）。
  // 0 = 关（默认，保持 SPEC §7.2 的「令牌桶不足就丢」语义）。
  double queue_fps = 0.0;
  uint64_t inject_seed = 42;
  std::string inject_trace;
  bool fec_enabled = true;
  // 抖动缓冲（FR-PIPE.2）。默认值刻意取小：目标深度 2 帧 = 1 个帧间隔（30 fps ≈ 33 ms）的
  // 驻留，等待预算 40 ms 只用来兜「缺帧时要不要再等一会儿」。把这两个数调大能把乱序纠正
  // 得更彻底，代价是端到端延迟线性上升——所以默认不下"几百 ms"那种手。
  bool jitter_enabled = true;
  long long jitter_max_frames = 8;     // 队列容量硬上限（帧）
  long long jitter_budget_ms = 40;     // 缺帧等待预算
  long long jitter_target_frames = 1;  // 固定目标深度（帧）；1 = 不驻留，只靠空洞预算纠乱序

  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto next = [&]() -> std::string_view {
      return (i + 1 < argc) ? std::string_view(argv[++i]) : std::string_view();
    };
    auto bad = [&](std::string_view val) {
      std::fprintf(stderr, "[cued] 参数 %.*s 非法: '%.*s'\n", static_cast<int>(a.size()), a.data(),
                   static_cast<int>(val.size()), val.data());
      std::exit(2);
    };
    auto want_int = [&](long long lo, long long hi) {
      long long v = 0;
      const auto val = next();
      if (!parse_int(val, lo, hi, v)) bad(val);
      return v;
    };
    if (a == "--ws-port") {
      ws_port = static_cast<uint16_t>(want_int(1, 65535));
    } else if (a == "--bind") {
      const auto val = next();
      if (val.empty()) bad(val);
      beast::error_code bec;
      net::ip::make_address(std::string(val), bec);  // 非法地址在启动前就拒掉
      if (bec) bad(val);
      bind_addr = std::string(val);
    } else if (a == "--probe-host") {
      const auto val = next();
      if (val.empty()) bad(val);
      probe_host = std::string(val);
    } else if (a == "--probe-port") {
      probe_port = static_cast<uint16_t>(want_int(1, 65535));
    } else if (a == "--transcode") {
      const auto val = next();
      if (val != "on" && val != "off") bad(val);
      transcode_enabled = (val == "on");
    } else if (a == "--width") {
      out_w = static_cast<int>(want_int(0, 4096));
    } else if (a == "--height") {
      out_h = static_cast<int>(want_int(0, 4096));
    } else if (a == "--bitrate") {
      out_kbps = static_cast<int>(want_int(1, 20000));
    } else if (a == "--fps") {
      out_fps = static_cast<int>(want_int(1, 240));
    } else if (a == "--gop") {
      out_gop = static_cast<int>(want_int(1, 600));
    } else if (a == "--threads") {
      out_threads = static_cast<int>(want_int(1, 16));
    } else if (a == "--preset") {
      const auto val = next();
      if (val.empty()) bad(val);
      preset = std::string(val);
    } else if (a == "--send-mbps") {
      const auto val = next();
      if (!parse_double(val, 0.1, 10000.0, send_mbps)) bad(val);
    } else if (a == "--inject-drop") {
      // 旧实现用 stod + static_cast<uint64_t>，负数会变成巨大值（UB）
      const auto val = next();
      if (!parse_double(val, 0.0, 100.0, inject_drop)) bad(val);
    } else if (a == "--inject-ingress-drop") {
      const auto val = next();
      if (!parse_double(val, 0.0, 100.0, inject_ingress_drop)) bad(val);
    } else if (a == "--queue-fps") {
      const auto val = next();
      if (!parse_double(val, 0.0, 240.0, queue_fps)) bad(val);
    } else if (a == "--inject-seed") {
      const auto val = next();
      if (val.empty()) bad(val);
      unsigned long long v = 0;
      const auto [pe, ec] = std::from_chars(val.data(), val.data() + val.size(), v);
      if (ec != std::errc() || pe != val.data() + val.size()) bad(val);
      inject_seed = static_cast<uint64_t>(v);
    } else if (a == "--inject-trace") {
      const auto val = next();
      if (val.empty()) bad(val);
      inject_trace = std::string(val);
    } else if (a == "--jitter") {
      const auto val = next();
      if (val != "on" && val != "off") bad(val);
      jitter_enabled = (val == "on");
    } else if (a == "--jitter-max-frames") {
      jitter_max_frames = want_int(1, 64);
    } else if (a == "--jitter-budget-ms") {
      jitter_budget_ms = want_int(0, 2000);
    } else if (a == "--jitter-target-frames") {
      jitter_target_frames = want_int(1, 16);
    } else if (a == "--fec") {
      const auto val = next();
      if (val != "on" && val != "off") bad(val);
      fec_enabled = (val == "on");
    } else if (a == "--help") {
      std::printf(
          "cued --ws-port 9000 [--bind 127.0.0.1] --probe-host 127.0.0.1 --probe-port 9001\n"
          "     [--transcode on|off] [--width N --height N] [--bitrate kbps] [--fps N]\n"
          "     [--gop N] [--preset NAME] [--threads N] [--send-mbps F]\n"
          "     [--inject-drop 0..100] [--inject-ingress-drop 0..100] [--queue-fps N]\n"
          "     [--inject-seed N] [--inject-trace FILE] [--fec on|off]\n"
          "     [--jitter on|off] [--jitter-target-frames 2] [--jitter-max-frames 8]\n"
          "     [--jitter-budget-ms 40]\n"
          "  --bind 默认 127.0.0.1（只回环）；跨机演示才用 --bind 0.0.0.0，\n"
          "  并且必须自己限制网络可达性（cued 本身没有鉴权）。\n");
      return 0;
    } else {
      std::fprintf(stderr, "[cued] 未知参数: %.*s\n", static_cast<int>(a.size()), a.data());
      return 2;
    }
  }

  // 抖动缓冲配置：目标深度 > 容量上限时把容量抬上去，否则每帧进队就立刻被挤掉，
  // 那不是"配置得紧"，是配置矛盾（静默按错误行为跑是最难查的一类）。
  if (jitter_target_frames > jitter_max_frames) {
    std::fprintf(stderr,
                 "[cued] --jitter-target-frames %lld > --jitter-max-frames %lld："
                 "容量上限抬到目标深度\n",
                 jitter_target_frames, jitter_max_frames);
    jitter_max_frames = jitter_target_frames;
  }
  cue::media::JitterBufferConfig jb_cfg;
  jb_cfg.enabled = jitter_enabled;
  jb_cfg.max_frames = static_cast<std::size_t>(jitter_max_frames);
  jb_cfg.target_frames = static_cast<std::size_t>(jitter_target_frames);
  jb_cfg.budget_us = jitter_budget_ms * 1000;
  // 帧间隔由 PTS 增量的 EWMA 自估（浏览器/转码的帧率不一定是 --fps），这里只给起点。
  jb_cfg.nominal_interval_us = 1'000'000 / std::max<int>(out_fps, 1);

  // 启动参数 → 运行期参数对象。这些值同时出现在两个地方：
  //  - 命令行参数（评测脚本依赖 "启动即生效"：--inject-drop 5 就是带着故障上电）；
  //  - ParamStore 的初值（控制面之后可以改它，改了不重启）。
  // 两者必须一致，否则"启动时的档位"和"第一帧看到的档位"会分叉。
  cue::pipeline::RuntimeParams initial_params;
  initial_params.transcode = transcode_enabled;
  initial_params.width = out_w;
  initial_params.height = out_h;
  initial_params.bitrate_kbps = out_kbps;
  initial_params.fps = out_fps;
  initial_params.gop = out_gop;
  initial_params.threads = out_threads;
  initial_params.preset = preset;
  initial_params.send_mbps = send_mbps;
  initial_params.queue_fps = queue_fps;
  initial_params.inject_drop = inject_drop;
  initial_params.inject_ingress_drop = inject_ingress_drop;
  initial_params.inject_seed = inject_seed;

  cue::media::H264Decoder decoder;
  {
    std::string err;
    if (!decoder.open(err)) {
      std::fprintf(stderr, "[cued] decoder open failed: %s\n", err.c_str());
      return 1;
    }
  }

  // ---- 退出路径：SIGTERM/SIGINT 交给 sigwait 线程 ----
  // 不装信号处理函数：main 线程这时可能正阻塞在 ws.read()/accept()，而处理函数里也
  // 做不了 stdio（异步信号不安全）。改为在创建任何线程之前先把这两个信号屏蔽掉，
  // 由专门的线程同步取走，正常写一条 pipeline.stopped 事件再退出。
  sigset_t term_set;
  sigemptyset(&term_set);
  sigaddset(&term_set, SIGTERM);
  sigaddset(&term_set, SIGINT);
  if (pthread_sigmask(SIG_BLOCK, &term_set, nullptr) != 0) {
    std::fprintf(stderr, "[cued] pthread_sigmask failed\n");
    return 1;
  }
  std::thread([term_set] {
    int sig = 0;
    if (sigwait(&term_set, &sig) != 0) return;
    emit_event("pipeline.stopped",
               json{{"reason", g_stop_from_control.load() ? "control" : "signal"},
                    {"signal", sig},
                    {"pid", ::getpid()}});
    // 退出前把控制面收干净：_exit 不跑析构，不在这里 stop 就会把
    // /tmp/cue/<pid>/control.sock（和它那个 0700 目录）留在原地。
    if (auto* c = g_control.exchange(nullptr)) c->stop();
    std::fflush(nullptr);
    _exit(sig == SIGINT ? 130 : 143);
  }).detach();

  // ---- 控制面（T-05）：UDS 上的运行期控制 ----
  // 为什么放在 sigmask 之后：控制面的 accept/连接线程会继承主线程的信号屏蔽，
  // SIGTERM 必须继续留给 sigwait 线程——否则 pipeline.stop 会把进程直接打死，
  // 连一条 pipeline.stopped 事件都来不及打。
  //
  // 为什么参数对象与快照都在这里：写者只有控制线程（改 ParamStore），读者只有媒体
  // 循环（每帧取一次快照）。编码器/解码器/WS 会话永远只有媒体线程在碰，所以
  // 「改参数不断流」是结构上成立的，而不是靠"小心一点"。
  cue::pipeline::ParamStore params(initial_params);
  MetricsView view;

  cue::pipeline::ControlPlane::Providers ctl_providers;
  ctl_providers.metrics = [&view](int window_ms) { return view.query(window_ms); };
  ctl_providers.frames = [&view] { return view.counters(); };
  ctl_providers.procs = [&params] { return procs_json(params); };
  // 控制面产生的事件必须走 cued 自己的事件出口（stdout 行 + 订阅推送），
  // 否则 agent/SSE 会看到「参数改了但事件流里没有」的分叉。
  ctl_providers.emit = [](const std::string& ev, const json& data) { emit_event(ev, data); };
  // pipeline.stop：延时线程只做"给自己发 SIGTERM"这一件事，事件由 sigwait 线程统一打
  // （延迟 50 ms 是为了让 ok 响应先离开 socket，见 control_plane.cpp）。
  ctl_providers.on_stop = [] {
    g_stop_from_control.store(true);
    ::kill(::getpid(), SIGTERM);
  };
  cue::pipeline::ControlPlane control(params, ctl_providers);
  // 先挂全局指针再 start：start 之后随时可能有请求进来，procs/emit 都要能用到它
  g_control.store(&control, std::memory_order_release);
  {
    // socket 路径按 pid 分目录：同机跑多个 cued（不同端口）也不会互相踩。
    // 目录由 ControlServer 建成 0700，其他用户既连不上也换不掉 socket 文件。
    const std::string ctl_path = "/tmp/cue/" +
                                 std::to_string(static_cast<long long>(::getpid())) +
                                 "/control.sock";
    std::string ctl_err;
    if (control.start(ctl_path, ctl_err)) {
      std::fprintf(stderr, "[cued] control channel %s -> %s\n", control.channel_detail(),
                   ctl_path.c_str());
      control.push_log(std::string("[cued] control channel ") + control.channel_detail() + " -> " +
                       ctl_path);
    } else {
      // 降级路径必须是**响的**：stderr 一行 + 一条事件 + 指标里的 control_channel=stdout。
      // start() 仍然成功返回——"控制面没建起来"不等于"媒体管线不能用"，但 harness 会从
      // 指标里看到降级，并明确拒绝"免重启改参数"（而不是退回去偷偷重启 cued）。
      std::fprintf(stderr, "[cued] 控制通道不可用（降级到 stdout 模式）：%s\n", ctl_err.c_str());
      emit_event("control.degraded", json{{"reason", ctl_err}, {"channel", "stdout"}});
      control.push_log("[cued] control degraded to stdout: " + ctl_err);
    }
  }

  cue::media::TranscodeChain chain;
  bool chain_ready = false;
  // 编码器/发送侧配置的"已应用版本"：只有媒体线程读写，所以用普通变量。
  //  - last_gen：令牌桶速率的已应用版本（改速率不需要重建任何东西）；
  //  - last_enc_gen：编码参数的已应用版本（变了就关编码器，下一帧懒重建）；
  //  - chain_failed_gen：上一代配置失败的版本号（避免每帧重试同一份坏配置刷屏）。
  //    它的初值必须是哨兵：generation 从 0 起，若这里也用 0，第一帧就会把 0 当成
  //    "这一代已经失败过"而跳过配置——实测症状是 transcode=on 却恒为透传、
  //    encode 段全 null（这一版真踩过）。
  // last_* 的初值 = 初始 generation（0）：第一帧不必重建，只需按初值配置一次。
  constexpr uint64_t kNoGeneration = ~0ULL;
  uint64_t last_gen = 0;
  uint64_t last_enc_gen = 0;
  uint64_t chain_failed_gen = kNoGeneration;
  // 10 Mbps / 64 包突发（SPEC §7.2）：包速率按 1224 B/包折算，头开销一并计入
  TokenBucket bucket(send_mbps * 1e6);

  net::io_context ioc;
  beast::error_code ec;
  const auto bind_ip = net::ip::make_address(bind_addr, ec);
  if (ec) {
    std::fprintf(stderr, "[cued] --bind 地址非法: %s\n", bind_addr.c_str());
    return 2;
  }
  tcp::acceptor acc(ioc);
  acc.open(bind_ip.is_v6() ? tcp::v6() : tcp::v4(), ec);
  acc.set_option(net::socket_base::reuse_address(true), ec);
  acc.bind(tcp::endpoint(bind_ip, ws_port), ec);
  acc.listen(net::socket_base::max_listen_connections, ec);
  if (ec) {
    std::fprintf(stderr, "[cued] listen %s:%u failed: %s\n", bind_addr.c_str(), ws_port,
                 ec.message().c_str());
    return 1;
  }
  if (!bind_ip.is_loopback()) {
    // 默认回环，显式对外时把风险说清楚：cued 不做任何鉴权
    std::fprintf(stderr,
                 "[cued] 警告：WS 监听 %s:%u（对外可达，cued 无鉴权），请自行限制网络访问\n",
                 bind_addr.c_str(), ws_port);
  }
  // 用原生 fd：主线程 sendto，NACK 线程 recvfrom（同一个 UDP socket 全双工）
  const int usock = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (usock < 0) {
    std::fprintf(stderr, "[cued] udp socket failed\n");
    return 1;
  }
  sockaddr_in probe_addr{};
  probe_addr.sin_family = AF_INET;
  probe_addr.sin_port = htons(probe_port);
  ::inet_pton(AF_INET, probe_host.c_str(), &probe_addr.sin_addr);
  // 注入不再是对象状态：每条判定都从参数快照里的 (seed, pct) 现算（见 inject_drop），
  // 所以控制面改注入值立刻生效，不需要重启进程。上行注入用同一 seed 的派生值：
  // 两条链路的丢弃序列互不相关，但同一 (seed, pct) 仍然完全可复现。
  FILE* trace_fp = nullptr;
  if (!inject_trace.empty()) {
    trace_fp = std::fopen(inject_trace.c_str(), "w");
    if (trace_fp) {
      std::setvbuf(trace_fp, nullptr, _IONBF, 0);  // 进程被 kill 时也要保证 trace 已落盘
    } else {
      std::fprintf(stderr, "[cued] cannot open trace %s\n", inject_trace.c_str());
    }
  }
  RetransmitCache xmit_cache;

  // NACK 重传线程：阻塞在 poll(200ms) 上而不是裸 recvfrom——退出路径才能
  // stop_nack=true 后 join()；旧实现从不 join，主循环哪天能正常退出就会被
  // std::thread 析构里的 std::terminate 打死（review #21）。
  std::atomic<bool> stop_nack{false};
  std::atomic<uint64_t> nack_resends{0};
  std::atomic<uint32_t> cur_session{0};  // 当前连接的会话号：NACK 线程据此过滤（review #20）
  std::thread nack_thread([&] {
    std::vector<uint8_t> nbuf(2048);
    RateLimitedLog err_log;
    while (!stop_nack.load(std::memory_order_relaxed)) {
      pollfd pfd{usock, POLLIN, 0};
      const int pr = ::poll(&pfd, 1, 200);
      if (pr < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (pr == 0) continue;  // 超时：回到循环头检查停止位
      sockaddr_in from{};
      socklen_t fl = sizeof(from);
      const ssize_t n = ::recvfrom(usock, nbuf.data(), nbuf.size(), MSG_DONTWAIT,
                                   reinterpret_cast<sockaddr*>(&from), &fl);
      if (n < static_cast<ssize_t>(cue::proto::kDatagramHeaderSize)) continue;
      // 只服务本会话的 probe：任意源都能触发重传就是一个反射放大器（review #20）
      if (from.sin_addr.s_addr != probe_addr.sin_addr.s_addr ||
          from.sin_port != probe_addr.sin_port) {
        continue;
      }
      auto nh = cue::proto::decode_datagram({nbuf.data(), static_cast<size_t>(n)});
      if (!nh) continue;
      if (nh->type == static_cast<uint8_t>(cue::proto::DownType::StatsReq)) {
        // SPEC §7.1 时钟对齐：probe 发下行回环请求（载荷 = 它的发戳），这里原样回显发戳
        // 并附上自己的时钟；跨机部署的端到端延迟由此有确定口径（review #9）。
        if (n < static_cast<ssize_t>(cue::proto::kDatagramHeaderSize + 8)) continue;
        cue::proto::DatagramHeader sh;
        sh.type = static_cast<uint8_t>(cue::proto::DownType::StatsReq);
        sh.session_id = nh->session_id;
        const auto shb = cue::proto::encode(sh);
        std::vector<uint8_t> rep(shb.begin(), shb.end());
        const std::size_t base = rep.size();
        rep.resize(base + 16, 0);
        std::memcpy(rep.data() + base, nbuf.data() + cue::proto::kDatagramHeaderSize, 8);
        cue::proto::detail::put_u64(rep.data() + base + 8,
                                    static_cast<uint64_t>(cue::now_us()));
        if (::sendto(usock, rep.data(), rep.size(), 0, reinterpret_cast<const sockaddr*>(&from),
                     fl) < 0) {
          err_log.log(cue::now_us(), std::string("stats 回包 sendto 失败: ") +
                                         std::strerror(errno));
        }
        continue;
      }
      if (nh->type != static_cast<uint8_t>(cue::proto::DownType::Nack)) continue;
      // 会话号校验（review #20）：probe 回带下行头里的 session_id
      const uint32_t sid = nh->session_id;
      const uint32_t cur = cur_session.load(std::memory_order_relaxed);
      if (sid != 0 && cur != 0 && sid != cur) continue;
      const auto entries = cue::proto::decode_nack_entries(
          {nbuf.data() + cue::proto::kDatagramHeaderSize,
           static_cast<std::size_t>(n) - cue::proto::kDatagramHeaderSize});
      for (const auto& e : entries) {
        auto pkt = xmit_cache.find(e.frame_id, e.frag_index);
        if (!pkt) continue;
        // SPEC §7.2 下行 flags bit1：重传包必须能被接收端识别成重传
        if (pkt->size() > 2) {
          (*pkt)[2] = static_cast<uint8_t>((*pkt)[2] | cue::proto::kDownFlagRetransmit);
        }
        const ssize_t sn = ::sendto(usock, pkt->data(), pkt->size(), 0,
                                    reinterpret_cast<const sockaddr*>(&from), fl);
        if (sn < 0) {
          err_log.log(cue::now_us(), std::string("nack 重传 sendto 失败: ") + std::strerror(errno));
          continue;
        }
        nack_resends.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });
  // 退出路径的 RAII：即使将来给主循环加了正常退出条件，也不会漏 join
  struct NackGuard {
    std::atomic<bool>& stop;
    std::thread& th;
    ~NackGuard() {
      stop.store(true);
      if (th.joinable()) th.join();
    }
  } nack_guard{stop_nack, nack_thread};

  std::fprintf(stderr, "[cued] ws %s:%u  udp -> %s:%u (M2a: decode + 6-stage profile)\n",
               bind_addr.c_str(), ws_port, probe_host.c_str(), probe_port);

  // codec 配置跨连接保留：cued 重启后缓存为空，直到浏览器再发一包 0x03。
  CodecConfig codec_cfg;

  // pipeline.started：一次 cued 生命周期的起点（重启会再出现一条，带新的 pid）
  emit_event("pipeline.started",
             json{{"pid", ::getpid()},
                  {"ws_port", ws_port},
                  {"bind", bind_addr},
                  {"probe_host", probe_host},
                  {"probe_port", probe_port},
                  {"fps", out_fps},
                  {"bitrate_kbps", out_kbps},
                  {"gop", out_gop},
                  {"transcode", transcode_enabled ? "on" : "off"},
                  {"fec", fec_enabled ? "on" : "off"},
                  {"inject_drop", inject_drop},
                  {"inject_seed", inject_seed},
                  {"control_channel", control.channel_tag()},
                  {"control_socket", control.degraded()
                                         ? std::string()
                                         : "/tmp/cue/" + std::to_string(::getpid()) + "/control.sock"},
                  {"jitter", jitter_enabled ? "on" : "off"},
                  {"jitter_target_frames", jitter_target_frames},
                  {"jitter_max_frames", jitter_max_frames},
                  {"jitter_budget_ms", jitter_budget_ms}});
  // 带 --inject-drop 启动 = 这条管线上电时故障已经生效（agent 会看到 applied 而不是 cleared）
  if (inject_drop > 0.0) {
    emit_event("fault.applied", json{{"kind", "drop"},
                                     {"value", inject_drop},
                                     {"seed", inject_seed},
                                     {"scope", "cued_start"}});
  }

  for (;;) {
    tcp::socket sock(ioc);
    acc.accept(sock, ec);
    if (ec) continue;

    websocket::stream<tcp::socket> ws(std::move(sock));
    ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
    ws.read_message_max(8 * 1024 * 1024);
    ws.accept(ec);
    if (ec) {
      std::fprintf(stderr, "[cued] handshake failed: %s\n", ec.message().c_str());
      continue;
    }
    std::fprintf(stderr, "[cued] client connected\n");
    if (codec_cfg.have) {
      // 新连接（含断线重连）：把缓存的 SPS/PPS 原样回放，agent 的 observation 立刻
      // 能看到媒体元信息，不必等浏览器再发一包 0x03。
      emit_event("codec.config", codec_cfg.as_json(true));
    }
    // 新连接必须先丢参考帧：否则旧 SPS/PPS 与参考帧残留到下一个 IDR，表现为持续花屏（#30）
    decoder.flush();

    std::map<uint32_t, FrameAcc> pending;
    Counters c;
    Ring rings[6];
    uint32_t session_id = 1;
    int64_t t_last_report = cue::now_us();
    int64_t t_last_cleanup = cue::now_us();
    uint64_t frames_prev = 0;
    // 注入丢包判定用的包序号：每连接一个、跨帧单调递增。
    // 旧口径 frame_id*64+pk 在 1080p 关键帧（>63 分片）时会与 FEC 包 / 下一帧撞号，
    // 丢包不再独立同分布；FEC 冗余包与数据包共用这一序号空间。
    uint64_t pkt_seq = 0;
    // 上行注入用的包序号（独立空间）
    uint64_t in_seq = 0;
    RateLimitedLog err_log;  // 每连接一份：重启 cued 后限流状态跟着新会话走
    // 抖动缓冲：每连接一份。上一代的 PTS 基线与积压帧不能带进新会话，
    // 否则新流前几帧会被判成"迟到"、旧帧会被发到新 probe 上。
    cue::media::JitterBuffer jb(jb_cfg);

    // 发送侧排队（--queue-fps）：不足「一帧的发送时刻」就留在队列里，而不是丢包。
    // 队列深度对 agent 可见：frames_in（已收帧）减 frames_out（已发帧）就是积压深度，
    // 这一点与令牌桶丢包（budget_drops 涨、深度不涨）是可区分的两种故障证据。
    struct QueuedSend {
      std::vector<std::vector<uint8_t>> datagrams;
      int64_t due_us = 0;
    };
    std::deque<QueuedSend> sendq;
    int64_t send_due_us = 0;
    // 已应用的排队节奏（NaN = 还没应用过，第一帧只记录不清理）。
    // 排队节奏不再在连接建立时定死：每帧从参数快照算（fault.set kind=backlog 运行期可改）。
    double applied_queue_fps = std::numeric_limits<double>::quiet_NaN();
    constexpr std::size_t kSendQueueMax = 8192;  // 兜底：队列不能无限涨（约 4.5 min @30fps）

    // sendto 的返回值与 errno 不能丢：ENOBUFS/EMSGSIZE 会被算成「已发送」（review #16）
    auto send_dgram = [&](const std::vector<uint8_t>& d) -> bool {
      const ssize_t sn = ::sendto(usock, d.data(), d.size(), 0,
                                  reinterpret_cast<const sockaddr*>(&probe_addr),
                                  sizeof(probe_addr));
      if (sn < 0) {
        ++c.send_errors;
        err_log.log(cue::now_us(), std::string("sendto 失败: ") + std::strerror(errno));
        return false;
      }
      return true;
    };
    // 到期的排队帧按顺序发出；fps 因此由 --queue-fps 决定，而不是由「收到多少帧」决定
    auto drain_sendq = [&](int64_t now_us) {
      while (!sendq.empty() && sendq.front().due_us <= now_us) {
        bool ok = true;
        for (const auto& d : sendq.front().datagrams) {
          if (!send_dgram(d)) ok = false;
        }
        if (ok) {
          ++c.frames_out;
          g_frames_out_total.fetch_add(1, std::memory_order_relaxed);
        }
        sendq.pop_front();
      }
    };

    // 一帧「重组完成之后」的全部处理：解码 → 处理器链 → 编码 → 令牌桶 → FEC → 发送。
    // 抽成 lambda 是因为抖动缓冲在**三个不同时刻**放行帧（push 当场 / 周期 tick / 断连接 flush），
    // 三条入口必须走同一条路径，否则「jb 放行的帧」和「直接处理的帧」会长出两套行为。
    auto process_frame = [&](cue::media::JitterFrame&& jf) {
      const uint64_t pts_us = jf.pts_us;
      const uint32_t frame_id = jf.frame_id;
      bool keyframe = jf.keyframe;
      // 每帧取一次参数快照（几十字节的拷贝）：整帧内编码器、注入、排队看到的是**同一个
      // 版本**，不会出现「bitrate 换了但 gop 还是旧的」这种半截配置。
      const auto snap = params.snapshot();
      const cue::pipeline::RuntimeParams& rp = snap.params;
      if (snap.generation != last_gen) {
        // 令牌桶速率原地改：速率是纯状态，改它不丢令牌、不打乱突发
        bucket.set_rate(rp.send_mbps * 1e6);
        last_gen = snap.generation;
      }
      if (snap.encoder_generation != last_enc_gen) {
        // 编码参数变了：先关掉编码器，下一帧懒重建。**WS 会话、解码器、抖动缓冲全都原地
        // 保留**——FR-PIPE.5「切换时不断流」就落在这里。注入类改动不会走这条路
        // （ParamStore 只在编码相关键上推进 encoder_generation）。
        const int64_t t_rebuild0 = cue::now_us();
        chain.close();
        chain_ready = false;
        chain_failed_gen = kNoGeneration;
        last_enc_gen = snap.encoder_generation;
        emit_event("pipeline.encoder_rebuild",
                   json{{"reason", "param_change"},
                        {"elapsed_us", cue::now_us() - t_rebuild0},
                        {"bitrate_kbps", rp.bitrate_kbps},
                        {"gop", rp.gop},
                        {"preset", rp.preset},
                        {"transcode", rp.transcode ? "on" : "off"}});
      }
      // 注入与排队参数每帧从快照现算：这两个值是"下一包怎么处理"，不涉及任何重建
      const uint64_t out_permille = permille_from_pct(rp.inject_drop);
      const double queue_period_us = rp.queue_fps > 0.0 ? 1e6 / rp.queue_fps : 0.0;
      if (std::isnan(applied_queue_fps) || rp.queue_fps != applied_queue_fps) {
        // 排队策略变了（例如 fault.set backlog=12 → 清除）：队列里的旧帧**按过期丢弃**。
        // 为什么不能留着慢慢排：队尾可能排着十几秒前的帧（每帧把 due 往后推一个周期），
        // 它们会和"此刻"的实时帧交错发出 → PTS 乱序 → 接收端抖动缓冲把延迟越推越高
        // （实测：改回 0 之后 probe 的 e2e p99 从 2.4 s 一路爬到 2.7 s，而 fps 反而冲到 42）。
        // 直播链路里十几秒前的帧没有发送价值，丢掉并记账比"按序补发"更诚实。
        if (!sendq.empty()) {
          const std::size_t n = sendq.size();
          sendq.clear();
          send_due_us = 0;
          // 只记 queue_dropped，不记 dropped：dropped 的语义是「媒体链路里丢掉的帧」
          // （重组超时/切片失败/令牌桶），而这里是**控制面切换策略**时主动丢弃的积压。
          // 两者的处置完全不同（一个要查网络/码控，一个是你刚改的排队节奏），混在一起
          // 会让「改了个参数」看起来像「链路在丢包」。
          c.queue_dropped += n;
          emit_event("pipeline.queue_flushed",
                     json{{"dropped", n},
                          {"from_fps", std::isnan(applied_queue_fps) ? json(nullptr)
                                                                     : json(applied_queue_fps)},
                          {"to_fps", rp.queue_fps}});
        }
        applied_queue_fps = rp.queue_fps;
      }
      // buffer 段：这一帧在抖动缓冲里真实等了多久（= 放行时刻 − 进队时刻）。
      // recv 段只含重组，两段从此不再同口径——旧实现两段都写重组耗时，buffer 等于没有。
      rings[1].push(jf.wait_us);

      const int64_t t_dec0 = cue::now_us();
      std::string derr;
      AVFrame* decoded = decoder.decode(jf.payload, derr);
      rings[2].push(cue::now_us() - t_dec0);
      if (decoded) {
        ++c.frames_decoded;
      } else if (!derr.empty()) {
        ++c.decode_errors;
      }

      const int64_t t_op0 = cue::now_us();
      std::vector<uint8_t> encoded_buf;
      std::span<const uint8_t> payload{jf.payload};
      bool skip_send = false;
      // 下行载荷的真实 codec：passthrough 时是上行声明的 codec，转码后恒为 H264。
      // probe 靠它决定「解码」还是「跳过」，所以必须跟着**实际发出的字节**走，
      // 而不是跟着上行包头走（转码路径一旦改成 H264，上行可能是别的 codec）。
      uint8_t out_codec = jf.codec;
      int64_t enc_us = 0;      // 编码段真实耗时（review #3：不再无条件记 0）
      bool enc_sampled = false;  // transcode 关闭时该段不采样，报告里显示 "-"

      if (rp.transcode && decoded) {
        if (!chain_ready && chain_failed_gen != snap.encoder_generation) {
          const int dw = rp.width > 0 ? rp.width : decoded->width;
          const int dh = rp.height > 0 ? rp.height : decoded->height;
          std::string cerr;
          // 重建耗时（close + configure）：这是「改编码参数」付出的全部停顿，
          // 事件里如实报出来（单位 µs），报告里"断流多少 ms"直接引用这个数。
          const int64_t t_cfg0 = cue::now_us();
          const bool cfg_ok =
              chain.configure(dw, dh, rp.bitrate_kbps, rp.fps, rp.gop, rp.preset, cerr, rp.threads);
          const int64_t cfg_us = cue::now_us() - t_cfg0;
          if (cfg_ok) {
            chain_ready = true;
            emit_event("pipeline.encoder",
                       json{{"phase", "configured"},
                            {"elapsed_us", cfg_us},
                            {"width", dw},
                            {"height", dh},
                            {"bitrate_kbps", rp.bitrate_kbps},
                            {"gop", rp.gop},
                            {"preset", rp.preset}});
          } else {
            // 配置失败**不**再偷偷把 transcode 改成 off（旧实现的静默降级）：
            // 参数仍按用户要求保持 on，失败按"这一代失败"记账，等参数再变时重试。
            chain_failed_gen = snap.encoder_generation;
            (void)cfg_us;
            ++c.encode_errors;
            emit_event("pipeline.encoder_error", json{{"reason", cerr}});
            err_log.log(cue::now_us(), "transcode configure failed: " + cerr);
          }
        }
        if (chain_ready) {
          std::string eerr;
          auto enc = chain.process(decoded, pts_us, eerr, &enc_us);
          enc_sampled = enc_us > 0;
          if (enc) {
            encoded_buf = std::move(enc->data);
            payload = encoded_buf;
            keyframe = enc->keyframe;
            out_codec = static_cast<uint8_t>(cue::proto::Codec::H264);
          } else {
            skip_send = true;
            if (enc.error() == cue::media::EncodeError::EncoderDelay) {
              ++c.encode_delays;  // 编码器内部延迟：正常路径，不是配置错误
            } else {
              // 尺寸/格式/编码器错误过去混在 nullopt 里"指标轻微变差"，现在必须可见（review #31）
              ++c.encode_errors;
              err_log.log(cue::now_us(), std::string("编码失败[") +
                                             cue::media::encode_error_name(enc.error()) + "]: " +
                                             eerr);
            }
          }
        }
      }
      // op 段只算 ProcessorChain 里除编码以外的部分（M2a 为 passthrough ≈0），
      // encode 段单列 —— 两段互不重叠，否则「op 含编码」会让六段口径名不副实。
      const int64_t t_op_end = cue::now_us();
      rings[3].push(std::max<int64_t>(0, t_op_end - t_op0 - enc_us));
      if (enc_sampled) rings[4].push(enc_us);

      const int64_t t_send0 = cue::now_us();
      std::vector<std::vector<uint8_t>> datagrams;
      cue::proto::SplitError split_err = cue::proto::SplitError::None;
      bool budget_ok = false;
      if (!skip_send) {
        datagrams = cue::proto::split_into_datagrams(payload, session_id, frame_id, pts_us,
                                                     keyframe, &split_err, out_codec);
        if (!datagrams.empty()) {
          // SPEC §7.2 令牌桶：按包计费（含 24 B 头）、64 包突发、不足时先丢非关键帧
          budget_ok = keyframe ? bucket.take_key(datagrams.size(), t_send0)
                               : bucket.take_nonkey(datagrams.size(), t_send0);
        }
      }
      if (skip_send) {
        ++c.frames_dropped;
      } else if (split_err != cue::proto::SplitError::None) {
        // 空帧 / 超过单帧 1024 分片：协议上限内发不出去，计 rejected 而不是静默丢（#34）
        ++c.rejected;
        ++c.frames_dropped;
        err_log.log(cue::now_us(),
                    std::string("帧无法切片: ") + cue::proto::split_error_reason(split_err));
      } else if (!budget_ok) {
        ++c.budget_drops;
        ++c.frames_dropped;
      } else {
        xmit_cache.put(frame_id, datagrams);

        // 注入丢包：判定序号必须是整条连接全局单调、且与 FEC 包共用一个序号空间。
        // 旧口径 frame_id*64+pk 在分片数 >64（720p 关键帧）时会与 FEC 包撞号，
        // 且 frame_id*64+64 == (frame_id+1)*64 会跨帧撞号 → 丢包不再独立同分布。
        // 注入在「进队之前」判定：排队模式换的是发送节奏，不是丢包策略。
        std::vector<std::vector<uint8_t>> outgoing;
        outgoing.reserve(datagrams.size() + 1);
        std::size_t pk = 0;
        for (const auto& d : datagrams) {
          const std::size_t cur_pk = pk++;
          if (should_drop_pkt(rp.inject_seed, out_permille, pkt_seq++)) {
            ++c.injected_drops;
            if (trace_fp) std::fprintf(trace_fp, "%u %zu\n", frame_id, cur_pk);
            continue;
          }
          outgoing.push_back(d);
        }

        // XOR-FEC：一帧的所有分片为一组，生成 1 个冗余包。
        // 载荷的字节序/字段顺序收在 wire.h（两端不再各写一份 memcpy，review #18）。
        if (fec_enabled && datagrams.size() > 1) {
          std::size_t maxlen = 0;
          for (auto& d : datagrams) {
            maxlen = std::max(maxlen, d.size() - cue::proto::kDatagramHeaderSize);
          }
          std::vector<uint8_t> parity(maxlen, 0);
          for (auto& d : datagrams) {
            const std::size_t plen = d.size() - cue::proto::kDatagramHeaderSize;
            for (std::size_t i = 0; i < plen; ++i) {
              parity[i] ^= d[cue::proto::kDatagramHeaderSize + i];
            }
          }
          cue::proto::DatagramHeader fh;
          fh.type = static_cast<uint8_t>(cue::proto::DownType::Fec);
          fh.session_id = session_id;
          fh.frame_id = frame_id;
          fh.frag_index = 0;
          fh.frag_count = static_cast<uint16_t>(datagrams.size());
          fh.pts_us = pts_us;
          // 冗余包本身不是可解码帧：显式不标注 codec，免得 probe 拿奇偶校验字节去解码
          fh.codec = cue::proto::kDownCodecUnspecified;
          const auto hdr = cue::proto::encode(fh);
          std::vector<uint8_t> fpkt(hdr.begin(), hdr.end());
          cue::proto::FecPayloadHeader ph;
          ph.base_frame_id = frame_id;
          ph.group_index = 0;  // 一帧一组：显式写 0，不再靠"零填充"隐式约定
          ph.member_count = static_cast<uint16_t>(datagrams.size());
          const auto phb = cue::proto::encode(ph);
          fpkt.insert(fpkt.end(), phb.begin(), phb.end());
          fpkt.insert(fpkt.end(), parity.begin(), parity.end());
          if (!should_drop_pkt(rp.inject_seed, out_permille, pkt_seq++)) {
            outgoing.push_back(std::move(fpkt));
          }
        }

        if (queue_period_us > 0.0) {
          // 排队模式：帧不立即发，按 --queue-fps 的节奏排空。队列涨 = 消费侧积压
          // （frames_in - frames_out 单调增长、dropped 不涨），与令牌桶丢包
          // （budget_drops 涨、队列不涨）是两种可区分的故障证据。
          if (sendq.size() >= kSendQueueMax) {
            ++c.budget_drops;
            ++c.frames_dropped;
            err_log.log(t_send0, "发送队列已满：积压超过上限，按丢帧处理");
          } else {
            const int64_t due = send_due_us > 0 ? send_due_us : t_send0;
            send_due_us = due + static_cast<int64_t>(queue_period_us);
            sendq.push_back(QueuedSend{std::move(outgoing), due});
            drain_sendq(t_send0);
          }
        } else {
          // 真实上线率：只要有一包没被内核接受，这一帧就不算发出去（review #16）
          bool all_sent = true;
          for (const auto& d : outgoing) {
            if (!send_dgram(d)) all_sent = false;
          }
          if (all_sent) {
            ++c.frames_out;
            g_frames_out_total.fetch_add(1, std::memory_order_relaxed);
          }
        }
      }
      rings[5].push(cue::now_us() - t_send0);
    };

    for (;;) {
      beast::flat_buffer buf;
      ws.read(buf, ec);
      if (ec) break;

      // 周期任务必须与「帧是否完成」解耦：丢尾片的帧走不到完成路径，若把清理挂在
      // 完成之后，pending 只增不减、2 s 阈值形同虚设（review #5）。这里每包都检查，
      // 500 ms 一次真正清理。
      {
        const int64_t now_tick = cue::now_us();
        if (now_tick - t_last_cleanup >= 500'000) {
          // 抖动缓冲的到期帧必须由时间驱动放行：只靠 push 驱动的话，缺帧把前端挡住时
          // 要等到下一个包才动，"等待预算"就变成了"等下一个包"（弱网下可能是几百 ms）。
          for (auto& ready : jb.poll(now_tick)) process_frame(std::move(ready));
          cleanup_stale(pending, now_tick, c);
          t_last_cleanup = now_tick;
        }
        // 报告同样不能挂在「帧完成」上：整帧持续丢尾片时 fps/丢帧仍要可见
        if (now_tick - t_last_report >= 1'000'000) {
          drain_sendq(now_tick);  // 报点前先把到期帧发出去
          // 报点前同样先放行抖动缓冲里的到期帧：否则这一秒的 jb_depth 会把"其实已经到期"
          // 的帧算成积压，buffer 段也会少采样几帧。
          for (auto& ready : jb.poll(now_tick)) process_frame(std::move(ready));
          c.decode_drops = decoder.stats().dropped;
          report(rings, c, now_tick, t_last_report, frames_prev, jb.stats(), view);
        }
      }

      const auto* p = static_cast<const uint8_t*>(buf.data().data());
      const std::size_t n = buf.data().size();

      if (ws.got_text()) {
        auto j = json::parse(std::string_view(reinterpret_cast<const char*>(p), n), nullptr, false);
        if (j.is_object() && j.value("op", "") == "ping") {
          json pong{{"op", "pong"}, {"t", j.value("t", static_cast<int64_t>(0))}, {"server_us", cue::now_us()}};
          ws.text(true);
          ws.write(net::buffer(pong.dump()), ec);
          ws.binary(true);
          if (ec) break;
        }
        continue;
      }

      // 控制类上行包（0x03 codec_config / 0x04 keyframe_req / 0x05 stats 能力上报）
      // 不带分片语义：发送端常把 frag_count 留 0，用放宽了分片校验的入口解
      // （其余边界仍然严格）。
      std::optional<cue::proto::MediaHeader> h;
      if (n >= 2 && p[0] == cue::proto::kMagicUp &&
          (p[1] == static_cast<uint8_t>(cue::proto::MediaType::CodecConfig) ||
           p[1] == static_cast<uint8_t>(cue::proto::MediaType::KeyframeReq) ||
           p[1] == static_cast<uint8_t>(cue::proto::MediaType::Stats))) {
        cue::proto::MediaHeader hc;
        if (cue::proto::validate_control({p, n}, hc) == cue::proto::MediaReject::None) h = hc;
      } else {
        h = cue::proto::decode_media({p, n});
      }
      if (!h) {
        ++c.rejected;  // 协议边界拒绝（wire.h 的 validate_media/validate_control 给出原因）
        continue;
      }

      if (h->type == static_cast<uint8_t>(cue::proto::MediaType::CodecConfig)) {
        // 缓存 SPS/PPS：新连接建立时原样回放给 agent 的 observation（见 accept 循环）
        const std::span<const uint8_t> body{p + cue::proto::kMediaHeaderSize, h->payload_len};
        codec_cfg.stream_id = h->stream_id;
        if (!parse_annexb_sps_pps(body, codec_cfg)) {
          const std::size_t cn = std::min<std::size_t>(body.size(), kMaxConfigRawBytes);
          codec_cfg.raw_hex = to_hex(body.data(), cn);
        } else {
          codec_cfg.raw_hex.clear();
        }
        codec_cfg.have = !codec_cfg.sps_hex.empty() || !codec_cfg.pps_hex.empty() ||
                         !codec_cfg.raw_hex.empty();
        emit_event("codec.config", codec_cfg.as_json(false));
        continue;
      }
      if (h->type == static_cast<uint8_t>(cue::proto::MediaType::KeyframeReq)) {
        // 只落事件流（M5 控制台可见"谁在请求关键帧"），暂不强制下发：
        // 强制 IDR 是编码侧（浏览器）的职责，cued 没有编码控制权。
        emit_event("keyframe.request", json{{"stream_id", h->stream_id},
                                            {"frame_id", h->frame_id},
                                            {"pts_us", h->pts_us}});
        continue;
      }
      if (h->type == static_cast<uint8_t>(cue::proto::MediaType::Stats)) {
        // 设备能力上报（FR-DEV.4）：浏览器把 VideoEncoder/AudioEncoder 的 isConfigSupported
        // 结果、enumerateDevices() 设备数与 UA 装进一条 0x05 控制包的 JSON 载荷。
        // 选 0x05 而不是新开一个 type：wire.h 里这个枚举早就留了 Stats 位、上行一直没人用，
        // 复用它不必改协议版本；载荷是自描述 JSON，收端不认识新键也不会坏。
        const std::string_view body(reinterpret_cast<const char*>(p) + cue::proto::kMediaHeaderSize,
                                    h->payload_len);
        auto caps = json::parse(body, nullptr, false);
        if (caps.is_object()) {
          caps["stream_id"] = h->stream_id;  // 谁报的（视频流 id）由收端补，发送端不必自报
          // 事件名不能由发送端随便定（上行来自浏览器，任何页面/扩展都能发字节）：
          // 只放行白名单，未知 kind 统一落 device.event，原值仍留在载荷里可查。
          const std::string kind = caps.value("kind", std::string("unknown"));
          const char* ev = "device.event";
          if (kind == "caps") ev = "device.caps";
          else if (kind == "codec.degraded") ev = "device.codec_degraded";
          else if (kind == "audio.disabled") ev = "device.audio_disabled";
          else if (kind == "audio.started") ev = "device.audio_started";
          emit_event(ev, caps);
        } else {
          ++c.rejected;  // 载荷不是 JSON 对象：这次上报作废，但不影响媒体流继续
        }
        continue;
      }
      if (h->type == static_cast<uint8_t>(cue::proto::MediaType::Audio)) {
        // 音频上行（FR-DEV.6）：**只喂计数，不解码、不下发**。
        // 之所以不假装处理：opus 真解码要 libavcodec opus 解码器 + 下行音频数据报类型
        // + 播放侧，是独立一条链路；本批验收只要「音频包能被收端看到」。
        // 20 ms 的 Opus 帧只有几百字节，远低于 16 KiB 分片上限，浏览器恒单分片，
        // 所以这里不做重组；见到多分片时单独计数（audio_multi_frag），
        // 让「音频在流但口径不完整」可见，而不是静默少计成 0。
        if (h->frag_count == 1) {
          ++c.audio_frames_in;
          c.audio_bytes_in += h->payload_len;
          c.audio_last_pts_us = static_cast<int64_t>(h->pts_us);
        } else {
          ++c.audio_multi_frag;
        }
        continue;
      }
      if (h->type != static_cast<uint8_t>(cue::proto::MediaType::Video)) continue;

      session_id = h->stream_id;
      cur_session.store(session_id, std::memory_order_relaxed);
      // 上行丢包注入：包在进入重组之前就丢了。单分片帧（本项目合成推流就是）整帧消失，
      // frames_in/frames_out 都不计它，于是只有 fps 掉下来——dropped 保持 0；
      // 多分片帧丢失非首片时会留在 pending 里，2 s 后由 cleanup_stale 结算成 dropped。
      // 两种形状都只涨 injected，与出向注入（fps 不动）正好可以区分开。
      // 上行注入：每包现取一次快照（30 fps 的帧数级开销；换来"改注入立刻生效"）。
      // 用同一个 seed 派生值：两条链路的丢弃序列互不相关，但同一 (seed, pct) 可复现。
      const auto isnap = params.snapshot();
      if (should_drop_pkt(isnap.params.inject_seed ^ 0xD1B54A32D192ED03ULL,
                          permille_from_pct(isnap.params.inject_ingress_drop), in_seq++)) {
        ++c.injected_drops;
        continue;
      }
      c.bytes_in += n;
      // frames_in 不再按 "frag_index==0" 计：首片丢失会少计、重复首片会多计。
      // 改成「帧完成或超时」时结算（review #35），与 probe 侧的 frames 口径一致。

      const int64_t now = cue::now_us();
      auto& fa = pending[h->frame_id];
      if (fa.count == 0) {
        fa.count = h->frag_count;
        fa.pts_us = h->pts_us;
        fa.keyframe = (h->flags & cue::proto::kUpFlagKeyframe) != 0;
        fa.first_seen_us = now;
        fa.parts.resize(fa.count);
      }
      if (h->frag_index < fa.count) {
        // 只吃 payload_len 字节：整段剩余字节可能带尾随垃圾（review #17）；
        // validate_media 已保证 payload_len <= n - 32
        const uint8_t* frag = p + cue::proto::kMediaHeaderSize;
        fa.parts[h->frag_index].assign(frag, frag + h->payload_len);
        if (h->flags & cue::proto::kUpFlagKeyframe) fa.keyframe = true;
      }

      bool complete = true;
      for (const auto& part : fa.parts) {
        if (part.empty()) {
          complete = false;
          break;
        }
      }
      if (!complete) continue;
      ++c.frames_in;  // 帧在「完成或超时」处结算（#35）
      g_frames_in_total.fetch_add(1, std::memory_order_relaxed);

      const int64_t t_reasm_done = cue::now_us();
      // recv 段：数据报到达 → 本帧重组完成。段意保持原样，**不再**把重组耗时灌进 buffer：
      // 那是「帧齐了」的开销，抖动缓冲的等待是另一个物理量（见 process_frame）。
      rings[0].push(t_reasm_done - fa.first_seen_us);

      std::vector<uint8_t> frame;
      std::size_t total = 0;
      for (const auto& part : fa.parts) total += part.size();
      frame.reserve(total);
      for (const auto& part : fa.parts) frame.insert(frame.end(), part.begin(), part.end());

      // 重组之后、解码之前：进抖动缓冲。fa 是 pending 里的引用，erase 之后不可再用，
      // 所以先把元信息搬进 JitterFrame，再 erase，最后 push。
      cue::media::JitterFrame jf;
      jf.pts_us = fa.pts_us;
      jf.frame_id = h->frame_id;
      jf.frag_count = fa.count;
      jf.keyframe = fa.keyframe;
      jf.codec = h->codec;
      jf.payload = std::move(frame);
      pending.erase(h->frame_id);

      for (auto& ready : jb.push(std::move(jf), t_reasm_done)) {
        process_frame(std::move(ready));
      }
    }
    // 断连接时抖动缓冲里还压着几帧（最多一个 hold）。把它们发出去而不是静默丢弃：
    // 丢掉的话每次断流都少几帧，probe 侧会把它看成 PTS 空洞（一次误判的"丢包"）。
    // 这里用"远未来"的 poll 让它们在到期判据上无条件放行。
    for (auto& ready : jb.poll(cue::now_us() + 10'000'000)) process_frame(std::move(ready));
    std::fprintf(stderr, "[cued] client disconnected\n");
  }
}
