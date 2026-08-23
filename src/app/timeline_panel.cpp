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

// Colorblind-friendly track accents (Module 10.4).
QColor trackColor(int index) {
    static const QColor palette[] = {
        QColor(0x00, 0xA8, 0xFF), QColor(0xFF, 0xB0, 0x20), QColor(0x9B, 0x59, 0xD0),
        QColor(0x2E, 0xCC, 0x71), QColor(0xE7, 0x4C, 0x3C), QColor(0x1A, 0xBC, 0x9C),
    };
    return palette[index % 6];
}
} // namespace

TimelinePanel::TimelinePanel(QWidget *parent) : QWidget(parent) {
    rows_ = {{QStringLiteral("V2"), false, false, false, false},
             {QStringLiteral("V1"), false, false, false, false},
             {QStringLiteral("A1"), true, false, false, false}};

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addStretch(1); // painted area occupies everything above the bar

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

double TimelinePanel::xToSeconds(int x) const {
    return std::max(0.0, (x - kHeaderWidth) / pps_);
}

int TimelinePanel::secondsToX(double seconds) const {
    return kHeaderWidth + static_cast<int>(seconds * pps_);
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

void TimelinePanel::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), kPanelBg);

    drawHeaderColumn(painter);
    drawRuler(painter);

    for (int r = 0; r < rows_.size(); ++r) {
        const QRect lane = laneRect(r);
        if (lane.top() >= areaHeight()) {
            break;
        }
        painter.fillRect(lane, rows_[r].isAudio ? kAudioTrack : kVideoTrack);
        painter.setPen(QColor(0x3A, 0x3A, 0x3A));
        painter.drawLine(lane.left(), lane.top(), lane.right(), lane.top());
        painter.fillRect(kHeaderWidth, lane.top(), 3, kTrackHeight, trackColor(r));
    }

    // Playhead.
    const int x = secondsToX(playhead_);
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

    for (int r = 0; r < rows_.size(); ++r) {
        const QRect cell(0, kRulerHeight + r * kTrackHeight, kHeaderWidth, kTrackHeight);
        if (cell.top() >= areaHeight()) {
            break;
        }
        const QColor accent = trackColor(r);
        painter.fillRect(0, cell.top(), 3, kTrackHeight, accent);

        QFont bold = painter.font();
        bold.setBold(true);
        painter.setFont(bold);
        painter.setPen(accent);
        painter.drawText(QRect(10, cell.top() + 4, 40, 18), Qt::AlignLeft, rows_[r].name);
        painter.setFont(QFont()); // reset

        // L / M / S state cells.
        const char *labels[3] = {"L", "M", "S"};
        const bool states[3] = {rows_[r].locked, rows_[r].muted, rows_[r].solo};
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

    double step = 1.0; // major tick: 1s / 5s / 30s by zoom
    while (step * pps_ < 70.0) {
        step *= 5.0;
    }
    const fc::FrameRate rate{static_cast<uint32_t>(std::lround(fps_ * 1000.0)), 1000, false};
    for (double t = 0.0; t <= duration_; t += step) {
        const int x = secondsToX(t);
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

void TimelinePanel::mousePressEvent(QMouseEvent *event) {
    const int x = event->pos().x();
    const int y = event->pos().y();

    // Header cells toggle lock/mute/solo.
    if (x >= 0 && x < kHeaderWidth && y > kRulerHeight) {
        const int row = (y - kRulerHeight) / kTrackHeight;
        if (row >= 0 && row < rows_.size()) {
            const int cellX = (x - 10) / 26;
            if (cellX >= 0 && cellX < 3) {
                bool *state = cellX == 0   ? &rows_[row].locked
                              : cellX == 1 ? &rows_[row].muted
                                           : &rows_[row].solo;
                *state = !*state;
                update();
                return;
            }
        }
        return;
    }

    if (x > kHeaderWidth && y <= areaHeight()) {
        playhead_ = xToSeconds(x);
        update();
        emit playheadMoved(playhead_);
    }
}

void TimelinePanel::mouseMoveEvent(QMouseEvent *event) {
    if ((event->buttons() & Qt::LeftButton) && event->pos().x() > kHeaderWidth &&
        event->pos().y() <= areaHeight()) {
        playhead_ = xToSeconds(event->pos().x());
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
