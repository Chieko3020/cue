// M1 链路自测：用 Node 模拟浏览器上行（不需要真实浏览器）
// 用法：node scripts/test-client.mjs [ws_url] [seconds]
// 目的：验证 cued → probe 的 C++ 链路（编码器不参与，发送的是合成 payload）

const url = process.argv[2] || 'ws://127.0.0.1:9000/';
const seconds = Number(process.argv[3] || 5);
const FPS = 30;
const FRAME_BYTES = 4096;
const HEADER = 32;

const ws = new WebSocket(url);
ws.binaryType = 'arraybuffer';

function u32(dv, off, v) { dv.setUint32(off, v >>> 0, true); }
function u64(dv, off, v) { dv.setBigUint64(off, BigInt(v), true); }

ws.onopen = async () => {
  console.log('[test-client] connected', url);

  // 时钟同步：取最小 RTT 的一次
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
  console.log(`[test-client] clock offset=${(offset / 1000).toFixed(2)}ms rtt=${(bestRtt / 1000).toFixed(2)}ms`);

  let frameId = 0;
  let sent = 0;
  const t0 = Date.now();

  const timer = setInterval(() => {
    const pts = Math.round(performance.now() * 1000 + offset);
    const payload = new Uint8Array(FRAME_BYTES);
    // 填充可辨识内容，方便 probe 侧扩展校验
    payload.fill(frameId & 0xff);
    const pkt = new Uint8Array(HEADER + payload.length);
    const dv = new DataView(pkt.buffer);
    pkt[0] = 0xc1;
    pkt[1] = 0x01; // video
    pkt[2] = 0x01; // h264
    pkt[3] = 0x01 | 0x04; // keyframe | last_frag
    u32(dv, 4, 1);        // stream_id
    u32(dv, 8, frameId);
    u32(dv, 12, 0);       // frag_index
    u32(dv, 16, 1);       // frag_count
    u64(dv, 20, pts);
    u32(dv, 28, payload.length);
    pkt.set(payload, HEADER);
    ws.send(pkt);
    frameId++;
    sent++;

    if (Date.now() - t0 >= seconds * 1000) {
      clearInterval(timer);
      console.log(`[test-client] sent ${sent} frames, closing`);
      ws.close();
      setTimeout(() => process.exit(0), 200);
    }
  }, 1000 / FPS);
};

ws.onerror = (e) => {
  console.error('[test-client] ws error', e.message || e);
  process.exit(1);
};
