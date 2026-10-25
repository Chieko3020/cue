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
#include <cstdarg>
#include <deque>
#include <expected>
#include <functional>
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
#include <memory>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

#include "cue/common/clock.h"
#include "cue/media/audio_decoder.h"
#include "cue/media/audio_rms.h"
#include "cue/media/delay_injector.h"
#include "cue/media/encoder.h"
#include "cue/media/h264_decoder.h"
#include "cue/media/jitter_buffer.h"
#include "cue/pipeline/control_plane.h"
#include "cue/pipeline/session_registry.h"
#include "cue/proto/wire.h"

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using udp = net::ip::udp;
using json = nlohmann::json;

namespace {
// ---- NAT 注册的对端状态（文件作用域：会话 lambda 与 nack 线程都要读写）----
// 公网部署时 cued 要往 NAT 后面的客户端发 UDP。客户端先发 HELLO 打开映射，
// 这里记住**观察到的源地址**，下行就用它。
std::mutex g_peer_mu;
sockaddr_in g_learned_peer{};
std::atomic<bool> g_peer_ready{false};


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
// AIMD 的步数与当前码率（NACK 线程写、报告线程读）。码率本身是进程级的
// （ParamStore 只有一份），所以这两个读数也按进程算，不按会话分。
std::atomic<uint64_t> g_aimd_steps{0};
std::atomic<int> g_aimd_bitrate_kbps{0};

// stdout/stderr 的**行级锁**。多会话线程 + 报告线程会同时往同一个日志文件写
// （测试里普遍 2>&1）：一行被别的线程插进去就作废了，尤其是 METRICS 行——
// 它整行是 JSON，断成两半两个解析器都读不出东西。
std::mutex g_stdout_mu;

__attribute__((format(printf, 1, 2))) void cued_log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::lock_guard<std::mutex> lk(g_stdout_mu);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
}

// 单行结构化事件： [cue-event] {"ev":"<type>","t":<unix_us>,"data":{...}}
// 与 [cued]/[probe] 人读行共存：现有解析器只认整行前缀，互不干扰。
// 外层用 ordered_json：键序固定为 ev/t/data（SPEC §5 的格式），不随字段名排序漂移。
//
// 一个出口写两处（stdout 行 + UDS 订阅推送）：两处各写一遍必然漂移，
// 而事件流是 agent/SSE 的权威来源——订阅者看到的必须与 harness 读到的同源。
void emit_event(std::string_view ev, const json& data) {
  nlohmann::ordered_json body{{"ev", std::string(ev)}, {"t", unix_us()}, {"data", data}};
  {
    // 与报告线程的 METRICS 行共用一把锁：事件行与指标行不能互相插进对方中间。
    std::lock_guard<std::mutex> lk(g_stdout_mu);
    cue::proto::emit_event_line(body.dump());
  }
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
  // 换测量窗口。只用于「样本分布换了一族」的场景（见 send 段的延迟注入），
  // 不是给"想让读数好看"准备的。
  void clear() {
    buf_.clear();
    head_ = 0;
  }

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

// 令牌桶：SPEC §7.2「10 Mbps 速率 + 关键帧透支上限 512 包」。
// 口径按**包**计费（包大小按 kMaxUdpPayload + 24 B 头折算成包速率），这样 24 B/包的
// 头开销不会被漏算；突发是一个常量包数，而不是"5% 窗口字节数"。
// 不足时先丢非关键帧：关键帧允许透支，但透支上限仍是有限的，关键帧风暴不能无限超发。
//
// 突发量为什么是 512 而不是规格最初写的 64：64 包 ≈ 76 KB，隐含假设"一帧的分片数
// 不超过 64"，而 1080p 的 IDR 有 100~200 个分片 —— 令牌一空整帧关键帧被丢，
// 每个 GOP 边界断一次参考链（实测 budget_drops 6 Mbps 时 11 次、12 Mbps 时 41 次，
// 码率越高丢得越多，因为 IDR 更大）。突发量是**容忍度**不是速率，取 512 仍是有界的。
// 为什么不按"码率 × 时长"推导：要覆盖的是**单个关键帧的大小**，而它与平均码率不是
// 线性关系（同一码率下 Keyframe 大小随场景剧烈变化），按时间算反而算不准。
class TokenBucket {
 public:
  // 关键帧允许透支的包数上限。
  //
  // 512 包 ≈ 600 KB，足以覆盖 1080p/4K 的单个 IDR；真正的限速靠 refill()，
  // 不是这个上限。上限本身的取值理由见类上方注释（那里也写了被丢的关键帧会造成什么）。
  static constexpr double kBurstPackets = 512.0;
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

// 首个错误原因的截断长度：一帧一个 Opus 包的失败原因是固定短句，
// 但协议拒绝会带上尺寸数字；500 字节足够读，又不至于把 METRICS 行撑爆。
constexpr std::size_t kMaxAudioErrorLen = 500;

// cued 里沿用原名（历史代码里到处是 AudioRmsWindow），实现已移到公共头。
using cue::media::AudioRmsWindow;

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
  // 延迟注入（FR-PIPE.10 的 delay / jitter）：被压进延迟队列的数据报数、以及单个数据报
  // 被压住的最长时间（us）。它们是"晚到而不是丢了"的正面证据——只有它们涨、
  // injected / budget_drops 不涨，才能说明包最终都发出去了。
  uint64_t delayed_pkts = 0;
  uint64_t delayed_max_us = 0;
  // FEC 冗余组（FR-PIPE.9）：本会话实际发出的组数（被注入丢包吃掉的组不计——
  // "发送的组数"要能对上 probe 侧"收到的组数"，虚报没发出去的不算）。
  uint64_t fec_groups = 0;
  // 迟到帧丢弃（实时流不追赶积压）。旧实现在编码器慢下来时会把 WS 上行积压一路读完，
  // 恢复后以 100+ fps 追帧，端到端延迟要好几秒才回来——实时流宁可丢帧也不排队。
  uint64_t ingress_late = 0;
  // 关键帧请求的抑制计数（FR-PIPE.11）：冷却窗口内或同 frame_id 的重复请求只计数不落事件。
  uint64_t kf_req_suppressed = 0;
  // 音频解码（FR-DEV.6）：上行 type=0x02 的载荷是**裸 Opus 包**（proto::upstream_pack.h：
  // 32 字节 MediaHeader 之后直接是编码器输出），本进程用 libopus 真解，解出的 PCM
  // 只用来算指标（RMS/耗时），不写下行——下行没有音频数据报类型，probe 也没有音频出口。
  // audio_decoded 的判据是 audio_decoded_frames > 0，不是"收到过音频包"：
  // 收到包与解得出声音是两件事，混在一起就退化成旧的"只计数"。
  uint64_t audio_frames_in = 0;
  uint64_t audio_bytes_in = 0;
  // 下行透传出去的音频包数（DownType::Audio）。与 audio_frames_in 分开计：
  // 差值是"收进来但没发出去"（解码失败不拦透传，但发送错误会被记下）。
  uint64_t audio_frames_out = 0;
  // 多分片音频不做重组，单独计数：少计会让「音频到底有没有在流」看不出来
  uint64_t audio_multi_frag = 0;
  // 出向注入（N-01）丢掉的音频下行包数。与 audio_frames_out 分开：
  // 前者是"被故障注入吃掉的"，后者是"真正发出去的"——把两者混在一起，
  // 就分不清"链路丢包"和"本机注入"（视频侧 injected 也是同一口径）。
  uint64_t audio_injected_drops = 0;
  int64_t audio_last_pts_us = 0;
  uint64_t audio_decoded_frames = 0;
  uint64_t audio_pcm_frames = 0;  // 累计解出的 PCM 样本数（mono，等于每声道样本数）
  uint64_t audio_decode_errors = 0;
  // 首个失败原因：后续错误不覆盖它（"第一件出的事"比"最后一件"更能定位故障形状）
  std::string audio_first_error;
  Ring audio_decode_us;      // 单帧解码耗时（µs），audio_decode_p99_us 的样本源
  AudioRmsWindow audio_rms;  // 最近 1 s 的 RMS 窗口
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

// "还没有任何一代"的哨兵。为什么不用 0：generation 从 0 起，用 0 会让第一帧把
// "0" 读成"这一代已经失败过"而跳过配置（实测症状：transcode=on 却恒为透传）。
constexpr uint64_t kNoGeneration = ~0ULL;

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

// feedback（技术设计 §4.2）：probe 每 200 ms 发来一条 type=0x06，cued 侧只做两件事：
//   1) 记账（fb_rx / fb_loss_rate / fb_rtt_ms / fb_recv_mbps）——永远做，与 transcode 无关；
//   2) 仅当 transcode=on 且 aimd=on 时按 loss 调码率（AIMD）。
// 为什么与 cued 的其他计数分开：NACK 线程会跨 WS 会话存活，而 Counters 是每连接重置的；
// 反馈读数属于「这一代对端/链路」，放原子变量里让 media 线程每秒读取即可。
struct FeedbackStats {
  std::atomic<uint64_t> rx{0};
  std::atomic<uint64_t> aimd_steps{0};
  std::atomic<double> loss_rate{-1.0};   // -1 = 还没收到过（指标里写 null）
  std::atomic<double> recv_mbps{-1.0};
  std::atomic<double> rtt_ms{-1.0};
  std::atomic<int> bitrate_kbps{0};      // AIMD 当前码率（初值 = 启动码率）
};

// AIMD 的本地状态（只在 NACK 线程里读写，不需要锁）。
// base_kbps 是**手动/启动**设定的码率，用来算上限 2×base；AIMD 自己写出来的值不算基准。
// last_written_kbps 用来识别「有人手动改过码率」：当前值不等于自己上次写的值 → 重新以
// 手动值为基准。手动优先，AIMD 从手动值继续调，不会把手动设置覆盖回旧值。
struct AimdState {
  int base_kbps = 0;
  int last_written_kbps = 0;
  int low_streak = 0;       // 连续 3 个 feedback 窗口 loss<2% 才升一次
  int64_t last_change_us = 0;
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

// ---------------------------------------------------------------- 会话相关的纯小工具
sockaddr_in make_probe_addr(const std::string& host, uint16_t port) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  ::inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
  return addr;
}

// 对端地址：设备表与事件里要能看出是哪台机器（多设备排查的第一步）。
template <typename Stream>
std::string peer_string(Stream& ws) {
  beast::error_code ec;
  const auto ep = ws.next_layer().remote_endpoint(ec);
  if (ec) return "?";
  return ep.address().to_string() + ":" + std::to_string(ep.port());
}

// 下发档位的 JSON（device.profile_assigned / session.list 用同一份字段）
json profile_json(const cue::pipeline::AssignedProfile& p) {
  return json{{"transcode", p.transcode},
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

// 最后一份上行 codec 配置（0x03）：只服务于"单设备断线重连时，观测面立刻能看到
// 媒体元信息"。多设备时不回放——把另一路的 SPS/PPS 报给 agent 只会误导。
std::mutex g_codec_mu;
CodecConfig g_last_codec;

void publish_last_codec(const CodecConfig& cfg) {
  std::lock_guard<std::mutex> lk(g_codec_mu);
  g_last_codec = cfg;
}

void maybe_replay_codec(const cue::pipeline::SessionRegistry& registry) {
  if (registry.live_count() != 1) return;
  CodecConfig cfg;
  {
    std::lock_guard<std::mutex> lk(g_codec_mu);
    if (!g_last_codec.have) return;
    cfg = g_last_codec;
  }
  emit_event("codec.config", cfg.as_json(true));
}

// ---------------------------------------------------------------- 多会话（FR-PIPE.12 / T-22）
//
// 并发模型：**每连接一个会话线程**。为什么不是单线程事件循环：解码/编码是阻塞的 CPU
// 工作（一次 x264/lavc 调用几十毫秒），单线程轮转只会把两路串行化，2 vCPU 上帧预算照样
// 超。线程模型下两路的 decode/encode 才真正并行，代价是每路一份解码器/编码器/缓冲
// （实测并发上限见 README「已知限制」）。
//
// 进程级共享的只有三样，同步方式都写死在结构上：
//   · UDS 控制面（ParamStore 一把锁 + 每帧快照）——改参数仍然是"下一帧生效、不断流"；
//   · 下行 UDP socket（sendto 线程安全）——**内核发送缓冲是唯一跨会话争用的资源**，
//     所以每路有自己的令牌桶，一路积压不能把别人的包挤掉；
//   · 事件出口（emit_event 一行一行写 stdout + 订阅推送）。

// 六段分位的进程级合并环：METRICS 行顶层的 recv/buffer/... 由它算。
// 单路时它与该会话自己的环样本完全相同（顶层读数与 T-22 之前逐个数字一致）；
// 多路时顶层代表"整条管线"，每路的细节在 per_session 数组里——两种问法都有答案。
struct SharedRings {
  void push(int stage, int64_t v) {
    std::lock_guard<std::mutex> lk(mu_);
    rings_[stage].push(v);
  }
  void clear(int stage) {
    std::lock_guard<std::mutex> lk(mu_);
    rings_[stage].clear();
  }
  // null = 这一段没有样本（"没采到"不能用 0 冒充），与 METRICS 行的旧口径一致。
  json summary(int stage) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (rings_[stage].size() == 0) return nullptr;
    return json{{"p50", double(rings_[stage].pct(0.50)) / 1000.0},
                {"p95", double(rings_[stage].pct(0.95)) / 1000.0},
                {"p99", double(rings_[stage].pct(0.99)) / 1000.0}};
  }

 private:
  mutable std::mutex mu_;
  Ring rings_[6];
};

// 会话自己的东西里，**只有这两样**会被本会话之外的线程碰：
//   · xmit：NACK 重传线程按 (frame_id, frag_index) 找包；
//   · fb：feedback 线程写、会话线程每帧报点读。
// 其余状态（解码器/编码器/抖动缓冲/发送队列/令牌桶）只属于会话线程，不需要锁。
struct CuedSessionShared final : cue::pipeline::SessionPayload {
  RetransmitCache xmit;
  FeedbackStats fb;
  // 下行关键帧请求的到达计数（NACK 线程 +1，会话线程消费）。
  // 用计数而不是 bool：会话线程按"值变了"判断，就不会漏掉在它处理期间连发的请求，
  // 也不需要原子地清零（fetch_add 比 compare_exchange 少一类 ABA 型 bug）。
  std::atomic<uint64_t> keyframe_reqs{0};
};

// 一个会话的每秒读数（会话线程算、报告线程读；键名与 METRICS 行一致）。
json session_snapshot(const Ring rings[6], const Counters& c, int64_t now_us, int64_t t_last,
                      uint64_t frames_prev, const cue::media::JitterBufferStats& jb,
                      const FeedbackStats& fb, uint32_t upstream_stream_id) {
  const double dt = t_last > 0 ? double(now_us - t_last) / 1e6 : 0.0;
  const uint64_t df = c.frames_out >= frames_prev ? c.frames_out - frames_prev : 0;
  const bool have_fb = fb.rx.load(std::memory_order_relaxed) > 0;
  // 音频读数：一次都没解出帧时 RMS 与 p99 必须是 null，不能拿 0 dBFS（满幅）或
  // -120 dBFS（静音）冒充"没有音频"。reason 同理：没流说没流，出错说第一件错事。
  // p99 只统计**解成功**的帧：失败帧不产生 PCM，混进来只会把"错误处理耗时"报成解码耗时。
  const json audio_rms_dbfs =
      c.audio_rms.empty() ? json(nullptr) : json(cue::media::dbfs_for_metric(c.audio_rms.dbfs()));
  const json audio_dec_p99 = c.audio_decode_us.size() == 0
                                 ? json(nullptr)
                                 : json(double(c.audio_decode_us.pct(0.99)));
  json audio_reason = nullptr;
  if (!c.audio_first_error.empty()) {
    audio_reason = c.audio_first_error;
  } else if (c.audio_frames_in == 0) {
    audio_reason = c.audio_multi_frag > 0 ? "audio_multi_frag_not_reassembled"
                                          : "no_audio_stream";
  }
  json m{{"fps", dt > 0.0 ? double(df) / dt : 0.0},
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
         {"delayed_pkts", c.delayed_pkts},
         {"delayed_max_us", c.delayed_max_us},
         {"fec_groups", c.fec_groups},
         {"fb_rx", fb.rx.load(std::memory_order_relaxed)},
         {"fb_loss_rate", have_fb ? json(fb.loss_rate.load(std::memory_order_relaxed))
                                  : json(nullptr)},
         {"fb_recv_mbps", have_fb ? json(fb.recv_mbps.load(std::memory_order_relaxed))
                                  : json(nullptr)},
         {"fb_rtt_ms", have_fb ? json(fb.rtt_ms.load(std::memory_order_relaxed)) : json(nullptr)},
         {"ingress_late", c.ingress_late},
         {"kf_req_suppressed", c.kf_req_suppressed},
         {"audio_frames_in", c.audio_frames_in},
         {"audio_frames_out", c.audio_frames_out},
         {"audio_injected_drops", c.audio_injected_drops},
         {"audio_bytes_in", c.audio_bytes_in},
         {"audio_multi_frag", c.audio_multi_frag},
         {"audio_last_pts_us", c.audio_last_pts_us},
         // 音频解码：audio_decoded 是"真解出过帧"的判据，不是"收到过包"。
         {"audio_decoded", c.audio_decoded_frames > 0},
         {"audio_decoded_frames", c.audio_decoded_frames},
         {"audio_pcm_frames", c.audio_pcm_frames},
         {"audio_decode_errors", c.audio_decode_errors},
         {"audio_decode_p99_us", audio_dec_p99},
         {"audio_rms_dbfs", audio_rms_dbfs},
         {"audio_rms_frames", c.audio_rms.frames()},
         {"audio_decode_reason", audio_reason},
         {"bytes_in", c.bytes_in},
         {"stream_id", upstream_stream_id},
         {"jb_depth", jb.depth},
         {"jb_level", jb.level},
         {"jb_switches", jb.switches},
         {"jb_target_frames", jb.target_frames},
         {"jb_skipped", jb.skipped},
         {"jb_late", jb.late_dropped},
         // 抖动估计的**原始值**（以帧间隔为单位）。probe 侧早就有这一项，cued 侧一直缺——
         // 而"抖动估计的基准点该用首片还是组装完成"这种问题正是靠它才能量化：
         // 只看离散的 jb_level 分不清 0.4 帧与 1.4 帧，那正好是升档门限的两侧。
         {"jb_jitter_frames", jb.jitter_frames}};
  for (int i = 0; i < 6; ++i) {
    if (rings[i].size() == 0) {
      m[kStageNames[i]] = nullptr;
    } else {
      m[kStageNames[i]] = json{{"p50", double(rings[i].pct(0.50)) / 1000.0},
                               {"p95", double(rings[i].pct(0.95)) / 1000.0},
                               {"p99", double(rings[i].pct(0.99)) / 1000.0}};
    }
  }
  return m;
}

// 进程级报告（报告线程调，1 Hz）：一行人读 + 一行 METRICS + 一份 metric.sample 事件。
// 顶层读数 = 各活会话的聚合（计数器求和、链路读数取最坏），per_session 里是每路的同一批
// 键——单路时顶层与 per_session[0] 的数字完全相同，多路时能从同一行里分清是哪一路。
void report_once(const cue::pipeline::SessionRegistry& registry, const SharedRings& shared,
                 MetricsView& view, int64_t now_us) {
  const json agg = registry.aggregate(now_us);
  const json& cs = agg["counters"];
  const auto u64 = [&cs](const char* key) -> uint64_t {
    const auto it = cs.find(key);
    if (it == cs.end() || !it->is_number()) return 0;
    const double v = it->get<double>();
    return v <= 0.0 ? 0 : static_cast<uint64_t>(v);
  };
  const auto num = [&cs](const char* key) -> double {
    const auto it = cs.find(key);
    return it != cs.end() && it->is_number() ? it->get<double>() : 0.0;
  };
  const auto null_or = [&cs](const char* key) -> json {
    const auto it = cs.find(key);
    return it != cs.end() && it->is_number() ? json(it->get<double>()) : json(nullptr);
  };
  const double fps = agg.contains("fps") && agg["fps"].is_number() ? agg["fps"].get<double>() : 0.0;
  const int sessions = agg.contains("live") && agg["live"].is_number() ? agg["live"].get<int>() : 0;
  // 顶层音频原因只给**类别**：多会话聚合时"具体哪条错"留给 per_session（那边是首个错误
  // 原文），顶层不能凭空挑一路的错误当全局原因。
  json audio_reason = nullptr;
  if (u64("audio_frames_in") == 0) {
    audio_reason = u64("audio_multi_frag") > 0 ? "audio_multi_frag_not_reassembled"
                                               : "no_audio_stream";
  } else if (u64("audio_decode_errors") > 0) {
    audio_reason = "decode_errors";
  }

  std::ostringstream human;
  human.setf(std::ios::fixed);
  human << "[cued] fps=" << std::setprecision(1) << fps
        << " in=" << u64("frames_in") << " out=" << u64("frames_out") << " drop=" << u64("dropped")
        << " inj=" << u64("injected") << " bkt=" << u64("budget_drops")
        << " serr=" << u64("send_errors") << " enc_err=" << u64("encode_errors")
        << " dec=" << u64("decoded") << " dec_err=" << u64("decode_errors")
        << " dec_drop=" << u64("decode_drops") << " rej=" << u64("rejected")
        << " dly=" << u64("delayed_pkts") << "/" << num("delayed_max_us") / 1000.0 << "ms"
        << " aud=" << u64("audio_frames_in") << " apcm=" << u64("audio_pcm_frames")
        << " aerr=" << u64("audio_decode_errors")
        << " ainj=" << u64("audio_injected_drops");
  {
    const json arms = null_or("audio_rms_dbfs");
    // 没有解出帧时不打这一项：打 -inf / 0 都会被读成"有读数"。
    if (!arms.is_null()) human << " arms=" << std::setprecision(1) << arms.get<double>() << "dBFS";
  }
  human << " MB=" << std::setprecision(2) << num("bytes_in") / (1024.0 * 1024.0);
  for (int i = 0; i < 6; ++i) {
    const json st = shared.summary(i);
    if (st.is_null()) {
      human << "  " << kStageNames[i] << "=-";
    } else {
      human << std::setprecision(3) << "  " << kStageNames[i] << "=" << st["p50"].get<double>()
            << "/" << st["p95"].get<double>() << "/" << st["p99"].get<double>();
    }
  }
  human << " ms(p50/p95/p99)";
  human << "  sess=" << sessions << " jb depth=" << u64("jb_depth") << " lvl=" << u64("jb_level")
        << " tgt=" << u64("jb_target_frames") << " sw=" << u64("jb_switches")
        << " skip=" << u64("jb_skipped") << " late=" << u64("jb_late");
  human << "  fb=" << u64("fb_rx");
  if (u64("fb_rx") > 0) {
    const json loss = null_or("fb_loss_rate");
    const json rtt = null_or("fb_rtt_ms");
    if (!loss.is_null()) {
      human << " loss=" << std::setprecision(1) << loss.get<double>() * 100.0 << "%";
    }
    if (!rtt.is_null()) human << " rtt=" << rtt.get<double>() << "ms";
  }
  human << " aimd=" << g_aimd_steps.load(std::memory_order_relaxed) << "/"
        << g_aimd_bitrate_kbps.load(std::memory_order_relaxed) << "kbps";
  const std::string human_line = human.str();
  std::fwrite(human_line.data(), 1, human_line.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
  if (auto* ctl_log = g_control.load(std::memory_order_acquire)) ctl_log->push_log(human_line);

  json m{{"fps", fps},
         {"frames_in", u64("frames_in")},
         {"frames_out", u64("frames_out")},
         {"dropped", u64("dropped")},
         {"injected", u64("injected")},
         {"decoded", u64("decoded")},
         {"decode_errors", u64("decode_errors")},
         {"rejected", u64("rejected")},
         {"budget_drops", u64("budget_drops")},
         {"send_errors", u64("send_errors")},
         {"encode_errors", u64("encode_errors")},
         {"encode_delays", u64("encode_delays")},
         {"decode_drops", u64("decode_drops")},
         {"queue_dropped", u64("queue_dropped")},
         {"delayed_pkts", u64("delayed_pkts")},
         {"delayed_max_us", u64("delayed_max_us")},
         {"fec_groups", u64("fec_groups")},
         {"fb_rx", u64("fb_rx")},
         {"fb_loss_rate", null_or("fb_loss_rate")},
         {"fb_recv_mbps", null_or("fb_recv_mbps")},
         {"fb_rtt_ms", null_or("fb_rtt_ms")},
         {"aimd_steps", g_aimd_steps.load(std::memory_order_relaxed)},
         {"aimd_bitrate_kbps", g_aimd_bitrate_kbps.load(std::memory_order_relaxed)},
         {"ingress_late", u64("ingress_late")},
         {"audio_frames_in", u64("audio_frames_in")},
         {"audio_frames_out", u64("audio_frames_out")},
         {"audio_injected_drops", u64("audio_injected_drops")},
         {"audio_bytes_in", u64("audio_bytes_in")},
         {"audio_multi_frag", u64("audio_multi_frag")},
         {"audio_last_pts_us", u64("audio_last_pts_us")},
         {"audio_decoded", u64("audio_decoded_frames") > 0},
         {"audio_decoded_frames", u64("audio_decoded_frames")},
         {"audio_pcm_frames", u64("audio_pcm_frames")},
         {"audio_decode_errors", u64("audio_decode_errors")},
         {"audio_decode_p99_us", null_or("audio_decode_p99_us")},
         {"audio_rms_dbfs", null_or("audio_rms_dbfs")},
         {"audio_decode_reason", audio_reason},
         {"jb_depth", u64("jb_depth")},
         {"jb_level", u64("jb_level")},
         {"jb_switches", u64("jb_switches")},
         {"jb_target_frames", u64("jb_target_frames")},
         {"jb_skipped", u64("jb_skipped")},
         {"jb_late", u64("jb_late")},
         // 多设备会话（FR-PIPE.12）：活会话数 + 每路读数。单路时 per_session[0] 与顶层
         // 是同一批数字（口径一致，可以互相校验）。
         {"sessions", sessions},
         {"per_session", agg["per_session"]}};
  for (int i = 0; i < 6; ++i) m[kStageNames[i]] = shared.summary(i);
  const auto* ctl = g_control.load(std::memory_order_acquire);
  m["control_channel"] = ctl != nullptr ? ctl->channel_tag() : "stdout";
  m["control_ops"] = ctl != nullptr ? ctl->ops() : 0;
  const std::string line = "METRICS " + m.dump();
  std::fwrite(line.data(), 1, line.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
  // 控制面与 stdout 读同一份快照（metrics.query / obs.fetch frames）：两个读数永远是
  // 同一次 report_once 的产物，不会"刚改完参数，两边数字不一样"。
  view.set(m, nlohmann::ordered_json{{"frames_in", u64("frames_in")},
                                     {"frames_out", u64("frames_out")},
                                     {"dropped", u64("dropped")},
                                     {"injected", u64("injected")},
                                     {"decoded", u64("decoded")},
                                     {"decode_errors", u64("decode_errors")},
                                     {"rejected", u64("rejected")},
                                     {"budget_drops", u64("budget_drops")},
                                     {"delayed_pkts", u64("delayed_pkts")},
                                     {"delayed_max_us", u64("delayed_max_us")},
                                     {"send_errors", u64("send_errors")},
                                     {"encode_errors", u64("encode_errors")},
                                     {"decode_drops", u64("decode_drops")},
                                     {"audio_frames_in", u64("audio_frames_in")},
                                     {"audio_pcm_frames", u64("audio_pcm_frames")},
                                     {"audio_decode_errors", u64("audio_decode_errors")},
                                     {"audio_decode_p99_us", null_or("audio_decode_p99_us")},
                                     {"audio_rms_dbfs", null_or("audio_rms_dbfs")}});
  json sample{{"fps", fps},
              {"frames_in", u64("frames_in")},
              {"frames_out", u64("frames_out")},
              {"dropped", u64("dropped")},
              {"injected", u64("injected")},
              {"rejected", u64("rejected")},
              {"serr", u64("send_errors")},
              {"fec_groups", u64("fec_groups")},
              {"fb_rx", u64("fb_rx")},
              {"aimd_steps", g_aimd_steps.load(std::memory_order_relaxed)},
              {"audio_frames_in", u64("audio_frames_in")},
              {"audio_decoded", u64("audio_decoded_frames") > 0},
              {"audio_pcm_frames", u64("audio_pcm_frames")},
              {"audio_decode_errors", u64("audio_decode_errors")},
              {"audio_rms_dbfs", null_or("audio_rms_dbfs")},
              {"jb_depth", u64("jb_depth")},
              {"jb_skipped", u64("jb_skipped")},
              {"jb_late", u64("jb_late")},
              {"jb_level", u64("jb_level")},
              {"sessions", sessions},
              {"per_session", agg["per_session"]}};
  for (int i = 0; i < 6; ++i) {
    const json st = m[kStageNames[i]];
    for (const char* p : {"p50", "p95", "p99"}) {
      const std::string key = std::string(kStageNames[i]) + "_" + p;
      sample[key] = st.is_null() ? json(nullptr) : st[p];
    }
  }
  emit_event("metric.sample", sample);
}


// 会话线程需要的**启动期**配置。运行期可变的参数不在这里：它们一律走 ParamStore
// 快照（每帧取一次），否则"改参数立刻生效"就又变成"改参数要重启"。
struct SessionEnv {
  cue::pipeline::ParamStore* params = nullptr;
  cue::pipeline::SessionRegistry* registry = nullptr;
  SharedRings* rings = nullptr;
  cue::media::JitterBufferConfig jb_cfg;
  int usock = -1;
  bool fec_enabled = true;
  int max_ingress_lag_ms = 300;
  FILE* trace_fp = nullptr;
  std::string probe_host;
};

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
// ---------------------------------------------------------------- 会话（FR-PIPE.12 / T-22）
//
// 一个 WS 连接 = 一个会话 = 一根线程。会话内的全部状态（重组表、抖动缓冲、解码器、
// 转码链、令牌桶、发送队列、延迟队列、六段采样环）都是本函数的**局部变量**：它们只属于
// 这条连接，结构上不可能被别的会话碰到——资源隔离不是靠"小心一点"。
//
// 与外部共享的东西都显式传入，且各有所有者：
//   · env.params   → 运行期参数（ParamStore，一把锁 + 每帧快照）
//   · env.registry → 会话表（设备名/租约/能力/档位/每会话读数）
//   · shared       → NACK 线程要用的重传缓存 + feedback 槽（只有这两样跨线程）
//   · env.usock    → 下行 UDP socket（sendto 线程安全；每路自己的令牌桶限速）
void run_session(const SessionEnv& env, websocket::stream<tcp::socket> ws) {
  auto& params = *env.params;
  auto& registry = *env.registry;
  const int usock = env.usock;
  const auto& jb_cfg = env.jb_cfg;
  const bool fec_enabled = env.fec_enabled;
  const int max_ingress_lag_ms = env.max_ingress_lag_ms;
  FILE* trace_fp = env.trace_fp;

  beast::error_code ec;
  ws.accept(ec);
  if (ec) {
    cued_log("[cued] handshake failed: %s\n", ec.message().c_str());
    return;
  }
  const std::string peer = peer_string(ws);
  const int ws_fd = static_cast<int>(ws.next_layer().native_handle());

  // 连接时刻还没有能力上报：先按配置下发档位（basis=no_caps）。设备上报能力之后
  // （0x05 kind=caps）按它的上限重算 —— 见下面 process_frame 里的 profile 分支。
  cue::pipeline::DeviceCaps device_caps;
  cue::pipeline::AssignedProfile profile =
      cue::pipeline::assign_profile(params.snapshot().params, device_caps);
  bool profile_caps_pending = false;  // 有新的能力上报，下一帧重算档位
  uint64_t profile_enc_gen = params.snapshot().encoder_generation;

  auto shared = std::make_shared<CuedSessionShared>();
  cue::pipeline::SessionRegistry::RegisterRequest req;
  req.peer = peer;
  req.fd = ws_fd;
  req.payload = shared;
  req.profile = profile;
  req.profile_detail = profile.detail;
  auto reg = registry.register_session(std::move(req), cue::now_us());
  if (!reg) {
    // 并发上限已满：明确拒绝（事件 + stderr + 关连接），不静默挂着
    emit_event("session.rejected", json{{"reason", reg.error()}, {"peer", peer}});
    cued_log("[cued] 拒绝连接（%s）：%s\n", peer.c_str(), reg.error().c_str());
    beast::error_code ign;
    ws.next_layer().close(ign);
    return;
  }
  const uint32_t session_id = reg->session_id;
  std::string device_id = reg->device_id;
  const sockaddr_in probe_addr = make_probe_addr(env.probe_host, reg->probe_port);
  cued_log("[cued] client connected: %s session=%u device=%s probe_port=%u slot=%d\n",
           peer.c_str(), session_id, device_id.c_str(), reg->probe_port, reg->slot);
  for (const uint32_t old_id : reg->replaced) {
    emit_event("device.replaced", json{{"device_id", device_id},
                                       {"old_session_id", old_id},
                                       {"new_session_id", session_id},
                                       {"reason", "same_device_reconnected"}});
  }
  emit_event("device.registered", json{{"session_id", session_id},
                                       {"device_id", device_id},
                                       {"peer", peer},
                                       {"probe_port", reg->probe_port},
                                       {"slot", reg->slot},
                                       {"lease_ms", reg->lease_us / 1000},
                                       {"caps_declared", false}});
  emit_event("device.profile_assigned", json{{"session_id", session_id},
                                             {"device_id", device_id},
                                             {"profile", profile_json(profile)},
                                             {"basis", profile.basis},
                                             {"clamped", profile.clamped},
                                             {"detail", profile.detail}});
  // 断线重连时把上一份 codec 配置回放给观测面。只在本会话是唯一一路时回放：
  // 多设备时把另一路的 SPS/PPS 报给 agent 的 observation 只会误导。
  maybe_replay_codec(registry);

  // 每会话一份：解码器（参考帧状态）、转码链、令牌桶（速率）、codec 配置缓存。
  cue::media::H264Decoder decoder;
  {
    std::string derr;
    if (!decoder.open(derr)) {
      cued_log("[cued] decoder open failed: %s\n", derr.c_str());
      registry.unregister(session_id);
      return;
    }
  }
  // 编码器/发送侧配置的"已应用版本"：只有本会话线程读写。
  //  - last_gen：令牌桶速率的已应用版本（改速率不需要重建任何东西）；
  //  - profile_enc_gen：档位对应的参数代（见 process_frame 的能力协商分支）；
  //  - chain_failed_gen：上一代配置失败的版本号（避免每帧重试同一份坏配置刷屏）。
  uint64_t last_gen = 0;
  uint64_t chain_failed_gen = kNoGeneration;
  cue::media::TranscodeChain chain;
  bool chain_ready = false;
  // 音频解码器（上行 type=0x02 → PCM，只用来算指标）。每会话一份：Opus 解码器是带状态的
  // （上一帧的尾巴参与下一帧的重叠相加），跨会话共用会把上一路的余音串到下一路。
  // 懒打开：只推视频的会话不付创建解码器的代价。
  cue::media::OpusAudioDecoder audio_dec;
  TokenBucket bucket(params.snapshot().params.send_mbps * 1e6);
  CodecConfig codec_cfg;
  uint32_t upstream_stream_id = 0;  // 客户端自报的流号，只作记录（下行用服务端会话号）

  std::map<uint32_t, FrameAcc> pending;
  Counters c;
  Ring rings[6];
  // 六段采样同时进本会话的环（per_session 用）和进程级合并环（顶层 METRICS 用）。
  auto stage_push = [&](int stage, int64_t v) {
    rings[stage].push(v);
    env.rings->push(stage, v);
  };
  auto stage_clear = [&](int stage) {
    rings[stage].clear();
    env.rings->clear(stage);
  };
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
    // send 段起点与帧号：帧被延迟注入压住时，send 段与 frames_out 要在
    // 「真正发出」的时刻结算，这两个值得跟着帧走（不能现查，排到时已经换帧了）。
    int64_t t_send0 = 0;
    uint32_t frame_id = 0;
  };
  std::deque<QueuedSend> sendq;
  int64_t send_due_us = 0;
  // 已应用的排队节奏（NaN = 还没应用过，第一帧只记录不清理）。
  // 排队节奏不再在连接建立时定死：每帧从参数快照算（fault.set kind=backlog 运行期可改）。
  double applied_queue_fps = std::numeric_limits<double>::quiet_NaN();
  constexpr std::size_t kSendQueueMax = 8192;  // 兜底：队列不能无限涨（约 4.5 min @30fps）

  // 下行目标：自动模式下优先用 NAT 注册学到的地址（含 NAT 分配的外部端口），
  // 否则用命令行给的 --probe-host/--probe-port。
  auto current_target = [&]() -> sockaddr_in {
    if (g_peer_ready.load(std::memory_order_acquire)) {
      std::lock_guard<std::mutex> lk(g_peer_mu);
      return g_learned_peer;
    }
    return probe_addr;
  };

  // sendto 的返回值与 errno 不能丢：ENOBUFS/EMSGSIZE 会被算成「已发送」（review #16）
  auto send_dgram = [&](const std::vector<uint8_t>& d) -> bool {
    const sockaddr_in dst = current_target();
    const ssize_t sn = ::sendto(usock, d.data(), d.size(), 0,
                                reinterpret_cast<const sockaddr*>(&dst),
                                sizeof(dst));
    if (sn < 0) {
      ++c.send_errors;
      err_log.log(cue::now_us(), std::string("sendto 失败: ") + std::strerror(errno));
      return false;
    }
    return true;
  };
  // ---------------------------------------------------------------- 延迟注入
  //
  // 与 sendq 是**串联的两段**，不是合并的一个队列：
  //   sendq  管「节奏」——按 --queue-fps 等距排空，due 由帧序推出来，切策略时整队清空；
  //   delayq 管「额外延迟」——due 由注入参数 + RNG 算出来，逐包独立（jitter 的基础）。
  // 合并会带来两个错误读数：fault.set backlog 的"清空积压"会把在途的延迟包算成
  // queue_dropped（延迟撤销不该丢帧）；反过来延迟队列被当成发送节奏，又会让
  // frames_in−frames_out 的增长看起来像 queue_backlog。
  struct DelayedFrame {
    std::size_t pending = 0;   // 还没真正 sendto 的包数
    bool all_ok = true;        // 有没有包 sendto 失败（失败帧不计 frames_out）
    int64_t t_send0 = 0;       // send 段起点：分片完成时刻
  };
  struct DelayedPkt {
    std::vector<uint8_t> data;
    std::shared_ptr<DelayedFrame> frame;  // 一帧的所有包共享一个结算点
    int64_t enqueued_us = 0;
  };
  // 按 due 排序（multimap 而不是 deque）：jitter 让同一帧的包 due 不同、跨帧也可能反序，
  // 单调队列会让队首页把后面已到期的包堵住（head-of-line blocking），
  // 结果就是"抖动被放大成固定延迟"，指标上反而看不出抖动。
  std::multimap<int64_t, DelayedPkt> delayq;
  // 兜底上限：正常注入（≤2 s × 帧率 × 每帧包数）远达不到。到了只说明上游在灌，
  // 此时"立即发出"比"无限涨内存"诚实——没压住的包不计入 delayed_pkts。
  constexpr std::size_t kDelayQueueMaxDgrams = 16384;
  cue::media::DelayInjector delay_inj;
  // 延迟注入当前是否生效（由 process_frame 每帧从快照更新）。它只影响"发送最后一段
  // 走哪条分支"，不参与任何判定：参数本身永远从快照读。
  bool session_delay_on = false;

  // 到期的延迟包按 due 顺序发出；一帧的**最后一个**包发出时才结算 frames_out 与 send 段，
  // 所以这两个读数反映的是"真正离开本机"的时刻，而不是"进了队列"。
  auto flush_delayq = [&](int64_t now_us) {
    while (!delayq.empty() && delayq.begin()->first <= now_us) {
      auto it = delayq.begin();
      DelayedPkt& p = it->second;
      if (!send_dgram(p.data)) p.frame->all_ok = false;
      const int64_t held = now_us - p.enqueued_us;
      if (held > 0 && static_cast<uint64_t>(held) > c.delayed_max_us) {
        c.delayed_max_us = static_cast<uint64_t>(held);
      }
      if (--p.frame->pending == 0) {
        if (p.frame->all_ok) {
          ++c.frames_out;
          g_frames_out_total.fetch_add(1, std::memory_order_relaxed);
        }
        stage_push(5, now_us - p.frame->t_send0);
      }
      delayq.erase(it);
    }
  };

  // 一帧数据报的最后一跳。返回 true = 已交给延迟队列（send 段稍后由 flush_delayq 结算），
  // false = 现场发完（send 段由调用方按旧口径结算）。
  auto deliver_frame = [&](std::vector<std::vector<uint8_t>>&& dgrams, uint32_t frame_id,
                           int64_t t_send0) -> bool {
    // 每帧取一次快照：同一帧的所有包看到同一组注入值，不会出现"前半帧延迟 200、
    // 后半帧 0"这种半截状态。
    const auto dsnap = params.snapshot();
    const cue::pipeline::RuntimeParams& rp = dsnap.params;
    delay_inj.sync(rp.inject_seed, rp.inject_delay_ms, rp.inject_jitter_ms);
    const auto direct_send = [&] {
      bool all_sent = true;
      for (const auto& d : dgrams) {
        if (!send_dgram(d)) all_sent = false;
      }
      // 与旧口径一致：包全丢（100% 注入）时也能走到这里，frames_out 照样 +1，
      // 于是「丢包但 fps 正常」这条与"时延"完全不同的证据得以保留。
      if (all_sent) {
        ++c.frames_out;
        g_frames_out_total.fetch_add(1, std::memory_order_relaxed);
      }
    };
    if (!delay_inj.active() || dgrams.empty()) {
      direct_send();
      return false;
    }
    if (delayq.size() + dgrams.size() > kDelayQueueMaxDgrams) {
      err_log.log(cue::now_us(), "延迟注入队列已满（>16384 包）：这一帧按不延迟发出");
      direct_send();
      return false;
    }
    auto frame = std::make_shared<DelayedFrame>();
    frame->t_send0 = t_send0;
    frame->pending = dgrams.size();
    const int64_t now = cue::now_us();
    for (std::size_t pk = 0; pk < dgrams.size(); ++pk) {
      const int64_t extra_us = delay_inj.extra_us();
      // 逐包决策落 trace（同一 seed 逐项可复现）：j 行与丢包的 `<frame> <pk>` 行
      // 共用一个文件，前缀区分行类型。
      if (trace_fp && rp.inject_jitter_ms > 0.0) {
        std::fprintf(trace_fp, "j %u %zu %lld\n", frame_id, pk,
                     static_cast<long long>(extra_us));
      }
      delayq.emplace(now + extra_us, DelayedPkt{std::move(dgrams[pk]), frame, now});
      ++c.delayed_pkts;
    }
    return true;
  };

  // 到期的排队帧按顺序发出；fps 因此由 --queue-fps 决定，而不是由「收到多少帧」决定。
  // 最后一段有两个分支，是为了让**没有延迟注入**时的行为与 M2 逐字节相同：
  // 每次 drain 都去参数快照里问一次"延迟生效了吗"会给排队路径加一次锁 + 一次
  // string 拷贝，虽然代价极小，但这条路径是 AC-15 五类既有 case 的地基，不值得为
  // 一个未启用的特性付出任何时序扰动。session_delay_on 由 process_frame 每帧更新。
  auto drain_sendq = [&](int64_t now_us) {
    while (!sendq.empty() && sendq.front().due_us <= now_us) {
      QueuedSend q = std::move(sendq.front());
      sendq.pop_front();
      if (session_delay_on) {
        (void)deliver_frame(std::move(q.datagrams), q.frame_id, q.t_send0);
        continue;
      }
      bool ok = true;
      for (const auto& d : q.datagrams) {
        if (!send_dgram(d)) ok = false;
      }
      if (ok) {
        ++c.frames_out;
        g_frames_out_total.fetch_add(1, std::memory_order_relaxed);
      }
    }
  };

  // 迟到帧丢弃用到的两个量（见 process_frame 开头）：
  //  · ingress_min_offset 是「这一帧到达时刻 − 它的上行 PTS」的历史最小值，
  //    充当跨机时钟差的基线；上行 PTS 来自设备时钟，绝对值不可比，但相对值可比。
  //  · 基线不重置：客户端时钟漂移典型 <100 ppm（每秒 0.1 ms），相对 300 ms 的阈值可忽略。
  int64_t ingress_min_offset = std::numeric_limits<int64_t>::max();
  // 已应用的延迟注入档位（NaN = 还没应用过）。它只用来判断"send 段的样本分布是否
  // 换了一族"：直发是 ≈0.05 ms，延迟注入是 ≈delay ms，两者混在一个 4096 样本的
  // 窗口里时，p50 会被历史样本按住、清除注入后 p99 也会被历史样本按住 136 s
  // （实测：清除 9 s 后 send p99 仍是 233 ms，agent 会读成"修了没好"）。
  double applied_delay_ms = std::numeric_limits<double>::quiet_NaN();
  double applied_jitter_ms = std::numeric_limits<double>::quiet_NaN();

  // 一帧「重组完成之后」的全部处理：解码 → 处理器链 → 编码 → 令牌桶 → FEC → 发送。
  // 抽成 lambda 是因为抖动缓冲在**三个不同时刻**放行帧（push 当场 / 周期 tick / 断连接 flush），
  // 三条入口必须走同一条路径，否则「jb 放行的帧」和「直接处理的帧」会长出两套行为。
  auto process_frame = [&](cue::media::JitterFrame&& jf) {
    const uint64_t pts_us = jf.pts_us;
    const uint32_t frame_id = jf.frame_id;
    bool keyframe = jf.keyframe;

    // ---- 实时流不追赶积压 ----
    // 编码器变慢时（placebo/1600x900 实测 >33 ms/帧），WS 上行会在 socket 缓冲里堆积。
    // 恢复后若仍按"读到就处理"消费，cued 会以远高于标称帧率的速率追帧（实测 40~109 fps），
    // 每个追赶帧的端到端延迟都是几百毫秒到几秒。旧实现靠"改参数就重启"把积压一起丢掉，
    // 所以问题被掩盖；改参数不再重启（T-05）之后它立刻暴露成 AC-15 回归。
    // 这里连关键帧一起丢：积压到这个程度时，"回到实时"比"花屏到下一个 IDR"更重要——
    // 实时链路的正确降级是丢帧，不是排队。
    if (max_ingress_lag_ms > 0) {
      const int64_t now = cue::now_us();
      const int64_t raw = now - static_cast<int64_t>(pts_us);
      if (raw < ingress_min_offset) ingress_min_offset = raw;
      const int64_t age = raw - ingress_min_offset;
      if (age > max_ingress_lag_ms * 1000) {
        ++c.ingress_late;
        if (c.ingress_late == 1 || (c.ingress_late % 30) == 0) {
          emit_event("pipeline.ingress_late",
                     json{{"frame_id", frame_id},
                          {"age_us", age},
                          {"keyframe", keyframe},
                          {"dropped_total", c.ingress_late},
                          {"budget_us", max_ingress_lag_ms * 1000}});
        }
        return;
      }
    }
    // 每帧取一次参数快照（几十字节的拷贝）：整帧内编码器、注入、排队看到的是**同一个
    // 版本**，不会出现「bitrate 换了但 gop 还是旧的」这种半截配置。
    const auto snap = params.snapshot();
    const cue::pipeline::RuntimeParams& rp = snap.params;
    if (snap.generation != last_gen) {
      // 令牌桶速率原地改：速率是纯状态，改它不丢令牌、不打乱突发
      bucket.set_rate(rp.send_mbps * 1e6);
      last_gen = snap.generation;
    }
    // ---- 能力协商（FR-PIPE.12）：档位 = 配置夹到设备能力 ----
    // 只在"参数代变了"或"收到新的能力上报"时重算（assign_profile 会建 JSON，不能每帧跑）。
    // 档位真变了才重建编码器：参数改了但夹取结果相同（例如 720p→800p 都夹到 480p）
    // 不需要动编码器——这正是"按能力下发"与"改参数就重启"的区别。
    if (profile_caps_pending || snap.encoder_generation != profile_enc_gen) {
      const bool from_caps = profile_caps_pending;
      profile_caps_pending = false;
      profile_enc_gen = snap.encoder_generation;
      cue::pipeline::AssignedProfile next =
          cue::pipeline::assign_profile(snap.params, device_caps);
      if (!cue::pipeline::same_profile(next, profile)) {
        profile = next;
        // 先关编码器，下一帧懒重建。**WS 会话、解码器、抖动缓冲全都原地保留**——
        // FR-PIPE.5「切换时不断流」就落在这里。
        const int64_t t_rebuild0 = cue::now_us();
        chain.close();
        chain_ready = false;
        chain_failed_gen = kNoGeneration;
        emit_event("pipeline.encoder_rebuild",
                   json{{"reason", from_caps ? "caps" : "param_change"},
                        {"elapsed_us", cue::now_us() - t_rebuild0},
                        {"bitrate_kbps", profile.bitrate_kbps},
                        {"gop", profile.gop},
                        {"preset", profile.preset},
                        {"transcode", profile.transcode ? "on" : "off"}});
        registry.set_profile(session_id, profile);
        emit_event("device.profile_assigned",
                   json{{"session_id", session_id},
                        {"device_id", device_id},
                        {"profile", profile_json(profile)},
                        {"basis", profile.basis},
                        {"clamped", profile.clamped},
                        {"detail", profile.detail}});
      }
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
    stage_push(1, jf.wait_us);

    const int64_t t_dec0 = cue::now_us();
    std::string derr;
    AVFrame* decoded = decoder.decode(jf.payload, derr);
    stage_push(2, cue::now_us() - t_dec0);
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

    if (profile.transcode && decoded) {
      if (!chain_ready && chain_failed_gen != profile_enc_gen) {
        // 用**下发档位**而不是原始参数配置编码器：这就是"设备只支持 480p 就不给
        // 它 720p 的编码参数"落地的地方（宽高/码率/帧率/preset 都从 profile 取）。
        const int dw = profile.width > 0 ? profile.width : decoded->width;
        const int dh = profile.height > 0 ? profile.height : decoded->height;
        std::string cerr;
        // 重建耗时（close + configure）：这是「改编码参数」付出的全部停顿，
        // 事件里如实报出来（单位 µs），报告里"断流多少 ms"直接引用这个数。
        const int64_t t_cfg0 = cue::now_us();
        const bool cfg_ok =
            chain.configure(dw, dh, profile.bitrate_kbps, profile.fps, profile.gop,
                            profile.preset, cerr, profile.threads);
        const int64_t cfg_us = cue::now_us() - t_cfg0;
        if (cfg_ok) {
          chain_ready = true;
          emit_event("pipeline.encoder",
                     json{{"phase", "configured"},
                          {"elapsed_us", cfg_us},
                          {"width", dw},
                          {"height", dh},
                          {"bitrate_kbps", profile.bitrate_kbps},
                          {"gop", profile.gop},
                          {"preset", profile.preset},
                          {"session_id", session_id}});
        } else {
          // 配置失败**不**再偷偷把 transcode 改成 off（旧实现的静默降级）：
          // 参数仍按用户要求保持 on，失败按"这一代失败"记账，等参数再变时重试。
          chain_failed_gen = profile_enc_gen;
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
    stage_push(3, std::max<int64_t>(0, t_op_end - t_op0 - enc_us));
    if (enc_sampled) stage_push(4, enc_us);

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
    // 延迟注入是否生效（用本帧已经取好的快照，避免同一帧看到两组注入值）。
    // 生效时 send 段与 frames_out 在"真正发出"时结算，而不是在分片完成时——否则
    // 延迟注入在指标上完全不可见（D-7 的成因就是这个）。
    const bool delay_on = rp.inject_delay_ms > 0.0 || rp.inject_jitter_ms > 0.0;
    session_delay_on = delay_on;
    bool send_settled = false;  // send 段已经由真正发出的一方采样过
    if (rp.inject_delay_ms != applied_delay_ms || rp.inject_jitter_ms != applied_jitter_ms) {
      // 注入档位变了：send 段从现在起测的是另一种分布，旧样本一并丢弃。
      // 不清的话"清除注入"在指标上要等 4096 帧（≈136 s @30fps）才看得出来。
      stage_clear(5);
      applied_delay_ms = rp.inject_delay_ms;
      applied_jitter_ms = rp.inject_jitter_ms;
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
      shared->xmit.put(frame_id, datagrams);

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

      // XOR-FEC（FR-PIPE.9）：每 N 个数据包一组，每组 1 个冗余包。
      // N 从运行期参数现取（set_param fec_group 下一帧生效）；分组/异或都走 wire.h，
      // 与 probe 的恢复逻辑同一份实现。帧内组分片 = 连续 frag 下标，组起始片号写进
      // 数据报头的 frag_index，member_count 写本组人数——接收端据此定位组员。
      if (fec_enabled && datagrams.size() > 1) {
        const std::size_t group_size = static_cast<std::size_t>(
            std::clamp(rp.fec_group, 1, static_cast<int>(cue::proto::kMaxFecGroupSize)));
        const auto groups = cue::proto::fec_group_spans(datagrams.size(), group_size);
        outgoing.reserve(outgoing.size() + groups.size());
        for (std::size_t g = 0; g < groups.size(); ++g) {
          const auto& span = groups[g];
          const std::size_t begin = span.offset;
          const std::size_t end = begin + span.member_count;
          // 载荷视图：跳过数据报头（冗余只覆盖 payload，不覆盖逐包变化的头）
          std::vector<std::vector<uint8_t>> payloads;
          payloads.reserve(span.member_count);
          for (std::size_t i = begin; i < end; ++i) {
            const auto& d = datagrams[i];
            payloads.emplace_back(d.begin() + cue::proto::kDatagramHeaderSize, d.end());
          }
          const std::vector<uint8_t> parity = cue::proto::fec_xor_parity(payloads);
          cue::proto::DatagramHeader fh;
          fh.type = static_cast<uint8_t>(cue::proto::DownType::Fec);
          fh.session_id = session_id;
          fh.frame_id = frame_id;
          fh.frag_index = span.offset;  // 组起始片号（旧格式恒 0）
          fh.frag_count = static_cast<uint16_t>(datagrams.size());
          fh.pts_us = pts_us;
          // 冗余包本身不是可解码帧：显式不标注 codec，免得 probe 拿奇偶校验字节去解码
          fh.codec = cue::proto::kDownCodecUnspecified;
          const auto hdr = cue::proto::encode(fh);
          std::vector<uint8_t> fpkt(hdr.begin(), hdr.end());
          cue::proto::FecPayloadHeader ph;
          ph.base_frame_id = frame_id;
          ph.group_index = static_cast<uint16_t>(g);
          ph.member_count = span.member_count;
          const auto phb = cue::proto::encode(ph);
          fpkt.insert(fpkt.end(), phb.begin(), phb.end());
          fpkt.insert(fpkt.end(), parity.begin(), parity.end());
          if (should_drop_pkt(rp.inject_seed, out_permille, pkt_seq++)) continue;
          outgoing.push_back(std::move(fpkt));
          ++c.fec_groups;
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
          sendq.push_back(QueuedSend{std::move(outgoing), due, t_send0, frame_id});
          drain_sendq(t_send0);
          // 无延迟注入时沿用旧口径（send 段 = 入队耗时）；有延迟注入时，
          // 这一帧的 send 段由 flush_delayq 在实际发出时采样。
          send_settled = delay_on;
        }
      } else {
        // 真实上线率：只要有一包没被内核接受，这一帧就不算发出去（review #16）
        send_settled = deliver_frame(std::move(outgoing), frame_id, t_send0);
      }
    }
    // 没被延迟注入压住的帧在这里采样；被压住的帧由 flush_delayq 在实际发出时采样
    // （含压住的时长），所以 send 段能反映真实延迟而不是"进队耗时"。
    if (!send_settled) stage_push(5, cue::now_us() - t_send0);
  };

  // 关键帧请求的冷却与在途合并状态（FR-PIPE.11）。必须在 for(;;) **外面**：
  // 写在循环体内的话每个包都会重置，抑制等于没做。
  // 每个会话一份（这个 for 循环就是一个会话的生命周期），两个设备互不影响。
  // 500 ms 的取法：一次 NACK 往返通常 <50 ms，客户端丢包后连发几条是常态；
  // 比这更密的请求只是同一件事的重复，落进事件流只会把控制台刷满。
  constexpr int64_t kKeyframeReqCooldownUs = 500'000;
  int64_t last_kf_req_us = 0;
  uint32_t last_kf_req_frame = 0;
  bool have_kf_req = false;
  // 下行音频包的独立帧号（与视频的 frame_id 分开）：probe 侧按会话号 + 帧号判丢包，
  // 音频恒单分片，所以这个号就是"第几个音频包"，缺口即丢失。
  uint32_t audio_frame_seq = 0;
  // 音频出向注入播的包序号（N-01）：与视频的 pkt_seq 分开，见音频分支里的注释。
  uint64_t audio_drop_seq = 0;
  // 下行转发上来的关键帧请求：按"计数变了"消费，冷却复用上游那套 500 ms 窗口。
  uint64_t kf_reqs_seen = 0;
  int64_t last_down_kf_us = 0;

  for (;;) {
    beast::flat_buffer buf;
    ws.read(buf, ec);
    if (ec) break;
    // 租约续租（FR-PIPE.12）：判据是「这个 WS 上还收得到字节」，不是"TCP 连接还在"。
    // 放在注入判定之前：--inject-ingress-drop 是**软丢包**注入，客户端其实还活着，
    // 不该因此被判死（半开连接的特征是"一个字节都收不到"）。
    registry.touch(session_id, cue::now_us());

    // ---- 接收端请求关键帧（probe 丢了帧 ⇒ H.264 参考链已断）----
    //
    // 为什么必须走到编码侧：cued 是转发器，没有编码控制权。它唯一能做的正确的事
    // 是把请求透传给采集端，由后者在下一帧插一个 IDR（见 cue-capture 的
    // WsSender::poll_control + X264Encoder::force_keyframe）。这样恢复时间从
    // "最坏一个 GOP"（默认 1 秒）降到"一个往返"（本机 <1 ms）。
    //
    // 参数集不需要单独补发：repeat-headers 现在是编码侧的契约，每个 IDR 都自带
    // SPS/PPS，而 probe 自己也会缓存见过的那一组并在需要时前置重喂
    // （cue-probe.cpp 的 dec.params / params_ready）。所以一次强制 IDR 同时解决
    // "参考链断了"和"参数集缺了"两件事。
    {
      const uint64_t want = shared->keyframe_reqs.load(std::memory_order_relaxed);
      if (want != kf_reqs_seen) {
        kf_reqs_seen = want;
        const int64_t now_kf = cue::now_us();
        if (now_kf - last_down_kf_us >= kKeyframeReqCooldownUs) {
          last_down_kf_us = now_kf;
          cue::proto::MediaHeader kh;
          kh.type = static_cast<uint8_t>(cue::proto::MediaType::KeyframeReq);
          kh.stream_id = upstream_stream_id;
          kh.frame_id = 0;
          kh.payload_len = 0;
          const auto kb = cue::proto::encode(kh);
          beast::error_code kec;
          ws.binary(true);
          ws.write(net::buffer(kb.data(), kb.size()), kec);
          if (kec) {
            // 写失败说明 WS 已经坏了：交给下一轮的 read 去判定退出，
            // 这里不额外记错误（否则同一条断开会被记两遍）。
            emit_event("keyframe.upstream_write_failed", json{{"error", kec.message()}});
          } else {
            emit_event("keyframe.forwarded_upstream",
                       json{{"stream_id", upstream_stream_id},
                            {"requests_total", want},
                            {"cooldown_ms", kKeyframeReqCooldownUs / 1000}});
          }
        }
      }
    }

    // 周期任务必须与「帧是否完成」解耦：丢尾片的帧走不到完成路径，若把清理挂在
    // 完成之后，pending 只增不减、2 s 阈值形同虚设（review #5）。这里每包都检查，
    // 500 ms 一次真正清理。
    {
      const int64_t now_tick = cue::now_us();
      // 延迟注入的到期包必须由时间驱动发出：只靠"下一帧到达"驱动的话，
      // 推流一停，最后一批被压住的数据报要等下一秒的报点才动（延迟被放大成 1 s）。
      flush_delayq(now_tick);
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
        // 刚由 sendq 挪进延迟队列的包如果已到期（延迟很小），同一拍也发出去，
        // 否则这一秒的 send 段会漏采这些帧。
        flush_delayq(now_tick);
        // 报点前同样先放行抖动缓冲里的到期帧：否则这一秒的 jb_depth 会把"其实已经到期"
        // 的帧算成积压，buffer 段也会少采样几帧。
        for (auto& ready : jb.poll(now_tick)) process_frame(std::move(ready));
        c.decode_drops = decoder.stats().dropped;
        // 本会话的读数交给报告线程（它负责聚合成每秒一行；per_session 里就是这一份）
        registry.publish_stats(session_id,
                               session_snapshot(rings, c, now_tick, t_last_report,
                                                frames_prev, jb.stats(), shared->fb,
                                                upstream_stream_id));
        frames_prev = c.frames_out;
        t_last_report = now_tick;
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
      publish_last_codec(codec_cfg);  // 供下一路单设备连接回放（多设备时不回放）
      continue;
    }
    if (h->type == static_cast<uint8_t>(cue::proto::MediaType::KeyframeReq)) {
      // 只落事件流（M5 控制台可见"谁在请求关键帧"），暂不强制下发：
      // 强制 IDR 是编码侧（浏览器）的职责，cued 没有编码控制权。
      //
      // 但**必须去重**（FR-PIPE.11）：丢包后客户端连发 KeyframeReq 是常态，
      // 原样全落事件会让事件流被同一请求刷满，控制台上看到的是噪声而不是信号。
      // 两种抑制：①冷却窗口内（距上次请求 < kKeyframeReqCooldownUs）；
      //           ②同一 frame_id 的重复（在途合并——它请求的就是同一帧，合并成一次）。
      const int64_t now_kf = cue::now_us();
      const bool same_frame = have_kf_req && h->frame_id == last_kf_req_frame;
      const bool cooling = have_kf_req && (now_kf - last_kf_req_us) < kKeyframeReqCooldownUs;
      if (same_frame || cooling) {
        ++c.kf_req_suppressed;
        emit_event("keyframe.request_suppressed",
                   json{{"stream_id", h->stream_id},
                        {"frame_id", h->frame_id},
                        {"reason", same_frame ? "same_frame" : "cooldown"}});
        continue;
      }
      last_kf_req_us = now_kf;
      last_kf_req_frame = h->frame_id;
      have_kf_req = true;
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
        caps["session_id"] = session_id;  // 谁报的：会话号由服务端分配，发送端不必自报
        const std::string kind = caps.value("kind", std::string("unknown"));
        if (kind == "caps") {
          // 设备能力上报（FR-PIPE.12）：这是"设备注册"里除连接之外的另一半。
          cue::pipeline::DeviceCaps dc = cue::pipeline::parse_device_caps(caps);
          std::vector<uint32_t> replaced_ids;
          bool relabeled = false;
          std::string old_device_id;
          registry.set_caps(session_id, dc, replaced_ids, relabeled, old_device_id);
          device_caps = dc;
          profile_caps_pending = true;  // 下一帧按新能力重算档位（见 process_frame）
          if (relabeled) {
            device_id = dc.device_id;
            emit_event("device.relabeled", json{{"session_id", session_id},
                                                {"old_device_id", old_device_id},
                                                {"new_device_id", device_id}});
          }
          for (const uint32_t old_id : replaced_ids) {
            emit_event("device.replaced", json{{"device_id", device_id},
                                               {"old_session_id", old_id},
                                               {"new_session_id", session_id},
                                               {"reason", "same_device_reconnected"}});
          }
          emit_event("device.caps", json{{"session_id", session_id},
                                         {"device_id", device_id},
                                         {"caps", cue::pipeline::device_caps_json(dc)},
                                         // 原始载荷原样留着：收端不认识的新键不丢
                                         {"raw", caps}});
        } else if (kind == "heartbeat") {
          // 心跳只用来续租，已经由每次 read 的 touch() 完成；不单独发事件
          // （1 Hz 的心跳会把 256 条的订阅事件环冲掉，"有事件"就贬值了）。
        } else {
          // 事件名不能由发送端随便定（上行来自浏览器，任何页面/扩展都能发字节）：
          // 只放行白名单，未知 kind 统一落 device.event，原值仍留在载荷里可查。
          const char* ev = "device.event";
          if (kind == "codec.degraded") ev = "device.codec_degraded";
          else if (kind == "audio.disabled") ev = "device.audio_disabled";
          else if (kind == "audio.started") ev = "device.audio_started";
          emit_event(ev, caps);
        }
      } else {
        ++c.rejected;  // 载荷不是 JSON 对象：这次上报作废，但不影响媒体流继续
      }
      continue;
    }
    if (h->type == static_cast<uint8_t>(cue::proto::MediaType::Audio)) {
      // 音频上行（FR-DEV.6）：载荷是**裸 Opus 包**。依据是协议本身，不是猜的：
      //   · proto::upstream_pack.h 的 pack_upstream() 只写 32 字节 MediaHeader + 原样载荷；
      //   · cue-capture 的 audio_loop() 把 OpusAudioEncoder 的输出直接交给它；
      //   · 浏览器 web/app.js 的 sendMediaPacket 同样是 32 字节头 + EncodedAudioChunk 字节；
      //   · wire.h 里 MediaType::Audio 没有二级头（sampleRate/声道数只在 0x03/0x05
      //     的能力上报里，不在每帧路径上）。
      // 所以这里从 payload 起始处直接解，不跳任何字节。
      //
      // 解出的 PCM 只用来算指标（RMS/耗时），下行转发的是**原始字节**，不是解码结果
      // （见下面的"音频下行"段：DownType::Audio 在 N-01 之前就已经有发送方）。
      // 20 ms 的 Opus 帧只有几十~几百字节，远低于 16 KiB 分片上限，浏览器与 cue-capture
      // 都恒单分片；多分片的包不重组（没有发送端会这么做，重组只会把异常掩盖成正常），
      // 单独计数（audio_multi_frag），让「音频在流但口径不完整」可见。
      if (h->frag_count == 1) {
        ++c.audio_frames_in;
        c.audio_bytes_in += h->payload_len;
        c.audio_last_pts_us = static_cast<int64_t>(h->pts_us);
        if (!audio_dec.is_open()) {
          std::string oerr;
          if (!audio_dec.open(oerr)) {
            // 打不开不是"这一帧解不出"，是整条音频路径废了：照样按帧计错误
            // （否则 audio_decode_errors=0 会被读成"只是没音频"），原因只留第一条。
            ++c.audio_decode_errors;
            if (c.audio_first_error.empty()) {
              c.audio_first_error = oerr.substr(0, kMaxAudioErrorLen);
            }
            err_log.log(cue::now_us(), "opus 解码器打开失败：" + oerr);
            continue;
          }
        }
        const int64_t t_dec0 = cue::now_us();
        std::string derr;
        auto frame =
            audio_dec.decode({p + cue::proto::kMediaHeaderSize, h->payload_len}, derr);
        const int64_t dec_us = cue::now_us() - t_dec0;
        if (!frame) {
          ++c.audio_decode_errors;
          const std::string why = std::string(cue::media::audio_decode_error_name(frame.error())) +
                                  ": " + derr;
          if (c.audio_first_error.empty()) {
            c.audio_first_error = why.substr(0, kMaxAudioErrorLen);
          }
          err_log.log(cue::now_us(), "opus 解码失败（" + why + "）");
        } else {
          ++c.audio_decoded_frames;
          c.audio_pcm_frames += frame->sample_count();
          c.audio_decode_us.push(dec_us);
          c.audio_rms.push(frame->samples.data(), frame->samples.size());
        }
        // ---- 音频下行（FR-PIPE.4 的补全）----
        // 上行载荷就是**裸 Opus 包**，下行原样转发，不做解码-重编码：这是 passthrough
        // 语义的正确形态（少一次编解码就少一份延迟），也让"转码开启"只影响视频。
        //
        // 三个刻意的取舍：
        //   · **不进 sendq/delayq**：那两条队列是为视频帧的限速与延迟注入准备的，
        //     音频帧只有几十~几百字节、20 ms 一发，排队只会把延迟做大；
        //   · **不参与抖动缓冲**：jb 的状态机是按视频帧间隔估的，混入音频会把
        //     interval 估计带偏；probe 侧按 pts 自行平滑；
        //   · **解码失败照发**：解码只是给指标用的旁路，上行包本身是完好的，
        //     因为"本端解不出"就不转发，等于把本机的解码问题变成对端的静音。
        {
          cue::proto::DatagramHeader ah;
          ah.type = static_cast<uint8_t>(cue::proto::DownType::Audio);
          ah.flags = 0;  // 下行没有 LastFrag 标志：单分片由 frag_index/frag_count 直接表达
          ah.session_id = session_id;
          ah.frame_id = audio_frame_seq++;
          ah.frag_index = 0;
          ah.frag_count = 1;   // 20 ms 的 Opus 帧恒 < 16 KiB，不分片
          ah.pts_us = h->pts_us;
          ah.codec = 0;        // 音频不标 codec（下行 codec 字段只用于视频解码选择）
          const auto ahdr = cue::proto::encode(ah);
          std::vector<uint8_t> apkt(ahdr.begin(), ahdr.end());
          const uint8_t* ap = p + cue::proto::kMediaHeaderSize;
          apkt.insert(apkt.end(), ap, ap + h->payload_len);
          // ---- 音频也参与出向注入（N-01）----
          // 旧实现只把 --inject-drop 打在视频分片上，音频直发：于是"下行注入 5% 丢包"
          // 在音频侧根本没有发生，FEC/PLC 的验收无从谈起。
          // 序列独立：seed 派生 ^0xA5A5A5A5（与视频的 pkt_seq、上行的 in_seq 都不共用
          // 序号空间）。两条流因此互不相关，但同一 (seed, pct) 仍然逐包可复现。
          // 每包现取一次快照（50 包/秒，开销可忽略）——控制面改注入立刻生效。
          // audio_frames_out 的语义是「真正发出去的音频包数」：被注入吃掉的不计，
          // 与 audio_frames_in 的差就只反映"发不出去"（send_dgram 失败）。
          const auto asnap = params.snapshot();
          if (should_drop_pkt(asnap.params.inject_seed ^ 0xA5A5A5A5ULL,
                              permille_from_pct(asnap.params.inject_drop), audio_drop_seq++)) {
            ++c.audio_injected_drops;
          } else if (send_dgram(apkt)) {
            ++c.audio_frames_out;
          }
        }
      } else {
        ++c.audio_multi_frag;
      }
      continue;
    }
    if (h->type != static_cast<uint8_t>(cue::proto::MediaType::Video)) continue;

    // 客户端自报的流号只作记录：下行数据包头里的 session_id 是**服务端分配**的
    // 会话号（两路客户端都写 stream_id=1 时，下行才分得清）。
    upstream_stream_id = h->stream_id;
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
    stage_push(0, t_reasm_done - fa.first_seen_us);

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
  // 同样把延迟队列里在途的包放出去：断连接时静默丢弃会让每秒少发几十个包，
  // probe 侧看到的是 PTS 空洞（一次误判的"丢包"）。
  flush_delayq(cue::now_us() + 10'000'000);

  // ---- 断连收尾 ----
  // 先取回收原因再清表：lease_expired / session.kill / device_replaced 都写在表里，
  // unregister 之后就读不到了。顺序反了，事件里永远是 client_closed。
  const std::string close_reason = registry.close_reason(session_id);
  // 清表必须先于 socket 关闭：kill 方是在表锁内 shutdown 的，表里还有这一行时它拿到的
  // fd 一定是这一条的；先关再清，新连接可能已经复用了同一个 fd 号。
  registry.unregister(session_id);
  if (close_reason == "lease_expired") {
    emit_event("device.lease_expired",
               json{{"session_id", session_id},
                    {"device_id", device_id},
                    {"peer", peer},
                    {"lease_ms", reg->lease_us / 1000},
                    // 判据是"租约内没有收到过任何字节"，不是"TCP 连接是否还活着"：
                    // 半开连接（客户端进程被冻结/崩溃，内核仍替它 ACK）只有租约能发现。
                    {"basis", "no_ws_message_within_lease"}});
  }
  emit_event("device.unregistered", json{{"session_id", session_id},
                                         {"device_id", device_id},
                                         {"peer", peer},
                                         {"reason", close_reason.empty() ? "client_closed"
                                                                         : close_reason}});
  cued_log("[cued] client disconnected: session=%u device=%s reason=%s\n", session_id,
           device_id.c_str(), close_reason.empty() ? "client_closed" : close_reason.c_str());
}


int main(int argc, char** argv) {
  g_process_start_us = cue::now_us();
  uint16_t ws_port = 9000;
  // WS 监听地址默认只回环：cued 没有鉴权，默认对外等于把解码/转码入口交给整个网络。
  // 跨机演示必须显式 --bind 0.0.0.0（并在报告/README 里说明）。
  std::string bind_addr = "127.0.0.1";
  std::string probe_host = "127.0.0.1";
  uint16_t probe_port = 9001;
  // 多设备会话（FR-PIPE.12）：并发上限与租约。默认值按 2 vCPU 的实测上限
  // （README「已知限制」）与"15 s 内一定能发现半开连接"来取。
  long long max_sessions = 2;
  long long lease_ms = 15000;

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
  // 延迟注入（FR-PIPE.10）：固定延迟与抖动幅度，单位毫秒。0 = 关（默认，发送路径与
  // M2 逐字节一致）。上限 2000 ms：再大就不只是"网络时延"，而是"链路断了在攒帧"，
  // 那不是延迟注入能表达的东西。
  double inject_delay_ms = 0.0;
  double inject_jitter_ms = 0.0;
  uint64_t inject_seed = 42;
  std::string inject_trace;
  bool fec_enabled = true;
  // FEC 组大小（FR-PIPE.9）：每 N 个数据包一组、每组 1 个 XOR 冗余。默认 8。
  // 1 = 每包一个副本（100% 冗余，仅用于对比实验）；大帧 >N 片时自动切成多组。
  long long fec_group = static_cast<long long>(cue::proto::kDefaultFecGroupSize);
  // AIMD（技术设计 §4.2）：仅 transcode=on 时按 feedback 的 loss_rate 调码率。
  // 默认 **off**：开启后无丢包时每 3 个 feedback 窗口升 5%，每次升档都重建编码器；
  // 这会让"没有网络故障"的基线也带上一秒一次的编码器抖动（实测把修复后的 e2e p99
  // 抬高 ~60 ms）。评测/A-B 需要确定性，所以默认关，`--aimd on` 或运行期
  // set_param aimd=on 显式开启。关掉之后码率只由手动参数决定。
  bool aimd_enabled = false;
  // 抖动缓冲（FR-PIPE.2）。默认值刻意取小：目标深度 2 帧 = 1 个帧间隔（30 fps ≈ 33 ms）的
  // 驻留，等待预算 40 ms 只用来兜「缺帧时要不要再等一会儿」。把这两个数调大能把乱序纠正
  // 得更彻底，代价是端到端延迟线性上升——所以默认不下"几百 ms"那种手。
  bool jitter_enabled = true;
  long long jitter_max_frames = 8;     // 队列容量硬上限（帧）
  long long jitter_budget_ms = 40;     // 缺帧等待预算
  long long jitter_target_frames = 1;  // 固定目标深度（帧）；1 = 不驻留，只靠空洞预算纠乱序
  // 抖动估计的基准点（与 probe 的 --jitter-basis 同名同义）：
  //   first = 首片到达时刻（默认）：反映**网络**抖动
  //   done  = 帧组装完成时刻：混入「整帧在途耗时 ≈ 帧字节数/带宽」，大 IDR 会周期性抬升它
  // 这个开关是为了能 A/B 量化后者对档位的影响，默认值与 probe 保持一致。
  std::string jitter_basis = "first";
  // 迟到帧预算（毫秒）：帧相对"最小延迟基线"晚到超过这个值就丢弃，见 process_frame 开头。
  // 0 = 关闭。300 ms 的取法：够编码器换档时的短暂抖动，又远小于"追赶积压"造成的秒级延迟。
  long long max_ingress_lag_ms = 300;

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
    } else if (a == "--max-sessions") {
      max_sessions = want_int(1, cue::pipeline::kMaxSessionsHardLimit);
    } else if (a == "--lease-ms") {
      // 租约 = "租约内没收到这个客户端的任何字节就回收"。下限 200 ms 只用于测试/演示。
      lease_ms = want_int(200, 600000);
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
    } else if (a == "--inject-delay-ms") {
      const auto val = next();
      if (!parse_double(val, 0.0, 2000.0, inject_delay_ms)) bad(val);
    } else if (a == "--inject-jitter-ms") {
      const auto val = next();
      if (!parse_double(val, 0.0, 2000.0, inject_jitter_ms)) bad(val);
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
    } else if (a == "--jitter-basis") {
      jitter_basis = next();
      if (jitter_basis != "first" && jitter_basis != "done") bad(jitter_basis);
    } else if (a == "--max-ingress-lag-ms") {
      max_ingress_lag_ms = want_int(0, 10000);
    } else if (a == "--fec") {
      const auto val = next();
      if (val != "on" && val != "off") bad(val);
      fec_enabled = (val == "on");
    } else if (a == "--fec-group") {
      fec_group = want_int(1, static_cast<long long>(cue::proto::kMaxFecGroupSize));
    } else if (a == "--aimd") {
      const auto val = next();
      if (val != "on" && val != "off") bad(val);
      aimd_enabled = (val == "on");
    } else if (a == "--help") {
      std::printf(
          "cued --ws-port 9000 [--bind 127.0.0.1] --probe-host 127.0.0.1 --probe-port 9001\n"
          "     [--transcode on|off] [--width N --height N] [--bitrate kbps] [--fps N]\n"
          "     [--gop N] [--preset NAME] [--threads N] [--send-mbps F]\n"
          "     [--inject-drop 0..100] [--inject-ingress-drop 0..100] [--queue-fps N]\n"
          "     [--inject-delay-ms 0..2000] [--inject-jitter-ms 0..2000]\n"
          "     [--inject-seed N] [--inject-trace FILE] [--fec on|off]\n"
          "     [--fec-group 1..64] [--aimd on|off]\n"
          "     [--jitter on|off] [--jitter-target-frames 2] [--jitter-max-frames 8]\n"
          "     [--jitter-budget-ms 40] [--jitter-basis first|done] [--max-ingress-lag-ms 300]\n"
          "     [--max-sessions 1..8] [--lease-ms 200..600000]\n"
          "  多设备：第 i 路（槽位 i）的下行发往 --probe-port+i，每路要有一个 cue-probe 在听；\n"
          "  并发上限按 2 vCPU 实测回填（README 已知限制），超了直接拒绝连接。\n"
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
  jb_cfg.basis = (jitter_basis == "done") ? cue::media::JitterBasis::AssemblyDone
                                          : cue::media::JitterBasis::FirstFragment;
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
  initial_params.fec_group = static_cast<int>(fec_group);
  initial_params.aimd = aimd_enabled;
  initial_params.inject_drop = inject_drop;
  initial_params.inject_ingress_drop = inject_ingress_drop;
  initial_params.inject_delay_ms = inject_delay_ms;
  initial_params.inject_jitter_ms = inject_jitter_ms;
  initial_params.inject_seed = inject_seed;

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
  // AIMD 的运行期状态：NACK 线程独占读写（不需要锁）。feedback 的测量值按会话分开
  // （见 CuedSessionShared::fb），码率本身是进程级的。
  AimdState aimd;
  aimd.base_kbps = out_kbps;
  aimd.last_written_kbps = out_kbps;
  g_aimd_bitrate_kbps.store(out_kbps);

  // 设备会话表（T-22）。放在控制面**之前**：session.list / session.kill 的提供方要引用它。
  cue::pipeline::SessionRegistry::Options sess_opt;
  sess_opt.lease_us = lease_ms * 1000;
  sess_opt.max_sessions = static_cast<int>(max_sessions);
  sess_opt.probe_port_base = probe_port;
  cue::pipeline::SessionRegistry registry(sess_opt);

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
  // session.list / session.kill（FR-PIPE.12）：控制面只认识会话表，不认识媒体循环。
  // kill 的语义是**立即断开**（表锁内 shutdown 这个会话的 socket），不优雅排空：
  // 踢一个坏设备时等它把积压发完，等于让它继续占着发送队列和带宽。
  ctl_providers.sessions = [&registry](int64_t now_us) {
    return registry.sessions_json(now_us, /*include_closed=*/true);
  };
  ctl_providers.session_kill = [&registry](uint32_t session_id, std::string& err) {
    if (!registry.kill(session_id, "session.kill")) {
      err = "未知会话 " + std::to_string(session_id) +
            "（已断开或不存在的会话号；用 session.list 查当前活着的会话）";
      return false;
    }
    return true;
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

  // ---- 监听与 UDP 出口（进程级，所有会话共用）----
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
  // 用原生 fd：会话线程 sendto、NACK 线程 recvfrom（同一个 UDP socket 全双工）。
  // 多路复用同一个 socket 是**有意的**：下行目标按会话区分端口，来源地址只有一个，
  // 每路一个 socket 只会多出 N 份 SO_SNDBUF 与 N 个要维护的 fd。
  const int usock = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (usock < 0) {
    std::fprintf(stderr, "[cued] udp socket failed\n");
    return 1;
  }
  // 自动对端（--probe-host 0.0.0.0）时，把下行 socket 绑到**已知端口** probe_port。
  //
  // 为什么必须这么做：这个 socket 平时只是个发送方，内核会给它一个临时端口
  // （实测 57099）。而 NAT 注册要求 probe 能主动往服务器发 HELLO —— 它只能发到
  // 一个**事先知道的**端口。临时端口没人知道，HELLO 就打空，穿透永远建立不起来。
  // 绑到 probe_port 之后：HELLO 打到这个 socket，cued 从同一个 socket 回包，
  // 源地址与 NAT 映射完全对应（这也正是 STUN 的做法：固定端口交换地址）。
  if (probe_host == "0.0.0.0") {
    sockaddr_in self_addr{};
    self_addr.sin_family = AF_INET;
    self_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    self_addr.sin_port = htons(probe_port);
    if (::bind(usock, reinterpret_cast<const sockaddr*>(&self_addr), sizeof(self_addr)) != 0) {
      std::fprintf(stderr, "[cued] 绑定下行 UDP 端口 %u 失败：%s\n",
                   static_cast<unsigned>(probe_port), std::strerror(errno));
      return 1;
    }
  }
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

  // ---- 每会话下行目标与共享采样 ----
  // 会话号**由服务端分配**：客户端自报的 stream_id 只是它自己的编号，两路都写 1 时下行就
  // 没法区分（NACK/feedback 靠 session_id 回带，撞号会把重传打给另一路）。客户端的上行
  // stream_id 仍然记进每会话读数（stream_id 键）里可查。
  cued_log("[cued] ws %s:%u  udp %s:%u..%u (max_sessions=%lld lease=%lldms)\n",
           bind_addr.c_str(), ws_port, probe_host.c_str(), probe_port,
           static_cast<unsigned>(probe_port + max_sessions - 1), max_sessions, lease_ms);
  SharedRings shared_rings;

  // 会话线程要用的启动期配置（运行期可变的参数一律走 ParamStore 快照，不在这里）。
  SessionEnv env;
  env.params = &params;
  env.registry = &registry;
  env.rings = &shared_rings;
  env.jb_cfg = jb_cfg;
  env.usock = usock;
  env.fec_enabled = fec_enabled;
  env.max_ingress_lag_ms = static_cast<int>(max_ingress_lag_ms);
  env.trace_fp = trace_fp;
  env.probe_host = probe_host;

  // ---- AIMD（技术设计 §4.2）：只改 ParamStore 的 bitrate_kbps ----
  // 走的是**现有的懒重建路径**：ParamStore 推进 encoder_generation → 各会话下一帧
  // close+configure，WS 会话/解码器/抖动缓冲原地保留。
  auto aimd_apply = [&](int new_kbps, int old_kbps, float loss, const char* reason,
                        float rtt_ms, int64_t now) {
    bool changed = false;
    std::string err;
    if (!params.set_param("bitrate_kbps", std::to_string(new_kbps), changed, err)) {
      emit_event("pipeline.aimd_error", json{{"reason", err},
                                             {"requested_kbps", new_kbps},
                                             {"current_kbps", old_kbps}});
      return;
    }
    if (!changed) return;
    aimd.last_written_kbps = new_kbps;
    aimd.last_change_us = now;
    g_aimd_steps.fetch_add(1, std::memory_order_relaxed);
    g_aimd_bitrate_kbps.store(new_kbps, std::memory_order_relaxed);
    emit_event("pipeline.aimd",
               json{{"old_kbps", old_kbps},
                    {"new_kbps", new_kbps},
                    {"loss_rate", loss},
                    {"rtt_ms", rtt_ms},
                    {"reason", reason},
                    {"base_kbps", aimd.base_kbps},
                    {"cap_kbps", std::min(20000, aimd.base_kbps * 2)},
                    {"encoder_rebuild", true},
                    {"scope", "feedback"}});
  };

  // 每一条 feedback 调用一次。loss>10% → 降 10%（下限 200 kbps）；
  // 连续 3 个 feedback 窗口 loss<2% → 升 5%（上限 2×手动基准，且不超过 20000）。
  // 两次调整至少间隔 1 s：feedback 是 200 ms 一条，逐条重建编码器会把 x264 打爆。
  auto aimd_on_feedback = [&](const cue::pipeline::ParamStore::Snapshot& snap,
                              const cue::proto::FeedbackPayload& f, int64_t now) {
    const auto& rp = snap.params;
    if (!rp.transcode || !rp.aimd) {
      aimd.low_streak = 0;
      return;
    }
    if (!std::isfinite(f.loss_rate)) return;
    const int cur = rp.bitrate_kbps;
    if (cur != aimd.last_written_kbps) {
      // 当前码率不是 AIMD 上次写的值 → 有人手动改过（pipeline.set_param / startup）。
      // 以手动值为新基准：手动优先，AIMD 从它继续调，不把手动值覆盖回旧码率。
      aimd.base_kbps = std::max(cur, 1);
      aimd.last_written_kbps = cur;
      aimd.low_streak = 0;
    }
    if (aimd.base_kbps <= 0) aimd.base_kbps = std::max(cur, 1);
    const double loss = double(f.loss_rate);
    if (loss > 0.10) {
      aimd.low_streak = 0;
      if (now - aimd.last_change_us < 1'000'000) return;
      const int next = std::max(200, cur - cur / 10);
      if (next < cur) aimd_apply(next, cur, f.loss_rate, "loss", f.rtt_ms, now);
      return;
    }
    if (loss < 0.02) {
      if (++aimd.low_streak < 3) return;
      aimd.low_streak = 0;
      if (now - aimd.last_change_us < 1'000'000) return;
      const int cap = std::min(20000, aimd.base_kbps * 2);
      const int next = std::min(cap, cur + std::max(1, cur / 20));
      if (next > cur) aimd_apply(next, cur, f.loss_rate, "recover", f.rtt_ms, now);
      return;
    }
    aimd.low_streak = 0;
  };

  // ---- NACK / feedback / 时钟对齐线程（一个 UDP socket，多会话按 session_id 路由）----
  // 为什么必须有端口校验：多设备时每路下行发往**自己的**探针端口（README 的
  // "下行 UDP 归到哪"）。只按 session_id 路由的话，任意本机进程都能伪造一条 NACK 让
  // cued 重传另一路的包（反射放大，review #20）。所以「会话号 + 来源端口」两条都对上才处理。
  // ---- NAT 注册（公网部署必需）----
  // 公网上 cued 要往"客户端"发 UDP，而客户端在 NAT 后面：只有它先发包出去，
  // 运营商的 NAT 才会为它建立映射，外界才能回包。所以约定一个 HELLO：
  // probe 周期性发 8 字节 "CUEHELLO"，cued 记住**观察到的源地址**并把它当作下行目标。
  // 这正是 WebRTC/STUN 的心智模型（先出后进），只是省掉了完整的 ICE。
  //
  // 为什么 HELLO 可以接受任意源：它只写一个地址、不触发任何重传或放大行为，
  // 构不成反射放大器（NACK 仍然受 session_id + 源地址校验约束）。
  // 只有在 --probe-host 0.0.0.0（"自动"）时才启用，固定地址时行为完全不变。
  const bool auto_peer = (probe_host == "0.0.0.0");
  std::atomic<bool> stop_nack{false};
  std::atomic<uint64_t> nack_resends{0};
  const sockaddr_in probe_host_addr = make_probe_addr(probe_host, probe_port);
  std::thread nack_thread([&] {
    std::vector<uint8_t> nbuf(2048);
    RateLimitedLog err_log;
    bool aimd_suspended = false;
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
      if (n < static_cast<ssize_t>(cue::proto::kDatagramHeaderSize)) {
        // 短包可能是 NAT 注册的 HELLO（8 字节），单独处理后丢弃。
        if (auto_peer && n >= 8 && std::memcmp(nbuf.data(), "CUEHELLO", 8) == 0) {
          {
            std::lock_guard<std::mutex> lk(g_peer_mu);
            g_learned_peer = from;
          }
          if (!g_peer_ready.exchange(true, std::memory_order_acq_rel)) {
            char ip[INET_ADDRSTRLEN]{};
            ::inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
            emit_event("probe.registered",
                       json{{"peer", ip}, {"port", ntohs(from.sin_port)}});
          }
        }
        continue;
      }
      // 自动模式下不校验源地址/端口：NAT 会把外部端口换成另一个值，按原端口校验
      // 必然把合法的 feedback/NACK 全丢掉。会话号仍然是对的。
      if (!auto_peer && from.sin_addr.s_addr != probe_host_addr.sin_addr.s_addr) continue;
      auto nh = cue::proto::decode_datagram({nbuf.data(), static_cast<size_t>(n)});
      if (!nh) continue;
      const uint32_t sid = nh->session_id;
      const uint16_t want_port = registry.probe_port_of(sid);
      if (!auto_peer && (want_port == 0 || from.sin_port != htons(want_port))) continue;
      const std::span<const uint8_t> body{nbuf.data() + cue::proto::kDatagramHeaderSize,
                                         static_cast<std::size_t>(n) -
                                             cue::proto::kDatagramHeaderSize};
      if (nh->type == static_cast<uint8_t>(cue::proto::DownType::Feedback)) {
        // feedback（技术设计 §4.2）：4 个 f32，编解码收在 wire.h（两端各写一份必漂移）。
        const auto f = cue::proto::decode_feedback(body);
        if (!f) continue;
        auto shared = std::static_pointer_cast<CuedSessionShared>(registry.payload(sid));
        if (!shared) continue;
        FeedbackStats& fb = shared->fb;
        // 先写测量值、最后推进 rx：指标行用 fb_rx>0 判断"有没有收到过"，
        // 顺序反了就会出现「rx 已经 +1、loss 还是 -1」的一瞬（报告里变成 null vs 数字）。
        fb.loss_rate.store(double(f->loss_rate), std::memory_order_relaxed);
        fb.recv_mbps.store(double(f->recv_bitrate_mbps), std::memory_order_relaxed);
        fb.rtt_ms.store(double(f->rtt_ms), std::memory_order_relaxed);
        fb.rx.fetch_add(1, std::memory_order_relaxed);
        // 每次反馈都同步一次当前码率：指标里不能停在旧值上。
        const auto snap = params.snapshot();
        g_aimd_bitrate_kbps.store(snap.params.bitrate_kbps, std::memory_order_relaxed);
        // AIMD 写的是**进程级**码率（ParamStore 只有一份）：多路时一路好一路坏的 loss
        // 会互相打脸，没有正确答案。多路时明确挂起并留一条事件，不装作用 feedback 在调。
        const int live = registry.live_count();
        if (live == 1) {
          aimd_suspended = false;
          aimd_on_feedback(snap, *f, cue::now_us());
        } else if (!aimd_suspended) {
          aimd_suspended = true;
          emit_event("pipeline.aimd_suspended",
                     json{{"reason", "multi_session"},
                          {"sessions", live},
                          {"bitrate_kbps", snap.params.bitrate_kbps},
                          {"aimd", snap.params.aimd ? "on" : "off"}});
        }
        continue;
      }
      if (nh->type == static_cast<uint8_t>(cue::proto::DownType::KeyframeReq)) {
        // 接收端丢帧后主动要关键帧（对应 RTCP 的 PLI）。这里只登记意图，
        // 真正的两件事（向上游要 IDR、把缓存的参数集重发下去）由会话线程做——
        // 因为上游 WS 和下行参数集都在会话线程手里，跨线程直接动会打架。
        // 冷却放在会话线程：它是唯一知道"上一次请求什么时候发出去"的地方。
        auto shared = std::static_pointer_cast<CuedSessionShared>(registry.payload(sid));
        if (shared) shared->keyframe_reqs.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      if (nh->type == static_cast<uint8_t>(cue::proto::DownType::StatsReq)) {
        // SPEC §7.1 时钟对齐：probe 发下行回环请求（载荷 = 它的发戳），这里原样回显发戳
        // 并附上自己的时钟；跨机部署的端到端延迟由此有确定口径（review #9）。
        if (body.size() < 8) continue;
        cue::proto::DatagramHeader sh;
        sh.type = static_cast<uint8_t>(cue::proto::DownType::StatsReq);
        sh.session_id = sid;
        const auto shb = cue::proto::encode(sh);
        std::vector<uint8_t> rep(shb.begin(), shb.end());
        const std::size_t base = rep.size();
        rep.resize(base + 16, 0);
        std::memcpy(rep.data() + base, body.data(), 8);
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
      auto shared = std::static_pointer_cast<CuedSessionShared>(registry.payload(sid));
      if (!shared) continue;
      const auto entries = cue::proto::decode_nack_entries(body);
      for (const auto& e : entries) {
        auto pkt = shared->xmit.find(e.frame_id, e.frag_index);
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

  // ---- 报告线程 ----
  // 为什么报告不放在会话线程里：多会话时"每秒一行 METRICS"必须只有一行（解析器、控制面
  // 快照、/api/state 都按 1 Hz 读）。报告线程只聚合，不做媒体工作，因此不会被某一路的
  // 编码卡顿拖住；租约到期扫描也挂在这一拍上（100 ms 粒度，15 s 的租约足够）。
  std::thread report_thread([&] {
    int64_t t_last = cue::now_us();
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      const int64_t now = cue::now_us();
      // 租约到期：标记 + shutdown socket。断开与事件由会话线程自己收尾（它才知道
      // device_id，也才能按"先清表再发事件"的顺序做）。
      const auto expired = registry.expire_due(now);
      if (!expired.empty()) {
        std::fprintf(stderr, "[cued] 租约到期，回收 %zu 路会话\n", expired.size());
      }
      if (registry.live_count() == 0) {
        // 没有活会话就不报点：与 T-22 之前"断连后不再打 METRICS"的行为一致，
        // 也免得 harness 把一串 0 读成"管线还在跑但没帧"。
        t_last = now;
        continue;
      }
      if (now - t_last >= 1'000'000) {
        report_once(registry, shared_rings, view, now);
        t_last = now;
      }
    }
  });
  report_thread.detach();

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
                  {"fec_group", fec_group},
                  {"aimd", aimd_enabled ? "on" : "off"},
                  {"inject_drop", inject_drop},
                  {"inject_delay_ms", inject_delay_ms},
                  {"inject_jitter_ms", inject_jitter_ms},
                  {"inject_seed", inject_seed},
                  {"control_channel", control.channel_tag()},
                  {"control_socket", control.degraded()
                                         ? std::string()
                                         : "/tmp/cue/" + std::to_string(::getpid()) + "/control.sock"},
                  {"jitter", jitter_enabled ? "on" : "off"},
                  {"jitter_target_frames", jitter_target_frames},
                  {"jitter_max_frames", jitter_max_frames},
                  {"jitter_budget_ms", jitter_budget_ms},
                  // 多设备会话（FR-PIPE.12）：并发上限、租约、以及"每路下行去哪个端口"。
                  // 下行目标写在这里而不是文档里：跑起来的人从事件流就能对出端口映射。
                  {"max_sessions", max_sessions},
                  {"lease_ms", lease_ms},
                  {"probe_port_base", probe_port},
                  {"probe_port_stride", 1},
                  {"probe_port_range",
                   json::array({probe_port, static_cast<int>(probe_port) +
                                                static_cast<int>(max_sessions) - 1})}});
  // 带 --inject-drop 启动 = 这条管线上电时故障已经生效（agent 会看到 applied 而不是 cleared）
  if (inject_drop > 0.0) {
    emit_event("fault.applied", json{{"kind", "drop"},
                                     {"value", inject_drop},
                                     {"seed", inject_seed},
                                     {"scope", "cued_start"}});
  }
  // 延迟注入同样是"上电即生效"的一类故障（命令行参数与 ParamStore 初值同源）
  if (inject_delay_ms > 0.0) {
    emit_event("fault.applied", json{{"kind", "delay"},
                                     {"value", inject_delay_ms},
                                     {"seed", inject_seed},
                                     {"scope", "cued_start"}});
  }
  if (inject_jitter_ms > 0.0) {
    emit_event("fault.applied", json{{"kind", "jitter"},
                                     {"value", inject_jitter_ms},
                                     {"seed", inject_seed},
                                     {"scope", "cued_start"}});
  }

  for (;;) {
    tcp::socket sock(ioc);
    acc.accept(sock, ec);
    if (ec) continue;
    // 并发预检：表满时立刻拒绝，不握手、不建线程。真正的记账在会话线程里注册时做
    // （槽位/端口/租约都在那张表上），这里只避免为一个注定被拒的连接起线程。
    if (registry.live_count() >= registry.max_sessions()) {
      emit_event("session.rejected", json{{"reason", "max_sessions"},
                                          {"max_sessions", registry.max_sessions()},
                                          {"live", registry.live_count()}});
      std::fprintf(stderr, "[cued] 拒绝连接：并发上限 %d 路已满（--max-sessions）\n",
                   registry.max_sessions());
      beast::error_code ign;
      sock.close(ign);
      continue;
    }
    websocket::stream<tcp::socket> ws(std::move(sock));
    ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
    ws.read_message_max(8 * 1024 * 1024);
    // 握手也放进会话线程：慢客户端只拖住自己，accept 循环能继续接下一路（多设备的前提）。
    std::thread([&env, w = std::move(ws)]() mutable { run_session(env, std::move(w)); }).detach();
  }
}

