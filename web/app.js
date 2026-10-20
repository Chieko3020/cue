// Cue · 浏览器端推流页：Canvas 合成源 → WebCodecs 编码 → WS 上行
// 协议见 SPEC §7.1（32 字节媒体头，小端）；时钟同步见 §7.1 的 offset 估算。
// WS 地址默认 ws(s)://<本页主机>:9000/，可用 ?ws=ws://host:port/ 覆盖，?ws_port=N 只换端口。

const HEADER = 32;
const MAX_FRAG = 16 * 1024;
const CLOCK_RESYNC_MS = 30_000;
const CLOCK_DRIFT_US = 5_000;

function resolveWsUrl() {
  const q = new URLSearchParams(location.search);
  const override = q.get('ws');
  if (override) return override;
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const port = q.get('ws_port') || '9000';
  // file:// 打开时 hostname 为空，退回回环地址
  const host = location.hostname || '127.0.0.1';
  return `${proto}//${host}:${port}/`;
}
const WS_URL = resolveWsUrl();

// SPEC §7.1 codec 字段：0x01 H264 / 0x02 VP8。目前只走 H264；映射集中在这里，
// 降级到 VP8 时头部不会继续谎报 H264（服务端 decode_media 不校验 codec）。
const CODEC_H264 = 0x01;
const CODEC_VP8 = 0x02;
function codecIdOf(codec) {
  if (codec.startsWith('avc1')) return CODEC_H264;
  if (codec.startsWith('vp8')) return CODEC_VP8;
  return 0;
}
let codecId = CODEC_H264;

const els = {
  start: document.getElementById('start'),
  stop: document.getElementById('stop'),
  res: document.getElementById('res'),
  fps: document.getElementById('fps'),
  canvas: document.getElementById('cv'),
  log: document.getElementById('log'),
};

let ws = null;
let encoder = null;
let timer = null;
let statsTimer = null;
let clockTimer = null;
let offsetUs = 0;
let clockAnchored = false;
let frameId = 0;
let streamId = 1;
let sentFrames = 0;
let sentBytes = 0;
let encodeErrors = 0;
// cued 重启（任何修复动作都会触发）后，新解码器要等下一个 GOP 才拿到 SPS/PPS，
// 中间这些帧全部解不出（实测 23 帧 dec_err）。重连后必须强制一次 IDR。
let needKeyframe = false;
let droppedNoWs = 0;
let manualStop = false;
let reconnecting = false;
let streaming = false;
let encoderBroken = false;

const LOG_MAX_LINES = 300;

// 日志一律走 textContent：消息里可能带异常文本，拼 HTML 就是注入面；
// 同时裁剪旧节点，避免长跑页面 DOM 无限增长。
function log(msg, cls) {
  const time = new Date().toISOString().slice(11, 23);
  const line = document.createElement('div');
  line.textContent = `${time}  ${msg}`;
  if (cls) line.className = cls;
  els.log.appendChild(line);
  while (els.log.childNodes.length > LOG_MAX_LINES) els.log.removeChild(els.log.firstChild);
  els.log.scrollTop = els.log.scrollHeight;
}

// AVC level 必须与分辨率匹配，否则 configure() 直接抛异常（M0 实测：L3.0 上限 414720 coded area）
function pickCodec(w, h) {
  const area = w * h;
  if (area <= 414720) return 'avc1.42001E';   // L3.0
  if (area <= 2097152) return 'avc1.640028';  // L4.0
  return null;
}

function drawFrame(i, w, h, fps) {
  const cv = els.canvas;
  const ctx = cv.getContext('2d', { alpha: false });
  const t = i / fps;
  const g = ctx.createLinearGradient(0, 0, w, h);
  g.addColorStop(0, `hsl(${(t * 40) % 360}, 70%, 45%)`);
  g.addColorStop(1, '#0b1020');
  ctx.fillStyle = g;
  ctx.fillRect(0, 0, w, h);

  // 移动方块：肉眼可见的卡顿/丢帧
  const bx = (t * 220) % (w - 80);
  ctx.fillStyle = '#ffffff';
  ctx.fillRect(bx, h * 0.55, 80, 80);

  // 时间码：肉眼可校验帧连续性
  ctx.fillStyle = '#ffffff';
  ctx.font = `${Math.round(h / 14)}px ui-monospace, monospace`;
  ctx.fillText(`frame ${i}`, 24, h * 0.18);
  ctx.fillText(`t=${t.toFixed(2)}s`, 24, h * 0.28);
  ctx.fillText(`${w}x${h}@${fps}`, 24, h * 0.38);
}

function sendChunk(chunk) {
  // 断线期间编码不停，但包发不出去：计数后丢弃，重连后继续（否则会崩在 ws.send 上）
  if (!ws || ws.readyState !== WebSocket.OPEN) {
    droppedNoWs++;
    return;
  }
  const bytes = new Uint8Array(chunk.byteLength);
  chunk.copyTo(bytes);
  const fragCount = Math.max(1, Math.ceil(bytes.length / MAX_FRAG));
  const key = chunk.type === 'key';
  for (let i = 0; i < fragCount; i++) {
    const off = i * MAX_FRAG;
    const len = Math.min(MAX_FRAG, bytes.length - off);
    const pkt = new Uint8Array(HEADER + len);
    const dv = new DataView(pkt.buffer);
    pkt[0] = 0xc1;
    pkt[1] = 0x01; // video
    pkt[2] = codecId;
    pkt[3] = (key ? 0x01 : 0) | (i === fragCount - 1 ? 0x04 : 0);
    dv.setUint32(4, streamId, true);
    dv.setUint32(8, frameId, true);
    dv.setUint32(12, i, true);
    dv.setUint32(16, fragCount, true);
    dv.setBigUint64(20, BigInt(chunk.timestamp), true);
    dv.setUint32(28, len, true);
    pkt.set(bytes.subarray(off, off + len), HEADER);
    try {
      ws.send(pkt);
    } catch (e) {
      droppedNoWs++;
      log(`WS 发送失败: ${e.message}`, 'bad');
      return;
    }
    sentBytes += pkt.length;
  }
  sentFrames++;
  frameId = (frameId + 1) >>> 0;
}

// 8 次 ping/pong 取 RTT 最小的一次，offset = server_us - (client_us + rtt/2)
// 每 30 s 重估；漂移 > 5 ms 重锚。事件留痕目前只有页面日志——结构化事件流在 C++/M5 侧。
async function syncClock(reanchor) {
  // 整轮同步绑定发起时的 socket：cued 重启会把 ws 换成 null/新连接，
  // 若继续用全局 ws 移除监听器会抛 TypeError 并让这轮 await 永不返回。
  const sock = ws;
  if (!sock || sock.readyState !== WebSocket.OPEN) return;
  let best = null;
  for (let i = 0; i < 8; i++) {
    if (sock.readyState !== WebSocket.OPEN) break;
    const t = Math.round(performance.now() * 1000);
    const pong = await new Promise((resolve) => {
      let done = false;
      let timerId = null;
      function finish(v) {
        if (done) return;
        done = true;
        if (timerId !== null) clearTimeout(timerId);
        sock.removeEventListener('message', onMsg);
        resolve(v);
      }
      function onMsg(ev) {
        try {
          const j = JSON.parse(typeof ev.data === 'string' ? ev.data : '');
          if (j.op === 'pong' && j.t === t) finish(j);
        } catch (_) {}
      }
      sock.addEventListener('message', onMsg);
      timerId = setTimeout(() => finish(null), 200);
      try {
        sock.send(JSON.stringify({ op: 'ping', t }));
      } catch (_) {
        finish(null);
      }
    });
    if (pong) {
      const nowUs = Math.round(performance.now() * 1000);
      const rtt = nowUs - t;
      const off = Number(pong.server_us) - (t + rtt / 2);
      if (!best || rtt < best.rtt) best = { rtt, off };
    }
  }
  if (sock !== ws) return;  // 期间已重连，交给新连接的 syncClock(true)
  if (!best) {
    log('时钟同步失败，沿用上一个 offset（端到端延迟会含基准偏差）', 'warn');
    return;
  }
  if (reanchor || !clockAnchored) {
    offsetUs = best.off;
    clockAnchored = true;
    // 服务端单调钟与页面 timeOrigin 原点不同，offset 绝对值大是正常的，它是"时钟域差"而非误差
    log(`时钟锚定: rtt=${(best.rtt / 1000).toFixed(2)}ms offset=${(best.off / 1000).toFixed(2)}ms`, 'k');
    return;
  }
  const drift = best.off - offsetUs;
  if (Math.abs(drift) > CLOCK_DRIFT_US) {
    offsetUs = best.off;
    log(`时钟漂移 ${(drift / 1000).toFixed(2)}ms 超过 5ms，已重锚`, 'warn');
  }
}

// WS 连接 + 时钟同步；cued 重启（改参数/注入故障都会重启）后靠它续上
async function connectWs() {
  const sock = new WebSocket(WS_URL);
  sock.binaryType = 'arraybuffer';
  await new Promise((resolve, reject) => {
    const timerId = setTimeout(() => fail(new Error('WS 连接超时')), 3000);
    function fail(err) {
      clearTimeout(timerId);
      try { sock.close(); } catch (_) {}  // 超时/报错的 socket 不能留成半连接
      reject(err);
    }
    sock.onopen = () => { clearTimeout(timerId); resolve(); };
    sock.onerror = () => fail(new Error('WS 连接失败'));
  });
  ws = sock;
  // 重连后必须重新来一个 IDR：onclose 里置的 needKeyframe 会被断线期间那 400ms
  // 的帧消耗掉，而那时 ws 还是 null，IDR 直接丢进 droppedNoWs；等连上再置一次，
  // 才能保证新 cued 的解码器拿到的第一帧带 SPS/PPS。
  needKeyframe = true;
  ws.onclose = () => {
    if (ws !== sock) return;  // 旧连接的迟到事件不能把新连接打掉
    if (manualStop) return;
    log('WS 关闭（cued 可能重启），准备重连', 'warn');
    needKeyframe = true;
    ws = null;
    clockAnchored = false;
    scheduleReconnect();
  };
  ws.onerror = () => log('WS 错误', 'bad');
  log(`WS 已连接 ${WS_URL}`, 'k');
  await syncClock(true);
}

function scheduleReconnect() {
  if (manualStop || reconnecting || !streaming) return;
  reconnecting = true;
  setTimeout(async () => {
    reconnecting = false;
    if (manualStop || !streaming) return;
    try {
      await connectWs();
    } catch (_) {
      scheduleReconnect();
    }
  }, 400);
}

async function start() {
  const [w, h] = els.res.value.split('x').map(Number);
  const fps = Number(els.fps.value);
  const codec = pickCodec(w, h);
  if (!codec) { log('分辨率超出 L4.0 上限', 'bad'); return; }
  const newCodecId = codecIdOf(codec);
  if (!newCodecId) { log(`不支持的 codec: ${codec}`, 'bad'); return; }

  manualStop = false;
  // 先置位：若 connectWs 期间连接掉了，onclose 的 scheduleReconnect 才不会因 !streaming 静默退出
  streaming = true;
  encoderBroken = false;
  clockAnchored = false;

  els.canvas.width = w;
  els.canvas.height = h;
  els.start.disabled = true;

  try {
    await connectWs();
  } catch (e) {
    log(`WS 连接失败: ${e.message}`, 'bad');
    stop();
    return;
  }

  if (typeof VideoEncoder === 'undefined') {
    log('当前浏览器不支持 WebCodecs', 'bad');
    stop();
    return;
  }
  // 码率随像素率自适应：854x480@30 ≈ 1.2 Mbps，1080p60 ≈ 12 Mbps（M1 透传模式，带宽是主要约束）
  const bitrate = Math.min(12_000_000, Math.max(800_000, Math.round(w * h * fps * 0.1)));
  const support = await VideoEncoder.isConfigSupported({
    codec, width: w, height: h, bitrate, framerate: fps, latencyMode: 'realtime',
  });
  if (!support.supported) {
    log(`codec ${codec} 不被支持`, 'bad');
    stop();
    return;
  }

  codecId = newCodecId;
  encoder = new VideoEncoder({
    output: (chunk) => sendChunk(chunk),
    error: (e) => {
      encodeErrors++;
      encoderBroken = true;
      log(`编码器错误，停止推流: ${e.message}`, 'bad');
      stop();  // 编码器已 closed，继续 encode 只会每帧抛 InvalidStateError
    },
  });
  // annexb：让服务端 libavcodec 直接吃（默认的 AVCC 需要额外做 h264_mp4toannexb）
  encoder.configure({
    codec, width: w, height: h, bitrate, framerate: fps, latencyMode: 'realtime',
    avc: { format: 'annexb' },
  });
  log(`编码器就绪: ${codec} ${w}x${h}@${fps} ${(bitrate / 1e6).toFixed(1)} Mbps`, 'k');

  let i = 0;
  const periodMs = 1000 / fps;
  timer = setInterval(() => {
    if (encoderBroken || !encoder) return;
    drawFrame(i, w, h, fps);
    // pts 在服务端时钟域；offset 为负且页面刚加载时可能把时间戳推成负数，
    // setBigUint64 会按 2^64 回绕成天文数字，先夹到 0。
    let tsUs = Math.round(performance.now() * 1000 + offsetUs);
    if (!Number.isFinite(tsUs) || tsUs < 0) tsUs = 0;
    const frame = new VideoFrame(els.canvas, { timestamp: tsUs, duration: Math.round(1e6 / fps) });
    try {
      encoder.encode(frame, { keyFrame: needKeyframe || i % (fps * 2) === 0 });
      needKeyframe = false;
      i++;
    } catch (e) {
      encodeErrors++;
      encoderBroken = true;
      log(`编码失败，停止推流: ${e.message}`, 'bad');
      stop();
    } finally {
      frame.close();  // encode 抛错时也必须释放 VideoFrame，否则 GPU/CPU 缓冲泄漏
    }
  }, periodMs);

  statsTimer = setInterval(() => {
    if (!timer) return;
    log(`已发送 ${sentFrames} 帧 / ${(sentBytes / 1024 / 1024).toFixed(2)} MB，编码错误 ${encodeErrors}` +
        (droppedNoWs ? `，断线丢弃 ${droppedNoWs}` : ''));
  }, 2000);
  clockTimer = setInterval(() => { syncClock(false).catch(() => {}); }, CLOCK_RESYNC_MS);

  els.stop.disabled = false;
  log('开始推流');
}

function stop() {
  manualStop = true;
  streaming = false;
  clockAnchored = false;
  if (timer) { clearInterval(timer); timer = null; }
  if (statsTimer) { clearInterval(statsTimer); statsTimer = null; }
  if (clockTimer) { clearInterval(clockTimer); clockTimer = null; }
  if (encoder) { try { encoder.close(); } catch (_) {} encoder = null; }
  if (ws) { try { ws.close(); } catch (_) {} ws = null; }
  els.start.disabled = false;
  els.stop.disabled = true;
  log(`停止。本次共 ${sentFrames} 帧 / ${(sentBytes / 1024 / 1024).toFixed(2)} MB`);
}

// e2e 脚本（scripts/e2e-browser.cjs）用这个快照做断言，不需要读 DOM
window.__cueStats = () => ({
  wsUrl: WS_URL,
  sentFrames, sentBytes, encodeErrors, droppedNoWs, streaming, encoderBroken,
  offsetUs, clockAnchored,
});

els.start.addEventListener('click', () => start().catch((e) => { log(`启动失败: ${e.message}`, 'bad'); stop(); }));
els.stop.addEventListener('click', stop);
log(`就绪。点击「开始推流」——WS 目标 ${WS_URL}`);
