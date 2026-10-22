// T-22（FR-PIPE.12）：会话表的纯逻辑（设备注册 / 租约 / 能力协商 / 回收原因）。
//
// 为什么这些断言值得单独存在：多设备会话最容易"看起来能跑但语义错"——
// 例如租约期满只断了 socket 却没清表、同一 device_id 重连时旧会话没被顶掉、
// 能力夹取把 1080p 原样发给只支持 720p 的设备。这些都是纯状态问题，
// 用假时钟就能钉死，不必起真进程（真链路的行为由 integration 覆盖）。
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "cue/pipeline/control_plane.h"
#include "cue/pipeline/session_registry.h"

namespace pipeline = cue::pipeline;
using json = nlohmann::json;

namespace {

int failures = 0;
void check(bool cond, const std::string& what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}

constexpr int64_t kLease = 1'000'000;  // 1 s
constexpr uint16_t kBase = 9001;

pipeline::SessionRegistry::Options opts(int max_sessions = 2) {
  pipeline::SessionRegistry::Options o;
  o.lease_us = kLease;
  o.max_sessions = max_sessions;
  o.probe_port_base = kBase;
  return o;
}

// fd = -1：不让 kill/回收真的去 shutdown 一个不存在的 socket
pipeline::SessionRegistry::RegisterRequest req(const std::string& dev) {
  pipeline::SessionRegistry::RegisterRequest r;
  r.device_id = dev;
  r.peer = "127.0.0.1:12345";
  r.fd = -1;
  return r;
}

pipeline::DeviceCaps caps_of(int max_w, int max_h) {
  pipeline::DeviceCaps c;
  c.declared = true;
  c.max_width = max_w;
  c.max_height = max_h;
  c.name = "test-device";
  return c;
}

}  // namespace

int main() {
  // ---------------------------------------------------------------- 1) 注册与容量
  {
    pipeline::SessionRegistry reg(opts(2));
    auto a = reg.register_session(req("dev-a"), 0);
    check(a.has_value(), "注册第一个会话成功");
    check(reg.live_count() == 1, "live_count 反映注册数");
    check(a->probe_port == kBase, "第一个会话拿到 probe_port_base");
    check(!a->device_id.empty(), "device_id 非空（自报或派生）");

    auto b = reg.register_session(req("dev-b"), 0);
    check(b.has_value(), "注册第二个会话成功");
    check(b->probe_port == kBase + 1, "第二个会话的 probe 端口递增");
    check(b->session_id != a->session_id, "会话号不重复");

    auto c = reg.register_session(req("dev-c"), 0);
    check(!c.has_value(), "超过 max_sessions 的注册被拒");
    check(reg.live_count() == 2, "被拒的注册不占名额");
  }

  // ---------------------------------------------------------------- 2) 租约
  {
    pipeline::SessionRegistry reg(opts(4));
    const int64_t t0 = 1'000;
    auto a = reg.register_session(req("dev-a"), t0);

    check(reg.expire_due(t0 + kLease - 1).empty(), "租约未到期不回收");
    check(reg.touch(a->session_id, t0 + kLease - 1), "心跳续租成功");
    check(reg.expire_due(t0 + kLease).empty(), "续租后原来的到期时刻不再回收");

    auto expired = reg.expire_due(t0 + 2 * kLease + 10);
    check(expired.size() == 1 && expired[0] == a->session_id, "超过租约后被回收");
    check(reg.close_reason(a->session_id) == "lease_expired",
          "回收原因写 lease_expired（不是空，也不是正常断开）");
    check(reg.live_count() == 0, "回收后 live_count 归零");
  }

  // ---------------------------------------------------------------- 3) 主动 kill
  {
    pipeline::SessionRegistry reg(opts(4));
    auto a = reg.register_session(req("dev-a"), 0);
    check(reg.kill(a->session_id, "session.kill"), "kill 返回 true");
    check(reg.close_reason(a->session_id) == "session.kill", "kill 的原因可查");
    check(!reg.touch(a->session_id, 1), "被 kill 的会话不能再续租");
    check(reg.live_count() == 0, "kill 后名额释放");
  }

  // ---------------------------------------------------------------- 4) 同一 device_id 顶掉旧会话
  // "客户端崩溃后立刻重连"不该等旧租约到期；代价是 cued 本来就不鉴权。
  {
    pipeline::SessionRegistry reg(opts(4));
    auto a = reg.register_session(req("same-dev"), 0);
    auto b = reg.register_session(req("same-dev"), 10);
    check(b.has_value() && b->session_id != a->session_id, "同 id 重连拿到新会话号");
    check(reg.close_reason(a->session_id) == "device_replaced", "旧会话被标 replaced");
    check(reg.live_count() == 1, "顶掉后只留一个活会话");
  }

  // ---------------------------------------------------------------- 5) 能力协商：夹取
  {
    pipeline::RuntimeParams rp;
    rp.transcode = true;
    rp.width = 1920;
    rp.height = 1080;
    rp.bitrate_kbps = 4000;

    // 设备只支持 720p：分辨率与码率都要按像素比例降下来，而不是原样下发
    auto p = pipeline::assign_profile(rp, caps_of(1280, 720));
    check(p.clamped, "能力不足时 clamped=true");
    check(p.width <= 1280 && p.height <= 720, "分辨率被夹到设备能力内");
    check(p.width % 2 == 0 && p.height % 2 == 0, "夹取后仍是偶数（编码器要求）");
    check(p.bitrate_kbps < 4000, "码率按像素比例下调");
    check(p.basis == "caps_clamped", "basis 标明是夹取结果");

    // 配置本身就在能力范围内：不该被动
    pipeline::RuntimeParams rp2 = rp;
    rp2.width = 640;
    rp2.height = 360;
    auto p2 = pipeline::assign_profile(rp2, caps_of(1280, 720));
    check(!p2.clamped, "能力足够时不夹取");
    check(p2.width == 640 && p2.height == 360, "配置值原样下发");
    check(p2.basis == "caps_ok", "basis 标明能力足够");

    // 没有能力上报（旧客户端 / 假域）：与 T-22 之前逐字节一致
    auto p3 = pipeline::assign_profile(rp, pipeline::DeviceCaps{});
    check(!p3.clamped && p3.width == 1920 && p3.height == 1080,
          "没有能力上报时按配置下发（不改旧行为）");
  }

  // ---------------------------------------------------------------- 6) 会话表的 JSON 视图
  {
    pipeline::SessionRegistry reg(opts(4));
    auto a = reg.register_session(req("dev-a"), 0);
    (void)reg.register_session(req("dev-b"), 0);
    // sessions_json 返回的是 {"sessions": [...], "count", "live", ...}，不是裸数组
    const auto live = reg.sessions_json(0, false);
    check(live.is_object() && live.contains("sessions"), "sessions_json 是对象且带 sessions 键");
    check(live["sessions"].is_array() && live["sessions"].size() == 2, "只列活会话");
    check(live["live"].get<int>() == 2, "live 计数与活会话数一致");
    check(live["sessions"][0].contains("session_id") && live["sessions"][0].contains("device_id"),
          "每行带 session_id 与 device_id");

    (void)reg.kill(a->session_id, "session.kill");
    const auto with_closed = reg.sessions_json(0, true);
    check(with_closed["sessions"].size() >= 2, "include_closed 时也能看到已关闭的（便于排查）");
    check(with_closed["live"].get<int>() == 1, "关闭一个之后 live 降到 1");

    const auto agg = reg.aggregate(0);
    check(agg.is_object(), "aggregate 返回对象（设备页汇总用）");
  }

  if (failures == 0) {
    std::printf("test_session_registry: OK\n");
  } else {
    std::printf("test_session_registry: %d failure(s)\n", failures);
  }
  return failures == 0 ? 0 : 1;
}
