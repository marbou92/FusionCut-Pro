#include "transitions.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fc {

// ---------------------------------------------------------------------------
// Shared pixel helpers (same discipline as effects.cpp: integer or
// explicitly-rounded double math, deterministic across toolchains).
// ---------------------------------------------------------------------------

namespace {

inline uint8_t clampByte(double v) {
    return static_cast<uint8_t>(v < 0.0 ? 0.0 : (v > 255.0 ? 255.0 : v));
}

inline uint8_t mixByte(uint8_t a, uint8_t b, double t) {
    return clampByte(std::lround(static_cast<double>(a) + (static_cast<double>(b) - a) * t));
}

// Fixed integer hash (same family as the film grain in effects.cpp).
inline uint32_t transHash(uint32_t x, uint32_t y, uint32_t s) {
    uint32_t h = x * 0x9E3779B1u + y * 0x85EBCA77u + s * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x2545F491u;
    h ^= h >> 13;
    return h;
}

inline size_t pixIndex(int w, int x, int y) {
    return (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
}

inline void copyPixel(const uint8_t *src, uint8_t *dst) {
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst[3] = src[3];
}

// Linear blend of two pixels into out (alpha blended too).
inline void blendPixel(const uint8_t *a, const uint8_t *b, uint8_t *out, double t) {
    out[0] = mixByte(a[0], b[0], t);
    out[1] = mixByte(a[1], b[1], t);
    out[2] = mixByte(a[2], b[2], t);
    out[3] = mixByte(a[3], b[3], t);
}

// Nearest-neighbor source read with edge clamp.
inline const uint8_t *sampleAt(const uint8_t *src, int w, int h, int x, int y) {
    const int cx = x < 0 ? 0 : (x > w - 1 ? w - 1 : x);
    const int cy = y < 0 ? 0 : (y > h - 1 ? h - 1 : y);
    return src + pixIndex(w, cx, cy);
}

// Separable box blur, in place, edge-clamped (radius >= 1). Mirrors
// effects.cpp's boxBlurRGBA; duplicated here because the effects helper is
// file-local and the transitions engine must stay self-contained.
void boxBlurInPlace(uint8_t *rgba, int w, int h, int radius) {
    const int r = std::max(1, radius);
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    std::vector<uint8_t> src(rgba, rgba + n);
    const int window = 2 * r + 1;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int sum[4] = {0, 0, 0, 0};
            for (int k = -r; k <= r; ++k) {
                const size_t i = pixIndex(w, x < -k ? 0 : (x + k > w - 1 ? w - 1 : x + k), y);
                sum[0] += src[i];
                sum[1] += src[i + 1];
                sum[2] += src[i + 2];
                sum[3] += src[i + 3];
            }
            uint8_t *p = rgba + pixIndex(w, x, y);
            p[0] = clampByte(std::lround(double(sum[0]) / window));
            p[1] = clampByte(std::lround(double(sum[1]) / window));
            p[2] = clampByte(std::lround(double(sum[2]) / window));
            p[3] = clampByte(std::lround(double(sum[3]) / window));
        }
    }
    std::memcpy(src.data(), rgba, n);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int sum[4] = {0, 0, 0, 0};
            for (int k = -r; k <= r; ++k) {
                const int yy = y < -k ? 0 : (y + k > h - 1 ? h - 1 : y + k);
                const size_t i = pixIndex(w, x, yy);
                sum[0] += src[i];
                sum[1] += src[i + 1];
                sum[2] += src[i + 2];
                sum[3] += src[i + 3];
            }
            uint8_t *p = rgba + pixIndex(w, x, y);
            p[0] = clampByte(std::lround(double(sum[0]) / window));
            p[1] = clampByte(std::lround(double(sum[1]) / window));
            p[2] = clampByte(std::lround(double(sum[2]) / window));
            p[3] = clampByte(std::lround(double(sum[3]) / window));
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Catalog (external linkage - the UI and the model enumerate it).
// ---------------------------------------------------------------------------

const std::vector<TransitionDescriptor> &transitionCatalog() {
    static const std::vector<TransitionDescriptor> catalog = {
        // Dissolve (6)
        {"dissolve.cross", "Cross Dissolve", "Dissolve"},
        {"dissolve.dip-black", "Dip to Black", "Dissolve"},
        {"dissolve.dip-white", "Dip to White", "Dissolve"},
        {"dissolve.additive", "Additive Dissolve", "Dissolve"},
        {"dissolve.film", "Film Dissolve", "Dissolve"},
        {"dissolve.blur", "Blur Dissolve", "Dissolve"},
        // Wipe (19)
        {"wipe.left", "Wipe Left", "Wipe"},
        {"wipe.right", "Wipe Right", "Wipe"},
        {"wipe.up", "Wipe Up", "Wipe"},
        {"wipe.down", "Wipe Down", "Wipe"},
        {"wipe.corner-tl", "Wipe Corner Top-Left", "Wipe"},
        {"wipe.corner-tr", "Wipe Corner Top-Right", "Wipe"},
        {"wipe.corner-bl", "Wipe Corner Bottom-Left", "Wipe"},
        {"wipe.corner-br", "Wipe Corner Bottom-Right", "Wipe"},
        {"wipe.iris-box", "Iris Box", "Wipe"},
        {"wipe.iris-box-out", "Iris Box Out", "Wipe"},
        {"wipe.circle", "Iris Circle", "Wipe"},
        {"wipe.diamond", "Iris Diamond", "Wipe"},
        {"wipe.clock", "Clock Wipe", "Wipe"},
        {"wipe.clock-ccw", "Clock Wipe Reverse", "Wipe"},
        {"wipe.blinds-h", "Blinds Horizontal", "Wipe"},
        {"wipe.blinds-v", "Blinds Vertical", "Wipe"},
        {"wipe.checker", "Checker Wipe", "Wipe"},
        {"wipe.barn-h", "Barn Doors Horizontal", "Wipe"},
        {"wipe.barn-v", "Barn Doors Vertical", "Wipe"},
        // Slide (4): the incoming clip slides in over the static outgoing
        // clip, entering at the named screen edge.
        {"slide.from-left", "Slide In From Left", "Slide"},
        {"slide.from-right", "Slide In From Right", "Slide"},
        {"slide.from-top", "Slide In From Top", "Slide"},
        {"slide.from-bottom", "Slide In From Bottom", "Slide"},
        // Push (4): the incoming clip enters at the named edge and pushes
        // the outgoing clip out of the frame (both translate).
        {"push.from-left", "Push From Left", "Push"},
        {"push.from-right", "Push From Right", "Push"},
        {"push.from-top", "Push From Top", "Push"},
        {"push.from-bottom", "Push From Bottom", "Push"},
        // Zoom (3)
        {"zoom.in", "Zoom In", "Zoom"},
        {"zoom.out", "Zoom Out", "Zoom"},
        {"zoom.through", "Zoom Through", "Zoom"},
    };
    return catalog;
}

const TransitionDescriptor *findTransition(const std::string &id) {
    for (const TransitionDescriptor &d : transitionCatalog()) {
        if (d.id == id) {
            return &d;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Dissolves.
// ---------------------------------------------------------------------------

// Generic per-pixel blend of A and B with weight t(x, y) in [0, 1].
void blendByWeight(const uint8_t *a, const uint8_t *b, uint8_t *out, int w, int h, double p,
                   double (*weight)(int, int, double)) {
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = pixIndex(w, x, y);
            blendPixel(a + i, b + i, out + i, weight(x, y, p));
        }
    }
}

double weightCross(int, int, double p) {
    return p;
}

double weightFilm(int x, int y, double p) {
    const double n =
        static_cast<double>(transHash(static_cast<uint32_t>(x), static_cast<uint32_t>(y), 777u)) /
        4294967296.0; // [0, 1)
    double t = p + 0.35 * p * (1.0 - p) * (2.0 * n - 1.0);
    if (t < 0.0) {
        t = 0.0;
    } else if (t > 1.0) {
        t = 1.0;
    }
    return t;
}

void applyCross(const uint8_t *a, const uint8_t *b, uint8_t *out, int w, int h, double p) {
    blendByWeight(a, b, out, w, h, p, weightCross);
}

void applyAdditive(const uint8_t *a, const uint8_t *b, uint8_t *out, int w, int h, double p) {
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    const double bump = 2.0 * p * (1.0 - p);
    for (size_t i = 0; i < n * 4; i += 4) {
        for (int c = 0; c < 4; ++c) {
            const double base =
                static_cast<double>(a[i + c]) + (static_cast<double>(b[i + c]) - a[i + c]) * p;
            const double screen = static_cast<double>(a[i + c]) * b[i + c] / 255.0;
            out[i + c] = clampByte(std::lround(base + bump * screen));
        }
    }
}

void applyDip(const uint8_t *a, const uint8_t *b, uint8_t *out, int w, int h, double p, uint8_t dr,
              uint8_t dg, uint8_t db) {
    uint8_t dip[4] = {dr, dg, db, 255};
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    for (size_t i = 0; i < n * 4; i += 4) {
        if (p < 0.5) {
            blendPixel(a + i, dip, out + i, p * 2.0);
        } else {
            blendPixel(dip, b + i, out + i, (p - 0.5) * 2.0);
        }
    }
}

void applyFilm(const uint8_t *a, const uint8_t *b, uint8_t *out, int w, int h, double p) {
    blendByWeight(a, b, out, w, h, p, weightFilm);
}

void applyBlurDissolve(const uint8_t *a, const uint8_t *b, uint8_t *out, int w, int h, double p) {
    applyCross(a, b, out, w, h, p);
    const int radius = static_cast<int>(std::lround(10.0 * p * (1.0 - p)));
    if (radius >= 1) {
        boxBlurInPlace(out, w, h, radius);
    }
}

// ---------------------------------------------------------------------------
// Wipes. Every wipe is a binary coverage function evaluated at pixel
// centers: through(x, y) true => the pixel shows B.
// ---------------------------------------------------------------------------

using ThroughFn = bool (*)(int, int, int, int, double);

void applyWipe(const uint8_t *a, const uint8_t *b, uint8_t *out, int w, int h, double p,
               ThroughFn through) {
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = pixIndex(w, x, y);
            copyPixel(through(x, y, w, h, p) ? b + i : a + i, out + i);
        }
    }
}

// Directional wipes follow the classic NLE convention: wipe.<dir> = the
// reveal EDGE travels in <dir>, so the incoming clip appears first at the
// OPPOSITE screen edge (Wipe Left reveals B starting at the right edge,
// sweeping left).
bool wipeLeft(int x, int, int w, int, double p) {
    return static_cast<double>(w - x) - 0.5 <= p * w;
}

bool wipeRight(int x, int, int w, int, double p) {
    return static_cast<double>(x) + 0.5 <= p * w;
}

bool wipeUp(int, int y, int, int h, double p) {
    return static_cast<double>(h - y) - 0.5 <= p * h;
}

bool wipeDown(int, int y, int, int h, double p) {
    return static_cast<double>(y) + 0.5 <= p * h;
}

// Corner wipes: distance of the pixel CENTER from the named corner, summed
// over both axes; the reveal edge is the iso-distance diagonal.
bool wipeCorner(int x, int y, int w, int h, double p, int cornerX, int cornerY) {
    const double dx = cornerX == 0 ? (x + 0.5) : (w - x - 0.5);
    const double dy = cornerY == 0 ? (y + 0.5) : (h - y - 0.5);
    return dx + dy <= p * (w + h);
}

bool wipeCornerTL(int x, int y, int w, int h, double p) {
    return wipeCorner(x, y, w, h, p, 0, 0);
}

bool wipeCornerTR(int x, int y, int w, int h, double p) {
    return wipeCorner(x, y, w, h, p, 1, 0);
}

bool wipeCornerBL(int x, int y, int w, int h, double p) {
    return wipeCorner(x, y, w, h, p, 0, 1);
}

bool wipeCornerBR(int x, int y, int w, int h, double p) {
    return wipeCorner(x, y, w, h, p, 1, 1);
}

bool wipeIrisBox(int x, int y, int w, int h, double p) {
    return std::fabs(x + 0.5 - w * 0.5) < p * w * 0.5 && std::fabs(y + 0.5 - h * 0.5) < p * h * 0.5;
}

bool wipeIrisBoxOut(int x, int y, int w, int h, double p) {
    const double q = 1.0 - p;
    return !(std::fabs(x + 0.5 - w * 0.5) < q * w * 0.5 &&
             std::fabs(y + 0.5 - h * 0.5) < q * h * 0.5);
}

bool wipeCircle(int x, int y, int w, int h, double p) {
    const double dx = x + 0.5 - w * 0.5;
    const double dy = y + 0.5 - h * 0.5;
    const double radius = std::sqrt(dx * dx + dy * dy);
    return radius < p * std::sqrt(static_cast<double>(w * w + h * h)) * 0.5;
}

bool wipeDiamond(int x, int y, int w, int h, double p) {
    return std::fabs(x + 0.5 - w * 0.5) + std::fabs(y + 0.5 - h * 0.5) < p * (w + h) * 0.5;
}

// Clock angle of the pixel center, clockwise from 12 o'clock, in [0, 2pi).
double clockAngle(int x, int y, int w, int h) {
    const double dx = x + 0.5 - w * 0.5;
    const double dy = h * 0.5 - (y + 0.5); // upward positive
    double phi = std::atan2(dx, dy);
    if (phi < 0.0) {
        phi += 2.0 * 3.14159265358979323846;
    }
    return phi;
}

bool wipeClock(int x, int y, int w, int h, double p) {
    return clockAngle(x, y, w, h) < p * 2.0 * 3.14159265358979323846;
}

bool wipeClockCcw(int x, int y, int w, int h, double p) {
    double phi = 2.0 * 3.14159265358979323846 - clockAngle(x, y, w, h);
    if (phi >= 2.0 * 3.14159265358979323846) {
        phi -= 2.0 * 3.14159265358979323846;
    }
    return phi < p * 2.0 * 3.14159265358979323846;
}

// Blinds: 8 fixed bands; the pixel's position inside its band gates it.
bool wipeBlindsH(int, int y, int, int h, double p) {
    const int bandH = (h + 7) / 8;
    const double u = static_cast<double>(y % bandH) + 0.5;
    return u / bandH <= p;
}

bool wipeBlindsV(int x, int, int w, int, double p) {
    const int bandW = (w + 7) / 8;
    const double u = static_cast<double>(x % bandW) + 0.5;
    return u / bandW <= p;
}

bool wipeChecker(int x, int y, int, int, double p) {
    const uint32_t h = transHash(static_cast<uint32_t>(x / 8), static_cast<uint32_t>(y / 8), 42u);
    const double threshold = static_cast<double>(h % 255u + 1u) / 256.0; // (0, 1)
    return p >= threshold;
}

bool wipeBarnH(int x, int, int w, int, double p) {
    return std::fabs(x + 0.5 - w * 0.5) >= (1.0 - p) * w * 0.5;
}

bool wipeBarnV(int, int y, int, int h, double p) {
    return std::fabs(y + 0.5 - h * 0.5) >= (1.0 - p) * h * 0.5;
}

// ---------------------------------------------------------------------------
// Slide / push: nearest-neighbor translation sampling at pixel centers.
// ---------------------------------------------------------------------------

void applySlideFromLeft(const uint8_t *a, const uint8_t *b, uint8_t *out, int w, int h, double p) {
    const double shift = (1.0 - p) * w;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = pixIndex(w, x, y);
            if (x + 0.5 < p * w) {
                const int col = static_cast<int>(std::floor(x + 0.5 + shift));
                copyPixel(sampleAt(b, w, h, col, y), out + i);
            } else {
                copyPixel(a + i, out + i);
            }
        }
    }
}

void applySlideFromRight(const uint8_t *a, const uint8_t *b, uint8_t *out, int w, int h, double p) {
    const double shift = (1.0 - p) * w;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = pixIndex(w, x, y);
            if (x + 0.5 >= shift) {
                const int col = static_cast<int>(std::floor(x + 0.5 - shift));
                copyPixel(sampleAt(b, w, h, col, y), out + i);
            } else {
                copyPixel(a + i, out + i);
            }
        }
    }
}

void applySlideFromTop(const uint8_t *a, const uint8_t *b, uint8_t *out, int w, int h, double p) {
    const double shift = (1.0 - p) * h;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = pixIndex(w, x, y);
            if (y + 0.5 < p * h) {
                const int row = static_cast<int>(std::floor(y + 0.5 + shift));
                copyPixel(sampleAt(b, w, h, x, row), out + i);
            } else {
                copyPixel(a + i, out + i);
            }
        }
    }
}

void applySlideFromBottom(const uint8_t *a, const uint8_t *b, uint8_t *out, int w, int h,
                          double p) {
    const double shift = (1.0 - p) * h;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = pixIndex(w, x, y);
            if (y + 0.5 >= shift) {
                const int row = static_cast<int>(std::floor(y + 0.5 - shift));
                copyPixel(sampleAt(b, w, h, x, row), out + i);
            } else {
                copyPixel(a + i, out + i);
            }
        }
    }
}

void applyPushFromLeft(const uint8_t *a, const uint8_t *b, uint8_t *out, int w, int h, double p) {
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = pixIndex(w, x, y);
            if (x + 0.5 < p * w) {
                const int col = static_cast<int>(std::floor(x + 0.5 + (1.0 - p) * w));
                copyPixel(sampleAt(b, w, h, col, y), out + i);
            } else {
                const int col = static_cast<int>(std::floor(x + 0.5 - p * w));
                copyPixel(sampleAt(a, w, h, col, y), out + i);
            }
        }
    }
}

void applyPushFromRight(const uint8_t *a, const uint8_t *b, uint8_t *out, int w, int h, double p) {
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = pixIndex(w, x, y);
            if (x + 0.5 >= (1.0 - p) * w) {
                const int col = static_cast<int>(std::floor(x + 0.5 - (1.0 - p) * w));
                copyPixel(sampleAt(b, w, h, col, y), out + i);
            } else {
                const int col = static_cast<int>(std::floor(x + 0.5 + p * w));
                copyPixel(sampleAt(a, w, h, col, y), out + i);
            }
        }
    }
}

void applyPushFromTop(const uint8_t *a, const uint8_t *b, uint8_t *out, int w, int h, double p) {
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = pixIndex(w, x, y);
            if (y + 0.5 < p * h) {
                const int row = static_cast<int>(std::floor(y + 0.5 + (1.0 - p) * h));
                copyPixel(sampleAt(b, w, h, x, row), out + i);
            } else {
                const int row = static_cast<int>(std::floor(y + 0.5 - p * h));
                copyPixel(sampleAt(a, w, h, x, row), out + i);
            }
        }
    }
}

void applyPushFromBottom(const uint8_t *a, const uint8_t *b, uint8_t *out, int w, int h, double p) {
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = pixIndex(w, x, y);
            if (y + 0.5 >= (1.0 - p) * h) {
                const int row = static_cast<int>(std::floor(y + 0.5 - (1.0 - p) * h));
                copyPixel(sampleAt(b, w, h, x, row), out + i);
            } else {
                const int row = static_cast<int>(std::floor(y + 0.5 + p * h));
                copyPixel(sampleAt(a, w, h, x, row), out + i);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Zoom: nearest-neighbor rescale around the frame center.
// ---------------------------------------------------------------------------

// Reads the pixel of `src` at continuous center coordinate (u, v) under
// scale factor s (magnification: s > 1 zooms IN). Out-of-bounds reads are
// either edge-clamped (letterbox = false) or opaque black (letterbox).
const uint8_t *zoomSample(const uint8_t *src, int w, int h, int x, int y, double s,
                          bool letterbox) {
    const double u = w * 0.5 + (x + 0.5 - w * 0.5) / s;
    const double v = h * 0.5 + (y + 0.5 - h * 0.5) / s;
    const int col = static_cast<int>(std::floor(u));
    const int row = static_cast<int>(std::floor(v));
    if (col < 0 || col > w - 1 || row < 0 || row > h - 1) {
        return letterbox ? nullptr : sampleAt(src, w, h, col, row);
    }
    return src + pixIndex(w, col, row);
}

void applyZoomIn(const uint8_t *a, const uint8_t *b, uint8_t *out, int w, int h, double p) {
    const double s = 1.0 + 3.0 * p; // A magnifies as the cut proceeds
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = pixIndex(w, x, y);
            const uint8_t *as = zoomSample(a, w, h, x, y, s, false);
            blendPixel(as, b + i, out + i, p);
        }
    }
}

void applyZoomOut(const uint8_t *a, const uint8_t *b, uint8_t *out, int w, int h, double p) {
    const double s = 1.0 / (1.0 + 3.0 * p); // A shrinks into a black letterbox
    static const uint8_t kBlack[4] = {0, 0, 0, 255};
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = pixIndex(w, x, y);
            const uint8_t *as = zoomSample(a, w, h, x, y, s, true);
            blendPixel(as ? as : kBlack, b + i, out + i, p);
        }
    }
}

void applyZoomThrough(const uint8_t *a, const uint8_t *b, uint8_t *out, int w, int h, double p) {
    // A magnifies toward the viewer while B starts magnified and settles.
    const double sa = 1.0 + 3.0 * p;
    const double sb = 1.0 + 3.0 * (1.0 - p);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = pixIndex(w, x, y);
            const uint8_t *as = zoomSample(a, w, h, x, y, sa, false);
            const uint8_t *bs = zoomSample(b, w, h, x, y, sb, false);
            blendPixel(as, bs, out + i, p);
        }
    }
}

// ---------------------------------------------------------------------------
// Dispatch.
// ---------------------------------------------------------------------------

void applyTransition(const uint8_t *a, const uint8_t *b, uint8_t *out, int width, int height,
                     const std::string &kind, double progress) {
    if (!a || !b || !out || width <= 0 || height <= 0) {
        return;
    }
    double p = progress < 0.0 ? 0.0 : (progress > 1.0 ? 1.0 : progress);
    const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    if (p <= 0.0) {
        std::memcpy(out, a, n);
        return;
    }
    if (p >= 1.0) {
        std::memcpy(out, b, n);
        return;
    }

    if (kind == "dissolve.cross") {
        applyCross(a, b, out, width, height, p);
    } else if (kind == "dissolve.dip-black") {
        applyDip(a, b, out, width, height, p, 0, 0, 0);
    } else if (kind == "dissolve.dip-white") {
        applyDip(a, b, out, width, height, p, 255, 255, 255);
    } else if (kind == "dissolve.additive") {
        applyAdditive(a, b, out, width, height, p);
    } else if (kind == "dissolve.film") {
        applyFilm(a, b, out, width, height, p);
    } else if (kind == "dissolve.blur") {
        applyBlurDissolve(a, b, out, width, height, p);
    } else if (kind == "wipe.left") {
        applyWipe(a, b, out, width, height, p, wipeLeft);
    } else if (kind == "wipe.right") {
        applyWipe(a, b, out, width, height, p, wipeRight);
    } else if (kind == "wipe.up") {
        applyWipe(a, b, out, width, height, p, wipeUp);
    } else if (kind == "wipe.down") {
        applyWipe(a, b, out, width, height, p, wipeDown);
    } else if (kind == "wipe.corner-tl") {
        applyWipe(a, b, out, width, height, p, wipeCornerTL);
    } else if (kind == "wipe.corner-tr") {
        applyWipe(a, b, out, width, height, p, wipeCornerTR);
    } else if (kind == "wipe.corner-bl") {
        applyWipe(a, b, out, width, height, p, wipeCornerBL);
    } else if (kind == "wipe.corner-br") {
        applyWipe(a, b, out, width, height, p, wipeCornerBR);
    } else if (kind == "wipe.iris-box") {
        applyWipe(a, b, out, width, height, p, wipeIrisBox);
    } else if (kind == "wipe.iris-box-out") {
        applyWipe(a, b, out, width, height, p, wipeIrisBoxOut);
    } else if (kind == "wipe.circle") {
        applyWipe(a, b, out, width, height, p, wipeCircle);
    } else if (kind == "wipe.diamond") {
        applyWipe(a, b, out, width, height, p, wipeDiamond);
    } else if (kind == "wipe.clock") {
        applyWipe(a, b, out, width, height, p, wipeClock);
    } else if (kind == "wipe.clock-ccw") {
        applyWipe(a, b, out, width, height, p, wipeClockCcw);
    } else if (kind == "wipe.blinds-h") {
        applyWipe(a, b, out, width, height, p, wipeBlindsH);
    } else if (kind == "wipe.blinds-v") {
        applyWipe(a, b, out, width, height, p, wipeBlindsV);
    } else if (kind == "wipe.checker") {
        applyWipe(a, b, out, width, height, p, wipeChecker);
    } else if (kind == "wipe.barn-h") {
        applyWipe(a, b, out, width, height, p, wipeBarnH);
    } else if (kind == "wipe.barn-v") {
        applyWipe(a, b, out, width, height, p, wipeBarnV);
    } else if (kind == "slide.from-left") {
        applySlideFromLeft(a, b, out, width, height, p);
    } else if (kind == "slide.from-right") {
        applySlideFromRight(a, b, out, width, height, p);
    } else if (kind == "slide.from-top") {
        applySlideFromTop(a, b, out, width, height, p);
    } else if (kind == "slide.from-bottom") {
        applySlideFromBottom(a, b, out, width, height, p);
    } else if (kind == "push.from-left") {
        applyPushFromLeft(a, b, out, width, height, p);
    } else if (kind == "push.from-right") {
        applyPushFromRight(a, b, out, width, height, p);
    } else if (kind == "push.from-top") {
        applyPushFromTop(a, b, out, width, height, p);
    } else if (kind == "push.from-bottom") {
        applyPushFromBottom(a, b, out, width, height, p);
    } else if (kind == "zoom.in") {
        applyZoomIn(a, b, out, width, height, p);
    } else if (kind == "zoom.out") {
        applyZoomOut(a, b, out, width, height, p);
    } else if (kind == "zoom.through") {
        applyZoomThrough(a, b, out, width, height, p);
    } else {
        // Unknown kind (e.g. a project saved by a newer catalog): degrade
        // to a hard cut - the outgoing frame passes through.
        std::memcpy(out, a, n);
    }
}

} // namespace fc
