#include "ffmpeg_wrappers.h"

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

} // namespace fc
