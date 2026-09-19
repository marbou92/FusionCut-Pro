#pragma once

#include <QChar>
#include <QString>

#include <cmath>
#include <cstdint>

namespace fc {
namespace timecode_display {

// Display-side timecode for the APP layer (suggestions #23 + #26 +
// Batch 6+ item 8): drop-frame NUMBERING with the ';' separator,
// formatted/parsed from a plain double fps - the model keeps counting
// raw frames and is never touched.
//
// DF is defined by SMPTE 12M only for 29.97 and 59.94 fps. Any other
// rate formats as non-drop "HH:MM:SS:FF" even when `drop` is true.
// Frames saturate into [0, one day) so absurd inputs cannot truncate
// silently (the core Timecode has the same contract).

// True when `fps` is close enough to 30000/1001 or 60000/1001 for DF
// numbering to be meaningful (0.02 fps window catches both the exact
// rational rates and their common 5-decimal spellings).
inline bool isDropCapable(double fps) {
    return std::fabs(fps - 30000.0 / 1001.0) < 0.02 || std::fabs(fps - 60000.0 / 1001.0) < 0.02;
}

struct Components {
    int hours = 0;
    int minutes = 0;
    int seconds = 0;
    int frames = 0;
    char separator = ':'; // ';' only for drop-frame output
};

// Splits a frame index into display components. Negative inputs clamp
// to 0; inputs past one day saturate to the last frame of a day.
// Returns false only when `fps` is not positive (no rate to count in).
inline bool components(int64_t frames, double fps, bool drop, Components *out) {
    if (fps <= 0.0 || out == nullptr) {
        return false;
    }
    const int64_t base = static_cast<int64_t>(std::llround(fps)); // 30 / 60 / 24 ...
    const bool df = drop && isDropCapable(fps);
    const int64_t dropPerMin = df ? static_cast<int64_t>(std::llround(fps * 2.0 / 30.0)) : 0;
    const int64_t fullMinute = base * 60;                   // 1800 / 3600
    const int64_t droppedMinute = fullMinute - dropPerMin;  // 1798 / 3596
    const int64_t block = 10 * fullMinute - 9 * dropPerMin; // 17982 / 35964
    const int64_t framesPerDay = 24 * 6 * block;

    int64_t f = frames;
    if (f < 0) {
        f = 0;
    }
    if (f >= framesPerDay) {
        f = framesPerDay - 1;
    }

    int64_t hour = 0;
    int64_t minute = 0;
    int64_t frameInMinute = 0;
    if (!df) {
        const int64_t totalSeconds = f / base;
        hour = totalSeconds / 3600;
        minute = (totalSeconds / 60) % 60;
        frameInMinute = f % base + (totalSeconds % 60) * base;
    } else {
        const int64_t blocks = f / block;
        const int64_t rem = f % block;
        hour = blocks / 6;
        const int64_t minuteBase = (blocks % 6) * 10; // 00/10/20/30/40/50
        if (rem < fullMinute) {
            // the un-dropped minute of the block (minute 0 of the ten)
            minute = minuteBase;
            frameInMinute = rem;
        } else {
            const int64_t k = (rem - fullMinute) / droppedMinute; // 0..8
            minute = minuteBase + 1 + k;
            // the first `dropPerMin` frame NUMBERS of a dropped minute
            // do not exist; display numbering starts at dropPerMin.
            frameInMinute = dropPerMin + (rem - fullMinute) % droppedMinute;
        }
    }

    out->hours = static_cast<int>(hour);
    out->minutes = static_cast<int>(minute);
    out->separator = df ? ';' : ':';
    out->seconds = static_cast<int>(frameInMinute / base);
    out->frames = static_cast<int>(frameInMinute % base);
    return true;
}

// "HH:MM:SS;FF" (drop) or "HH:MM:SS:FF" (non-drop). A non-positive fps
// yields the zero timecode rather than a nonsense string.
inline QString format(int64_t frames, double fps, bool drop) {
    Components c;
    if (!components(frames, fps, drop, &c)) {
        return QStringLiteral("00:00:00:00");
    }
    return QString("%1:%2:%3%5%4")
        .arg(c.hours, 2, 10, QChar('0'))
        .arg(c.minutes, 2, 10, QChar('0'))
        .arg(c.seconds, 2, 10, QChar('0'))
        .arg(c.frames, 2, 10, QChar('0'))
        .arg(QChar(c.separator));
}

// Parses "HH:MM:SS:FF", "HH:MM:SS;FF", "MM:SS:FF", "MM:SS;FF", "SS:FF"
// or "SS;FF" (2-4 fields, either separator; the separator TYPE never
// changes the math - the rate decides DF). Field ranges: h/m/s 0-59
// except hours (0-23 when 4 fields), frames 0..llround(fps)-1.
// Dropped frame numbers typed at a dropped minute start map onto the
// same timeline frame as the first existing number (lenient, stable).
// On success writes the timeline frame index and returns true.
inline bool parse(const QString &text, double fps, bool drop, int64_t *outFrames) {
    if (fps <= 0.0 || outFrames == nullptr) {
        return false;
    }
    const int64_t base = static_cast<int64_t>(std::llround(fps));
    const bool df = drop && isDropCapable(fps);
    const int64_t dropPerMin = df ? static_cast<int64_t>(std::llround(fps * 2.0 / 30.0)) : 0;

    // tokenize on ':' and ';' (also accept '.' for locale-typed input)
    int64_t fields[4] = {0, 0, 0, 0};
    int count = 0;
    int64_t current = -1; // -1 = no digits yet in this field
    int digits = 0;
    for (int i = 0; i <= text.length(); ++i) {
        const QChar ch = i < text.length() ? text.at(i) : QChar(':');
        const char cc = ch.toLatin1();
        if (cc == ':' || cc == ';' || cc == '.') {
            if (digits == 0) {
                return false; // empty field / leading separator
            }
            if (count >= 4) {
                return false;
            }
            fields[count++] = current;
            current = -1;
            digits = 0;
            continue;
        }
        if (!ch.isDigit()) {
            return false;
        }
        if (current < 0) {
            current = 0;
        }
        current = current * 10 + ch.digitValue();
        if (++digits > 3) {
            return false; // no field is ever wider than "000"
        }
    }
    if (count < 2) {
        return false; // need at least SS:FF
    }

    const int64_t ff = fields[count - 1]; // last field = frames
    const int64_t ss = fields[count - 2];
    const int64_t mm = count >= 3 ? fields[count - 3] : 0;
    const int64_t hh = count >= 4 ? fields[count - 4] : 0;
    if (ff >= base || ss > 59 || mm > 59 || (count >= 4 && hh > 23)) {
        return false;
    }

    const int64_t totalMinutes = hh * 60 + mm;
    int64_t frames = (totalMinutes * 60 + ss) * base + ff;
    if (df) {
        frames -= dropPerMin * (totalMinutes - totalMinutes / 10);
    }
    if (frames < 0) {
        return false; // before first frame
    }
    *outFrames = frames;
    return true;
}

} // namespace timecode_display
} // namespace fc
