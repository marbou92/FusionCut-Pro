#pragma once

#include <functional>
#include <string>

namespace fc {

// Tuning knobs for proxy generation (Module 3: "auto-generate low
// resolution proxies (360p) for smooth playback on low-end hardware").
struct ProxyConfig {
    int targetHeight = 360; // proxy height; sources are never upscaled
    int crf = 28;           // x264 constant-rate factor (higher = smaller)
    std::string preset = "veryfast";
    bool withAudio = true; // re-encode audio to 48 kHz stereo AAC
    int audioSampleRate = 48000;
};

// Called after each transcoded video frame with the fraction of the
// source duration completed [0..1]. Return false to cancel the job.
using ProxyProgress = std::function<bool(double fraction)>;

// Transcodes a source video into an edit-friendly low-resolution proxy.
// Falls back from H.264/x264 to MPEG-4 Part 2 when no H.264 encoder is
// present in the FFmpeg build.
class ProxyGenerator {
public:
    static bool generate(const std::string &srcPath, const std::string &dstPath,
                         const ProxyConfig &config, const ProxyProgress &progress,
                         std::string &error);
};

} // namespace fc
