#!/usr/bin/env node
// Windows 侧中继：TCP 流 → 解 4 字节长度前缀 → UDP 数据报给 cue-probe。
//
// 两个坑都是实测踩出来的，改之前先看清楚：
//
// 1) **不能用 Buffer.concat 累积**。第一版这么写，高码率下每次 data 都整块复制，
//    复杂度 O(n²)，CPU 打满 → TCP 反压 → 中继侧丢包，8% 的包就这么没了。
//    改成"积压缓冲 + 偏移游标"，只在容量不够时扩容、只在消费后 compact。
//
// 2) **交给 dgram.send 的必须是副本，不能是 acc 的视图**。
//    dgram.send 是异步的：缓冲区满时那个 Buffer 会被 libuv 挂在待发送队列里，
//    而本循环紧接着就会用 acc.copy(acc, 0, pos, off) 把同一块内存往前搬。
//    结果就是"待发送的包在真正发出之前内容被改写了"——接收端看到的是头部完好、
//    载荷损坏的帧（cue-probe 侧表现为每一帧 send_packet failed: -1094995529，
//    也就是 AVERROR_INVALIDDATA，画面停在最后一帧，而帧数指标全都正常）。
//    发送端一次写很多包时这个竞态才暴露；一次只发一两个包时几乎撞不上，
//    所以它在"小流量正常、正常码率花屏"这种形态里非常容易被误判成编码问题。
//
// 用法：udp-relay.js [tcp_port] [udp_port]
const net = require('node:net');
const dgram = require('node:dgram');

const tcpPort = Number(process.argv[2] || 19999);
const udpPort = Number(process.argv[3] || 9971);

const udp = dgram.createSocket('udp4');
udp.bind(() => {
  try { udp.setRecvBufferSize(4 * 1024 * 1024); } catch (_) {}
});

let acc = Buffer.alloc(256 * 1024);
let off = 0;
let n = 0;
let bad = 0;

net.createServer((sock) => {
  sock.setNoDelay(true);
  sock.on('data', (chunk) => {
    if (off + chunk.length > acc.length) {
      const need = off + chunk.length;
      const next = Buffer.alloc(Math.max(acc.length * 2, need));
      acc.copy(next, 0, 0, off);
      acc = next;
    }
    chunk.copy(acc, off);
    off += chunk.length;

    let pos = 0;
    while (off - pos >= 4) {
      const len = acc.readUInt32BE(pos);
      // 上界检查：一个长度前缀就把游标带出缓冲区，说明流已经错位，
      // 继续按这个长度切片只会把后面的包全切碎。宁可丢连接重建，也不要"看起来在转"。
      if (len > 65507) {
        bad += 1;
        console.log('长度前缀异常 (' + len + ')，重建连接');
        sock.destroy();
        acc = Buffer.alloc(256 * 1024);
        off = 0;
        return;
      }
      if (off - pos - 4 < len) break;
      // ★ 必须复制：见文件头第 2 条
      udp.send(Buffer.from(acc.subarray(pos + 4, pos + 4 + len)), udpPort, '127.0.0.1');
      pos += 4 + len;
      n++;
    }
    if (pos > 0) {
      acc.copy(acc, 0, pos, off);
      off -= pos;
    }
  });
  sock.on('error', () => {});
  sock.on('close', () => {
    // 连接结束时的残留半包不能再留给下一个连接，否则新流从错位处开始解析。
    if (off > 0) {
      console.log('连接关闭，丢弃残留 ' + off + ' B');
      off = 0;
    }
  });
}).listen(tcpPort, '127.0.0.1', () => {
  console.log('relay TCP:' + tcpPort + ' -> UDP:' + udpPort);
});

setInterval(() => console.log('relayed ' + n + ' (bad ' + bad + ', pending ' + off + ')'), 5000);
