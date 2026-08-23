#pragma once

#include <memory>
#include <string>

#include "timecode.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/rational.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace fc {

//---- RAII ownership for the FFmpeg C objects used by the media layer. ----

struct FormatContextDeleter {
    void operator()(AVFormatContext *ctx) const { avformat_close_input(&ctx); }
};
using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;

struct PacketDeleter {
    void operator()(AVPacket *pkt) const {
        if (pkt) {
            av_packet_free(&pkt);
        }
    }
};
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

struct FrameDeleter {
    void operator()(AVFrame *frame) const {
        if (frame) {
            av_frame_free(&frame);
        }
    }
};
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

struct CodecContextDeleter {
    void operator()(AVCodecContext *ctx) const {
        if (ctx) {
            avcodec_free_context(&ctx);
        }
    }
};
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;

struct SwsContextDeleter {
    void operator()(SwsContext *ctx) const { sws_freeContext(ctx); }
};
using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;

struct SwrContextDeleter {
    void operator()(SwrContext *ctx) const { swr_free(&ctx); }
};
using SwrContextPtr = std::unique_ptr<SwrContext, SwrContextDeleter>;

//---- Small helpers shared by probe / decoder / proxy generator. ----

// C++-safe replacement for the av_err2str macro (which uses compound
// literals and is unusable with MSVC).
std::string fcError(int errnum);

// Human-readable build/runtime version report (for the About dialog and
// test logs).
std::string ffmpegVersionInfo();

// AVRational <-> FrameRate. Returns 24/1 when the rational is invalid.
FrameRate avRationalToFrameRate(AVRational rational);
AVRational frameRateToAv(const FrameRate &rate);

} // namespace fc
