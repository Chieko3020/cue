// M1 真实浏览器端到端验证：headless Chromium 打开控制台页面 → 点击开始 → 收集页面日志
// 用法：NODE_PATH=<playwright 的 node_modules> node scripts/e2e-browser.cjs [seconds]
const { chromium } = require('playwright');

(async () => {
  const seconds = Number(process.argv[2] || 6);
  const browser = await chromium.launch({ args: ['--no-sandbox'] });
  const page = await browser.newPage();
  const consoleLogs = [];
  page.on('console', (m) => consoleLogs.push(`[console] ${m.text()}`));
  page.on('pageerror', (e) => consoleLogs.push(`[pageerror] ${e.message}`));

  await page.goto('http://127.0.0.1:8080/', { waitUntil: 'domcontentloaded' });
  await page.waitForTimeout(500);
  await page.click('#start');
  await page.waitForTimeout(seconds * 1000);

  const logText = await page.textContent('#log');
  const stats = await page.evaluate(() => ({
    ua: navigator.userAgent,
    sentFrames: typeof sentFrames !== 'undefined' ? sentFrames : null,
  })).catch(() => ({}));

  console.log('===== page log =====');
  console.log(logText);
  console.log('===== console =====');
  console.log(consoleLogs.slice(-10).join('\n'));
  console.log('===== env =====');
  console.log('UA:', stats.ua);

  await browser.close();
})().catch((e) => { console.error('E2E FAIL:', e.message); process.exit(1); });
