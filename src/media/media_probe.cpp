#include "media_probe.h"

#include "ffmpeg_wrappers.h"

namespace fc {

namespace {

std::string codecNameFor(const AVCodecParameters *par) {
    const char *name = avcodec_get_name(par->codec_id);
    return name ? std::string(name) : std::string("unknown");
}

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
std::string describeLayout(const AVChannelLayout &layout) {
    char buf[64] = {0};
    if (av_channel_layout_describe(&layout, buf, sizeof(buf)) == 0) {
        return std::string(buf);
    }
    return std::string();
}
#else
std::string describeLayout(uint64_t mask, int channels) {
    if (mask == 0) {
        mask = av_get_default_channel_layout(channels);
    }
    char buf[64] = {0};
    av_get_channel_layout_string(buf, sizeof(buf), channels, mask);
    return std::string(buf);
}
#endif

void fillVideoStream(const AVFormatContext *fmt, const AVStream *stream, VideoStreamInfo &out) {
    out.streamIndex = stream->index;
    out.codecName = codecNameFor(stream->codecpar);
    out.width = stream->codecpar->width;
    out.height = stream->codecpar->height;
    const char *pixName = av_get_pix_fmt_name(
        stream->codecpar->format ? static_cast<AVPixelFormat>(stream->codecpar->format)
                                 : AV_PIX_FMT_NONE);
    out.pixelFormat = pixName ? std::string(pixName) : std::string("unknown");

    // Prefer the real base rate (r_frame_rate): it carries the stream's
    // actual tick rate (e.g. 30000/1001) and stays exact for CFR files,
    // while avg_frame_rate degenerates to frames/duration semantics
    // (e.g. 300/29) on the last-frame boundary of mp4 files.
    AVRational rate = stream->r_frame_rate;
    if (rate.num <= 0 || rate.den <= 0) {
        rate = stream->avg_frame_rate;
    }
    out.frameRate = avRationalToFrameRate(rate);

    if (stream->nb_frames > 0) {
        out.frameCount = static_cast<int64_t>(stream->nb_frames);
    } else if (fmt->duration > 0) {
        out.frameCount = static_cast<int64_t>((static_cast<double>(fmt->duration) / AV_TIME_BASE) *
                                              out.frameRate.toDouble());
    }
}

void fillAudioStream(const AVStream *stream, AudioStreamInfo &out) {
    out.streamIndex = stream->index;
    out.codecName = codecNameFor(stream->codecpar);
    out.sampleRate = stream->codecpar->sample_rate;
    out.sampleFormat =
        std::string(av_get_sample_fmt_name(static_cast<AVSampleFormat>(stream->codecpar->format)));

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
    out.channels = stream->codecpar->ch_layout.nb_channels;
    out.channelLayout = describeLayout(stream->codecpar->ch_layout);
#else
    out.channels = stream->codecpar->channels;
    out.channelLayout =
        describeLayout(stream->codecpar->channel_layout, stream->codecpar->channels);
#endif
}

} // namespace

bool MediaProbe::probe(const std::string &path, MediaInfo &out, std::string &error) {
    out = MediaInfo();
    out.path = path;

    AVFormatContext *rawCtx = nullptr;
    int rc = avformat_open_input(&rawCtx, path.c_str(), nullptr, nullptr);
    if (rc < 0) {
        error = "open failed: " + fcError(rc);
        return false;
    }
    FormatContextPtr ctx(rawCtx);

    rc = avformat_find_stream_info(ctx.get(), nullptr);
    if (rc < 0) {
        error = "stream info failed: " + fcError(rc);
        return false;
    }

    out.container = ctx->iformat->long_name ? ctx->iformat->long_name : "";
    out.formatName = ctx->iformat->name ? ctx->iformat->name : "";
    out.durationUs = ctx->duration; // AV_TIME_BASE units, may be AV_NOPTS_VALUE

    const int videoIdx = av_find_best_stream(ctx.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoIdx >= 0) {
        out.hasVideo = true;
        fillVideoStream(ctx.get(), ctx->streams[videoIdx], out.video);
    }

    for (unsigned int i = 0; i < ctx->nb_streams; ++i) {
        const AVStream *stream = ctx->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            AudioStreamInfo audio;
            fillAudioStream(stream, audio);
            out.audioStreams.push_back(audio);
        }
    }

    return true;
}

} // namespace fc
