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
};

} // namespace fc
