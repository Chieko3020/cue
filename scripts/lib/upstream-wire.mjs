// 上行媒体包打包 —— Node 侧唯一实现。
//
// 为什么单独一个模块：浏览器推流页（web/app.js）不能 import .mjs，但 Node 侧至少要让
// "浏览器模拟端"（scripts/test-client.mjs）与协议对照脚本（scripts/compare-upstream-wire.mjs）
// 用**同一份**分片逻辑。这样 C++ 采集端与 Node 模拟端之间的逐字节对照才有意义
// （否则对照的只是两份手抄本，抄错的地方两边一起错）。
//
// 规则必须与 include/cue/proto/upstream_pack.h 完全一致：
//   frag_count = max(1, ceil(len / 16 KiB))；空载荷也是 1 片、payload_len = 0
//   flags：每片都带 keyframe 位（bit0）；只有最后一片带 last_frag 位（bit2）
//   stream_id/frame_id/frag_index/frag_count/pts_us/payload_len 全部小端

export const HEADER = 32;
export const MAX_FRAG = 16 * 1024;

export const TYPE_VIDEO = 0x01;
export const TYPE_AUDIO = 0x02;
export const TYPE_CODEC_CONFIG = 0x03;
export const TYPE_KEYFRAME_REQ = 0x04;
export const TYPE_STATS = 0x05;

export const CODEC_H264 = 0x01;
export const CODEC_VP8 = 0x02;
export const CODEC_OPUS = 0x03;

/**
 * 把一帧已编码载荷切成上行包。
 * @param {{type:number, codec:number, streamId:number, frameId:number, keyframe:boolean, ptsUs:number}} meta
 * @param {Uint8Array} bytes
 * @returns {Uint8Array[]} 完整的上行包（32 字节头 + 分片载荷）
 */
export function packMediaPackets(meta, bytes) {
  const fragCount = Math.max(1, Math.ceil(bytes.length / MAX_FRAG));
  const packets = [];
  for (let i = 0; i < fragCount; i++) {
    const off = i * MAX_FRAG;
    const len = Math.min(MAX_FRAG, bytes.length - off);
    const pkt = new Uint8Array(HEADER + len);
    const dv = new DataView(pkt.buffer);
    pkt[0] = 0xc1;
    pkt[1] = meta.type;
    pkt[2] = meta.codec;
    pkt[3] = (meta.keyframe ? 0x01 : 0) | (i === fragCount - 1 ? 0x04 : 0);
    dv.setUint32(4, meta.streamId, true);
    dv.setUint32(8, meta.frameId, true);
    dv.setUint32(12, i, true);
    dv.setUint32(16, fragCount, true);
    dv.setBigUint64(20, BigInt(meta.ptsUs), true);
    dv.setUint32(28, len, true);
    if (len > 0) pkt.set(bytes.subarray(off, off + len), HEADER);
    packets.push(pkt);
  }
  return packets;
}
