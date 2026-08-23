#include "video_decoder.h"

#include <cmath>

#include "media_probe.h"

namespace fc {

bool VideoDecoder::open(const std::string &path, std::string &error) {
    close();

    if (!MediaProbe::probe(path, info_, error)) { // includes media_probe.h via chain
        return false;
    }
    if (!info_.hasVideo) {
        error = "no video stream in " + path;
        return false;
    }

    AVFormatContext *rawFmt = nullptr;
    int rc = avformat_open_input(&rawFmt, path.c_str(), nullptr, nullptr);
    if (rc < 0) {
        error = "open failed: " + fcError(rc);
        return false;
    }
    format_.reset(rawFmt);
    if (avformat_find_stream_info(format_.get(), nullptr) < 0) {
        error = "stream info failed";
        return false;
    }

    videoStreamIndex_ = info_.video.streamIndex;
    AVStream *stream = format_->streams[videoStreamIndex_];
    streamTimebase_ = stream->time_base;

    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        error = "no decoder for codec " + info_.video.codecName;
        return false;
    }

    codec_.reset(avcodec_alloc_context3(codec));
    if (!codec_) {
        error = "codec context alloc failed";
        return false;
    }
    if (avcodec_parameters_to_context(codec_.get(), stream->codecpar) < 0) {
        error = "codec parameters copy failed";
        return false;
    }
    if (avcodec_open2(codec_.get(), codec, nullptr) < 0) {
        error = "codec open failed";
        return false;
    }

    packet_.reset(av_packet_alloc());
    frame_.reset(av_frame_alloc());
    endOfFile_ = false;
    scaler_.reset();
    return true;
}

void VideoDecoder::close() {
    scaler_.reset();
    frame_.reset();
    packet_.reset();
    codec_.reset();
    format_.reset();
    videoStreamIndex_ = -1;
    endOfFile_ = false;
    info_ = MediaInfo();
}

bool VideoDecoder::seekToSeconds(double seconds, std::string &error) {
    if (!isOpen()) {
        error = "decoder not open";
        return false;
    }
    AVStream *stream = format_->streams[videoStreamIndex_];
    const int64_t timestamp =
        static_cast<int64_t>(std::llround(seconds / av_q2d(stream->time_base)));
    const int rc = av_seek_frame(format_.get(), videoStreamIndex_, timestamp, AVSEEK_FLAG_BACKWARD);
    if (rc < 0) {
        error = "seek failed: " + fcError(rc);
        return false;
    }
    avcodec_flush_buffers(codec_.get());
    endOfFile_ = false;
    seekTarget_ = seconds;
    return true;
}

bool VideoDecoder::readFrame(DecodedFrame &out, std::string &error) {
    if (!isOpen()) {
        error = "decoder not open";
        return false;
    }

    while (true) {
        // Drain any frames the codec is still holding.
        int rc = avcodec_receive_frame(codec_.get(), frame_.get());
        if (rc == 0) {
            if (!ensureScaler(error)) {
                return false;
            }

            out.width = codec_->width;
            out.height = codec_->height;
            out.rgba.assign(static_cast<size_t>(out.width) * out.height * 4, 0);
            out.pts = frame_->pts != AV_NOPTS_VALUE ? frame_->pts : frame_->best_effort_timestamp;

            uint8_t *dst = out.rgba.data();
            int dstStride = out.width * 4;
            sws_scale(scaler_.get(), frame_->data, frame_->linesize, 0, out.height, &dst,
                      &dstStride);
            out.ptsSeconds = static_cast<double>(out.pts) * av_q2d(streamTimebase_);
            av_frame_unref(frame_.get());

            // While resuming from a seek, drop frames that precede the
            // requested timestamp (half-frame tolerance for rounding).
            if (seekTarget_ >= 0.0) {
                const double halfFrame = 0.5 / std::max(1.0, info_.video.frameRate.toDouble());
                if (out.ptsSeconds < seekTarget_ - halfFrame) {
                    continue;
                }
                seekTarget_ = -1.0;
            }
            return true;
        }
        if (rc == AVERROR(EAGAIN)) {
            // Need more input.
        } else if (rc == AVERROR_EOF) {
            seekTarget_ = -1.0;
            return false; // clean end of stream
        } else {
            error = "decode failed: " + fcError(rc);
            return false;
        }

        if (endOfFile_) {
            // Feed the decoder a flush packet to drain remaining frames.
            rc = avcodec_send_packet(codec_.get(), nullptr);
            if (rc < 0 && rc != AVERROR_EOF) {
                error = "flush failed: " + fcError(rc);
                return false;
            }
            continue;
        }

        rc = av_read_frame(format_.get(), packet_.get());
        if (rc < 0) {
            if (rc == AVERROR_EOF) {
                endOfFile_ = true;
                continue;
            }
            error = "read failed: " + fcError(rc);
            return false;
        }
        if (packet_->stream_index == videoStreamIndex_) {
            // mp4 demuxers may flag trailing packets AV_PKT_FLAG_DISCARD when
            // the sample pts coincides with the container duration boundary.
            // Those are real frames for an editor - keep decoding them.
            packet_->flags &= ~AV_PKT_FLAG_DISCARD;
            rc = avcodec_send_packet(codec_.get(), packet_.get());
            if (rc < 0) {
                av_packet_unref(packet_.get());
                error = "send failed: " + fcError(rc);
                return false;
            }
        }
        av_packet_unref(packet_.get());
    }
}

bool VideoDecoder::ensureScaler(std::string &error) {
    const AVPixelFormat srcFmt = codec_->pix_fmt;
    if (scaler_ && scalerSrcFormat_ == srcFmt && scalerSrcW_ == codec_->width &&
        scalerSrcH_ == codec_->height) {
        return true;
    }
    scaler_.reset(sws_getContext(codec_->width, codec_->height, srcFmt, codec_->width,
                                 codec_->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr,
                                 nullptr));
    if (!scaler_) {
        error = "swscale context creation failed";
        return false;
    }
    scalerSrcFormat_ = srcFmt;
    scalerSrcW_ = codec_->width;
    scalerSrcH_ = codec_->height;
    return true;
}

} // namespace fc
