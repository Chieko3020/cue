// 真实浏览器端到端验证：headless Chromium 打开推流页 → 点击开始 → 断言确实在推流
// 用法：NODE_PATH=<playwright 的 node_modules> node scripts/e2e-browser.cjs [seconds]
//   页面地址可用 CUE_WEB_URL 覆盖（默认 http://127.0.0.1:8080/?ws_port=9000）
//   需先起 cued + 静态页（./scripts/run.sh demo-min）。
const { chromium } = require('playwright');

(async () => {
  const seconds = Number(process.argv[2] || 6);
  const url = process.env.CUE_WEB_URL || 'http://127.0.0.1:8080/?ws_port=9000';
  const browser = await chromium.launch({ args: ['--no-sandbox'] });
  const page = await browser.newPage();
  const consoleLogs = [];
  const pageErrors = [];
  page.on('console', (m) => consoleLogs.push(`[console] ${m.text()}`));
  page.on('pageerror', (e) => pageErrors.push(`[pageerror] ${e.message}`));

  await page.goto(url, { waitUntil: 'domcontentloaded' });
  await page.waitForTimeout(500);
  await page.click('#start');
  await page.waitForTimeout(seconds * 1000);

  const logText = await page.textContent('#log');
  // 页面只在推流运行期间暴露 __cueStats；拿不到本身就说明启动失败
  const stats = await page.evaluate(() => (window.__cueStats ? window.__cueStats() : null));
  const badLines = await page.evaluate(
    () => Array.from(document.querySelectorAll('#log .bad')).map((e) => e.textContent),
  );

  console.log('===== page log =====');
  console.log(logText);
  console.log('===== console =====');
  console.log(consoleLogs.slice(-10).join('\n'));
  console.log('===== stats =====');
  console.log(JSON.stringify(stats, null, 2));

  const failures = [];
  if (!stats) {
    failures.push('页面未暴露 __cueStats（脚本未执行或页面未加载）');
  } else {
    if (!(stats.sentFrames > 0)) failures.push(`sentFrames=${stats.sentFrames}，没有发出任何帧`);
    if (stats.encodeErrors !== 0) failures.push(`encodeErrors=${stats.encodeErrors}`);
    if (!stats.streaming) failures.push('streaming=false，推流已停止（断线未重连或编码器故障）');
    if (stats.offsetUs === 0) failures.push('offsetUs=0，时钟未同步');
  }
  if (badLines.length) failures.push(`页面有 ${badLines.length} 条 bad 日志：${badLines.join(' | ')}`);
  if (pageErrors.length) failures.push(`页面异常：${pageErrors.join(' | ')}`);

  await browser.close();

  if (failures.length) {
    console.error('E2E FAIL:\n - ' + failures.join('\n - '));
    process.exit(1);
  }
  console.log(`E2E PASS: ${seconds}s 内发送 ${stats.sentFrames} 帧，编码错误 0`);
})().catch((e) => { console.error('E2E FAIL:', e.message); process.exit(1); });
