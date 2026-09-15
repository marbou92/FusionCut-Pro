#include "ffmpeg_wrappers.h"

#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fc {

std::string fcError(int errnum) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    if (av_strerror(errnum, buf, sizeof(buf)) == 0) {
        return std::string(buf);
    }
    return "unknown FFmpeg error " + std::to_string(errnum);
}

std::string ffmpegVersionInfo() {
    std::string info;
    info += "libavcodec ";
    info += std::to_string(avcodec_version());
    info += ", libavformat ";
    info += std::to_string(avformat_version());
    info += ", libavutil ";
    info += std::to_string(avutil_version());
    info += ", runtime: ";
    info += av_version_info();
    return info;
}

FrameRate avRationalToFrameRate(AVRational rational) {
    if (rational.num <= 0 || rational.den <= 0) {
        return FrameRate(); // 24/1 fallback
    }
    FrameRate rate;
    rate.num = static_cast<uint32_t>(rational.num);
    rate.den = static_cast<uint32_t>(rational.den);
    return rate;
}

AVRational frameRateToAv(const FrameRate &rate) {
    if (!rate.isValid()) {
        return AVRational{1, 24};
    }
    return AVRational{static_cast<int>(rate.num), static_cast<int>(rate.den)};
}

bool removeFileUtf8(const std::string &path) {
#ifdef _WIN32
    // The media layer speaks UTF-8 (FFmpeg's filename contract, and what
    // QString::toStdString produces), but the msvcrt narrow std::remove
    // interprets bytes in the system ANSI code page - any non-ASCII
    // character outside the user's code page (e.g. an export target on a
    // desktop of a differently-localized Windows) makes the deletion
    // silently miss the file. Go through the wide API instead.
    const int wideLen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (wideLen <= 0) {
        return false;
    }
    std::wstring wide(static_cast<size_t>(wideLen), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wide[0], wideLen) <= 0) {
        return false;
    }
    return DeleteFileW(wide.c_str()) != 0;
#else
    return std::remove(path.c_str()) == 0;
#endif
}

} // namespace fc
