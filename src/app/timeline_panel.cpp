#include "timeline_panel.h"

#include <QEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSlider>
#include <QToolButton>
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
constexpr int kToolRowHeight = 30;
constexpr int kEdgeGrabPx = 8; // edge-trim / roll grab zone

const QColor kPanelBg(0x1B, 0x1B, 0x1B);
const QColor kVideoTrack(0x2B, 0x30, 0x3A);
const QColor kAudioTrack(0x1F, 0x36, 0x2E);
const QColor kRulerBg(0x21, 0x21, 0x21);
const QColor kClipFill(0x37, 0x4B, 0x5A);
const QColor kClipSelected(0x00, 0xA8, 0xFF);
const QColor kGhostFill(0x6E, 0x9B, 0xB8);
const QColor kGhostBad(0xB0, 0x3A, 0x2E);
} // namespace

TimelinePanel::TimelinePanel(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // ---- M4b tool row: [Select] [Razor] | [Ripple] -----------------------
    auto *toolRow = new QWidget(this);
    toolRow->setFixedHeight(kToolRowHeight);
    auto *tools = new QHBoxLayout(toolRow);
    tools->setContentsMargins(kHeaderWidth + 8, 2, 12, 2);
    tools->setSpacing(4);

    selectTool_ = new QToolButton(toolRow);
    selectTool_->setText(tr("Select"));
    selectTool_->setToolTip(tr("Select / move / trim tool"));
    selectTool_->setCheckable(true);
    selectTool_->setChecked(true);
    selectTool_->setShortcut(QKeySequence(Qt::Key_V));

    razorTool_ = new QToolButton(toolRow);
    razorTool_->setText(tr("Razor"));
    razorTool_->setToolTip(tr("Razor tool: click a clip to split it at that frame"));
    razorTool_->setCheckable(true);
    razorTool_->setShortcut(QKeySequence(Qt::Key_C));

    rippleTool_ = new QToolButton(toolRow);
    rippleTool_->setText(tr("Ripple"));
    rippleTool_->setToolTip(tr("Ripple edits: delete and tail-trim close the gap "
                               "instead of leaving a hole"));
    rippleTool_->setCheckable(true);

    tools->addWidget(selectTool_);
    tools->addWidget(razorTool_);
    tools->addSpacing(12);
    tools->addWidget(rippleTool_);
    tools->addStretch(1);
    layout->addWidget(toolRow);

    // Razor/select buttons drive the single shared razorMode_ flag; the
    // toggled handlers below only act on the "checked" edge, so the
    // syncToolButtons() feedback (unchecking the sibling) cannot loop.
    connect(razorTool_, &QToolButton::toggled, this, [this](bool on) {
        if (on) {
            setRazorMode(true);
        }
    });
    connect(selectTool_, &QToolButton::toggled, this, [this](bool on) {
        if (on) {
            setRazorMode(false);
        }
    });
    connect(rippleTool_, &QToolButton::toggled, this, [this](bool on) { rippleEnabled_ = on; });

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

// Content area geometry: tool row (top) + ruler + lanes + zoom row
// (bottom). The paint code addresses the lanes directly, so every
// helper is offset by the tool row height.
int TimelinePanel::contentTop() const {
    return kToolRowHeight;
}

int TimelinePanel::areaHeight() const {
    return height() - kZoomBarHeight - kToolRowHeight;
}

QRect TimelinePanel::laneRect(int row) const {
    return QRect(kHeaderWidth, contentTop() + kRulerHeight + row * kTrackHeight,
                 std::max(0, width() - kHeaderWidth), kTrackHeight);
}

int64_t TimelinePanel::xToFrame(int x) const {
    return static_cast<int64_t>(std::max(0.0, (x - kHeaderWidth) / pps_));
}

int TimelinePanel::frameToX(int64_t frame) const {
    return kHeaderWidth + static_cast<int>(frame * pps_);
}

int TimelinePanel::trackRowAt(int y) const {
    const int laneTop = contentTop() + kRulerHeight;
    if (y < laneTop || y >= areaHeight() + contentTop()) {
        return -1;
    }
    return (y - laneTop) / kTrackHeight;
}

QColor TimelinePanel::trackColor(int index) const {
    static const QColor palette[] = {
        QColor(0x00, 0xA8, 0xFF), QColor(0xFF, 0xB0, 0x20), QColor(0x9B, 0x59, 0xD0),
        QColor(0x2E, 0xCC, 0x71), QColor(0xE7, 0x4C, 0x3C), QColor(0x1A, 0xBC, 0x9C),
    };
    return palette[index % 6];
}

double TimelinePanel::trackDimFactor(int index) const {
    if (!model_) {
        return 1.0;
    }
    const fc::Track *track = model_->trackAt(index);
    if (!track) {
        return 1.0;
    }
    if (track->locked) {
        return 0.45;
    }
    // Solo logic: when any audio track is soloed, every other audio
    // track dims (video lanes are unaffected).
    bool anySolo = false;
    for (const fc::Track &t : model_->tracks()) {
        if (t.isAudio && t.solo) {
            anySolo = true;
            break;
        }
    }
    if (anySolo && track->isAudio && !track->solo) {
        return 0.45;
    }
    if (track->muted) {
        return 0.55;
    }
    return 1.0;
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
    syncToolButtons();
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
    drawDragGhost(painter);
    drawRazorHover(painter);

    const int x = frameToX(static_cast<int64_t>(playhead_ * fps_));
    if (x >= kHeaderWidth && x <= width()) {
        painter.setPen(QPen(QColor(0xE8, 0xE8, 0xE8), 1));
        painter.drawLine(x, contentTop(), x, areaHeight() + contentTop());
        painter.setBrush(QColor(0xE8, 0xE8, 0xE8));
        painter.setPen(Qt::NoPen);
        painter.drawPolygon(QPolygon() << QPoint(x - 5, contentTop()) << QPoint(x + 5, contentTop())
                                       << QPoint(x, contentTop() + 8));
    }
}

void TimelinePanel::drawHeaderColumn(QPainter &painter) const {
    const QRect headerArea(0, contentTop() + kRulerHeight, kHeaderWidth,
                           areaHeight() - kRulerHeight);
    painter.fillRect(headerArea, QColor(0x18, 0x18, 0x18));
    painter.setPen(QColor(0x3A, 0x3A, 0x3A));
    painter.drawLine(headerArea.right(), headerArea.top(), headerArea.right(), headerArea.bottom());

    const int trackCount = model_ ? model_->trackCount() : 0;
    for (int r = 0; r < trackCount; ++r) {
        const QRect cell(0, contentTop() + kRulerHeight + r * kTrackHeight, kHeaderWidth,
                         kTrackHeight);
        if (cell.top() >= areaHeight() + contentTop()) {
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
    const QRect rulerRect(kHeaderWidth, contentTop(), std::max(0, width() - kHeaderWidth),
                          kRulerHeight);
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

        QColor fill = selected ? kClipSelected : kClipFill;
        QColor border = selected ? QColor(0xFF, 0xFF, 0xFF) : QColor(0x55, 0x66, 0x77);
        QColor text = selected ? QColor(0x10, 0x10, 0x10) : QColor(0xE8, 0xE8, 0xE8);
        // Locked / muted / non-solo lanes render dimmed.
        const double dim = trackDimFactor(clip.trackIndex);
        if (dim < 1.0) {
            fill = QColor(fill.red(), fill.green(), fill.blue(), int(fill.alpha() * dim));
            border = QColor(border.red(), border.green(), border.blue(), int(border.alpha() * dim));
            text = QColor(text.red(), text.green(), text.blue(), int(text.alpha() * dim));
        }
        // Edge affordance: thin lighter bars where a trim/roll drag can
        // start, only on editable lanes.
        if (!track->locked) {
            QColor edge(0x9F, 0xC7, 0xE0, int(200 * dim));
            painter.fillRect(rect.left(), rect.top(), 3, rect.height(), edge);
            painter.fillRect(rect.right() - 3, rect.top(), 3, rect.height(), edge);
        }
        painter.setBrush(fill);
        painter.setPen(border);
        painter.drawRoundedRect(rect, 4, 4);
        painter.setPen(text);
        painter.drawText(rect.adjusted(6, 0, -6, 0), Qt::AlignLeft | Qt::AlignVCenter,
                         QString::fromStdString(clip.label));
    }
}

void TimelinePanel::drawDragGhost(QPainter &painter) const {
    if (dragMode_ == DragMode::None || !model_) {
        return;
    }
    const fc::Clip *clip = model_->clipById(dragClipId_);
    if (!clip) {
        return;
    }
    if (dragMode_ == DragMode::Move) {
        if (ghostRow_ < 0) {
            return;
        }
        const QRect lane = laneRect(ghostRow_);
        const int64_t dur = clip->durationFrames();
        const int x0 = frameToX(ghostStart_);
        const int x1 = frameToX(ghostStart_ + std::max<int64_t>(1, dur));
        const QRect rect(x0, lane.top() + 4, std::max(8, x1 - x0), lane.height() - 8);
        // Ghost: translucent; blue-grey when the drop is legal, red when
        // the current lane/spot cannot host the clip.
        const QColor &c = ghostValid_ ? kGhostFill : kGhostBad;
        painter.setBrush(QColor(c.red(), c.green(), c.blue(), 140));
        painter.setPen(QColor(c.red(), c.green(), c.blue(), 220));
        painter.drawRoundedRect(rect, 4, 4);
        return;
    }
    if (dragMode_ == DragMode::TrimStart || dragMode_ == DragMode::TrimEnd) {
        // Preview of the new in/out edge position.
        const QRect lane = laneRect(clip->trackIndex);
        const int64_t start = dragMode_ == DragMode::TrimStart ? ghostStart_ : clip->timelineStart;
        const int64_t end = dragMode_ == DragMode::TrimEnd ? ghostEnd_ : clip->timelineEnd();
        const int x0 = frameToX(start);
        const int x1 = frameToX(end);
        const QRect rect(x0, lane.top() + 2, std::max(8, x1 - x0), lane.height() - 4);
        painter.setBrush(QColor(0x9F, 0xC7, 0xE0, 60));
        painter.setPen(QColor(0x9F, 0xC7, 0xE0, 200));
        painter.drawRect(rect);
        return;
    }
    if (dragMode_ == DragMode::RollBoundary) {
        // Boundary marker line between the two clips being rolled.
        const int x = frameToX(ghostEnd_);
        const QRect lane = laneRect(clip->trackIndex);
        painter.setPen(QPen(QColor(0xFF, 0xB0, 0x20), 2));
        painter.drawLine(x, lane.top() + 2, x, lane.bottom() - 2);
    }
}

void TimelinePanel::drawRazorHover(QPainter &painter) const {
    if (razorHoverX_ < kHeaderWidth || razorHoverX_ > width()) {
        return;
    }
    QPen pen(QColor(0xE0, 0xE0, 0xE0), 1, Qt::DashLine);
    painter.setPen(pen);
    painter.drawLine(razorHoverX_, contentTop() + kRulerHeight, razorHoverX_,
                     areaHeight() + contentTop());
}

const fc::Clip *TimelinePanel::clipAtPos(const QPoint &pos, int *rowOut) const {
    if (!model_) {
        return nullptr;
    }
    const int row = trackRowAt(pos.y());
    if (rowOut) {
        *rowOut = row;
    }
    if (row < 0 || pos.x() < kHeaderWidth) {
        return nullptr;
    }
    return model_->clipAt(xToFrame(pos.x()), row);
}

const fc::Clip *TimelinePanel::rightNeighborOf(const fc::Clip *clip) const {
    if (!model_ || !clip) {
        return nullptr;
    }
    const fc::Clip *best = nullptr;
    for (const fc::Clip &other : model_->clips()) {
        if (other.trackIndex != clip->trackIndex || other.id == clip->id) {
            continue;
        }
        if (other.timelineStart == clip->timelineEnd() &&
            (!best || other.timelineStart < best->timelineStart)) {
            best = &other;
        }
    }
    return best;
}

void TimelinePanel::beginClipDrag(const QPoint &pos) {
    const fc::Clip *clip = clipAtPos(pos, &dragRow_);
    if (!clip || !model_) {
        resetDrag();
        return;
    }
    dragClipId_ = clip->id;
    dragOriginStart_ = clip->timelineStart;
    dragOriginEnd_ = clip->timelineEnd();
    ghostStart_ = clip->timelineStart;
    ghostEnd_ = clip->timelineEnd();
    const int clipX0 = frameToX(clip->timelineStart);
    const int clipX1 = frameToX(clip->timelineEnd());

    // Alt + edge press where two clips touch => rolling edit on that
    // boundary; plain edge press => trim; body press => move.
    const bool altHeld = QGuiApplication::queryKeyboardModifiers() & Qt::AltModifier;
    if (pos.x() - clipX0 <= kEdgeGrabPx && clipX1 - pos.x() > kEdgeGrabPx) {
        const fc::Clip *left = nullptr;
        // Left edge of THIS clip: it is the right side of the pair.
        for (const fc::Clip &other : model_->clips()) {
            if (other.trackIndex == clip->trackIndex && other.id != clip->id &&
                other.timelineEnd() == clip->timelineStart) {
                left = &other;
                break;
            }
        }
        if (altHeld && left) {
            dragMode_ = DragMode::RollBoundary;
            dragBoundary_ = clip->timelineStart;
            dragRollLeftId_ = left->id;
            dragRollRightId_ = clip->id;
            dragRollMin_ = left->timelineStart + 1;
            dragRollMax_ = clip->timelineEnd() - 1;
            ghostEnd_ = dragBoundary_;
        } else {
            dragMode_ = DragMode::TrimStart;
            ghostStart_ = clip->timelineStart;
        }
    } else if (clipX1 - pos.x() <= kEdgeGrabPx) {
        const fc::Clip *right = rightNeighborOf(clip);
        if (altHeld && right) {
            dragMode_ = DragMode::RollBoundary;
            dragBoundary_ = clip->timelineEnd();
            dragRollLeftId_ = clip->id;
            dragRollRightId_ = right->id;
            dragRollMin_ = clip->timelineStart + 1;
            dragRollMax_ = right->timelineEnd() - 1;
            ghostEnd_ = dragBoundary_;
        } else {
            dragMode_ = DragMode::TrimEnd;
            ghostEnd_ = clip->timelineEnd();
        }
    } else {
        dragMode_ = DragMode::Move;
        dragGrabOffset_ = xToFrame(pos.x()) - clip->timelineStart;
        ghostRow_ = dragRow_;
        ghostValid_ = true;
    }
}

void TimelinePanel::resetDrag() {
    dragMode_ = DragMode::None;
    dragClipId_ = -1;
    dragRollLeftId_ = -1;
    dragRollRightId_ = -1;
    dragRow_ = -1;
    ghostRow_ = -1;
    ghostStart_ = 0;
    ghostEnd_ = 0;
    ghostValid_ = false;
    update();
}

void TimelinePanel::mousePressEvent(QMouseEvent *event) {
    const int x = event->pos().x();
    const int y = event->pos().y();
    const int row = trackRowAt(y);

    // Header column: L/M/S toggles (M4b).
    if (row >= 0 && x >= 0 && x < kHeaderWidth && event->button() == Qt::LeftButton) {
        const int relY = y - (contentTop() + kRulerHeight + row * kTrackHeight);
        if (relY >= 22) { // the L/M/S boxes live in the lower half of the cell
            const int box = (x - 10) / 26;
            if (box >= 0 && box < 3 && x >= 10 && x <= 10 + 3 * 26 - 4) {
                emit trackStateToggleRequested(row, box);
                return;
            }
        }
        return;
    }

    if (x < kHeaderWidth || row < 0 || y > areaHeight() + contentTop()) {
        resetDrag();
        return;
    }

    const int64_t frame = xToFrame(x);
    if (razorMode_) {
        emit splitRequested(row, frame);
        return;
    }

    if (event->button() == Qt::LeftButton && model_) {
        beginClipDrag(event->pos());
        if (dragClipId_ > 0) {
            selectedClipId_ = dragClipId_;
            emit clipSelected(selectedClipId_);
            update();
            return;
        }
        // Empty space: fall through to playhead scrub.
        resetDrag();
    }

    playhead_ = static_cast<double>(frame) / fps_;
    update();
    emit playheadMoved(playhead_);
}

void TimelinePanel::mouseMoveEvent(QMouseEvent *event) {
    const int x = event->pos().x();

    // Razor hover preview line.
    if (razorMode_ && !(event->buttons() & Qt::LeftButton)) {
        razorHoverX_ = x;
        update();
        return;
    }

    if (dragMode_ != DragMode::None && model_) {
        const fc::Clip *clip = model_->clipById(dragClipId_);
        if (!clip) {
            resetDrag();
            return;
        }
        const int64_t frame = xToFrame(x);
        const int row = trackRowAt(event->pos().y());
        switch (dragMode_) {
        case DragMode::Move: {
            const int64_t desired = frame - dragGrabOffset_;
            const int64_t safeDesired = desired < 0 ? 0 : desired;
            const int targetRow = row >= 0 ? row : dragRow_;
            const int64_t snapped = model_->findDropPosition(targetRow, dragClipId_, safeDesired,
                                                             clip->durationFrames());
            ghostRow_ = targetRow;
            if (snapped >= 0) {
                ghostStart_ = snapped;
                ghostValid_ = true;
            } else {
                ghostStart_ = safeDesired;
                ghostValid_ = false;
            }
            break;
        }
        case DragMode::TrimStart:
            // Head may not pass the clip's tail (keep >= 1 frame) nor go
            // before the timeline start.
            ghostStart_ = std::min(std::max<int64_t>(0, frame), dragOriginEnd_ - 1);
            break;
        case DragMode::TrimEnd:
            ghostEnd_ = std::max(dragOriginStart_ + 1, frame);
            break;
        case DragMode::RollBoundary:
            // The boundary stays inside both clips.
            ghostEnd_ = std::min(std::max(frame, dragRollMin_), dragRollMax_);
            break;
        default:
            break;
        }
        update();
        return;
    }

    // Playhead scrub (M4a behavior).
    if ((event->buttons() & Qt::LeftButton) && x > kHeaderWidth &&
        event->pos().y() <= areaHeight() + contentTop() && !razorMode_) {
        playhead_ = static_cast<double>(xToFrame(x)) / fps_;
        update();
        emit playheadMoved(playhead_);
        return;
    }

    // Hover cursor affordance over clip edges.
    if (!(event->buttons() & Qt::LeftButton) && !razorMode_ && model_) {
        const fc::Clip *clip = clipAtPos(event->pos());
        if (clip) {
            const int x0 = frameToX(clip->timelineStart);
            const int x1 = frameToX(clip->timelineEnd());
            const bool nearEdge = (x - x0 <= kEdgeGrabPx) || (x1 - x <= kEdgeGrabPx);
            const fc::Track *track = model_->trackAt(clip->trackIndex);
            setCursor(track && track->locked ? Qt::ForbiddenCursor
                                             : (nearEdge ? Qt::SizeHorCursor : Qt::ArrowCursor));
        } else {
            setCursor(Qt::ArrowCursor);
        }
    }
}

void TimelinePanel::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton || dragMode_ == DragMode::None || !model_) {
        resetDrag();
        return;
    }
    const fc::Clip *clip = model_->clipById(dragClipId_);
    const fc::Track *track = clip ? model_->trackAt(clip->trackIndex) : nullptr;

    switch (dragMode_) {
    case DragMode::Move:
        // The ghost row may be a different (same-kind, valid) lane; the
        // lock check here mirrors MainWindow's safety net.
        if (ghostRow_ >= 0 && model_->trackAt(ghostRow_)) {
            const fc::Track *target = model_->trackAt(ghostRow_);
            if (target && !target->locked && !(track && track->locked)) {
                emit clipMoveRequested(dragClipId_, ghostRow_, ghostStart_);
            }
        }
        break;
    case DragMode::TrimStart: {
        if (track && !track->locked) {
            const int64_t delta = ghostStart_ - dragOriginStart_;
            if (delta != 0) {
                emit clipTrimRequested(dragClipId_, 0, delta);
            }
        }
        break;
    }
    case DragMode::TrimEnd: {
        if (track && !track->locked) {
            const int64_t delta = ghostEnd_ - dragOriginEnd_;
            if (delta != 0) {
                emit clipTrimRequested(dragClipId_, 1, delta);
            }
        }
        break;
    }
    case DragMode::RollBoundary: {
        const fc::Clip *leftClip = model_->clipById(dragRollLeftId_);
        const fc::Track *leftTrack = leftClip ? model_->trackAt(leftClip->trackIndex) : nullptr;
        const int64_t delta = ghostEnd_ - dragBoundary_;
        if (delta != 0 && leftTrack && !leftTrack->locked && !(track && track->locked)) {
            emit rollEditRequested(dragRollLeftId_, dragRollRightId_, delta);
        }
        break;
    }
    default:
        break;
    }
    resetDrag();
}

void TimelinePanel::leaveEvent(QEvent *event) {
    razorHoverX_ = -1;
    update();
    QWidget::leaveEvent(event);
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

void TimelinePanel::syncToolButtons() {
    if (razorTool_) {
        razorTool_->setChecked(razorMode_);
    }
    if (selectTool_) {
        selectTool_->setChecked(!razorMode_);
    }
    if (rippleTool_) {
        rippleTool_->setChecked(rippleEnabled_);
    }
}
