#include "cue/media/processor.h"
#include <algorithm>
#include <exception>
#include <utility>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}
#include "cue/common/clock.h"
#include "cue/proto/wire.h"

namespace cue::media {
ProcessorMedia ProcessorMedia::raw(const AVFrame* frame, int64_t pts) {
  ProcessorMedia m;
  m.kind = MediaKind::Raw; m.borrowed_frame = frame; m.pts = pts;
  return m;
}
ProcessorMedia ProcessorMedia::encoded(std::span<const uint8_t> data, uint8_t codec,
                                        int64_t pts, bool keyframe) {
  ProcessorMedia m;
  m.data.assign(data.begin(), data.end()); m.codec = codec; m.pts = pts; m.keyframe = keyframe;
  return m;
}
namespace {
ProcessorError error(EncodeError code, std::string message) {
  ProcessorError e; e.code = code; e.message = std::move(message); return e;
}
class Passthrough final : public Processor {
 public:
  MediaKind input_kind() const override { return MediaKind::Encoded; }
  MediaKind output_kind() const override { return MediaKind::Encoded; }
  std::expected<ProcessorMedia, ProcessorError> process(ProcessorMedia m, ProcessorTiming&) override {
    return m;
  }
};
class Scale final : public Processor {
 public:
  Scale(int w, int h) : w_(w), h_(h) {}
  ~Scale() override { if (sws_) sws_freeContext(sws_); }
  MediaKind input_kind() const override { return MediaKind::Raw; }
  MediaKind output_kind() const override { return MediaKind::Raw; }
  std::expected<ProcessorMedia, ProcessorError> process(ProcessorMedia m, ProcessorTiming&) override {
    const AVFrame* in = m.frame();
    if (!in || in->width <= 0 || in->height <= 0 || !in->data[0] ||
        !sws_isSupportedInput(static_cast<AVPixelFormat>(in->format)))
      return std::unexpected(error(EncodeError::InvalidFrame, "invalid raw frame"));
    int minimum[4]{};
    if (av_image_check_size(in->width, in->height, 0, nullptr) < 0 ||
        av_image_fill_linesizes(minimum, static_cast<AVPixelFormat>(in->format), in->width) < 0)
      return std::unexpected(error(EncodeError::InvalidFrame, "invalid raw dimensions"));
    for (int p = 0; p < 4; ++p) {
      if (minimum[p] && (!in->data[p] ||
          std::abs(static_cast<int64_t>(in->linesize[p])) < minimum[p]))
        return std::unexpected(error(EncodeError::InvalidFrame, "missing raw plane or invalid stride"));
    }
    if (in->width == w_ && in->height == h_ && in->format == AV_PIX_FMT_YUV420P) {
      AVFrame* ref = av_frame_clone(in);
      if (!ref) return std::unexpected(error(EncodeError::ScaleFailed, "av_frame_clone failed"));
      ref->pts = m.pts;
      m.owned_frame = std::shared_ptr<const AVFrame>(ref, [](const AVFrame* f) {
        auto* mutable_frame = const_cast<AVFrame*>(f); av_frame_free(&mutable_frame);
      });
      m.borrowed_frame = nullptr;
      return m;
    }
    sws_ = sws_getCachedContext(sws_, in->width, in->height, static_cast<AVPixelFormat>(in->format),
                               w_, h_, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_) return std::unexpected(error(EncodeError::ScaleFailed, "sws_getCachedContext failed"));
    AVFrame* dst = av_frame_alloc();
    if (!dst) return std::unexpected(error(EncodeError::ScaleFailed, "av_frame_alloc failed"));
    std::shared_ptr<const AVFrame> owned(dst, [](const AVFrame* f) {
      auto* mutable_frame = const_cast<AVFrame*>(f); av_frame_free(&mutable_frame);
    });
    dst->format = AV_PIX_FMT_YUV420P; dst->width = w_; dst->height = h_; dst->pts = m.pts;
    if (av_frame_get_buffer(dst, 32) < 0 ||
        sws_scale(sws_, in->data, in->linesize, 0, in->height, dst->data, dst->linesize) != h_)
      return std::unexpected(error(EncodeError::ScaleFailed, "scale output failed"));
    m.borrowed_frame = nullptr; m.owned_frame = std::move(owned);
    return m;
  }
 private:
  int w_, h_;
  SwsContext* sws_ = nullptr;
};
class Transcode final : public Processor {
 public:
  bool open(const ProcessorOptions& o, std::string& err) {
    return encoder_.open(o.width, o.height, o.bitrate_kbps, o.fps, o.gop, o.preset, err, o.threads);
  }
  MediaKind input_kind() const override { return MediaKind::Raw; }
  MediaKind output_kind() const override { return MediaKind::Encoded; }
  std::expected<ProcessorMedia, ProcessorError> process(ProcessorMedia m, ProcessorTiming& timing) override {
    const auto* frame = m.frame();
    if (frame && frame->format == AV_PIX_FMT_YUV420P) {
      for (int p = 0; p < 3; ++p)
        if (!frame->data[p] || frame->linesize[p] < (p ? frame->width / 2 : frame->width))
          return std::unexpected(error(EncodeError::InvalidFrame, "missing raw plane or invalid stride"));
    }
    std::string err; int64_t elapsed = 0;
    auto result = encoder_.encode(frame, m.pts, err, &elapsed);
    timing.encode_us += elapsed;
    if (!result) return std::unexpected(error(result.error(), std::move(err)));
    ProcessorMedia out;
    out.data = std::move(result->data); out.pts = result->pts; out.keyframe = result->keyframe;
    out.codec = static_cast<uint8_t>(cue::proto::Codec::H264);
    return out;
  }
 private:
  X264Encoder encoder_;
};
}  // namespace
std::expected<void, std::string> ProcessorRegistry::add(std::string name, Factory factory) {
  if (name.empty() || !factory) return std::unexpected("empty processor name/factory");
  if (!factories_.emplace(std::move(name), std::move(factory)).second) {
    return std::unexpected("duplicate processor registration");
  }
  return {};
}
std::expected<std::unique_ptr<Processor>, std::string> ProcessorRegistry::create(
    std::string_view name, const ProcessorOptions& options) const {
  auto it = factories_.find(std::string(name));
  if (it == factories_.end()) return std::unexpected("unknown processor: " + std::string(name));
  try {
    auto result = it->second(options);
    if (result && !*result) return std::unexpected("factory returned null processor");
    return result;
  } catch (const std::exception& e) {
    return std::unexpected(std::string("processor factory threw: ") + e.what());
  }
}
ProcessorRegistry ProcessorRegistry::builtins() {
  ProcessorRegistry r;
  r.add("passthrough", [](const ProcessorOptions&) -> std::expected<std::unique_ptr<Processor>, std::string> {
    return std::make_unique<Passthrough>();
  });
  r.add("scale", [](const ProcessorOptions& o) -> std::expected<std::unique_ptr<Processor>, std::string> {
    if (o.width <= 0 || o.height <= 0 || o.width % 2 || o.height % 2)
      return std::unexpected("scale dimensions must be positive and even");
    return std::make_unique<Scale>(o.width, o.height);
  });
  r.add("transcode", [](const ProcessorOptions& o) -> std::expected<std::unique_ptr<Processor>, std::string> {
    auto p = std::make_unique<Transcode>(); std::string err;
    if (!p->open(o, err)) return std::unexpected(err);
    return p;
  });
  return r;
}
std::expected<void, ProcessorError> ProcessorChain::configure(const ProcessorRegistry& registry,
    const std::vector<ProcessorSpec>& specs, MediaKind input) {
  close(); input_ = input;
  if (specs.empty()) return std::unexpected(error(EncodeError::NotOpen, "empty processor chain"));
  MediaKind kind = input;
  for (size_t i = 0; i < specs.size(); ++i) {
    auto p = registry.create(specs[i].name, specs[i].options);
    if (!p || (*p)->input_kind() != kind) {
      auto e = error(EncodeError::InvalidFrame, p ? "incompatible media kind" : p.error());
      e.processor = specs[i].name; e.index = i; close(); return std::unexpected(std::move(e));
    }
    kind = (*p)->output_kind(); names_.push_back(specs[i].name);
    processors_.push_back(std::move(*p));
  }
  return {};
}
void ProcessorChain::close() { processors_.clear(); names_.clear(); }
std::expected<ProcessorMedia, ProcessorError> ProcessorChain::process(ProcessorMedia input,
                                                                    ProcessorTiming* timing) {
  const auto start = cue::now_us(); ProcessorTiming measured;
  ++stats_.processed;
  auto finish = [&] {
    measured.elapsed_us = cue::now_us() - start;
    measured.op_us = std::max<int64_t>(0, measured.elapsed_us - measured.encode_us);
    stats_.timing.elapsed_us += measured.elapsed_us;
    stats_.timing.op_us += measured.op_us; stats_.timing.encode_us += measured.encode_us;
    if (timing) *timing = measured;
  };
  auto fail = [&](ProcessorError e) -> std::expected<ProcessorMedia, ProcessorError> {
    finish(); e.timing = measured;
    if (e.code == EncodeError::EncoderDelay) ++stats_.delays; else ++stats_.failed;
    return std::unexpected(std::move(e));
  };
  if (processors_.empty()) return fail(error(EncodeError::NotOpen, "processor chain not configured"));
  if (input.kind != input_) return fail(error(EncodeError::InvalidFrame, "chain input media kind mismatch"));
  for (size_t i = 0; i < processors_.size(); ++i) {
    auto result = [&]() -> std::expected<ProcessorMedia, ProcessorError> {
      try {
        return processors_[i]->process(std::move(input), measured);
      } catch (const std::exception& e) {
        return std::unexpected(error(EncodeError::EncodeFailed,
                                     std::string("processor threw: ") + e.what()));
      }
    }();
    if (!result) {
      auto e = std::move(result.error()); e.processor = names_[i]; e.index = i; return fail(std::move(e));
    }
    if (result->kind != processors_[i]->output_kind()) {
      auto e = error(EncodeError::InvalidFrame, "processor returned incorrect media kind");
      e.processor = names_[i]; e.index = i; return fail(std::move(e));
    }
    input = std::move(*result);
  }
  finish(); ++stats_.succeeded;
  return input;
}
}  // namespace cue::media
