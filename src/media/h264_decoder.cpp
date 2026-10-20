#include <cstring>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

#include "cue/media/h264_decoder.h"

namespace cue::media {

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
  return true;
}

void H264Decoder::close() {
  if (pkt_) av_packet_free(&pkt_);
  if (last_) av_frame_free(&last_);
  if (frame_) av_frame_free(&frame_);
  if (ctx_) avcodec_free_context(&ctx_);
}

AVFrame* H264Decoder::decode(std::span<const uint8_t> data, std::string& err) {
  err.clear();  // 调用方常复用同一个 string：不清空会读到上一次的旧错误
  if (!ctx_ || data.empty()) return nullptr;

  av_packet_unref(pkt_);
  if (av_new_packet(pkt_, static_cast<int>(data.size())) < 0) {
    err = "av_new_packet failed";
    ++stats_.errors;
    ++stats_.dropped;
    return nullptr;
  }
  std::memcpy(pkt_->data, data.data(), data.size());
  pkt_->pts = AV_NOPTS_VALUE;

  // 把解码器里已就绪的帧全部收走，最后一张留在 last_ 返回。
  // 返回是否取到过帧；err 由调用方判断。
  bool got = false;
  auto drain = [&](bool* failed) {
    for (;;) {
      const int r = avcodec_receive_frame(ctx_, frame_);
      if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return;
      if (r < 0) {
        ++stats_.errors;
        err = "receive_frame failed: " + std::to_string(r);
        if (failed) *failed = true;
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
    drain(nullptr);
    ret = avcodec_send_packet(ctx_, pkt_);
    if (ret == AVERROR(EAGAIN)) {
      ++stats_.dropped;
      err = "send_packet EAGAIN (packet not consumed)";
      return got ? last_ : nullptr;
    }
  }
  if (ret < 0) {
    ++stats_.errors;
    ++stats_.dropped;
    err = "send_packet failed: " + std::to_string(ret);
    // 错误分支也要先把已就绪的帧排空，否则会白丢一帧（review P1 #12）
    drain(nullptr);
    return got ? last_ : nullptr;
  }
  ++stats_.packets;

  drain(nullptr);
  if (!got) return nullptr;
  return last_;
}

void H264Decoder::flush() {
  if (!ctx_) return;
  avcodec_flush_buffers(ctx_);
  if (last_) av_frame_unref(last_);
  flushed_ = true;
  ++stats_.flushes;
}

}  // namespace cue::media
