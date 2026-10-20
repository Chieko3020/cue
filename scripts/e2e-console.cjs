#!/usr/bin/env node
// M5 控制台的浏览器端到端验证（可重复跑）
//
// 用法：
//   NODE_PATH=/tmp/pwtest/node_modules node scripts/e2e-console.cjs
//
// 环境变量（都有默认值，互不冲突于 run.sh 的 8080/9000/9001）：
//   BUILD_DIR=build-m5  CONSOLE_PORT=8199  WS_PORT=9100  PROBE_PORT=9101
//   SHOT_DIR=/tmp/cue-e2e（截图输出）  KEEP_RUNNING=1（不清理进程，便于手动看页面）
//
// 它自己拉起整条链路（cue-probe + cue-serve（内部起 cued）+ 合成推流客户端），
// 然后用 headless Chromium 断言：
//   1. 五页（设备/管线/会话/审批/评测）可切换
//   2. SSE 事件在「会话」页实时出现（>= 3 条，且 3 秒内继续增长）
//   3. 触发危险动作（inject_fault）→「审批」页出现请求 → 点允许 → 动作真的执行
//   4. 指标页数字在动（frames_in 增长、fps > 0）
//   5. 没有任何外部域名请求（无外部 CDN；断网也能加载）
//   6. 刷新后事件流能从 /api/state 快照恢复（SSE 补齐的兜底路径）
//
// 退出码：0 全通过；1 有失败项；2 环境不满足（缺二进制/缺 web/dist）。
const { spawn, spawnSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

const ROOT = path.resolve(__dirname, '..');
const BUILD = process.env.BUILD_DIR || 'build-m5';
const PORT = Number(process.env.CONSOLE_PORT || 8199);
const WS_PORT = Number(process.env.WS_PORT || 9100);
const PROBE_PORT = Number(process.env.PROBE_PORT || 9101);
const BASE = `http://127.0.0.1:${PORT}`;
const SHOT_DIR = process.env.SHOT_DIR || '/tmp/cue-e2e';
const KEEP = process.env.KEEP_RUNNING === '1';

const children = [];
const failures = [];
let pipelinePid = null;

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

function fail(msg) {
  failures.push(msg);
  console.error(`[e2e] FAIL: ${msg}`);
}

function ok(msg) {
  console.log(`[e2e] ok: ${msg}`);
}

function haveBin(p) {
  try {
    fs.accessSync(p, fs.constants.X_OK);
    return true;
  } catch {
    return false;
  }
}

// 起子进程并保留输出尾部（失败时打印，方便定位；成功时不刷屏）
function start(name, cmd, args, opts = {}) {
  const child = spawn(cmd, args, {
    cwd: ROOT,
    stdio: ['ignore', 'pipe', 'pipe'],
    detached: false,
    ...opts,
  });
  const tail = [];
  const push = (buf, stream) => {
    const text = buf.toString();
    tail.push(`[${stream}] ${text}`);
    if (tail.length > 200) tail.shift();
  };
  child.stdout.on('data', (b) => push(b, 'out'));
  child.stderr.on('data', (b) => push(b, 'err'));
  child.on('error', (e) => fail(`${name} 进程错误：${e.message}`));
  child.exited = new Promise((resolve) => child.on('exit', (code, sig) => resolve({ code, sig })));
  children.push({ name, child, tail });
  return child;
}

async function waitHttp(url, timeoutMs = 15000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      const r = await fetch(url, { cache: 'no-store' });
      if (r.ok) return await r.json();
    } catch {
      /* 还没起来 */
    }
    await sleep(200);
  }
  return null;
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
      /* ignore */
    }
    await sleep(200);
  }
  fail(`等待超时：${what}`);
  return null;
}

function processAlive(pid) {
  try {
    process.kill(pid, 0);
    return true;
  } catch {
    return false;
  }
}

async function shutdown() {
  for (const { name, child } of children.slice().reverse()) {
    if (name === 'cue-serve' && child.exitCode === null) {
      child.kill('SIGTERM');
      await Promise.race([child.exited, sleep(5000)]);
    } else if (child.exitCode === null) {
      child.kill('SIGTERM');
      await Promise.race([child.exited, sleep(2000)]);
    }
  }
}

(async () => {
  // ---------------------------------------------------------------- 0. 前置
  const serveBin = path.join(ROOT, BUILD, 'cue-serve');
  const cuedBin = path.join(ROOT, BUILD, 'cued');
  const probeBin = path.join(ROOT, BUILD, 'cue-probe');
  for (const bin of [serveBin, cuedBin, probeBin]) {
    if (!haveBin(bin)) {
      console.error(`[e2e] 缺少 ${bin}\n先构建：cmake -S . -B ${BUILD} -DCMAKE_CXX_COMPILER=g++-14 && cmake --build ${BUILD} -j1`);
      process.exit(2);
    }
  }
  const distIndex = path.join(ROOT, 'web', 'dist', 'index.html');
  if (!fs.existsSync(distIndex)) {
    console.error('[e2e] web/dist 不存在，先构建控制台：cd web/console && npm install && npm run build');
    process.exit(2);
  }
  fs.mkdirSync(SHOT_DIR, { recursive: true });

  let chromium;
  try {
    ({ chromium } = require('playwright'));
  } catch (e) {
    console.error(`[e2e] 无法加载 playwright（需要 NODE_PATH 指向含 playwright 的 node_modules）：${e.message}`);
    process.exit(2);
  }

  // ---------------------------------------------------------------- 1. 起链路
  start('cue-probe', probeBin, ['--port', String(PROBE_PORT)]);
  const serve = start('cue-serve', serveBin, [
    '--port', String(PORT),
    '--ws-port', String(WS_PORT),
    '--probe-port', String(PROBE_PORT),
    '--cued', cuedBin,
    '--web', 'web/dist',
    '--runs', 'runs',
    '--approval', 'ask',
    '--approval-timeout', '30',
  ]);
  const health = await waitHttp(`${BASE}/api/health`, 15000);
  if (!health || !health.ok) {
    console.error('[e2e] cue-serve 没起来');
    for (const { name, tail } of children) console.error(`--- ${name} ---\n${tail.join('')}`);
    process.exit(2);
  }
  ok(`cue-serve 就绪 ${BASE}`);

  // 等 cued 真正开始监听（pipeline.started 在 bind 成功后才发）：
  // 否则 test-client 可能在 cued listen 之前连一次就失败退出（实测踩过）
  const started = await waitFor(async () => {
    const s = await getState();
    return (s.events || []).some((e) => e.ev === 'pipeline.started') ? s : null;
  }, 15000, 'cued 报告 pipeline.started（WS 端口已监听）');
  if (started && started.metrics) pipelinePid = started.metrics.pid;

  // 合成推流源：每帧真实走 WS → cued 解码 → UDP，指标才有内容。
  // 连接是"一次性"的（test-client 不重连），所以起 client 后没帧就重起一次，最多 3 次。
  let withFrames = null;
  for (let attempt = 1; attempt <= 3 && !withFrames; attempt += 1) {
    const client = start('test-client', process.execPath, [
      'scripts/test-client.mjs', `ws://127.0.0.1:${WS_PORT}/`, '900',
    ]);
    const deadline = Date.now() + 8000;
    while (Date.now() < deadline) {
      const s = await getState();
      if (s.metrics && s.metrics.frames_in > 0) {
        withFrames = s;
        break;
      }
      await sleep(200);
    }
    if (!withFrames) {
      console.log(`[e2e] test-client 第 ${attempt} 次没送到帧，重试`);
      client.kill('SIGTERM');
      await Promise.race([client.exited, sleep(1000)]);
    }
  }
  if (withFrames) {
    pipelinePid = withFrames.metrics.pid;
    ok(`管线收到帧（frames_in=${withFrames.metrics.frames_in}, pid=${pipelinePid}）`);
  } else {
    fail('cued 一直没收到帧（metrics.frames_in 始终为 0）');
  }

  // ---------------------------------------------------------------- 2. 浏览器
  const browser = await chromium.launch({ args: ['--no-sandbox'] });
  const page = await browser.newPage({ viewport: { width: 1360, height: 900 } });
  const pageErrors = [];
  const externalRequests = [];
  page.on('pageerror', (e) => pageErrors.push(String(e && e.message ? e.message : e)));
  page.on('request', (req) => {
    const u = new URL(req.url());
    if (!['127.0.0.1', 'localhost', '::1'].includes(u.hostname)) externalRequests.push(req.url());
  });

  await page.goto(`${BASE}/`, { waitUntil: 'domcontentloaded' });
  await page.waitForSelector('[data-testid="page-device"]', { timeout: 15000 });
  ok('页面加载完成（设备页）');

  // ---------------------------------------------------------------- 3. 五页可切换
  const pages = [
    ['device', '设备'],
    ['pipeline', '管线'],
    ['session', '会话'],
    ['approval', '审批'],
    ['runs', '评测'],
  ];
  for (const [id] of pages) {
    await page.click(`[data-testid="tab-${id}"]`);
    await page.waitForSelector(`[data-testid="page-${id}"]`, { timeout: 5000 });
    ok(`切到「${id}」页`);
  }

  // ---------------------------------------------------------------- 4. 指标在动
  await page.click('[data-testid="tab-pipeline"]');
  await page.waitForSelector('[data-testid="metric-frames-in"]', { timeout: 5000 });
  const numOf = async (sel) => Number((await page.textContent(sel)).replace(/[^0-9.-]/g, '')) || 0;
  const fi1 = await waitFor(async () => {
    const v = await numOf('[data-testid="metric-frames-in"]');
    return v > 0 ? v : 0;
  }, 10000, '指标页 frames_in > 0');
  const fps1 = await numOf('[data-testid="metric-fps"]');
  await sleep(3000);
  const fi2 = await numOf('[data-testid="metric-frames-in"]');
  const fps2 = await numOf('[data-testid="metric-fps"]');
  if (fps1 <= 0 && fps2 <= 0) fail(`指标 fps 没有读数（fps1=${fps1} fps2=${fps2}）`);
  if (!(fi2 > fi1)) fail(`指标没有在动：frames_in ${fi1} → ${fi2}`);
  else ok(`指标在动：frames_in ${fi1} → ${fi2}，fps ${fps1} → ${fps2}`);
  await page.screenshot({ path: path.join(SHOT_DIR, 'pipeline.png'), fullPage: true });

  // ---------------------------------------------------------------- 5. 会话页 SSE 实时出现
  await page.click('[data-testid="tab-session"]');
  await page.waitForSelector('[data-testid="event-row"]', { timeout: 15000 });
  const rows = () => page.locator('[data-testid="event-row"]').count();
  const n1 = await waitFor(async () => {
    const c = await rows();
    return c >= 3 ? c : 0;
  }, 15000, '会话页出现 >= 3 条事件');
  await sleep(3000);
  const n2 = await rows();
  if (n2 <= n1) fail(`事件流没有实时增长：${n1} → ${n2}`);
  else ok(`会话页 SSE 实时事件 ${n1} → ${n2} 条`);
  const sseStatus = await page.textContent('[data-testid="sse-status"]');
  if (sseStatus !== 'live') fail(`SSE 状态是 ${sseStatus}，不是 live`);
  await page.screenshot({ path: path.join(SHOT_DIR, 'session.png'), fullPage: true });

  // ---------------------------------------------------------------- 6. 危险动作 → 审批 → 执行
  await page.click('[data-testid="tab-pipeline"]');
  await page.waitForSelector('[data-testid="ctl-inject"]');
  await page.fill('[data-testid="param-inject-drop"]', '5');
  await page.click('[data-testid="ctl-inject"]'); // 不 await 请求：它要等审批才返回
  ok('已提交 inject_fault（等待审批）');

  await page.click('[data-testid="tab-approval"]');
  await page.waitForSelector('[data-testid="approval-item"]', { timeout: 10000 });
  const itemTool = await page.getAttribute('[data-testid="approval-item"]', 'data-tool');
  if (itemTool !== 'inject_fault') fail(`审批项工具是 ${itemTool}，期望 inject_fault`);
  else ok('审批页出现 inject_fault 待审批请求');
  const countdownText = await page.textContent('[data-testid="approval-countdown"]');
  if (!/\d+s/.test(countdownText)) fail(`倒计时文本异常：${countdownText}`);
  await page.screenshot({ path: path.join(SHOT_DIR, 'approval.png'), fullPage: true });

  await page.click('[data-testid="approval-allow"]');
  await page.waitForSelector('[data-testid="approval-notice"]', { timeout: 5000 });
  const notice = await page.textContent('[data-testid="approval-notice"]');
  if (!notice.includes('已提交')) fail(`批准回执异常：${notice}`);
  else ok('点击「允许」后审批被提交');

  // 动作真的执行了：/api/state 的 actions 里出现 executed
  const executed = await waitFor(async () => {
    const s = await getState();
    return (s.actions || []).some((a) => a.tool === 'inject_fault' && a.status === 'executed');
  }, 20000, 'inject_fault 在控制台动作历史中变成 executed');
  if (executed) {
    ok('动作已执行（actions: inject_fault → executed）');
    // 注入会重启 cued：记下新一代 pid，退出时用它验证"无孤儿"
    const s2 = await getState();
    if (s2.metrics && s2.metrics.pid) pipelinePid = s2.metrics.pid;
  }

  // 执行结果也进了 SSE：会话页应有 tool.result 事件
  await page.click('[data-testid="tab-session"]');
  const toolResult = await page.waitForSelector('[data-testid="event-row"][data-ev="tool.result"]', { timeout: 10000 });
  const trText = await toolResult.textContent();
  if (!trText.includes('executed')) fail(`tool.result 事件里没有 executed：${trText}`);
  else ok('SSE 上收到 tool.result（status=executed）');

  // 管线页的结果面板也应该是"已执行"
  await page.click('[data-testid="tab-pipeline"]');
  await page.waitForSelector('[data-testid="action-result"]');
  const arText = await page.textContent('[data-testid="action-result"]');
  if (!/executed|已执行/.test(arText)) fail(`动作结果面板没有显示已执行：${arText}`);
  else ok('动作结果面板显示已执行');

  // ---------------------------------------------------------------- 7. 评测页
  const reports = fs.existsSync(path.join(ROOT, 'runs'))
    ? fs.readdirSync(path.join(ROOT, 'runs')).filter((d) => {
        const rep = path.join(ROOT, 'runs', d, 'report.md');
        return d.startsWith('run-') && fs.existsSync(rep);
      }).length
    : 0;
  await page.click('[data-testid="tab-runs"]');
  await page.waitForSelector('[data-testid="page-runs"]', { timeout: 5000 });
  // 有报告就必须等卡片真的渲染出来（等空态会抢在 fetch 完成之前，实测踩过）
  if (reports > 0) {
    await page.waitForSelector('[data-testid="run-card"]', { timeout: 15000 }).catch(() => {});
    const runCards = await page.locator('[data-testid="run-card"]').count();
    if (runCards === 0) fail(`runs 目录有 ${reports} 份报告但评测页没渲染出卡片`);
    else ok(`评测页渲染 ${runCards} 份报告（目录里 ${reports} 份）`);
  } else {
    await page.waitForSelector('[data-testid="runs-empty"]', { timeout: 10000 }).catch(() => {});
    ok('评测页可用（runs 目录暂无报告，显示空态）');
  }
  await page.screenshot({ path: path.join(SHOT_DIR, 'runs.png'), fullPage: true });
  await page.click('[data-testid="tab-device"]');
  await page.screenshot({ path: path.join(SHOT_DIR, 'device.png'), fullPage: true });

  // ---------------------------------------------------------------- 8. 刷新后事件从快照恢复
  await page.reload({ waitUntil: 'domcontentloaded' });
  await page.waitForSelector('[data-testid="page-device"]', { timeout: 15000 });
  await page.click('[data-testid="tab-session"]');
  const afterReload = await waitFor(async () => {
    const c = await rows();
    return c >= 3 ? c : 0;
  }, 10000, '刷新后事件流从 /api/state 快照恢复 >= 3 条');
  if (afterReload) ok(`刷新后仍有 ${afterReload} 条事件（快照补齐）`);

  // ---------------------------------------------------------------- 9. 无外部请求
  if (externalRequests.length > 0) {
    fail(`出现了外部请求（违反无 CDN 约束）：${externalRequests.slice(0, 5).join(', ')}`);
  } else {
    ok('所有请求都是同源/回环（无外部 CDN）');
  }
  if (pageErrors.length > 0) fail(`页面 JS 报错：${pageErrors.join(' | ')}`);

  await browser.close();

  // ---------------------------------------------------------------- 10. 清理
  if (!KEEP) {
    await shutdown();
    if (pipelinePid) {
      const stillAlive = processAlive(pipelinePid);
      if (stillAlive) fail(`cue-serve 退出后 cued(pid=${pipelinePid}) 仍在（孤儿进程）`);
      else ok('cue-serve 退出后 cued 已随之停止（无孤儿）');
    }
  } else {
    console.log(`[e2e] KEEP_RUNNING=1，链路仍在跑：${BASE}/`);
  }

  console.log('');
  if (failures.length === 0) {
    console.log(`[e2e] 全部通过（截图：${SHOT_DIR}/*.png）`);
    process.exit(0);
  }
  console.log(`[e2e] ${failures.length} 项失败：`);
  for (const f of failures) console.log(`  - ${f}`);
  process.exit(1);
})().catch(async (e) => {
  console.error('[e2e] 未捕获异常:', e);
  await shutdown();
  process.exit(1);
});
