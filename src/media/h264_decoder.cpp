#include <cstring>
#include <limits>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

#include "cue/media/h264_decoder.h"

namespace cue::media {

namespace {

// 一包 Annex-B 里到底有什么。只做分诊，不解码。
struct NalScan {
  bool any = false;  // 至少见到一个起始码（否则这包根本不是 Annex-B）
  bool vcl = false;  // 有 VCL（类型 1..5）：这一包才携带画面数据
  bool idr = false;  // 有 IDR(5)：唯一能重建参考链的 NAL
  bool sps = false;  // 7
  bool pps = false;  // 8
};

// 起始码判据与 probe 的 scan_param_sets 一致：4 字节优先，否则 3 字节。
// 为什么不能只找 00 00 01：00 00 00 01 会被读成「00 00 01 + 一个多余的 0」，
// 那个 0 会被当成 NAL 头，类型就全错位了。
NalScan scan_nals(std::span<const uint8_t> in) {
  NalScan s;
  for (std::size_t i = 0; i + 3 <= in.size();) {
    std::size_t sc = 0;
    if (i + 4 <= in.size() && in[i] == 0 && in[i + 1] == 0 && in[i + 2] == 0 && in[i + 3] == 1) {
      sc = 4;
    } else if (in[i] == 0 && in[i + 1] == 0 && in[i + 2] == 1) {
      sc = 3;
    }
    if (sc == 0) {
      ++i;
      continue;
    }
    s.any = true;
    const std::size_t header = i + sc;
    if (header < in.size()) {
      const uint8_t type = static_cast<uint8_t>(in[header] & 0x1F);
      if (type >= 1 && type <= 5) s.vcl = true;
      if (type == 5) s.idr = true;
      if (type == 7) s.sps = true;
      if (type == 8) s.pps = true;
    }
    i = header + 1;
  }
  return s;
}

}  // namespace

H264Decoder::~H264Decoder() { close(); }

bool H264Decoder::open(std::string& err) {
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
  if (!codec) {
    err = "h264 decoder not found";
    return false;
  }
  ctx_ = avcodec_alloc_context3(codec);
  if (!ctx_) {
    err = "avcodec_alloc_context3 failed";
    return false;
  }
  // 低延迟：不缓冲多帧；2 vCPU 下单线程解码足够
  ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
  ctx_->thread_count = 1;
  if (avcodec_open2(ctx_, codec, nullptr) < 0) {
    err = "avcodec_open2 failed";
    close();
    return false;
  }
  frame_ = av_frame_alloc();
  last_ = av_frame_alloc();
  pkt_ = av_packet_alloc();
  if (!frame_ || !last_ || !pkt_) {
    err = "av_frame_alloc/av_packet_alloc failed";
    close();
    return false;
  }
  // 新解码器没有任何参考帧，也没见过参数集：第一个包必须自带 SPS/PPS 且是 IDR。
  waiting_keyframe_ = true;
  saw_sps_ = false;
  saw_pps_ = false;
  return true;
}

void H264Decoder::close() {
  if (pkt_) av_packet_free(&pkt_);
  if (last_) av_frame_free(&last_);
  if (frame_) av_frame_free(&frame_);
  if (ctx_) avcodec_free_context(&ctx_);
  // 与 open() 同一初始态：close 之后再 open 要像一个全新解码器那样要求 IDR。
  waiting_keyframe_ = true;
  saw_sps_ = false;
  saw_pps_ = false;
}

AVFrame* H264Decoder::decode(std::span<const uint8_t> data, std::string& err) {
  err.clear();  // 调用方常复用同一个 string：不清空会读到上一次的旧错误
  // 上一张输出帧的引用必须在这里放掉，**不能**留到下一次 drain 里。
  // avcodec_send_packet 内部会对「调用方仍持有的上一张输出帧」再做一次 av_frame_ref，
  // 我们若把它留到下一轮才 unref，解码器每帧就多留一个完整帧缓冲，引用计数永远回不到 0。
  // 泄漏量正好是一帧 YUV：1080p 约 3.1 MB/帧，实测 cued 与 cue-probe 都按这个速率
  // 涨到 GB 级（1970 MB 的机器上 7 秒涨 666 MB），最终被 cgroup OOM 杀掉，表现为
  // 「画面声音突然冻住、WS 断开、采集端退出」。
  // 这里放掉是符合接口约定的：返回值只在「下次 decode 之前」有效。
  if (last_) av_frame_unref(last_);
  if (!ctx_ || data.empty()) return nullptr;

  // static_cast<int> 溢出是 UB，而 av_new_packet 拿到负长度只会失败得更莫名其妙。
  // 一帧 H.264 到不了 2 GiB，超限就是调用方传错了东西，如实报错比截断好。
  if (data.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    err = "packet too large (over INT_MAX)";
    ++stats_.errors;
    ++stats_.dropped;
    return nullptr;
  }

  // ---- 参考链状态机 ----
  // 只在"等 IDR"期间扫 NAL：稳态下每帧扫一遍几十万字节是白烧 CPU，而稳态下
  // 我们不需要知道这一包里有什么（参数集/分辨率变化由解码器自己处理）。
  if (waiting_keyframe_) {
    const NalScan scan = scan_nals(data);
    if (!scan.any) {
      // 没有起始码 = 不是 Annex-B（可能是 AVCC 长度前缀形态）。喂进去只会每帧换回
      // 一个 AVERROR_INVALIDDATA（实测 -1094995529），把真正的"缺参数集"淹掉。
      err = "packet is not Annex-B (no start code)";
      ++stats_.errors;
      ++stats_.dropped;
      return nullptr;
    }
    if (scan.sps) saw_sps_ = true;
    if (scan.pps) saw_pps_ = true;

    if (!scan.vcl) {
      // 只有参数集/SEI/AUD 的包：照喂（解码器把参数集收进去），但不产帧、也不算丢，
      // 且**保持**等 IDR——参数集自己不能建立参考链。
    } else if (!scan.idr) {
      // 非 IDR 的 VCL：参考链已经断了（丢过帧 / 刚打开 / 刚 flush），解出来必然是错的
      // 画面（H.264 的间预测会拿不存在的参考帧做预测，剩下的只是掩盖误差的假象）。
      // 丢掉并记账，err 保持为空：这是策略而不是失败，否则调用方的 fail_streak 会把
      // "设计上在等恢复"算成"每帧都解码失败"。
      ++stats_.gated;
      ++stats_.dropped;
      return nullptr;
    } else if (!(saw_sps_ && saw_pps_)) {
      // IDR 到了，但参数集从没见过：解码器对每个包都回 INVALIDDATA 且永不自愈
      // （事实：下行丢了开头的 CodecConfig 后，probe 每帧 -1094995529，画面永久冻住）。
      // 这一条是真失败（不是策略），所以要写 err 让调用方的 decode_errors 涨起来。
      err = "IDR without SPS/PPS (waiting for parameter sets)";
      ++stats_.errors;
      ++stats_.gated;
      ++stats_.dropped;
      return nullptr;
    } else {
      // IDR + 参数集齐了：这是唯一能重建参考链的入口，放行并离开等待态。
      waiting_keyframe_ = false;
    }
  }

  av_packet_unref(pkt_);
  if (av_new_packet(pkt_, static_cast<int>(data.size())) < 0) {
    err = "av_new_packet failed";
    ++stats_.errors;
    ++stats_.dropped;
    // last_ 在函数开头就已经放掉了：这里返回 nullptr 不会留下"半更新"的帧——
    // 借用指针的语义是"只到下次 decode 之前有效"，被失效的那个指针调用方本就不该再用。
    return nullptr;
  }
  std::memcpy(pkt_->data, data.data(), data.size());
  pkt_->pts = AV_NOPTS_VALUE;

  // 把解码器里已就绪的帧全部收走，最后一张留在 last_ 返回。
  bool got = false;
  auto drain = [&]() {
    for (;;) {
      const int r = avcodec_receive_frame(ctx_, frame_);
      if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return;
      if (r < 0) {
        ++stats_.errors;
        err = "receive_frame failed: " + std::to_string(r);
        // 出错时 frame_ 的内容没有定义，不清掉的话下一次 receive 会在脏帧上做 move：
        // FFmpeg 只保证"成功时 dst 是一张完整的新帧"。
        if (frame_) av_frame_unref(frame_);
        return;
      }
      ++stats_.frames;
      // 低延迟模式下一次 send 通常只产出 0~1 帧；若产出多帧只保留最后一帧
      if (got) av_frame_unref(last_);
      av_frame_move_ref(last_, frame_);
      got = true;
    }
  };

  int ret = avcodec_send_packet(ctx_, pkt_);
  if (ret == AVERROR(EAGAIN)) {
    // 上一包还没被取走：EAGAIN 不等于成功。先排空，再重试 send；
    // 仍 EAGAIN 说明本包确实未被消费 → 计入 dropped，不能静默当成功。
    drain();
    ret = avcodec_send_packet(ctx_, pkt_);
    if (ret == AVERROR(EAGAIN)) {
      // 这里不记 gated：gated 的语义是「等 IDR/参数集」，而这一包是被解码器的输出队列
      // 堵住了，处置完全不同（要查的是调用方有没有及时把帧取走）。
      ++stats_.dropped;
      err = "send_packet EAGAIN (packet not consumed)";
      return got ? last_ : nullptr;
    }
  }
  if (ret < 0) {
    // send 失败（INVALIDDATA 等）：这是本包自己的问题，**不**进入等 IDR 状态。
    // 一帧损坏不代表参考链断了，H.264 的 frame_num 连续性可以自愈；自动切到
    // "等 IDR"反而会让一次偶发错误把画面冻住一整个 GOP。
    ++stats_.errors;
    ++stats_.dropped;
    err = "send_packet failed: " + std::to_string(ret);
    // 错误分支也要先把已就绪的帧排空，否则会白丢一帧（review P1 #12）
    drain();
    return got ? last_ : nullptr;
  }
  ++stats_.packets;

  drain();
  if (!got) return nullptr;
  return last_;
}

void H264Decoder::flush() {
  if (!ctx_) return;
  // avcodec_flush_buffers 保证清掉 DPB/参考帧/延迟输出（这是"换代"需要的全部），
  // 但**参数集是否随之失效**属于各解码器的实现细节，API 不承诺。
  avcodec_flush_buffers(ctx_);
  if (last_) av_frame_unref(last_);
  if (frame_) av_frame_unref(frame_);
  // DPB 空了，任何 P/B 帧都缺参考：强制等下一个 IDR。
  waiting_keyframe_ = true;
  // 但**不**清 saw_sps_/saw_pps_。两个方向都有代价，这里选代价小的那个：
  //   · 当成"参数集还在"：若实现真的清了，接下来几帧会报 INVALIDDATA，直到一个自带
  //     SPS/PPS 的 IDR 到达（repeat-headers 打开时就在下一个 IDR）——能自愈。
  //   · 当成"参数集没了"：若解码器其实保留着，我们会空等一个可能永不重发的 SPS
  //     （关掉 repeat-headers 的产线就是这样）——永久冻屏，和这次要修的故障一模一样。
  ++stats_.flushes;
}

}  // namespace cue::media
