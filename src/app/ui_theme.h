#pragma once

#include <QColor>

#include <cstdint>

namespace fc {
namespace ui {

// FusionCut Pro design tokens (suggestion #55: one small palette so
// panels stop hand-picking hex values). The values CONSOLIDATE the
// colors the app already shipped (mainwindow's anonymous-namespace
// tokens + the timeline/preview locals) - nothing changes color by
// adopting this header; the names are the contract now.
//
// dark-first, contrast-checked against kSurface2 for text tokens
// (kText 0xE8E8E8 on 0x252525 is ~12:1, kTextDim 0x999999 ~5.5:1 -
// both clear WCAG AA for body text).
inline constexpr unsigned int kSurface = 0x1E1E1E;      // window charcoal
inline constexpr unsigned int kSurface2 = 0x252525;     // panel background
inline constexpr unsigned int kSurface3 = 0x2E2E2E;     // button / input chrome
inline constexpr unsigned int kCanvas = 0x141414;       // monitor canvas
inline constexpr unsigned int kTimelineBg = 0x1B1B1B;   // timeline lane backdrop
inline constexpr unsigned int kLine = 0x3A3A3A;         // hairlines / borders
inline constexpr unsigned int kText = 0xE8E8E8;         // primary text
inline constexpr unsigned int kTextDim = 0x999999;      // secondary text
inline constexpr unsigned int kTextDisabled = 0x777777; // disabled text
inline constexpr unsigned int kOnAccent = 0x101010;     // text on accent fills
inline constexpr unsigned int kAccent = 0x00A8FF;       // FusionCut accent
inline constexpr unsigned int kAccentBright = 0x33B9FF; // hover/active accent
inline constexpr unsigned int kDanger = 0xE74C3C;       // errors / destructive
inline constexpr unsigned int kWarning = 0xFFB020;      // attention (rate chip)
inline constexpr unsigned int kSuccess = 0x2ECC71;      // confirm / solo

// QColor is not a literal type, so tokens convert through this helper.
inline QColor color(unsigned int token) {
    return QColor(token);
}

// Add alpha to a token (0-255) without hand-building QColor triples.
inline QColor withAlpha(unsigned int token, int alpha) {
    QColor c(token);
    c.setAlpha(alpha);
    return c;
}

// Linear blend a->b at t (0 = a, 1 = b) - tinted fills (banners,
// hover chips) derive from the tokens instead of new hex values.
inline QColor mix(const QColor &a, const QColor &b, double t) {
    if (t < 0.0) {
        t = 0.0;
    }
    if (t > 1.0) {
        t = 1.0;
    }
    const int r = static_cast<int>(a.red() + (b.red() - a.red()) * t);
    const int g = static_cast<int>(a.green() + (b.green() - a.green()) * t);
    const int bl = static_cast<int>(a.blue() + (b.blue() - a.blue()) * t);
    return QColor(r, g, bl);
}

// Tinted surface fill for attention surfaces (error banners, hatches):
// 12-15% of the signal color over kSurface2 keeps dark-theme contrast.
inline QColor tint(unsigned int token) {
    return mix(color(kSurface2), color(token), 0.14);
}

} // namespace ui
} // namespace fc
