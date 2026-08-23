#include "proxy_generator.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "ffmpeg_wrappers.h"
#include "media_probe.h"

namespace fc {

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
#define FC_HAVE_CH_LAYOUT 1
#endif

namespace {

int evenDown(int value) {
    return std::max(2, value - (value % 2));
}

struct AudioFifoDeleter {
    void operator()(AVAudioFifo *fifo) const {
        if (fifo) {
            av_audio_fifo_free(fifo);
        }
    }
};
using AudioFifoPtr = std::unique_ptr<AVAudioFifo, AudioFifoDeleter>;

struct IoContextDeleter {
    void operator()(AVIOContext *io) const {
        if (io) {
            avio_closep(&io);
        }
    }
};
using IoContextPtr = std::unique_ptr<AVIOContext, IoContextDeleter>;

bool setupVideoEncoder(AVCodecContext *&ctx, const AVCodec *&codec, int width, int height,
                       AVRational timebase, int gopSize, int crf, const std::string &preset,
                       std::string &error) {
    codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    }
    if (!codec) {
        error = "no usable video encoder (h264/mpeg4) in this FFmpeg build";
        return false;
    }

    ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        error = "video encoder context alloc failed";
        return false;
    }

    ctx->width = width;
    ctx->height = height;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->time_base = timebase;
    ctx->gop_size = gopSize;
    ctx->max_b_frames = 2;

    if (codec->id == AV_CODEC_ID_H264) {
        // Options belong to libx264; ignore failures for other h264 encoders.
        av_opt_set(ctx->priv_data, "crf", std::to_string(crf).c_str(), 0);
        av_opt_set(ctx->priv_data, "preset", preset.c_str(), 0);
    } else {
        ctx->bit_rate = 1500000;
    }

    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        error = "video encoder open failed";
        avcodec_free_context(&ctx);
        return false;
    }
    return true;
}

bool setupAudioEncoder(AVCodecContext *&ctx, const AVCodec *&codec, int sampleRate,
                       std::string &error) {
    codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        error = "no AAC encoder in this FFmpeg build";
        return false;
    }
    ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        error = "audio encoder context alloc failed";
        return false;
    }
    ctx->sample_rate = sampleRate;
    ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    ctx->bit_rate = 96000;
#ifdef FC_HAVE_CH_LAYOUT
    av_channel_layout_default(&ctx->ch_layout, 2);
#else
    ctx->channels = 2;
    ctx->channel_layout = av_get_default_channel_layout(2);
#endif
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        error = "audio encoder open failed";
        avcodec_free_context(&ctx);
        return false;
    }
    return true;
}

// Configures a resampler from a decoded frame's layout to the encoder's
// stereo FLTP target. Returns an unset pointer on failure.
SwrContextPtr makeResampler(const AVFrame *src, const AVCodecContext *enc) {
    SwrContext *raw = nullptr;
#ifdef FC_HAVE_CH_LAYOUT
    AVChannelLayout inLayout;
    av_channel_layout_default(&inLayout, src->ch_layout.nb_channels);
    if (swr_alloc_set_opts2(&raw, &inLayout, static_cast<AVSampleFormat>(src->format),
                            src->sample_rate, &enc->ch_layout, enc->sample_fmt, enc->sample_rate, 0,
                            nullptr) < 0) {
        return SwrContextPtr();
    }
#else
    const uint64_t inMask =
        src->channel_layout ? src->channel_layout : av_get_default_channel_layout(src->channels);
    raw =
        swr_alloc_set_opts(nullptr, enc->channel_layout, enc->sample_fmt, enc->sample_rate, inMask,
                           static_cast<AVSampleFormat>(src->format), src->sample_rate, 0, nullptr);
#endif
    SwrContextPtr resampler(raw);
    if (swr_init(resampler.get()) < 0) {
        return SwrContextPtr();
    }
    return resampler;
}

// Writes one packet through the interleaved muxer and unrefs it.
bool writePacket(AVFormatContext *out, AVPacket *pkt, int streamIndex, std::string &error) {
    pkt->stream_index = streamIndex;
    const int rc = av_interleaved_write_frame(out, pkt);
    av_packet_unref(pkt);
    if (rc < 0) {
        error = "mux write failed: " + fcError(rc);
        return false;
    }
    return true;
}

// Drains an encoder and writes every produced packet.
bool drainEncoder(AVFormatContext *out, AVCodecContext *enc, AVPacket *pkt, int streamIndex,
                  std::string &error) {
    avcodec_send_frame(enc, nullptr);
    while (avcodec_receive_packet(enc, pkt) == 0) {
        if (!writePacket(out, pkt, streamIndex, error)) {
            return false;
        }
    }
    return true;
}

} // namespace

bool ProxyGenerator::generate(const std::string &srcPath, const std::string &dstPath,
                              const ProxyConfig &config, const ProxyProgress &progress,
                              std::string &error) {
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

    MediaInfo info;
    if (!MediaProbe::probe(srcPath, info, error)) {
        return false;
    }
    if (!info.hasVideo) {
        error = "proxy source has no video stream: " + srcPath;
        return false;
    }

    //---- Input ----
    AVFormatContext *rawIn = nullptr;
    int rc = avformat_open_input(&rawIn, srcPath.c_str(), nullptr, nullptr);
    if (rc < 0) {
        error = "open failed: " + fcError(rc);
        return false;
    }
    FormatContextPtr in(rawIn);
    avformat_find_stream_info(in.get(), nullptr);

    AVStream *inVideo = in->streams[info.video.streamIndex];
    const int videoIdx = info.video.streamIndex;

    const AVCodec *videoDecoderCodec = avcodec_find_decoder(inVideo->codecpar->codec_id);
    if (!videoDecoderCodec) {
        error = "no decoder for source codec " + info.video.codecName;
        return false;
    }
    CodecContextPtr videoDecoder(avcodec_alloc_context3(videoDecoderCodec));
    avcodec_parameters_to_context(videoDecoder.get(), inVideo->codecpar);
    if (avcodec_open2(videoDecoder.get(), videoDecoderCodec, nullptr) < 0) {
        error = "source video decoder open failed";
        return false;
    }

    // Audio: pick the first audio stream.
    int audioIdx = -1;
    for (const AudioStreamInfo &audio : info.audioStreams) {
        audioIdx = audio.streamIndex;
        break;
    }
    CodecContextPtr audioDecoder;
    if (config.withAudio && audioIdx >= 0) {
        AVStream *inAudio = in->streams[audioIdx];
        const AVCodec *audioDecoderCodec = avcodec_find_decoder(inAudio->codecpar->codec_id);
        if (audioDecoderCodec) {
            audioDecoder.reset(avcodec_alloc_context3(audioDecoderCodec));
            avcodec_parameters_to_context(audioDecoder.get(), inAudio->codecpar);
            if (avcodec_open2(audioDecoder.get(), audioDecoderCodec, nullptr) < 0) {
                audioDecoder.reset(); // continue without audio
            }
        }
    }

    //---- Proxy geometry: never upscale, keep aspect, even dimensions ----
    const int targetH = evenDown(std::min(config.targetHeight, info.video.height));
    const int targetW = evenDown(static_cast<int>(
        std::llround(static_cast<double>(info.video.width) * targetH / info.video.height)));

    //---- Output container ----
    AVFormatContext *rawOut = nullptr;
    rc = avformat_alloc_output_context2(&rawOut, nullptr, nullptr, dstPath.c_str());
    if (rc < 0 || !rawOut) {
        error = "output context alloc failed: " + fcError(rc);
        return false;
    }
    // The input-side deleter (avformat_close_input) must not own an output
    // context; free it with avformat_free_context instead.
    struct OutputDeleter {
        void operator()(AVFormatContext *ctx) const {
            if (ctx) {
                avformat_free_context(ctx);
            }
        }
    };
    std::unique_ptr<AVFormatContext, OutputDeleter> out(rawOut);

    //---- Encoders + streams ----
    const AVRational srcRate = frameRateToAv(info.video.frameRate);
    const AVRational outTimebase = av_inv_q(srcRate);
    const int gop =
        static_cast<int>(std::clamp(2.0 * info.video.frameRate.toDouble(), 12.0, 120.0));

    AVCodecContext *videoEnc = nullptr;
    const AVCodec *videoEncCodec = nullptr;
    if (!setupVideoEncoder(videoEnc, videoEncCodec, targetW, targetH, outTimebase, gop, config.crf,
                           config.preset, error)) {
        return false;
    }
    CodecContextPtr videoEncGuard(videoEnc);

    AVStream *outVideo = avformat_new_stream(out.get(), nullptr);
    outVideo->time_base = videoEnc->time_base;
    avcodec_parameters_from_context(outVideo->codecpar, videoEnc);

    AVCodecContext *audioEnc = nullptr;
    const AVCodec *audioEncCodec = nullptr;
    AVStream *outAudio = nullptr;
    AudioFifoPtr audioFifo;
    if (config.withAudio && audioIdx >= 0 && audioDecoder) {
        if (!setupAudioEncoder(audioEnc, audioEncCodec, config.audioSampleRate, error)) {
            return false;
        }
        audioFifo.reset(av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, 2, audioEnc->frame_size * 2));
        outAudio = avformat_new_stream(out.get(), nullptr);
        outAudio->time_base = AVRational{1, audioEnc->sample_rate};
        avcodec_parameters_from_context(outAudio->codecpar, audioEnc);
    }
    CodecContextPtr audioEncGuard(audioEnc);

    if (!(out->oformat->flags & AVFMT_NOFILE)) {
        AVIOContext *io = nullptr;
        rc = avio_open(&io, dstPath.c_str(), AVIO_FLAG_WRITE);
        if (rc < 0) {
            error = "open output file failed: " + fcError(rc);
            return false;
        }
        out->pb = io; // closed via avio_closep after the trailer
    }

    rc = avformat_write_header(out.get(), nullptr);
    if (rc < 0) {
        error = "write header failed: " + fcError(rc);
        return false;
    }

    //---- Scratch objects ----
    PacketPtr packet(av_packet_alloc());
    PacketPtr encPacket(av_packet_alloc());
    FramePtr decoded(av_frame_alloc());
    SwsContextPtr scaler(sws_getContext(info.video.width, info.video.height, videoDecoder->pix_fmt,
                                        targetW, targetH, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr,
                                        nullptr, nullptr));
    if (!scaler) {
        error = "scaler creation failed";
        return false;
    }
    FramePtr proxyFrame(av_frame_alloc());
    proxyFrame->format = AV_PIX_FMT_YUV420P;
    proxyFrame->width = targetW;
    proxyFrame->height = targetH;
    if (av_frame_get_buffer(proxyFrame.get(), 0) < 0) {
        error = "proxy frame buffer alloc failed";
        return false;
    }

    const double srcDuration = info.durationSeconds();
    SwrContextPtr resampler;
    FramePtr audioOutFrame;
    int64_t audioSamplesWritten = 0;
    if (audioEnc) {
        audioOutFrame.reset(av_frame_alloc());
        audioOutFrame->format = audioEnc->sample_fmt;
        audioOutFrame->sample_rate = audioEnc->sample_rate;
        audioOutFrame->nb_samples = audioEnc->frame_size;
#ifdef FC_HAVE_CH_LAYOUT
        av_channel_layout_copy(&audioOutFrame->ch_layout, &audioEnc->ch_layout);
#else
        audioOutFrame->channel_layout = audioEnc->channel_layout;
        audioOutFrame->channels = audioEnc->channels;
#endif
        if (av_frame_get_buffer(audioOutFrame.get(), 0) < 0) {
            error = "audio frame buffer alloc failed";
            return false;
        }
    }

    // Encodes one converted video frame.
    auto encodeVideoFrame = [&](AVFrame *scaled) -> bool {
        const int sendRc = avcodec_send_frame(videoEnc, scaled);
        if (sendRc < 0) {
            error = "video encode send failed: " + fcError(sendRc);
            return false;
        }
        while (avcodec_receive_packet(videoEnc, encPacket.get()) == 0) {
            if (!writePacket(out.get(), encPacket.get(), outVideo->index, error)) {
                return false;
            }
        }
        return true;
    };

    // Pulls FIFO samples into AAC frames and encodes them.
    auto flushAudioFifo = [&]() -> bool {
        while (audioFifo && av_audio_fifo_size(audioFifo.get()) >= audioEnc->frame_size) {
            av_audio_fifo_read(audioFifo.get(),
                               reinterpret_cast<void **>(audioOutFrame->extended_data),
                               audioEnc->frame_size);
            audioOutFrame->pts = audioSamplesWritten;
            audioSamplesWritten += audioEnc->frame_size;
            const int sendRc = avcodec_send_frame(audioEnc, audioOutFrame.get());
            if (sendRc < 0) {
                error = "audio encode send failed: " + fcError(sendRc);
                return false;
            }
            while (avcodec_receive_packet(audioEnc, encPacket.get()) == 0) {
                if (!writePacket(out.get(), encPacket.get(), outAudio->index, error)) {
                    return false;
                }
            }
        }
        return true;
    };

    // Converts one decoded audio frame into the FIFO.
    auto convertAudioFrame = [&](AVFrame *srcFrame) -> bool {
        if (!resampler) {
            resampler = makeResampler(srcFrame, audioEnc);
            if (!resampler) {
                return true; // skip audio on resampler failure
            }
        }
        const int outCapacity =
            srcFrame->nb_samples +
            static_cast<int>(swr_get_delay(resampler.get(), srcFrame->sample_rate)) + 16;
        uint8_t **buffer = nullptr;
        if (av_samples_alloc_array_and_samples(&buffer, nullptr, 2, outCapacity, AV_SAMPLE_FMT_FLTP,
                                               0) < 0) {
            return true; // skip on allocation failure
        }
        // The const_cast is required for FFmpeg <= 6.1, where swr_convert
        // takes `const uint8_t **` (C++ forbids the implicit T** conversion).
        // FFmpeg 7.x takes `const uint8_t *const *`, which also accepts the
        // result - so this compiles against every supported API generation.
        const int converted = swr_convert(resampler.get(), buffer, outCapacity,
                                          const_cast<const uint8_t **>(srcFrame->extended_data),
                                          srcFrame->nb_samples);
        if (converted > 0) {
            if (av_audio_fifo_space(audioFifo.get()) < converted) {
                if (av_audio_fifo_realloc(audioFifo.get(),
                                          av_audio_fifo_size(audioFifo.get()) + converted) < 0) {
                    // Out of memory: drop this audio frame rather than fail
                    // the whole proxy job.
                    av_freep(&buffer[0]);
                    av_freep(&buffer);
                    return flushAudioFifo();
                }
            }
            av_audio_fifo_write(audioFifo.get(), reinterpret_cast<void **>(buffer), converted);
        }
        if (buffer) {
            av_freep(&buffer[0]);
            av_freep(&buffer);
        }
        return flushAudioFifo();
    };

    //---- Main demux/decode/transcode loop ----
    bool cancelled = false;
    while (!cancelled) {
        rc = av_read_frame(in.get(), packet.get());
        if (rc == AVERROR_EOF) {
            break;
        }
        if (rc < 0) {
            error = "read failed: " + fcError(rc);
            return false;
        }

        if (packet->stream_index == videoIdx) {
            // See VideoDecoder: keep trailing DISCARD-flagged frames.
            packet->flags &= ~AV_PKT_FLAG_DISCARD;
            if (avcodec_send_packet(videoDecoder.get(), packet.get()) == 0) {
                while (avcodec_receive_frame(videoDecoder.get(), decoded.get()) == 0) {
                    av_frame_make_writable(proxyFrame.get());
                    sws_scale(scaler.get(), decoded->data, decoded->linesize, 0,
                              videoDecoder->height, proxyFrame->data, proxyFrame->linesize);
                    const int64_t pts = decoded->pts != AV_NOPTS_VALUE
                                            ? decoded->pts
                                            : decoded->best_effort_timestamp;
                    proxyFrame->pts = av_rescale_q(pts, inVideo->time_base, videoEnc->time_base);
                    if (!encodeVideoFrame(proxyFrame.get())) {
                        return false;
                    }
                    if (progress && srcDuration > 0.0) {
                        const double done = static_cast<double>(pts) * av_q2d(inVideo->time_base);
                        if (!progress(std::clamp(done / srcDuration, 0.0, 1.0))) {
                            cancelled = true;
                            break;
                        }
                    }
                    av_frame_unref(decoded.get());
                }
            }
        } else if (audioDecoder && packet->stream_index == audioIdx) {
            packet->flags &= ~AV_PKT_FLAG_DISCARD;
            if (avcodec_send_packet(audioDecoder.get(), packet.get()) == 0) {
                while (avcodec_receive_frame(audioDecoder.get(), decoded.get()) == 0) {
                    if (!convertAudioFrame(decoded.get())) {
                        return false;
                    }
                    av_frame_unref(decoded.get());
                }
            }
        }
        av_packet_unref(packet.get());
    }

    if (cancelled) {
        error = "cancelled by caller";
        return false;
    }

    // Flush the video decoder, then both encoders.
    avcodec_send_packet(videoDecoder.get(), nullptr);
    while (avcodec_receive_frame(videoDecoder.get(), decoded.get()) == 0) {
        av_frame_make_writable(proxyFrame.get());
        sws_scale(scaler.get(), decoded->data, decoded->linesize, 0, videoDecoder->height,
                  proxyFrame->data, proxyFrame->linesize);
        const int64_t pts =
            decoded->pts != AV_NOPTS_VALUE ? decoded->pts : decoded->best_effort_timestamp;
        proxyFrame->pts = av_rescale_q(pts, inVideo->time_base, videoEnc->time_base);
        if (!encodeVideoFrame(proxyFrame.get())) {
            return false;
        }
        av_frame_unref(decoded.get());
    }
    if (audioDecoder) {
        avcodec_send_packet(audioDecoder.get(), nullptr);
        while (avcodec_receive_frame(audioDecoder.get(), decoded.get()) == 0) {
            if (!convertAudioFrame(decoded.get())) {
                return false;
            }
            av_frame_unref(decoded.get());
        }
        if (!flushAudioFifo()) {
            return false;
        }
    }

    if (!drainEncoder(out.get(), videoEnc, encPacket.get(), outVideo->index, error)) {
        return false;
    }
    if (audioEnc && !drainEncoder(out.get(), audioEnc, encPacket.get(), outAudio->index, error)) {
        return false;
    }

    rc = av_write_trailer(out.get());
    if (rc < 0) {
        error = "write trailer failed: " + fcError(rc);
        return false;
    }

    if (out->pb && !(out->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&out->pb);
    }
    cleaner.committed = true;
    return true;
}

} // namespace fc
