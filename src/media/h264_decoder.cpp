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
  if (!ctx_ || data.empty()) return nullptr;

  av_packet_unref(pkt_);
  if (av_new_packet(pkt_, static_cast<int>(data.size())) < 0) {
    err = "av_new_packet failed";
    ++stats_.errors;
    return nullptr;
  }
  std::memcpy(pkt_->data, data.data(), data.size());
  pkt_->pts = AV_NOPTS_VALUE;

  int ret = avcodec_send_packet(ctx_, pkt_);
  if (ret < 0 && ret != AVERROR(EAGAIN)) {
    ++stats_.errors;
    err = "send_packet failed: " + std::to_string(ret);
    return nullptr;
  }
  ++stats_.packets;

  bool got = false;
  for (;;) {
    ret = avcodec_receive_frame(ctx_, frame_);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
    if (ret < 0) {
      ++stats_.errors;
      err = "receive_frame failed: " + std::to_string(ret);
      break;
    }
    ++stats_.frames;
    // 低延迟模式下一次 send 通常只产出 0~1 帧；若产出多帧只保留最后一帧
    if (got) av_frame_unref(last_);
    av_frame_move_ref(last_, frame_);
    got = true;
  }
  if (!got) return nullptr;
  return last_;
}

}  // namespace cue::media
