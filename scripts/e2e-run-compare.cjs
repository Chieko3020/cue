#!/usr/bin/env node
// T-24（FR-UI.5）评测页两 run 对比的端到端验证：
// 两次 scripted 评测 → /api/runs + /api/diff → 浏览器里的对比表。
//
// 用法：
//   NODE_PATH=/tmp/pwtest/node_modules node scripts/e2e-run-compare.cjs
//
// 环境变量：BUILD_DIR=build  CONSOLE_PORT=8302  EVAL_OUT=/tmp/cue-e2e-compare
//           SHOT_DIR=/tmp/cue-e2e-compare-shots  KEEP_RUNNING=1
//
// 断言（全部实测）：
//   1. 两次 `cue-eval --suite suites/m4-diagnosis.json --mode scripted` 都成功
//   2. /api/runs 列出两份报告；/api/diff 给出 case 级分类（两次同指纹 → 全部「仍通过」）
//   3. 浏览器：对比卡出现 A/B 下拉、汇总两列（用例数/三级成功率/pass^k/Run Error/MTTR/成本）、
//      分类徽章计数、逐 case 表带 data-class，且明确写「该 run 没有时间序列」
//   4. p99：报告该列是 n/a → 必须显示"没有可用的逐 case p99"，而不是画一条 0ms 的假条形
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

const ROOT = path.resolve(__dirname, '..');
const BUILD = process.env.BUILD_DIR || 'build';
const PORT = Number(process.env.CONSOLE_PORT || 8302);
const BASE = `http://127.0.0.1:${PORT}`;
const EVAL_OUT = process.env.EVAL_OUT || '/tmp/cue-e2e-compare';
const SHOT_DIR = process.env.SHOT_DIR || '/tmp/cue-e2e-compare-shots';
const KEEP = process.env.KEEP_RUNNING === '1';
const ALLOWED_CLASSES = new Set([
  'new_failure',
  'fixed',
  'still_failing',
  'still_passing',
  'inconclusive',
  'only_a',
  'only_b',
]);

const children = [];
const failures = [];

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const ok = (m) => console.log(`[e2e-compare] ok: ${m}`);
function fail(m) {
  failures.push(m);
  console.error(`[e2e-compare] FAIL: ${m}`);
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

function runDirs(dir) {
  if (!fs.existsSync(dir)) return [];
  return fs
    .readdirSync(dir)
    .filter((n) => n.startsWith('run-') && fs.existsSync(path.join(dir, n, 'report.md')))
    .sort();
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
  const evalBin = path.join(ROOT, BUILD, 'cue-eval');
  for (const bin of [serveBin, evalBin]) {
    if (!haveBin(bin)) {
      console.error(`[e2e-compare] 缺少 ${bin}\n先构建：cmake -S . -B ${BUILD} -DCMAKE_CXX_COMPILER=g++-14 && cmake --build ${BUILD} -j1`);
      process.exit(2);
    }
  }
  if (!fs.existsSync(path.join(ROOT, 'web', 'dist', 'index.html'))) {
    console.error('[e2e-compare] web/dist 不存在，先构建控制台：cd web/console && npm run build');
    process.exit(2);
  }
  let chromium;
  try {
    ({ chromium } = require('playwright'));
  } catch (e) {
    console.error(`[e2e-compare] 无法加载 playwright（NODE_PATH 要指向含它的 node_modules）：${e.message}`);
    process.exit(2);
  }
  fs.mkdirSync(SHOT_DIR, { recursive: true });

  // ---------------------------------------------------------------- 1. 两次 scripted 评测
  let runs = runDirs(EVAL_OUT);
  if (runs.length < 2) {
    fs.rmSync(EVAL_OUT, { recursive: true, force: true });
    fs.mkdirSync(EVAL_OUT, { recursive: true });
    for (let i = 1; i <= 2; i += 1) {
      const child = start(`cue-eval-${i}`, evalBin, [
        '--suite', 'suites/m4-diagnosis.json', '--mode', 'scripted', '--out', EVAL_OUT,
      ]);
      const done = await Promise.race([child.exited, sleep(180000).then(() => null)]);
      if (!done || done.code !== 0) {
        for (const { name, tail } of children) console.error(`--- ${name} ---\n${tail.join('')}`);
        fail(`第 ${i} 次 cue-eval 失败（${done ? `退出码 ${done.code}` : '180s 超时'}）`);
        await shutdown();
        process.exit(1);
      }
      ok(`第 ${i} 次 cue-eval 完成`);
      runs = runDirs(EVAL_OUT);
    }
  } else {
    ok(`复用 ${EVAL_OUT} 里已有的 ${runs.length} 份报告`);
  }
  console.log(`[e2e-compare] runs = ${JSON.stringify(runs)}`);
  if (runs.length < 2) {
    fail(`只有 ${runs.length} 份报告，无法对比`);
    process.exit(1);
  }
  const [runA, runB] = runs.slice(-2);

  // ---------------------------------------------------------------- 2. 起 cue-serve
  start('cue-serve', serveBin, ['--no-pipeline', '--port', String(PORT), '--web', 'web/dist', '--runs', EVAL_OUT]);
  const health = await waitHealth();
  if (!health || !health.ok) {
    for (const { name, tail } of children) console.error(`--- ${name} ---\n${tail.join('')}`);
    fail('cue-serve 没起来');
    await shutdown();
    process.exit(2);
  }
  ok(`cue-serve 就绪 ${BASE}（runs_dir=${EVAL_OUT}）`);

  // ---------------------------------------------------------------- 3. /api/runs + /api/diff
  const runsJson = await (await fetch(`${BASE}/api/runs`, { cache: 'no-store' })).json();
  const listed = (runsJson.runs || []).map((r) => r.dir);
  console.log(`[e2e-compare] /api/runs = ${JSON.stringify(listed)}`);
  if (listed.length < 2) fail(`/api/runs 只有 ${listed.length} 份报告`);
  const diff = await (await fetch(`${BASE}/api/diff?a=${runA}&b=${runB}`, { cache: 'no-store' })).json();
  console.log(`[e2e-compare] /api/diff counts = ${JSON.stringify(diff.counts)}`);
  console.log(`[e2e-compare] /api/diff comparable=${diff.comparable} warnings=${JSON.stringify(diff.warnings || [])}`);
  if (!diff.ok) fail(`/api/diff 失败：${diff.error}`);
  if (!diff.counts) fail('/api/diff 没有 counts');
  const caseCount = (diff.cases || []).length;
  if (caseCount < 2) fail(`/api/diff 只有 ${caseCount} 条 case（suite 有 2 个）`);
  if ((diff.counts?.still_passing || 0) !== caseCount) {
    fail(`两次同指纹 scripted 运行应全部「仍通过」：counts=${JSON.stringify(diff.counts)} case=${caseCount}`);
  }
  if (!diff.comparable) fail(`两次同指纹运行应可比：${JSON.stringify(diff.warnings)}`);
  // 没有时间序列这件事必须能从数据本身确认：state.jsonl 只有逐 case outcome
  const stateLines = fs
    .readFileSync(path.join(EVAL_OUT, runA, 'state.jsonl'), 'utf8')
    .split('\n')
    .filter(Boolean);
  console.log(`[e2e-compare] ${runA}/state.jsonl 行数=${stateLines.length}（header + 逐 case outcome，没有按秒采样）`);
  if (failures.length === 0) ok(`断言 2：/api/diff 给出 ${caseCount} 条 case，全部「仍通过」，config 可比`);

  // ---------------------------------------------------------------- 4. 浏览器对比页
  const browser2 = await chromium.launch({ args: ['--no-sandbox'] });
  const browser = browser2;
  const page = await browser.newPage({ viewport: { width: 1400, height: 1200 } });
  const pageErrors = [];
  page.on('pageerror', (e) => pageErrors.push(String(e && e.message ? e.message : e)));
  await page.goto(`${BASE}/`, { waitUntil: 'domcontentloaded' });
  await page.click('[data-testid="tab-runs"]');
  await page.waitForSelector('[data-testid="compare-card"]', { timeout: 15000 });
  await waitFor(async () => (await page.locator('[data-testid="diff-row"]').count()) > 0, 15000, '逐 case 对比表出现');

  const selA = await page.locator('[data-testid="diff-a"] option').count();
  const selB = await page.locator('[data-testid="diff-b"] option').count();
  console.log(`[e2e-compare] 下拉 A options=${selA} B options=${selB}`);
  if (selA < 2 || selB < 2) fail('A/B 下拉里的 run 少于 2 个');

  const summaryRows = await page.locator('[data-testid="diff-summary-row"]').allInnerTexts();
  console.log('[e2e-compare] 汇总行（指标 | A | B）：');
  console.log(summaryRows.map((t) => `  ${t.replace(/\n/g, ' | ')}`).join('\n'));
  const summaryText = summaryRows.join('\n');
  for (const k of ['用例数', '根因命中', '证据成立', 'SLA 达成', 'pass^k', 'Run Error', 'MTTR', '成本']) {
    if (!summaryText.includes(k)) fail(`汇总表缺少「${k}」`);
  }

  const diffRows = page.locator('[data-testid="diff-row"]');
  const rowCount = await diffRows.count();
  const classes = await diffRows.evaluateAll((els) => els.map((e) => e.getAttribute('data-class')));
  const classNames = await page.locator('[data-testid="diff-class"]').allInnerTexts();
  console.log(`[e2e-compare] 逐 case 行=${rowCount} data-class=${JSON.stringify(classes)} 结论文本=${JSON.stringify(classNames)}`);
  for (const c of classes) {
    if (!ALLOWED_CLASSES.has(c)) fail(`未知的对比分类 data-class=${c}`);
  }
  const counts = await page.locator('[data-testid^="diff-count-"]').allInnerTexts();
  console.log(`[e2e-compare] 分类计数徽章 = ${JSON.stringify(counts)}`);
  const stillPassing = await page.locator('[data-testid="diff-count-still_passing"]').innerText();
  console.log(`[e2e-compare] 不变（仍通过）徽章 = ${stillPassing}`);
  if (!/不变（仍通过）\s*\d+/.test(stillPassing)) fail(`「不变（仍通过）」徽章文本不符：${stillPassing}`);
  if (!classes.every((c) => c === 'still_passing')) fail(`两次同指纹运行应全为 still_passing：${JSON.stringify(classes)}`);

  const noTs = await page.locator('[data-testid="no-timeseries"]').innerText();
  if (!/没有时间序列/.test(noTs)) fail(`缺少「该 run 没有时间序列」的说明：${noTs}`);
  else ok('断言 3：对比表出现分类（不变/仍通过），并明确写了「该 run 没有时间序列」');

  // p99：报告该列是 n/a，所以必须是"没有可画的条形"，而不是 0ms 假条形
  const p99Empty = await page.locator('[data-testid="p99-empty"]').count();
  const p99Chart = await page.locator('[data-testid="p99-chart"]').count();
  console.log(`[e2e-compare] p99: 空态=${p99Empty} 图表=${p99Chart}`);
  if (p99Empty + p99Chart !== 1) fail('p99 区域应恰好是空态或图表之一');
  if (p99Empty === 1) {
    const t = await page.locator('[data-testid="p99-empty"]').innerText();
    if (!/没有可用的逐 case p99/.test(t)) fail(`p99 空态文案不符：${t}`);
    ok('断言 4：报告 p99 列为 n/a → 显示空态（不画 0ms 假条形）');
  }

  // 失败模式：scripted 全过 → 应明确写"无失败模式"，而不是空白
  const fmA = await page.locator('[data-testid="failure-modes-empty-a"]').count();
  const fmB = await page.locator('[data-testid="failure-modes-empty-b"]').count();
  console.log(`[e2e-compare] 失败模式空态 A=${fmA} B=${fmB}`);
  if (fmA !== 1 || fmB !== 1) fail('失败模式区域没有明确写出「无失败模式」');

  if (pageErrors.length > 0) fail(`页面抛错：${pageErrors.join('; ')}`);
  await page.screenshot({ path: path.join(SHOT_DIR, 'runs-compare.png'), fullPage: true });
  // 浏览器留到夹具阶段之后统一关闭：夹具复用同一个 chromium 实例。

  // ---------------------------------------------------------------- 5. 逐 case p99 并列条形（UI 夹具）
  // 真实 scripted 报告的 decode/op p99 全是 n/a（该段没采到分位），所以这一分支必须用夹具
  // 才能走到：复制两份真实 run，只把 report.md 的 decode p99 列填上数值。
  // 这里断言的是"条形按数值画出来了、A/B 长度不同"，**不是**这些数值本身——它们是造的。
  const fixture = `${EVAL_OUT}-fixture-p99`;
  fs.rmSync(fixture, { recursive: true, force: true });
  fs.mkdirSync(fixture, { recursive: true });
  const p99Vals = [
    [{ 'netloss-5pct': 42.5, 'netloss-10pct-gop30': 51.0 }, 'run-p99-a'],
    [{ 'netloss-5pct': 38.0, 'netloss-10pct-gop30': 46.5 }, 'run-p99-b'],
  ];
  p99Vals.forEach(([vals, name], i) => {
    const src = path.join(EVAL_OUT, i === 0 ? runA : runB);
    const dst = path.join(fixture, name);
    fs.cpSync(src, dst, { recursive: true });
    const mdPath = path.join(dst, 'report.md');
    const lines = fs.readFileSync(mdPath, 'utf8').split('\n');
    let header = null;
    const out = lines.map((ln) => {
      if (!ln.startsWith('|')) return ln;
      const cells = ln.split('|');
      if (!header && /decode p99/.test(ln)) {
        header = cells.map((c) => c.trim());
        return ln;
      }
      if (!header) return ln;
      const id = (cells[1] || '').trim();
      const idx = header.indexOf('decode p99');
      if (idx > 0 && vals[id] !== undefined) cells[idx] = ` ${vals[id]} `;
      return cells.join('|');
    });
    fs.writeFileSync(mdPath, out.join('\n'));
  });
  start('cue-serve-p99', serveBin, [
    '--no-pipeline', '--port', String(PORT + 1), '--web', 'web/dist', '--runs', fixture,
  ]);
  const health2 = await (async () => {
    const deadline = Date.now() + 15000;
    while (Date.now() < deadline) {
      try {
        const r = await fetch(`http://127.0.0.1:${PORT + 1}/api/health`, { cache: 'no-store' });
        if (r.ok) return await r.json();
      } catch {
        /* 还没起来 */
      }
      await sleep(200);
    }
    return null;
  })();
  if (!health2 || !health2.ok) {
    fail('p99 夹具用的 cue-serve 没起来');
    await browser2.close();
  }
  else {
    const page2 = await browser2.newPage({ viewport: { width: 1400, height: 1000 } });
    page2.on('pageerror', (e) => pageErrors.push(String(e && e.message ? e.message : e)));
    await page2.goto(`http://127.0.0.1:${PORT + 1}/`, { waitUntil: 'domcontentloaded' });
    await page2.click('[data-testid="tab-runs"]');
    await page2.waitForSelector('[data-testid="compare-card"]', { timeout: 15000 });
    await waitFor(async () => (await page2.locator('[data-testid="p99-row"]').count()) > 0, 15000, 'p99 条形出现');
    const rows = await page2.locator('[data-testid="p99-row"]').count();
    const widthsA = await page2.locator('[data-testid="p99-bar-a"]').evaluateAll((els) => els.map((e) => e.style.width));
    const widthsB = await page2.locator('[data-testid="p99-bar-b"]').evaluateAll((els) => els.map((e) => e.style.width));
    const labels = await page2.locator('.bar-val').allInnerTexts();
    console.log(`[e2e-compare] p99 夹具：rows=${rows} A=${JSON.stringify(widthsA)} B=${JSON.stringify(widthsB)} 标签=${JSON.stringify(labels)}`);
    if (rows < 2) fail(`p99 行数=${rows}，期望 2`);
    if (!widthsA.every((w) => /^\d+(\.\d+)?%$/.test(w)) || !widthsB.every((w) => /^\d+(\.\d+)?%$/.test(w))) {
      fail(`条形宽度不是百分比：A=${JSON.stringify(widthsA)} B=${JSON.stringify(widthsB)}`);
    }
    // 夹具里 A/B 数值不同 → 同一 case 的两条条形不允许等长（等长说明画的是同一份数据）
    for (let i = 0; i < Math.min(widthsA.length, widthsB.length); i += 1) {
      if (widthsA[i] === widthsB[i]) fail(`第 ${i} 个 case 的 A/B 条形等长（${widthsA[i]}），夹具数值本来就不同`);
    }
    if (!labels.some((l) => /ms$/.test(l))) fail(`条形旁没有 ms 数值：${JSON.stringify(labels)}`);
    if (pageErrors.length > 0) fail(`页面抛错：${pageErrors.join('; ')}`);
    if (failures.length === 0) ok('断言 5：有逐 case p99 时画出并列条形（A 蓝 / B 绿，按最大值归一）');
    await page2.screenshot({ path: path.join(SHOT_DIR, 'runs-compare-p99.png'), fullPage: true });
    await browser2.close();
  }
  await shutdown();

  if (failures.length > 0) {
    console.error(`[e2e-compare] ${failures.length} 项失败`);
    process.exit(1);
  }
  console.log('[e2e-compare] PASS');
  process.exit(0);
})();
