#include "timecode.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace fc {

const FrameRate FrameRate::Fps23976{24000, 1001, false};
const FrameRate FrameRate::Fps24{24, 1, false};
const FrameRate FrameRate::Fps25{25, 1, false};
const FrameRate FrameRate::Fps2997NDF{30000, 1001, false};
const FrameRate FrameRate::Fps2997DF{30000, 1001, true};
const FrameRate FrameRate::Fps30{30, 1, false};
const FrameRate FrameRate::Fps50{50, 1, false};
const FrameRate FrameRate::Fps5994{60000, 1001, false};
const FrameRate FrameRate::Fps60{60, 1, false};

namespace {

// Frame count displayed per timecode "second". For fractional rates this
// is the rounded integer (30000/1001 -> 30), matching standard non-drop
// display.
int displayFps(const FrameRate &rate) {
    if (rate.num % rate.den == 0) {
        return static_cast<int>(rate.num / rate.den);
    }
    return static_cast<int>(
        std::lround(static_cast<double>(rate.num) / static_cast<double>(rate.den)));
}

int toInt(const std::string &digits) {
    if (digits.empty() || digits.size() > 6) {
        return -1; // empty or too long to be a sane field
    }
    return std::atoi(digits.c_str());
}

} // namespace

Timecode::Timecode(int64_t frames, FrameRate rate) : frames_(frames), rate_(rate) {}

Timecode Timecode::fromFrames(int64_t frames, FrameRate rate) {
    return Timecode(frames, rate);
}

Timecode Timecode::fromSeconds(double seconds, FrameRate rate) {
    const double frames = seconds * static_cast<double>(rate.num) / static_cast<double>(rate.den);
    return Timecode(static_cast<int64_t>(std::llround(frames)), rate);
}

double Timecode::totalSeconds() const {
    return static_cast<double>(frames_) * static_cast<double>(rate_.den) /
           static_cast<double>(rate_.num);
}

int Timecode::hours() const noexcept {
    const int64_t absFrames = frames_ < 0 ? -frames_ : frames_;
    return static_cast<int>(absFrames / (static_cast<int64_t>(displayFps(rate_)) * 3600));
}

int Timecode::minutes() const noexcept {
    const int64_t absFrames = frames_ < 0 ? -frames_ : frames_;
    return static_cast<int>((absFrames / (static_cast<int64_t>(displayFps(rate_)) * 60)) % 60);
}

int Timecode::seconds() const noexcept {
    const int64_t absFrames = frames_ < 0 ? -frames_ : frames_;
    return static_cast<int>((absFrames / static_cast<int64_t>(displayFps(rate_))) % 60);
}

int Timecode::frames() const noexcept {
    const int64_t absFrames = frames_ < 0 ? -frames_ : frames_;
    return static_cast<int>(absFrames % static_cast<int64_t>(displayFps(rate_)));
}

std::string Timecode::toString() const {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s%02d:%02d:%02d:%02d", frames_ < 0 ? "-" : "", hours(),
                  minutes(), seconds(), frames());
    return std::string(buf);
}

bool Timecode::parse(const std::string &text, FrameRate rate, Timecode &out) {
    if (!rate.isValid()) {
        return false;
    }

    std::string s = text;
    bool negative = false;
    if (!s.empty() && (s[0] == '-' || s[0] == '+')) {
        negative = (s[0] == '-');
        s.erase(0, 1);
    }

    std::vector<int> parts;
    std::string current;
    for (char c : s) {
        if (c == ':' || c == ';') {
            const int value = toInt(current);
            if (value < 0) {
                return false;
            }
            parts.push_back(value);
            current.clear();
        } else if (c >= '0' && c <= '9') {
            current += c;
        } else {
            return false;
        }
    }
    const int last = toInt(current);
    if (last < 0) {
        return false;
    }
    parts.push_back(last);

    if (parts.size() != 3 && parts.size() != 4) {
        return false;
    }

    int hh = 0;
    int mm = 0;
    int ss = 0;
    int ff = 0;
    if (parts.size() == 4) {
        hh = parts[0];
        mm = parts[1];
        ss = parts[2];
        ff = parts[3];
    } else {
        mm = parts[0];
        ss = parts[1];
        ff = parts[2];
    }

    const int fps = displayFps(rate);
    if (hh < 0 || hh > 999 || mm < 0 || mm > 59 || ss < 0 || ss > 59) {
        return false;
    }
    if (ff < 0 || ff >= fps) {
        return false;
    }

    int64_t frames = ((static_cast<int64_t>(hh) * 3600 + mm * 60 + ss) * fps) + ff;
    if (negative) {
        frames = -frames;
    }
    out = Timecode(frames, rate);
    return true;
}

Timecode Timecode::operator+(const Timecode &other) const {
    return Timecode(frames_ + other.frames_, rate_);
}

Timecode Timecode::operator-(const Timecode &other) const {
    return Timecode(frames_ - other.frames_, rate_);
}

bool Timecode::operator==(const Timecode &other) const {
    return frames_ == other.frames_ && rate_.num == other.rate_.num && rate_.den == other.rate_.den;
}

} // namespace fc
