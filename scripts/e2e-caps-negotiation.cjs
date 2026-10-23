// FR-DEV.4 端到端：设备 0x05 上报能力 → cued 按能力夹取下发档位（不重启）。
//
// 为什么需要一个独立脚本：夹取逻辑（assign_profile）有单测，但"真客户端发 0x05 →
// 服务端重算 → 事件里能读到夹取结果"这条链路此前从未端到端跑过 —— 实测时
// caps_declared 一直是 false，等于能力协商只在单测里成立。
//
// 两个容易踩的点（本脚本踩过）：
//   1. 0x05 的载荷必须带 "kind":"caps"，否则 cued 落到 device.event 分支，只当普通事件；
//   2. 档位重算在 process_frame 里（解码之前），所以**必须推几帧**才会触发 ——
//      只发 0x05 不够。真实场景下浏览器持续推流，所以生产路径没问题。
//
// 用法：node scripts/e2e-caps-negotiation.cjs [ws_url]
//   需要先起 cued（建议 --transcode on --width 1920 --height 1080 --bitrate 4000），
//   脚本会从 cued 的 stderr 判断结果 —— 所以调用方要把 cued 日志喂给它。
const { execFileSync } = require('node:child_process');

const url = process.argv[2] || 'ws://127.0.0.1:9500/';
// cued 的日志路径由调用方通过 CUE_LOG 传；没有就只打印事件、不做断言。
const logPath = process.env.CUE_LOG || '';

const caps = {
  kind: 'caps',
  device_id: 'caps-probe',
  name: 'FR-DEV.4 e2e',
  source: 'e2e',
  capture_path: 'e2e',
  video: {
    chosen: 'avc1.42001E', degraded: false, h264: true, vp8: true,
    width: 1920, height: 1080, fps: 30,
    max: { width: 1280, height: 720, fps: 30 },
    native: { width: 1280, height: 720 },
  },
  audio: { opus: true },
  devices: { total: 2 },
};

function hdr(type, flags, frameId, fragIdx, fragCount, pts, payloadLen) {
  const b = Buffer.alloc(32);
  b[0] = 0xc1; b[1] = type; b[2] = 0x01; b[3] = flags;
  b.writeUInt32LE(1, 4); b.writeUInt32LE(frameId, 8);
  b.writeUInt32LE(fragIdx, 12); b.writeUInt32LE(fragCount, 16);
  b.writeBigUInt64LE(BigInt(pts), 20); b.writeUInt32LE(payloadLen, 28);
  return b;
}
const statsPacket = (obj) => {
  const body = Buffer.from(JSON.stringify(obj), 'utf8');
  return Buffer.concat([hdr(0x05, 0, 0, 0, 0, 0, body.length), body]);
};
// 伪 IDR：只为让包进重组与 jb，进而跑到 process_frame 里的档位重算（在解码之前）。
// 解码会失败（decode_errors 增加），与本次要验的夹取无关。
const fakeIdr = Buffer.from('000000016742001e0000000168ce3c80', 'hex');

async function main() {
  const ws = new WebSocket(url);
  ws.binaryType = 'arraybuffer';
  await new Promise((res, rej) => {
    ws.addEventListener('open', res, { once: true });
    ws.addEventListener('error', (e) => rej(new Error(e.message || 'ws error')), { once: true });
  });
  ws.send(statsPacket(caps));
  for (let i = 0; i < 30; i++) {
    const body = fakeIdr;
    ws.send(Buffer.concat([hdr(0x01, 0x01 | 0x04, i, 0, 1, 1_000_000 + i * 33333, body.length), body]));
    await new Promise((r) => setTimeout(r, 33));
  }
  await new Promise((r) => setTimeout(r, 800));
  ws.close();
  await new Promise((r) => setTimeout(r, 300));

  if (!logPath) {
    console.log('[caps] 包已发送；未给 CUE_LOG，跳过断言（用 CUE_LOG=<cued 日志> 启用）');
    return;
  }
  const log = require('node:fs').readFileSync(logPath, 'utf8');
  const events = log.split('\n')
    .map((l) => { const i = l.indexOf('{'); if (i < 0) return null; try { return JSON.parse(l.slice(i)); } catch { return null; } })
    .filter((d) => d && typeof d.ev === 'string' && d.ev.startsWith('device.'));

  const capsEv = events.find((e) => e.ev === 'device.caps');
  const assigned = events.filter((e) => e.ev === 'device.profile_assigned');
  const clamped = assigned.find((e) => (e.data || {}).basis === 'caps_clamped');

  const fail = [];
  if (!capsEv) fail.push('没收到 device.caps（0x05 被当成了普通事件？检查 kind=caps）');
  if (assigned.length < 2) fail.push(`profile_assigned 只出现 ${assigned.length} 次（期望注册 1 次 + 重算 1 次）`);
  if (!clamped) fail.push('没有 basis=caps_clamped 的下发（档位没按能力重算）');
  if (clamped) {
    // 夹取结果在 detail.clamped（不是事件顶层）：事件顶层只有 basis/clamped/detail。
    const det = (clamped.data || {}).detail || {};
    const c = det.clamped;
    if (!c) {
      fail.push('detail.clamped 缺失（看不到 from/to）');
    } else {
      if (c.width > 1280 || c.height > 720) fail.push(`夹取后仍是 ${c.width}x${c.height}，超过设备上限 1280x720`);
      if (!(c.bitrate_kbps > 0 && c.bitrate_kbps < 4000)) fail.push(`码率没按像素比例下调：${c.bitrate_kbps}`);
      const f = c.from || {};
      console.log('[caps] 夹取：%dx%d @%d kbps → %dx%d @%d kbps',
        f.width, f.height, f.bitrate_kbps, c.width, c.height, c.bitrate_kbps);
    }
  }
  if (fail.length) { console.error('[caps] FAIL：\n  - ' + fail.join('\n  - ')); process.exit(1); }
  {
    const c = ((clamped.data || {}).detail || {}).clamped || {};
    console.log('[caps] PASS：0x05 → 能力入库 → 档位按能力夹取 (%dx%d → %dx%d)',
      1920, 1080, c.width, c.height);
  }
}

main().catch((e) => { console.error('[caps] 异常：', e.message); process.exit(1); });
