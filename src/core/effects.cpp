#include "effects.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fc {

// ---------------------------------------------------------------------------
// Shared pixel helpers. All math is integer or explicitly rounded double -
// deterministic across platforms and toolchains.
// ---------------------------------------------------------------------------

namespace {

const double kPi = 3.14159265358979323846;

// One overload for everything: int/long/double arguments all convert to
// double, and every value in [0, 255] (and the small sums around it) is
// exactly representable - no ambiguity, no precision loss.
inline uint8_t clampByte(double v) {
    return static_cast<uint8_t>(v < 0.0 ? 0.0 : (v > 255.0 ? 255.0 : v));
}

// Rec.601 integer luma (matches the 299/587/114 weights exactly).
inline int lumaOf(int r, int g, int b) {
    return (r * 299 + g * 587 + b * 114 + 500) / 1000;
}

inline uint8_t mixByte(int a, int b, double t) {
    return clampByte(std::lround(a + (b - a) * t));
}

// Fixed integer hash for the deterministic film grain.
inline uint32_t grainHash(uint32_t x, uint32_t y, uint32_t s) {
    uint32_t h = x * 0x9E3779B1u + y * 0x85EBCA77u + s * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x2545F491u;
    h ^= h >> 13;
    return h;
}

inline int grainNoise(uint32_t x, uint32_t y, uint32_t s) {
    return static_cast<int>(grainHash(x, y, s) % 511u) - 255; // -255..255
}

struct Px {
    uint8_t *r, *g, *b, *a;
};

inline Px pxAt(uint8_t *rgba, int w, int x, int y) {
    uint8_t *p =
        rgba + (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
    return Px{p, p + 1, p + 2, p + 3};
}

inline int clampIdx(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// ---------------------------------------------------------------------------
// Per-effect processors. Each takes the instance (parameter values), the
// in-place RGBA buffer, and its size. Every processor preserves alpha.
// ---------------------------------------------------------------------------

void applyBrightness(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const int add = static_cast<int>(std::lround(fx.param("amount") * 255.0));
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    for (size_t i = 0; i < n; i += 4) {
        rgba[i] = clampByte(rgba[i] + add);
        rgba[i + 1] = clampByte(rgba[i + 1] + add);
        rgba[i + 2] = clampByte(rgba[i + 2] + add);
    }
}

void applyContrast(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const double k = 1.0 + fx.param("amount");
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    for (size_t i = 0; i < n; i += 4) {
        rgba[i] = clampByte(128.0 + (rgba[i] - 128.0) * k);
        rgba[i + 1] = clampByte(128.0 + (rgba[i + 1] - 128.0) * k);
        rgba[i + 2] = clampByte(128.0 + (rgba[i + 2] - 128.0) * k);
    }
}

void applySaturation(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const double k = fx.param("amount");
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    for (size_t i = 0; i < n; i += 4) {
        const int l = lumaOf(rgba[i], rgba[i + 1], rgba[i + 2]);
        rgba[i] = clampByte(std::lround(l + (rgba[i] - l) * k));
        rgba[i + 1] = clampByte(std::lround(l + (rgba[i + 1] - l) * k));
        rgba[i + 2] = clampByte(std::lround(l + (rgba[i + 2] - l) * k));
    }
}

void applyVibrance(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const double amount = fx.param("amount");
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    for (size_t i = 0; i < n; i += 4) {
        const int mx = std::max({rgba[i], rgba[i + 1], rgba[i + 2]});
        const int mn = std::min({rgba[i], rgba[i + 1], rgba[i + 2]});
        // Adaptive: already-saturated pixels get less boost.
        const double k = 1.0 + amount * (1.0 - static_cast<double>(mx - mn) / 255.0);
        const int l = lumaOf(rgba[i], rgba[i + 1], rgba[i + 2]);
        rgba[i] = clampByte(std::lround(l + (rgba[i] - l) * k));
        rgba[i + 1] = clampByte(std::lround(l + (rgba[i + 1] - l) * k));
        rgba[i + 2] = clampByte(std::lround(l + (rgba[i + 2] - l) * k));
    }
}

void applyHue(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const double deg = fx.param("angle");
    const double cosv = std::cos(deg * kPi / 180.0);
    const double sinv = std::sin(deg * kPi / 180.0);
    // SVG feColorMatrix hueRotate matrix.
    const double m[3][3] = {
        {0.213 + cosv * 0.787 - sinv * 0.213, 0.715 - cosv * 0.715 - sinv * 0.715,
         0.072 - cosv * 0.072 + sinv * 0.928},
        {0.213 - cosv * 0.213 + sinv * 0.143, 0.715 + cosv * 0.285 + sinv * 0.140,
         0.072 - cosv * 0.072 - sinv * 0.283},
        {0.213 - cosv * 0.213 - sinv * 0.787, 0.715 - cosv * 0.715 + sinv * 0.715,
         0.072 + cosv * 0.928 + sinv * 0.072}};
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    for (size_t i = 0; i < n; i += 4) {
        const int r = rgba[i];
        const int g = rgba[i + 1];
        const int b = rgba[i + 2];
        rgba[i] = clampByte(std::lround(m[0][0] * r + m[0][1] * g + m[0][2] * b));
        rgba[i + 1] = clampByte(std::lround(m[1][0] * r + m[1][1] * g + m[1][2] * b));
        rgba[i + 2] = clampByte(std::lround(m[2][0] * r + m[2][1] * g + m[2][2] * b));
    }
}

void applyTemperature(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const int d = static_cast<int>(std::lround(fx.param("amount") * 40.0));
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    for (size_t i = 0; i < n; i += 4) {
        rgba[i] = clampByte(rgba[i] + d);         // red: warm up
        rgba[i + 2] = clampByte(rgba[i + 2] - d); // blue: cool down
    }
}

void applyTint(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const int d = static_cast<int>(std::lround(fx.param("amount") * 40.0));
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    for (size_t i = 0; i < n; i += 4) {
        rgba[i] = clampByte(rgba[i] + d / 2);
        rgba[i + 1] = clampByte(rgba[i + 1] - d);
        rgba[i + 2] = clampByte(rgba[i + 2] + d / 2);
    }
}

void applyExposure(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const double k = std::pow(2.0, fx.param("stops"));
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    for (size_t i = 0; i < n; i += 4) {
        rgba[i] = clampByte(std::lround(rgba[i] * k));
        rgba[i + 1] = clampByte(std::lround(rgba[i + 1] * k));
        rgba[i + 2] = clampByte(std::lround(rgba[i + 2] * k));
    }
}

void applyGamma(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const double g = fx.param("gamma");
    const double inv = g > 0.0 ? 1.0 / g : 1.0;
    uint8_t lut[256];
    for (int v = 0; v < 256; ++v) {
        lut[v] = clampByte(std::lround(255.0 * std::pow(v / 255.0, inv)));
    }
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    for (size_t i = 0; i < n; i += 4) {
        rgba[i] = lut[rgba[i]];
        rgba[i + 1] = lut[rgba[i + 1]];
        rgba[i + 2] = lut[rgba[i + 2]];
    }
}

void applyLevels(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const double inBlack = fx.param("inBlack");
    const double inWhite = fx.param("inWhite");
    const double outBlack = fx.param("outBlack");
    const double outWhite = fx.param("outWhite");
    const double denom = inWhite - inBlack;
    const double scale = denom > 0.0 ? (outWhite - outBlack) / denom : 0.0;
    uint8_t lut[256];
    for (int v = 0; v < 256; ++v) {
        lut[v] = clampByte(std::lround(outBlack + (v - inBlack) * scale));
    }
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    for (size_t i = 0; i < n; i += 4) {
        rgba[i] = lut[rgba[i]];
        rgba[i + 1] = lut[rgba[i + 1]];
        rgba[i + 2] = lut[rgba[i + 2]];
    }
}

void applyPosterize(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const int levels = std::max(2, static_cast<int>(std::lround(fx.param("levels"))));
    const double step = 255.0 / (levels - 1);
    uint8_t lut[256];
    for (int v = 0; v < 256; ++v) {
        lut[v] = clampByte(std::lround(std::lround(v / step) * step));
    }
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    for (size_t i = 0; i < n; i += 4) {
        rgba[i] = lut[rgba[i]];
        rgba[i + 1] = lut[rgba[i + 1]];
        rgba[i + 2] = lut[rgba[i + 2]];
    }
}

void applyThreshold(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const int cut = static_cast<int>(std::lround(fx.param("cutoff") * 255.0));
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    for (size_t i = 0; i < n; i += 4) {
        const uint8_t v = lumaOf(rgba[i], rgba[i + 1], rgba[i + 2]) >= cut ? 255 : 0;
        rgba[i] = v;
        rgba[i + 1] = v;
        rgba[i + 2] = v;
    }
}

void applySolarize(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const int cut = static_cast<int>(std::lround(fx.param("threshold") * 255.0));
    uint8_t lut[256];
    for (int v = 0; v < 256; ++v) {
        lut[v] = v > cut ? static_cast<uint8_t>(255 - v) : static_cast<uint8_t>(v);
    }
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    for (size_t i = 0; i < n; i += 4) {
        rgba[i] = lut[rgba[i]];
        rgba[i + 1] = lut[rgba[i + 1]];
        rgba[i + 2] = lut[rgba[i + 2]];
    }
}

void applyInvert(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const double amount = fx.param("amount");
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    for (size_t i = 0; i < n; i += 4) {
        rgba[i] = mixByte(rgba[i], 255 - rgba[i], amount);
        rgba[i + 1] = mixByte(rgba[i + 1], 255 - rgba[i + 1], amount);
        rgba[i + 2] = mixByte(rgba[i + 2], 255 - rgba[i + 2], amount);
    }
}

void applyGrayscale(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const double amount = fx.param("amount");
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    for (size_t i = 0; i < n; i += 4) {
        const int l = lumaOf(rgba[i], rgba[i + 1], rgba[i + 2]);
        rgba[i] = mixByte(rgba[i], l, amount);
        rgba[i + 1] = mixByte(rgba[i + 1], l, amount);
        rgba[i + 2] = mixByte(rgba[i + 2], l, amount);
    }
}

void applySepia(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const double amount = fx.param("amount");
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    for (size_t i = 0; i < n; i += 4) {
        const int r = rgba[i];
        const int g = rgba[i + 1];
        const int b = rgba[i + 2];
        const int sr = static_cast<int>(std::lround(0.393 * r + 0.769 * g + 0.189 * b));
        const int sg = static_cast<int>(std::lround(0.349 * r + 0.686 * g + 0.168 * b));
        const int sb = static_cast<int>(std::lround(0.272 * r + 0.534 * g + 0.131 * b));
        rgba[i] = mixByte(r, sr, amount);
        rgba[i + 1] = mixByte(g, sg, amount);
        rgba[i + 2] = mixByte(b, sb, amount);
    }
}

void applyVignette(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const double amount = fx.param("amount");
    const double radius = fx.param("radius");
    const double cx = w * 0.5;
    const double cy = h * 0.5;
    const double maxD = std::sqrt(cx * cx + cy * cy);
    const double inner = maxD * radius;
    const double outer = maxD;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const double dx = x - cx;
            const double dy = y - cy;
            const double d = std::sqrt(dx * dx + dy * dy);
            double t = 0.0;
            if (d > inner) {
                t = (d - inner) / (outer - inner);
                t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
                t *= t; // smooth quadratic falloff
            }
            const double f = 1.0 - amount * t;
            Px p = pxAt(rgba, w, x, y);
            *p.r = clampByte(std::lround(*p.r * f));
            *p.g = clampByte(std::lround(*p.g * f));
            *p.b = clampByte(std::lround(*p.b * f));
        }
    }
}

void applyGrain(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const double amount = fx.param("amount");
    const uint32_t seed = static_cast<uint32_t>(std::lround(fx.param("seed") * 1000.0));
    const bool mono = fx.paramBool("monochrome");
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            Px p = pxAt(rgba, w, x, y);
            const int n = grainNoise(static_cast<uint32_t>(x), static_cast<uint32_t>(y), seed);
            const int delta = static_cast<int>(std::lround(n * amount / 4.0));
            if (mono) {
                *p.r = clampByte(*p.r + delta);
                *p.g = clampByte(*p.g + delta);
                *p.b = clampByte(*p.b + delta);
            } else {
                const int nr =
                    grainNoise(static_cast<uint32_t>(x) * 3u + 0u, static_cast<uint32_t>(y), seed);
                const int ng =
                    grainNoise(static_cast<uint32_t>(x) * 3u + 1u, static_cast<uint32_t>(y), seed);
                const int nb =
                    grainNoise(static_cast<uint32_t>(x) * 3u + 2u, static_cast<uint32_t>(y), seed);
                *p.r = clampByte(*p.r + static_cast<int>(std::lround(nr * amount / 4.0)));
                *p.g = clampByte(*p.g + static_cast<int>(std::lround(ng * amount / 4.0)));
                *p.b = clampByte(*p.b + static_cast<int>(std::lround(nb * amount / 4.0)));
            }
        }
    }
}

void applyPixelate(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    int block = static_cast<int>(std::lround(fx.param("block")));
    block = std::max(1, block);
    if (block <= 1) {
        return;
    }
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    std::vector<uint8_t> src(rgba, rgba + n);
    for (int by = 0; by < h; by += block) {
        for (int bx = 0; bx < w; bx += block) {
            const int x1 = std::min(bx + block, w);
            const int y1 = std::min(by + block, h);
            long sum[3] = {0, 0, 0};
            const int count = (x1 - bx) * (y1 - by);
            for (int y = by; y < y1; ++y) {
                for (int x = bx; x < x1; ++x) {
                    const size_t i = (static_cast<size_t>(y) * w + x) * 4;
                    sum[0] += src[i];
                    sum[1] += src[i + 1];
                    sum[2] += src[i + 2];
                }
            }
            const uint8_t avg[3] = {clampByte(std::lround(double(sum[0]) / count)),
                                    clampByte(std::lround(double(sum[1]) / count)),
                                    clampByte(std::lround(double(sum[2]) / count))};
            for (int y = by; y < y1; ++y) {
                for (int x = bx; x < x1; ++x) {
                    Px p = pxAt(rgba, w, x, y);
                    *p.r = avg[0];
                    *p.g = avg[1];
                    *p.b = avg[2];
                }
            }
        }
    }
}

void applyChromatic(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const int shift = static_cast<int>(std::lround(fx.param("shift")));
    if (shift == 0) {
        return;
    }
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    std::vector<uint8_t> src(rgba, rgba + n);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int xr = clampIdx(x + shift, 0, w - 1);
            const int xb = clampIdx(x - shift, 0, w - 1);
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            const size_t ir = (static_cast<size_t>(y) * w + xr) * 4;
            const size_t ib = (static_cast<size_t>(y) * w + xb) * 4;
            rgba[i] = src[ir];         // red shifted right
            rgba[i + 2] = src[ib + 2]; // blue shifted left
        }
    }
}

// Separable box blur over RGB (alpha untouched). Two passes with edge
// clamping; uniform images are exact identities.
void boxBlurRGBA(uint8_t *rgba, int w, int h, int radius) {
    const int r = std::max(1, radius);
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    std::vector<uint8_t> src(rgba, rgba + n);
    const int window = 2 * r + 1;
    // Horizontal: src -> rgba.
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int sum[3] = {0, 0, 0};
            for (int k = -r; k <= r; ++k) {
                const size_t i = (static_cast<size_t>(y) * w + clampIdx(x + k, 0, w - 1)) * 4;
                sum[0] += src[i];
                sum[1] += src[i + 1];
                sum[2] += src[i + 2];
            }
            Px p = pxAt(rgba, w, x, y);
            *p.r = clampByte(std::lround(double(sum[0]) / window));
            *p.g = clampByte(std::lround(double(sum[1]) / window));
            *p.b = clampByte(std::lround(double(sum[2]) / window));
        }
    }
    // Vertical: rgba -> src, then copy back.
    std::memcpy(src.data(), rgba, n);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int sum[3] = {0, 0, 0};
            for (int k = -r; k <= r; ++k) {
                const size_t i = (static_cast<size_t>(clampIdx(y + k, 0, h - 1)) * w + x) * 4;
                sum[0] += src[i];
                sum[1] += src[i + 1];
                sum[2] += src[i + 2];
            }
            Px p = pxAt(rgba, w, x, y);
            *p.r = clampByte(std::lround(double(sum[0]) / window));
            *p.g = clampByte(std::lround(double(sum[1]) / window));
            *p.b = clampByte(std::lround(double(sum[2]) / window));
        }
    }
}

void applyBoxBlur(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    boxBlurRGBA(rgba, w, h, std::max(1, static_cast<int>(std::lround(fx.param("radius")))));
}

void applyGaussianBlur(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const int r = std::max(1, static_cast<int>(std::lround(fx.param("radius"))));
    const double sigma = r * 0.5;
    const double denom = 2.0 * sigma * sigma;
    std::vector<double> kernel(static_cast<size_t>(2 * r + 1));
    double sum = 0.0;
    for (int k = -r; k <= r; ++k) {
        const double v = std::exp(-(static_cast<double>(k) * k) / denom);
        kernel[static_cast<size_t>(k + r)] = v;
        sum += v;
    }
    for (double &v : kernel) {
        v /= sum;
    }
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    std::vector<uint8_t> src(rgba, rgba + n);
    // Horizontal: src -> rgba.
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double acc[3] = {0.0, 0.0, 0.0};
            for (int k = -r; k <= r; ++k) {
                const size_t i = (static_cast<size_t>(y) * w + clampIdx(x + k, 0, w - 1)) * 4;
                const double kv = kernel[static_cast<size_t>(k + r)];
                acc[0] += src[i] * kv;
                acc[1] += src[i + 1] * kv;
                acc[2] += src[i + 2] * kv;
            }
            Px p = pxAt(rgba, w, x, y);
            *p.r = clampByte(std::lround(acc[0]));
            *p.g = clampByte(std::lround(acc[1]));
            *p.b = clampByte(std::lround(acc[2]));
        }
    }
    // Vertical: rgba -> src, then copy back.
    std::memcpy(src.data(), rgba, n);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double acc[3] = {0.0, 0.0, 0.0};
            for (int k = -r; k <= r; ++k) {
                const size_t i = (static_cast<size_t>(clampIdx(y + k, 0, h - 1)) * w + x) * 4;
                const double kv = kernel[static_cast<size_t>(k + r)];
                acc[0] += src[i] * kv;
                acc[1] += src[i + 1] * kv;
                acc[2] += src[i + 2] * kv;
            }
            Px p = pxAt(rgba, w, x, y);
            *p.r = clampByte(std::lround(acc[0]));
            *p.g = clampByte(std::lround(acc[1]));
            *p.b = clampByte(std::lround(acc[2]));
        }
    }
}

void applySharpen(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const double amount = fx.param("amount");
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    std::vector<uint8_t> blurred(rgba, rgba + n);
    boxBlurRGBA(blurred.data(), w, h, 1);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            for (int c = 0; c < 3; ++c) {
                const int v = rgba[i + c];
                const int b = blurred[i + c];
                rgba[i + c] = clampByte(std::lround(v + amount * (v - b)));
            }
        }
    }
}

void applyEdges(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const double amount = fx.param("amount");
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    std::vector<uint8_t> src(rgba, rgba + n);
    auto lumaAt = [&](int x, int y) {
        const size_t i = (static_cast<size_t>(clampIdx(y, 0, h - 1)) * w +
                          static_cast<size_t>(clampIdx(x, 0, w - 1))) *
                         4;
        return lumaOf(src[i], src[i + 1], src[i + 2]);
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            // Sobel on luma.
            const int gx = -lumaAt(x - 1, y - 1) + lumaAt(x + 1, y - 1)  //
                           - 2 * lumaAt(x - 1, y) + 2 * lumaAt(x + 1, y) //
                           - lumaAt(x - 1, y + 1) + lumaAt(x + 1, y + 1);
            const int gy = -lumaAt(x - 1, y - 1) - 2 * lumaAt(x, y - 1)  //
                           - lumaAt(x + 1, y - 1) + lumaAt(x - 1, y + 1) //
                           + 2 * lumaAt(x, y + 1) + lumaAt(x + 1, y + 1);
            const int mag =
                clampByte(std::lround(std::sqrt(static_cast<double>(gx * gx + gy * gy))));
            Px p = pxAt(rgba, w, x, y);
            *p.r = mixByte(*p.r, mag, amount);
            *p.g = mixByte(*p.g, mag, amount);
            *p.b = mixByte(*p.b, mag, amount);
        }
    }
}

void applyEmboss(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const double strength = fx.param("strength");
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    std::vector<uint8_t> src(rgba, rgba + n);
    auto lumaAt = [&](int x, int y) {
        const size_t i = (static_cast<size_t>(clampIdx(y, 0, h - 1)) * w +
                          static_cast<size_t>(clampIdx(x, 0, w - 1))) *
                         4;
        return lumaOf(src[i], src[i + 1], src[i + 2]);
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int d = lumaAt(x, y) - lumaAt(x - 1, y - 1);
            const uint8_t v = clampByte(128 + std::lround(d * strength));
            Px p = pxAt(rgba, w, x, y);
            *p.r = v;
            *p.g = v;
            *p.b = v;
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Catalog
// ---------------------------------------------------------------------------

const std::vector<EffectDescriptor> &effectCatalog() {
    static const std::vector<EffectDescriptor> catalog = {
        // ---- Color ----
        {"color.brightness",
         "Brightness",
         "Color",
         {{"amount", "Amount", EffectParamType::Number, -1.0, 1.0, 0.0}}},
        {"color.contrast",
         "Contrast",
         "Color",
         {{"amount", "Amount", EffectParamType::Number, -1.0, 1.0, 0.0}}},
        {"color.saturation",
         "Saturation",
         "Color",
         {{"amount", "Amount", EffectParamType::Number, 0.0, 2.0, 1.0}}},
        {"color.vibrance",
         "Vibrance",
         "Color",
         {{"amount", "Amount", EffectParamType::Number, 0.0, 1.0, 0.0}}},
        {"color.hue",
         "Hue",
         "Color",
         {{"angle", "Angle", EffectParamType::Number, -180.0, 180.0, 0.0}}},
        {"color.temperature",
         "Color Temperature",
         "Color",
         {{"amount", "Amount", EffectParamType::Number, -1.0, 1.0, 0.0}}},
        {"color.tint",
         "Tint",
         "Color",
         {{"amount", "Amount", EffectParamType::Number, -1.0, 1.0, 0.0}}},
        {"color.exposure",
         "Exposure",
         "Color",
         {{"stops", "Stops", EffectParamType::Number, -2.0, 2.0, 0.0}}},
        {"color.gamma",
         "Gamma",
         "Color",
         {{"gamma", "Gamma", EffectParamType::Number, 0.2, 3.0, 1.0}}},
        // ---- Tone ----
        {"tone.levels",
         "Levels",
         "Tone",
         {{"inBlack", "Input Black", EffectParamType::Number, 0.0, 255.0, 0.0},
          {"inWhite", "Input White", EffectParamType::Number, 0.0, 255.0, 255.0},
          {"outBlack", "Output Black", EffectParamType::Number, 0.0, 255.0, 0.0},
          {"outWhite", "Output White", EffectParamType::Number, 0.0, 255.0, 255.0}}},
        {"tone.posterize",
         "Posterize",
         "Tone",
         {{"levels", "Levels", EffectParamType::Number, 2.0, 16.0, 8.0}}},
        {"tone.threshold",
         "Threshold",
         "Tone",
         {{"cutoff", "Cutoff", EffectParamType::Number, 0.0, 1.0, 0.5}}},
        {"tone.solarize",
         "Solarize",
         "Tone",
         {{"threshold", "Threshold", EffectParamType::Number, 0.0, 1.0, 0.5}}},
        {"tone.invert",
         "Invert",
         "Tone",
         {{"amount", "Amount", EffectParamType::Number, 0.0, 1.0, 1.0}}},
        // ---- Filter ----
        {"filter.grayscale",
         "Black & White",
         "Filter",
         {{"amount", "Amount", EffectParamType::Number, 0.0, 1.0, 1.0}}},
        {"filter.sepia",
         "Sepia",
         "Filter",
         {{"amount", "Amount", EffectParamType::Number, 0.0, 1.0, 1.0}}},
        {"filter.vignette",
         "Vignette",
         "Filter",
         {{"amount", "Amount", EffectParamType::Number, 0.0, 1.0, 0.5},
          {"radius", "Radius", EffectParamType::Number, 0.3, 1.0, 0.75}}},
        {"filter.grain",
         "Film Grain",
         "Filter",
         {{"amount", "Amount", EffectParamType::Number, 0.0, 1.0, 0.2},
          {"seed", "Seed", EffectParamType::Number, 0.0, 1.0, 0.5},
          {"monochrome", "Monochrome", EffectParamType::Boolean, 0.0, 1.0, 1.0}}},
        {"filter.pixelate",
         "Pixelate",
         "Filter",
         {{"block", "Block Size", EffectParamType::Number, 2.0, 64.0, 8.0}}},
        {"filter.chromatic",
         "Chromatic Aberration",
         "Filter",
         {{"shift", "Shift", EffectParamType::Number, 0.0, 12.0, 2.0}}},
        // ---- Blur & Sharpen ----
        {"blur.box",
         "Box Blur",
         "Blur & Sharpen",
         {{"radius", "Radius", EffectParamType::Number, 1.0, 16.0, 2.0}}},
        {"blur.gaussian",
         "Gaussian Blur",
         "Blur & Sharpen",
         {{"radius", "Radius", EffectParamType::Number, 1.0, 16.0, 3.0}}},
        {"stylize.sharpen",
         "Sharpen",
         "Blur & Sharpen",
         {{"amount", "Amount", EffectParamType::Number, 0.0, 2.0, 0.5}}},
        // ---- Stylize ----
        {"stylize.edges",
         "Find Edges",
         "Stylize",
         {{"amount", "Amount", EffectParamType::Number, 0.0, 1.0, 1.0}}},
        {"stylize.emboss",
         "Emboss",
         "Stylize",
         {{"strength", "Strength", EffectParamType::Number, 0.0, 2.0, 1.0}}},
    };
    return catalog;
}

const EffectDescriptor *findEffect(const std::string &id) {
    for (const EffectDescriptor &d : effectCatalog()) {
        if (d.id == id) {
            return &d;
        }
    }
    return nullptr;
}

EffectInstance makeEffectInstance(const std::string &id) {
    EffectInstance fx;
    const EffectDescriptor *d = findEffect(id);
    if (!d) {
        return fx; // empty effectId: never processed
    }
    fx.effectId = id;
    fx.enabled = true;
    fx.values.reserve(d->params.size());
    for (const EffectParamDescriptor &p : d->params) {
        fx.values.push_back(p.defaultValue);
    }
    return fx;
}

const EffectDescriptor *EffectInstance::descriptor() const {
    return effectId.empty() ? nullptr : findEffect(effectId);
}

double EffectInstance::param(const std::string &key, double fallback) const {
    const EffectDescriptor *d = descriptor();
    if (!d) {
        return fallback;
    }
    for (size_t i = 0; i < d->params.size(); ++i) {
        if (d->params[i].key == key) {
            return i < values.size() ? values[i] : d->params[i].defaultValue;
        }
    }
    return fallback;
}

void EffectInstance::setParam(const std::string &key, double value) {
    const EffectDescriptor *d = descriptor();
    if (!d) {
        return;
    }
    if (values.size() < d->params.size()) {
        const size_t old = values.size();
        values.resize(d->params.size());
        for (size_t i = old; i < values.size(); ++i) {
            values[i] = d->params[i].defaultValue;
        }
    }
    for (size_t i = 0; i < d->params.size(); ++i) {
        if (d->params[i].key == key) {
            values[i] = std::min(std::max(value, d->params[i].minValue), d->params[i].maxValue);
            return;
        }
    }
}

bool EffectInstance::paramBool(const std::string &key) const {
    return param(key, 0.0) >= 0.5;
}

void applyEffectStack(uint8_t *rgba, int width, int height,
                      const std::vector<EffectInstance> &stack) {
    if (!rgba || width <= 0 || height <= 0) {
        return;
    }
    for (const EffectInstance &fx : stack) {
        if (!fx.enabled || fx.effectId.empty()) {
            continue;
        }
        const EffectDescriptor *d = findEffect(fx.effectId);
        if (!d) {
            continue; // unknown id (from a newer catalog): skip, do not crash
        }
        const std::string &id = d->id;
        if (id == "color.brightness") {
            applyBrightness(fx, rgba, width, height);
        } else if (id == "color.contrast") {
            applyContrast(fx, rgba, width, height);
        } else if (id == "color.saturation") {
            applySaturation(fx, rgba, width, height);
        } else if (id == "color.vibrance") {
            applyVibrance(fx, rgba, width, height);
        } else if (id == "color.hue") {
            applyHue(fx, rgba, width, height);
        } else if (id == "color.temperature") {
            applyTemperature(fx, rgba, width, height);
        } else if (id == "color.tint") {
            applyTint(fx, rgba, width, height);
        } else if (id == "color.exposure") {
            applyExposure(fx, rgba, width, height);
        } else if (id == "color.gamma") {
            applyGamma(fx, rgba, width, height);
        } else if (id == "tone.levels") {
            applyLevels(fx, rgba, width, height);
        } else if (id == "tone.posterize") {
            applyPosterize(fx, rgba, width, height);
        } else if (id == "tone.threshold") {
            applyThreshold(fx, rgba, width, height);
        } else if (id == "tone.solarize") {
            applySolarize(fx, rgba, width, height);
        } else if (id == "tone.invert") {
            applyInvert(fx, rgba, width, height);
        } else if (id == "filter.grayscale") {
            applyGrayscale(fx, rgba, width, height);
        } else if (id == "filter.sepia") {
            applySepia(fx, rgba, width, height);
        } else if (id == "filter.vignette") {
            applyVignette(fx, rgba, width, height);
        } else if (id == "filter.grain") {
            applyGrain(fx, rgba, width, height);
        } else if (id == "filter.pixelate") {
            applyPixelate(fx, rgba, width, height);
        } else if (id == "filter.chromatic") {
            applyChromatic(fx, rgba, width, height);
        } else if (id == "blur.box") {
            applyBoxBlur(fx, rgba, width, height);
        } else if (id == "blur.gaussian") {
            applyGaussianBlur(fx, rgba, width, height);
        } else if (id == "stylize.sharpen") {
            applySharpen(fx, rgba, width, height);
        } else if (id == "stylize.edges") {
            applyEdges(fx, rgba, width, height);
        } else if (id == "stylize.emboss") {
            applyEmboss(fx, rgba, width, height);
        }
    }
}

} // namespace fc
