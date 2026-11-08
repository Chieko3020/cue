#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <stdexcept>
extern "C" {
#include <libavutil/frame.h>
}
#include "cue/media/processor.h"
#include "cue/media/h264_decoder.h"

using namespace cue::media;
namespace {
int failures = 0;
void check(bool ok, const char* what) { if (!ok) { std::fprintf(stderr, "FAIL %s\n", what); ++failures; } }
std::shared_ptr<AVFrame> frame(int width = 64, int height = 64, int format = AV_PIX_FMT_YUV420P) {
  auto f = std::shared_ptr<AVFrame>(av_frame_alloc(), [](AVFrame* f) { av_frame_free(&f); });
  if (!f) std::abort();
  f->width = width; f->height = height; f->format = format;
  if (av_frame_get_buffer(f.get(), 32) < 0) std::abort();
  for (int p = 0; p < 3; ++p)
    for (int y = 0; y < (p ? height / 2 : height); ++y)
      std::memset(f->data[p] + y * f->linesize[p], p ? 128 : 40, p ? width / 2 : width);
  return f;
}
class Paint final : public Processor {
 public:
  explicit Paint(int value) : value_(value) {}
  MediaKind input_kind() const override { return MediaKind::Raw; }
  MediaKind output_kind() const override { return MediaKind::Raw; }
  std::expected<ProcessorMedia, ProcessorError> process(ProcessorMedia m, ProcessorTiming&) override {
    auto f = std::shared_ptr<AVFrame>(av_frame_clone(m.frame()), [](AVFrame* f) { av_frame_free(&f); });
    if (!f || av_frame_make_writable(f.get()) < 0) std::abort();
    for (int y = 0; y < f->height; ++y) std::memset(f->data[0] + y * f->linesize[0], value_, f->width);
    m.owned_frame = std::move(f); m.borrowed_frame = nullptr;
    return m;
  }
 private: int value_;
};
class Fail final : public Processor {
 public:
  explicit Fail(EncodeError code) : code_(code) {}
  MediaKind input_kind() const override { return MediaKind::Raw; }
  MediaKind output_kind() const override { return MediaKind::Raw; }
  std::expected<ProcessorMedia, ProcessorError> process(ProcessorMedia, ProcessorTiming&) override {
    ProcessorError e; e.code = code_; e.message = "test failure"; return std::unexpected(e);
  }
 private: EncodeError code_;
};
class Throw final : public Processor {
 public:
  MediaKind input_kind() const override { return MediaKind::Raw; }
  MediaKind output_kind() const override { return MediaKind::Raw; }
  std::expected<ProcessorMedia, ProcessorError> process(ProcessorMedia, ProcessorTiming&) override {
    throw std::runtime_error("custom process error");
  }
};
void passthrough_and_registration() {
  auto r = ProcessorRegistry::builtins(); std::string err;
  check(!r.add("", {}), "empty registration rejected");
  check(!r.add("empty", {}), "empty factory rejected");
  check(!r.add("passthrough", [](const auto&) -> std::expected<std::unique_ptr<Processor>, std::string> {
    return std::make_unique<Paint>(1);
  }), "duplicate rejected");
  check(bool(r.add("null", [](const auto&) -> std::expected<std::unique_ptr<Processor>, std::string> {
    return std::unique_ptr<Processor>{};
  })), "register explicit null-result factory");
  check(bool(r.add("throw_factory", [](const auto&) -> std::expected<std::unique_ptr<Processor>, std::string> {
    throw std::runtime_error("custom factory error");
  })), "register throwing factory");
  auto thrown = r.create("throw_factory",{});
  check(!thrown && thrown.error().find("custom factory error") != std::string::npos,
        "factory exception converted into expected error");
  ProcessorChain c;
  check(!c.configure(r, {}, MediaKind::Raw), "empty graph rejected");
  auto bad = c.configure(r, {{"unknown", {}}}, MediaKind::Raw);
  check(!bad && bad.error().processor == "unknown" && bad.error().index == 0, "unknown located");
  check(!c.configure(r, {{"null", {}}}, MediaKind::Raw), "null factory result rejected");
  ProcessorOptions o; o.width=32; o.height=32;
  bad = c.configure(r, {{"transcode", o}, {"scale", o}}, MediaKind::Raw);
  check(!bad && bad.error().index == 1 && bad.error().processor == "scale", "invalid adjacency rejected");
  check(!c.process(ProcessorMedia::raw(nullptr, 1)), "failed configure leaves closed chain");
  check(bool(c.configure(r, {{"passthrough", {}}}, MediaKind::Encoded)), "passthrough configure");
  auto input = ProcessorMedia::encoded(std::vector<uint8_t>{1,0,3,255}, 7, 1234567, true);
  auto out = c.process(input);
  check(out && out->data == input.data && out->codec == 7 && out->pts == input.pts && out->keyframe,
        "passthrough exact bytes and metadata");
  check(c.stats().timing.encode_us == 0, "passthrough no encode sample");
  check(!c.process(ProcessorMedia::raw(nullptr,0)), "runtime wrong input kind rejected");
  c.close(); c.close();
  check(out && out->data == input.data, "encoded output survives close");
  check(c.stats().processed == 3 && c.stats().succeeded == 1 && c.stats().failed == 2,
        "attempts successes failures cumulative");
}
void scale_and_lifetime() {
  auto registry = ProcessorRegistry::builtins(); ProcessorChain c; ProcessorOptions o; o.width=32; o.height=48;
  check(bool(c.configure(registry, {{"scale", o}}, MediaKind::Raw)), "scale standalone configure");
  auto in = frame(); auto out = c.process(ProcessorMedia::raw(in.get(), 71));
  check(out && out->kind == MediaKind::Raw && out->owned_frame && !out->borrowed_frame, "scale owns output");
  if (!out) return;
  check(out->frame()->width == 32 && out->frame()->height == 48 && out->frame()->format == AV_PIX_FMT_YUV420P,
        "scale dimensions and pixel format");
  check(out->frame()->data[0][0] == 40 && out->frame()->data[1][0] == 128 && out->pts == 71,
        "scale actual pixels and PTS");
  std::memset(in->data[0], 99, 64);
  for (int i = 0; i < 20; ++i) check(bool(c.process(ProcessorMedia::raw(in.get(),i))), "repeated scale");
  in.reset(); c.close();
  check(out->frame()->data[0][0] == 40, "owned frame independent of source, repeat, close");
  o.width=33; check(!c.configure(registry, {{"scale",o}}, MediaKind::Raw), "odd output rejected");
  o.width=32; check(bool(c.configure(registry, {{"scale",o}}, MediaKind::Raw)), "scale reconfigure");
  ProcessorTiming t; auto invalid = c.process(ProcessorMedia::raw(nullptr,0), &t);
  check(!invalid && invalid.error().processor == "scale" && invalid.error().code == EncodeError::InvalidFrame,
        "invalid scale frame located");
  check(!invalid && invalid.error().timing.elapsed_us == t.elapsed_us && t.encode_us == 0,
        "error timing survives return");
  auto rgb = frame(64,64,AV_PIX_FMT_YUV444P);
  check(bool(c.process(ProcessorMedia::raw(rgb.get(),0))), "scale converts non-420 planar source");
}
void encoding_and_custom() {
  auto registry = ProcessorRegistry::builtins(); std::string err;
  for (int value : {80,160}) check(bool(registry.add("paint"+std::to_string(value),
      [value](const auto&) -> std::expected<std::unique_ptr<Processor>, std::string> {
        return std::make_unique<Paint>(value);
      })), "register custom raw processor");
  auto in = frame(); ProcessorOptions o; o.width=64; o.height=64; o.gop=1; o.preset="ultrafast";
  for (bool scaling : {false,true}) {
    ProcessorChain c; H264Decoder decoder; check(decoder.open(err), "decoder open");
    o.width = scaling ? 32 : 64; o.height = scaling ? 48 : 64;
    std::vector<ProcessorSpec> graph;
    if (scaling) graph.push_back({"scale",o});
    // Two custom operators prove configured execution order; the second wins.
    graph.push_back({"paint80",{}}); graph.push_back({"paint160",{}}); graph.push_back({"transcode",o});
    check(bool(c.configure(registry,graph,MediaKind::Raw)), "real composition configure");
    int decoded = 0;
    for (int i=0; i<25; ++i) {
      ProcessorTiming t; auto out = c.process(ProcessorMedia::raw(in.get(),1000+i), &t);
      if (!out) { check(out.error().code == EncodeError::EncoderDelay, "only normal encoder delay allowed"); continue; }
      check(out->kind == MediaKind::Encoded && out->pts == 1000+i && out->codec == 1 && !out->data.empty(),
            "transcode bytes codec PTS");
      auto* f = decoder.decode(out->data,err);
      check(err.empty(), "real h264 output decodes");
      if (f) { ++decoded; check(f->width == o.width && f->height == o.height, "decoded intended dimensions");
        check(std::abs(int(f->data[0][0])-160)<5, "custom raw order visible after real encode/decode"); }
      check(t.elapsed_us >= t.encode_us && t.op_us == t.elapsed_us-t.encode_us, "op/encode nonoverlap");
    }
    check(decoded > 0 && c.stats().succeeded > 0 && c.stats().failed == 0, "real frame outputs and stats");
    check(in->data[0][0]==40, "custom copy-on-write preserves borrowed source");
    c.close(); check(bool(c.configure(registry,graph,MediaKind::Raw)), "encoder reconfigure after repeats");
  }
  ProcessorChain c; o.width=32; o.height=32;
  check(bool(c.configure(registry,{{"transcode",o}},MediaKind::Raw)), "standalone transcode configure");
  auto bad = c.process(ProcessorMedia::raw(in.get(),1));
  check(!bad && bad.error().code == EncodeError::SizeMismatch && bad.error().processor == "transcode",
        "bare transcode never scales");
  o.width=64; o.height=64; check(bool(c.configure(registry,{{"transcode",o}},MediaKind::Raw)), "format test configure");
  auto other = frame(64,64,AV_PIX_FMT_YUV444P); bad=c.process(ProcessorMedia::raw(other.get(),1));
  check(!bad && bad.error().code == EncodeError::FormatMismatch, "bare transcode never converts format");
  for (auto code : {EncodeError::EncoderDelay, EncodeError::EncodeFailed}) {
    check(bool(registry.add(encode_error_name(code),[code](const auto&) -> std::expected<std::unique_ptr<Processor>, std::string> {
      return std::make_unique<Fail>(code);
    })), "register classification fixture");
    ProcessorChain failing;
    check(bool(failing.configure(registry,{{"scale",o},{encode_error_name(code),{}}},MediaKind::Raw)), "failure graph");
    ProcessorTiming t; bad=failing.process(ProcessorMedia::raw(in.get(),0), &t);
    check(!bad && bad.error().index==1 && bad.error().code==code && bad.error().timing.elapsed_us==t.elapsed_us,
          "downstream failure attributed with prior scale timing");
    check(failing.stats().processed==1 && failing.stats().delays==(code==EncodeError::EncoderDelay ? 1u:0u)
        && failing.stats().failed==(code==EncodeError::EncodeFailed ? 1u:0u), "delay not failure statistics");
  }
  // Changing only the source dimensions must not reopen x264 or force a new IDR.
  TranscodeChain stable;
  check(stable.configure(64,64,500,30,1000,"ultrafast",err), "stable adapter configure");
  auto resized = frame(32,48);
  bool observed_after_resize = false;
  for (int i=0; i<12; ++i) {
    auto out=stable.process(i<5 ? in.get() : resized.get(),i,err);
    if (!out) { check(out.error()==EncodeError::EncoderDelay,"stable delay classification"); continue; }
    if (i>=5) { observed_after_resize=true; check(!out->keyframe,"source resize preserves encoder reference state"); }
  }
  check(observed_after_resize,"source resize still outputs media");
  check(bool(registry.add("throw", [](const auto&) -> std::expected<std::unique_ptr<Processor>, std::string> {
    return std::make_unique<Throw>();
  })), "register throwing processor");
  ProcessorChain throwing;
  check(bool(throwing.configure(registry,{{"scale",o},{"throw",{}}},MediaKind::Raw)), "throw graph configure");
  ProcessorTiming thrown_timing;
  auto thrown = throwing.process(ProcessorMedia::raw(in.get(),0),&thrown_timing);
  check(!thrown && thrown.error().processor=="throw" && thrown.error().index==1 &&
        thrown.error().message.find("custom process error")!=std::string::npos,
        "process exception located and converted into expected error");
  check(throwing.stats().failed==1 && throwing.stats().processed==1 &&
        thrown.error().timing.elapsed_us==thrown_timing.elapsed_us &&
        throwing.stats().timing.elapsed_us==thrown_timing.elapsed_us,
        "process exception settles timing and failure stats");
  TranscodeChain legacy;
  check(legacy.configure(32,32,500,30,1,"ultrafast",err), "legacy configure delegates");
  H264Decoder decoder; check(decoder.open(err), "legacy decoder"); int decoded=0;
  for(int i=0;i<10;++i) { auto out=legacy.process(in.get(),i,err); if(out) {
    auto* f=decoder.decode(out->data,err); if(f) {++decoded;check(f->width==32 && f->height==32,"legacy explicit scale");}
  } else check(out.error()==EncodeError::EncoderDelay,"legacy only normal delay"); }
  check(decoded>0,"legacy real output"); legacy.close(); legacy.close();
  auto closed=legacy.process(in.get(),0,err);check(!closed && closed.error()==EncodeError::NotOpen,"legacy closed error");
}
}
int main(int argc, char** argv) {
  if (argc==5 && std::string(argv[1])=="--decode") {
    std::ifstream file(argv[2],std::ios::binary); H264Decoder decoder; std::string err;
    if (!file || !decoder.open(err)) return 2;
    int count=0;
    while (file.peek()!=std::char_traits<char>::eof()) {
      uint32_t size=0; file.read(reinterpret_cast<char*>(&size),sizeof(size));
      if (!file || size>1024*1024) return 3;
      std::vector<uint8_t> payload(size); file.read(reinterpret_cast<char*>(payload.data()),size);
      if (!file) return 4;
      auto* frame=decoder.decode(payload,err);
      if (!err.empty()) return 5;
      if (frame) { ++count; if(frame->width!=std::atoi(argv[3]) || frame->height!=std::atoi(argv[4])) return 6; }
    }
    std::printf("decoded=%d dimensions=%sx%s\n",count,argv[3],argv[4]); return count>0 ? 0:7;
  }
  passthrough_and_registration(); scale_and_lifetime(); encoding_and_custom();
  std::printf("processor: %d failures\n", failures); return failures ? 1 : 0; }
