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
                   std::string &error, const ExportAudioProvider &audioProvider) {
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
    if (config.audioChannels != 1 && config.audioChannels != 2) {
        config.audioChannels = 2;
    }
    if (config.sampleRate < 8000 || config.sampleRate > 192000) {
        config.sampleRate = 48000;
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

    // ---- Audio encoder (optional: only when a provider was passed) ----
    // Windows tile the timeline exactly at frame granularity:
    // edge(f) = floor(f * rate * den / num) for the fps rational (the
    // video encoder's fpsQ above), so [edge(f), edge(f+1)) never
    // overlaps or gaps. The provider mixes each window; a FIFO
    // accumulates AAC frame_size groups.
    CodecContextPtr audioEnc;
    AVStream *audioStream = nullptr;
    FramePtr audioFrame;
    int64_t audioFrameSize = 0;
    int64_t audioPts = 0;         // in SAMPLES at config.sampleRate
    std::vector<float> audioFifo; // interleaved, audioChannels per frame
    if (audioProvider) {
        const AVCodec *aac = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!aac) {
            error = "no AAC encoder in this FFmpeg build";
            return false;
        }
        audioEnc.reset(avcodec_alloc_context3(aac));
        if (!audioEnc) {
            error = "audio encoder context alloc failed";
            return false;
        }
        audioEnc->sample_rate = config.sampleRate;
        audioEnc->sample_fmt = AV_SAMPLE_FMT_FLTP; // the native AAC contract
        audioEnc->bit_rate = 192000;
        audioEnc->time_base = AVRational{1, config.sampleRate};
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
        av_channel_layout_default(&audioEnc->ch_layout, config.audioChannels);
#else
        audioEnc->channels = config.audioChannels;
        audioEnc->channel_layout = av_get_default_channel_layout(config.audioChannels);
#endif
        if (avcodec_open2(audioEnc.get(), aac, nullptr) < 0) {
            error = "audio encoder open failed";
            return false;
        }
        audioFrameSize = audioEnc->frame_size > 0 ? audioEnc->frame_size : 1024;

        audioFrame.reset(av_frame_alloc());
        if (!audioFrame) {
            error = "audio frame alloc failed";
            return false;
        }
        audioFrame->format = AV_SAMPLE_FMT_FLTP;
        audioFrame->sample_rate = config.sampleRate;
        audioFrame->nb_samples = static_cast<int>(audioFrameSize);
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
        av_channel_layout_copy(&audioFrame->ch_layout, &audioEnc->ch_layout);
#else
        audioFrame->channel_layout = audioEnc->channel_layout;
        audioFrame->channels = audioEnc->channels;
#endif
        if (av_frame_get_buffer(audioFrame.get(), 0) < 0) {
            error = "audio frame buffer alloc failed";
            return false;
        }
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
    if (audioProvider) {
        audioStream = avformat_new_stream(out.get(), nullptr);
        if (!audioStream) {
            error = "audio stream alloc failed";
            return false;
        }
        audioStream->time_base = AVRational{1, config.sampleRate};
        avcodec_parameters_from_context(audioStream->codecpar, audioEnc.get());
    }

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

    // Encodes one full interleaved window of FIFO samples as an AAC
    // frame (planar FLTP layout) and writes the packet.
    auto encodeAudioFrame = [&]() -> bool {
        if (!audioProvider || !audioEnc) {
            return true;
        }
        const int ch = config.audioChannels;
        av_frame_make_writable(audioFrame.get());
        for (int c = 0; c < ch; ++c) {
            float *plane = reinterpret_cast<float *>(audioFrame->extended_data[c]);
            for (int64_t i = 0; i < audioFrameSize; ++i) {
                plane[i] = audioFifo[size_t(i) * ch + size_t(c)];
            }
        }
        audioFrame->pts = audioPts;
        audioPts += audioFrameSize;
        const int sendRc = avcodec_send_frame(audioEnc.get(), audioFrame.get());
        if (sendRc < 0) {
            error = "audio encode send failed: " + fcError(sendRc);
            return false;
        }
        while (avcodec_receive_packet(audioEnc.get(), pkt.get()) == 0) {
            if (!writePacket(out.get(), audioEnc.get(), audioStream, pkt.get(), error)) {
                return false;
            }
        }
        return true;
    };

    // Pulls the provider's window for frame f into the FIFO and encodes
    // every complete AAC frame that becomes available.
    auto pullAudioWindow = [&](int64_t f) -> bool {
        if (!audioProvider || !audioEnc) {
            return true;
        }
        // Exact rational window edges: floor(f * rate * den / num), in
        // int64 (a 3-hour 29.97 sequence stays far inside the range).
        const int64_t e0 = (f * int64_t(config.sampleRate) * int64_t(fpsQ.den)) / int64_t(fpsQ.num);
        const int64_t e1 =
            ((f + 1) * int64_t(config.sampleRate) * int64_t(fpsQ.den)) / int64_t(fpsQ.num);
        if (e1 <= e0) {
            return true; // degenerate (sub-sample frame) - nothing to pull
        }
        const int count = static_cast<int>(e1 - e0);
        audioFifo.insert(audioFifo.end(), size_t(count) * config.audioChannels, 0.0f);
        float *dst = audioFifo.data() + (audioFifo.size() - size_t(count) * config.audioChannels);
        if (!audioProvider(e0, count, dst)) {
            error.clear(); // cancellation: no error message
            return false;
        }
        while (audioFifo.size() / config.audioChannels >= size_t(audioFrameSize)) {
            if (!encodeAudioFrame()) {
                return false;
            }
            audioFifo.erase(audioFifo.begin(),
                            audioFifo.begin() + size_t(audioFrameSize) * config.audioChannels);
        }
        return true;
    };

    for (int64_t f = 0; f < config.totalFrames; ++f) {
        if (!provider(static_cast<int>(f), rgba.data())) {
            error.clear(); // cancellation: no error message
            return false;
        }
        if (!pullAudioWindow(f)) {
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
    if (audioProvider && audioEnc) {
        // Pad the tail with silence to a whole AAC frame so the last
        // partial window flushes (<= one frame, ~21 ms at 48 kHz).
        const size_t have = audioFifo.size() / config.audioChannels;
        if (have > 0) {
            audioFifo.resize(size_t(audioFrameSize) * config.audioChannels, 0.0f);
            if (!encodeAudioFrame()) {
                return false;
            }
            audioFifo.clear();
        }
        avcodec_send_frame(audioEnc.get(), nullptr);
        while (avcodec_receive_packet(audioEnc.get(), pkt.get()) == 0) {
            if (!writePacket(out.get(), audioEnc.get(), audioStream, pkt.get(), error)) {
                return false;
            }
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
