#include "audio_math.h"

#include <cmath>

namespace fc {

double audioDbToLinear(double db) {
    if (std::isnan(db)) {
        return 0.0;
    }
    if (db <= -120.0) {
        return 0.0; // -inf-ish: silence
    }
    return std::pow(10.0, db / 20.0);
}

void audioPanCoefficients(double pan, double &left, double &right) {
    if (std::isnan(pan)) {
        left = 0.0;
        right = 0.0;
        return;
    }
    if (pan < -1.0) {
        pan = -1.0;
    } else if (pan > 1.0) {
        pan = 1.0;
    }
    // Constant power: theta sweeps 0 (hard left) -> pi/2 (hard right).
    // left = cos(theta), right = sin(theta); the identity
    // left^2 + right^2 == 1 holds everywhere.
    const double theta = (pan + 1.0) * 0.78539816339744830962; // (pan+1) * pi/4
    left = std::cos(theta);
    right = std::sin(theta);
}

double audioFadeMultiplier(double tSec, double fadeSec) {
    if (!(fadeSec > 0.0) || std::isnan(fadeSec)) {
        return 1.0; // no fade configured
    }
    if (std::isnan(tSec)) {
        return 0.0;
    }
    if (tSec <= 0.0) {
        return 0.0;
    }
    if (tSec >= fadeSec) {
        return 1.0;
    }
    return tSec / fadeSec;
}

float audioClipSample(float v) {
    if (std::isnan(v) || std::isinf(v)) {
        return 0.0f;
    }
    if (v > 1.0f) {
        return 1.0f;
    }
    if (v < -1.0f) {
        return -1.0f;
    }
    return v;
}

void audioFinishStereoWindow(float *dst, size_t frames, double masterGain) {
    const double g = std::isnan(masterGain) ? 0.0 : masterGain;
    for (size_t i = 0; i < frames; ++i) {
        dst[2 * i] = audioClipSample(static_cast<float>(static_cast<double>(dst[2 * i]) * g));
        dst[2 * i + 1] =
            audioClipSample(static_cast<float>(static_cast<double>(dst[2 * i + 1]) * g));
    }
}

} // namespace fc
