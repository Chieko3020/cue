// Cue M1 · 浏览器端：Canvas 合成源 → WebCodecs 编码 → WS 上行
// 协议见 SPEC §7.1（32 字节媒体头，小端）；时钟同步见 §7.1 的 offset 估算。

const WS_URL = `ws://${location.hostname}:9000/`;
const HEADER = 32;
const MAX_FRAG = 16 * 1024;

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
let offsetUs = 0;
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

function log(msg, cls) {
  const time = new Date().toISOString().slice(11, 23);
  const span = cls ? `<span class="${cls}">${msg}</span>` : msg;
  els.log.insertAdjacentHTML('beforeend', `${time}  ${span}\n`);
  els.log.scrollTop = els.log.scrollHeight;
}

// AVC level 必须与分辨率匹配，否则 configure() 直接抛异常（M0 实测：L3.0 上限 414720 coded area）
function pickCodec(w, h) {
  const area = w * h;
  if (area <= 414720) return 'avc1.42001E';   // L3.0
  if (area <= 2097152) return 'avc1.640028';  // L4.0
  return null;
}

function drawFrame(i, w, h) {
  const cv = els.canvas;
  const ctx = cv.getContext('2d', { alpha: false });
  const t = i / 30;
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
  ctx.fillText(`${w}x${h}`, 24, h * 0.38);
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
    pkt[2] = 0x01; // h264
    pkt[3] = (key ? 0x01 : 0) | (i === fragCount - 1 ? 0x04 : 0);
    dv.setUint32(4, streamId, true);
    dv.setUint32(8, frameId, true);
    dv.setUint32(12, i, true);
    dv.setUint32(16, fragCount, true);
    dv.setBigUint64(20, BigInt(chunk.timestamp), true);
    dv.setUint32(28, len, true);
    pkt.set(bytes.subarray(off, off + len), HEADER);
    ws.send(pkt);
    sentBytes += pkt.length;
  }
  sentFrames++;
  frameId = (frameId + 1) >>> 0;
}

// 8 次 ping/pong 取 RTT 最小的一次，offset = server_us - (client_us + rtt/2)
async function syncClock() {
  let best = null;
  for (let i = 0; i < 8; i++) {
    const t = Math.round(performance.now() * 1000);
    const pong = await new Promise((resolve) => {
      const onMsg = (ev) => {
        try {
          const j = JSON.parse(typeof ev.data === 'string' ? ev.data : '');
          if (j.op === 'pong' && j.t === t) {
            ws.removeEventListener('message', onMsg);
            resolve(j);
          }
        } catch (_) {}
      };
      ws.addEventListener('message', onMsg);
      ws.send(JSON.stringify({ op: 'ping', t }));
      setTimeout(() => { ws.removeEventListener('message', onMsg); resolve(null); }, 200);
    });
    if (pong) {
      const nowUs = Math.round(performance.now() * 1000);
      const rtt = nowUs - t;
      const off = Number(pong.server_us) - (t + rtt / 2);
      if (!best || rtt < best.rtt) best = { rtt, off };
    }
  }
  if (best) {
    offsetUs = best.off;
    log(`时钟同步: rtt=${(best.rtt / 1000).toFixed(2)}ms offset=${(best.off / 1000).toFixed(2)}ms`, 'k');
  } else {
    log('时钟同步失败，offset=0（端到端延迟会含基准偏差）', 'warn');
  }
}

// WS 连接 + 时钟同步；cued 重启（改参数/注入故障都会重启）后靠它续上
async function connectWs() {
  const sock = new WebSocket(WS_URL);
  sock.binaryType = 'arraybuffer';
  await new Promise((resolve, reject) => {
    sock.onopen = resolve;
    sock.onerror = () => reject(new Error('WS 连接失败'));
    setTimeout(() => reject(new Error('WS 连接超时')), 3000);
  });
  ws = sock;
  ws.onclose = () => {
    if (ws !== sock) return;  // 旧连接的迟到事件不能把新连接打掉
    if (manualStop) return;
    log('WS 关闭（cued 可能重启），准备重连', 'warn');
    needKeyframe = true;
    ws = null;
    scheduleReconnect();
  };
  ws.onerror = () => log('WS 错误', 'bad');
  log(`WS 已连接 ${WS_URL}`, 'k');
  await syncClock();
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

  els.canvas.width = w;
  els.canvas.height = h;
  els.start.disabled = true;

  manualStop = false;
  await connectWs();

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

  encoder = new VideoEncoder({
    output: (chunk) => sendChunk(chunk),
    error: (e) => { encodeErrors++; log(`编码错误: ${e.message}`, 'bad'); },
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
    drawFrame(i, w, h);
    const tsUs = Math.round(performance.now() * 1000 + offsetUs);
    const frame = new VideoFrame(els.canvas, { timestamp: tsUs, duration: Math.round(1e6 / fps) });
    encoder.encode(frame, { keyFrame: needKeyframe || i % (fps * 2) === 0 });
    needKeyframe = false;
    frame.close();
    i++;
  }, periodMs);

  els.stop.disabled = false;
  streaming = true;
  setInterval(() => {
    if (!timer) return;
    log(`已发送 ${sentFrames} 帧 / ${(sentBytes / 1024 / 1024).toFixed(2)} MB，编码错误 ${encodeErrors}` +
        (droppedNoWs ? `，断线丢弃 ${droppedNoWs}` : ''));
  }, 2000);
}

function stop() {
  manualStop = true;
  streaming = false;
  if (timer) { clearInterval(timer); timer = null; }
  if (encoder) { try { encoder.close(); } catch (_) {} encoder = null; }
  if (ws) { try { ws.close(); } catch (_) {} ws = null; }
  els.start.disabled = false;
  els.stop.disabled = true;
  log(`停止。本次共 ${sentFrames} 帧 / ${(sentBytes / 1024 / 1024).toFixed(2)} MB`);
}

els.start.addEventListener('click', () => start().catch((e) => { log(`启动失败: ${e.message}`, 'bad'); stop(); }));
els.stop.addEventListener('click', stop);
log('就绪。点击「开始推流」——需要 cued 已在 :9000 监听。');
