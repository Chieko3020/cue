#!/usr/bin/env node
// T-01 浏览器采集三路径的端到端验证（AC-1 的实测脚本）
//
// 服务器没有摄像头/声卡（/dev/snd 都不存在），所以「真设备」这一层只能靠 Chromium 的
// fake device 顶替：--use-fake-device-for-media-stream 会给 getUserMedia 提供
// 彩条视频 + 正弦波音频。**这不等于真摄像头路径已验证**——脚本会在这行里如实打印。
//
// 用法：
//   NODE_PATH=/tmp/pwtest/node_modules node scripts/e2e-capture.cjs [--seconds N]
// 依赖：build/cued、build/cue-probe（cmake --build build -j1 产出）
// 场景：
//   1) 摄像头（fake device）：getUserMedia 成功 → VideoEncoder 出帧 → cued frames_in 涨
//      → probe 5 秒内出端到端分位；同时音频（Opus）包数在涨
//   2) 权限被拒：不带 --use-fake-ui-for-media-stream 的浏览器 → NotAllowedError
//      → 页面回退合成源且仍在推流
//   3) 降级：?codec=vp8 测试钩子 → 头部 codec 字段如实写 VP8，收端照常收帧
const { chromium } = require('playwright');
const { spawn } = require('node:child_process');
const dgram = require('node:dgram');
const net = require('node:net');
const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');

const ROOT = path.resolve(__dirname, '..');
const BUILD_DIR = process.env.BUILD_DIR || 'build';
const CUED = path.join(ROOT, BUILD_DIR, 'cued');
const PROBE = path.join(ROOT, BUILD_DIR, 'cue-probe');
const WEB_DIR = path.join(ROOT, 'web');
const SECONDS = Number(process.env.E2E_SECONDS || 8);
const E2E_DEADLINE_MS = 5000;  // AC-1 原句：5 秒内显示端到端指标

// ---------------------------------------------------------------- 小工具
function freeTcpPort() {
  return new Promise((resolve, reject) => {
    const s = net.createServer();
    s.listen(0, '127.0.0.1', () => {
      const p = s.address().port;
      s.close(() => resolve(p));
    });
    s.on('error', reject);
  });
}

function freeUdpPort() {
  return new Promise((resolve, reject) => {
    const s = dgram.createSocket('udp4');
    s.bind(0, '127.0.0.1', () => {
      const p = s.address().port;
      s.close(() => resolve(p));
    });
    s.on('error', reject);
  });
}

// 静态服务器直接服务 web/：cue-serve 的默认目录是 web/dist（React 控制台），
// 这里要的是推流页本身，所以不走 cue-serve，免得跟 dist 混淆。
function startStaticServer() {
  const mime = {
    '.html': 'text/html; charset=utf-8',
    '.js': 'text/javascript; charset=utf-8',
    '.css': 'text/css; charset=utf-8',
    '.json': 'application/json; charset=utf-8',
    '.png': 'image/png',
    '.svg': 'image/svg+xml',
  };
  const server = http.createServer((req, res) => {
    const url = new URL(req.url, 'http://127.0.0.1');
    const rel = url.pathname === '/' ? '/index.html' : url.pathname;
    const file = path.join(WEB_DIR, path.normalize(rel).replace(/^(\.\.[/\\])+/, ''));
    fs.readFile(file, (err, data) => {
      if (err) {
        res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' });
        res.end('not found');
        return;
      }
      res.writeHead(200, { 'Content-Type': mime[path.extname(file)] || 'application/octet-stream' });
      res.end(data);
    });
  });
  return new Promise((resolve) => {
    server.listen(0, '127.0.0.1', () => resolve({ server, port: server.address().port }));
  });
}

// 起子进程并逐行收集 stdout/stderr（cued 的 METRICS 行与 [cue-event] 行都在 stdout）
function startProc(bin, argv, label) {
  const proc = spawn(bin, argv, { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] });
  const state = { proc, lines: [], metrics: [], events: [], stderr: [], maxMetrics: null };
  let buf = '';
  const feed = (chunk) => {
    buf += chunk.toString('utf8');
    let nl;
    while ((nl = buf.indexOf('\n')) >= 0) {
      const line = buf.slice(0, nl);
      buf = buf.slice(nl + 1);
      state.lines.push(line);
      if (line.startsWith('METRICS ')) {
        try {
          const m = JSON.parse(line.slice('METRICS '.length));
          state.metrics.push({ at: Date.now(), m });
          state.maxMetrics = m;
        } catch (_) {}
      } else if (line.startsWith('[cue-event] ')) {
        try {
          state.events.push({ at: Date.now(), ev: JSON.parse(line.slice('[cue-event] '.length)) });
        } catch (_) {}
      }
    }
    if (state.lines.length > 4000) state.lines.splice(0, 2000);
  };
  proc.stdout.on('data', feed);
  proc.stderr.on('data', (c) => {
    feed(c);
    const s = c.toString('utf8');
    state.stderr.push(s);
    if (state.stderr.length > 400) state.stderr.splice(0, 200);
  });
  proc.on('exit', (code) => state.exit = code);
  return state;
}

function killProc(state) {
  if (!state || !state.proc || state.proc.killed) return;
  try { state.proc.kill('SIGKILL'); } catch (_) {}
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ---------------------------------------------------------------- 页面操作
async function openPage(browser, url, initScript) {
  const context = await browser.newContext({ permissions: [] });
  const page = await context.newPage();
  // 兜底路径验证：在页面脚本执行前把 MediaStreamTrackProcessor 藏起来，
  // 逼 app.js 走 <video> + requestVideoFrameCallback 那条路（不然这条分支永远不会被跑到）
  if (initScript) await page.addInitScript(initScript);
  const consoleLogs = [];
  const pageErrors = [];
  const logLines = [];
  page.on('console', (m) => consoleLogs.push(`[console] ${m.text()}`));
  page.on('pageerror', (e) => pageErrors.push(`[pageerror] ${e.message}`));
  await page.goto(url, { waitUntil: 'domcontentloaded' });
  await page.waitForTimeout(300);
  return { context, page, consoleLogs, pageErrors, logLines };
}

async function pageStats(page) {
  return page.evaluate(() => (window.__cueStats ? window.__cueStats() : null));
}

async function waitFor(pred, { timeout = 15000, interval = 100, what = '条件' } = {}) {
  const t0 = Date.now();
  for (;;) {
    let v = null;
    try { v = await pred(); } catch (_) {}
    if (v) return { value: v, elapsed: Date.now() - t0 };
    if (Date.now() - t0 > timeout) throw new Error(`等待超时（${timeout}ms）：${what}`);
    await sleep(interval);
  }
}

async function pageLogText(page) {
  return page.evaluate(() => (document.getElementById('log') || {}).textContent || '');
}

// ---------------------------------------------------------------- 单个场景
async function runStreamScenario({ label, source, query, expect, launch, initScript }) {
  const scenarioStart = Date.now();
  const wsPort = await freeTcpPort();
  const udpPort = await freeUdpPort();
  const { server, port: httpPort } = await startStaticServer();
  const probe = startProc(PROBE, ['--port', String(udpPort), '--bind', '127.0.0.1'], 'probe');
  await sleep(200);  // probe 先绑好 UDP，cued 才能把包发出去
  const cued = startProc(CUED, ['--ws-port', String(wsPort), '--bind', '127.0.0.1',
                                '--probe-host', '127.0.0.1', '--probe-port', String(udpPort)], 'cued');
  await sleep(400);

  const launchOpts = { headless: true, args: launch.args };
  if (launch.channel) launchOpts.channel = launch.channel;  // 不传时走 Playwright 默认的 headless shell
  const browser = await chromium.launch(launchOpts);
  const url = `http://127.0.0.1:${httpPort}/?ws_port=${wsPort}${query || ''}`;
  const { context, page, pageErrors } = await openPage(browser, url, initScript);

  const result = { label, url, failures: [], info: {} };
  const fail = (msg) => result.failures.push(msg);
  let streamStart = 0;
  let firstLat = null;

  try {
    if (source) await page.selectOption('#src', source);
    const before = Date.now();
    await page.click('#start');

    // 1) 页面开始真正发帧（fake device 下 getUserMedia 是同步给流的）
    const sent = await waitFor(async () => {
      const s = await pageStats(page);
      return s && s.sentFrames > 0 ? s : null;
    }, { timeout: 20000, what: `${label}：页面开始发帧` });
    streamStart = Date.now();
    result.info.startupMs = streamStart - before;
    const stats = sent.value;
    result.info.stats = stats;

    // 2) AC-1：probe 在 5 秒内出现端到端指标
    try {
      const hit = await waitFor(() => {
        for (const { at, ev } of probe.events) {
          if (at < streamStart) continue;
          if (ev.ev === 'probe.metric' && ev.data && ev.data.lat_p50 !== null && ev.data.lat_p50 !== undefined) {
            return { ev: ev.data, at };
          }
        }
        return null;
      }, { timeout: E2E_DEADLINE_MS, what: 'probe 端到端分位' });
      firstLat = hit.value.ev;
      result.info.firstLatencyMs = hit.elapsed;
      result.info.lat = { p50: firstLat.lat_p50, p95: firstLat.lat_p95, p99: firstLat.lat_p99 };
    } catch (e) {
      fail(`AC-1 未满足：probe 在 ${E2E_DEADLINE_MS}ms 内没有给出端到端分位（${e.message}）`);
    }

    // 3) 让它再跑几秒，收干净数字
    await sleep(Math.max(1000, SECONDS * 1000 - (Date.now() - streamStart)));

    const finalStats = await pageStats(page);
    result.info.finalStats = finalStats;
    const pageLog = await pageLogText(page);
    result.info.pageLogTail = pageLog.split('\n').slice(-12).join('\n');

    // 4) 采集侧断言
    if (expect === 'denied' || expect === 'nodevice') {
      const wantErr = expect === 'denied' ? 'NotAllowedError' : 'NotFoundError';
      if (finalStats.source !== 'synthetic') fail(`${wantErr} 后应在合成源，实际 source=${finalStats.source}`);
      if (!(finalStats.sentFrames > 0)) fail(`${wantErr} 后没有继续推流（sentFrames=0）`);
      if (!/回退到合成源/.test(pageLog)) fail('页面日志里没有「回退到合成源」的明确提示');
      if (!pageLog.includes(wantErr)) fail(`页面日志没有区分出 ${wantErr}（应把错误名给用户看）`);
    } else {
      if (finalStats.source !== source) fail(`输入源应为 ${source}，实际 ${finalStats.source}`);
      if (!['mstp', 'rvfc'].includes(finalStats.capturePath)) {
        fail(`采集路径应为 mstp 或 rvfc，实际 ${finalStats.capturePath}`);
      }
      if (!(finalStats.videoFrames > 0)) fail(`VideoEncoder 没有出帧（videoFrames=${finalStats.videoFrames}）`);
      if (finalStats.encodeErrors !== 0) fail(`encodeErrors=${finalStats.encodeErrors}`);
      if (!finalStats.codec) fail('页面没有报告实际 codec');
      if (expect === 'rvfc' && finalStats.capturePath !== 'rvfc') {
        fail(`藏掉 MSTP 后应走 rVFC，实际 ${finalStats.capturePath}`);
      }
      if (expect === 'rvfc') {
        // 音频兜底（AudioWorklet 版）没实现：这里必须看到显式原因，而不是"没声音也不知道为什么"
        if (finalStats.audio.enabled) fail('MSTP 不可用时音频不应启用（本批没有 AudioWorklet 兜底）');
        if (!/MediaStreamTrackProcessor/.test(finalStats.audio.note || '')) {
          fail(`音频未启用但备注没说明原因：${finalStats.audio.note}`);
        }
      }
      if (expect === 'vp8') {
        // 降级分支必须让头部 codec 字段跟着真实编码器走，否则 probe 会按 H264 解 VP8 字节
        if (finalStats.codec !== 'vp8' || finalStats.codecId !== 2) {
          fail(`降级后应为 vp8/0x02，实际 ${finalStats.codec}/${finalStats.codecId}`);
        }
        const deg = cued.events.find((x) => x.ev.ev === 'device.codec_degraded');
        if (!deg) fail('cued 没有收到降级事件（device.codec_degraded）');
      } else if (!finalStats.codec.startsWith('avc1')) {
        fail(`默认路径应走 H.264，实际 ${finalStats.codec}`);
      }
    }

    // 5) 收端断言：cued 的 METRICS 里 frames_in 在涨 + 能力上报到达 + 音频计数
    const m0 = cued.metrics.find((x) => x.at >= streamStart);
    const mLast = cued.metrics.length ? cued.metrics[cued.metrics.length - 1] : null;
    result.info.cued = mLast ? mLast.m : null;
    if (!mLast) {
      fail('cued 没有输出任何 METRICS 行');
    } else {
      if (!(mLast.m.frames_in > 0)) fail(`cued frames_in=${mLast.m.frames_in}，没有收到视频帧`);
      if (m0 && !(mLast.m.frames_in > m0.m.frames_in)) fail('cued frames_in 没有在增长');
      if (typeof mLast.m.audio_frames_in !== 'number') fail('cued METRICS 缺少 audio_frames_in 字段');
      // 音频真正开始编码时会补一条 device.audio_started（带原生格式），
      // 这是「音频不是只在页面上写着，而是真编码出血了包」的收端证据
      const audioEv = cued.events.find((x) => x.ev.ev === 'device.audio_started');
      if (audioEv) {
        result.info.audioNative = audioEv.ev.data && audioEv.ev.data.native;
        if (!audioEv.ev.data.native) fail('device.audio_started 里没有采集侧原生格式');
      }
      if (expect === 'camera') {
        if (!(mLast.m.audio_frames_in > 0)) {
          fail(`摄像头模式下 cued 没收音频包（audio_frames_in=${mLast.m.audio_frames_in}，` +
               `页面音频备注：${finalStats.audio.note || '无'}）`);
        }
        if (mLast.m.audio_decoded !== false) fail('audio_decoded 应显式为 false（本批只计数不解码）');
      }
    }
    // 能力上报在首帧之前就发出，所以按「场景开始时刻」过滤，而不是按首帧时刻
    const capsEv = cued.events.find((x) => x.ev.ev === 'device.caps' && x.at >= scenarioStart);
    if (!capsEv) {
      fail('cued 没有收到设备能力上报（device.caps 事件）');
    } else {
      result.info.caps = capsEv.ev.data;
      const d = capsEv.ev.data || {};
      if (!d.ua) fail('能力上报里没有 UA');
      if (!d.devices || typeof d.devices.total !== 'number') fail('能力上报里没有设备数');
      if (!d.video || !d.audio) fail('能力上报里缺少 video/audio 的 isConfigSupported 结果');
    }

    const probeLast = probe.events.filter((x) => x.ev.ev === 'probe.metric').pop();
    result.info.probe = probeLast ? probeLast.ev.data : null;

    if (pageErrors.length) fail(`页面异常：${pageErrors.join(' | ')}`);
  } finally {
    await context.close().catch(() => {});
    await browser.close().catch(() => {});
    server.close();
    killProc(cued);
    killProc(probe);
  }
  return result;
}

// ---------------------------------------------------------------- main
(async () => {
  if (!fs.existsSync(CUED) || !fs.existsSync(PROBE)) {
    console.error(`缺少二进制：${CUED} 或 ${PROBE}\n先构建：cmake --build build -j1`);
    process.exit(2);
  }
  // headless shell（Playwright 默认）+ fake UI：getUserMedia 直接给流，走「有设备且有权限」的正常路径
  const shellArgs = ['--no-sandbox', '--use-fake-device-for-media-stream',
                     '--use-fake-ui-for-media-stream', '--autoplay-policy=no-user-gesture-required'];
  // 权限拒绝分支必须用「完整 Chromium + 不给 fake UI」：headless shell 在没有 fake UI 时
  // 抛的是 NotSupportedError（它压根没实现权限询问），只有完整 Chromium 才会正常回
  // NotAllowedError「Permission denied」——这正是真实用户点「阻止」时的形状。
  const denyArgs = ['--no-sandbox', '--use-fake-device-for-media-stream'];
  // 「没有摄像头设备」：什么设备 flag 都不给，让 getUserMedia 抛 NotFoundError
  const noDevArgs = ['--no-sandbox'];
  const results = [];
  results.push(await runStreamScenario({
    label: '摄像头（fake device）', source: 'camera', expect: 'camera',
    launch: { args: shellArgs },
  }));
  results.push(await runStreamScenario({
    label: '权限被拒 → 回退合成源', source: 'camera', expect: 'denied',
    launch: { channel: 'chromium', args: denyArgs },
  }));
  results.push(await runStreamScenario({
    label: '无摄像头设备 → 回退合成源', source: 'camera', expect: 'nodevice',
    launch: { channel: 'chromium', args: noDevArgs },
  }));
  results.push(await runStreamScenario({
    label: 'H.264 → VP8 降级', source: 'camera', query: '&codec=vp8', expect: 'vp8',
    launch: { args: shellArgs },
  }));
  // MSTP 不可用时的兜底路径：把 MediaStreamTrackProcessor 藏掉，逼走 rVFC。
  // 这条分支如果只是"写了"，就永远没人跑到；藏掉 API 是最接近真实环境的验证方式
  // （Firefox 至今没有 MSTP，用户在那儿遇到的就是这条路）。
  results.push(await runStreamScenario({
    label: 'MSTP 不可用 → rVFC 兜底', source: 'camera', expect: 'rvfc',
    launch: { args: shellArgs },
    initScript: () => {
      // 直接删会留下不可配置的占位，改为覆盖成 undefined（app.js 用 typeof 判断）
      Object.defineProperty(window, 'MediaStreamTrackProcessor', {
        configurable: true, writable: true, value: undefined,
      });
    },
  }));

  let failed = 0;
  for (const r of results) {
    const ok = r.failures.length === 0;
    if (!ok) failed++;
    console.log(`\n===== ${ok ? 'PASS' : 'FAIL'} · ${r.label} =====`);
    const s = r.info.finalStats || {};
    console.log(`  页面: source=${s.source} path=${s.capturePath} ${s.resolution} codec=${s.codec}` +
                ` 视频=${s.videoFrames}帧 音频=${s.audio ? s.audio.frames : '?'}包` +
                ` encodeErrors=${s.encodeErrors}`);
    const c = r.info.cued || {};
    console.log(`  cued: frames_in=${c.frames_in} out=${c.frames_out} audio_frames_in=${c.audio_frames_in}` +
                ` audio_decoded=${c.audio_decoded} reject=${c.rejected}` +
                (r.info.audioNative ? ` 音频原生格式=${r.info.audioNative}` : ''));
    const p = r.info.probe || {};
    console.log(`  probe: fps=${p.fps} frames=${p.frames} decoded=${p.decoded}` +
                ` e2e p50=${p.lat_p50}ms p99=${p.lat_p99}ms` +
                ` (首个端到端样本 ${r.info.firstLatencyMs}ms)`);
    console.log(`  caps: ${r.info.caps ? JSON.stringify({ devices: r.info.caps.devices,
      video: r.info.caps.video && { h264: r.info.caps.video.h264, vp8: r.info.caps.video.vp8,
                                    chosen: r.info.caps.video.chosen },
      audio: r.info.caps.audio }) : '（未收到）'}`);
    if (r.failures.length) {
      for (const f of r.failures) console.log(`  ✗ ${f}`);
      console.log('  --- 页面日志尾部 ---');
      console.log('  ' + String(r.info.pageLogTail || '').replace(/\n/g, '\n  '));
    }
  }
  console.log(`\n说明：以上摄像头/麦克风路径用的是 Chromium fake device（服务器无摄像头、无声卡），` +
              `真摄像头/真麦克风路径未经真机验证。`);
  if (failed) {
    console.error(`\nE2E FAIL: ${failed}/${results.length} 个场景失败`);
    process.exit(1);
  }
  console.log(`\nE2E PASS: ${results.length}/${results.length} 个场景通过`);
})().catch((e) => {
  console.error('E2E FAIL:', e && e.stack ? e.stack : e);
  process.exit(1);
});
