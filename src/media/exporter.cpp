#include "exporter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "ffmpeg_wrappers.h"

namespace fc {

namespace {

int evenDown(int value) {
    return std::max(2, value - (value % 2));
}

struct OutputContextDeleter {
    void operator()(AVFormatContext *ctx) const {
        if (ctx) {
            avformat_free_context(ctx);
        }
    }
};
using OutputContextPtr = std::unique_ptr<AVFormatContext, OutputContextDeleter>;

struct IoContextDeleter {
    void operator()(AVIOContext *io) const {
        if (io) {
            avio_closep(&io);
        }
    }
};
using IoContextPtr = std::unique_ptr<AVIOContext, IoContextDeleter>;

bool writePacket(AVFormatContext *out, AVCodecContext *enc, AVStream *stream, AVPacket *pkt,
                 std::string &error) {
    // The muxer is free to change stream->time_base in write_header; the
    // encoder emits pts in enc->time_base - rescale before writing.
    av_packet_rescale_ts(pkt, enc->time_base, stream->time_base);
    pkt->stream_index = stream->index;
    const int rc = av_interleaved_write_frame(out, pkt);
    av_packet_unref(pkt);
    if (rc < 0) {
        error = "mux write failed: " + fcError(rc);
        return false;
    }
    return true;
}

} // namespace

bool Exporter::run(const std::string &dstPath, const ExportConfig &configIn,
                   const ExportFrameProvider &provider, const ExportProgress &progress,
                   std::string &error) {
    if (!provider) {
        error = "no frame provider";
        return false;
    }

    ExportConfig config = configIn;
    config.width = evenDown(config.width);
    config.height = evenDown(config.height);
    if (config.fps <= 0.0 || config.fps > 240.0) {
        error = "fps out of range";
        return false;
    }
    if (config.totalFrames <= 0) {
        error = "nothing to export (empty sequence)";
        return false;
    }

    // Remove partial output when the job fails or is cancelled.
    struct PartialFileCleaner {
        std::string path;
        bool committed = false;
        ~PartialFileCleaner() {
            if (!committed) {
                std::remove(path.c_str());
            }
        }
    } cleaner{dstPath, false};

    // ---- Encoder ----
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    }
    if (!codec) {
        error = "no usable video encoder (h264/mpeg4) in this FFmpeg build";
        return false;
    }
    CodecContextPtr enc(avcodec_alloc_context3(codec));
    if (!enc) {
        error = "encoder context alloc failed";
        return false;
    }
    enc->width = config.width;
    enc->height = config.height;
    enc->pix_fmt = AV_PIX_FMT_YUV420P;
    // Exact output frame rate as the timebase: pts == frame index, and
    // av_d2q keeps fractional rates (23.976 -> 1001/24000) exact.
    AVRational fpsQ = av_d2q(config.fps, 1001);
    if (fpsQ.num <= 0 || fpsQ.den <= 0) {
        fpsQ = AVRational{24, 1};
    }
    enc->time_base = av_inv_q(fpsQ);
    enc->gop_size = static_cast<int>(std::clamp(2.0 * config.fps, 12.0, 120.0));
    enc->max_b_frames = 2;
    if (codec->id == AV_CODEC_ID_H264) {
        av_opt_set(enc->priv_data, "crf", std::to_string(config.crf).c_str(), 0);
        av_opt_set(enc->priv_data, "preset", config.preset.c_str(), 0);
    } else {
        enc->bit_rate = 2000000;
    }
    if (avcodec_open2(enc.get(), codec, nullptr) < 0) {
        error = "encoder open failed";
        return false;
    }

    // ---- Output container ----
    AVFormatContext *rawOut = nullptr;
    int rc = avformat_alloc_output_context2(&rawOut, nullptr, nullptr, dstPath.c_str());
    if (rc < 0 || !rawOut) {
        error = "output context alloc failed: " + fcError(rc);
        return false;
    }
    OutputContextPtr out(rawOut);
    AVStream *stream = avformat_new_stream(out.get(), nullptr);
    if (!stream) {
        error = "output stream alloc failed";
        return false;
    }
    stream->time_base = enc->time_base;
    avcodec_parameters_from_context(stream->codecpar, enc.get());

    IoContextPtr io;
    if (!(out->oformat->flags & AVFMT_NOFILE)) {
        AVIOContext *rawIo = nullptr;
        rc = avio_open(&rawIo, dstPath.c_str(), AVIO_FLAG_WRITE);
        if (rc < 0) {
            error = "open output file failed: " + fcError(rc);
            return false;
        }
        io.reset(rawIo);
        out->pb = rawIo;
    }
    rc = avformat_write_header(out.get(), nullptr);
    if (rc < 0) {
        error = "write header failed: " + fcError(rc);
        return false;
    }

    // ---- Frame plumbing: RGBA8888 -> yuv420p via swscale ----
    SwsContextPtr scaler(sws_getContext(config.width, config.height, AV_PIX_FMT_RGBA, config.width,
                                        config.height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr,
                                        nullptr, nullptr));
    if (!scaler) {
        error = "scaler alloc failed";
        return false;
    }
    FramePtr frame(av_frame_alloc());
    if (!frame) {
        error = "frame alloc failed";
        return false;
    }
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = config.width;
    frame->height = config.height;
    if (av_frame_get_buffer(frame.get(), 32) < 0) {
        error = "frame buffer alloc failed";
        return false;
    }
    // The provider fills one packed RGBA line at a time; sws_scale wants a
    // pointer array, so hand it the single contiguous plane.
    const int rgbaStride = config.width * 4;
    std::vector<uint8_t> rgba(static_cast<size_t>(rgbaStride) * config.height);

    PacketPtr pkt(av_packet_alloc());
    if (!pkt) {
        error = "packet alloc failed";
        return false;
    }

    for (int64_t f = 0; f < config.totalFrames; ++f) {
        if (!provider(static_cast<int>(f), rgba.data())) {
            error.clear(); // cancellation: no error message
            return false;
        }
        const uint8_t *srcPlane[1] = {rgba.data()};
        const int srcStride[1] = {rgbaStride};
        av_frame_make_writable(frame.get());
        sws_scale(scaler.get(), srcPlane, srcStride, 0, config.height, frame->data,
                  frame->linesize);
        frame->pts = f;
        if (avcodec_send_frame(enc.get(), frame.get()) < 0) {
            error = "encode send failed";
            return false;
        }
        while (avcodec_receive_packet(enc.get(), pkt.get()) == 0) {
            if (!writePacket(out.get(), enc.get(), stream, pkt.get(), error)) {
                return false;
            }
        }
        if (progress &&
            !progress(static_cast<double>(f + 1) / static_cast<double>(config.totalFrames))) {
            error.clear(); // cancelled by the progress callback
            return false;
        }
    }

    // ---- Drain + trailer ----
    avcodec_send_frame(enc.get(), nullptr);
    while (avcodec_receive_packet(enc.get(), pkt.get()) == 0) {
        if (!writePacket(out.get(), enc.get(), stream, pkt.get(), error)) {
            return false;
        }
    }
    rc = av_write_trailer(out.get());
    if (rc < 0) {
        error = "write trailer failed: " + fcError(rc);
        return false;
    }
    cleaner.committed = true;
    return true;
}

} // namespace fc
