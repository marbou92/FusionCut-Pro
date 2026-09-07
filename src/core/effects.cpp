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

// ---------------------------------------------------------------------------
// M5 Phase 3 additions (27 effects). Same contract: RGBA8888 in place,
// alpha preserved, deterministic math, integer or explicitly rounded
// double everywhere.
// ---------------------------------------------------------------------------

// Integer HSV -> RGB at full saturation/value (hue in degrees). Used by
// the colorize and duotone tints; test points sit far from the sector
// boundaries.
inline void hueToRgb255(double deg, int &r, int &g, int &b) {
    double hdeg = std::fmod(deg, 360.0);
    if (hdeg < 0.0) {
        hdeg += 360.0;
    }
    const double sector = hdeg / 60.0;
    const int i = static_cast<int>(sector) % 6;
    const double f = sector - static_cast<int>(sector);
    const int t = static_cast<int>(std::lround(255.0 * f));
    const int q = static_cast<int>(std::lround(255.0 * (1.0 - f)));
    switch (i) {
    case 0:
        r = 255;
        g = t;
        b = 0;
        break;
    case 1:
        r = q;
        g = 255;
        b = 0;
        break;
    case 2:
        r = 0;
        g = 255;
        b = t;
        break;
    case 3:
        r = 0;
        g = q;
        b = 255;
        break;
    case 4:
        r = t;
        g = 0;
        b = 255;
        break;
    default:
        r = 255;
        g = 0;
        b = q;
        break;
    }
}

// The Color Panel backend: one combined 9-knob grade. Every default is
// neutral, so a default instance is an exact identity. Pipeline order:
// exposure -> shadows/highlights lifts -> temperature/tint -> contrast ->
// hue rotation -> saturation/vibrance.
void applyCorrector(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const double expK = std::pow(2.0, fx.param("exposure"));
    const double contrast = 1.0 + fx.param("contrast");
    const double highlights = fx.param("highlights");
    const double shadows = fx.param("shadows");
    const int tempD = static_cast<int>(std::lround(fx.param("temperature") * 40.0));
    const int tintD = static_cast<int>(std::lround(fx.param("tint") * 40.0));
    const double sat = fx.param("saturation");
    const double vib = fx.param("vibrance");
    const double deg = fx.param("hue");
    const double cosv = std::cos(deg * kPi / 180.0);
    const double sinv = std::sin(deg * kPi / 180.0);
    const double m[3][3] = {
        {0.213 + cosv * 0.787 - sinv * 0.213, 0.715 - cosv * 0.715 - sinv * 0.715,
         0.072 - cosv * 0.072 + sinv * 0.928},
        {0.213 - cosv * 0.213 + sinv * 0.143, 0.715 + cosv * 0.285 + sinv * 0.140,
         0.072 - cosv * 0.072 - sinv * 0.283},
        {0.213 - cosv * 0.213 - sinv * 0.787, 0.715 - cosv * 0.715 + sinv * 0.715,
         0.072 + cosv * 0.928 + sinv * 0.072}};
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    for (size_t i = 0; i < n; i += 4) {
        // Exposure.
        double dr = rgba[i] * expK;
        double dg = rgba[i + 1] * expK;
        double db = rgba[i + 2] * expK;
        // Shadows / highlights (luma-weighted after exposure).
        const int l = lumaOf(clampByte(std::lround(dr)), clampByte(std::lround(dg)),
                             clampByte(std::lround(db)));
        double wS = (128.0 - l) / 128.0;
        wS = wS < 0.0 ? 0.0 : (wS > 1.0 ? 1.0 : wS);
        double wH = (l - 128.0) / 127.0;
        wH = wH < 0.0 ? 0.0 : (wH > 1.0 ? 1.0 : wH);
        dr += shadows * 90.0 * wS + highlights * 90.0 * wH;
        dg += shadows * 90.0 * wS + highlights * 90.0 * wH;
        db += shadows * 90.0 * wS + highlights * 90.0 * wH;
        // Temperature / tint.
        dr += tempD + tintD / 2.0;
        dg += -tintD;
        db += -tempD + tintD / 2.0;
        // Contrast around 128.
        dr = 128.0 + (dr - 128.0) * contrast;
        dg = 128.0 + (dg - 128.0) * contrast;
        db = 128.0 + (db - 128.0) * contrast;
        // Hue rotation.
        const double hr = m[0][0] * dr + m[0][1] * dg + m[0][2] * db;
        const double hg = m[1][0] * dr + m[1][1] * dg + m[1][2] * db;
        const double hb = m[2][0] * dr + m[2][1] * dg + m[2][2] * db;
        dr = hr;
        dg = hg;
        db = hb;
        // Saturation + vibrance (adaptive) around the post-grade luma.
        const int l2 = lumaOf(clampByte(std::lround(dr)), clampByte(std::lround(dg)),
                              clampByte(std::lround(db)));
        const int mx = std::max(
            {clampByte(std::lround(dr)), clampByte(std::lround(dg)), clampByte(std::lround(db))});
        const int mn = std::min(
            {clampByte(std::lround(dr)), clampByte(std::lround(dg)), clampByte(std::lround(db))});
        const double k = sat + vib * (1.0 - static_cast<double>(mx - mn) / 255.0);
        rgba[i] = clampByte(std::lround(l2 + (dr - l2) * k));
        rgba[i + 1] = clampByte(std::lround(l2 + (dg - l2) * k));
        rgba[i + 2] = clampByte(std::lround(l2 + (db - l2) * k));
    }
}

void applyChannelGain(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const double rg = fx.param("rGain");
    const double gg = fx.param("gGain");
    const double bg = fx.param("bGain");
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    for (size_t i = 0; i < n; i += 4) {
        rgba[i] = clampByte(std::lround(rgba[i] * rg));
        rgba[i + 1] = clampByte(std::lround(rgba[i + 1] * gg));
        rgba[i + 2] = clampByte(std::lround(rgba[i + 2] * bg));
    }
}

void applyColorize(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const int amount = static_cast<int>(std::lround(fx.param("amount") * 255.0));
    if (amount == 0) {
        return;
    }
    int tr, tg, tb;
    hueToRgb255(fx.param("hue"), tr, tg, tb);
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    for (size_t i = 0; i < n; i += 4) {
        const int l = lumaOf(rgba[i], rgba[i + 1], rgba[i + 2]);
        // Tinted = the hue's channel scaled by the pixel's luma.
        const int tintR = tr * l / 255;
        const int tintG = tg * l / 255;
        const int tintB = tb * l / 255;
        rgba[i] = static_cast<uint8_t>(rgba[i] + (tintR - rgba[i]) * amount / 255);
        rgba[i + 1] = static_cast<uint8_t>(rgba[i + 1] + (tintG - rgba[i + 1]) * amount / 255);
        rgba[i + 2] = static_cast<uint8_t>(rgba[i + 2] + (tintB - rgba[i + 2]) * amount / 255);
    }
}

void applyDuotone(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const int amount = static_cast<int>(std::lround(fx.param("amount") * 255.0));
    if (amount == 0) {
        return;
    }
    int sr, sg, sb, hr, hg, hb;
    hueToRgb255(fx.param("shadowHue"), sr, sg, sb);
    hueToRgb255(fx.param("highlightHue"), hr, hg, hb);
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    for (size_t i = 0; i < n; i += 4) {
        const int l = lumaOf(rgba[i], rgba[i + 1], rgba[i + 2]);
        const int t = l; // lerp factor 0..255 (luma)
        const int dr = sr + (hr - sr) * t / 255;
        const int dg = sg + (hg - sg) * t / 255;
        const int db = sb + (hb - sb) * t / 255;
        rgba[i] = static_cast<uint8_t>(rgba[i] + (dr - rgba[i]) * amount / 255);
        rgba[i + 1] = static_cast<uint8_t>(rgba[i + 1] + (dg - rgba[i + 1]) * amount / 255);
        rgba[i + 2] = static_cast<uint8_t>(rgba[i + 2] + (db - rgba[i + 2]) * amount / 255);
    }
}

void applyFade(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const double amount = fx.param("amount");
    if (amount == 0.0) {
        return;
    }
    const int lift = static_cast<int>(std::lround(amount * 30.0));
    const double k = 1.0 - 0.3 * amount; // gentle desaturation
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    for (size_t i = 0; i < n; i += 4) {
        const int l = lumaOf(rgba[i], rgba[i + 1], rgba[i + 2]);
        rgba[i] = clampByte(std::lround(l + (rgba[i] - l) * k) + lift);
        rgba[i + 1] = clampByte(std::lround(l + (rgba[i + 1] - l) * k) + lift);
        rgba[i + 2] = clampByte(std::lround(l + (rgba[i + 2] - l) * k) + lift);
    }
}

void applyHighlights(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const double amount = fx.param("amount");
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    for (size_t i = 0; i < n; i += 4) {
        const int l = lumaOf(rgba[i], rgba[i + 1], rgba[i + 2]);
        double wgt = (l - 128.0) / 127.0;
        wgt = wgt < 0.0 ? 0.0 : (wgt > 1.0 ? 1.0 : wgt);
        const int d = static_cast<int>(std::lround(amount * 90.0 * wgt));
        rgba[i] = clampByte(rgba[i] + d);
        rgba[i + 1] = clampByte(rgba[i + 1] + d);
        rgba[i + 2] = clampByte(rgba[i + 2] + d);
    }
}

void applyShadows(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const double amount = fx.param("amount");
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    for (size_t i = 0; i < n; i += 4) {
        const int l = lumaOf(rgba[i], rgba[i + 1], rgba[i + 2]);
        double wgt = (128.0 - l) / 128.0;
        wgt = wgt < 0.0 ? 0.0 : (wgt > 1.0 ? 1.0 : wgt);
        const int d = static_cast<int>(std::lround(amount * 90.0 * wgt));
        rgba[i] = clampByte(rgba[i] + d);
        rgba[i + 1] = clampByte(rgba[i + 1] + d);
        rgba[i + 2] = clampByte(rgba[i + 2] + d);
    }
}

void applyToneGain(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const double gain = fx.param("gain");
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    for (size_t i = 0; i < n; i += 4) {
        rgba[i] = clampByte(std::lround(rgba[i] * gain));
        rgba[i + 1] = clampByte(std::lround(rgba[i + 1] * gain));
        rgba[i + 2] = clampByte(std::lround(rgba[i + 2] * gain));
    }
}

// 4x4 ordered Bayer dither to `levels` quantization steps.
void applyBayerDither(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    static const int kMatrix[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    int levels = static_cast<int>(std::lround(fx.param("levels")));
    levels = std::max(2, levels);
    const double step = 255.0 / (levels - 1);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const double thresh = (kMatrix[x & 3][y & 3] / 16.0 - 0.5) * step;
            Px p = pxAt(rgba, w, x, y);
            *p.r = clampByte(std::lround(std::lround((*p.r + thresh) / step) * step));
            *p.g = clampByte(std::lround(std::lround((*p.g + thresh) / step) * step));
            *p.b = clampByte(std::lround(std::lround((*p.b + thresh) / step) * step));
        }
    }
}

// Mirror one axis about `axis` (0..1 of the extent). Samples come from a
// snapshot so the mirrored side is never re-sampled.
void applyMirror(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const bool vertical = fx.paramBool("vertical");
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    std::vector<uint8_t> src(rgba, rgba + n);
    if (vertical) {
        const int axisX = clampIdx(static_cast<int>(std::lround(fx.param("axis") * w)), 0, w);
        for (int y = 0; y < h; ++y) {
            for (int x = axisX; x < w; ++x) {
                const int xs = clampIdx(2 * axisX - 1 - x, 0, w - 1);
                const size_t i = (static_cast<size_t>(y) * w + x) * 4;
                const size_t is = (static_cast<size_t>(y) * w + xs) * 4;
                rgba[i] = src[is];
                rgba[i + 1] = src[is + 1];
                rgba[i + 2] = src[is + 2];
            }
        }
    } else {
        const int axisY = clampIdx(static_cast<int>(std::lround(fx.param("axis") * h)), 0, h);
        for (int y = axisY; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const int ys = clampIdx(2 * axisY - 1 - y, 0, h - 1);
                const size_t i = (static_cast<size_t>(y) * w + x) * 4;
                const size_t is = (static_cast<size_t>(ys) * w + x) * 4;
                rgba[i] = src[is];
                rgba[i + 1] = src[is + 1];
                rgba[i + 2] = src[is + 2];
            }
        }
    }
}

void applyFlip(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const bool vertical = fx.paramBool("vertical");
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    std::vector<uint8_t> src(rgba, rgba + n);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int xs = vertical ? x : (w - 1 - x);
            const int ys = vertical ? (h - 1 - y) : y;
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            const size_t is = (static_cast<size_t>(ys) * w + xs) * 4;
            rgba[i] = src[is];
            rgba[i + 1] = src[is + 1];
            rgba[i + 2] = src[is + 2];
        }
    }
}

// Soft glow: screen-blend a box-blurred copy back over the original.
void applyGlow(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const int radius = std::max(1, static_cast<int>(std::lround(fx.param("radius"))));
    const double intensity = fx.param("intensity");
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    std::vector<uint8_t> blurred(rgba, rgba + n);
    boxBlurRGBA(blurred.data(), w, h, radius);
    for (size_t i = 0; i < n; i += 4) {
        for (int c = 0; c < 3; ++c) {
            const int a = rgba[i + c];
            const int b = blurred[i + c];
            const int screen = a + b - a * b / 255;
            rgba[i + c] = mixByte(a, screen, intensity);
        }
    }
}

void applyScanlines(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const double amount = fx.param("amount");
    int period = std::max(2, static_cast<int>(std::lround(fx.param("period"))));
    const double f = 1.0 - amount;
    for (int y = 0; y < h; ++y) {
        if (y % period != 0) {
            continue;
        }
        for (int x = 0; x < w; ++x) {
            Px p = pxAt(rgba, w, x, y);
            *p.r = clampByte(std::lround(*p.r * f));
            *p.g = clampByte(std::lround(*p.g * f));
            *p.b = clampByte(std::lround(*p.b * f));
        }
    }
}

// Halftone dots: per cell, a dot whose radius tracks the cell's average
// luma; inside the dot the output is 255, outside 0 (then blended).
void applyHalftone(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    int cell = std::max(2, static_cast<int>(std::lround(fx.param("cell"))));
    const double amount = fx.param("amount");
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    std::vector<uint8_t> src(rgba, rgba + n);
    for (int by = 0; by < h; by += cell) {
        for (int bx = 0; bx < w; bx += cell) {
            const int x1 = std::min(bx + cell, w);
            const int y1 = std::min(by + cell, h);
            long lsum = 0;
            const int count = (x1 - bx) * (y1 - by);
            for (int y = by; y < y1; ++y) {
                for (int x = bx; x < x1; ++x) {
                    const size_t i = (static_cast<size_t>(y) * w + x) * 4;
                    lsum += lumaOf(src[i], src[i + 1], src[i + 2]);
                }
            }
            const double avgL = static_cast<double>(lsum) / count;
            const double dotR = avgL / 255.0 * (cell * 0.5);
            const double cx = bx + cell * 0.5;
            const double cy = by + cell * 0.5;
            for (int y = by; y < y1; ++y) {
                for (int x = bx; x < x1; ++x) {
                    const double dx = x + 0.5 - cx;
                    const double dy = y + 0.5 - cy;
                    const uint8_t dot = (std::sqrt(dx * dx + dy * dy) <= dotR) ? 255 : 0;
                    Px p = pxAt(rgba, w, x, y);
                    *p.r = mixByte(src[(static_cast<size_t>(y) * w + x) * 4], dot, amount);
                    *p.g = mixByte(src[(static_cast<size_t>(y) * w + x) * 4 + 1], dot, amount);
                    *p.b = mixByte(src[(static_cast<size_t>(y) * w + x) * 4 + 2], dot, amount);
                }
            }
        }
    }
}

// Horizontal-only box average (one separable axis of the box blur).
void applyMotionBlurH(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const int r = std::max(1, static_cast<int>(std::lround(fx.param("radius"))));
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    std::vector<uint8_t> src(rgba, rgba + n);
    const int window = 2 * r + 1;
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
}

// Vertical-only box average.
void applyMotionBlurV(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const int r = std::max(1, static_cast<int>(std::lround(fx.param("radius"))));
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    std::vector<uint8_t> src(rgba, rgba + n);
    const int window = 2 * r + 1;
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

// Zoom (radial) blur: average of 8 nearest-neighbor samples along the ray
// from the image center toward the pixel, pulled in by `amount`.
void applyRadialBlur(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const double amount = fx.param("amount");
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    std::vector<uint8_t> src(rgba, rgba + n);
    const double cx = w * 0.5;
    const double cy = h * 0.5;
    const int kSamples = 8;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double sum[3] = {0.0, 0.0, 0.0};
            const double dx = x - cx;
            const double dy = y - cy;
            for (int s = 0; s < kSamples; ++s) {
                const double t = 1.0 - amount * s / kSamples;
                const int xs = clampIdx(static_cast<int>(std::lround(cx + dx * t)), 0, w - 1);
                const int ys = clampIdx(static_cast<int>(std::lround(cy + dy * t)), 0, h - 1);
                const size_t i = (static_cast<size_t>(ys) * w + xs) * 4;
                sum[0] += src[i];
                sum[1] += src[i + 1];
                sum[2] += src[i + 2];
            }
            Px p = pxAt(rgba, w, x, y);
            *p.r = clampByte(std::lround(sum[0] / kSamples));
            *p.g = clampByte(std::lround(sum[1] / kSamples));
            *p.b = clampByte(std::lround(sum[2] / kSamples));
        }
    }
}

// Sine displacement along one axis (nearest sampling, edge clamp).
void applyWave(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const int amp = static_cast<int>(std::lround(fx.param("amplitude")));
    const double wavelength = std::max(1.0, fx.param("wavelength"));
    const bool vertical = fx.paramBool("vertical");
    if (amp == 0) {
        return;
    }
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    std::vector<uint8_t> src(rgba, rgba + n);
    const double k = 2.0 * kPi / wavelength;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int xs = x;
            int ys = y;
            if (vertical) {
                ys = clampIdx(y + static_cast<int>(std::lround(amp * std::sin(k * x))), 0, h - 1);
            } else {
                xs = clampIdx(x + static_cast<int>(std::lround(amp * std::sin(k * y))), 0, w - 1);
            }
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            const size_t is = (static_cast<size_t>(ys) * w + xs) * 4;
            rgba[i] = src[is];
            rgba[i + 1] = src[is + 1];
            rgba[i + 2] = src[is + 2];
        }
    }
}

// Radial ripple rings: displacement along the (normalized) ray from the
// center by amplitude * sin(2 pi dist / wavelength).
void applyRipple(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const int amp = static_cast<int>(std::lround(fx.param("amplitude")));
    const double wavelength = std::max(1.0, fx.param("wavelength"));
    if (amp == 0) {
        return;
    }
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    std::vector<uint8_t> src(rgba, rgba + n);
    const double cx = w * 0.5;
    const double cy = h * 0.5;
    const double k = 2.0 * kPi / wavelength;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const double dx = x + 0.5 - cx;
            const double dy = y + 0.5 - cy;
            const double dist = std::sqrt(dx * dx + dy * dy);
            const int off = static_cast<int>(std::lround(amp * std::sin(k * dist)));
            int xs = x;
            int ys = y;
            if (dist > 0.5) {
                xs = clampIdx(x + static_cast<int>(std::lround(dx / dist * off)), 0, w - 1);
                ys = clampIdx(y + static_cast<int>(std::lround(dy / dist * off)), 0, h - 1);
            }
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            const size_t is = (static_cast<size_t>(ys) * w + xs) * 4;
            rgba[i] = src[is];
            rgba[i + 1] = src[is + 1];
            rgba[i + 2] = src[is + 2];
        }
    }
}

// Fisheye barrel distortion: sample positions pulled toward the center by
// (1 - amount * (1 - r^2)) in normalized radius. Corner-based center
// math (x - cx, like the vignette) keeps amount 0 an exact identity.
void applyFisheye(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const double amount = fx.param("amount");
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    std::vector<uint8_t> src(rgba, rgba + n);
    const double cx = w * 0.5;
    const double cy = h * 0.5;
    const double rMax = std::sqrt(cx * cx + cy * cy);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const double dx = x - cx;
            const double dy = y - cy;
            const double r = std::sqrt(dx * dx + dy * dy);
            const double rn = r / rMax;
            const double f = 1.0 - amount * (1.0 - rn * rn);
            const int xs = clampIdx(static_cast<int>(std::lround(cx + dx * f)), 0, w - 1);
            const int ys = clampIdx(static_cast<int>(std::lround(cy + dy * f)), 0, h - 1);
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            const size_t is = (static_cast<size_t>(ys) * w + xs) * 4;
            rgba[i] = src[is];
            rgba[i + 1] = src[is + 1];
            rgba[i + 2] = src[is + 2];
        }
    }
}

// Wrap-tiling: the image repeats every blockW x blockH pixels.
void applyTile(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const int bw = std::max(1, static_cast<int>(std::lround(fx.param("blockW"))));
    const int bh = std::max(1, static_cast<int>(std::lround(fx.param("blockH"))));
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    std::vector<uint8_t> src(rgba, rgba + n);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int xs = x % bw;
            const int ys = y % bh;
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            const size_t is = (static_cast<size_t>(ys) * w + xs) * 4;
            rgba[i] = src[is];
            rgba[i + 1] = src[is + 1];
            rgba[i + 2] = src[is + 2];
        }
    }
}

// SMPTE-style 7-column color bars blended over the frame.
void applyBars(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const double amount = fx.param("amount");
    if (amount <= 0.0) {
        return;
    }
    static const int kBars[7][3] = {{255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
                                    {255, 0, 255},   {255, 0, 0},   {0, 0, 255}};
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int col = clampIdx(x * 7 / w, 0, 6);
            Px p = pxAt(rgba, w, x, y);
            *p.r = mixByte(*p.r, kBars[col][0], amount);
            *p.g = mixByte(*p.g, kBars[col][1], amount);
            *p.b = mixByte(*p.b, kBars[col][2], amount);
        }
    }
}

// Horizontal (or vertical) grayscale ramp blended over the frame.
void applyGradient(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const double amount = fx.param("amount");
    const bool vertical = fx.paramBool("vertical");
    if (amount <= 0.0) {
        return;
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const double t = vertical ? (h > 1 ? static_cast<double>(y) / (h - 1) : 0.0)
                                      : (w > 1 ? static_cast<double>(x) / (w - 1) : 0.0);
            const int v = static_cast<int>(std::lround(t * 255.0));
            Px p = pxAt(rgba, w, x, y);
            *p.r = mixByte(*p.r, v, amount);
            *p.g = mixByte(*p.g, v, amount);
            *p.b = mixByte(*p.b, v, amount);
        }
    }
}

// Grid lines over the frame.
void applyGrid(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const double amount = fx.param("amount");
    int spacing = std::max(2, static_cast<int>(std::lround(fx.param("spacing"))));
    int thickness = std::max(1, static_cast<int>(std::lround(fx.param("thickness"))));
    if (amount <= 0.0) {
        return;
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (x % spacing >= thickness && y % spacing >= thickness) {
                continue;
            }
            Px p = pxAt(rgba, w, x, y);
            *p.r = mixByte(*p.r, 0, amount);
            *p.g = mixByte(*p.g, 0, amount);
            *p.b = mixByte(*p.b, 0, amount);
        }
    }
}

// Full-frame monochrome hash noise (the film-grain noise family).
void applyNoiseGen(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const double amount = fx.param("amount");
    const uint32_t seed = static_cast<uint32_t>(std::lround(fx.param("seed") * 1000.0));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int v = clampByte(
                128 + grainNoise(static_cast<uint32_t>(x), static_cast<uint32_t>(y), seed));
            Px p = pxAt(rgba, w, x, y);
            *p.r = mixByte(*p.r, v, amount);
            *p.g = mixByte(*p.g, v, amount);
            *p.b = mixByte(*p.b, v, amount);
        }
    }
}

// Ironbow-style thermal palette over luma (6 stops).
void applyThermal(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const double amount = fx.param("amount");
    static const int kStops[6][3] = {{0, 0, 0},     {0, 0, 128},    {128, 0, 128},
                                     {192, 32, 32}, {240, 160, 32}, {255, 255, 224}};
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    for (size_t i = 0; i < n; i += 4) {
        const int l = lumaOf(rgba[i], rgba[i + 1], rgba[i + 2]);
        const int si = clampIdx(l / 51, 0, 5); // l 255 -> index 5
        const int base = si * 51;
        const int t = clampIdx(l - base, 0, 51);
        const int sn = si < 5 ? si + 1 : 5;
        const int dr = kStops[si][0] + (kStops[sn][0] - kStops[si][0]) * t / 51;
        const int dg = kStops[si][1] + (kStops[sn][1] - kStops[si][1]) * t / 51;
        const int db = kStops[si][2] + (kStops[sn][2] - kStops[si][2]) * t / 51;
        rgba[i] = mixByte(rgba[i], clampByte(dr), amount);
        rgba[i + 1] = mixByte(rgba[i + 1], clampByte(dg), amount);
        rgba[i + 2] = mixByte(rgba[i + 2], clampByte(db), amount);
    }
}

// Hash-driven per-block-row horizontal displacement.
void applyGlitch(const EffectInstance &fx, uint8_t *rgba, int w, int h) {
    const double amount = fx.param("amount");
    if (amount <= 0.0) {
        return;
    }
    int blockH = std::max(1, static_cast<int>(std::lround(fx.param("blockH"))));
    int maxShift = std::max(1, static_cast<int>(std::lround(fx.param("maxShift"))));
    const uint32_t seed = static_cast<uint32_t>(std::lround(fx.param("seed") * 1000.0));
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    std::vector<uint8_t> src(rgba, rgba + n);
    for (int y = 0; y < h; ++y) {
        const uint32_t blockRow = static_cast<uint32_t>(y / blockH);
        const int raw = static_cast<int>(grainHash(0, blockRow, seed) %
                                         static_cast<uint32_t>(2 * maxShift + 1)) -
                        maxShift;
        const int shift = static_cast<int>(std::lround(raw * amount));
        for (int x = 0; x < w; ++x) {
            const int xs = clampIdx(x + shift, 0, w - 1);
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            const size_t is = (static_cast<size_t>(y) * w + xs) * 4;
            rgba[i] = src[is];
            rgba[i + 1] = src[is + 1];
            rgba[i + 2] = src[is + 2];
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
        // ---- Color (Phase 3) ----
        {"color.corrector",
         "Color Correction",
         "Color",
         {{"exposure", "Exposure", EffectParamType::Number, -2.0, 2.0, 0.0},
          {"contrast", "Contrast", EffectParamType::Number, -1.0, 1.0, 0.0},
          {"highlights", "Highlights", EffectParamType::Number, -1.0, 1.0, 0.0},
          {"shadows", "Shadows", EffectParamType::Number, -1.0, 1.0, 0.0},
          {"saturation", "Saturation", EffectParamType::Number, 0.0, 2.0, 1.0},
          {"vibrance", "Vibrance", EffectParamType::Number, 0.0, 1.0, 0.0},
          {"temperature", "Temperature", EffectParamType::Number, -1.0, 1.0, 0.0},
          {"tint", "Tint", EffectParamType::Number, -1.0, 1.0, 0.0},
          {"hue", "Hue", EffectParamType::Number, -180.0, 180.0, 0.0}}},
        {"color.channelgain",
         "Channel Gain",
         "Color",
         {{"rGain", "Red Gain", EffectParamType::Number, 0.0, 2.0, 1.0},
          {"gGain", "Green Gain", EffectParamType::Number, 0.0, 2.0, 1.0},
          {"bGain", "Blue Gain", EffectParamType::Number, 0.0, 2.0, 1.0}}},
        {"color.colorize",
         "Colorize",
         "Color",
         {{"hue", "Hue", EffectParamType::Number, 0.0, 360.0, 30.0},
          {"amount", "Amount", EffectParamType::Number, 0.0, 1.0, 1.0}}},
        {"color.duotone",
         "Duotone",
         "Color",
         {{"shadowHue", "Shadow Hue", EffectParamType::Number, 0.0, 360.0, 210.0},
          {"highlightHue", "Highlight Hue", EffectParamType::Number, 0.0, 360.0, 40.0},
          {"amount", "Amount", EffectParamType::Number, 0.0, 1.0, 1.0}}},
        {"color.fade",
         "Faded Film",
         "Color",
         {{"amount", "Amount", EffectParamType::Number, 0.0, 1.0, 0.5}}},
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
        // ---- Tone (Phase 3) ----
        {"tone.highlights",
         "Highlights",
         "Tone",
         {{"amount", "Amount", EffectParamType::Number, -1.0, 1.0, 0.0}}},
        {"tone.shadows",
         "Shadows",
         "Tone",
         {{"amount", "Amount", EffectParamType::Number, -1.0, 1.0, 0.0}}},
        {"tone.gain", "Gain", "Tone", {{"gain", "Gain", EffectParamType::Number, 0.0, 2.0, 1.0}}},
        {"tone.bayer",
         "Bayer Dither",
         "Tone",
         {{"levels", "Levels", EffectParamType::Number, 2.0, 8.0, 2.0}}},
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
        // ---- Filter (Phase 3) ----
        {"filter.mirror",
         "Mirror",
         "Filter",
         {{"axis", "Axis", EffectParamType::Number, 0.05, 0.95, 0.5},
          {"vertical", "Vertical Axis", EffectParamType::Boolean, 0.0, 1.0, 1.0}}},
        {"filter.flip",
         "Flip",
         "Filter",
         {{"vertical", "Vertical", EffectParamType::Boolean, 0.0, 1.0, 0.0}}},
        {"filter.glow",
         "Glow",
         "Filter",
         {{"radius", "Radius", EffectParamType::Number, 1.0, 16.0, 6.0},
          {"intensity", "Intensity", EffectParamType::Number, 0.0, 1.0, 0.5}}},
        {"filter.scanlines",
         "Scanlines",
         "Filter",
         {{"amount", "Amount", EffectParamType::Number, 0.0, 1.0, 0.5},
          {"period", "Period", EffectParamType::Number, 2.0, 8.0, 3.0}}},
        {"filter.halftone",
         "Halftone",
         "Filter",
         {{"cell", "Cell Size", EffectParamType::Number, 2.0, 16.0, 6.0},
          {"amount", "Amount", EffectParamType::Number, 0.0, 1.0, 1.0}}},
        // ---- Blur & Sharpen ----
        {"blur.box",
         "Box Blur",
         "Blur & Sharpen",
         {{"radius", "Radius", EffectParamType::Number, 1.0, 16.0, 2.0}}},
        {"blur.gaussian",
         "Gaussian Blur",
         "Blur & Sharpen",
         {{"radius", "Radius", EffectParamType::Number, 1.0, 16.0, 3.0}}},
        // ---- Blur & Sharpen (Phase 3) ----
        {"blur.motionH",
         "Motion Blur H",
         "Blur & Sharpen",
         {{"radius", "Radius", EffectParamType::Number, 1.0, 32.0, 8.0}}},
        {"blur.motionV",
         "Motion Blur V",
         "Blur & Sharpen",
         {{"radius", "Radius", EffectParamType::Number, 1.0, 32.0, 8.0}}},
        {"blur.radial",
         "Radial Blur",
         "Blur & Sharpen",
         {{"amount", "Amount", EffectParamType::Number, 0.0, 1.0, 0.5}}},
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
        // ---- Stylize (Phase 3) ----
        {"stylize.thermal",
         "Thermal",
         "Stylize",
         {{"amount", "Amount", EffectParamType::Number, 0.0, 1.0, 1.0}}},
        {"stylize.glitch",
         "Glitch",
         "Stylize",
         {{"amount", "Amount", EffectParamType::Number, 0.0, 1.0, 0.4},
          {"seed", "Seed", EffectParamType::Number, 0.0, 1.0, 0.5},
          {"blockH", "Block Height", EffectParamType::Number, 2.0, 64.0, 8.0},
          {"maxShift", "Max Shift", EffectParamType::Number, 1.0, 64.0, 24.0}}},
        // ---- Distort (Phase 3, new category) ----
        {"distort.wave",
         "Wave",
         "Distort",
         {{"amplitude", "Amplitude", EffectParamType::Number, 0.0, 32.0, 6.0},
          {"wavelength", "Wavelength", EffectParamType::Number, 8.0, 128.0, 32.0},
          {"vertical", "Vertical", EffectParamType::Boolean, 0.0, 1.0, 0.0}}},
        {"distort.ripple",
         "Ripple",
         "Distort",
         {{"amplitude", "Amplitude", EffectParamType::Number, 0.0, 24.0, 4.0},
          {"wavelength", "Wavelength", EffectParamType::Number, 16.0, 256.0, 64.0}}},
        {"distort.fisheye",
         "Fisheye",
         "Distort",
         {{"amount", "Amount", EffectParamType::Number, 0.0, 1.0, 0.5}}},
        {"distort.tile",
         "Tile",
         "Distort",
         {{"blockW", "Block Width", EffectParamType::Number, 2.0, 64.0, 16.0},
          {"blockH", "Block Height", EffectParamType::Number, 2.0, 64.0, 16.0}}},
        // ---- Generate (Phase 3, new category) ----
        {"generate.bars",
         "Color Bars",
         "Generate",
         {{"amount", "Amount", EffectParamType::Number, 0.0, 1.0, 1.0}}},
        {"generate.gradient",
         "Gradient",
         "Generate",
         {{"amount", "Amount", EffectParamType::Number, 0.0, 1.0, 1.0},
          {"vertical", "Vertical", EffectParamType::Boolean, 0.0, 1.0, 0.0}}},
        {"generate.grid",
         "Grid",
         "Generate",
         {{"spacing", "Spacing", EffectParamType::Number, 4.0, 64.0, 32.0},
          {"thickness", "Thickness", EffectParamType::Number, 1.0, 4.0, 1.0},
          {"amount", "Amount", EffectParamType::Number, 0.0, 1.0, 1.0}}},
        {"generate.noise",
         "Noise",
         "Generate",
         {{"amount", "Amount", EffectParamType::Number, 0.0, 1.0, 0.5},
          {"seed", "Seed", EffectParamType::Number, 0.0, 1.0, 0.5}}},
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

// ---- M5 Phase 3: keyframe resolution + editing ----

// True when `key` names a Number param of the descriptor (only Number
// params can be keyframed).
static bool isNumberParam(const EffectDescriptor *d, const std::string &key) {
    if (!d) {
        return false;
    }
    for (const EffectParamDescriptor &p : d->params) {
        if (p.key == key) {
            return p.type == EffectParamType::Number;
        }
    }
    return false;
}

// Clamps a value into the descriptor range of `key` (doubles pass through
// when the key is unknown - never called for those).
static double clampToParam(const EffectDescriptor *d, const std::string &key, double value) {
    for (const EffectParamDescriptor &p : d->params) {
        if (p.key == key) {
            return std::min(std::max(value, p.minValue), p.maxValue);
        }
    }
    return value;
}

double EffectInstance::paramAt(const std::string &key, int64_t clipFrame) const {
    if (clipFrame == kNoKeyframeTime) {
        return param(key);
    }
    const EffectDescriptor *d = descriptor();
    if (!d || !isNumberParam(d, key)) {
        return param(key);
    }
    const std::vector<EffectKeyframe> *track = keyframeTrack(key);
    if (!track || track->empty()) {
        return param(key);
    }
    if (clipFrame <= track->front().frame) {
        return track->front().value;
    }
    if (clipFrame >= track->back().frame) {
        return track->back().value;
    }
    for (size_t i = 0; i + 1 < track->size(); ++i) {
        const EffectKeyframe &a = (*track)[i];
        const EffectKeyframe &b = (*track)[i + 1];
        if (a.frame <= clipFrame && clipFrame <= b.frame) {
            if (b.frame == a.frame) {
                return b.value; // unreachable: frames are unique
            }
            const double t =
                static_cast<double>(clipFrame - a.frame) / static_cast<double>(b.frame - a.frame);
            return a.value + (b.value - a.value) * t;
        }
    }
    return track->back().value;
}

void EffectInstance::setKeyframe(const std::string &key, int64_t frame, double value) {
    const EffectDescriptor *d = descriptor();
    if (!d || !isNumberParam(d, key)) {
        return;
    }
    const double clamped = clampToParam(d, key, value);
    for (EffectKeyframeTrack &track : keyframes) {
        if (track.key != key) {
            continue;
        }
        for (EffectKeyframe &pt : track.points) {
            if (pt.frame == frame) {
                pt.value = clamped;
                return;
            }
        }
        const EffectKeyframe nk{frame, clamped};
        auto it = std::lower_bound(
            track.points.begin(), track.points.end(), nk,
            [](const EffectKeyframe &a, const EffectKeyframe &b) { return a.frame < b.frame; });
        track.points.insert(it, nk);
        return;
    }
    EffectKeyframeTrack track;
    track.key = key;
    track.points.push_back({frame, clamped});
    keyframes.push_back(std::move(track));
}

bool EffectInstance::removeKeyframe(const std::string &key, int64_t frame) {
    for (auto it = keyframes.begin(); it != keyframes.end(); ++it) {
        if (it->key != key) {
            continue;
        }
        for (auto pt = it->points.begin(); pt != it->points.end(); ++pt) {
            if (pt->frame == frame) {
                it->points.erase(pt);
                if (it->points.empty()) {
                    keyframes.erase(it); // empty tracks never persist
                }
                return true;
            }
        }
        return false;
    }
    return false;
}

const EffectKeyframe *EffectInstance::keyframeAt(const std::string &key, int64_t frame) const {
    const std::vector<EffectKeyframe> *track = keyframeTrack(key);
    if (!track) {
        return nullptr;
    }
    for (const EffectKeyframe &pt : *track) {
        if (pt.frame == frame) {
            return &pt;
        }
    }
    return nullptr;
}

const std::vector<EffectKeyframe> *EffectInstance::keyframeTrack(const std::string &key) const {
    for (const EffectKeyframeTrack &track : keyframes) {
        if (track.key == key) {
            return &track.points;
        }
    }
    return nullptr;
}

void EffectInstance::clearKeyframes(const std::string &key) {
    for (auto it = keyframes.begin(); it != keyframes.end(); ++it) {
        if (it->key == key) {
            keyframes.erase(it);
            return;
        }
    }
}

bool EffectInstance::hasKeyframes() const {
    for (const EffectKeyframeTrack &track : keyframes) {
        if (!track.points.empty()) {
            return true;
        }
    }
    return false;
}

void EffectInstance::rebaseKeyframes(int64_t offset) {
    for (auto it = keyframes.begin(); it != keyframes.end();) {
        std::vector<EffectKeyframe> kept;
        kept.reserve(it->points.size());
        for (const EffectKeyframe &pt : it->points) {
            const int64_t nf = pt.frame - offset;
            if (nf >= 0) {
                kept.push_back({nf, pt.value});
            }
        }
        if (kept.empty()) {
            it = keyframes.erase(it);
        } else {
            it->points = std::move(kept);
            ++it;
        }
    }
}

void applyEffectStack(uint8_t *rgba, int width, int height,
                      const std::vector<EffectInstance> &stack, int64_t clipFrame) {
    if (!rgba || width <= 0 || height <= 0) {
        return;
    }
    for (const EffectInstance &fxIn : stack) {
        if (!fxIn.enabled || fxIn.effectId.empty()) {
            continue;
        }
        const EffectDescriptor *d = findEffect(fxIn.effectId);
        if (!d) {
            continue; // unknown id (from a newer catalog): skip, do not crash
        }
        // Keyframe resolution: when a time context is given and the
        // instance carries tracks, evaluate every param at clipFrame into
        // a local resolved copy (values stay in descriptor order, so the
        // per-effect processors keep reading fx.param() untouched).
        EffectInstance resolved;
        const EffectInstance *fx = &fxIn;
        if (clipFrame != kNoKeyframeTime && fxIn.hasKeyframes()) {
            resolved = fxIn;
            if (resolved.values.size() < d->params.size()) {
                const size_t old = resolved.values.size();
                resolved.values.resize(d->params.size());
                for (size_t i = old; i < resolved.values.size(); ++i) {
                    resolved.values[i] = d->params[i].defaultValue;
                }
            }
            for (size_t i = 0; i < d->params.size(); ++i) {
                resolved.values[i] = fxIn.paramAt(d->params[i].key, clipFrame);
            }
            fx = &resolved;
        }
        const std::string &id = d->id;
        if (id == "color.brightness") {
            applyBrightness(*fx, rgba, width, height);
        } else if (id == "color.contrast") {
            applyContrast(*fx, rgba, width, height);
        } else if (id == "color.saturation") {
            applySaturation(*fx, rgba, width, height);
        } else if (id == "color.vibrance") {
            applyVibrance(*fx, rgba, width, height);
        } else if (id == "color.hue") {
            applyHue(*fx, rgba, width, height);
        } else if (id == "color.temperature") {
            applyTemperature(*fx, rgba, width, height);
        } else if (id == "color.tint") {
            applyTint(*fx, rgba, width, height);
        } else if (id == "color.exposure") {
            applyExposure(*fx, rgba, width, height);
        } else if (id == "color.gamma") {
            applyGamma(*fx, rgba, width, height);
        } else if (id == "tone.levels") {
            applyLevels(*fx, rgba, width, height);
        } else if (id == "tone.posterize") {
            applyPosterize(*fx, rgba, width, height);
        } else if (id == "tone.threshold") {
            applyThreshold(*fx, rgba, width, height);
        } else if (id == "tone.solarize") {
            applySolarize(*fx, rgba, width, height);
        } else if (id == "tone.invert") {
            applyInvert(*fx, rgba, width, height);
        } else if (id == "filter.grayscale") {
            applyGrayscale(*fx, rgba, width, height);
        } else if (id == "filter.sepia") {
            applySepia(*fx, rgba, width, height);
        } else if (id == "filter.vignette") {
            applyVignette(*fx, rgba, width, height);
        } else if (id == "filter.grain") {
            applyGrain(*fx, rgba, width, height);
        } else if (id == "filter.pixelate") {
            applyPixelate(*fx, rgba, width, height);
        } else if (id == "filter.chromatic") {
            applyChromatic(*fx, rgba, width, height);
        } else if (id == "blur.box") {
            applyBoxBlur(*fx, rgba, width, height);
        } else if (id == "blur.gaussian") {
            applyGaussianBlur(*fx, rgba, width, height);
        } else if (id == "stylize.sharpen") {
            applySharpen(*fx, rgba, width, height);
        } else if (id == "stylize.edges") {
            applyEdges(*fx, rgba, width, height);
        } else if (id == "stylize.emboss") {
            applyEmboss(*fx, rgba, width, height);
        } else if (id == "color.corrector") {
            applyCorrector(*fx, rgba, width, height);
        } else if (id == "color.channelgain") {
            applyChannelGain(*fx, rgba, width, height);
        } else if (id == "color.colorize") {
            applyColorize(*fx, rgba, width, height);
        } else if (id == "color.duotone") {
            applyDuotone(*fx, rgba, width, height);
        } else if (id == "color.fade") {
            applyFade(*fx, rgba, width, height);
        } else if (id == "tone.highlights") {
            applyHighlights(*fx, rgba, width, height);
        } else if (id == "tone.shadows") {
            applyShadows(*fx, rgba, width, height);
        } else if (id == "tone.gain") {
            applyToneGain(*fx, rgba, width, height);
        } else if (id == "tone.bayer") {
            applyBayerDither(*fx, rgba, width, height);
        } else if (id == "filter.mirror") {
            applyMirror(*fx, rgba, width, height);
        } else if (id == "filter.flip") {
            applyFlip(*fx, rgba, width, height);
        } else if (id == "filter.glow") {
            applyGlow(*fx, rgba, width, height);
        } else if (id == "filter.scanlines") {
            applyScanlines(*fx, rgba, width, height);
        } else if (id == "filter.halftone") {
            applyHalftone(*fx, rgba, width, height);
        } else if (id == "blur.motionH") {
            applyMotionBlurH(*fx, rgba, width, height);
        } else if (id == "blur.motionV") {
            applyMotionBlurV(*fx, rgba, width, height);
        } else if (id == "blur.radial") {
            applyRadialBlur(*fx, rgba, width, height);
        } else if (id == "stylize.thermal") {
            applyThermal(*fx, rgba, width, height);
        } else if (id == "stylize.glitch") {
            applyGlitch(*fx, rgba, width, height);
        } else if (id == "distort.wave") {
            applyWave(*fx, rgba, width, height);
        } else if (id == "distort.ripple") {
            applyRipple(*fx, rgba, width, height);
        } else if (id == "distort.fisheye") {
            applyFisheye(*fx, rgba, width, height);
        } else if (id == "distort.tile") {
            applyTile(*fx, rgba, width, height);
        } else if (id == "generate.bars") {
            applyBars(*fx, rgba, width, height);
        } else if (id == "generate.gradient") {
            applyGradient(*fx, rgba, width, height);
        } else if (id == "generate.grid") {
            applyGrid(*fx, rgba, width, height);
        } else if (id == "generate.noise") {
            applyNoiseGen(*fx, rgba, width, height);
        }
    }
}

} // namespace fc
