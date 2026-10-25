#!/usr/bin/env node
// AVC level 表的跨文件一致性检查（N-09）。
//
// 这不是浏览器行为测试。它只比较三份源码里的静态事实，不启动浏览器、不发网络请求、
// 不跑 WebCodecs：真正的浏览器行为（configure() 会不会抛异常、isConfigSupported 返回
// 什么）只有 scripts/e2e-*.cjs 那几条需要真 chromium 的测试能回答，本脚本一条都不碰。
//
// 为什么需要它：分辨率 → AVC level 这张表有两份平行实现——
//   1) C++ 采集端：include/cue/media/avc_level.h（阈值常量 + pick_avc_level 的返回值）
//   2) 浏览器端：web/app.js 的 pickCodec（阈值 + avc1.xxxxxx 的 codec 字符串）
// 两份必须对同一分辨率给出同一个 level，否则"同一分辨率在两条产线上编码参数一致"
// 这个前提就不成立。C++ 那份的边界由 tests/test_avc_level.cpp 钉住；本脚本负责钉住
// "两份不漂"。任何一侧单独改动都会在这里失败。
//
// 检查的三组事实（每组都打印实际读数，失败时指出文件与期望/实际）：
//   A. 阈值相等：两个 area 上限在头文件与 app.js 里必须逐一相等，且顺序一致；
//   B. idc ↔ 版本号：头文件里 `if (area <= kAvcLevel30MaxArea) return 30;` 的 idc，
//      喂给 avc_level_name() 的实现（/ 10、% 10、"."）必须得到 "3.0"、"4.0"，与
//      app.js 分支注释里的 level 名对应；
//   C. codec 字符串里的 level 十六进制与 idc 对应：avc1.42001E → level_idc = 0x1E = 30，
//      avc1.640028 → 0x28 = 40。注意 codec 串是 profile-level-id（AVCDecoderConfiguration-
//      Record 口径）：前 4 个 hex 是 profile_idc + constraint flags，**最后 2 个 hex 才是
//      level_idc**；4200/6400 与 level 无关，断言它们等于 30/40 是错的。
//
// 用法：
//   node scripts/check-avc-level.mjs        # 退出码 0 = 一致，1 = 有漂移，2 = 解析失败

import fs from 'node:fs';

const ROOT = new URL('../', import.meta.url);
const FILES = {
  header: new URL('include/cue/media/avc_level.h', ROOT),
  args: new URL('include/cue/media/ffmpeg_args.h', ROOT),
  web: new URL('web/app.js', ROOT),
};

const problems = [];
const readings = [];

function readUtf8(url) {
  try {
    return fs.readFileSync(url, 'utf8');
  } catch (e) {
    console.error(`解析失败：读不到 ${url.pathname}（${e.message}）`);
    process.exit(2);
  }
}

function fail(msg) {
  problems.push(msg);
}

function note(msg) {
  readings.push(msg);
}

// 从 `function foo(` / `int foo(` 的参数表之后取配平的 `{ ... }`，避免正则跨出函数体。
function bodyOf(src, marker, label) {
  const at = src.indexOf(marker);
  if (at < 0) {
    console.error(`解析失败：${label} 里找不到「${marker}」——函数被改名/搬家了？`);
    process.exit(2);
  }
  const open = src.indexOf('{', at);
  if (open < 0) {
    console.error(`解析失败：${label} 的「${marker}」后面没有函数体`);
    process.exit(2);
  }
  let depth = 0;
  for (let i = open; i < src.length; i += 1) {
    if (src[i] === '{') depth += 1;
    else if (src[i] === '}') {
      depth -= 1;
      if (depth === 0) return src.slice(open + 1, i);
    }
  }
  console.error(`解析失败：${label} 的「${marker}」函数体没有配平的右花括号`);
  process.exit(2);
}

// 头文件里的常量可能是 `414720` 或 `1620 * 256`：只接受数字与乘号，其余一律拒绝执行。
function evalAreaExpr(expr, where) {
  const cleaned = expr.trim();
  if (!/^[0-9\s*]+$/.test(cleaned)) {
    console.error(`解析失败：${where} 的值「${cleaned}」不是纯数字乘法表达式，拒绝求值`);
    process.exit(2);
  }
  return cleaned
    .split('*')
    .map((s) => Number.parseInt(s.trim(), 10))
    .reduce((a, b) => a * b, 1);
}

// ---------------------------------------------------------------- 读取三份源码

const headerSrc = readUtf8(FILES.header);
const argsSrc = readUtf8(FILES.args);
const webSrc = readUtf8(FILES.web);

// ---- C++ 侧：两个阈值常量 ----
function constOf(src, name) {
  const m = src.match(new RegExp(`inline\\s+constexpr\\s+int\\s+${name}\\s*=\\s*([^;]+);`));
  if (!m) {
    console.error(`解析失败：include/cue/media/avc_level.h 里找不到常量 ${name}`);
    process.exit(2);
  }
  return evalAreaExpr(m[1], name);
}

const headerL30 = constOf(headerSrc, 'kAvcLevel30MaxArea');
const headerL40 = constOf(headerSrc, 'kAvcLevel40MaxArea');

// ---- C++ 侧：pick_avc_level 里「哪个阈值 → 返回哪个 idc」 ----
const pickBody = bodyOf(headerSrc, 'pick_avc_level(', 'include/cue/media/avc_level.h');
const picker = [];
for (const m of pickBody.matchAll(
  /if\s*\(\s*area\s*<=\s*kAvcLevel(\d+)MaxArea\s*\)\s*return\s+(\d+)\s*;/g,
)) {
  picker.push({ tag: m[1], idc: Number.parseInt(m[2], 10) });
}
const fallback = pickBody.match(/return\s+(\d+)\s*;\s*$/);
if (picker.length !== 2 || !fallback) {
  console.error(
    `解析失败：pick_avc_level 的分支形状变了（if 分支 ${picker.length} 个、末尾 return ` +
      `${fallback ? fallback[1] : '缺失'}）。本脚本按「两个阈值分支 + 一个兜底 return」解析，` +
      '请同步更新 scripts/check-avc-level.mjs。',
  );
  process.exit(2);
}
const pickByTag = Object.fromEntries(picker.map((p) => [p.tag, p.idc]));

// ---- C++ 侧：avc_level_name 的 idc → 版本号实现 ----
const nameBody = bodyOf(argsSrc, 'avc_level_name(', 'include/cue/media/ffmpeg_args.h');
const div = nameBody.match(/level_idc\s*\/\s*(\d+)/);
const mod = nameBody.match(/level_idc\s*%\s*(\d+)/);
const sep = nameBody.match(/"([^"]*)"/g);
const hasAuto = /level_idc\s*<=\s*0\s*\)\s*return\s+"auto"/.test(nameBody);
if (!div || !mod || !sep || !hasAuto) {
  console.error(
    '解析失败：avc_level_name 的实现形状变了（找不到 level_idc / N、level_idc % N、' +
      '字符串字面量或 <= 0 → "auto" 分支）。请同步更新 scripts/check-avc-level.mjs。',
  );
  process.exit(2);
}
const divisor = Number.parseInt(div[1], 10);
const modulo = Number.parseInt(mod[1], 10);
// 拼接用的分隔符是函数体里最后一个字符串字面量（`"auto"` 与 `"."` 中取后者）。
const separator = sep[sep.length - 1].slice(1, -1);
function versionOf(idc) {
  return `${Math.trunc(idc / divisor)}${separator}${idc % modulo}`;
}

// ---- 浏览器侧：pickCodec 的两个阈值 + 两个 codec 字符串 ----
const pickCodecBody = bodyOf(webSrc, 'function pickCodec(', 'web/app.js');
const webBranches = [];
for (const m of pickCodecBody.matchAll(
  /if\s*\(\s*area\s*<=\s*(\d+)\s*\)\s*return\s*'([^']+)'\s*;/g,
)) {
  webBranches.push({ area: Number.parseInt(m[1], 10), codec: m[2] });
}
if (webBranches.length !== 2 || !/return\s+null\s*;/.test(pickCodecBody)) {
  console.error(
    `解析失败：web/app.js 的 pickCodec 形状变了（阈值分支 ${webBranches.length} 个、兜底 ` +
      `return null ${/return\s+null\s*;/.test(pickCodecBody) ? '存在' : '缺失'}）。` +
      '本脚本按「两个阈值分支 + return null」解析，请同步更新。',
  );
  process.exit(2);
}

// codec 串是 profile-level-id（6 个 hex）：前 4 个是 profile_idc + constraint flags，
// 最后 2 个是 level_idc。这里只解析 level 部分。
function levelOfCodec(codec, where) {
  const m = codec.match(/^avc1\.([0-9A-Fa-f]{6})$/);
  if (!m) {
    console.error(`解析失败：${where} 的 codec 串「${codec}」不是 avc1.PPCCLL 形状`);
    process.exit(2);
  }
  const hex = m[1].toUpperCase();
  return {
    profileBits: hex.slice(0, 4),
    levelHex: hex.slice(4),
    levelIdc: Number.parseInt(hex.slice(4), 16),
  };
}

const webL30 = { area: webBranches[0].area, ...levelOfCodec(webBranches[0].codec, 'web/app.js L3.0 分支') };
const webL40 = { area: webBranches[1].area, ...levelOfCodec(webBranches[1].codec, 'web/app.js L4.0 分支') };

// ---------------------------------------------------------------- 校验

// A. 阈值相等（跨文件，逐一配对，顺序也要一致）
const thresholdPairs = [
  { tag: '30', name: 'L3.0', cpp: headerL30, js: webL30 },
  { tag: '40', name: 'L4.0', cpp: headerL40, js: webL40 },
];
for (const p of thresholdPairs) {
  note(
    `阈值 ${p.name}：include/cue/media/avc_level.h = ${p.cpp}，` +
      `web/app.js = ${p.js.area}`,
  );
  if (p.cpp !== p.js.area) {
    fail(
      `阈值漂移（${p.name}）：期望两侧相等，实际 avc_level.h 的 ` +
        `kAvcLevel${p.tag}MaxArea = ${p.cpp}，而 web/app.js 的 pickCodec 用了 ${p.js.area}`,
    );
  }
}
if (headerL30 >= headerL40) {
  fail(`阈值顺序反了：kAvcLevel30MaxArea = ${headerL30} 必须小于 kAvcLevel40MaxArea = ${headerL40}`);
}
if (webL30.area >= webL40.area) {
  fail(`web/app.js 阈值顺序反了：${webL30.area}（L3.0）必须小于 ${webL40.area}（L4.0）`);
}

// B. idc ↔ 版本号（两边各自的 idc 与文案）
const idcPairs = [
  { tag: '30', name: 'L3.0', cppIdc: pickByTag['30'], js: webL30 },
  { tag: '40', name: 'L4.0', cppIdc: pickByTag['40'], js: webL40 },
];
for (const p of idcPairs) {
  const version = versionOf(p.cppIdc);
  note(
    `idc ${p.name}：pick_avc_level 返回 ${p.cppIdc}，` +
      `avc_level_name 写成 "${version}"，codec 串 level 0x${p.js.levelHex} = ${p.js.levelIdc}`,
  );
  const expectedVersion = `${p.tag[0]}.${p.tag[1]}`;
  if (version !== expectedVersion) {
    fail(
      `版本号不一致（${p.name}）：pick_avc_level 返回 idc ${p.cppIdc}，而 avc_level_name 的实现` +
        `（/ ${divisor}、% ${modulo}、分隔符 "${separator}"）把它写成 "${version}"，` +
        `期望 "${expectedVersion}"`,
    );
  }
}

// C. codec 串里的 level 十六进制 ↔ C++ 侧的 idc
for (const p of idcPairs) {
  if (p.js.levelIdc !== p.cppIdc) {
    fail(
      `codec level 与 idc 不对应（${p.name}）：include/cue/media/avc_level.h 的 pick_avc_level ` +
        `在该阈值分支返回 idc ${p.cppIdc}，而 web/app.js 的 codec 串 ` +
        `${webBranches[p.tag === '30' ? 0 : 1].codec} 里 level 字节是 0x${p.js.levelHex}` +
        ` = ${p.js.levelIdc}（前 4 个 hex ${p.js.profileBits} 是 profile 部分，与此无关）`,
    );
  }
}

// D. 与 AVC 附录 A 的换算自洽：MaxFS 以宏块计，1 宏块 = 16×16 = 256 样本。
//    这条不替代 A/B/C 的跨文件比对，只是拦住"两边同时改错"（比如把 L3.0 上限写成
//    415000），失败信息里直接给出宏块数，便于回查规范。
const spec = [
  { name: 'L3.0', area: headerL30, macroblocks: 1620, samplesPerMb: 256 },
  { name: 'L4.0', area: headerL40, macroblocks: 8192, samplesPerMb: 256 },
];
for (const s of spec) {
  const expected = s.macroblocks * s.samplesPerMb;
  note(`规范换算 ${s.name}：${s.macroblocks} 宏块 × ${s.samplesPerMb} 样本 = ${expected}，实测 ${s.area}`);
  if (s.area !== expected) {
    fail(`${s.name} 上限不是 AVC 附录 A 的换算值：期望 ${s.macroblocks}×${s.samplesPerMb} = ${expected}，实际 ${s.area}`);
  }
}

// E. 兜底 return 必须是 0（"超出已知表交给 x264 自动"，不是错误码）；app.js 对应 null。
if (Number.parseInt(fallback[1], 10) !== 0) {
  fail(
    `pick_avc_level 的兜底 return 是 ${fallback[1]}，期望 0（0 = 交给 x264 自动推导，不是错误码）`,
  );
}
note(`兜底：avc_level.h return ${fallback[1]}（0 = 交给 x264 自动），web/app.js return null`);

// ---------------------------------------------------------------- 输出

console.log('===== 读数 =====');
for (const r of readings) console.log(`  ${r}`);

if (problems.length === 0) {
  console.log(`===== PASS：${readings.length} 组读数一致，两份平行实现没有漂 =====`);
  process.exit(0);
}
console.log(`===== FAIL：${problems.length} 处不一致 =====`);
for (const p of problems) console.log(`  ✗ ${p}`);
console.log(
  '提示：改一侧就要同步另一侧——C++ 是 include/cue/media/avc_level.h，' +
    '浏览器是 web/app.js 的 pickCodec。',
);
process.exit(1);
