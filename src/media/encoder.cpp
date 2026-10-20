#include <cstdint>
#include <cstring>
#include <string>

// x264.h 要求先包含 stdint（M0 实测踩过：否则 uint8_t 未定义）
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <x264.h>
}

#include "cue/common/clock.h"
#include "cue/media/encoder.h"

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
                       std::string& err, int threads) {
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
  im.param.i_threads = threads > 0 ? threads : 1;  // 2 vCPU 默认 1：给解码与网络留核
  im.param.b_vfr_input = 0;
  im.param.rc.i_lookahead = 0;
  im.param.i_bframe = 0;
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

struct TranscodeChain::Scaler {
  SwsContext* sws = nullptr;
  AVFrame* dst = nullptr;
  int src_w = 0, src_h = 0, dst_w = 0, dst_h = 0;
  int src_fmt = -1;

  void reset() {
    if (dst) av_frame_free(&dst);
    if (sws) sws_freeContext(sws);
    sws = nullptr;
    dst = nullptr;
    src_w = src_h = dst_w = dst_h = 0;
    src_fmt = -1;
  }

  bool ensure(int sw, int sh, int sf, int dw, int dh, std::string& err) {
    if (sws && src_w == sw && src_h == sh && src_fmt == sf && dst_w == dw && dst_h == dh) return true;
    reset();
    sws = sws_getContext(sw, sh, static_cast<AVPixelFormat>(sf), dw, dh, AV_PIX_FMT_YUV420P,
                         SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) {
      err = "sws_getContext failed";
      return false;
    }
    dst = av_frame_alloc();
    if (!dst) {
      err = "av_frame_alloc failed";
      reset();
      return false;
    }
    dst->format = AV_PIX_FMT_YUV420P;
    dst->width = dw;
    dst->height = dh;
    if (av_frame_get_buffer(dst, 32) < 0) {
      err = "av_frame_get_buffer failed";
      reset();
      return false;
    }
    src_w = sw;
    src_h = sh;
    dst_w = dw;
    dst_h = dh;
    src_fmt = sf;
    return true;
  }
};

TranscodeChain::TranscodeChain() = default;
TranscodeChain::~TranscodeChain() = default;

bool TranscodeChain::configure(int dst_w, int dst_h, int bitrate_kbps, int fps, int gop,
                               std::string_view preset, std::string& err, int threads) {
  close();
  if (!scaler_) scaler_ = std::make_unique<Scaler>();
  dst_w_ = dst_w;
  dst_h_ = dst_h;
  if (!encoder_.open(dst_w, dst_h, bitrate_kbps, fps, gop, preset, err, threads)) return false;
  enabled_ = true;
  return true;
}

void TranscodeChain::close() {
  enabled_ = false;
  encoder_.close();
  if (scaler_) scaler_->reset();
}

std::expected<EncodedResult, EncodeError> TranscodeChain::process(const AVFrame* in, int64_t pts,
                                                                 std::string& err, int64_t* encode_ns) {
  if (encode_ns) *encode_ns = 0;
  if (!enabled_) {
    err = "transcode chain not configured";
    return std::unexpected(EncodeError::NotOpen);
  }
  if (!in) {
    err = "null frame";
    return std::unexpected(EncodeError::InvalidFrame);
  }
  if (!scaler_) scaler_ = std::make_unique<Scaler>();

  const AVFrame* src = in;
  if (in->width != dst_w_ || in->height != dst_h_ || in->format != AV_PIX_FMT_YUV420P) {
    if (!scaler_->ensure(in->width, in->height, in->format, dst_w_, dst_h_, err)) {
      return std::unexpected(EncodeError::ScaleFailed);
    }
    if (sws_scale(scaler_->sws, in->data, in->linesize, 0, in->height, scaler_->dst->data,
                  scaler_->dst->linesize) <= 0) {
      err = "sws_scale failed";
      return std::unexpected(EncodeError::ScaleFailed);
    }
    src = scaler_->dst;
  }
  return encoder_.encode(src, pts, err, encode_ns);
}

}  // namespace cue::media
