#include "scopes_panel.h"

#include <QPainter>
#include <QTimer>

#include <algorithm>
#include <cmath>

#include "ui_theme.h"

// ui_theme.h tokens live in fc::ui; the panel addresses them as ui::...
using namespace fc;

namespace fc {

namespace {

// Trace base colors (suggestion #38): per-channel tones reuse the theme
// tokens - R: kDanger (#E74C3C tone), G: kSuccess (#2ECC71 tone),
// B: kAccent; luma is a green-grey mix. Alpha per pixel accumulates with
// the sample count so dense areas glow brighter than sparse ones.
QColor paradeChannelColor(int channel) {
    switch (channel) {
    case 0:
        return ui::color(ui::kDanger);
    case 1:
        return ui::color(ui::kSuccess);
    default:
        return ui::color(ui::kAccent);
    }
}

constexpr int kAlphaStep = 40; // per-sample intensity accumulation (saturates ~6 hits)

// Bumps the alpha byte of an RGBA8888 pixel, saturating at 255.
inline void bumpPixelAlpha(uchar *pixel, const QColor &color) {
    pixel[0] = static_cast<uchar>(color.red());
    pixel[1] = static_cast<uchar>(color.green());
    pixel[2] = static_cast<uchar>(color.blue());
    int a = pixel[3] + kAlphaStep;
    pixel[3] = static_cast<uchar>(a > 255 ? 255 : a);
}

} // namespace

ScopesPanel::ScopesPanel(QWidget *parent) : QWidget(parent) {
    setMinimumSize(220, 130);
    setToolTip(tr("Program scopes: RGB parade (left) and luma waveform (right)"));

    analysisTimer_ = new QTimer(this);
    analysisTimer_->setSingleShot(true);
    analysisTimer_->setInterval(kThrottleMs);
    connect(analysisTimer_, &QTimer::timeout, this, [this] { analyzePending(); });
}

void ScopesPanel::setFrame(const QImage &frame) {
    pendingFrame_ = frame; // shallow copy: only the last frame survives
    pendingValid_ = true;
    if (!analysisTimer_->isActive()) {
        analysisTimer_->start();
    }
}

void ScopesPanel::analyzePending() {
    if (!pendingValid_) {
        return;
    }
    pendingValid_ = false;

    if (pendingFrame_.isNull() || pendingFrame_.width() <= 0 || pendingFrame_.height() <= 0) {
        anaW_ = anaH_ = 0;
        lumaTrace_ = QImage();
        paradeTrace_ = QImage();
        update();
        return;
    }

    // Downscale to <= 320x180 (aspect kept, FastTransformation per the
    // scope contract). Analysis resolution is independent of the widget.
    const QImage ana =
        (pendingFrame_.width() <= kMaxAnalysisW && pendingFrame_.height() <= kMaxAnalysisH
             ? pendingFrame_.convertToFormat(QImage::Format_RGB32)
             : pendingFrame_
                   .scaled(kMaxAnalysisW, kMaxAnalysisH, Qt::KeepAspectRatio,
                           Qt::FastTransformation)
                   .convertToFormat(QImage::Format_RGB32));
    anaW_ = ana.width();
    anaH_ = ana.height();

    // (Re)create the trace buffers only when the geometry changed -
    // steady-state playback reuses them with zero allocation.
    if (lumaTrace_.width() != anaW_ || lumaTrace_.height() != kLumaBins) {
        lumaTrace_ = QImage(anaW_, kLumaBins, QImage::Format_RGBA8888);
    }
    if (paradeTrace_.width() != kParadeChannels * anaW_ || paradeTrace_.height() != kLumaBins) {
        paradeTrace_ = QImage(kParadeChannels * anaW_, kLumaBins, QImage::Format_RGBA8888);
    }
    const QColor transparent(0, 0, 0, 0);
    lumaTrace_.fill(transparent);
    paradeTrace_.fill(transparent);

    const QColor lumaColor =
        ui::mix(ui::color(ui::kSuccess), ui::color(ui::kTextDim), 0.45); // green-grey

    const int srcStride = ana.bytesPerLine();
    const int lumaStride = lumaTrace_.bytesPerLine();
    const int paradeStride = paradeTrace_.bytesPerLine();
    uchar *lumaBits = lumaTrace_.bits();
    uchar *paradeBits = paradeTrace_.bits();

    // One pass over the downscaled frame fills both traces.
    for (int y = 0; y < anaH_; ++y) {
        const uchar *srcLine = ana.constBits() + y * srcStride;
        for (int x = 0; x < anaW_; ++x) {
            // Format_RGB32 stores 0xffRRGGBB in the native uint32.
            const unsigned int px = *reinterpret_cast<const unsigned int *>(srcLine + x * 4);
            const int r = static_cast<int>((px >> 16) & 0xFFu);
            const int g = static_cast<int>((px >> 8) & 0xFFu);
            const int b = static_cast<int>(px & 0xFFu);
            const int luma = std::min(255, (r * 299 + g * 587 + b * 114) / 1000); // BT.601, 0..255

            // Luma waveform: column x, row = inverted luma (0% at bottom).
            bumpPixelAlpha(lumaBits + (kLumaBins - 1 - luma) * lumaStride + x * 4, lumaColor);

            // RGB parade: channel c's whole-image trace in the c-th third.
            const int channels[3] = {r, g, b};
            for (int c = 0; c < kParadeChannels; ++c) {
                const int v = std::min(255, channels[c]);
                bumpPixelAlpha(paradeBits + (kLumaBins - 1 - v) * paradeStride +
                                   (c * anaW_ + x) * 4,
                               paradeChannelColor(c));
            }
        }
    }
    update();
}

void ScopesPanel::paintEvent(QPaintEvent * /*event*/) {
    QPainter p(this);

    p.fillRect(rect(), ui::color(ui::kCanvas));

    // Two panes: parade LEFT, luma RIGHT, with a caption band on top.
    constexpr int kMargin = 4;
    constexpr int kGap = 8;
    constexpr int kCaptionH = 14;
    const int paneW = std::max(10, (width() - kMargin * 2 - kGap) / 2);
    const int paneH = std::max(10, height() - kMargin - kCaptionH - kMargin);
    const QRect paradeRect(kMargin, kMargin + kCaptionH, paneW, paneH);
    const QRect lumaRect(kMargin + paneW + kGap, kMargin + kCaptionH, paneW, paneH);

    // Graticule: horizontal lines at 0 / 25 / 50 / 75 / 100 %.
    p.setPen(ui::color(ui::kLine));
    for (const QRect &pane : {paradeRect, lumaRect}) {
        for (int i = 0; i <= 4; ++i) {
            const int gy = pane.top() + i * pane.height() / 4;
            p.drawLine(pane.left(), gy, pane.right(), gy);
        }
    }

    if (anaW_ > 0) {
        p.drawImage(paradeRect, paradeTrace_);
        p.drawImage(lumaRect, lumaTrace_);
    } else {
        p.setPen(ui::color(ui::kTextDisabled));
        p.drawText(rect(), Qt::AlignCenter, tr("No signal"));
    }

    p.setPen(ui::color(ui::kTextDim));
    p.drawText(QRect(kMargin, kMargin, paneW, kCaptionH), Qt::AlignLeft | Qt::AlignVCenter,
               tr("RGB PARADE"));
    p.drawText(QRect(kMargin + paneW + kGap, kMargin, paneW, kCaptionH),
               Qt::AlignLeft | Qt::AlignVCenter, tr("LUMA"));
}

} // namespace fc
