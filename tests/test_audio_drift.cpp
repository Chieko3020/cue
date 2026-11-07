#include "audio_drift.h"
#include "probe_render.h"
#include <algorithm>
#include <chrono>
#include <charconv>
#include <string_view>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numbers>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>
namespace {
int failures=0;
void check(bool yes,const char* text){if(!yes){++failures;std::fprintf(stderr,"FAIL: %s\n",text);}}
void library_quality(){
  for(int sign:{-1,1}){
    cue::media::AudioResampler r;cue::media::ResampleSpec spec{48000,1,48000,1,true};std::string error;
    check(r.open(spec,error),"force same-rate filter initialized");
    check(r.compensate(sign*48,48000,error).has_value(),"official soft compensation accepts both directions");
    std::vector<int16_t> all;int64_t n=0;
    for(int block=0;block<50;++block){int16_t in[960];for(auto& sample:in)sample=static_cast<int16_t>(8000*std::sin(2*std::numbers::pi*440*double(n++)/48000));
      check(r.convert(in,960,all,error).has_value(),"convert phase-continuous sine chunks");}
    check(r.flush(all,error).has_value(),"flush filter tail");
    check(sign>0?all.size()>48000:all.size()<48000,"positive delta produces more samples, negative fewer");
    int peak=0,jump=0;double energy=0;for(std::size_t i=64;i+64<all.size();++i){peak=std::max(peak,std::abs(int(all[i])));energy+=double(all[i])*all[i];jump=std::max(jump,std::abs(int(all[i])-int(all[i-1])));}
    const double rms=std::sqrt(energy/double(all.size()-128));
    check(peak<9000&&peak>7800&&rms>5400&&rms<5900&&jump<1000,"low sine no clipping/gain error/abnormal inter-chunk jump");
    check(r.open(spec,error),"reset filter");all.clear();int16_t silence[960]{};
    for(int i=0;i<50;++i)check(r.convert(silence,960,all,error).has_value(),"silent convert");
    check(r.flush(all,error).has_value(),"silent flush");
    check(std::all_of(all.begin(),all.end(),[](int16_t v){return v==0;}),"reset silence has no old tone/filter residue");
  }
}
void changing_compensation_quality(){
  cue::media::AudioResampler r;cue::media::ResampleSpec spec{48000,1,48000,1,true};std::string error;
  check(r.open(spec,error),"dynamic same-rate filter opens");std::vector<int16_t> all;int64_t n=0;
  for(int block=0;block<200;++block){
    if(block%25==0){const int values[]={48,0,-48,0};check(r.compensate(values[(block/25)%4],48000,error).has_value(),"live compensation updates without reopening");}
    int16_t pcm[960];for(auto& v:pcm)v=static_cast<int16_t>(8000*std::sin(2*std::numbers::pi*440*double(n++)/48000));
    check(r.convert(pcm,960,all,error).has_value(),"dynamic sine conversion");
  }
  check(r.flush(all,error).has_value(),"dynamic sine flush");int jump=0,peak=0;
  for(std::size_t i=64;i+64<all.size();++i){jump=std::max(jump,std::abs(int(all[i])-int(all[i-1])));peak=std::max(peak,std::abs(int(all[i])));}
  check(peak<9000&&peak>7800&&jump<1000,"switch +/zero/- compensation keeps sine gain/continuity");
}
nlohmann::json simulate(int ppm,bool enabled){
  cue::probe::AudioDrift drift;check(drift.configure(48000,1,250,enabled),"playback compensation opens");
  double depth=12000,minimum=depth,maximum=depth;uint64_t empty=0;int16_t pcm[960]{};std::vector<int16_t> out;
  // 1800 seconds of media; virtual consumer has specified rate offset. All
  // PCM traverses the real FFmpeg resampler. This is explicitly accelerated.
  for(int i=0;i<90000;++i){
    depth-=960.0*(1.0+double(ppm)/1e6);if(depth<0){++empty;depth=0;}
    drift.update(static_cast<int>(depth/48.0),int64_t(i)*20000+1,true);
    check(drift.convert(pcm,960,out),"simulated clock PCM converted");depth+=double(out.size());
    minimum=std::min(minimum,depth);maximum=std::max(maximum,depth);
  }
  const auto& s=drift.stats();
  if(enabled){check(empty==0&&minimum/48>150&&maximum/48<350,"30min +/-500ppm queue bounded without depletion");check(s.errors==0&&s.updates>1700,"actual compensation updates succeed");}
  return {{"consumer_ppm",ppm},{"enabled",enabled},{"media_seconds",1800},{"accelerated",true},{"minimum_ms",minimum/48},{"maximum_ms",maximum/48},{"final_ms",depth/48},{"empty_ticks",empty},{"input_samples",s.input_samples},{"output_samples",s.output_samples},{"updates",s.updates},{"last_ppm",s.ppm},{"errors",s.errors}};
}
void wallclock(int seconds,const std::string& path){
  using clock=std::chrono::steady_clock;
  cue::probe::Renderer r;cue::probe::RenderConfig cfg;cfg.audio_prebuffer_ms=250;std::string error;
  check(r.open(cfg,error)&&r.queued_ms()>=0,"actual SDL audio device opened for wallclock run");
  if(r.queued_ms()<0)return;
  const auto start=clock::now();uint64_t input=0;int peak_queue=0;std::vector<int> depths;
  std::ofstream log(path);log<<"{\"kind\":\"start\",\"seconds\":"<<seconds<<",\"scope\":\"real wallclock Renderer SDL dummy; source +/-500ppm relative to observed SDL consumed samples, not independent hardware clocks\"}"<<std::endl;
  int last_report=-1;
  uint64_t previous_consumed=0;
  double desired_source_samples=12000; // Constant initial 250ms credit, no queue feedback.

  while(true){const auto now=clock::now();const double elapsed=std::chrono::duration<double>(now-start).count();if(elapsed>=seconds)break;
    r.sample_audio();
    const auto counters=r.audio_stats();
    const int queued_samples=r.queued_samples();
    const uint64_t consumed=counters.output_samples-static_cast<uint64_t>(std::max(0,queued_samples));
    const int phase=std::min(2,static_cast<int>(elapsed/(double(seconds)/3)));
    const double source_ppm=phase==1?500.0:phase==2?-500.0:0.0;
    desired_source_samples+=double(consumed-previous_consumed)*(1+source_ppm/1e6);
    previous_consumed=consumed;
    if(double(input)<desired_source_samples){int16_t pcm[960];const bool silent=(static_cast<int>(elapsed/20)%3)==1;
      for(int i=0;i<960;++i)pcm[i]=silent?0:static_cast<int16_t>(6000*std::sin(2*std::numbers::pi*440*double(input+i)/48000));
      input+=960;r.queue_audio(pcm,960);
      // Three equal clock phases: nominal, source +500ppm, source -500ppm.

    }
    const int depth=r.queued_ms();depths.push_back(depth);peak_queue=std::max(peak_queue,depth);
    const int tick=static_cast<int>(elapsed);
    if(tick!=last_report){last_report=tick;auto s=r.audio_stats();log<<nlohmann::json{{"kind","sample"},{"elapsed_s",elapsed},{"queue_ms",depth},{"phase",phase},{"source_relative_ppm",source_ppm},{"source_samples",input},{"consumed_samples",consumed},{"starved",s.starved},{"trimmed",s.trimmed},{"dropped",s.dropped_drift},{"rebuffered",s.rebuffered},{"updates",s.compensation_updates},{"ppm",s.compensation_ppm},{"errors",s.compensation_errors},{"input_samples",s.input_samples},{"output_samples",s.output_samples}}.dump()<<std::endl;}
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if(depths.empty()){check(false,"wallclock collected queue samples");return;}
  std::sort(depths.begin(),depths.end());auto s=r.audio_stats();auto pct=[&](double p){return depths[static_cast<std::size_t>(p*double(depths.size()-1))];};
  const double elapsed=std::chrono::duration<double>(clock::now()-start).count();
  log<<nlohmann::json{{"kind","final"},{"elapsed_s",elapsed},{"queue_p50_ms",pct(.5)},{"queue_p95_ms",pct(.95)},{"queue_p99_ms",pct(.99)},{"queue_max_ms",peak_queue},{"source_samples",input},{"consumed_samples",previous_consumed},{"starved",s.starved},{"trimmed",s.trimmed},{"dropped",s.dropped_drift},{"rebuffered",s.rebuffered},{"updates",s.compensation_updates},{"ppm",s.compensation_ppm},{"errors",s.compensation_errors},{"input_samples",s.input_samples},{"output_samples",s.output_samples}}.dump()<<std::endl;
  check(elapsed>=seconds&&peak_queue<1000&&s.push_errors==0&&s.compensation_errors==0,
        "actual wallclock bounded queue and no processing errors");
  check(s.starved==0&&s.rebuffered==0&&s.trimmed==0&&s.dropped_drift==0,
        "controlled relative-clock SDL endurance has no depletion, rebuffer or whole-block drops");
}
}
int main(int argc,char** argv){
  if(argc==4&&std::string(argv[1])=="--wallclock"){
    int seconds=0;const std::string_view value(argv[2]);
    const auto parsed=std::from_chars(value.data(),value.data()+value.size(),seconds);
    if(parsed.ec!=std::errc{}||parsed.ptr!=value.data()+value.size()||seconds<1||seconds>86400){
      std::fprintf(stderr,"wallclock duration must be integer 1..86400 seconds\n");return 2;
    }
    wallclock(seconds,argv[3]);return failures?1:0;
  }
  library_quality();changing_compensation_quality();nlohmann::json results=nlohmann::json::array();
  for(int ppm:{-500,500})for(bool enabled:{false,true})results.push_back(simulate(ppm,enabled));
  std::puts(results.dump(2).c_str());return failures?1:0;
}
