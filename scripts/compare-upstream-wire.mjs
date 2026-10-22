#!/usr/bin/env node
// 协议对照：原生采集端（C++ proto::pack_upstream）⇄ 浏览器模拟端（JS upstream-wire.mjs）
//
// 用法：
//   build/cue-capture --dump-wire /tmp/cue-wire.jsonl
//   node scripts/compare-upstream-wire.mjs /tmp/cue-wire.jsonl
//
// 对照的是**完整包**（32 字节头 + 分片载荷），逐字节；不只是"字段名一样"。
// 两侧输入完全相同：同一个 payload 长度、同一份 meta、同样的 0xA5 填充。
// 退出码非 0 表示有字节不一致（可以直接放进 CI）。

import fs from 'node:fs';
import { packMediaPackets } from './lib/upstream-wire.mjs';

const path = process.argv[2];
if (!path) {
  console.error('用法：node scripts/compare-upstream-wire.mjs <cue-capture --dump-wire 输出文件>');
  process.exit(2);
}

const lines = fs.readFileSync(path, 'utf8').split('\n').filter((l) => l.trim() !== '');
if (lines.length === 0) {
  console.error('没有读到任何 case');
  process.exit(2);
}

function u32(pkt, off) {
  return Buffer.from(pkt.buffer, pkt.byteOffset + off, 4).readUInt32LE(0);
}
function u64(pkt, off) {
  return Buffer.from(pkt.buffer, pkt.byteOffset + off, 8).readBigUInt64LE(0).toString();
}
function bytesEqual(a, b) {
  if (a.length !== b.length) return false;
  return Buffer.from(a.buffer, a.byteOffset, a.length).equals(
    Buffer.from(b.buffer, b.byteOffset, b.length),
  );
}

// 32 字节头的字段表（wire.h 的布局；offset/size 是与 C++ 无关的独立读数）
const FIELDS = [
  { offset: 0, name: 'magic', size: 1, get: (p) => '0x' + p[0].toString(16).padStart(2, '0') },
  { offset: 1, name: 'type', size: 1, get: (p) => '0x' + p[1].toString(16).padStart(2, '0') },
  { offset: 2, name: 'codec', size: 1, get: (p) => '0x' + p[2].toString(16).padStart(2, '0') },
  { offset: 3, name: 'flags', size: 1, get: (p) => '0x' + p[3].toString(16).padStart(2, '0') },
  { offset: 4, name: 'stream_id', size: 4, get: (p) => String(u32(p, 4)) },
  { offset: 8, name: 'frame_id', size: 4, get: (p) => String(u32(p, 8)) },
  { offset: 12, name: 'frag_index', size: 4, get: (p) => String(u32(p, 12)) },
  { offset: 16, name: 'frag_count', size: 4, get: (p) => String(u32(p, 16)) },
  { offset: 20, name: 'pts_us', size: 8, get: (p) => u64(p, 20) },
  { offset: 28, name: 'payload_len', size: 4, get: (p) => String(u32(p, 28)) },
];

let failures = 0;
let cases = 0;
let printedTable = false;

for (const line of lines) {
  const c = JSON.parse(line);
  const name = c.case;
  cases++;
  const meta = {
    type: c.meta.type,
    codec: c.meta.codec,
    streamId: c.meta.stream_id,
    frameId: c.meta.frame_id,
    keyframe: c.meta.keyframe,
    ptsUs: c.meta.pts_us,
  };
  const payload = Buffer.alloc(c.payload_len, 0xa5);
  const js = packMediaPackets(meta, payload);

  const problems = [];
  if (js.length !== c.frag_count) problems.push(`frag_count C++=${c.frag_count} JS=${js.length}`);
  if (js.length !== c.packets.length) {
    problems.push(`packet count C++=${c.packets.length} JS=${js.length}`);
  }
  let allBytesEqual = true;
  const n = Math.min(js.length, c.packets.length);
  for (let i = 0; i < n; i++) {
    const cpp = Buffer.from(c.packets[i], 'hex');
    if (!bytesEqual(cpp, js[i])) {
      allBytesEqual = false;
      let at = -1;
      for (let k = 0; k < Math.min(cpp.length, js[i].length); k++) {
        if (cpp[k] !== js[i][k]) { at = k; break; }
      }
      problems.push(`packet[${i}] 字节不一致（首个差异 offset=${at}）`);
    }
  }
  if (!allBytesEqual) problems.push('完整包字节不一致');

  // 代表 case 打印逐字段表：多分片 + 关键帧，覆盖 frag_index/frag_count/last_frag
  if (!printedTable && name === 'frag-plus-one') {
    printedTable = true;
    const cpp0 = Buffer.from(c.packets[0], 'hex');
    const js0 = js[0];
    console.log(`\n逐字段对照（case=${name}，payload ${c.payload_len} B，${c.frag_count} 片，取第 0 片）`);
    console.log('offset  field        size  C++                     JS                      match');
    for (const f of FIELDS) {
      const a = f.get(cpp0);
      const b = f.get(js0);
      console.log(
        String(f.offset).padEnd(8) + f.name.padEnd(13) + String(f.size).padEnd(6) +
        a.padEnd(24) + b.padEnd(24) + (a === b ? 'yes' : 'NO'),
      );
    }
    console.log('\n分片语义（每片 flags / payload_len / 与 JS 的完整包字节比较）');
    for (let i = 0; i < Math.min(c.packets.length, js.length); i++) {
      const cpp = Buffer.from(c.packets[i], 'hex');
      console.log(
        `  frag[${i}] flags=0x${cpp[3].toString(16).padStart(2, '0')} ` +
        `payload_len=${u32(cpp, 28)} bytes_equal=${bytesEqual(cpp, js[i]) ? 'yes' : 'NO'}`,
      );
    }
    console.log('');
  }

  console.log(
    `case=${String(name).padEnd(13)} payload_len=${String(c.payload_len).padEnd(7)} ` +
    `frags C++/JS=${c.frag_count}/${js.length} ` +
    `bytes_equal=${allBytesEqual ? 'yes' : 'NO'}` +
    (problems.length ? '  ← ' + problems.join('; ') : ''),
  );
  if (problems.length) failures++;
}

console.log(`\n对照 ${cases} 个 case：${failures === 0 ? '全部逐字节一致' : failures + ' 个不一致'}`);
process.exit(failures === 0 ? 0 : 1);
