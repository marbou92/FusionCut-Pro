#include "audio_decoder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "ffmpeg_wrappers.h"

namespace fc {

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
#define FC_HAVE_CH_LAYOUT 1
#endif

bool AudioDecoder::open(const std::string &path, int sampleRate, int channels, std::string &error) {
    close();

    rate_ = std::clamp(sampleRate, 8000, 192000);
    if (channels != 1 && channels != 2) {
        channels = 2;
    }
    channels_ = channels;

    FormatContextPtr raw;
    AVFormatContext *rawFmt = nullptr;
    const int rc = avformat_open_input(&rawFmt, path.c_str(), nullptr, nullptr);
    if (rc < 0) {
        error = "open failed: " + fcError(rc);
        return false;
    }
    raw.reset(rawFmt);
    if (avformat_find_stream_info(raw.get(), nullptr) < 0) {
        error = "stream discovery failed";
        return false;
    }
    format_ = std::move(raw);

    const int streamIndex =
        av_find_best_stream(format_.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (streamIndex < 0) {
        error = "no audio stream in " + path;
        close();
        return false;
    }
    audioStreamIndex_ = streamIndex;
    AVStream *stream = format_->streams[streamIndex];
    streamTimebase_ = stream->time_base;

    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        error = "no decoder for the audio stream";
        close();
        return false;
    }
    CodecContextPtr ctx(avcodec_alloc_context3(codec));
    if (!ctx) {
        error = "audio codec context alloc failed";
        close();
        return false;
    }
    if (avcodec_parameters_to_context(ctx.get(), stream->codecpar) < 0) {
        error = "audio codec parameters failed";
        close();
        return false;
    }
    if (avcodec_open2(ctx.get(), codec, nullptr) < 0) {
        error = "audio codec open failed";
        close();
        return false;
    }
    codec_ = std::move(ctx);

    packet_.reset(av_packet_alloc());
    frame_.reset(av_frame_alloc());
    if (!packet_ || !frame_) {
        error = "audio scratch objects alloc failed";
        close();
        return false;
    }

    endOfFile_ = false;
    draining_ = false;
    seekTarget_ = -1.0;
    return true;
}

void AudioDecoder::close() {
    format_.reset();
    codec_.reset();
    resampler_.reset();
    packet_.reset();
    frame_.reset();
    audioStreamIndex_ = -1;
    streamTimebase_ = AVRational{0, 1};
    endOfFile_ = false;
    draining_ = false;
    seekTarget_ = -1.0;
    resamplerSrcChannels_ = 0;
    resamplerSrcRate_ = 0;
    resamplerSrcFormat_ = -1;
}

bool AudioDecoder::ensureResampler(std::string &error) {
    const int srcChannels =
#ifdef FC_HAVE_CH_LAYOUT
        frame_->ch_layout.nb_channels;
#else
        frame_->channels;
#endif
    const int srcRate = frame_->sample_rate;
    const int srcFormat = frame_->format;
    if (resampler_ && srcChannels == resamplerSrcChannels_ && srcRate == resamplerSrcRate_ &&
        srcFormat == resamplerSrcFormat_) {
        return true; // unchanged geometry
    }

    SwrContext *raw = nullptr;
#ifdef FC_HAVE_CH_LAYOUT
    // NOTE: swr_alloc_set_opts2 takes the OUTPUT geometry FIRST (out
    // layout, out format, out rate), then the input's - the argument
    // order is the reverse of what the name suggests.
    AVChannelLayout inLayout;
    av_channel_layout_default(&inLayout, srcChannels > 0 ? srcChannels : 2);
    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, channels_);
    if (swr_alloc_set_opts2(&raw, &outLayout, AV_SAMPLE_FMT_FLT, rate_, &inLayout,
                            static_cast<AVSampleFormat>(srcFormat), srcRate, 0, nullptr) < 0) {
        error = "audio resampler alloc failed";
        return false;
    }
#else
    const uint64_t inMask = frame_->channel_layout
                                ? frame_->channel_layout
                                : av_get_default_channel_layout(srcChannels > 0 ? srcChannels : 2);
    raw = swr_alloc_set_opts(nullptr, av_get_default_channel_layout(channels_), AV_SAMPLE_FMT_FLT,
                             rate_, inMask, static_cast<AVSampleFormat>(srcFormat), srcRate, 0,
                             nullptr);
    if (!raw) {
        error = "audio resampler alloc failed";
        return false;
    }
#endif
    SwrContextPtr resampled(raw);
    if (swr_init(resampled.get()) < 0) {
        error = "audio resampler init failed";
        return false;
    }
    resampler_ = std::move(resampled);
    resamplerSrcChannels_ = srcChannels;
    resamplerSrcRate_ = srcRate;
    resamplerSrcFormat_ = srcFormat;
    return true;
}

bool AudioDecoder::seekToSeconds(double seconds, std::string &error) {
    if (!isOpen()) {
        error = "decoder not open";
        return false;
    }
    if (seconds < 0.0) {
        seconds = 0.0;
    }
    AVStream *stream = format_->streams[audioStreamIndex_];
    const int64_t timestamp =
        static_cast<int64_t>(std::llround(seconds / av_q2d(stream->time_base)));
    const int rc = av_seek_frame(format_.get(), audioStreamIndex_, timestamp, AVSEEK_FLAG_BACKWARD);
    if (rc < 0) {
        error = "audio seek failed: " + fcError(rc);
        return false;
    }
    avcodec_flush_buffers(codec_.get());
    endOfFile_ = false;
    draining_ = false;
    seekTarget_ = seconds;
    return true;
}

bool AudioDecoder::readSamples(DecodedAudio &out, std::string &error) {
    if (!isOpen()) {
        error = "decoder not open";
        return false;
    }

    while (true) {
        // Drain any frames the codec is still holding.
        int rc = avcodec_receive_frame(codec_.get(), frame_.get());
        if (rc == 0) {
            const int64_t pts =
                frame_->pts != AV_NOPTS_VALUE ? frame_->pts : frame_->best_effort_timestamp;
            const double ptsSeconds =
                pts != AV_NOPTS_VALUE ? static_cast<double>(pts) * av_q2d(streamTimebase_) : 0.0;

            if (!ensureResampler(error)) {
                av_frame_unref(frame_.get());
                return false;
            }
            const int64_t delay = swr_get_delay(resampler_.get(), frame_->sample_rate);
            const int capacity = frame_->nb_samples + static_cast<int>(delay) + 16;
            out.samples.assign(static_cast<size_t>(capacity) * channels_, 0.0f);
            float *planes[1] = {out.samples.data()};
            // The const_cast is required for FFmpeg <= 6.1, where
            // swr_convert takes `const uint8_t **` (C++ forbids the
            // implicit T** conversion). FFmpeg 7.x takes
            // `const uint8_t *const *`, which also accepts the result.
            const int converted = swr_convert(
                resampler_.get(), reinterpret_cast<uint8_t **>(planes), capacity,
                const_cast<const uint8_t **>(frame_->extended_data), frame_->nb_samples);
            av_frame_unref(frame_.get());
            if (converted < 0) {
                error = "audio resample failed: " + fcError(converted);
                return false;
            }
            if (converted == 0) {
                continue; // resampler warm-up delay; try the next frame
            }
            // While resuming from a seek, drop chunks whose frames all
            // precede the target (half a frame's tolerance).
            if (seekTarget_ >= 0.0) {
                const double chunkSec = static_cast<double>(converted) / rate_;
                if (ptsSeconds + chunkSec < seekTarget_ + 0.5 / rate_) {
                    continue;
                }
                seekTarget_ = -1.0;
            }
            out.samples.resize(static_cast<size_t>(converted) * channels_);
            out.frames = converted;
            out.ptsSeconds = ptsSeconds;
            return true;
        }
        if (rc != AVERROR(EAGAIN)) {
            // Decoder is fully drained.
            if (!draining_) {
                draining_ = true;
                avcodec_send_packet(codec_.get(), nullptr); // enter drain mode
                continue;
            }
            if (endOfFile_) {
                return false; // clean end of stream (error left empty)
            }
            error = "audio decode failed: " + fcError(rc);
            return false;
        }

        // Need more input.
        while (true) {
            const int prc = av_read_frame(format_.get(), packet_.get());
            if (prc < 0) {
                // Demuxer exhausted: flush the decoder (one transition),
                // then a later EAGAIN return ends the stream cleanly.
                endOfFile_ = true;
                avcodec_send_packet(codec_.get(), nullptr);
                break;
            }
            if (packet_->stream_index != audioStreamIndex_) {
                av_packet_unref(packet_.get());
                continue;
            }
            const int src = avcodec_send_packet(codec_.get(), packet_.get());
            av_packet_unref(packet_.get());
            if (src < 0) {
                error = "audio packet send failed: " + fcError(src);
                return false;
            }
            break;
        }
    }
}

} // namespace fc
