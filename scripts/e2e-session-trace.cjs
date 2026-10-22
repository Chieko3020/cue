#!/usr/bin/env node
// T-24（FR-UI.3）会话页的端到端验证：cue demo 写出的真实 session jsonl →
// GET /api/sessions / /api/session → 浏览器里的按阶段轨迹。
//
// 用法：
//   NODE_PATH=/tmp/pwtest/node_modules node scripts/e2e-session-trace.cjs
//
// 环境变量：BUILD_DIR=build  CONSOLE_PORT=8301  SHOT_DIR=/tmp/cue-e2e-session
//           KEEP_RUNNING=1（不清理进程/不删会话文件）
//
// 断言（全部实测）：
//   1. `cue demo --session runs/e2e/sessions/demo.jsonl` 生成会话（scripted，不联网）
//   2. /api/sessions 列出它：session_id/run_id/final_stage/turns/bytes 都有值
//   3. /api/session?id=demo 返回七类记录分组（meta/message/tool/observation/stage/budget/end）
//      且 diagnose 段之前的那条 stage 记录带着 diagnosis.evidence
//   4. 路径穿越 ?id=../../etc/passwd 被拒（HTTP 4xx，响应里不含 /etc/passwd 内容）
//   5. 浏览器：会话页 → 至少一个 diagnose 段，段内能看到 evidence，工具调用条目出现；
//      段标题带耗时与 turns；事件流视图仍可切换
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

const ROOT = path.resolve(__dirname, '..');
const BUILD = process.env.BUILD_DIR || 'build';
const PORT = Number(process.env.CONSOLE_PORT || 8301);
const BASE = `http://127.0.0.1:${PORT}`;
const SHOT_DIR = process.env.SHOT_DIR || '/tmp/cue-e2e-session';
const KEEP = process.env.KEEP_RUNNING === '1';
const SESSION_REL = 'runs/e2e/sessions/demo.jsonl';

const children = [];
const failures = [];

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}
function ok(msg) {
  console.log(`[e2e-session] ok: ${msg}`);
}
function fail(msg) {
  failures.push(msg);
  console.error(`[e2e-session] FAIL: ${msg}`);
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
  const serveBin = path.join(ROOT, BUILD, 'cue-serve');
  const cueBin = path.join(ROOT, BUILD, 'cue');
  for (const bin of [serveBin, cueBin]) {
    if (!haveBin(bin)) {
      console.error(`[e2e-session] 缺少 ${bin}\n先构建：cmake -S . -B ${BUILD} -DCMAKE_CXX_COMPILER=g++-14 && cmake --build ${BUILD} -j1`);
      process.exit(2);
    }
  }
  if (!fs.existsSync(path.join(ROOT, 'web', 'dist', 'index.html'))) {
    console.error('[e2e-session] web/dist 不存在，先构建控制台：cd web/console && npm run build');
    process.exit(2);
  }
  let chromium;
  try {
    ({ chromium } = require('playwright'));
  } catch (e) {
    console.error(`[e2e-session] 无法加载 playwright（NODE_PATH 要指向含它的 node_modules）：${e.message}`);
    process.exit(2);
  }
  fs.mkdirSync(SHOT_DIR, { recursive: true });

  // ---------------------------------------------------------------- 1. 生成一个真实会话
  // 上一次跑残留的 demo.jsonl 是「已结束」的会话，--session 会拒绝续写（这是对的），
  // 所以先删掉再跑；不删的话第二次执行这条验收会假失败。
  const sessionPath = path.join(ROOT, SESSION_REL);
  if (!KEEP) {
    for (const p of [sessionPath, `${sessionPath}.lock`]) {
      try {
        fs.unlinkSync(p);
      } catch {
        /* 不存在就算了 */
      }
    }
  }
  const demo = start('cue-demo', cueBin, ['demo', '--session', SESSION_REL]);
  const demoExit = await Promise.race([demo.exited, sleep(60000).then(() => null)]);
  if (!demoExit) {
    fail('cue demo 60s 未结束');
    await shutdown();
    process.exit(1);
  }
  if (demoExit.code !== 0) {
    for (const { name, tail } of children) console.error(`--- ${name} ---\n${tail.join('')}`);
    fail(`cue demo 退出码 ${demoExit.code}`);
    await shutdown();
    process.exit(1);
  }
  if (!fs.existsSync(sessionPath)) {
    fail(`cue demo 没有写出 ${SESSION_REL}`);
    await shutdown();
    process.exit(1);
  }
  ok(`cue demo 写出 ${SESSION_REL}（${fs.statSync(sessionPath).size} 字节）`);

  // ---------------------------------------------------------------- 2. 起 cue-serve
  start('cue-serve', serveBin, ['--no-pipeline', '--port', String(PORT), '--web', 'web/dist', '--runs', 'runs']);
  const health = await waitHealth();
  if (!health || !health.ok) {
    for (const { name, tail } of children) console.error(`--- ${name} ---\n${tail.join('')}`);
    fail('cue-serve 没起来');
    await shutdown();
    process.exit(2);
  }
  ok(`cue-serve 就绪 ${BASE}`);

  // ---------------------------------------------------------------- 3. /api/sessions
  const list = await (await fetch(`${BASE}/api/sessions`, { cache: 'no-store' })).json();
  const entry = (list.sessions || []).find((s) => s.session_id === 'demo');
  console.log('[e2e-session] /api/sessions 里的 demo：');
  console.log(
    JSON.stringify(
      entry ? { session_id: entry.session_id, run_id: entry.run_id, final_stage: entry.final_stage, turns: entry.turns, bytes: entry.bytes, path: entry.path } : null,
      null,
      2,
    ),
  );
  if (!entry) fail('/api/sessions 没列出 demo');
  else {
    if (entry.run_id !== 'e2e') fail(`run_id=${entry.run_id}，期望 e2e（由路径 <runs>/<run>/sessions/<sid>.jsonl 反推）`);
    if (entry.final_stage !== 'done') fail(`final_stage=${entry.final_stage}，期望 done`);
    if (!(entry.turns > 0)) fail(`turns=${entry.turns}，期望 > 0`);
    if (!(entry.bytes > 0)) fail(`bytes=${entry.bytes}，期望 > 0`);
    if (!(entry.mtime_us > 0)) fail(`mtime_us=${entry.mtime_us}，期望 > 0`);
    if (failures.length === 0) ok('断言 2：/api/sessions 列出 demo（run_id/final_stage/turns/bytes/mtime 有值）');
  }

  // ---------------------------------------------------------------- 4. /api/session 七类分组
  const detail = await (await fetch(`${BASE}/api/session?id=demo`, { cache: 'no-store' })).json();
  const counts = detail.counts || {};
  console.log(`[e2e-session] /api/session?id=demo counts = ${JSON.stringify(counts)}`);
  for (const k of ['meta', 'message', 'tool', 'observation', 'stage', 'budget', 'end']) {
    if (!(counts[k] > 0)) fail(`七类记录里 ${k} 缺失（counts.${k}=${counts[k]}）`);
  }
  if (!Array.isArray(detail.messages) || !Array.isArray(detail.tools) || !Array.isArray(detail.stages)) {
    fail('响应缺少分组数组（messages/tools/stages）');
  }
  const diagStage = (detail.stages || []).find((s) => s.diagnosis && (s.diagnosis.evidence || []).length > 0);
  console.log(`[e2e-session] 带证据的 stage 记录 = ${diagStage ? JSON.stringify({ from: diagStage.from, to: diagStage.to, diagnosis: diagStage.diagnosis }) : null}`);
  if (!diagStage) fail('stage 记录里没有 diagnosis.evidence');
  if (diagStage && diagStage.to !== 'repair') fail(`带诊断的迁移应为 diagnose→repair，实际 ${diagStage.from}→${diagStage.to}`);
  const seqs = [...(detail.messages || []), ...(detail.tools || []), ...(detail.stages || [])].map((r) => r.seq);
  if (seqs.some((s) => typeof s !== 'number')) fail('分组记录缺 seq（行号），前端无法还原顺序');
  if (failures.length === 0) ok('断言 3：/api/session 返回七类分组，diagnose→repair 记录带 evidence，记录带 seq');

  // ---------------------------------------------------------------- 5. 路径穿越
  const trav = await fetch(`${BASE}/api/session?id=${encodeURIComponent('../../etc/passwd')}`, { cache: 'no-store' });
  const travText = await trav.text();
  console.log(`[e2e-session] 路径穿越 ?id=../../etc/passwd → HTTP ${trav.status} body=${travText}`);
  if (trav.status < 400 || trav.status >= 500) fail(`路径穿越应为 4xx，实际 ${trav.status}`);
  if (/root:x:|daemon:x:/.test(travText)) fail('路径穿越响应里出现了 /etc/passwd 的内容');
  if (travText.includes('ok":true')) fail('路径穿越竟然返回 ok:true');
  if (failures.length === 0) ok('断言 4：?id=../../etc/passwd 被拒（4xx，无文件内容）');

  // ---------------------------------------------------------------- 6. 浏览器轨迹视图
  const browser = await chromium.launch({ args: ['--no-sandbox'] });
  const page = await browser.newPage({ viewport: { width: 1360, height: 1000 } });
  const pageErrors = [];
  page.on('pageerror', (e) => pageErrors.push(String(e && e.message ? e.message : e)));
  await page.goto(`${BASE}/`, { waitUntil: 'domcontentloaded' });
  await page.click('[data-testid="tab-session"]');
  await page.waitForSelector('[data-testid="trace-card"]', { timeout: 15000 });
  await waitFor(async () => (await page.locator('[data-testid="trace-stage"]').count()) > 0, 15000, '轨迹段出现');

  const stages = page.locator('[data-testid="trace-stage"]');
  const stageCount = await stages.count();
  const stageNames = await page.locator('[data-testid="trace-stage"] h3').allInnerTexts();
  console.log(`[e2e-session] 段数=${stageCount}`);
  console.log(stageNames.map((s) => `  ${s.replace(/\n/g, ' ')}`).join('\n'));
  if (stageCount < 4) fail(`段数=${stageCount}，期望 collect/diagnose/repair/verify 至少 4 段`);

  // 段标题必须带耗时与 turns（不是只有阶段名）
  for (const t of stageNames) {
    if (!/段耗时\s*[\d.]+\s*(ms|s)/.test(t)) fail(`段标题缺少「段耗时」：${t.replace(/\n/g, ' ')}`);
    if (!/turns\s*\d+/.test(t)) fail(`段标题缺少「turns」：${t.replace(/\n/g, ' ')}`);
  }

  const diagnose = page.locator('[data-testid="trace-stage"][data-stage="diagnose"]');
  const diagnoseCount = await diagnose.count();
  if (diagnoseCount !== 1) fail(`diagnose 段数=${diagnoseCount}，期望 1`);

  const evidence = page.locator('[data-testid="trace-evidence"]');
  const evidenceCount = await evidence.count();
  if (evidenceCount !== 1) fail(`diagnose 段里 trace-evidence 数量=${evidenceCount}，期望 1`);
  const evidenceText = evidenceCount ? await evidence.innerText() : '';
  console.log('[e2e-session] diagnose 段的证据块：');
  console.log(evidenceText.split('\n').map((l) => `  ${l}`).join('\n'));
  if (!/root_cause/.test(evidenceText)) fail('证据块里没有 root_cause');
  if (!/nack_retransmit=42/.test(evidenceText)) fail('证据块里没有诊断给出的 evidence 项（nack_retransmit=42）');
  const items = page.locator('[data-testid="trace-evidence-item"]');
  const itemCount = await items.count();
  if (itemCount < 1) fail(`证据条目数=${itemCount}，期望 ≥1`);
  const foundMarks = await items.first().getAttribute('data-found');
  console.log(`[e2e-session] 证据条目 ${itemCount} 条，第一条 data-found=${foundMarks}`);
  if (foundMarks !== 'true') fail('证据项没有在 observation 原文里核对到（data-found!=true）');

  // 工具调用条目：出现在轨迹里且能看到名字与结果摘要
  const tools = page.locator('[data-testid="trace-tool"]');
  const toolCount = await tools.count();
  const toolNames = await tools.evaluateAll((els) => els.map((e) => e.getAttribute('data-tool')));
  console.log(`[e2e-session] 工具条目 ${toolCount} 条：${JSON.stringify(toolNames)}`);
  if (toolCount < 3) fail(`工具条目数=${toolCount}，期望 ≥3（demo 有 metrics/set_param/metrics 三次调用）`);
  if (!toolNames.includes('query_metrics')) fail('工具条目里没有 query_metrics');
  if (!toolNames.includes('pipeline_set_param')) fail('工具条目里没有 pipeline_set_param');
  const firstToolText = await tools.first().innerText();
  if (!/参数/.test(firstToolText) || !/结果摘要/.test(firstToolText)) {
    fail(`工具条目缺少参数/结果摘要：${firstToolText.replace(/\n/g, ' ')}`);
  }
  const expands = await tools.first().locator('details').count();
  if (expands < 1) fail('工具条目没有可展开的完整结果（details）');

  // 思考条目：demo 的 assistant 正文（scripted 剧本里有内容）
  const thinks = await page.locator('[data-testid="trace-think"]').count();
  console.log(`[e2e-session] assistant 思考条目 ${thinks} 条`);
  const observations = await page.locator('[data-testid="trace-observation"]').count();
  console.log(`[e2e-session] 观测条目 ${observations} 条`);
  if (observations < 1) fail('轨迹里没有 observation 条目');

  if (pageErrors.length > 0) fail(`页面抛错：${pageErrors.join('; ')}`);
  if (failures.length === 0) {
    ok('断言 5：会话页出现 diagnose 段 + 段内证据 + 工具调用条目（含参数/摘要/可展开），段标题带耗时与 turns');
  }
  await page.screenshot({ path: path.join(SHOT_DIR, 'session-trace.png'), fullPage: true });

  // ---------------------------------------------------------------- 7. 事件流视图仍可切换
  await page.click('[data-testid="view-events"]');
  const eventsVisible = await page.locator('[data-testid="event-list"], [data-testid="session-empty"]').count();
  if (eventsVisible < 1) fail('切到事件流视图后既没有事件列表也没有空态');
  await page.click('[data-testid="view-trace"]');
  if ((await page.locator('[data-testid="trace-card"]').count()) < 1) fail('切回轨迹视图失败');
  if (failures.length === 0) ok('断言 6：轨迹 / 事件流两个视图可互相切换');

  await browser.close();
  await shutdown();
  if (!KEEP) {
    for (const p of [sessionPath, `${sessionPath}.lock`]) {
      try {
        fs.unlinkSync(p);
      } catch {
        /* 忽略 */
      }
    }
  }

  if (failures.length > 0) {
    console.error(`[e2e-session] ${failures.length} 项失败`);
    process.exit(1);
  }
  console.log('[e2e-session] PASS');
  process.exit(0);
})();
