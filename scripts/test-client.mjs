// Cue 链路自测：用 Node 模拟浏览器上行（不需要真实浏览器）
// 用法：node scripts/test-client.mjs [ws_url] [seconds]
//   WS 地址默认 ws://127.0.0.1:${WS_PORT:-9000}/，可显式传参。
//
// 发送的是内嵌的 64x64 单帧 Annex-B IDR（约 677 B，SPS/PPS 内联），cued 会真实解码，
// 因此在 cued/probe 侧应看到 dec 增长、dec_err 为 0。
// 不含编码器、不测 WebCodecs（那部分由 web/app.js + e2e-browser.cjs 覆盖），
// 也不覆盖多分片重组（该帧 < 16 KiB 分片上限）。

// 分片/打包逻辑与协议对照脚本共用 scripts/lib/upstream-wire.mjs：
// 同一份实现才谈得上"浏览器模拟端与原生采集端逐字节一致"。
import { packMediaPackets, TYPE_VIDEO, CODEC_H264 } from './lib/upstream-wire.mjs';

const url = process.argv[2] || `ws://127.0.0.1:${process.env.WS_PORT || 9000}/`;
const seconds = Number(process.argv[3] || 5);
const FPS = 30;

// ffmpeg -f lavfi -i color=c=blue:s=64x64:d=0.04 -frames:v 1 -c:v libx264 \
//   -profile:v baseline -pix_fmt yuv420p -x264-params keyint=1:min-keyint=1:scenecut=0 -f h264
const IDR_ANNEXB = Buffer.from(
  'AAAAAWdCwArcQmwEQAAAAwBAAAAMg8SJ4AAAAAFozg8sgAAAAQYF//9Z3EXpvebZSLeWLNgg2SPu73gyNjQgLSBjb3JlIDE2NCByMzEwOCAzMWUxOWY5IC0gSC4yNjQvTVBFRy00IEFWQyBjb2RlYyAtIENvcHlsZWZ0IDIwMDMtMjAyMyAtIGh0dHA6Ly93d3cudmlkZW9sYW4ub3JnL3gyNjQuaHRtbCAtIG9wdGlvbnM6IGNhYmFjPTAgcmVmPTEgZGVibG9jaz0xOjA6MCBhbmFseXNlPTB4MToweDExMSBtZT1oZXggc3VibWU9NyBwc3k9MSBwc3lfcmQ9MS4wMDowLjAwIG1peGVkX3JlZj0wIG1lX3JhbmdlPTE2IGNocm9tYV9tZT0xIHRyZWxsaXM9MSA4eDhkY3Q9MCBjcW09MCBkZWFkem9uZT0yMSwxMSBmYXN0X3Bza2lwPTEgY2hyb21hX3FwX29mZnNldD0tMiB0aHJlYWRzPTIgbG9va2FoZWFkX3RocmVhZHM9MSBzbGljZWRfdGhyZWFkcz0wIG5yPTAgZGVjaW1hdGU9MSBpbnRlcmxhY2VkPTAgYmx1cmF5X2NvbXBhdD0wIGNvbnN0cmFpbmVkX2ludHJhPTAgYmZyYW1lcz0wIHdlaWdodHA9MCBrZXlpbnQ9MSBrZXlpbnRfbWluPTEgc2NlbmVjdXQ9MCBpbnRyYV9yZWZyZXNoPTAgcmM9Y3JmIG1idHJlZT0wIGNyZj0yMy4wIHFjb21wPTAuNjAgcXBtaW49MCBxcG1heD02OSBxcHN0ZXA9NCBpcF9yYXRpbz0xLjQwIGFxPTE6MS4wMACAAAABZYiEBLxGKAAMxMcAAX1o4AAiQycnJ1111111111114A=',
  'base64',
);

const ws = new WebSocket(url);
ws.binaryType = 'arraybuffer';

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
    const pts = Math.max(0, Math.round(performance.now() * 1000 + offset));
    const packets = packMediaPackets(
      { type: TYPE_VIDEO, codec: CODEC_H264, streamId: 1, frameId, keyframe: true, ptsUs: pts },
      IDR_ANNEXB,
    );
    for (const pkt of packets) ws.send(pkt);
    frameId = (frameId + 1) >>> 0;
    sent++;
  }, 1000 / FPS);

  setTimeout(() => {
    clearInterval(timer);
    console.log(`[test-client] sent ${sent} frames (${(sent * IDR_ANNEXB.length / 1024).toFixed(0)} KiB payload), closing`);
    ws.close();
    setTimeout(() => process.exit(0), 200);
  }, seconds * 1000);
};

ws.onerror = (e) => {
  console.error('[test-client] ws error', e.message || e);
  process.exit(1);
};
