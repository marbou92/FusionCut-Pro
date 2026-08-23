#pragma once

#include <string>
#include <vector>

#include "timecode.h"

namespace fc {

// Dependency-free description of a media file. UI code may include this
// header without pulling in any FFmpeg headers.
struct VideoStreamInfo {
    int streamIndex = -1;
    std::string codecName;   // "h264", "hevc", "vp9", ...
    int width = 0;           // pixels
    int height = 0;          // pixels
    FrameRate frameRate;     // average frame rate
    int64_t frameCount = 0;  // exact or estimated; 0 when unknown
    std::string pixelFormat; // "yuv420p", "nv12", ...
};

struct AudioStreamInfo {
    int streamIndex = -1;
    std::string codecName; // "aac", "mp3", "flac", ...
    int sampleRate = 0;    // Hz
    int channels = 0;
    std::string channelLayout; // "stereo", "mono", "5.1", ...
    std::string sampleFormat;  // "fltp", "s16", ...
};

struct MediaInfo {
    std::string path;
    std::string container; // "mov,mp4,m4a,3gp,3g2,mj2" style long name
    std::string formatName;
    int64_t durationUs = 0; // AV_TIME_BASE units (microseconds)

    bool hasVideo = false;
    VideoStreamInfo video;
    std::vector<AudioStreamInfo> audioStreams;

    double durationSeconds() const { return static_cast<double>(durationUs) / 1000000.0; }
};

// One decoded video frame converted to packed 8-bit RGBA.
struct DecodedFrame {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba; // width * height * 4 bytes
    double ptsSeconds = 0.0;   // presentation time in source seconds
    int64_t pts = 0;           // raw pts in stream timebase units
};

} // namespace fc
