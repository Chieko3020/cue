// Real x264/FFmpeg behavior, including valid PPS-only replacement. No mocked codec.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
extern "C" {
#include <x264.h>
#include <libavutil/frame.h>
}
#include "cue/media/h264_decoder.h"
namespace {
using Bytes = std::vector<uint8_t>;
int failures = 0;
void check(bool ok, const char* message) {
  if (!ok) { std::fprintf(stderr, "FAIL %s\n", message); ++failures; }
}
std::vector<Bytes> generate(int width, int height, int chroma = 0, bool full_chroma = false) {
  x264_param_t p{};
  if (x264_param_default_preset(&p, "ultrafast", "zerolatency") < 0) std::abort();
  p.i_width=width; p.i_height=height; p.i_csp=full_chroma?X264_CSP_I444:X264_CSP_I420;
  p.i_threads=1; p.i_fps_num=25; p.i_fps_den=1; p.i_keyint_max=5; p.i_keyint_min=1;
  p.i_scenecut_threshold=0; p.b_repeat_headers=1; p.b_annexb=1; p.i_bframe=0;
  p.analyse.i_chroma_qp_offset=chroma;
  p.i_log_level=X264_LOG_ERROR;
  x264_t* encoder=x264_encoder_open(&p);
  if (!encoder) std::abort();
  x264_picture_t in{}, out{};
  if (x264_picture_alloc(&in, p.i_csp,width,height) < 0) std::abort();
  std::vector<Bytes> result;
  for (int i=0;i<20;++i) {
    for (int plane=0;plane<3;++plane)
      for (int y=0;y<(plane && !full_chroma?height/2:height);++y)
        std::memset(in.img.plane[plane]+y*in.img.i_stride[plane],plane?128:40+i,plane && !full_chroma?width/2:width);
    in.i_pts=i;
    x264_nal_t* nals=nullptr; int count=0;
    if (x264_encoder_encode(encoder,&nals,&count,&in,&out)<=0) std::abort();
    Bytes bytes;
    for (int n=0;n<count;++n) bytes.insert(bytes.end(),nals[n].p_payload,nals[n].p_payload+nals[n].i_payload);
    result.push_back(std::move(bytes));
  }
  x264_picture_clean(&in); x264_encoder_close(encoder);
  return result;
}
std::vector<Bytes> nals(const Bytes& bytes) {
  std::vector<Bytes> out;
  for (size_t i=0;i+3<bytes.size();) {
    size_t sc=0;
    if (bytes[i]==0 && bytes[i+1]==0 && bytes[i+2]==1) sc=3;
    else if (i+4<bytes.size() && bytes[i]==0 && bytes[i+1]==0 && bytes[i+2]==0 && bytes[i+3]==1) sc=4;
    if (!sc) { ++i; continue; }
    size_t start=i+sc,end=start;
    while (end+3<bytes.size() && !(bytes[end]==0 && bytes[end+1]==0 &&
      (bytes[end+2]==1 || (bytes[end+2]==0 && bytes[end+3]==1)))) ++end;
    if (end+3>=bytes.size()) end=bytes.size();
    while (end>start && bytes[end-1]==0) --end;
    out.emplace_back(bytes.begin()+start,bytes.begin()+end); i=end;
  }
  return out;
}
Bytes select(const Bytes& bytes, bool params) {
  Bytes out;
  for (const auto& nal:nals(bytes)) if (((nal[0]&31)==7 || (nal[0]&31)==8)==params) {
    out.insert(out.end(),{0,0,0,1}); out.insert(out.end(),nal.begin(),nal.end());
  }
  return out;
}
void run() {
  auto a=generate(64,64), b=generate(96,80), c=generate(64,64,2);
  cue::media::H264Decoder dec; std::string err;
  check(dec.open(err),"open");
  for (const auto* segment:{&a,&b,&a}) {
    const int width=segment==&b?96:64, height=segment==&b?80:64;
    for (size_t i=0;i<segment->size();++i) {
      AVFrame* f=dec.decode((*segment)[i],err);
      check(f && f->width==width && f->height==height,"A-B-A actual dimensions");
      check(f && f->data[0] && f->data[0][0]>=36 && f->data[0][0]<=64,"actual luma");
      check(err.empty(),"healthy transition no error");
    }
  }
  check(dec.stats().frames==60 && dec.stats().output_changes==2,"exact frames and output changes");
  check(dec.stats().sps_changes==2 && dec.stats().recovery_rebuilds==0 && dec.stats().flushes==0,"repeat headers no reset");
  const auto old_sps=select(a[0],true), new_params=select(c[0],true);
  check(old_sps!=new_params,"encoder generated genuinely different parameters");
  const auto sps_before=dec.stats().sps_changes, pps_before=dec.stats().pps_changes;
  for (const auto& bytes:c) check(dec.decode(bytes,err)!=nullptr && err.empty(),"valid PPS stream");
  check(dec.stats().sps_changes==sps_before && dec.stats().pps_changes==pps_before+1,"PPS-only valid change");
  // Start code length and trailing_zero_8bits are not content changes.
  Bytes equivalent;
  for (const auto& nal:nals(new_params)) { equivalent.insert(equivalent.end(),{0,0,1}); equivalent.insert(equivalent.end(),nal.begin(),nal.end()); equivalent.insert(equivalent.end(),3,0); }
  const auto changes=dec.stats().pps_changes;
  dec.decode(equivalent,err);
  check(err.empty() && dec.stats().pps_changes==changes,"equivalent header padding no change");
  auto full=generate(64,64,0,true);
  const int old_format=dec.stats().output_format;
  const auto old_output_changes=dec.stats().output_changes;
  check(dec.decode(full[0],err)!=nullptr && err.empty() && dec.stats().output_format!=old_format &&
        dec.stats().output_changes==old_output_changes+1,"actual YUV444 output format change");
  check(dec.decode(c[0],err)!=nullptr && err.empty() && dec.stats().output_format==old_format,
        "actual YUV420 format restoration");
  dec.flush();
  check(dec.decode(c[1],err)==nullptr && err.empty() && dec.waiting_keyframe(),"non-IDR after flush gated");
  dec.close(); check(dec.open(err),"reopen");
  const auto params=select(b[0],true), picture=select(b[0],false);
  check(dec.decode(params,err)==nullptr && err.empty() && dec.waiting_keyframe(),"separate parameters wait without error");
  check(dec.decode(b[1],err)==nullptr && err.empty(),"parameters before delayed IDR gate P");
  auto* f=dec.decode(picture,err);
  check(f && f->width==96 && err.empty(),"separate parameters then IDR real output");
  const Bytes damaged_slice{0,0,1,0x65,0xff,0xff,0xff,0xff,0xff,0xff};
  const auto error_before = dec.stats().errors;
  dec.decode(damaged_slice,err);
  check(!err.empty() && dec.stats().errors > error_before,"bad VCL remains a real decode error with CHUNKS");
  // Repeated damage arms a single recovery, but never reopens on garbage.
  Bytes bad{0,0,1,0x67};
  for (int i=0;i<80;++i) check(!dec.decode(bad,err) && !err.empty(),"truncated header rejected");
  check(dec.stats().recovery_rebuilds==0,"bad packets do not rebuild");
  check(dec.decode(b[0],err)!=nullptr && err.empty(),"valid inline IDR recovers");
  check(dec.stats().recovery_rebuilds==1,"threshold has one shared recovery");
  Bytes oversized{0,0,1,0x68}; oversized.resize(2050,0x55);
  check(!dec.decode(oversized,err) && !err.empty(),"oversized bounded");
  for(int i=0;i<5;++i) {
    for(int n=0;n<130;++n) dec.decode(bad,err);
    check(dec.decode(b[0],err)!=nullptr,"valid IDR keeps decoding even after recovery budget");
  }
  check(dec.stats().recovery_rebuilds==3 && dec.stats().recovery_exhausted,"hard recovery limit");
  const auto rebuilds = dec.stats().recovery_rebuilds;
  for(int n=0;n<40;++n) dec.decode(bad,err);
  dec.close(); check(dec.open(err),"external reopen after damage");
  check(dec.stats().failure_streak==0 && dec.decode(a[0],err)!=nullptr && err.empty() &&
        dec.stats().recovery_rebuilds==rebuilds,"external reopen clears stale recovery trigger");
  std::printf("dynamic frames=%llu errors=%llu output_changes=%llu rebuilds=%llu\n",
    (unsigned long long)dec.stats().frames,(unsigned long long)dec.stats().errors,
    (unsigned long long)dec.stats().output_changes,(unsigned long long)dec.stats().recovery_rebuilds);
}
}
int main(int argc,char** argv) {
  if(argc==3 && std::string(argv[1])=="--generate") {
    std::filesystem::create_directories(argv[2]);
    for(auto dims:{std::pair{64,64},std::pair{96,80}}) {
      std::ofstream out(std::filesystem::path(argv[2])/(std::to_string(dims.first)+".frames"),std::ios::binary);
      for(const auto& bytes:generate(dims.first,dims.second)) {
        uint32_t size=bytes.size(); out.write(reinterpret_cast<const char*>(&size),4);out.write(reinterpret_cast<const char*>(bytes.data()),size);
      }
      if(!out) return 2;
    }
    return 0;
  }
  run(); return failures?1:0;
}
