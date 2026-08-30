#include "timeline_panel.h"

#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSlider>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

#include "timecode.h"

namespace {
constexpr int kHeaderWidth = 96;
constexpr int kRulerHeight = 26;
constexpr int kTrackHeight = 44;
constexpr int kZoomBarHeight = 30;

const QColor kPanelBg(0x1B, 0x1B, 0x1B);
const QColor kVideoTrack(0x2B, 0x30, 0x3A);
const QColor kAudioTrack(0x1F, 0x36, 0x2E);
const QColor kRulerBg(0x21, 0x21, 0x21);
const QColor kClipFill(0x37, 0x4B, 0x5A);
const QColor kClipSelected(0x00, 0xA8, 0xFF);
} // namespace

TimelinePanel::TimelinePanel(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addStretch(1);

    zoom_ = new QSlider(Qt::Horizontal, this);
    zoom_->setRange(5, 400);
    zoom_->setValue(static_cast<int>(pps_));
    zoom_->setFixedHeight(kZoomBarHeight - 6);
    connect(zoom_, &QSlider::valueChanged, this, [this](int value) {
        pps_ = static_cast<double>(value);
        update();
    });

    auto *zoomRow = new QWidget(this);
    zoomRow->setFixedHeight(kZoomBarHeight);
    auto *zoomLayout = new QHBoxLayout(zoomRow);
    zoomLayout->setContentsMargins(kHeaderWidth + 8, 0, 12, 0);
    auto *zoomLabel = new QLabel(tr("Zoom"), zoomRow);
    zoomLayout->addWidget(zoomLabel);
    zoomLayout->addWidget(zoom_, 1);
    layout->addWidget(zoomRow);
}

int TimelinePanel::areaHeight() const {
    return height() - kZoomBarHeight;
}

QRect TimelinePanel::laneRect(int row) const {
    return QRect(kHeaderWidth, kRulerHeight + row * kTrackHeight,
                 std::max(0, width() - kHeaderWidth), kTrackHeight);
}

int64_t TimelinePanel::xToFrame(int x) const {
    return static_cast<int64_t>(std::max(0.0, (x - kHeaderWidth) / pps_));
}

int TimelinePanel::frameToX(int64_t frame) const {
    return kHeaderWidth + static_cast<int>(frame * pps_);
}

int TimelinePanel::trackRowAt(int y) const {
    if (y < kRulerHeight || y >= areaHeight()) {
        return -1;
    }
    return (y - kRulerHeight) / kTrackHeight;
}

QColor TimelinePanel::trackColor(int index) const {
    static const QColor palette[] = {
        QColor(0x00, 0xA8, 0xFF), QColor(0xFF, 0xB0, 0x20), QColor(0x9B, 0x59, 0xD0),
        QColor(0x2E, 0xCC, 0x71), QColor(0xE7, 0x4C, 0x3C), QColor(0x1A, 0xBC, 0x9C),
    };
    return palette[index % 6];
}

void TimelinePanel::setModel(const fc::TimelineModel *model) {
    model_ = model;
    update();
}

void TimelinePanel::setSequenceDuration(double seconds) {
    duration_ = seconds > 0.0 ? seconds : 10.0;
    update();
}

void TimelinePanel::setPlayhead(double seconds) {
    playhead_ = std::max(0.0, seconds);
    update();
}

void TimelinePanel::setFps(double fps) {
    fps_ = fps > 1.0 ? fps : 24.0;
    update();
}

void TimelinePanel::setRazorMode(bool on) {
    razorMode_ = on;
    setCursor(on ? Qt::CrossCursor : Qt::ArrowCursor);
    update();
}

void TimelinePanel::clearSelection() {
    selectedClipId_ = -1;
    update();
}

void TimelinePanel::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), kPanelBg);

    drawHeaderColumn(painter);
    drawRuler(painter);
    drawClips(painter);

    const int x = frameToX(static_cast<int64_t>(playhead_ * fps_));
    if (x >= kHeaderWidth && x <= width()) {
        painter.setPen(QPen(QColor(0xE8, 0xE8, 0xE8), 1));
        painter.drawLine(x, 0, x, areaHeight());
        painter.setBrush(QColor(0xE8, 0xE8, 0xE8));
        painter.setPen(Qt::NoPen);
        painter.drawPolygon(QPolygon() << QPoint(x - 5, 0) << QPoint(x + 5, 0) << QPoint(x, 8));
    }
}

void TimelinePanel::drawHeaderColumn(QPainter &painter) const {
    const QRect headerArea(0, kRulerHeight, kHeaderWidth, areaHeight() - kRulerHeight);
    painter.fillRect(headerArea, QColor(0x18, 0x18, 0x18));
    painter.setPen(QColor(0x3A, 0x3A, 0x3A));
    painter.drawLine(headerArea.right(), headerArea.top(), headerArea.right(), headerArea.bottom());

    const int trackCount = model_ ? model_->trackCount() : 0;
    for (int r = 0; r < trackCount; ++r) {
        const QRect cell(0, kRulerHeight + r * kTrackHeight, kHeaderWidth, kTrackHeight);
        if (cell.top() >= areaHeight()) {
            break;
        }
        const fc::Track *track = model_->trackAt(r);
        if (!track) {
            continue;
        }
        const QColor accent = trackColor(r);
        painter.fillRect(0, cell.top(), 3, kTrackHeight, accent);

        QFont bold = painter.font();
        bold.setBold(true);
        painter.setFont(bold);
        painter.setPen(accent);
        painter.drawText(QRect(10, cell.top() + 4, 40, 18), Qt::AlignLeft,
                         QString::fromStdString(track->name));
        painter.setFont(QFont());

        const char *labels[3] = {"L", "M", "S"};
        const bool states[3] = {track->locked, track->muted, track->solo};
        for (int c = 0; c < 3; ++c) {
            const QRect box(10 + c * 26, cell.top() + 22, 22, 16);
            painter.setPen(QColor(0x55, 0x55, 0x55));
            painter.setBrush(states[c] ? accent : QColor(0x24, 0x24, 0x24));
            painter.drawRect(box);
            painter.setPen(states[c] ? QColor(0x10, 0x10, 0x10) : QColor(0x99, 0x99, 0x99));
            painter.drawText(box, Qt::AlignCenter, QString::fromLatin1(labels[c]));
        }
    }
}

void TimelinePanel::drawRuler(QPainter &painter) const {
    const QRect rulerRect(kHeaderWidth, 0, std::max(0, width() - kHeaderWidth), kRulerHeight);
    painter.fillRect(rulerRect, kRulerBg);
    painter.setPen(QColor(0x9A, 0x9A, 0x9A));

    double step = 1.0;
    while (step * pps_ < 70.0) {
        step *= 5.0;
    }
    const fc::FrameRate rate{static_cast<uint32_t>(std::lround(fps_ * 1000.0)), 1000, false};
    for (double t = 0.0; t <= duration_; t += step) {
        const int x = frameToX(static_cast<int64_t>(std::llround(t * fps_)));
        if (x < rulerRect.left() || x > rulerRect.right()) {
            continue;
        }
        painter.drawLine(x, rulerRect.bottom() - 8, x, rulerRect.bottom());
        const int64_t frames = static_cast<int64_t>(std::llround(t * fps_));
        painter.drawText(QRect(x + 3, rulerRect.top(), 96, rulerRect.height()),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QString::fromStdString(fc::Timecode::fromFrames(frames, rate).toString()));
    }
}

void TimelinePanel::drawClips(QPainter &painter) const {
    if (!model_) {
        return;
    }
    painter.setRenderHint(QPainter::Antialiasing, false);
    for (const fc::Clip &clip : model_->clips()) {
        const fc::Track *track = model_->trackAt(clip.trackIndex);
        if (!track) {
            continue;
        }
        const QRect lane = laneRect(clip.trackIndex);
        const int x0 = frameToX(clip.timelineStart);
        const int x1 = frameToX(clip.timelineEnd());
        const QRect rect(x0, lane.top() + 4, std::max(8, x1 - x0), lane.height() - 8);
        const bool selected = clip.id == selectedClipId_;
        painter.setBrush(selected ? kClipSelected : kClipFill);
        painter.setPen(selected ? QColor(0xFF, 0xFF, 0xFF) : QColor(0x55, 0x66, 0x77));
        painter.drawRoundedRect(rect, 4, 4);
        painter.setPen(selected ? QColor(0x10, 0x10, 0x10) : QColor(0xE8, 0xE8, 0xE8));
        painter.drawText(rect.adjusted(6, 0, -6, 0), Qt::AlignLeft | Qt::AlignVCenter,
                         QString::fromStdString(clip.label));
    }
}

void TimelinePanel::mousePressEvent(QMouseEvent *event) {
    const int x = event->pos().x();
    const int y = event->pos().y();
    const int row = trackRowAt(y);

    if (row >= 0 && x >= 0 && x < kHeaderWidth) {
        return; // header cell toggling ships in M4b
    }

    if (x < kHeaderWidth || row < 0 || y > areaHeight()) {
        return;
    }

    const int64_t frame = xToFrame(x);
    if (razorMode_) {
        emit splitRequested(row, frame);
        return;
    }

    if (model_) {
        const fc::Clip *clip = model_->clipAt(frame, row);
        selectedClipId_ = clip ? clip->id : -1;
        emit clipSelected(selectedClipId_);
        update();
    }

    playhead_ = static_cast<double>(frame) / fps_;
    update();
    emit playheadMoved(playhead_);
}

void TimelinePanel::mouseMoveEvent(QMouseEvent *event) {
    if ((event->buttons() & Qt::LeftButton) && event->pos().x() > kHeaderWidth &&
        event->pos().y() <= areaHeight() && !razorMode_) {
        playhead_ = static_cast<double>(xToFrame(event->pos().x())) / fps_;
        update();
        emit playheadMoved(playhead_);
    }
}

void TimelinePanel::wheelEvent(QWheelEvent *event) {
    if (event->modifiers() & Qt::ControlModifier) {
        const int delta = event->angleDelta().y() > 0 ? 20 : -20;
        pps_ = std::min(400.0, std::max(5.0, pps_ + delta));
        zoom_->blockSignals(true);
        zoom_->setValue(static_cast<int>(pps_));
        zoom_->blockSignals(false);
        update();
        event->accept();
        return;
    }
    QWidget::wheelEvent(event);
}
