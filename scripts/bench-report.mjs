#!/usr/bin/env node
// 把 scripts/bench.sh 收集的原始日志整理成 Markdown 表格。
// 用法：node scripts/bench-report.mjs <run_dir>
//   读取 <run_dir>/run.json（场景清单与环境）与 raw/<scenario>.{cued,probe,client}.log
//   输出 Markdown 到 stdout（bench.sh 负责落盘到 bench/<timestamp>.md）
//
// 所有数字都来自被测量进程自己打印的指标行，脚本不做任何估算/补零：
//   * cued "METRICS {json}"（每秒一行，环形缓冲累积到 4096 样本，取最后一行的分位）
//   * probe "[probe] fps=... frames=... fec_rec=... nack=... loss=..."
//   * probe "[cue-event] {\"ev\":\"probe.metric\",...}"（端到端 p50/p99，需时钟已对齐）
//   * client 的 JSON 摘要（实际发出帧数）
import fs from 'node:fs';
import path from 'node:path';

const runDir = process.argv[2];
if (!runDir) {
  console.error('用法: node scripts/bench-report.mjs <run_dir>');
  process.exit(2);
}
const meta = JSON.parse(fs.readFileSync(path.join(runDir, 'run.json'), 'utf8'));

const readIf = (p) => (fs.existsSync(p) ? fs.readFileSync(p, 'utf8') : '');
const lastCuedMetrics = (log) => {
  const lines = log.split('\n').filter((l) => l.startsWith('METRICS '));
  if (lines.length === 0) return null;
  try {
    return JSON.parse(lines[lines.length - 1].slice('METRICS '.length));
  } catch {
    return null;
  }
};

const RE_PROBE = /\[probe\] fps=([\d.]+) frames=(\d+) lost=(\d+) pend=\d+ pkts=(\d+) nack=(\d+) pause=(\d+) fec_rec=(\d+) rej=(\d+) loss=([\d.]+)%/;
const lastProbe = (log) => {
  const ms = [...log.matchAll(new RegExp(RE_PROBE, 'g'))];
  if (ms.length === 0) return null;
  const m = ms[ms.length - 1];
  return {
    fps: Number(m[1]), frames: Number(m[2]), lost: Number(m[3]), packets: Number(m[4]),
    nack: Number(m[5]), nack_pauses: Number(m[6]), fec_rec: Number(m[7]),
    rejected: Number(m[8]), loss_pct: Number(m[9]),
  };
};
// 分片丢失率：probe 每秒打印一次窗口值（loss=NN%），收尾静默窗口是 0% 且没有包到达。
// 只把「pkts 有增长」的窗口计入，避免把空闲窗口当成 0 丢失拉低均值。
const windowLoss = (log) => {
  const vals = [];
  let prevPkts = null;
  for (const line of log.split('\n')) {
    const m = line.match(RE_PROBE);
    if (!m) continue;
    const pkts = Number(m[4]);
    if (prevPkts !== null && pkts > prevPkts) vals.push(Number(m[9]));
    prevPkts = pkts;
  }
  if (vals.length === 0) return null;
  return vals.reduce((a, b) => a + b, 0) / vals.length;
};

const lastProbeLat = (log) => {
  // probe.metric 事件里的 lat_p50/lat_p99 是「已对齐时钟后」的端到端延迟（ms）
  let last = null;
  for (const line of log.split('\n')) {
    const at = line.indexOf('[cue-event] ');
    if (at < 0) continue;
    try {
      const ev = JSON.parse(line.slice(at + '[cue-event] '.length));
      if (ev.ev === 'probe.metric') last = ev.data;
    } catch { /* 截断行忽略 */ }
  }
  return last;
};

const data = meta.scenarios.map((s) => {
  const base = path.join(runDir, 'raw', s.label);
  const cued = lastCuedMetrics(readIf(`${base}.cued.log`));
  const probeLog = readIf(`${base}.probe.log`);
  const probe = lastProbe(readIf(`${base}.probe.log`));
  const lat = lastProbeLat(probeLog);
  const lossWindow = windowLoss(probeLog);
  const client = JSON.parse(readIf(`${base}.client.json`) || '{}');
  return { s, cued, probe, lat, lossWindow, client };
});

const f = (v, d = 3, suffix = '') => (v === null || v === undefined || Number.isNaN(v) ? 'n/a' : `${Number(v).toFixed(d)}${suffix}`);
const stage = (cued, name, p) => (cued && cued[name] ? f(cued[name][p], 3) : '-');
const pct = (num, den) => (den > 0 ? (num / den) * 100 : NaN);

const lines = [];
lines.push(`# Cue benchmark ${meta.stamp}`);
lines.push('');
lines.push(`- 生成时间：${meta.iso}`);
lines.push(`- 主机：${meta.host}（${meta.nproc} vCPU，${meta.mem_gib} GiB）`);
lines.push(`- 构建：\`${meta.build_dir}\`（${meta.build_type}，${meta.compiler}）`);
lines.push(`- 样本：${meta.sample.name}，${meta.sample.width}x${meta.sample.height} @${meta.sample.fps}fps，`
  + `循环 ${meta.sample.frames} 帧 / 平均 ${meta.sample.avg_frame_bytes} B/帧 / `
  + `${meta.sample.avg_fragments} 个下行数据报每帧`);
lines.push(`- 每场景：客户端推 ${meta.seconds_per_run} s（${meta.fps} fps 目标），结束后再等 ${meta.settle_secs} s 让 probe 结算未完成帧`);
lines.push(`- 注入：cued \`--inject-drop <pct>\`（分片后、sendto 前，\`splitmix64(seed=42, 包序号)\` 单线程决策）`);
lines.push('');

// ---- 表 1/2：六段耗时分位（无故障）----
const clean = data.filter((d) => d.s.drop_pct === 0);
const row = (d, p) => `| ${d.s.label} | \`--transcode ${d.s.transcode}\` | ${stage(d.cued, 'recv', p)} | ${stage(d.cued, 'buffer', p)} | `
  + `${stage(d.cued, 'decode', p)} | ${stage(d.cued, 'op', p)} | ${stage(d.cued, 'encode', p)} | ${stage(d.cued, 'send', p)} |`
  + ` ${d.lat ? f(d.lat.lat_p50, 2) : 'n/a'} / ${d.lat ? f(d.lat.lat_p99, 2) : 'n/a'} | ${f(d.cued && d.cued.fps, 1)} |`;

for (const [title, p] of [['p50', 'p50'], ['p99', 'p99']]) {
  lines.push(`## cued 六段耗时 ${title}（无故障，单位 ms）`);
  lines.push('');
  lines.push('| 场景 | 档位 | recv | buffer | decode | op | encode | send | probe 端到端 p50 / p99 (ms) | cued fps |');
  lines.push('|---|---|---|---|---|---|---|---|---|---|');
  for (const d of clean) lines.push(row(d, p));
  lines.push('');
  lines.push('`encode` 为 `-` 表示该段没有被采样（passthrough 不编码），不是 0 ms。');
  lines.push('');
}
lines.push('> `recv` 与 `buffer` 同口径（M2a 尚未拆出抖动缓冲）：都是「帧内首片到达 → 重组完成」。');
lines.push('');

// ---- 表 3：5% 丢包的帧完成率 ----
const lossy = data.filter((d) => d.s.drop_pct > 0);
lines.push(`## ${meta.sample.drop_pct_for_lossy || 5}% 丢包下的帧完成率与 FEC/NACK 恢复`);
lines.push('');
lines.push('| 配置 | FEC | NACK | 客户端发帧 | probe 完成帧 | 帧完成率 | probe 判丢帧 | FEC 恢复分片 | NACK 请求 | 分片丢失率（窗口均值） |');
lines.push('|---|---|---|---|---|---|---|---|---|---|');
for (const d of lossy) {
  const sent = d.client.frames_sent;
  const done = d.probe ? d.probe.frames : null;
  const rate = sent && done !== null ? pct(done, sent) : NaN;
  lines.push(`| ${d.s.label} | ${d.s.fec ? 'on' : 'off'} | ${d.s.nack ? 'on' : 'off'} | ${f(sent, 0)} | ${f(done, 0)} | `
    + `${Number.isNaN(rate) ? 'n/a' : rate.toFixed(2) + '%'} | ${f(d.probe && d.probe.lost, 0)} | `
    + `${f(d.probe && d.probe.fec_rec, 0)} | ${f(d.probe && d.probe.nack, 0)} | ${f(d.probe && d.probe.loss_pct, 2, '%')} |`);
}
lines.push('');
lines.push('帧完成率 = probe 完整重组的帧数 / 客户端实际发出的帧数（端到端，含 cued 侧令牌桶/切片失败丢的帧）。');
lines.push('probe 判丢帧 = 2 s 内未凑齐分片而超时结算的帧数；'
  + '分片丢失率 = probe 每个 1 s 窗口的 loss%（只统计有包到达的窗口）的均值。');
lines.push('');

// ---- 表 4：恢复率（相对无恢复基线）----
const baseline = lossy.find((d) => !d.s.fec && !d.s.nack);
if (baseline && baseline.client.frames_sent && baseline.probe) {
  const bDone = baseline.probe.frames;
  const bSent = baseline.client.frames_sent;
  const baseRate = bDone / bSent;
  lines.push('## 恢复率（相对「无恢复」基线）');
  lines.push('');
  lines.push('| 配置 | 帧完成率 | 相对无恢复的恢复率 | 说明 |');
  lines.push('|---|---|---|---|');
  for (const d of lossy) {
    const sent = d.client.frames_sent;
    const done = d.probe ? d.probe.frames : null;
    const rate = done === null || !sent ? NaN : done / sent;
    const rec = Number.isNaN(rate) ? NaN : (rate - baseRate) / (1 - baseRate);
    const desc = d.s.fec && d.s.nack ? 'FEC + NACK' : d.s.fec ? '仅 FEC' : d.s.nack ? '仅 NACK' : '基线·无恢复';
    lines.push(`| ${d.s.label}（${desc}） | ${Number.isNaN(rate) ? 'n/a' : (rate * 100).toFixed(2) + '%'} | `
      + `${Number.isNaN(rec) ? 'n/a' : (rec * 100).toFixed(1) + '%'} | `
      + `${desc.startsWith('基线') ? '分母' : '在无恢复时会丢掉的帧中，被恢复机制补回来的比例'} |`);
  }
  lines.push('');
  lines.push('恢复率 =（本配置完成率 − 基线完成率）/（1 − 基线完成率）。分母是「无恢复时本会丢掉的帧」。');
  lines.push('');
}

// ---- 原始计数（便于复核）----
lines.push('## 原始计数');
lines.push('');
lines.push('| 场景 | cued frames_in | cued frames_out | cued injected | budget_drops | encode_errors | probe pkts | probe 完成帧 |');
lines.push('|---|---|---|---|---|---|---|---|');
for (const d of data) {
  const c = d.cued || {};
  lines.push(`| ${d.s.label} | ${f(c.frames_in, 0)} | ${f(c.frames_out, 0)} | ${f(c.injected, 0)} | `
    + `${f(c.budget_drops, 0)} | ${f(c.encode_errors, 0)} | ${f(d.probe && d.probe.packets, 0)} | ${f(d.probe && d.probe.frames, 0)} |`);
}
lines.push('');
process.stdout.write(lines.join('\n'));
