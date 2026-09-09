#pragma once

#include <cstddef>

namespace fc {

// ---------------------------------------------------------------------------
// Audio math - the shared, dependency-free primitives of the mixing
// pipeline. The media layer's window mixer (multi-track summing for
// export), the preview player, and the mixer panel all evaluate through
// THESE functions so every path agrees on the numbers and the unit
// tests pin them once.
//
// Pure fc_core: no Qt, no FFmpeg, no state.
// ---------------------------------------------------------------------------

// Decibels -> linear amplitude. -inf-ish values clamp to 0 (the mixer
// panel floor is -60 dB, but arithmetic callers may pass anything);
// anything above +12 dB also comes from the panel (its ceiling is +6).
//   audioDbToLinear(0)   == 1
//   audioDbToLinear(-6.02) ~= 0.5
double audioDbToLinear(double db);

// Constant-power pan law. `pan` in [-1..1]: -1 = hard left, 0 = center,
// +1 = hard right. For every position left*left + right*right == 1, so
// a panned source keeps constant perceived power (center = sqrt(0.5)
// on each side, NOT 1.0 - the classic mistake that makes the center
// position +3 dB louder). Out-of-range pans clamp.
void audioPanCoefficients(double pan, double &left, double &right);

// Linear fade multiplier for a position `tSec` inside a fade of
// `fadeSec` seconds: ramps 0 -> 1 across [0, fadeSec], 0 before, 1
// after. fadeSec <= 0 (or non-finite) disables the fade: always 1.
// Used for clip-boundary audio fades (click-free cut edges) - the
// export and the preview evaluate the same ramp at the same position.
double audioFadeMultiplier(double tSec, double fadeSec);

// Hard-clips one float sample into [-1, 1]. Non-finite input maps to 0
// (a NaN must never reach an encoder or an audio device). This is the
// LAST stage of mixing, after every gain and every sum.
float audioClipSample(float v);

// Accumulates one interleaved STEREO output frame:
// dst[0] += srcL * gainL; dst[1] += srcR * gainR. (dst keeps its
// previous content - summing is the caller's loop.)
inline void audioAccumulateStereoFrame(float *dst, float srcL, float srcR, double gainL,
                                       double gainR) {
    dst[0] += static_cast<float>(static_cast<double>(srcL) * gainL);
    dst[1] += static_cast<float>(static_cast<double>(srcR) * gainR);
}

// Master limiting applied to a whole interleaved stereo window in
// place: master gain first, then the hard clip.
void audioFinishStereoWindow(float *dst, size_t frames, double masterGain);

} // namespace fc
