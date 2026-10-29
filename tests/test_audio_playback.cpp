#include <cstdio>
#include <string>
#include <vector>
#include "audio_playback.h"

namespace {
int failures = 0;
void check(bool ok, const char* what) { if (!ok) { std::printf("FAIL: %s\n",what); ++failures; } }
struct Device final : cue::probe::AudioQueueDevice {
  bool present = true, paused = true, fail = false;
  uint32_t bytes = 0;
  std::vector<std::string> actions;
  bool available() const override { return present; }
  int queued_ms() const override { return static_cast<int>(bytes / 96); }
  void pause(bool p) override { paused=p;actions.push_back(p?"pause":"play"); }
  void clear() override { bytes=0;actions.push_back("clear"); }
  bool queue(const int16_t*, uint32_t count) override {
    actions.push_back("queue"); if(fail)return false;bytes+=count;return true;
  }
};
int16_t pcm[960]{};
void targets() {
  for(int target:{0,250,260,500,2000}) {
    Device d;cue::probe::AudioPlaybackQueue q(d);
    check(q.configure(target),"legal prebuffer accepted");
    check(d.paused==(target>0),"initial device pause matches target");
    const int packets=target?((target+19)/20):1;
    for(int i=0;i<packets;++i){
      q.push(pcm,960);
      check(q.started()==(target==0||(i+1)*20>=target),"starts on successful threshold segment");
    }
    check(!d.paused&&q.started(),"all targets start with enough PCM");
    check(q.stats().queued==static_cast<uint64_t>(packets)&&q.stats().dropped_drift==0,
          "startup is never prevented by playback soft limit");
  }
}
void limits_and_reset() {
  Device d;cue::probe::AudioPlaybackQueue q(d);q.configure(0);
  d.bytes=250*96;q.push(pcm,960);
  check(d.queued_ms()==270&&q.stats().queued==1,"soft threshold equality accepts current segment");
  d.bytes=251*96;q.push(pcm,960);
  check(d.queued_ms()==251&&q.stats().over_soft==1&&q.stats().dropped_drift==1,
        "soft overflow drops current segment without clearing old PCM");
  d.bytes=2000*96;q.push(pcm,960);
  check(q.stats().trimmed==0&&d.queued_ms()==2000,"hard threshold equality does not clear");
  for(int target:{0,500}) {
    q.configure(target);d.bytes=2001*96;d.actions.clear();q.push(pcm,960);
    const std::vector<std::string> wanted=target?std::vector<std::string>{"pause","clear","queue"}:
        std::vector<std::string>{"pause","clear","play","queue"};
    check(d.actions==wanted,"hard recovery pauses, clears, rearms, then queues current PCM");
    check(d.queued_ms()==20&&d.paused==(target>0),"hard clear reuses configured startup state");
  }
  check(q.stats().trimmed==2,"hard overflow counted once per recovery");
  const auto queued=q.stats().queued;
  d.actions.clear();q.reset();
  check(d.actions==std::vector<std::string>{"pause","clear"}&&d.bytes==0,
        "buffering reset clears and remains paused");
  check(q.stats().trimmed==2&&q.stats().queued==queued,"reset preserves cumulative counters");
  for(int i=0;i<25;++i)q.push(pcm,960);
  check(q.started(),"positive target starts again after reset");
  q.reset();check(!q.started()&&d.paused&&d.bytes==0,"playing reset returns to prebuffer");
  q.configure(0);d.actions.clear();q.reset();
  check(d.actions==std::vector<std::string>{"pause","clear","play"},"zero target reset resumes immediately");
}
void failures_and_unavailable() {
  Device d;cue::probe::AudioPlaybackQueue q(d);q.configure(500);d.bytes=480*96;d.fail=true;
  q.push(pcm,960);
  check(!q.started()&&d.paused&&q.stats().queued==0&&q.stats().push_errors==1,
        "failed threshold segment neither starts nor counts success");
  d.fail=false;q.push(pcm,960);
  check(q.started()&&q.stats().queued==1&&q.stats().push_errors==1,"successful retry can start");
  const auto count=q.stats().queued;q.push(nullptr,960);q.push(pcm,0);q.push(pcm,-1);
  check(q.stats().queued==count,"invalid PCM calls do nothing");
  check(!q.configure(-1)&&!q.configure(2001),"out of range direct configuration rejected");
  Device absent;absent.present=false;cue::probe::AudioPlaybackQueue no_device(absent);
  no_device.configure(500);no_device.push(pcm,960);no_device.reset();
  check(absent.actions.empty()&&no_device.stats().queued==0,"unavailable device has no operations or success");
}
}
int main(){targets();limits_and_reset();failures_and_unavailable();
  std::printf("audio playback failures=%d\n",failures);return failures?1:0;}
