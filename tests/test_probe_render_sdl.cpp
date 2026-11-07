// Real SDL queue and dummy consumer. This test never selects a physical device.
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <cstdio>
#include <string>
#include <vector>
#include "probe_render.h"
namespace {
int failures=0;
void check(bool ok,const char* what){if(!ok){std::printf("FAIL: %s\n",what);++failures;}}
int16_t pcm[960]{};
bool drained(cue::probe::Renderer& r){
  const Uint64 end=SDL_GetTicks64()+6000;
  while(SDL_GetTicks64()<end){if(r.queued_ms()==0)return true;SDL_Delay(10);}return false;
}
void targets(){
  for(int target:{0,250,260,500,2000}){
    cue::probe::Renderer r;cue::probe::RenderConfig cfg;cfg.audio_prebuffer_ms=target;cfg.audio_drift_compensation=false;
    std::string error;check(r.open(cfg,error),"real SDL Renderer opens");
    check(r.queued_ms()>=0,"dummy audio device actually opened");
    if(r.queued_ms()<0)continue;
    const int packets=target?((target+19)/20):1;
    for(int i=0;i<packets-1;++i)r.queue_audio(pcm,960);
    if(packets>1){const int before=r.queued_ms();SDL_Delay(80);
      check(before==(packets-1)*20&&r.queued_ms()==before,"below target queue remains paused");}
    r.queue_audio(pcm,960);
    check(r.audio_stats().queued==static_cast<uint64_t>(packets)&&
          r.audio_stats().dropped_drift==0,"all startup PCM accepted by real SDL");
    check(drained(r),"real SDL consumer starts and drains every legal target");
  }
}
void compensated_targets(){
  for(int target:{260,500,2000}){
    cue::probe::Renderer r;cue::probe::RenderConfig cfg;cfg.audio_prebuffer_ms=target;
    std::string error;check(r.open(cfg,error)&&r.queued_ms()>=0,"default swr SDL target opens");
    if(r.queued_ms()<0)continue;
    for(int i=0;i<(target+19)/20+1;++i)r.queue_audio(pcm,960);
    for(int i=0;i<15;++i){SDL_Delay(20);r.sample_audio();r.queue_audio(pcm,960);}
    const auto stats=r.audio_stats();
    check(stats.dropped_drift==0&&stats.trimmed==0&&stats.compensation_errors==0,
          "default swr normal continuation at high target neither drops nor hard-clears");
    check(stats.input_samples>0&&stats.output_samples>0,"default playback traverses real swr");
  }
}
void resets(){
  cue::probe::Renderer r;cue::probe::RenderConfig cfg;cfg.audio_prebuffer_ms=500;cfg.audio_drift_compensation=false;
  std::string error;check(r.open(cfg,error),"reset SDL Renderer opens");
  if(r.queued_ms()<0){check(false,"reset dummy audio opened");return;}
  for(int i=0;i<25;++i)r.queue_audio(pcm,960);
  const auto count=r.audio_stats().queued;r.reset_audio();r.queue_audio(pcm,960);
  SDL_Delay(80);check(r.queued_ms()==20,"reset really pauses and re-prebuffers");
  for(int i=0;i<24;++i)r.queue_audio(pcm,960);
  check(drained(r)&&r.audio_stats().queued==count+25,"reset starts again and counts cumulatively");
  r.reset_audio();std::vector<int16_t> burst(48000*10);
  r.queue_audio(burst.data(),static_cast<int>(burst.size()));
  check(r.queued_ms()>2000,"real SDL hard limit fixture has actual excess queued PCM");
  r.queue_audio(pcm,960);
  SDL_Delay(80);check(r.queued_ms()==20&&r.audio_stats().trimmed==1,
                      "hard clear really rearms positive prebuffer and retains current segment");
  for(int i=0;i<24;++i)r.queue_audio(pcm,960);
  check(drained(r),"real SDL consumes again after hard clear");
}
}
int main(){
  SDL_SetMainReady();SDL_setenv("SDL_AUDIODRIVER","dummy",1);
  SDL_setenv("SDL_VIDEODRIVER","dummy",1);SDL_setenv("SDL_RENDER_DRIVER","software",1);
  check(cue::probe::Renderer::available(),"test requires actual SDL branch");
  for(int invalid:{-1,2001}){cue::probe::Renderer r;cue::probe::RenderConfig cfg;
    cfg.audio_prebuffer_ms=invalid;std::string error;
    check(!r.open(cfg,error)&&error.find("2000")!=std::string::npos,"invalid direct Renderer configuration fails");}
  targets();resets();compensated_targets();std::printf("SDL renderer failures=%d\n",failures);return failures?1:0;
}
