// Cue · 浏览器端推流页：摄像头 / 屏幕 / Canvas 合成源 → WebCodecs 编码 → WS 上行
// 协议见 SPEC §7.1（32 字节媒体头，小端）；时钟同步见 §7.1 的 offset 估算。
// WS 地址默认 ws(s)://<本页主机>:9000/，可用 ?ws=ws://host:port/ 覆盖，?ws_port=N 只换端口。
// ?codec=vp8 是测试钩子：假装 H.264 不被支持，用来验证降级分支（真实降级不该只靠运气撞上）。

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

// SPEC §7.1 的 type / codec 字段（wire.h 的 MediaType / Codec）。
// 这里的值必须与**实际发出的字节**一致：收端（cued）把 codec 原样填进下行数据报头，
// probe 靠它决定「解码」还是「跳过」——谎报 H264 而发 VP8 字节，probe 只会一直报解码错误。
const TYPE_VIDEO = 0x01;
const TYPE_AUDIO = 0x02;
const TYPE_STATS = 0x05;  // 上行控制包：能力/降级上报，载荷是 UTF-8 JSON（cued 侧只落事件）
const CODEC_H264 = 0x01;
const CODEC_VP8 = 0x02;
const CODEC_OPUS = 0x03;

// 音频单独占一个 stream_id：它与视频的帧号节奏不同（Opus 20ms 一包 = 50 包/秒，视频 30 帧/秒），
// 共用一个号段会让日志/事件里「第几帧」无法与另一条流对齐；分开是零成本的。
const VIDEO_STREAM_ID = 1;
const AUDIO_STREAM_ID = 2;

const forceVp8 = new URLSearchParams(location.search).get('codec') === 'vp8';

const SOURCE_CAMERA = 'camera';
const SOURCE_SCREEN = 'screen';
const SOURCE_SYNTH = 'synthetic';
const SOURCE_LABELS = { camera: '摄像头', screen: '屏幕', synthetic: '合成源' };

const els = {
  start: document.getElementById('start'),
  stop: document.getElementById('stop'),
  res: document.getElementById('res'),
  fps: document.getElementById('fps'),
  bitrate: document.getElementById('bitrate'),
  gop: document.getElementById('gop'),
  src: document.getElementById('src'),
  canvas: document.getElementById('cv'),
  preview: document.getElementById('preview'),
  log: document.getElementById('log'),
  statSrc: document.getElementById('stat-src'),
  statPath: document.getElementById('stat-path'),
  statRes: document.getElementById('stat-res'),
  statCodec: document.getElementById('stat-codec'),
  statErr: document.getElementById('stat-err'),
  statAudio: document.getElementById('stat-audio'),
};

let ws = null;
let encoder = null;
let timer = null;          // 合成源的绘制定时器
let statsTimer = null;     // 页面日志/状态刷新
let clockTimer = null;
let offsetUs = 0;
let clockAnchored = false;
let frameId = 0;
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

// ---- 采集侧状态（T-01）----
let currentSource = SOURCE_SYNTH;   // 实际在用的输入源（回退后会变，UI 选择框跟着改）
let capturePath = 'canvas';         // canvas | mstp | rvfc —— 面板上如实显示走了哪条路径
let mediaStream = null;
let videoTrack = null;
let audioTrack = null;
let videoReader = null;             // MediaStreamTrackProcessor 的 reader（mstp 路径）
let audioReader = null;
let actualW = 0;                    // 实际采集尺寸：AVC level 必须按它选，不能用选择框的值
let actualH = 0;
let fpsNow = 30;
let bitrateBps = 0;
let gopSec = 2;
let gopFrames = 60;
let codecName = '';                 // 实际生效的 codec 字符串（avc1.42001E / vp8）
let codecId = CODEC_H264;           // 写进上行头里的 codec 字段
let frameIdx = 0;
let encodedFrames = 0;
let measuredFps = 0;
// 编码输出里的 timestamp 来自输入帧（采集轨的时间基），与 performance.now 原点不同；
// 这里按 timestamp → 上行 pts 建一张表，让每一帧在**被送入编码器那一刻**打时间戳。
// 不直接给 VideoFrame 换时间戳：那要复制一整帧（720p 约 1.4 MB/帧），纯浪费。
const videoPts = new Map();

// ---- 音频侧状态（FR-DEV.6）----
let audioEncoder = null;
let audioFrameId = 0;
let audioSentFrames = 0;
let audioBaseUs = null;   // 轨道时间基 → performance.now 的标定差值
let audioNote = '';       // 音频没启用的原因：显式暴露在面板与能力上报里，不静默
let audioNative = '';     // 采集到的原始音频格式（写进日志/能力上报，便于判断是谁的问题）
// 20 ms 打包缓冲：见 startAudio 里的说明（编码器要求输入与 frameDuration 精确匹配）
let pcmChunks = [];
let pcmCount = 0;
let pcmTsUs = null;
const AUDIO_RATE = 48000;
const AUDIO_FRAME_MS = 20;
const AUDIO_FRAME_SAMPLES = (AUDIO_RATE * AUDIO_FRAME_MS) / 1000;  // 960
let capsSent = null;      // 最近一次能力上报的载荷，重连后原样重发

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

function codecIdOf(codec) {
  if (!codec) return 0;
  if (codec.startsWith('avc1')) return CODEC_H264;
  if (codec.startsWith('vp8')) return CODEC_VP8;
  return 0;
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

// pts 在服务端时钟域；offset 为负且页面刚加载时可能把时间戳推成负数，
// setBigUint64 会按 2^64 回绕成天文数字，先夹到 0。
function nowPtsUs() {
  let ts = Math.round(performance.now() * 1000 + offsetUs);
  if (!Number.isFinite(ts) || ts < 0) ts = 0;
  return ts;
}

// 上行媒体包：视频与音频共用同一套封装，只有头里的 type/codec/stream_id 不同。
// 断线期间编码不停，但包发不出去：计数后丢弃，重连后继续（否则会崩在 ws.send 上）。
function sendMediaPacket(meta, bytes) {
  if (!ws || ws.readyState !== WebSocket.OPEN) {
    droppedNoWs++;
    return false;
  }
  const fragCount = Math.max(1, Math.ceil(bytes.length / MAX_FRAG));
  for (let i = 0; i < fragCount; i++) {
    const off = i * MAX_FRAG;
    const len = Math.min(MAX_FRAG, bytes.length - off);
    const pkt = new Uint8Array(HEADER + len);
    const dv = new DataView(pkt.buffer);
    pkt[0] = 0xc1;
    pkt[1] = meta.type;
    pkt[2] = meta.codec;
    pkt[3] = (meta.keyframe ? 0x01 : 0) | (i === fragCount - 1 ? 0x04 : 0);
    dv.setUint32(4, meta.streamId, true);
    dv.setUint32(8, meta.frameId, true);
    dv.setUint32(12, i, true);
    dv.setUint32(16, fragCount, true);
    dv.setBigUint64(20, BigInt(meta.ptsUs), true);
    dv.setUint32(28, len, true);
    pkt.set(bytes.subarray(off, off + len), HEADER);
    try {
      ws.send(pkt);
    } catch (e) {
      droppedNoWs++;
      log(`WS 发送失败: ${e.message}`, 'bad');
      return false;
    }
    sentBytes += pkt.length;
  }
  return true;
}

// 能力/降级上报走 0x05 控制包：wire.h 的 MediaType::Stats 早留了位、上行一直没人用，
// 复用不必改协议；载荷自描述 JSON，收端不认识新键也不会坏。frag_count 写 1：
// cued 对控制包用 validate_control 解（放宽分片语义），但写规范值更便于用 Wireshark 看。
function sendControlJson(kind, obj) {
  if (!ws || ws.readyState !== WebSocket.OPEN) return false;
  const bytes = new TextEncoder().encode(JSON.stringify({ kind, ...obj }));
  return sendMediaPacket(
    { type: TYPE_STATS, codec: 0, streamId: VIDEO_STREAM_ID, frameId: 0, keyframe: false, ptsUs: nowPtsUs() },
    bytes,
  );
}

function onVideoChunk(chunk) {
  const bytes = new Uint8Array(chunk.byteLength);
  chunk.copyTo(bytes);
  // 送进编码器时记的那个时刻才是「采集"生产"这一帧」的时刻；在这里取 now 会把编码器
  // 内部延迟算进 pts，端到端延迟就会被系统性低估。
  let ptsUs = videoPts.get(chunk.timestamp);
  videoPts.delete(chunk.timestamp);
  if (ptsUs === undefined) ptsUs = nowPtsUs();
  const ok = sendMediaPacket(
    { type: TYPE_VIDEO, codec: codecId, streamId: VIDEO_STREAM_ID, frameId,
      keyframe: chunk.type === 'key', ptsUs },
    bytes,
  );
  if (!ok) return;
  sentFrames++;
  frameId = (frameId + 1) >>> 0;
}

function onAudioChunk(chunk) {
  const bytes = new Uint8Array(chunk.byteLength);
  chunk.copyTo(bytes);
  // 音频 pts 同样要落在服务端时钟域：轨道时间基 + 标定差值 + 当前 offset。
  // 不按「输出时刻」打戳——那样会把编码器延迟和调度抖动混进 20ms 的节奏里。
  let ptsUs = 0;
  if (audioBaseUs !== null) {
    ptsUs = Math.round(audioBaseUs + chunk.timestamp + offsetUs);
    if (!Number.isFinite(ptsUs) || ptsUs < 0) ptsUs = 0;
  } else {
    ptsUs = nowPtsUs();
  }
  const ok = sendMediaPacket(
    { type: TYPE_AUDIO, codec: CODEC_OPUS, streamId: AUDIO_STREAM_ID, frameId: audioFrameId,
      keyframe: true, ptsUs },
    bytes,
  );
  if (!ok) return;
  audioFrameId = (audioFrameId + 1) >>> 0;
  audioSentFrames++;
  // 第一个音频包发出去时补一条带真实采集格式的上报：能力上报发生在采集循环刚起步、
  // 还不知道轨道格式（audioNative 常常还是空）的时候，这里补上才算把音频链路说清楚。
  if (audioSentFrames === 1) {
    sendControlJson('audio.started', {
      native: audioNative,
      sampleRate: AUDIO_RATE,
      channels: 1,
      frameMs: AUDIO_FRAME_MS,
      bitrate: 48000,
      packets: 1,
    });
  }
}

// 三条采集路径（canvas / mstp / rvfc）唯一汇合点：GOP 判定、pts 记录、错误计数都在这里，
// 各路径只管「怎么拿到一个 VideoFrame」。所有权归本函数：无论成功失败都会 close。
function submitFrame(frame, ptsUs) {
  try {
    if (encoderBroken || !encoder) return;
    videoPts.set(frame.timestamp, ptsUs);
    encoder.encode(frame, { keyFrame: needKeyframe || frameIdx % gopFrames === 0 });
    needKeyframe = false;
    frameIdx++;
    encodedFrames++;
  } catch (e) {
    encodeErrors++;
    encoderBroken = true;
    log(`编码失败，停止推流: ${e.message}`, 'bad');
    stop();
  } finally {
    try { frame.close(); } catch (_) {}  // encode 抛错时也必须释放，否则 GPU/CPU 缓冲泄漏
  }
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
  // 能力上报跟着连接走：cued 重启后内存里的上一份没了，重连时必须补报一次
  if (capsSent) sendControlJson('caps', capsSent);
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

// ---------------------------------------------------------------- 采集：三类输入源
// 权限/设备错误必须分类到用户能行动的一句话（去点允许 / 插设备 / 关掉占用它的程序），
// 统一一句 "无法获取媒体" 等于没说。
function describeMediaError(kind, err) {
  const name = (err && err.name) || 'UnknownError';
  const label = SOURCE_LABELS[kind] || kind;
  if (name === 'NotAllowedError' || name === 'SecurityError') {
    return `${label}权限被拒绝（${name}）：请点地址栏的摄像头/屏幕图标重新允许，或检查页面是否在安全上下文（https / localhost）`;
  }
  if (name === 'NotFoundError' || name === 'DevicesNotFoundError' || name === 'OverconstrainedError') {
    return `未找到可用的${label}设备（${name}）：请确认设备已连接且未被禁用`;
  }
  if (name === 'NotReadableError' || name === 'TrackStartError' || name === 'AbortError') {
    return `${label}无法读取（${name}）：设备可能被其他程序占用（会议软件/另一个标签页）`;
  }
  return `获取${label}失败（${name}）: ${(err && err.message) || '无更多信息'}`;
}

async function acquireStream(kind, want) {
  if (!navigator.mediaDevices) {
    throw new Error('navigator.mediaDevices 不存在（非安全上下文或浏览器太旧）');
  }
  if (kind === SOURCE_SCREEN) {
    // 屏幕共享通常不带音频，协议上也不从屏幕取音频（见任务约束）
    return navigator.mediaDevices.getDisplayMedia({
      video: { frameRate: { ideal: want.fps } },
      audio: false,
    });
  }
  // 关闭 AGC/降噪/回声消除：这三个是**处理**，会把音频变成"已经改过的信号"，
  // 端到端链路测的是传输与编解码，采集侧再叠加处理就分不清延迟/失真归谁。
  // 声道固定单声道、48 kHz：Opus 的推荐采样率，也便于收端按 20 ms 帧切。
  return navigator.mediaDevices.getUserMedia({
    video: {
      width: { ideal: want.w },
      height: { ideal: want.h },
      frameRate: { ideal: want.fps },
    },
    audio: {
      channelCount: { ideal: 1 },
      sampleRate: { ideal: 48000 },
      echoCancellation: false,
      noiseSuppression: false,
      autoGainControl: false,
    },
  });
}

function showPreview(stream) {
  if (!els.preview) return;
  els.preview.srcObject = stream;
  els.preview.muted = true;
  els.preview.play().catch(() => {});  // play() 被自动播放策略拒了不影响编码路径
}

// 采集失败/中断统一走这里：把选择框拨回合成源、明确告知原因，然后继续推流。
// 「整页卡死」是最差的失败模式——用户看不到画面也看不出原因，只能怀疑代码坏了。
function fallbackToSynthetic(reason) {
  log(`${reason}；已自动回退到合成源继续推流`, 'bad');
  els.src.value = SOURCE_SYNTH;
  currentSource = SOURCE_SYNTH;
  releaseMedia();
  // 选择框是**程序**拨回去的，不会触发 change 事件：不主动同步的话预览区会停在
  // 一个已经断流的 <video> 上，用户看到黑屏却以为页面卡住了。
  syncPreviewVisibility();
}

// 合成源显示 canvas，采集源显示 <video> 预览
function syncPreviewVisibility() {
  const synth = els.src.value === SOURCE_SYNTH;
  els.canvas.style.display = synth ? 'block' : 'none';
  els.preview.style.display = synth ? 'none' : 'block';
  if (synth) els.preview.srcObject = null;
}

function releaseMedia() {
  if (videoReader) { try { videoReader.cancel(); } catch (_) {} videoReader = null; }
  if (audioReader) { try { audioReader.cancel(); } catch (_) {} audioReader = null; }
  if (videoTrack) { try { videoTrack.onended = null; videoTrack.stop(); } catch (_) {} videoTrack = null; }
  if (audioTrack) { try { audioTrack.onended = null; audioTrack.stop(); } catch (_) {} audioTrack = null; }
  if (audioEncoder) { try { audioEncoder.close(); } catch (_) {} audioEncoder = null; }
  if (mediaStream) {
    for (const t of mediaStream.getTracks()) { try { t.stop(); } catch (_) {} }
    mediaStream = null;
  }
  if (els.preview) els.preview.srcObject = null;
}

// 返回 true 表示拿到了采集流（或本来就是合成源），false 表示已回退。
async function setupSource(want) {
  const kind = els.src.value;
  if (kind === SOURCE_SYNTH) {
    currentSource = SOURCE_SYNTH;
    capturePath = 'canvas';
    actualW = want.w;
    actualH = want.h;
    fpsNow = want.fps;
    return true;
  }
  currentSource = kind;
  let stream = null;
  try {
    stream = await acquireStream(kind, want);
  } catch (e) {
    fallbackToSynthetic(describeMediaError(kind, e));
    actualW = want.w;
    actualH = want.h;
    fpsNow = want.fps;
    return false;
  }
  mediaStream = stream;
  videoTrack = stream.getVideoTracks()[0] || null;
  audioTrack = stream.getAudioTracks()[0] || null;
  if (!videoTrack) {
    fallbackToSynthetic(`${SOURCE_LABELS[kind]}流里没有视频轨`);
    actualW = want.w;
    actualH = want.h;
    fpsNow = want.fps;
    return false;
  }
  // 用户在系统层面"停止共享"/拔掉摄像头时轨道会结束：此时必须回退而不是空转
  videoTrack.onended = () => {
    if (!streaming) return;
    fallbackToSynthetic('视频轨已结束（用户停止共享或设备断开）');
    // 重启一次让合成源接上：stop() 后 start() 会读选择框（已被拨回 synthetic）
    stop();
    setTimeout(() => { start().catch((e) => log(`重启失败: ${e.message}`, 'bad')); }, 50);
  };
  // 实际尺寸由设备决定（屏幕共享尤其不能假定选择框的值）：AVC level 必须按真实尺寸选，
  // 用错 level 时 configure() 直接抛异常。
  const s = videoTrack.getSettings ? videoTrack.getSettings() : {};
  actualW = Number(s.width) || want.w;
  actualH = Number(s.height) || want.h;
  fpsNow = Number(s.frameRate) ? Math.round(Number(s.frameRate)) : want.fps;
  showPreview(stream);
  log(`已获得${SOURCE_LABELS[kind]}：${actualW}x${actualH}@${fpsNow}（音轨 ${audioTrack ? '有' : '无'}）`, 'k');
  return true;
}

// 采集路径 A：MediaStreamTrackProcessor（Chrome/Edge）。原生 VideoFrame 流，
// 不必经过并不可靠的 <video> 解码回环，也不会因为视频元素渲染节流而丢帧。
async function startTrackProcessor(track) {
  const proc = new MediaStreamTrackProcessor({ track });
  const reader = proc.readable.getReader();
  videoReader = reader;
  capturePath = 'mstp';
  for (;;) {
    let res;
    try {
      res = await reader.read();
    } catch (e) {
      if (streaming) log(`读采集帧失败: ${e.message}`, 'bad');
      break;
    }
    const { value, done } = res;
    if (done) break;
    if (!value) continue;
    if (!streaming || encoderBroken) { try { value.close(); } catch (_) {} continue; }
    // pts 用「帧到达脚本」的时刻：这比真实采集时刻晚（少算采集到脚本的几毫秒），
    // 所以端到端延迟读数偏乐观；换用轨道时间基需要标定两张时钟，本批先不做。
    submitFrame(value, nowPtsUs());
  }
}

// 采集路径 B：<video> + requestVideoFrameCallback（MSTP 不可用时的兜底）。
// rVFC 只在真有新帧时回调，不会像定时器那样把同一帧重复编码。
async function startRvfcLoop() {
  const v = els.preview;
  if (!v) return;
  if (typeof v.requestVideoFrameCallback !== 'function') {
    log('该浏览器既没有 MediaStreamTrackProcessor 也没有 requestVideoFrameCallback，无法采集', 'bad');
    return;
  }
  capturePath = 'rvfc';
  const step = () => {
    if (!streaming) return;
    if (!encoderBroken && encoder) {
      const tsUs = nowPtsUs();
      let frame = null;
      try {
        frame = new VideoFrame(v, { timestamp: tsUs, duration: Math.round(1e6 / fpsNow) });
      } catch (e) {
        encodeErrors++;
        log(`从视频元素取帧失败: ${e.message}`, 'bad');
      }
      if (frame) submitFrame(frame, tsUs);
    }
    v.requestVideoFrameCallback(step);
  };
  v.requestVideoFrameCallback(step);
}

// 采集路径 C：Canvas 合成源（确定性画面：时间码/帧号水印，方便肉眼校验帧连续性）
function startCanvasLoop(w, h, fps) {
  capturePath = 'canvas';
  currentSource = SOURCE_SYNTH;
  els.canvas.width = w;
  els.canvas.height = h;
  let i = 0;
  timer = setInterval(() => {
    if (encoderBroken || !encoder) return;
    drawFrame(i, w, h, fps);
    const tsUs = nowPtsUs();
    const frame = new VideoFrame(els.canvas, { timestamp: tsUs, duration: Math.round(1e6 / fps) });
    submitFrame(frame, tsUs);
    i++;
  }, 1000 / fps);
}

// ---------------------------------------------------------------- 视频编码器
function videoConfig(codec, w, h, fps, bitrate) {
  return { codec, width: w, height: h, bitrate, framerate: fps, latencyMode: 'realtime' };
}

// 降级链：H.264（按尺寸选 level）→ VP8。为什么必须真降级而不是报错退出：
// 有的环境（容器里没有 H.264 硬编、某些 Linux 发行版的 Chrome 构建）只有 VP8，
// 而 VP8 走同一条 libavcodec 下行，probe 也认 codec 字段，功能等价。
async function chooseVideoCodec(w, h, fps) {
  const wanted = forceVp8 ? null : pickCodec(w, h);
  const tried = [];
  const candidates = [];
  if (wanted) candidates.push(wanted);
  candidates.push('vp8');
  for (const codec of candidates) {
    let supported = false;
    let reason = '';
    try {
      const s = await VideoEncoder.isConfigSupported(videoConfig(codec, w, h, fps, bitrateBps));
      supported = !!(s && s.supported);
    } catch (e) {
      reason = e.message;
    }
    tried.push({ codec, supported, reason });
    if (supported) return { codec, tried, degraded: codec !== wanted };
  }
  return { codec: null, tried, degraded: false };
}

// ---------------------------------------------------------------- 音频（FR-DEV.6）
// 采集到的 AudioData 一律先转成 48 kHz 单声道 f32，再按 20 ms 重新打包。
// 为什么不能直接把采集块喂给编码器：配置里写了 opus.frameDuration=20000 时，
// Chrome 要求每个输入缓冲区时长精确等于它，而采集侧给的是 10 ms 一块，直接喂会抛
// "Input audio buffer is incompatible with codec parameters"（实测踩到）。
// 帧长交给采集侧去凑，等于把 20 ms 这个约定交给浏览器，不如自己做确定。
function audioToMono(ad) {
  const n = ad.numberOfFrames;
  const ch = ad.numberOfChannels;
  const acc = new Float32Array(n);
  const planar = ad.format.endsWith('-planar');
  for (let c = 0; c < ch; c++) {
    if (planar) {
      const tmp = new Float32Array(n);
      // 指定 format=f32-planar：s16/u8 等格式由 copyTo 就地转换（规格允许）
      ad.copyTo(tmp, { planeIndex: c, format: 'f32-planar' });
      for (let i = 0; i < n; i++) acc[i] += tmp[i] / ch;
    } else {
      const inter = new Float32Array(n * ch);
      ad.copyTo(inter, { planeIndex: 0, format: 'f32' });
      for (let i = 0; i < n; i++) acc[i] += inter[i * ch + c] / ch;
    }
  }
  if (ad.sampleRate === AUDIO_RATE) return acc;
  // 真机麦克风常见 44.1 kHz：线性插值到 48 kHz。这条链路测的是传输与打包，
  // 重采样音质不在范围内；但不能因为采样率不符就静默丢音频。
  const ratio = ad.sampleRate / AUDIO_RATE;
  const outLen = Math.max(1, Math.round(n / ratio));
  const out = new Float32Array(outLen);
  for (let i = 0; i < outLen; i++) {
    const x = i * ratio;
    const i0 = Math.min(n - 1, Math.floor(x));
    const i1 = Math.min(n - 1, i0 + 1);
    const f = x - i0;
    out[i] = acc[i0] * (1 - f) + acc[i1] * f;
  }
  return out;
}

function pushPcm(samples, tsUs) {
  if (pcmTsUs === null) pcmTsUs = tsUs;
  pcmChunks.push(samples);
  pcmCount += samples.length;
  while (pcmCount >= AUDIO_FRAME_SAMPLES && audioEncoder) {
    const frame = new Float32Array(AUDIO_FRAME_SAMPLES);
    let filled = 0;
    while (filled < AUDIO_FRAME_SAMPLES) {
      const head = pcmChunks[0];
      const take = Math.min(head.length, AUDIO_FRAME_SAMPLES - filled);
      frame.set(head.subarray(0, take), filled);
      filled += take;
      if (take === head.length) pcmChunks.shift();
      else pcmChunks[0] = head.subarray(take);
    }
    pcmCount -= AUDIO_FRAME_SAMPLES;
    const ad = new AudioData({
      format: 'f32-planar',
      sampleRate: AUDIO_RATE,
      numberOfFrames: AUDIO_FRAME_SAMPLES,
      numberOfChannels: 1,
      timestamp: pcmTsUs,
      data: frame,
    });
    pcmTsUs += (AUDIO_FRAME_SAMPLES * 1e6) / AUDIO_RATE;  // 20000 µs：保持 20 ms 节奏
    try {
      audioEncoder.encode(ad);
    } catch (e) {
      audioNote = `AudioEncoder.encode 失败: ${e.message}`;
      log(`音频编码失败，音频停止（视频继续）: ${e.message}`, 'bad');
      try { audioEncoder.close(); } catch (_) {}
      audioEncoder = null;
    } finally {
      try { ad.close(); } catch (_) {}
    }
  }
}

// 摄像头模式才做音频；屏幕共享不带音频，合成源没有音频。
// 失败一律写进 audioNote 并继续推视频——「音频没起来」不能靠用户猜。
async function startAudio() {
  if (!audioTrack) {
    audioNote = currentSource === SOURCE_SCREEN ? '屏幕共享不带音轨' : '输入源没有音轨';
    log(`音频未启用：${audioNote}（继续只推视频）`, 'warn');
    return;
  }
  if (typeof AudioEncoder === 'undefined') {
    audioNote = 'AudioEncoder 不可用（浏览器不支持 WebCodecs 音频）';
    log(`音频未启用：${audioNote}`, 'warn');
    return;
  }
  const cfg = {
    codec: 'opus',
    sampleRate: AUDIO_RATE,
    numberOfChannels: 1,
    bitrate: 48000,
    // 帧长 20 ms：Opus 的标准打包粒度，收端按 20 ms 一包做抖动缓冲最省事。
    // 注意它与「输入缓冲区必须正好 20 ms」是绑定的，所以上面自己做重打包。
    opus: { frameDuration: AUDIO_FRAME_MS * 1000 },
  };
  let support = null;
  try {
    support = await AudioEncoder.isConfigSupported(cfg);
  } catch (e) {
    audioNote = `AudioEncoder.isConfigSupported 抛异常: ${e.message}`;
    log(`音频未启用：${audioNote}`, 'warn');
    return;
  }
  if (!support || !support.supported) {
    audioNote = 'AudioEncoder.isConfigSupported({codec:"opus",48000,1ch}) = false';
    log(`音频未启用：${audioNote}（继续只推视频）`, 'warn');
    return;
  }
  if (typeof MediaStreamTrackProcessor === 'undefined') {
    // 音频的兜底需要 AudioWorklet（再写一遍重采样/打包），本批不做；显式说明而不是静默丢
    audioNote = 'MediaStreamTrackProcessor 不可用，音频采集无兜底路径（未实现 AudioWorklet 版本）';
    log(`音频未启用：${audioNote}`, 'warn');
    return;
  }
  audioEncoder = new AudioEncoder({
    output: (chunk) => onAudioChunk(chunk),
    error: (e) => {
      audioNote = `AudioEncoder 错误: ${e.message}`;
      log(`音频编码器错误，音频停止（视频继续）: ${e.message}`, 'bad');
      try { audioEncoder.close(); } catch (_) {}
      audioEncoder = null;
    },
  });
  audioEncoder.configure(cfg);
  const proc = new MediaStreamTrackProcessor({ track: audioTrack });
  const reader = proc.readable.getReader();
  audioReader = reader;
  log(`音频编码器就绪: opus 48kHz mono 20ms 48kbps`, 'k');
  (async () => {
    let loggedNative = false;
    for (;;) {
      let res;
      try {
        res = await reader.read();
      } catch (e) {
        if (streaming) log(`读音频帧失败: ${e.message}`, 'bad');
        break;
      }
      const { value, done } = res;
      if (done) break;
      if (!value) continue;
      if (!streaming || !audioEncoder) { try { value.close(); } catch (_) {} continue; }
      try {
        if (!loggedNative) {
          loggedNative = true;
          audioNative = `${value.format} ${value.sampleRate}Hz ${value.numberOfChannels}ch`;
          log(`音频采集: ${audioNative} → 重打包 ${AUDIO_FRAME_MS}ms mono ${AUDIO_RATE}Hz`);
          if (audioBaseUs === null) audioBaseUs = performance.now() * 1000 - value.timestamp;
        }
        pushPcm(audioToMono(value), value.timestamp);
      } catch (e) {
        audioNote = `音频格式转换失败: ${e.message}`;
        log(`音频格式转换失败，音频停止（视频继续）: ${e.message}`, 'bad');
        try { audioEncoder.close(); } catch (_) {}
        audioEncoder = null;
      } finally {
        try { value.close(); } catch (_) {}
      }
    }
  })();
}

// ---------------------------------------------------------------- 能力上报（FR-DEV.4 最小版）
async function buildCaps(videoSupport, audioSupported) {
  let devices = { total: 0, videoinput: 0, audioinput: 0 };
  try {
    const list = await navigator.mediaDevices.enumerateDevices();
    devices.total = list.length;  // 未授权时 label 为空但条目数仍是真实设备数
    for (const d of list) {
      if (d.kind === 'videoinput') devices.videoinput++;
      else if (d.kind === 'audioinput') devices.audioinput++;
    }
  } catch (_) {}
  return {
    ua: navigator.userAgent,
    secure_context: !!window.isSecureContext,
    source: currentSource,
    capture_path: capturePath,
    devices,
    video: {
      h264: videoSupport.some((x) => x.codec.startsWith('avc1') && x.supported),
      vp8: videoSupport.some((x) => x.codec.startsWith('vp8') && x.supported),
      chosen: codecName,
      degraded: codecName !== '' && !codecName.startsWith('avc1'),
      tried: videoSupport,
    },
    audio: {
      opus: !!audioSupported,
      note: audioNote,
      // 采集侧原始格式：opus 支不支持是一回事，「拿到的 AudioData 长什么样」是另一回事
      input: audioNative || null,
    },
    feature: {
      // 浏览器到底有没有这些 API：收端据此判断「能力上限」是环境限制还是实现缺失
      media_stream_track_processor: typeof MediaStreamTrackProcessor !== 'undefined',
      request_video_frame_callback:
        typeof HTMLVideoElement !== 'undefined' &&
        typeof HTMLVideoElement.prototype.requestVideoFrameCallback === 'function',
      get_user_media: !!(navigator.mediaDevices && navigator.mediaDevices.getUserMedia),
      get_display_media: !!(navigator.mediaDevices && navigator.mediaDevices.getDisplayMedia),
    },
    display: { width: actualW, height: actualH, fps: fpsNow, bitrate: bitrateBps, gop_sec: gopSec },
  };
}

function readOptions() {
  const [w, h] = els.res.value.split('x').map(Number);
  const fps = Number(els.fps.value);
  const bitrateSel = els.bitrate ? els.bitrate.value : 'auto';
  // 码率随像素率自适应：854x480@30 ≈ 1.2 Mbps，1080p60 ≈ 12 Mbps（M1 透传模式，带宽是主要约束）
  const auto = Math.min(12_000_000, Math.max(800_000, Math.round(w * h * fps * 0.1)));
  const bitrate = bitrateSel === 'auto' ? auto : Number(bitrateSel) * 1_000_000;
  const gop = els.gop ? Number(els.gop.value) : 2;
  return { w, h, fps, bitrate, gop };
}

async function start() {
  const want = readOptions();
  if (!pickCodec(want.w, want.h) && want.source !== SOURCE_CAMERA && want.source !== SOURCE_SCREEN) {
    log('分辨率超出 L4.0 上限', 'bad');
    return;
  }

  manualStop = false;
  // 先置位：若 connectWs 期间连接掉了，onclose 的 scheduleReconnect 才不会因 !streaming 静默退出
  streaming = true;
  encoderBroken = false;
  clockAnchored = false;
  audioNote = '';
  audioBaseUs = null;
  audioNative = '';
  pcmChunks = [];
  pcmCount = 0;
  pcmTsUs = null;
  videoPts.clear();   // 上一轮的 timestamp→pts 映射不能带进新会话
  capsSent = null;
  frameIdx = 0;
  encodedFrames = 0;
  measuredFps = 0;
  els.start.disabled = true;

  // 采集必须在点击的同一轮用户激活里发起：getDisplayMedia 明确要求 transient activation，
  // 先 await 一次网络往返再调它，Chrome 会以 NotAllowedError 拒掉。
  const acquired = await setupSource(want);
  actualW = actualW || want.w;
  actualH = actualH || want.h;
  fpsNow = fpsNow || want.fps;
  bitrateBps = want.bitrate;
  gopSec = want.gop;

  if (typeof VideoEncoder === 'undefined') {
    log('当前浏览器不支持 WebCodecs', 'bad');
    stop();
    return;
  }
  // AVC level 按**实际采集尺寸**选：屏幕共享/摄像头的真实尺寸可以不同于选择框
  const levelCodec = pickCodec(actualW, actualH);
  if (!levelCodec && !forceVp8) {
    log(`采集尺寸 ${actualW}x${actualH} 超出 L4.0 上限（${Math.round(actualW * actualH)} > 2097152 coded area）：` +
        '请选择更小的窗口/分辨率后再试', 'bad');
    if (acquired) {
      fallbackToSynthetic('采集尺寸超出可编码上限');
      actualW = want.w;
      actualH = want.h;
      fpsNow = want.fps;
    }
  }
  gopFrames = Math.max(1, Math.round(fpsNow * gopSec));

  try {
    await connectWs();
  } catch (e) {
    log(`WS 连接失败: ${e.message}`, 'bad');
    stop();
    return;
  }

  const chosen = await chooseVideoCodec(actualW, actualH, fpsNow);
  if (!chosen.codec) {
    log(`没有可用的视频编码器（试过 ${chosen.tried.map((t) => t.codec).join(', ')}），停止推流`, 'bad');
    stop();
    return;
  }
  codecName = chosen.codec;
  codecId = codecIdOf(codecName);
  if (!codecId) {
    log(`不认识的 codec: ${codecName}`, 'bad');
    stop();
    return;
  }
  if (chosen.degraded) {
    const why = forceVp8
      ? '测试钩子 ?codec=vp8 要求'
      : `H.264 ${levelCodec || '(尺寸超 L4.0)'} 的 isConfigSupported 返回 false`;
    log(`已降级到 VP8：${why}（上行 codec 字段如实写 0x02，不再谎报 H264）`, 'warn');
    sendControlJson('codec.degraded', { from: levelCodec, to: codecName, reason: why });
  }

  encoder = new VideoEncoder({
    output: (chunk) => onVideoChunk(chunk),
    error: (e) => {
      encodeErrors++;
      encoderBroken = true;
      log(`编码器错误，停止推流: ${e.message}`, 'bad');
      stop();  // 编码器已 closed，继续 encode 只会每帧抛 InvalidStateError
    },
  });
  // annexb：让服务端 libavcodec 直接吃（默认的 AVCC 需要额外做 h264_mp4toannexb）。
  // VP8 没有参数集，avc 选项对它无效，只在 H264 时带。
  const cfg = videoConfig(codecName, actualW, actualH, fpsNow, bitrateBps);
  if (codecId === CODEC_H264) cfg.avc = { format: 'annexb' };
  try {
    encoder.configure(cfg);
  } catch (e) {
    // isConfigSupported 说支持却 configure 失败，通常是 level 与尺寸的硬约束被撞上
    // （M0 实测过：AVC level 的 coded area 上限）。这里必须说清是哪个参数，否则只剩一句"启动失败"。
    log(`编码器 configure 失败（${codecName} ${actualW}x${actualH}@${fpsNow}）：${e.message}`, 'bad');
    stop();
    return;
  }
  log(`编码器就绪: ${codecName} ${actualW}x${actualH}@${fpsNow} ${(bitrateBps / 1e6).toFixed(1)} Mbps gop=${gopSec}s`,
      'k');

  await startAudio();

  // 能力上报：把 isConfigSupported 结果 / 设备数 / UA / 实际采集参数发给 cued（事件流可见）
  try {
    let opusOk = false;
    if (typeof AudioEncoder !== 'undefined') {
      try {
        opusOk = !!(await AudioEncoder.isConfigSupported({
          codec: 'opus', sampleRate: 48000, numberOfChannels: 1, bitrate: 48000,
        })).supported;
      } catch (_) {}
    }
    capsSent = await buildCaps(chosen.tried, opusOk);
    if (sendControlJson('caps', capsSent)) log('设备能力已上报 cued（device.caps）', 'k');
  } catch (e) {
    log(`能力上报失败（不影响推流）: ${e.message}`, 'warn');
  }

  // 帧循环：按实际可用的采集路径选，三条路径最终都进 submitFrame()
  if (currentSource === SOURCE_SYNTH || !videoTrack) {
    startCanvasLoop(want.w, want.h, want.fps);
  } else if (typeof MediaStreamTrackProcessor !== 'undefined') {
    startTrackProcessor(videoTrack).catch((e) => {
      if (streaming) log(`采集循环异常退出: ${e.message}`, 'bad');
    });
  } else {
    await startRvfcLoop();
  }

  statsTimer = setInterval(() => {
    updateStatus(performance.now());
    log(`已发送 视频 ${sentFrames} 帧 / ${(sentBytes / 1024 / 1024).toFixed(2)} MB` +
        `，音频 ${audioSentFrames} 包，编码错误 ${encodeErrors}` +
        (droppedNoWs ? `，断线丢弃 ${droppedNoWs}` : '') +
        (audioEncoder ? '' : `（音频未启用：${audioNote || '无音轨'}）`));
  }, 2000);
  clockTimer = setInterval(() => { syncClock(false).catch(() => {}); }, CLOCK_RESYNC_MS);

  els.stop.disabled = false;
  updateStatus(performance.now(), true);
  log(`开始推流（输入源：${SOURCE_LABELS[currentSource] || currentSource}，采集路径：${capturePath}）`);
}

let lastStatMs = 0;
let lastStatFrames = 0;
function updateStatus(now, reset) {
  if (reset || lastStatMs === 0) {
    lastStatMs = now;
    lastStatFrames = encodedFrames;
  } else {
    const dt = (now - lastStatMs) / 1000;
    if (dt >= 0.5) {
      measuredFps = (encodedFrames - lastStatFrames) / dt;
      lastStatMs = now;
      lastStatFrames = encodedFrames;
    }
  }
  if (els.statSrc) els.statSrc.textContent = SOURCE_LABELS[currentSource] || currentSource;
  if (els.statPath) els.statPath.textContent = capturePath;
  if (els.statRes) {
    els.statRes.textContent = actualW
      ? `${actualW}×${actualH} @${measuredFps > 0 ? measuredFps.toFixed(1) : fpsNow} fps`
      : '-';
  }
  if (els.statCodec) els.statCodec.textContent = codecName ? `${codecName} (0x${codecId.toString(16)})` : '-';
  if (els.statErr) els.statErr.textContent = String(encodeErrors);
  if (els.statAudio) {
    els.statAudio.textContent = audioEncoder
      ? `Opus 48k mono 20ms，已发 ${audioSentFrames} 包`
      : `未启用：${audioNote || '输入源无音轨'}`;
  }
}

function stop() {
  manualStop = true;
  streaming = false;
  clockAnchored = false;
  videoPts.clear();
  pcmChunks = [];
  pcmCount = 0;
  pcmTsUs = null;
  if (timer) { clearInterval(timer); timer = null; }
  if (statsTimer) { clearInterval(statsTimer); statsTimer = null; }
  if (clockTimer) { clearInterval(clockTimer); clockTimer = null; }
  releaseMedia();  // 不释放的话摄像头指示灯常亮、麦克风一直占用
  if (encoder) { try { encoder.close(); } catch (_) {} encoder = null; }
  if (ws) { try { ws.close(); } catch (_) {} ws = null; }
  els.start.disabled = false;
  els.stop.disabled = true;
  log(`停止。本次共 视频 ${sentFrames} 帧 / ${(sentBytes / 1024 / 1024).toFixed(2)} MB，音频 ${audioSentFrames} 包`);
}

// e2e 脚本（scripts/e2e-capture.cjs / e2e-browser.cjs）用这个快照做断言，不需要读 DOM
window.__cueStats = () => ({
  wsUrl: WS_URL,
  sentFrames, sentBytes, encodeErrors, droppedNoWs, streaming, encoderBroken,
  offsetUs, clockAnchored,
  // 采集侧（T-01）：断言「真的用了摄像头」「真的降级了」「真的回退了」都靠这些字段
  source: currentSource,
  sourceLabel: SOURCE_LABELS[currentSource] || currentSource,
  capturePath,
  resolution: actualW ? `${actualW}x${actualH}` : '',
  actualWidth: actualW,
  actualHeight: actualH,
  fps: fpsNow,
  measuredFps,
  codec: codecName,
  codecId,
  bitrate: bitrateBps,
  gopSec,
  videoFrames: encodedFrames,
  audio: {
    enabled: !!audioEncoder,
    codec: 'opus',
    sampleRate: 48000,
    channels: 1,
    frameMs: 20,
    frames: audioSentFrames,
    note: audioNote,
  },
  caps: capsSent,
});

els.start.addEventListener('click', () => start().catch((e) => { log(`启动失败: ${e.message}`, 'bad'); stop(); }));
els.stop.addEventListener('click', stop);
els.src.addEventListener('change', syncPreviewVisibility);
log(`就绪。点击「开始推流」——WS 目标 ${WS_URL}`);
if (forceVp8) log('测试钩子 ?codec=vp8 已生效：将假装 H.264 不被支持，走降级分支', 'warn');
