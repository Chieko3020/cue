#!/usr/bin/env node
// T-23（FR-UI.1）设备页的端到端验证：真实 cued 会话表 → /api/state → 控制台设备页。
//
// 用法：
//   NODE_PATH=/tmp/pwtest/node_modules node scripts/e2e-devices.cjs
//
// 环境变量（默认值与 run.sh / e2e-console.cjs 的端口都不冲突）：
//   BUILD_DIR=build  CONSOLE_PORT=8299  WS_PORT=9200  PROBE_PORT=9201
//   SHOT_DIR=/tmp/cue-e2e-devices  KEEP_RUNNING=1（不清理进程）
//
// 断言（全部实测，不是"跑通即过"）：
//   1. /api/state 的 devices 里有 2 台设备，device_id / peer / lease_ms 都有值，
//      devices_summary.live == 2 且 lease_ms == 5000（cued 真的用了 --lease-ms 5000）
//   2. 设备页出现 2 个 data-testid="device-row"，每行的租约剩余是"数字 + s"
//   3. 杀掉一路 capture → /api/state 的 live 降到 1 → 刷新页面后 device-row 只剩 1 个，
//      被断开的那台出现在「最近断开」里（cued 的 session.list 带 include_closed）
//
// 为什么用包装脚本给 cued 传参：cue-serve 托管 cued 的启动（控制 socket 路径按 pid 推导），
// 但还没暴露 --max-sessions/--lease-ms 两个开关。包装脚本让 cued 真的带着这两个参数启动，
// 比"改默认值"或"绕过 cue-serve 自己起 cued"都更接近实际部署形态。
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

const ROOT = path.resolve(__dirname, '..');
const BUILD = process.env.BUILD_DIR || 'build';
const PORT = Number(process.env.CONSOLE_PORT || 8299);
const WS_PORT = Number(process.env.WS_PORT || 9200);
const PROBE_PORT = Number(process.env.PROBE_PORT || 9201);
const BASE = `http://127.0.0.1:${PORT}`;
const SHOT_DIR = process.env.SHOT_DIR || '/tmp/cue-e2e-devices';
const KEEP = process.env.KEEP_RUNNING === '1';
const MAX_SESSIONS = 2;
const LEASE_MS = 5000;

const children = [];
const failures = [];

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

function ok(msg) {
  console.log(`[e2e-devices] ok: ${msg}`);
}

function fail(msg) {
  failures.push(msg);
  console.error(`[e2e-devices] FAIL: ${msg}`);
}

function haveBin(p) {
  try {
    fs.accessSync(p, fs.constants.X_OK);
    return true;
  } catch {
    return false;
  }
}

function start(name, cmd, args) {
  const child = spawn(cmd, args, { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] });
  const tail = [];
  const push = (b, s) => {
    tail.push(`[${s}] ${b.toString()}`);
    if (tail.length > 100) tail.shift();
  };
  child.stdout.on('data', (b) => push(b, 'out'));
  child.stderr.on('data', (b) => push(b, 'err'));
  child.on('error', (e) => fail(`${name} 进程错误：${e.message}`));
  child.exited = new Promise((r) => child.on('exit', (code, sig) => r({ code, sig })));
  children.push({ name, child, tail });
  return child;
}

async function getState() {
  const r = await fetch(`${BASE}/api/state`, { cache: 'no-store' });
  return await r.json();
}

async function waitFor(fn, timeoutMs, what) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      const v = await fn();
      if (v) return v;
    } catch {
      /* 还没就绪 */
    }
    await sleep(200);
  }
  fail(`等待超时：${what}`);
  return null;
}

async function waitHealth(timeoutMs = 15000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      const r = await fetch(`${BASE}/api/health`, { cache: 'no-store' });
      if (r.ok) return await r.json();
    } catch {
      /* 还没起来 */
    }
    await sleep(200);
  }
  return null;
}

function liveDevices(st) {
  return (st.devices || []).filter((d) => d.state === 'live');
}

async function shutdown() {
  for (const { child } of children.slice().reverse()) {
    if (child.exitCode === null) {
      child.kill('SIGTERM');
      await Promise.race([child.exited, sleep(3000)]);
      if (child.exitCode === null) child.kill('SIGKILL');
    }
  }
}

(async () => {
  // ---------------------------------------------------------------- 0. 前置
  const serveBin = path.join(ROOT, BUILD, 'cue-serve');
  const cuedBin = path.join(ROOT, BUILD, 'cued');
  const captureBin = path.join(ROOT, BUILD, 'cue-capture');
  for (const bin of [serveBin, cuedBin, captureBin]) {
    if (!haveBin(bin)) {
      console.error(`[e2e-devices] 缺少 ${bin}\n先构建：cmake -S . -B ${BUILD} -DCMAKE_CXX_COMPILER=g++-14 && cmake --build ${BUILD} -j1`);
      process.exit(2);
    }
  }
  if (!fs.existsSync(path.join(ROOT, 'web', 'dist', 'index.html'))) {
    console.error('[e2e-devices] web/dist 不存在，先构建控制台：cd web/console && npm run build');
    process.exit(2);
  }
  let chromium;
  try {
    ({ chromium } = require('playwright'));
  } catch (e) {
    console.error(`[e2e-devices] 无法加载 playwright（NODE_PATH 要指向含它的 node_modules）：${e.message}`);
    process.exit(2);
  }
  fs.mkdirSync(SHOT_DIR, { recursive: true });

  // cued 的启动参数包装：cue-serve 之后追加的 --ws-port/--probe-port 等原样透传。
  const wrapper = '/tmp/cue-e2e-cued.sh';
  fs.writeFileSync(
    wrapper,
    `#!/bin/sh\nexec "${cuedBin}" --max-sessions ${MAX_SESSIONS} --lease-ms ${LEASE_MS} "$@"\n`,
    { mode: 0o755 },
  );

  // ---------------------------------------------------------------- 1. 起链路
  start('cue-serve', serveBin, [
    '--port', String(PORT),
    '--ws-port', String(WS_PORT),
    '--probe-port', String(PROBE_PORT),
    '--cued', wrapper,
    '--web', 'web/dist',
    '--runs', 'runs',
    '--approval', 'ask',
  ]);
  const health = await waitHealth();
  if (!health || !health.ok) {
    console.error('[e2e-devices] cue-serve 没起来');
    for (const { name, tail } of children) console.error(`--- ${name} ---\n${tail.join('')}`);
    process.exit(2);
  }
  ok(`cue-serve 就绪 ${BASE}`);

  const started = await waitFor(async () => {
    const s = await getState();
    return (s.events || []).some((e) => e.ev === 'pipeline.started') ? s : null;
  }, 20000, 'cued 报告 pipeline.started（WS 端口已监听）');
  if (!started) {
    for (const { name, tail } of children) console.error(`--- ${name} ---\n${tail.join('')}`);
    await shutdown();
    process.exit(1);
  }
  ok(`cued 已监听 ws://127.0.0.1:${WS_PORT}/（max-sessions=${MAX_SESSIONS} lease=${LEASE_MS}ms）`);

  // ---------------------------------------------------------------- 2. 两路设备接入
  const wsUrl = `ws://127.0.0.1:${WS_PORT}/`;
  const capA = start('capture-640x360', captureBin, [
    '--backend', 'lavfi', '--ws', wsUrl, '--size', '640x360', '--fps', '30', '--audio', 'off',
  ]);
  const capB = start('capture-1280x720', captureBin, [
    '--backend', 'lavfi', '--ws', wsUrl, '--size', '1280x720', '--fps', '30', '--audio', 'off',
  ]);

  const two = await waitFor(async () => {
    const s = await getState();
    return liveDevices(s).length === 2 ? s : null;
  }, 40000, '/api/state 的 devices 出现 2 台 live 设备');
  if (!two) {
    for (const { name, tail } of children) console.error(`--- ${name} ---\n${tail.join('')}`);
    await shutdown();
    process.exit(1);
  }

  // ---- 断言 1：/api/state 的设备字段有真实值
  console.log('[e2e-devices] /api/state devices（live）:');
  console.log(JSON.stringify(liveDevices(two).map((d) => ({
    session_id: d.session_id,
    device_id: d.device_id,
    peer: d.peer,
    state: d.state,
    lease_ms: d.lease_ms,
    expires_in_ms: d.expires_in_ms,
    caps_declared: d.caps && d.caps.declared,
  })), null, 2));
  console.log(`[e2e-devices] /api/state devices_summary = ${JSON.stringify(two.devices_summary)}`);
  if (two.devices_error) fail(`/api/state 带 devices_error（设备表取不到）：${two.devices_error}`);
  for (const d of liveDevices(two)) {
    if (!d.device_id) fail(`设备 ${d.session_id} 没有 device_id`);
    if (!d.peer || !/:\d+$/.test(d.peer)) fail(`设备 ${d.session_id} 的 peer 不是 ip:port（${d.peer}）`);
    if (d.lease_ms !== LEASE_MS) fail(`设备 ${d.session_id} 的 lease_ms=${d.lease_ms}，期望 ${LEASE_MS}`);
  }
  const summary = two.devices_summary || {};
  if (summary.live !== 2) fail(`devices_summary.live=${summary.live}，期望 2`);
  if (summary.lease_ms !== LEASE_MS) fail(`devices_summary.lease_ms=${summary.lease_ms}，期望 ${LEASE_MS}`);
  if (failures.length === 0) ok('断言 1：devices 里 2 台设备，device_id/peer/lease_ms 都有值');

  // ---------------------------------------------------------------- 3. 浏览器设备页
  const browser = await chromium.launch({ args: ['--no-sandbox'] });
  const page = await browser.newPage({ viewport: { width: 1360, height: 900 } });
  const pageErrors = [];
  page.on('pageerror', (e) => pageErrors.push(String(e && e.message ? e.message : e)));
  await page.goto(`${BASE}/`, { waitUntil: 'domcontentloaded' });
  await page.waitForSelector('[data-testid="device-list-card"]', { timeout: 15000 });

  const rows = () => page.locator('[data-testid="device-row"]');
  await waitFor(async () => (await rows().count()) === 2, 15000, '设备页出现 2 个 device-row');
  const pageRows = await rows().count();
  const pageText = await page.locator('[data-testid="device-list-card"]').innerText();
  console.log('[e2e-devices] 设备页卡片文本：');
  console.log(pageText.split('\n').map((l) => `  ${l}`).join('\n'));
  const leases = await page.locator('[data-testid="device-lease"]').allInnerTexts();
  const states = await page.locator('[data-testid="device-state"]').allInnerTexts();
  console.log(`[e2e-devices] 页面行数=${pageRows} 租约列=${JSON.stringify(leases)} 状态列=${JSON.stringify(states)}`);
  if (pageRows !== 2) fail(`设备页 device-row 数量=${pageRows}，期望 2`);
  if (states.some((s) => s.trim() !== 'live')) fail(`状态列应全为 live，实际 ${JSON.stringify(states)}`);
  if (!leases.every((s) => /^\d+\.\d+s$/.test(s.trim()))) {
    fail(`租约列应形如 "4.9s"，实际 ${JSON.stringify(leases)}`);
  }
  // 「本浏览器」与「接入设备」必须能分开读：两张卡的说明都在
  if ((await page.locator('[data-testid="caps-scope-note"]').count()) !== 1) {
    fail('缺少「本浏览器能力探测」的范围说明');
  }
  if (pageErrors.length > 0) fail(`页面抛错：${pageErrors.join('; ')}`);
  if (failures.length === 0) ok('断言 2：设备页出现 2 个 device-row，状态全为 live，租约列是递减倒计时');
  await page.screenshot({ path: path.join(SHOT_DIR, 'devices-2.png'), fullPage: true });

  // 倒计时真的在走：同一行 1.5 s 后的读数必须变化（不是静态文本）
  const lease0 = (await page.locator('[data-testid="device-lease"]').first().innerText()).trim();
  const lease1 = await waitFor(async () => {
    const v = (await page.locator('[data-testid="device-lease"]').first().innerText()).trim();
    return v !== lease0 ? v : null;
  }, 5000, '租约倒计时在变化');
  if (lease1) ok(`租约倒计时在走：${lease0} → ${lease1}`);

  // ---------------------------------------------------------------- 4. 杀掉一路 → 列表变 1
  const killed = liveDevices(two)[1];
  console.log(`[e2e-devices] 杀掉第二路（session_id=${killed.session_id} device_id=${killed.device_id}）`);
  capB.kill('SIGTERM');
  await Promise.race([capB.exited, sleep(5000)]);

  const oneState = await waitFor(async () => {
    const s = await getState();
    return liveDevices(s).length === 1 ? s : null;
  }, 20000, '/api/state 的 live 设备降到 1');
  if (oneState) {
    const left = liveDevices(oneState)[0];
    console.log(`[e2e-devices] 剩余设备：${JSON.stringify({ device_id: left.device_id, peer: left.peer, lease_ms: left.lease_ms })}`);
    ok(`断言 3a：/api/state 的 live 从 2 降到 1（剩余 ${left.device_id}）`);
  }

  await page.reload({ waitUntil: 'domcontentloaded' });
  await page.waitForSelector('[data-testid="device-list-card"]', { timeout: 15000 });
  await waitFor(async () => (await rows().count()) === 1, 15000, '刷新后只剩 1 个 device-row');
  const afterRows = await rows().count();
  const afterIds = await page.locator('[data-testid="device-row"]').evaluateAll((els) =>
    els.map((e) => e.getAttribute('data-device-id')));
  const closedNote = await page
    .locator('[data-testid="devices-closed-note"]')
    .innerText()
    .catch(() => '');
  console.log(`[e2e-devices] 刷新后 device-row=${afterRows} 设备=${JSON.stringify(afterIds)}`);
  console.log(`[e2e-devices] 最近断开说明：${closedNote.replace(/\n/g, ' ')}`);
  if (afterRows !== 1) fail(`刷新后 device-row=${afterRows}，期望 1`);
  if (afterIds.includes(killed.device_id)) fail(`被杀掉的 ${killed.device_id} 仍出现在在线列表里`);
  if (!closedNote.includes(killed.device_id)) {
    fail(`「最近断开」里没有 ${killed.device_id}（cued 的 include_closed 历史没透过来）`);
  }
  if (failures.length === 0) ok('断言 3b：杀掉一路后刷新，在线列表只剩 1 个，被杀的进「最近断开」');
  await page.screenshot({ path: path.join(SHOT_DIR, 'devices-1.png'), fullPage: true });

  await browser.close();

  // ---------------------------------------------------------------- 5. 收尾
  if (!KEEP) await shutdown();
  else console.log('[e2e-devices] KEEP_RUNNING=1：进程保留，手动 stop.sh');

  console.log('');
  if (failures.length === 0) {
    console.log('[e2e-devices] PASS（3 项断言全过）');
    process.exit(0);
  }
  console.log(`[e2e-devices] FAIL（${failures.length} 项）`);
  for (const f of failures) console.log(`  - ${f}`);
  process.exit(1);
})().catch(async (e) => {
  console.error(`[e2e-devices] 异常：${e && e.stack ? e.stack : e}`);
  await shutdown().catch(() => {});
  process.exit(1);
});
