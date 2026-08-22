#pragma once

#include <cstdint>
#include <string>

namespace fc {

// Rational frame rate. Supports integer rates (24, 25, 30, 50, 60) and
// NTSC-style fractional rates (24000/1001, 30000/1001, 60000/1001).
//
// dropFrame marks a rate that should *present* with drop-frame numbering.
// v0.1 stores the flag; drop-frame rendering ships with the timeline
// engine, which owns frame-number arithmetic across playback.
struct FrameRate {
    uint32_t num = 24;
    uint32_t den = 1;
    bool dropFrame = false;

    bool isValid() const noexcept { return num > 0 && den > 0; }
    double toDouble() const noexcept {
        return static_cast<double>(num) / static_cast<double>(den);
    }

    static const FrameRate Fps23976;
    static const FrameRate Fps24;
    static const FrameRate Fps25;
    static const FrameRate Fps2997NDF;
    static const FrameRate Fps2997DF;
    static const FrameRate Fps30;
    static const FrameRate Fps50;
    static const FrameRate Fps5994;
    static const FrameRate Fps60;
};

// Non-drop timecode over a rational frame rate.
// Format: "HH:MM:SS:FF" (with optional leading '-' for negative values).
class Timecode {
public:
    Timecode() = default;
    Timecode(int64_t frames, FrameRate rate);

    static Timecode fromFrames(int64_t frames, FrameRate rate);
    static Timecode fromSeconds(double seconds, FrameRate rate);

    // Parses "HH:MM:SS:FF" or "MM:SS:FF". Accepts ';' in place of ':' and
    // a leading '+'/'-'. Returns false on invalid input or out-of-range
    // fields (seconds/minutes must be 0-59, frames 0..fps-1).
    static bool parse(const std::string &text, FrameRate rate, Timecode &out);

    int64_t totalFrames() const noexcept { return frames_; }
    double totalSeconds() const;
    FrameRate rate() const noexcept { return rate_; }

    // Components are computed on the absolute value of the frame count.
    int hours() const noexcept;
    int minutes() const noexcept;
    int seconds() const noexcept;
    int frames() const noexcept;

    std::string toString() const;

    // Arithmetic operates on raw frame counts using the left-hand rate;
    // callers are expected to combine timecodes of the same rate.
    Timecode operator+(const Timecode &other) const;
    Timecode operator-(const Timecode &other) const;
    bool operator==(const Timecode &other) const;

private:
    int64_t frames_ = 0;
    FrameRate rate_ = FrameRate::Fps24;
};

} // namespace fc
