#include <cstdint>
#include <cstring>
#include <string>

// x264.h 要求先包含 stdint（M0 实测踩过：否则 uint8_t 未定义）
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <x264.h>
}

#include "cue/common/clock.h"
#include "cue/media/encoder.h"
#include "cue/media/processor.h"

namespace cue::media {

namespace {

// 用析构写回耗时：所有 return 分支（含无输出/失败）都能带上已发生的编码耗时
class ElapsedWrite {
 public:
  explicit ElapsedWrite(int64_t* out) : out_(out), t0_(cue::now_us()) {
    if (out_) *out_ = 0;
  }
  ~ElapsedWrite() {
    if (out_) *out_ = cue::now_us() - t0_;
  }
  ElapsedWrite(const ElapsedWrite&) = delete;
  ElapsedWrite& operator=(const ElapsedWrite&) = delete;

 private:
  int64_t* out_;
  int64_t t0_;
};

}  // namespace

// ---------------------------------------------------------------- X264Encoder

struct X264Encoder::Impl {
  x264_t* enc = nullptr;
  x264_param_t param{};
  x264_picture_t pic_in{};
  x264_picture_t pic_out{};
  int w = 0;
  int h = 0;
  int64_t frame_count = 0;
  bool pic_in_alloced = false;  // #32：close() 只在真的 alloc 过时才 clean
};

X264Encoder::X264Encoder() : impl_(std::make_unique<Impl>()) {}
X264Encoder::~X264Encoder() { close(); }

bool X264Encoder::is_open() const { return impl_ && impl_->enc != nullptr; }

bool X264Encoder::open(int w, int h, int bitrate_kbps, int fps, int gop, std::string_view preset,
                       std::string& err, int threads, int level_idc) {
  close();
  if (w <= 0 || h <= 0 || w % 2 || h % 2) {
    err = "invalid dimensions (must be positive and even)";
    return false;
  }
  auto& im = *impl_;
  im.w = w;
  im.h = h;
  im.frame_count = 0;

  std::string preset_s(preset.empty() ? "veryfast" : std::string(preset));
  // zerolatency：关闭 B 帧与 lookahead，专为实时链路
  if (x264_param_default_preset(&im.param, preset_s.c_str(), "zerolatency") < 0) {
    err = "x264_param_default_preset failed for preset=" + preset_s;
    return false;
  }
  im.param.i_csp = X264_CSP_I420;
  im.param.i_width = w;
  im.param.i_height = h;
  im.param.i_fps_num = fps > 0 ? fps : 30;
  im.param.i_fps_den = 1;
  im.param.rc.i_bitrate = bitrate_kbps > 0 ? bitrate_kbps : 1500;
  im.param.rc.i_rc_method = X264_RC_ABR;
  im.param.i_keyint_max = gop > 0 ? gop : 60;
  im.param.i_keyint_min = 1;
  im.param.b_annexb = 1;  // 输出 Annex-B，与上行协议一致
  // 0 = 让 x264 按核数自动决定。这里原来是写死的 1，注释理由是「2 vCPU 留核给
  // 解码与网络」——但在 1080p 上实测单线程 encode p50 29.5 ms，帧预算 33 ms 只剩
  // 10%，采集端直接掉到 28 fps；而整条链路总共只占 9% CPU，留核留在了一个不需要
  // 的地方。低延迟不靠少开线程：tune=zerolatency 会按 slice 并行编，不引入帧级排队。
  im.param.i_threads = threads > 0 ? threads : 0;
  im.param.b_vfr_input = 0;
  im.param.rc.i_lookahead = 0;
  im.param.i_bframe = 0;
  // 下面两条把「丢包后能自愈」和「GOP 可预测」从"依赖 x264 默认值"变成契约。
  // b_repeat_headers：每个 IDR 前重发 SPS/PPS。libx264 的默认值确实是 1，但接收端
  //   一旦错过参数集就**永久**解不出画面（实测 probe 每帧 AVERROR_INVALIDDATA），
  //   这种关键能力不该建立在第三方库的默认值上——库升级改默认值，故障是静默的。
  // i_scenecut_threshold=0：不关的话关键帧由场景切换额外插入，-g 只是上限，
  //   probe 侧按"关键帧间隔"做的对照（恢复时间、GOP 抖动）就不可复现。
  im.param.b_repeat_headers = 1;
  im.param.i_scenecut_threshold = 0;
  // VBV（1 s 缓冲）：ABR 不禁突发，与「低延迟 + 10 Mbps 令牌桶」的实时语义不符。
  // 开了 VBV 之后码率在 1 s 窗口内可控，下游令牌桶才有意义（review #33）。
  const int kbps = im.param.rc.i_bitrate;
  im.param.rc.i_vbv_max_bitrate = kbps;
  im.param.rc.i_vbv_buffer_size = kbps;  // 单位 kbit → 1 s 缓冲
  im.param.rc.f_vbv_buffer_init = 0.5;   // 初始只预灌半缓冲：低延迟优先

  if (x264_param_apply_profile(&im.param, "high") < 0) {
    err = "x264_param_apply_profile(high) failed";
    return false;
  }
  // level 必须在 apply_profile 之后设：apply_profile 会按 profile 限制参数并对部分字段
  // 做归一化，先写 i_level_idc 会被它覆盖。0 保持 x264 的自动推导（cued 的口径）。
  if (level_idc > 0) im.param.i_level_idc = level_idc;
  im.enc = x264_encoder_open(&im.param);
  if (!im.enc) {
    err = "x264_encoder_open failed (preset=" + preset_s + ")";
    return false;
  }
  if (x264_picture_alloc(&im.pic_in, X264_CSP_I420, w, h) < 0) {
    err = "x264_picture_alloc failed";
    close();
    return false;
  }
  im.pic_in_alloced = true;
  return true;
}

void X264Encoder::close() {
  if (!impl_) return;
  auto& im = *impl_;
  if (im.enc) {
    x264_encoder_close(im.enc);
    im.enc = nullptr;
  }
  if (im.pic_in_alloced) {  // 两次 close() 也安全，不依赖 x264_free(nullptr) 的实现细节
    x264_picture_clean(&im.pic_in);
    im.pic_in_alloced = false;
  }
  im.pic_in = x264_picture_t{};
}

std::expected<EncodedResult, EncodeError> X264Encoder::encode(const AVFrame* frame, int64_t pts,
                                                             std::string& err, int64_t* encode_ns) {
  ElapsedWrite elapsed(encode_ns);
  if (!is_open()) {
    err = "encoder not open";
    return std::unexpected(EncodeError::NotOpen);
  }
  if (!frame) {
    err = "null frame";
    return std::unexpected(EncodeError::InvalidFrame);
  }
  auto& im = *impl_;
  if (frame->width != im.w || frame->height != im.h) {
    err = "frame size mismatch: got " + std::to_string(frame->width) + "x" +
          std::to_string(frame->height) + ", encoder is " + std::to_string(im.w) + "x" +
          std::to_string(im.h);
    return std::unexpected(EncodeError::SizeMismatch);
  }
  if (frame->format != AV_PIX_FMT_YUV420P) {
    err = "frame format is not yuv420p (" + std::to_string(frame->format) + ")";
    return std::unexpected(EncodeError::FormatMismatch);
  }

  for (int p = 0; p < 3; ++p) {
    const int plane_h = (p == 0) ? im.h : im.h / 2;
    const int plane_w = (p == 0) ? im.w : im.w / 2;
    for (int y = 0; y < plane_h; ++y) {
      std::memcpy(im.pic_in.img.plane[p] + static_cast<size_t>(y) * im.pic_in.img.i_stride[p],
                  frame->data[p] + static_cast<size_t>(y) * frame->linesize[p],
                  static_cast<size_t>(plane_w));
    }
  }
  im.pic_in.i_pts = im.frame_count++;
  // 强制 IDR：接收端丢帧后主动请求（见 X264Encoder::force_keyframe）。
  // 必须显式清掉 i_type，否则下一帧会继续被标成 keyframe —— x264 只在
  // 调用方把 i_type 改回 X264_TYPE_AUTO 后才恢复按 GOP/scenecut 自动决定。
  if (force_keyframe_) {
    im.pic_in.i_type = X264_TYPE_KEYFRAME;
    force_keyframe_ = false;
  } else {
    im.pic_in.i_type = X264_TYPE_AUTO;
  }

  x264_nal_t* nals = nullptr;
  int i_nals = 0;
  const int size = x264_encoder_encode(im.enc, &nals, &i_nals, &im.pic_in, &im.pic_out);
  if (size < 0) {
    err = "x264_encoder_encode failed: " + std::to_string(size);
    return std::unexpected(EncodeError::EncodeFailed);
  }
  // 编码器内部延迟：此刻没有可发数据，是正常路径而不是配置错误
  if (size == 0 || i_nals <= 0) return std::unexpected(EncodeError::EncoderDelay);

  EncodedResult r;
  r.keyframe = im.pic_out.b_keyframe != 0;
  r.pts = pts;
  r.data.reserve(static_cast<size_t>(size));
  for (int i = 0; i < i_nals; ++i) {
    r.data.insert(r.data.end(), nals[i].p_payload, nals[i].p_payload + nals[i].i_payload);
  }
  return r;
}

// -------------------------------------------------------------- TranscodeChain

TranscodeChain::TranscodeChain() : chain_(std::make_unique<ProcessorChain>()) {}
TranscodeChain::~TranscodeChain() = default;

bool TranscodeChain::configure(int dst_w, int dst_h, int bitrate_kbps, int fps, int gop,
                               std::string_view preset, std::string& err, int threads) {
  close();
  dst_w_ = dst_w; dst_h_ = dst_h;
  ProcessorOptions options{dst_w, dst_h, bitrate_kbps, fps, gop, threads, std::string(preset)};
  auto configured = chain_->configure(ProcessorRegistry::builtins(),
      {{"scale", options}, {"transcode", options}}, MediaKind::Raw);
  if (!configured) { err = configured.error().message; return false; }
  enabled_ = true;
  return true;
}
void TranscodeChain::close() {
  enabled_ = false;
  chain_->close();
}
std::expected<EncodedResult, EncodeError> TranscodeChain::process(const AVFrame* in, int64_t pts,
                                                                std::string& err, int64_t* encode_ns) {
  if (encode_ns) *encode_ns = 0;
  if (!enabled_) { err = "transcode chain not configured"; return std::unexpected(EncodeError::NotOpen); }
  if (!in) { err = "null frame"; return std::unexpected(EncodeError::InvalidFrame); }
  // The compatibility graph is fixed for this configuration. Scale has a same-size
  // YUV420P fast path, so a source format/size change never silently restarts x264.
  ProcessorTiming timing;
  auto result = chain_->process(ProcessorMedia::raw(in, pts), &timing);
  if (encode_ns) *encode_ns = timing.encode_us;
  if (!result) {
    err = result.error().processor + ": " + result.error().message;
    return std::unexpected(result.error().code);
  }
  return EncodedResult{std::move(result->data), result->keyframe, result->pts};
}

}  // namespace cue::media
