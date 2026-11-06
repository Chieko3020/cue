#!/usr/bin/env node
// C08 real loopback server + browser recovery. Metrics/failure-mode assertions
// are explicitly routed frontend fixtures; no pipeline/devices/models are used.
// NODE_PATH=/tmp/pwtest/node_modules BUILD_DIR=build-codex-c16 node scripts/e2e-console-recovery.cjs
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const net = require('node:net');
const { spawn } = require('node:child_process');
const { chromium } = require('playwright');
const ROOT = path.resolve(__dirname, '..');
const OUT = path.resolve(process.env.ARTIFACT_DIR || '/tmp/cue-console-recovery');
fs.mkdirSync(OUT, { recursive: true });
const delay = (ms) => new Promise((r) => setTimeout(r, ms));
const processes = [];
const checks = [];
let child, browser, base, port;
const check = (name, detail = {}) => { checks.push({ name, ...detail }); console.log('PASS', name); };
async function until(fn, timeout = 15000) {
  const start = Date.now();
  while (Date.now() - start < timeout) { if (await fn()) return; await delay(100); }
  throw new Error('condition timeout');
}
async function startServer() {
  const log = fs.openSync(path.join(OUT, `server-${processes.length + 1}.log`), 'w');
  child = spawn(path.join(ROOT, process.env.BUILD_DIR || 'build-codex-c16', 'cue-serve'),
    ['--bind', '127.0.0.1', '--port', String(port), '--no-pipeline', '--web', path.join(ROOT, 'web/dist'),
      '--stream-dir', path.join(ROOT, 'web'), '--runs', path.join(OUT, 'runs')],
    { cwd: ROOT, stdio: ['ignore', log, log] });
  fs.closeSync(log);
  const rec = { pid: child.pid, reaped: false, argv: child.spawnargs };
  processes.push(rec);
  child.once('exit', (code, signal) => Object.assign(rec, { reaped: true, code, signal }));
  await until(async () => { try { return (await fetch(base + '/api/state')).ok; } catch { return false; } });
  return (await (await fetch(base + '/api/state')).json()).server.generation;
}
async function stopServer() {
  if (!child || child.exitCode !== null || child.signalCode !== null) return;
  const current = child;
  current.kill('SIGTERM');
  try { await until(() => current.exitCode !== null || current.signalCode !== null, 4000); }
  catch { current.kill('SIGKILL'); await until(() => current.signalCode !== null); }
}
async function action() {
  const res = await fetch(base + '/api/action', { method: 'POST', headers: { 'content-type': 'application/json' }, body: '{"action":"metrics"}' });
  const reply = await res.json();
  assert.equal(res.status, 200);
  assert.equal(reply.tool, 'query_metrics');
  // --no-pipeline deliberately produces a failed query, still real tool events.
  assert(['executed', 'failed'].includes(reply.status));
}
async function main() {
  const reservation = net.createServer();
  await new Promise((r) => reservation.listen(0, '127.0.0.1', r));
  port = reservation.address().port;
  await new Promise((r) => reservation.close(r));
  base = `http://127.0.0.1:${port}`;
  const generation1 = await startServer();
  browser = await chromium.launch({ headless: true });
  const page = await browser.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  await page.addInitScript(() => {
    const Native = window.EventSource;
    window.__sse = [];
    window.__sources = [];
    window.EventSource = class extends Native {
      constructor(...args) {
        super(...args);
        window.__sources.push(this);
        this.addEventListener('message', (e) => {
          window.__sse.push({ cursor: e.lastEventId, msg: JSON.parse(e.data) });
        });
      }
    };
    window.__pageIdentity = Math.random();
  });
  for (const suffix of ['/stream', '/stream/']) {
    const stream = await browser.newPage();
    const resource = stream.waitForResponse((r) => r.url() === base + '/app.js');
    await stream.goto(base + suffix);
    const response = await resource;
    assert.equal(response.status(), 200);
    assert.match(response.headers()['content-type'], /javascript/);
    assert.match(await response.text(), /Cue.*浏览器端推流/);
    await stream.waitForFunction(() => typeof window.resolveWsUrl === 'function');
    check(`real stream resources ${suffix}`);
    await stream.close();
  }
  await page.goto(base);
  await page.waitForFunction(() => document.querySelector('[data-testid="backend-status"]')?.textContent === '在线');
  const identity = await page.evaluate(() => window.__pageIdentity);
  await action();
  await page.waitForFunction(() => window.__sse.some((e) => e.msg.ev === 'tool.result' && e.cursor === `${e.msg.generation}:${e.msg.id}`));
  check('real live SSE carries resumable generation cursor');
  const before = (await (await fetch(base + '/api/state')).json()).server.sse_last_id;
  await action();
  const reconnectController = new AbortController();
  const reconnectTimer = setTimeout(() => reconnectController.abort(), 4000);
  try {
    const reconnect = await fetch(base + '/events', {
      headers: { 'last-event-id': `${generation1}:${before}` }, signal: reconnectController.signal,
    });
    const reader = reconnect.body.getReader();
    let frames = '';
    while (!frames.includes(`"id":${before + 2}`)) {
      const part = await reader.read();
      assert(!part.done);
      frames += new TextDecoder().decode(part.value);
    }
    assert(frames.includes(`"replayed_from":${before}`));
    assert(frames.includes(`id: ${generation1}:${before + 1}`));
    assert(!frames.includes(`id: ${generation1}:${before}\n`));
    check('real same-generation reconnect resumes with case-insensitive Last-Event-ID');
  } finally { clearTimeout(reconnectTimer); reconnectController.abort(); }
  await page.click('[data-testid="tab-session"]');
  await page.click('[data-testid="view-events"]');
  await page.waitForSelector('[data-testid="event-row"]');
  await stopServer();
  await page.waitForSelector('[data-testid="snapshot-stale"]', { timeout: 7000 });
  assert.match(await page.textContent('[data-testid="backend-status"]'), /离线/);
  check('real stopped backend exposes stale snapshot');
  const generation2 = await startServer();
  assert.notEqual(generation1, generation2);
  await page.waitForFunction((g) => window.__sse.some((e) => e.msg.ev === 'console.ready' && e.msg.generation === g), generation2);
  await page.waitForFunction(() => document.querySelector('[data-testid="backend-status"]')?.textContent === '在线');
  await action();
  await page.waitForFunction((g) => window.__sse.some((e) => e.msg.ev === 'tool.result' && e.msg.generation === g), generation2);
  await page.waitForSelector(`[data-testid="event-row"][data-ev="tool.result"][data-generation="${generation2}"]`);
  assert.equal(await page.evaluate(() => window.__pageIdentity), identity);
  check('real backend restart receives new low-id events without refresh', { generation1, generation2 });

  // Freeze only /api/state, retaining a real SSE connection. 18 distinct values
  // establish the exact stage/percentile mapping independently of media timing.
  const snapshot = await (await fetch(base + '/api/state')).json();
  const stages = ['recv', 'buffer', 'decode', 'op', 'encode', 'send'];
  const percentiles = ['p50', 'p95', 'p99'];
  let mode = 'values', releaseOld, oldPending, releaseSame, samePending;
  await page.route('**/api/state', async (route) => {
    try {
      if (mode === 'hang') { await delay(6000); await route.abort().catch(() => {}); return; }
      if (mode === 'http-error') { await route.fulfill({ status: 503, json: { ok: true } }); return; }
      const j = structuredClone(snapshot);
      if (mode === 'delayed-same') {
        samePending = true;
        await new Promise((r) => { releaseSame = r; });
      }
      if (mode === 'delayed-old') {
        oldPending = true;
        await new Promise((r) => { releaseOld = r; });
      }
      stages.forEach((s, i) => percentiles.forEach((p, k) => { j.metrics[`${s}_${p}_ms`] = mode === 'empty' ? null : i * 10 + k + 0.125; }));
      if (mode === 'empty') { j.metrics.recv_p50_ms = 0; delete j.metrics.send_p99_ms; }
      await route.fulfill({ json: j });
    } finally { /* route lifecycle is owned by the page */ }
  });
  await page.click('[data-testid="tab-pipeline"]');
  await until(async () => (await page.textContent('[data-testid="seg-recv"]')).includes('0.13'));
  for (const [i, s] of stages.entries()) {
    assert.deepEqual(await page.locator(`[data-testid="seg-${s}"] td`).allTextContents(), [s, ...percentiles.map((_p, k) => (i * 10 + k + 0.125).toFixed(2))]);
  }
  check('fixture all 18 percentile cells map to exact API keys');
  mode = 'empty';
  await until(async () => (await page.textContent('[data-testid="seg-recv"]')).includes('0.00'));
  assert.deepEqual(await page.locator('[data-testid="seg-recv"] td').allTextContents(), ['recv', '0.00', '—', '—']);
  assert.deepEqual(await page.locator('[data-testid="seg-send"] td').allTextContents(), ['send', '—', '—', '—']);
  check('fixture null/missing stay unknown and numeric zero stays zero');
  mode = 'http-error';
  await page.waitForSelector('[data-testid="snapshot-stale"]');
  check('fixture HTTP 503 cannot claim online despite ok:true body');
  mode = 'values';
  await page.waitForFunction(() => document.querySelector('[data-testid="backend-status"]')?.textContent === '在线');
  mode = 'hang';
  await page.waitForSelector('[data-testid="snapshot-stale"]', { timeout: 7500 });
  check('fixture hung state request times out while SSE stays live');
  // Server-side handlers may outlive client abort; count browser requests instead
  // through the serial recovery cadence after the timeout, no delayed reply wins.
  mode = 'values';
  await page.waitForFunction(() => document.querySelector('[data-testid="backend-status"]')?.textContent === '在线', null, { timeout: 10000 });
  check('fixture snapshot polling recovers after timeout');
  mode = 'delayed-same';
  await until(() => samePending);
  await page.evaluate(() => window.__sources[0].dispatchEvent(new Event('error')));
  await page.waitForSelector('[data-testid="snapshot-stale"]');
  releaseSame();
  mode = 'values';
  await delay(300);
  assert.match(await page.textContent('[data-testid="backend-status"]'), /离线/);
  await page.waitForFunction(() => document.querySelector('[data-testid="backend-status"]')?.textContent === '在线');
  check('fixture pre-disconnect same-generation success cannot erase stale state');

  // An old in-flight snapshot must not undo a generation handshake. This
  // synthetic SSE pair is a race fixture, distinct from the real restart above.
  mode = 'delayed-old';
  await until(() => oldPending);
  const fixtureGeneration = generation2 + '-race';
  await page.evaluate((generation) => {
    const source = window.__sources[0];
    source.dispatchEvent(new MessageEvent('message', { data: JSON.stringify({ ev: 'console.ready', id: 0, generation }) }));
    source.dispatchEvent(new MessageEvent('message', { data: JSON.stringify({ ev: 'fixture.new', id: 1, generation, data: {} }) }));
  }, fixtureGeneration);
  await page.waitForSelector('[data-testid="snapshot-stale"]');
  releaseOld();
  mode = 'values';
  snapshot.server.generation = fixtureGeneration;
  snapshot.events = [{ ev: 'fixture.new', id: 1, generation: fixtureGeneration, data: {} }];
  // The released response captures the old generation. Before the next poll,
  // the UI must still show stale rather than accepting its success.
  await delay(300);
  assert.match(await page.textContent('[data-testid="backend-status"]'), /离线/);
  assert.equal(await page.textContent('[data-testid="event-count"]'), '1');
  await page.waitForFunction(() => document.querySelector('[data-testid="backend-status"]')?.textContent === '在线');
  check('fixture late old snapshot cannot undo SSE generation or current events');
  await page.screenshot({ path: path.join(OUT, 'pipeline.png'), fullPage: true });
  assert.deepEqual(errors, []);
  fs.writeFileSync(path.join(OUT, 'sse.json'), JSON.stringify(await page.evaluate(() => window.__sse), null, 2));
  check('browser no JavaScript errors');
}
(async () => {
  let error;
  try { await main(); } catch (e) { error = e.stack; console.error(error); process.exitCode = 1; }
  finally {
    if (browser) await browser.close();
    await stopServer();
    fs.writeFileSync(path.join(OUT, 'summary.json'), JSON.stringify({ checks, error: error || null, processes }, null, 2));
  }
})();
