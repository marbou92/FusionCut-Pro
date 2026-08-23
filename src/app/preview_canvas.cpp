#include "preview_canvas.h"

#include <algorithm>

#include <QPaintEvent>
#include <QPainter>

namespace {
constexpr unsigned int kCanvasBg = 0x141414;
}

PreviewCanvas::PreviewCanvas(QWidget *parent) : QWidget(parent) {
    setMinimumSize(160, 90);
}

QSize PreviewCanvas::minimumSizeHint() const {
    return QSize(160, 90);
}

void PreviewCanvas::setAspectHint(double aspect) {
    if (aspect > 0.05 && aspect < 20.0) {
        aspectHint_ = aspect;
        update();
    }
}

void PreviewCanvas::setFrame(const QImage &frame, double ptsSeconds) {
    current_ = frame;
    lastPts_ = ptsSeconds;
    if (!current_.isNull() && current_.height() > 0) {
        aspectHint_ = static_cast<double>(current_.width()) / current_.height();
    }
    update();
}

void PreviewCanvas::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(kCanvasBg));

    if (current_.isNull()) {
        painter.setPen(QColor(0x6A, 0x6A, 0x6A));
        painter.drawText(rect(), Qt::AlignCenter, tr("No media loaded\nImport a file to begin"));
        return;
    }

    // Letterbox: largest rect with the frame aspect inside the widget.
    const double widgetAspect = static_cast<double>(width()) / std::max(1, height());
    int drawW = width();
    int drawH = height();
    if (widgetAspect > aspectHint_) {
        drawW = static_cast<int>(height() * aspectHint_);
    } else {
        drawH = static_cast<int>(width() / aspectHint_);
    }
    const int x = (width() - drawW) / 2;
    const int y = (height() - drawH) / 2;

    // Fast transform: proxies are 360p, scaling cost stays negligible on
    // legacy CPUs (the 1 GB target machines).
    buffer_ = QPixmap::fromImage(
        current_.scaled(drawW, drawH, Qt::IgnoreAspectRatio, Qt::FastTransformation));
    painter.drawPixmap(x, y, buffer_);
}
