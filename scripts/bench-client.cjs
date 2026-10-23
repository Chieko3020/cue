#!/usr/bin/env node
// 合成上行客户端（benchmark 专用）：把 H.264 Annex-B 文件按「访问单元」切分，按固定帧率
// 从 WebSocket 推进 cued。与 scripts/test-client.mjs 的区别：
//   * test-client 反复发同一张 677 B 的小 IDR → 只有 1 个下行分片，FEC 永不参与；
//   * 这里用真实编码流（每帧数 KB → 数个 ≤1200 B 数据报），多分片重组/FEC/NACK 才有意义。
//
// 用法：
//   node scripts/bench-client.cjs <ws_url> <file.h264> <seconds> [--fps 30] [--json out.json]
//
// 视频必须带 AUD（x264 的 aud=1）：访问单元边界靠 AUD 判定，否则只靠 start code 无法
// 区分「新帧的 slice」与「同帧的多个 slice」。bench.sh 生成样本时已固定此参数。
const fs = require('node:fs');

const [urlArg, fileArg, secondsArg] = process.argv.slice(2);
if (!urlArg || !fileArg || !secondsArg) {
  console.error('用法: node scripts/bench-client.cjs <ws_url> <file.h264> <seconds> [--fps 30] [--json out.json]');
  process.exit(2);
}
const url = urlArg.endsWith('/') ? urlArg : `${urlArg}/`;
const seconds = Number(secondsArg);

function opt(name, dflt) {
  const i = process.argv.indexOf(name);
  return i > 0 && i + 1 < process.argv.length ? process.argv[i + 1] : dflt;
}
const FPS = Number(opt('--fps', 30));
const JSON_OUT = opt('--json', '');
const HEADER = 32;
const MAX_FRAG = 16 * 1024; // 上行分片上限（SPEC §7.1）

// ---- Annex-B 切访问单元 ----
function splitAccessUnits(buf) {
  // 找所有 start code 的位置与长度
  const starts = [];
  for (let i = 0; i + 3 <= buf.length; ) {
    if (buf[i] === 0 && buf[i + 1] === 0 && buf[i + 2] === 1) {
      starts.push({ at: i, len: 3 });
      i += 3;
    } else if (i + 4 <= buf.length && buf[i] === 0 && buf[i + 1] === 0 && buf[i + 2] === 0 && buf[i + 3] === 1) {
      starts.push({ at: i, len: 4 });
      i += 4;
    } else {
      i += 1;
    }
  }
  if (starts.length === 0) return [];
  const nals = starts.map((s, k) => ({
    start: s.at,
    end: k + 1 < starts.length ? starts[k + 1].at : buf.length,
    type: buf[s.at + s.len] & 0x1f,
  }));
  const aus = [];
  let cur = null;
  for (const nal of nals) {
    // AUD(9) 一定标志新访问单元；流开头没有 AUD 时也开一个新单元
    if (nal.type === 9 || cur === null) {
      if (cur) aus.push(cur);
      cur = { start: nal.start, end: nal.end };
    } else {
      cur.end = nal.end;
    }
  }
  if (cur) aus.push(cur);
  return aus
    .map((a) => {
      const au = buf.subarray(a.start, a.end);
      // 访问单元里出现 SPS(7) / IDR 片(5) 即视为关键帧（上行头只有 1 bit 表达这件事）
      let key = false;
      for (const nal of nals) {
        if (nal.start < a.start || nal.start >= a.end) continue;
        if (nal.type === 7 || nal.type === 5) { key = true; break; }
      }
      return { au, key };
    })
    .filter((f) => f.au.length > 0);
}

const sample = fs.readFileSync(fileArg);
const frames = splitAccessUnits(sample);
if (frames.length === 0) {
  console.error('[bench-client] 样本里没有 Annex-B NAL（需要 AUD 分隔的访问单元）');
  process.exit(2);
}
const fragCounts = frames.map((f) => Math.max(1, Math.ceil(f.au.length / MAX_FRAG)));
const avgBytes = frames.reduce((s, f) => s + f.au.length, 0) / frames.length;

// ---- WS + 时钟同步（与 test-client 一致：8 次 ping 取最小 RTT 的 offset）----
let sawSummary = false;
// onerror/onclose 要报 frames_sent，但它们可能在发送循环给这个变量赋值之前就触发。
// 缺这行声明时，错误处理自己抛 ReferenceError，把真正的断连原因盖掉
// （实测只看到 "sentFrames is not defined"，看不到 cued 到底是拒绝还是半路挂掉）。
let sentFrames = 0;
const ws = new WebSocket(url);
ws.binaryType = 'arraybuffer';

function u32(dv, off, v) { dv.setUint32(off, v >>> 0, true); }
function u64(dv, off, v) { dv.setBigUint64(off, BigInt(v), true); }

// 连接异常必须带上下文：只打 "error" 定位不到是哪一端、哪一秒断的（调试时吃过这个亏）
ws.onerror = (e) => {
  if (sawSummary) return; // 正常收尾阶段的 close 竞态不计为失败
  console.error(`[bench-client] WS 错误: ${e.message || e.type}`
    + ` detail=${JSON.stringify(e.error && (e.error.message || String(e.error)))}`
    + ` frames_sent=${sentFrames}`);
  process.exit(1);
};
ws.onclose = (e) => {
  if (sawSummary) return;
  console.error(`[bench-client] WS 关闭: code=${e.code} reason=${e.reason || ''} clean=${e.wasClean}`
    + ` frames_sent=${sentFrames}`);
  process.exit(1);
};

ws.onopen = async () => {
  let offset = 0;
  let bestRtt = Infinity;
  for (let i = 0; i < 8; i++) {
    const t = Math.round(performance.now() * 1000);
    const pong = await new Promise((resolve) => {
      const onMsg = (ev) => {
        try {
          const j = JSON.parse(ev.data);
          if (j.op === 'pong' && j.t === t) { ws.removeEventListener('message', onMsg); resolve(j); }
        } catch (_) {}
      };
      ws.addEventListener('message', onMsg);
      ws.send(JSON.stringify({ op: 'ping', t }));
      setTimeout(() => { ws.removeEventListener('message', onMsg); resolve(null); }, 200);
    });
    if (pong) {
      const nowUs = Math.round(performance.now() * 1000);
      const rtt = nowUs - t;
      if (rtt < bestRtt) {
        bestRtt = rtt;
        offset = Number(pong.server_us) - (t + rtt / 2);
      }
    }
  }

  let frameId = 0;
  let packets = 0;
  let ai = 0;
  const t0 = Date.now();
  const intervalMs = 1000 / FPS;

  const sendOne = () => {
    const { au, key } = frames[ai % frames.length];
    ai += 1;
    const pts = Math.max(0, Math.round(performance.now() * 1000 + offset));
    const fragCount = Math.max(1, Math.ceil(au.length / MAX_FRAG));
    for (let i = 0; i < fragCount; i++) {
      const off = i * MAX_FRAG;
      const len = Math.min(MAX_FRAG, au.length - off);
      const pkt = new Uint8Array(HEADER + len);
      const dv = new DataView(pkt.buffer);
      pkt[0] = 0xc1;
      pkt[1] = 0x01; // video
      pkt[2] = 0x01; // h264
      pkt[3] = (key ? 0x01 : 0x00) | (i === fragCount - 1 ? 0x04 : 0x00);
      u32(dv, 4, 1); // stream_id
      u32(dv, 8, frameId);
      u32(dv, 12, i);
      u32(dv, 16, fragCount);
      u64(dv, 20, pts);
      u32(dv, 28, len);
      pkt.set(au.subarray(off, off + len), HEADER);
      ws.send(pkt);
      packets += 1;
    }
    frameId += 1;
    sentFrames = frameId;
  };

  const timer = setInterval(() => {
    if (Date.now() - t0 >= seconds * 1000) {
      clearInterval(timer);
      const summary = {
        frames_sent: frameId,
        packets_sent: packets,
        seconds: (Date.now() - t0) / 1000,
        fps: frameId / ((Date.now() - t0) / 1000),
        sample_frames: frames.length,
        sample_frame_bytes: Math.round(avgBytes),
        sample_fragments_per_frame: Number((fragCounts.reduce((s, n) => s + n, 0) / fragCounts.length).toFixed(2)),
        clock_offset_us: Math.round(offset),
        clock_rtt_us: Number.isFinite(bestRtt) ? bestRtt : null,
      };
      sawSummary = true;
      console.log(`[bench-client] ${JSON.stringify(summary)}`);
      if (JSON_OUT) fs.writeFileSync(JSON_OUT, JSON.stringify(summary, null, 2));
      // 给 cued 最后一秒的 METRICS 一个完整的结算窗口再退出
      setTimeout(() => { ws.close(); process.exit(0); }, 1200);
      return;
    }
    sendOne();
  }, intervalMs);
};
