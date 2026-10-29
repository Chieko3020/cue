// Real Opus packets, an explicit clock, and only the receiver's public PCM/statistics seam.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <string>
#include <vector>
#include <opus/opus.h>

#include "cue/media/audio_receiver.h"
#include "cue/media/opus_encoder.h"

namespace {
using cue::media::AudioOutputKind;
using cue::media::AudioReceiver;
int failures = 0;
void check(bool ok, const char* what) {
  if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}
using Packets = std::vector<std::vector<uint8_t>>;
Packets packets(bool silent = false) {
  cue::media::OpusAudioEncoder encoder;
  encoder.set_dtx(silent);
  std::string error;
  check(encoder.open(error), "real encoder opens");
  Packets all;
  std::vector<int16_t> pcm(960);
  for (int frame = 0; frame < 130; ++frame) {
    for (std::size_t i = 0; i < pcm.size(); ++i) {
      const double phase = double(frame * 960 + static_cast<int>(i)) / 48000.0;
      pcm[i] = silent ? 0 : static_cast<int16_t>(12000 * std::sin(2 * std::numbers::pi * 440 * phase));
    }
    auto encoded = encoder.encode(pcm.data(), pcm.size(), error);
    check(encoded.has_value(), "real packet encodes");
    if (encoded && frame >= 30) all.push_back(std::move(*encoded));
  }
  return all;
}
struct Output { uint32_t seq; AudioOutputKind kind; };
struct Sink {
  std::vector<Output> frames;
  AudioReceiver::Emit emit = [this](uint32_t seq, AudioOutputKind kind, std::span<const int16_t> pcm) {
    check(pcm.size() == 960, "each output is one 20ms PCM slot");
    frames.push_back({seq, kind});
  };
  void sequences(std::initializer_list<uint32_t> expected) const {
    std::vector<uint32_t> got;
    for (const auto& f : frames) got.push_back(f.seq);
    check(got == std::vector<uint32_t>(expected), "output sequence/time slots match");
  }
};
void ledger(const AudioReceiver& receiver) {
  const auto& s = receiver.stats();
  check(s.committed == s.decoded_frames + s.failed_frames + s.fec_recovered + s.plc_frames +
                           s.unconcealed, "committed slot ledger reconciles");
  check(s.pcm_samples == s.decoded_frames * 960, "Normal PCM count excludes repair frames");
  check(receiver.pending_packets() <= AudioReceiver::kMaxPending &&
        receiver.pending_bytes() <= AudioReceiver::kMaxPending * cue::media::kMaxOpusPacketBytes,
        "queue packet and byte bounds hold");
}

void normal_and_reorder(const Packets& p) {
  AudioReceiver r; Sink out;
  r.insert(0,p[0],0,out.emit); check(out.frames.size()==1,"continuous first packet immediate");
  r.insert(2,p[2],100,out.emit);
  check(r.next_deadline_us()==60'100 && out.frames.size()==1,"successor waits for gap deadline");
  r.insert(2,p[2],200,out.emit);
  r.insert(1,p[1],60'099,out.emit);
  r.insert(2,p[2],60'099,out.emit);
  out.sequences({0,1,2});
  check(r.stats().gaps==0 && r.stats().duplicates==2 && r.stats().reordered==1,
        "0,2,1 restores order without manufactured loss or duplicate output");
  check(r.next_deadline_us()==0,"continuous queue has no outstanding deadline"); ledger(r);

  AudioReceiver multi; Sink multiple;
  multi.insert(0,p[0],0,multiple.emit); multi.insert(3,p[3],100,multiple.emit);
  multi.insert(1,p[1],200,multiple.emit); multi.insert(2,p[2],300,multiple.emit);
  multiple.sequences({0,1,2,3}); check(multi.stats().gaps==0,"0,3,1,2 has no finalized loss"); ledger(multi);
}

void deadlines(const Packets& p) {
  AudioReceiver r(false,true); Sink out;
  r.insert(0,p[0],0,out.emit); r.insert(2,p[2],100,out.emit);
  r.advance(60'099,out.emit); check(out.frames.size()==1,"not before exact deadline");
  r.insert(1,p[1],60'100,out.emit); // advance occurs before considering this now-late packet.
  out.sequences({0,1,2}); check(out.frames[1].kind==AudioOutputKind::Plc,"missing slot is explicitly PLC");
  check(r.stats().decoded_frames==2 && r.stats().plc_frames==1 && r.stats().stale==1,
        "exact deadline late packet cannot replace its compensated slot");
  r.insert(1,p[1],60'101,out.emit); check(out.frames.size()==3,"later packet never replays committed slot");
  r.advance(10'000'000,out.emit); check(out.frames.size()==3,"silence cannot invent tail PLC"); ledger(r);

  AudioReceiver quiet(false,true); Sink q;
  quiet.insert(0,p[0],0,q.emit); quiet.insert(3,p[3],100,q.emit);
  quiet.insert(4,p[4],20'000,q.emit); quiet.insert(4,p[4],59'000,q.emit);
  check(quiet.next_deadline_us()==60'100,"future and duplicate packets cannot refresh deadline");
  quiet.advance(60'100,q.emit); q.sequences({0,1,2,3,4});
  check(quiet.stats().gaps==2 && quiet.stats().plc_frames==2,"no new packet needed to finalize gap"); ledger(quiet);
}

void switches(const Packets& p) {
  cue::media::OpusAudioDecoder probe; std::string error;
  check(probe.open(error) && probe.fec_payload_present(p[2]),"fixture has real LBRR redundancy");
  for (bool fec : {false,true}) for (bool plc : {false,true}) for(uint32_t gap=1;gap<=4;++gap) {
    AudioReceiver r(fec,plc); Sink out;
    r.insert(0,p[0],0,out.emit); r.insert(gap+1,p[gap+1],10,out.emit); r.advance(60'010,out.emit);
    const auto& s=r.stats();
    const uint64_t plc_expected=plc?std::min<uint32_t>(3,gap-(fec?1:0)):0;
    check(s.decoded_frames==2 && s.gaps==gap,"all option/gap combinations commit Normal packets once");
    check(s.fec_recovered==(fec?1u:0u),"FEC used only when enabled and actually present");
    check(s.plc_frames==plc_expected,"PLC obeys switch and shared three-slot budget");
    check(s.unconcealed==gap-s.fec_recovered-s.plc_frames,"every uncovered slot is accounted");
    check(out.frames.size()==2+s.fec_recovered+s.plc_frames,"output count equals actual unique slots");
    for(std::size_t i=1;i<out.frames.size();++i)
      check(out.frames[i].seq>out.frames[i-1].seq,"all option/gap output slots strictly advance");
    ledger(r);
  }
  AudioReceiver large; Sink out;
  large.insert(0,p[0],0,out.emit); large.insert(1000,p[1],1,out.emit); large.advance(60'001,out.emit);
  out.sequences({0,1000});
  check(large.stats().gaps==999 && large.stats().unconcealed==999 &&
        large.stats().fec_recovered==0 && large.stats().plc_frames==0 && large.stats().resyncs==1,
        "large gap resyncs rather than emitting a repair burst from wrong state"); ledger(large);
  // With PLC off, an earlier missing slot must cause a re-anchor before the last FEC slot.
  AudioReceiver short_gap(true,false); Sink short_out;
  short_gap.insert(0,p[0],0,short_out.emit); short_gap.insert(3,p[3],1,short_out.emit);
  short_gap.advance(60'001,short_out.emit);
  check(short_gap.stats().unconcealed==1 && short_gap.stats().resyncs>=1,
        "unadvanced earlier gap re-anchors decoder"); ledger(short_gap);
}

void bounds_wrap_reset(const Packets& p) {
  AudioReceiver r(false,false); Sink out;
  r.insert(0,p[0],0,out.emit);
  for(uint32_t i=2;i<=18;i+=2) {r.insert(i,p[i],1,out.emit);ledger(r);}
  check(r.stats().capacity_flushes==1,"capacity pressure makes bounded forward progress");
  r.advance(60'001,out.emit); ledger(r);
  for(std::size_t i=1;i<out.frames.size();++i)
    check(out.frames[i].seq>out.frames[i-1].seq,"capacity release never reverses output");

  // Actual valid Opus padding exercises the byte limit, not only a packet-count proxy.
  auto padded=p[0];const auto original=padded.size();padded.resize(1275);
  check(opus_packet_pad(padded.data(),static_cast<opus_int32>(original),1275)==OPUS_OK,
        "real packet padded to payload bound");
  AudioReceiver closest(false,false);Sink c;
  closest.insert(0,p[0],0,c.emit);
  for(uint32_t i=10;i<18;++i)closest.insert(i,padded,1,c.emit);
  check(closest.pending_bytes()==8*1275,"eight maximum-size packets occupy exact byte bound");
  closest.insert(2,p[2],1,c.emit); // A new packet is closer than all eight queued successors.
  check(closest.stats().capacity_flushes==1&&closest.pending_packets()==8,
        "closer incoming successor settles without a ninth queued packet");
  closest.advance(60'001,c.emit);c.sequences({0,2,10,11,12,13,14,15,16,17});ledger(closest);

  AudioReceiver wrap; Sink wrapped;
  const uint32_t last=std::numeric_limits<uint32_t>::max();
  wrap.insert(last-1,p[0],0,wrapped.emit);wrap.insert(0,p[2],1,wrapped.emit);
  wrap.insert(last,p[1],2,wrapped.emit);wrapped.sequences({last-1,last,0});
  wrap.insert(1u+0x80000000u,p[3],3,wrapped.emit);
  check(wrap.stats().stale==1 && wrap.pending_packets()==0,"half-space ambiguity rejected");ledger(wrap);

  AudioReceiver reset; Sink fresh;
  reset.insert(0,p[0],0,fresh.emit);reset.insert(2,p[2],1,fresh.emit);
  reset.reset_stream();fresh.frames.clear();reset.insert(0,p[3],2,fresh.emit);reset.advance(99'999,fresh.emit);
  fresh.sequences({0});check(reset.stats().decoded_frames==2&&reset.stats().gaps==0,
        "reset discards old pending state but preserves cumulative counters");ledger(reset);
}

void invalid_and_silence(const Packets& p,const Packets& silence) {
  AudioReceiver unanchored;Sink initial;
  unanchored.insert(900,{},0,initial.emit);unanchored.insert(0,p[0],1,initial.emit);
  initial.sequences({0});check(unanchored.stats().decode_errors==1&&
      unanchored.stats().committed==1,"invalid first packet cannot manufacture a starting prefix");
  check(!unanchored.first_error().empty(),"first error available before reset");
  unanchored.reset_stream();check(unanchored.first_error().empty()&&
      unanchored.stats().decode_errors==1,"reset clears error text while preserving counters");ledger(unanchored);
  AudioReceiver r; Sink out;
  r.insert(0,p[0],0,out.emit);r.insert(1,{},1,out.emit);r.insert(1,p[1],2,out.emit);
  r.insert(2,p[2],3,out.emit);std::vector<uint8_t> oversized(1276,0);
  r.insert(3,oversized,4,out.emit);r.insert(4,std::vector<uint8_t>{0xff},5,out.emit);
  out.sequences({0,2});check(r.stats().decode_errors==3&&r.stats().failed_frames==3&&
      r.stats().plc_frames==0&&r.stats().fec_recovered==0&&r.stats().gaps==0,
      "bad current slots are errors, never disguised as PLC or replayed");ledger(r);
  AudioReceiver pending; Sink none;
  pending.insert(0,p[0],0,none.emit);pending.insert(2,oversized,1,none.emit);
  check(pending.pending_bytes()==0,"oversized queued marker does not retain oversized data");
  pending.insert(1,p[1],2,none.emit);check(pending.stats().decode_errors==1,"queued error counted once");ledger(pending);

  AudioReceiver quiet; Sink q;bool dtx=false;
  for(std::size_t i=0;i<silence.size();++i){dtx|=silence[i].size()<=2;quiet.insert(static_cast<uint32_t>(i),silence[i],static_cast<int64_t>(i)*20000,q.emit);}
  quiet.advance(100'000'000,q.emit);
  check(dtx&&quiet.stats().decoded_frames==silence.size()&&quiet.stats().decode_errors==0&&
        quiet.stats().plc_frames==0&&quiet.stats().fec_recovered==0,"DTX/quiet packets do not invent loss");ledger(quiet);
  AudioReceiver bounded; Sink b;
  bounded.insert(0,p[0],0,b.emit); bounded.insert(5,silence.back(),1,b.emit);
  bounded.advance(60'001,b.emit);
  check(bounded.stats().gaps==4&&bounded.stats().plc_frames==3&&
        bounded.stats().fec_recovered==0&&bounded.stats().unconcealed==1&&
        bounded.stats().resyncs>=1,"FEC-unavailable fallback shares three-PLC budget for gap four");
  b.sequences({0,1,2,3,5});ledger(bounded);
}
}  // namespace

int main(){
  const auto speech=packets(), silence=packets(true);
  if(speech.size()<30||silence.empty())return 1;
  normal_and_reorder(speech);deadlines(speech);switches(speech);
  bounds_wrap_reset(speech);invalid_and_silence(speech,silence);
  std::printf("audio receiver failures=%d\n",failures);return failures?1:0;
}
