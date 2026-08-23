#include "media_test_util.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "ffmpeg_wrappers.h"

namespace fc {

bool generateTestVideo(const std::string &path, const TestMediaSpec &spec, std::string &error) {
    const AVCodec *videoCodec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!videoCodec) {
        videoCodec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    }
    if (!videoCodec) {
        error = "no H.264/MPEG-4 encoder available";
        return false;
    }

    AVFormatContext *rawOut = nullptr;
    if (avformat_alloc_output_context2(&rawOut, nullptr, nullptr, path.c_str()) < 0 || !rawOut) {
        error = "output context alloc failed";
        return false;
    }
    struct OutputDeleter {
        void operator()(AVFormatContext *ctx) const { avformat_free_context(ctx); }
    };
    std::unique_ptr<AVFormatContext, OutputDeleter> out(rawOut);

    //---- Video encoder ----
    CodecContextPtr videoEnc(avcodec_alloc_context3(videoCodec));
    videoEnc->width = spec.width;
    videoEnc->height = spec.height;
    videoEnc->pix_fmt = AV_PIX_FMT_YUV420P;
    videoEnc->time_base = AVRational{1, spec.fps};
    videoEnc->gop_size = spec.fps; // one keyframe per second
    videoEnc->max_b_frames = 0;    // keep frame order trivial for assertions
    if (videoCodec->id == AV_CODEC_ID_H264) {
        av_opt_set(videoEnc->priv_data, "crf", "20", 0);
        av_opt_set(videoEnc->priv_data, "preset", "veryfast", 0);
    } else {
        videoEnc->bit_rate = 2000000;
    }
    if (avcodec_open2(videoEnc.get(), videoCodec, nullptr) < 0) {
        error = "video encoder open failed";
        return false;
    }
    AVStream *videoStream = avformat_new_stream(out.get(), nullptr);
    videoStream->time_base = videoEnc->time_base;
    avcodec_parameters_from_context(videoStream->codecpar, videoEnc.get());

    //---- Audio encoder (optional) ----
    CodecContextPtr audioEnc;
    AVStream *audioStream = nullptr;
    if (spec.withAudio) {
        const AVCodec *audioCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!audioCodec) {
            error = "no AAC encoder available";
            return false;
        }
        audioEnc.reset(avcodec_alloc_context3(audioCodec));
        audioEnc->sample_rate = 48000;
        audioEnc->sample_fmt = AV_SAMPLE_FMT_FLTP;
        audioEnc->bit_rate = 64000;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
        av_channel_layout_default(&audioEnc->ch_layout, 2);
#else
        audioEnc->channels = 2;
        audioEnc->channel_layout = av_get_default_channel_layout(2);
#endif
        if (avcodec_open2(audioEnc.get(), audioCodec, nullptr) < 0) {
            error = "audio encoder open failed";
            return false;
        }
        audioStream = avformat_new_stream(out.get(), nullptr);
        audioStream->time_base = AVRational{1, audioEnc->sample_rate};
        avcodec_parameters_from_context(audioStream->codecpar, audioEnc.get());
    }

    if (!(out->oformat->flags & AVFMT_NOFILE)) {
        AVIOContext *io = nullptr;
        if (avio_open(&io, path.c_str(), AVIO_FLAG_WRITE) < 0) {
            error = "output file open failed";
            return false;
        }
        out->pb = io;
    }

    if (avformat_write_header(out.get(), nullptr) < 0) {
        error = "write header failed";
        return false;
    }

    //---- Video frames ----
    FramePtr frame(av_frame_alloc());
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = spec.width;
    frame->height = spec.height;
    if (av_frame_get_buffer(frame.get(), 0) < 0) {
        error = "frame buffer alloc failed";
        return false;
    }

    PacketPtr pkt(av_packet_alloc());
    const int totalFrames = static_cast<int>(spec.seconds * spec.fps);
    auto encodeVideo = [&](AVFrame *f) -> bool {
        const int rc = avcodec_send_frame(videoEnc.get(), f);
        if (rc < 0) {
            error = "video encode failed: " + fcError(rc);
            return false;
        }
        while (avcodec_receive_packet(videoEnc.get(), pkt.get()) == 0) {
            av_packet_rescale_ts(pkt.get(), videoEnc->time_base, videoStream->time_base);
            pkt->stream_index = videoStream->index;
            if (av_interleaved_write_frame(out.get(), pkt.get()) < 0) {
                error = "video mux failed";
                return false;
            }
            av_packet_unref(pkt.get());
        }
        return true;
    };

    for (int i = 0; i < totalFrames; ++i) {
        av_frame_make_writable(frame.get());
        const uint8_t *rgb = (i / spec.fps) % 2 == 0 ? spec.colorA : spec.colorB;
        // RGB -> rough YUV without chroma subsampling precision: fill Y
        // plane per-pixel, U/V planes uniformly (good enough for solid
        // color assertions after an RGBA round trip).
        for (int y = 0; y < spec.height; ++y) {
            uint8_t *yRow = frame->data[0] + y * frame->linesize[0];
            for (int x = 0; x < spec.width; ++x) {
                yRow[x] =
                    static_cast<uint8_t>((66 * rgb[0] + 129 * rgb[1] + 25 * rgb[2] + 128) >> 8);
            }
        }
        const uint8_t u = static_cast<uint8_t>(
            (-38 * rgb[0] - 74 * rgb[1] + 112 * rgb[2] + 128 + (128 << 8)) >> 8);
        const uint8_t v = static_cast<uint8_t>(
            (112 * rgb[0] - 94 * rgb[1] - 18 * rgb[2] + 128 + (128 << 8)) >> 8);
        for (int y = 0; y < spec.height / 2; ++y) {
            memset(frame->data[1] + y * frame->linesize[1], u, spec.width / 2);
            memset(frame->data[2] + y * frame->linesize[2], v, spec.width / 2);
        }
        frame->pts = i;
        if (!encodeVideo(frame.get())) {
            return false;
        }
    }
    encodeVideo(nullptr); // flush

    //---- Audio: one second of 440 Hz tone per encoded frame batch ----
    if (audioEnc) {
        FramePtr audioFrame(av_frame_alloc());
        audioFrame->format = audioEnc->sample_fmt;
        audioFrame->sample_rate = audioEnc->sample_rate;
        audioFrame->nb_samples = audioEnc->frame_size;
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
        const int64_t totalSamples = static_cast<int64_t>(spec.seconds * audioEnc->sample_rate);
        int64_t written = 0;
        double phase = 0.0;
        const double step = 2.0 * 3.14159265358979323846 * spec.audioHz / audioEnc->sample_rate;
        auto encodeAudio = [&](AVFrame *f) -> bool {
            const int rc = avcodec_send_frame(audioEnc.get(), f);
            if (rc < 0) {
                error = "audio encode failed: " + fcError(rc);
                return false;
            }
            while (avcodec_receive_packet(audioEnc.get(), pkt.get()) == 0) {
                av_packet_rescale_ts(pkt.get(), AVRational{1, audioEnc->sample_rate},
                                     audioStream->time_base);
                pkt->stream_index = audioStream->index;
                if (av_interleaved_write_frame(out.get(), pkt.get()) < 0) {
                    error = "audio mux failed";
                    return false;
                }
                av_packet_unref(pkt.get());
            }
            return true;
        };
        while (written + audioEnc->frame_size <= totalSamples) {
            av_frame_make_writable(audioFrame.get());
            for (int s = 0; s < audioFrame->nb_samples; ++s) {
                const float sample = 0.4f * static_cast<float>(std::sin(phase));
                phase += step;
                for (int ch = 0; ch < 2; ++ch) {
                    reinterpret_cast<float *>(audioFrame->extended_data[ch])[s] = sample;
                }
            }
            audioFrame->pts = written;
            written += audioEnc->frame_size;
            if (!encodeAudio(audioFrame.get())) {
                return false;
            }
        }
        encodeAudio(nullptr); // flush
    }

    if (av_write_trailer(out.get()) < 0) {
        error = "write trailer failed";
        return false;
    }
    if (out->pb && !(out->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&out->pb);
    }
    return true;
}

} // namespace fc
