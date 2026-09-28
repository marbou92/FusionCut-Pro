#pragma once

#include <string>

#include "ffmpeg_wrappers.h"
#include "media_info.h"

namespace fc {

// Sequential/seeking decode pump for one video stream. Converts every
// decoded frame to packed RGBA. Owns all FFmpeg resources; reopen() closes
// any previously opened file.
class VideoDecoder {
public:
    VideoDecoder() = default;
    ~VideoDecoder() = default;

    VideoDecoder(const VideoDecoder &) = delete;
    VideoDecoder &operator=(const VideoDecoder &) = delete;

    // Opens `path` and prepares the first decodable video stream.
    bool open(const std::string &path, std::string &error);
    bool isOpen() const { return format_ != nullptr; }
    void close();

    const MediaInfo &info() const { return info_; }

    // Seeks backwards to the nearest keyframe at or before `seconds` and
    // flushes the decoder. Subsequent readFrame() calls decode forward from
    // that keyframe but skip frames until the first one with
    // pts >= seconds (half-frame tolerance), so callers resume exactly at
    // the requested position.
    bool seekToSeconds(double seconds, std::string &error);

    // Decodes the next frame. Returns false at end of stream (error is
    // left empty in that case) or on failure.
    bool readFrame(DecodedFrame &out, std::string &error);

    // Output scale for the PREVIEW pipeline: 1.0 (default) = native
    // resolution, 0.5 / 0.25 = frames are scaled down on the way out.
    // The H.264/H.265 decode itself stays full-resolution (a codec
    // cannot decode a downscaled picture); what the scale cuts is all
    // the per-frame work AFTER the codec - the full-frame YUV->RGBA
    // conversion, the QImage copy, the effect composite and the paint -
    // which is exactly the work that made 1/2 preview feel identical to
    // Full. Export is untouched: it renders through its own pipeline.
    void setOutputScale(double factor);
    double outputScale() const { return outputScale_; }

private:
    bool ensureScaler(std::string &error);

    MediaInfo info_;
    FormatContextPtr format_;
    CodecContextPtr codec_;
    SwsContextPtr scaler_;
    PacketPtr packet_;
    FramePtr frame_;
    int videoStreamIndex_ = -1;
    AVRational streamTimebase_{0, 1};
    bool endOfFile_ = false;
    double seekTarget_ = -1.0; // >= 0 while skipping to the seek target
    AVPixelFormat scalerSrcFormat_ = AV_PIX_FMT_NONE;
    int scalerSrcW_ = 0;
    int scalerSrcH_ = 0;
    double scalerSrcScale_ = 1.0;
    double outputScale_ = 1.0;
};

} // namespace fc
