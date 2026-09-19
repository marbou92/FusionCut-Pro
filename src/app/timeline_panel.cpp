#include "timeline_panel.h"

#include <QAction>
#include <QEvent>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShortcut>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

#include "timecode.h"
#include "ui_theme.h"

// ui_theme.h tokens live in fc::ui; the timeline addresses them as
// ui::... throughout its paint code, so the file pulls in the fc
// namespace (its class names never collide with the local ones).
using namespace fc;

namespace {
constexpr int kHeaderWidth = 96;
constexpr int kRulerHeight = 26;
constexpr int kTrackHeight = 44;
constexpr int kZoomBarHeight = 30;
constexpr int kToolRowHeight = 30;
constexpr int kHScrollHeight = 14;
constexpr int kLanePad = 160;  // tail padding past the last frame (px)
constexpr int kEdgeGrabPx = 8; // edge-trim / roll grab zone
// Ruler label-step floor: each label draws into a 96 px rect anchored
// 3 px right of its tick, so two adjacent labels need at least
// 96 + 3 px between their ticks or the timecodes overlap.
constexpr double kRulerLabelMinPx = 99.0;

// Shared design tokens (ui_theme.h): the timeline backdrop and the
// selection accent come from the app palette; the track identity
// palette further down stays local to the timeline.
const QColor kPanelBg = ui::color(ui::kTimelineBg);
const QColor kVideoTrack(0x2B, 0x30, 0x3A);
const QColor kAudioTrack(0x1F, 0x36, 0x2E);
const QColor kTextTrack(0x32, 0x28, 0x3A);
const QColor kRulerBg(0x21, 0x21, 0x21);
const QColor kClipFill(0x37, 0x4B, 0x5A);
const QColor kTextClipFill(0x59, 0x3D, 0x70);
const QColor kClipSelected = ui::color(ui::kAccent);
const QColor kGhostFill(0x6E, 0x9B, 0xB8);
const QColor kGhostBad(0xB0, 0x3A, 0x2E);
} // namespace

TimelinePanel::TimelinePanel(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // ---- tool row: [Select] [Razor] | [Ripple] -----------------------
    auto *toolRow = new QWidget(this);
    toolRow->setFixedHeight(kToolRowHeight);
    auto *tools = new QHBoxLayout(toolRow);
    tools->setContentsMargins(kHeaderWidth + 8, 2, 12, 2);
    tools->setSpacing(4);

    selectTool_ = new QToolButton(toolRow);
    selectTool_->setText(tr("Select"));
    selectTool_->setToolTip(tr("Select / move / trim tool (V)"));
    selectTool_->setCheckable(true);
    selectTool_->setChecked(true);
    selectTool_->setShortcut(QKeySequence(Qt::Key_V));
    selectTool_->setAccessibleName(tr("Select tool"));

    razorTool_ = new QToolButton(toolRow);
    razorTool_->setText(tr("Razor"));
    razorTool_->setToolTip(tr("Razor tool: click a clip to split it at that frame (C)"));
    razorTool_->setCheckable(true);
    razorTool_->setShortcut(QKeySequence(Qt::Key_C));
    razorTool_->setAccessibleName(tr("Razor tool"));

    rippleTool_ = new QToolButton(toolRow);
    rippleTool_->setText(tr("Ripple"));
    rippleTool_->setToolTip(tr("Ripple edits: delete and tail-trim close the gap "
                               "instead of leaving a hole (R)"));
    rippleTool_->setCheckable(true);
    rippleTool_->setAccessibleName(tr("Ripple toggle"));
    // Tool keyboard switching (#15): V/C are the existing select/razor
    // bindings; R toggles ripple (plain R is free - Ctrl+R stays the
    // Speed / Duration dialog at window level).
    rippleTool_->setShortcut(QKeySequence(Qt::Key_R));

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

    // Horizontal lane scroll: the lanes + ruler scroll, the track
    // header column and the tool/zoom rows stay fixed.
    hscroll_ = new QScrollBar(Qt::Horizontal, this);
    hscroll_->setFixedHeight(kHScrollHeight);
    hscroll_->setRange(0, 0);
    hscroll_->setAccessibleName(tr("Horizontal timeline scroll"));
    connect(hscroll_, &QScrollBar::valueChanged, this, [this](int value) {
        scrollX_ = value;
        update();
        if (!scrollGuard_) {
            followPlayhead_ = false; // manual scroll disarms follow (#11)
        }
    });
    layout->addWidget(hscroll_);

    // Vertical lane scroll (#12): the track stack scrolls under the
    // frozen ruler row and header column. MANUAL child - layoutChrome()
    // hugs it to the right edge of the lanes area on every resize.
    vscroll_ = new QScrollBar(Qt::Vertical, this);
    vscroll_->setRange(0, 0);
    vscroll_->setAccessibleName(tr("Vertical timeline scroll"));
    connect(vscroll_, &QScrollBar::valueChanged, this, [this](int value) {
        scrollY_ = value;
        update();
        if (!scrollGuard_) {
            followPlayhead_ = false;
        }
    });

    // Drag auto-page timer (#11): repeats while a drag parks at a
    // viewport edge, scrolling that way and re-resolving the ghost.
    autoPageTimer_ = new QTimer(this);
    autoPageTimer_->setInterval(60);
    connect(autoPageTimer_, &QTimer::timeout, this, &TimelinePanel::autoPageStep);

    // Zoom keyboard (#10): = / - step around the playhead, \ fits the
    // sequence. WidgetWithChildrenShortcut keeps them local to the
    // timeline so typing elsewhere never zooms.
    auto *zoomInKey = new QShortcut(QKeySequence(Qt::Key_Equal), this);
    zoomInKey->setContext(Qt::WidgetWithChildrenShortcut);
    connect(zoomInKey, &QShortcut::activated, this, [this] { applyZoom(pps_ * 1.25); });
    auto *zoomOutKey = new QShortcut(QKeySequence(Qt::Key_Minus), this);
    zoomOutKey->setContext(Qt::WidgetWithChildrenShortcut);
    connect(zoomOutKey, &QShortcut::activated, this, [this] { applyZoom(pps_ / 1.25); });
    auto *fitKey = new QShortcut(QKeySequence(Qt::Key_Backslash), this);
    fitKey->setContext(Qt::WidgetWithChildrenShortcut);
    connect(fitKey, &QShortcut::activated, this, &TimelinePanel::fitToSequence);

    // The panel takes focus on click so the shortcuts above fire after
    // any timeline interaction.
    setFocusPolicy(Qt::StrongFocus);
    layoutChrome();

    zoom_ = new QSlider(Qt::Horizontal, this);
    zoom_->setRange(5, 400);
    zoom_->setValue(static_cast<int>(pps_));
    zoom_->setFixedHeight(kZoomBarHeight - 6);
    connect(zoom_, &QSlider::valueChanged, this,
            [this](int value) { applyZoom(static_cast<double>(value)); });

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
    return height() - kZoomBarHeight - kHScrollHeight - kToolRowHeight;
}

// The lanes viewport in WIDGET coordinates: right of the header column,
// below the ruler, left of the vertical scrollbar, above the horizontal
// scrollbar. Lane painting clips to this rect and the edge hit-tests
// (auto-page, wheel routing) use it too.
QRect TimelinePanel::lanesViewport() const {
    return QRect(kHeaderWidth, contentTop() + kRulerHeight,
                 std::max(0, width() - kHeaderWidth - kHScrollHeight),
                 std::max(0, areaHeight() - kRulerHeight));
}

// #12: the panel never shrinks below the tool row + ruler + two track
// rows + the scroll/zoom chrome. QDockWidget honors the inner widget's
// minimumSizeHint, so the bottom dock respects it.
QSize TimelinePanel::minimumSizeHint() const {
    return QSize(560, kToolRowHeight + kRulerHeight + 2 * kTrackHeight + kHScrollHeight +
                          kZoomBarHeight + 2);
}

QRect TimelinePanel::laneRect(int row) const {
    // Vertical scroll (#12): every lane row shifts up by scrollY_; the
    // ruler band and the header column chrome stay frozen.
    return QRect(kHeaderWidth, contentTop() + kRulerHeight + row * kTrackHeight - scrollY_,
                 laneContentWidth(), kTrackHeight);
}

int TimelinePanel::laneContentWidth() const {
    // The sequence extent at the current zoom plus tail padding, so the
    // ruler labels and the clip tails past the last frame stay reachable.
    // pps_ is pixels-per-FRAME (xToFrame/frameToX are frame-scaled), so
    // the extent is duration * fps * pps - the old duration * pps
    // computed the scroll range as if pps_ were pixels-per-second, which
    // kept the scrollbar dead at default zoom (10 s @ 24 fps paints
    // 14400 px but the range said 600) and clipped everything past the
    // first seconds of a project out of view.
    return static_cast<int>(duration_ * fps_ * pps_) + kLanePad;
}

int64_t TimelinePanel::xToFrame(int x) const {
    // x is a WIDGET coordinate; the content it points at sits scrollX_
    // pixels to the right.
    return static_cast<int64_t>(std::max(0.0, (x - kHeaderWidth + scrollX_) / pps_));
}

int TimelinePanel::frameToX(int64_t frame) const {
    return kHeaderWidth + static_cast<int>(frame * pps_);
}

int TimelinePanel::trackRowAt(int y) const {
    const int laneTop = contentTop() + kRulerHeight;
    if (y < laneTop || y >= areaHeight() + contentTop()) {
        return -1;
    }
    // Scroll-aware (#12): rows past the last track stay valid indices
    // ("empty space") - callers treat them as no-clip, as before.
    return (y - laneTop + scrollY_) / kTrackHeight;
}

void TimelinePanel::updateScrollRange() {
    if (!hscroll_) {
        return;
    }
    const int viewport = std::max(1, width() - kHeaderWidth - kHScrollHeight);
    const int maxScroll = std::max(0, laneContentWidth() - viewport);
    hscroll_->blockSignals(true);
    hscroll_->setRange(0, maxScroll);
    hscroll_->setPageStep(viewport);
    hscroll_->setSingleStep(40);
    scrollX_ = std::min(std::max(scrollX_, 0), maxScroll);
    hscroll_->setValue(scrollX_);
    hscroll_->blockSignals(false);

    // Vertical range (#12): the track stack height against the lanes
    // viewport. Zero range = everything fits, scrollbar dead, plain
    // wheel falls back to the legacy horizontal scroll.
    if (vscroll_) {
        const int lanesHeight = std::max(1, areaHeight() - kRulerHeight);
        const int vMax =
            std::max(0, (model_ ? model_->trackCount() : 0) * kTrackHeight - lanesHeight);
        vscroll_->blockSignals(true);
        vscroll_->setRange(0, vMax);
        vscroll_->setPageStep(std::max(1, lanesHeight - kTrackHeight));
        vscroll_->setSingleStep(std::max(8, kTrackHeight / 2));
        scrollY_ = std::min(std::max(scrollY_, 0), vMax);
        vscroll_->setValue(scrollY_);
        vscroll_->blockSignals(false);
    }
}

void TimelinePanel::ensurePlayheadVisible() {
    if (!hscroll_) {
        return;
    }
    const int playheadX = frameToX(static_cast<int64_t>(playhead_ * fps_)) - scrollX_; // widget x
    const int margin = 32;
    const int rightEdge = width() - kHScrollHeight;
    if (playheadX < kHeaderWidth + margin || playheadX > rightEdge - margin) {
        // Center the playhead in the lane viewport (clamped by the range).
        const int target = frameToX(static_cast<int64_t>(playhead_ * fps_)) - kHeaderWidth -
                           std::max(1, rightEdge - kHeaderWidth) / 2;
        scrollGuard_ = true; // programmatic follow must not disarm itself
        hscroll_->setValue(std::min(std::max(target, 0), hscroll_->maximum()));
        scrollGuard_ = false;
    }
}

// Playback vertical follow (#11): scroll the MINIMUM amount that brings
// the selected clip's lane back into the lanes viewport.
void TimelinePanel::ensureSelectedLaneVisible() {
    if (!model_ || !vscroll_) {
        return;
    }
    const fc::Clip *clip = model_->clipById(selectedClipId_);
    if (!clip) {
        return;
    }
    const QRect lane = laneRect(clip->trackIndex);
    const QRect vp = lanesViewport();
    if (lane.top() < vp.top()) {
        scrollY_ -= vp.top() - lane.top();
    } else if (lane.bottom() > vp.bottom()) {
        scrollY_ += lane.bottom() - vp.bottom();
    } else {
        return;
    }
    scrollGuard_ = true;
    vscroll_->setValue(std::min(std::max(scrollY_, 0), vscroll_->maximum()));
    scrollGuard_ = false;
}

void TimelinePanel::applyZoom(double pps) {
    applyZoom(pps, -1, -1); // legacy entry: anchor at the playhead
}

// Single zoom entry point with an anchor (#10): after pps_ changes, the
// content frame `anchorFrame` is placed back under the widget x
// `anchorX`. anchorX < kHeaderWidth anchors at the playhead instead
// (the slider and the = / - keys keep the legacy behavior).
void TimelinePanel::applyZoom(double pps, int anchorX, int64_t anchorFrame) {
    pps_ = std::min(400.0, std::max(5.0, pps));
    if (zoom_) {
        zoom_->blockSignals(true);
        zoom_->setValue(static_cast<int>(pps_));
        zoom_->blockSignals(false);
    }
    updateScrollRange();
    if (anchorX >= kHeaderWidth && anchorFrame >= 0) {
        // widget x of the anchor frame = kHeaderWidth + frame*pps - scrollX_
        const int target = static_cast<int>(std::llround(static_cast<double>(anchorFrame) * pps_)) +
                           kHeaderWidth - anchorX;
        scrollX_ = std::min(std::max(target, 0), hscroll_ ? hscroll_->maximum() : 0);
        if (hscroll_) {
            hscroll_->blockSignals(true);
            hscroll_->setValue(scrollX_);
            hscroll_->blockSignals(false);
        }
    } else {
        ensurePlayheadVisible(); // zoom around the playhead, not the left edge
    }
    update();
}

// Fit-to-sequence ("\", #10): the zoom level at which the whole
// sequence spans the lanes viewport, scrolled to the start.
void TimelinePanel::fitToSequence() {
    const int vpWidth = std::max(1, width() - kHeaderWidth - kHScrollHeight);
    const double frames = std::max(1.0, duration_ * fps_);
    applyZoom(vpWidth / frames, -1, -1);
    scrollX_ = 0;
    if (hscroll_) {
        hscroll_->blockSignals(true);
        hscroll_->setValue(0);
        hscroll_->blockSignals(false);
    }
    update();
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
    // The selection ids belonged to the previous model content: a
    // replacement model may not contain them (or may reuse the ids for
    // different clips), so the highlight must not survive the swap.
    selectedClipId_ = -1;
    selectedTransitionId_ = -1;
    updateScrollRange();
    update();
}

void TimelinePanel::setSequenceDuration(double seconds) {
    duration_ = seconds > 0.0 ? seconds : 10.0;
    updateScrollRange();
    update();
}

void TimelinePanel::setPlayhead(double seconds) {
    playhead_ = std::max(0.0, seconds);
    if (playbackActive_ && followPlayhead_) {
        ensurePlayheadVisible();     // playback auto-follows (#11)
        ensureSelectedLaneVisible(); // ...vertically too
    }
    update();
}

void TimelinePanel::setPlaybackActive(bool active) {
    playbackActive_ = active;
    if (active) {
        followPlayhead_ = true; // every playback start re-arms follow
        ensurePlayheadVisible();
        ensureSelectedLaneVisible();
    }
    update();
}

void TimelinePanel::setFps(double fps) {
    fps_ = fps > 1.0 ? fps : 24.0;
    // The content extent is fps-scaled (laneContentWidth) and the ruler
    // tick spacing is fps-scaled (drawRuler) - both need a re-range, and
    // the playhead pixel position moves with fps too.
    updateScrollRange();
    ensurePlayheadVisible();
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

void TimelinePanel::clearTransitionSelection() {
    if (selectedTransitionId_ != -1) {
        selectedTransitionId_ = -1;
        update();
    }
}

void TimelinePanel::selectTransition(int64_t transitionId) {
    if (!model_ || model_->transitionById(transitionId) == nullptr) {
        return; // stale or unknown id - keep the current state
    }
    selectedTransitionId_ = transitionId;
    selectedClipId_ = -1;
    emit transitionSelected(transitionId);
    update();
}

void TimelinePanel::selectClip(int64_t clipId) {
    if (!model_ || clipId <= 0 || model_->clipById(clipId) == nullptr) {
        return; // stale or unknown id - keep the current state
    }
    selectedClipId_ = clipId;
    selectedTransitionId_ = -1;
    emit clipSelected(clipId);
    update();
}

void TimelinePanel::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), kPanelBg);

    drawHeaderColumn(painter);

    const QRect rulerBand(kHeaderWidth, contentTop(),
                          std::max(0, width() - kHeaderWidth - kHScrollHeight), kRulerHeight);
    const QRect lanes = lanesViewport();

    // The RULER scrolls horizontally but never vertically (#12): the
    // frozen row the track stack slides under.
    painter.save();
    painter.setClipRect(rulerBand);
    painter.translate(-scrollX_, 0);
    drawRuler(painter);
    painter.restore();

    // The LANES scroll both ways; the clip rect keeps translated content
    // off the frozen ruler row, header column and scrollbars. x shifts
    // via translate; y is already baked into laneRect().
    painter.save();
    painter.setClipRect(lanes);
    painter.translate(-scrollX_, 0);
    drawClips(painter);
    drawTransitions(painter);
    drawDragGhost(painter);
    // Snap indicator (#14): a magnet engaged during a Move drag draws a
    // vertical accent line at the snapped position (Alt-suspended drags
    // keep ghostStart_ == dragRawStart_, so the line vanishes).
    if (dragMode_ == DragMode::Move && ghostStart_ != dragRawStart_) {
        const int sx = frameToX(ghostStart_);
        painter.setPen(QPen(ui::color(ui::kAccentBright), 1));
        painter.drawLine(sx, lanes.top(), sx, lanes.bottom());
        painter.fillRect(sx - 1, lanes.top(), 3, 3, ui::color(ui::kAccentBright));
    }
    painter.restore();

    // The PLAYHEAD spans ruler + lanes (never the header column).
    painter.save();
    painter.setClipRect(QRect(kHeaderWidth, contentTop(),
                              std::max(0, width() - kHeaderWidth - kHScrollHeight),
                              std::max(0, areaHeight())));
    painter.translate(-scrollX_, 0);
    const int x = frameToX(static_cast<int64_t>(playhead_ * fps_));
    if (x >= kHeaderWidth) {
        painter.setPen(QPen(ui::color(ui::kText), 1));
        painter.drawLine(x, contentTop(), x, areaHeight() + contentTop());
        painter.setBrush(ui::color(ui::kText));
        painter.setPen(Qt::NoPen);
        painter.drawPolygon(QPolygon() << QPoint(x - 5, contentTop()) << QPoint(x + 5, contentTop())
                                       << QPoint(x, contentTop() + 8));
    }
    painter.restore();

    // The razor hover line tracks the mouse in WIDGET coordinates.
    drawRazorHover(painter);
}

void TimelinePanel::drawHeaderColumn(QPainter &painter) const {
    const QRect headerArea(0, contentTop() + kRulerHeight, kHeaderWidth,
                           std::max(0, areaHeight() - kRulerHeight));
    painter.fillRect(headerArea, QColor(0x18, 0x18, 0x18));
    painter.setPen(ui::color(ui::kLine));
    painter.drawLine(headerArea.right(), headerArea.top(), headerArea.right(), headerArea.bottom());

    const int trackCount = model_ ? model_->trackCount() : 0;
    for (int r = 0; r < trackCount; ++r) {
        // The cell rides the SAME vertical offset as its lane (#12);
        // scrolled-away cells are simply skipped.
        const QRect cell(0, contentTop() + kRulerHeight + r * kTrackHeight - scrollY_, kHeaderWidth,
                         kTrackHeight);
        if (!cell.intersects(headerArea)) {
            continue;
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
    // Content coordinates: the ruler extends over the whole scrollable
    // extent (the viewport clip culls what is off-screen).
    const QRect rulerRect(kHeaderWidth, contentTop(), laneContentWidth(), kRulerHeight);
    painter.fillRect(rulerRect, kRulerBg);
    painter.setPen(QColor(0x9A, 0x9A, 0x9A));

    double step = 1.0;
    // One step is `step` seconds; its pixel distance is step * fps * pps
    // (pps_ is pixels-per-frame). Without the fps factor the ticks land
    // fps-times sparser than the 70 px target. The ladder exits at the
    // label spacing floor (not 70 px) so adjacent timecode labels can
    // never overlap: a 70-96 px step would leave the 96 px label rect
    // of one tick running into the text of the next.
    while (step * fps_ * pps_ < kRulerLabelMinPx) {
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

    // Minor ticks (#19): sub-label rhythm with no labels. Fifth of the
    // major step when that stays readable, else 1-second ticks when a
    // second is wide enough; never when neither clears 12 px.
    double minor = 0.0;
    if (step / 5.0 * fps_ * pps_ >= 12.0) {
        minor = step / 5.0;
    } else if (step > 1.0 && fps_ * pps_ >= 12.0) {
        minor = 1.0;
    }
    if (minor > 0.0) {
        painter.setPen(QColor(0x55, 0x55, 0x55));
        for (double t = 0.0; t <= duration_; t += minor) {
            const int mx = frameToX(static_cast<int64_t>(std::llround(t * fps_)));
            if (mx < rulerRect.left() || mx > rulerRect.right()) {
                continue;
            }
            painter.drawLine(mx, rulerRect.bottom() - 4, mx, rulerRect.bottom());
        }
    }
}

// The clip body rect in CONTENT x / shifted-widget y - the exact rect
// drawClips paints. Shared with the chip helpers so hit-tests match
// the paint byte-for-byte.
QRect TimelinePanel::clipBodyRect(const fc::Clip &clip, int row) const {
    const QRect lane = laneRect(row);
    const int x0 = frameToX(clip.timelineStart);
    const int x1 = frameToX(clip.timelineEnd());
    return QRect(x0, lane.top() + 4, std::max(8, x1 - x0), lane.height() - 8);
}

// Speed chip (#9): the top-right corner chip on clips whose rate
// deviates from 1.0. Hit-tested by the double-click handler.
QRect TimelinePanel::speedChipRect(const fc::Clip &clip, int row) const {
    const QRect body = clipBodyRect(clip, row);
    return QRect(body.right() - 44, body.top() + 2, 42, 14);
}

void TimelinePanel::drawClips(QPainter &painter) const {
    if (!model_) {
        return;
    }
    painter.setRenderHint(QPainter::Antialiasing, false);

    // Per-row clip lists (sorted by timeline start) drive the gap hatch
    // (#18); the paint pass itself stays the flat model loop below.
    std::map<int, std::vector<const fc::Clip *>> rows;
    for (const fc::Clip &clip : model_->clips()) {
        if (model_->trackAt(clip.trackIndex)) {
            rows[clip.trackIndex].push_back(&clip);
        }
    }
    for (auto &entry : rows) {
        std::sort(entry.second.begin(), entry.second.end(),
                  [](const fc::Clip *a, const fc::Clip *b) {
                      return a->timelineStart < b->timelineStart;
                  });
    }
    // Gap hatch (#18): inter-clip holes render as a subtle grey hatch;
    // an OVERLAP (model-invariant violation - purely defensive) screams
    // in a red crosshatch.
    for (const auto &entry : rows) {
        const QRect lane = laneRect(entry.first);
        for (size_t i = 1; i < entry.second.size(); ++i) {
            const fc::Clip *prev = entry.second[i - 1];
            const fc::Clip *next = entry.second[i];
            if (next->timelineStart < prev->timelineEnd()) {
                const int x0 = frameToX(next->timelineStart);
                const int x1 = frameToX(prev->timelineEnd());
                painter.fillRect(QRect(x0, lane.top() + 4, std::max(2, x1 - x0), lane.height() - 8),
                                 QBrush(ui::withAlpha(ui::kDanger, 60), Qt::DiagCrossPattern));
            } else if (next->timelineStart > prev->timelineEnd()) {
                const int x0 = frameToX(prev->timelineEnd());
                const int x1 = frameToX(next->timelineStart);
                if (x1 - x0 >= 2) {
                    painter.fillRect(QRect(x0, lane.top() + 4, x1 - x0, lane.height() - 8),
                                     QBrush(ui::withAlpha(ui::kText, 26), Qt::BDiagPattern));
                }
            }
        }
    }

    for (const fc::Clip &clip : model_->clips()) {
        const fc::Track *track = model_->trackAt(clip.trackIndex);
        if (!track) {
            continue;
        }
        const QRect rect = clipBodyRect(clip, clip.trackIndex);
        const bool selected = clip.id == selectedClipId_;

        QColor fill = selected ? kClipSelected : (clip.isText ? kTextClipFill : kClipFill);
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
        // Speed chip (#9): corner badge on rate-adjusted clips; double-
        // click opens the Speed / Duration dialog (mouseDoubleClickEvent).
        if (std::fabs(clip.rate - 1.0) > 1e-3) {
            const QRect chip = speedChipRect(clip, clip.trackIndex);
            painter.setBrush(ui::tint(ui::kWarning));
            painter.setPen(ui::color(ui::kWarning));
            painter.drawRect(chip);
            painter.setPen(ui::color(ui::kText));
            painter.setFont(QFont(QString::fromLatin1("Segoe UI"), 7));
            QString rateText = QString::number(clip.rate, 'f', 2);
            while (rateText.endsWith(QLatin1Char('0'))) {
                rateText.chop(1);
            }
            if (rateText.endsWith(QLatin1Char('.'))) {
                rateText.chop(1);
            }
            painter.drawText(chip, Qt::AlignCenter, rateText + QChar(0x00D7));
            painter.setFont(QFont());
        }
        // "fx N" chip (#37): bottom-right badge on clips carrying an
        // effect stack; clicking the clip selects it and Effect Controls
        // follows the selection (existing wiring).
        if (!clip.effectStack.empty()) {
            const QRect chip(rect.right() - 42, rect.bottom() - 15, 40, 13);
            painter.setBrush(ui::color(ui::kSurface3));
            painter.setPen(ui::color(ui::kLine));
            painter.drawRect(chip);
            painter.setPen(ui::color(ui::kTextDim));
            painter.setFont(QFont(QString::fromLatin1("Segoe UI"), 7, QFont::Bold));
            painter.drawText(chip, Qt::AlignCenter,
                             QStringLiteral("fx %1").arg(clip.effectStack.size()));
            painter.setFont(QFont()); // restore default for the next clip
        }
        // teal "T" badge on text clips (right-aligned, clear of the
        // speed chip above and the fx chip below).
        if (clip.isText) {
            QColor tColor(0x1A, 0xBC, 0x9C, int(255 * dim));
            painter.setPen(tColor);
            painter.setFont(QFont(QString::fromLatin1("Segoe UI"), 8, QFont::Bold));
            painter.drawText(QRect(rect.right() - 56, rect.top(), 26, rect.height()),
                             Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("T"));
            painter.setFont(QFont()); // restore default for the next clip
        }
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

// ---------------------------------------------------------------------------
// cut transition markers.
// ---------------------------------------------------------------------------

QRect TimelinePanel::transitionRect(const fc::Transition &t) const {
    const fc::Clip *left = model_ ? model_->clipById(t.leftClipId) : nullptr;
    if (!left) {
        return QRect();
    }
    const int64_t boundary = left->timelineEnd();
    const int64_t duration = t.durationFrames < 1 ? 1 : t.durationFrames;
    const int x0 = frameToX(boundary - duration);
    const int x1 = frameToX(boundary);
    const QRect lane = laneRect(t.trackIndex);
    return QRect(x0, lane.top() + 4, std::max(10, x1 - x0), lane.height() - 8);
}

void TimelinePanel::drawTransitions(QPainter &painter) const {
    if (!model_) {
        return;
    }
    painter.setRenderHint(QPainter::Antialiasing, false);
    for (const fc::Transition &t : model_->transitions()) {
        const fc::Clip *left = model_->clipById(t.leftClipId);
        if (!left) {
            continue; // pruned models never hold these; defensive only
        }
        const QRect rect = transitionRect(t);
        if (rect.width() <= 0 || rect.height() <= 0) {
            continue;
        }
        const double dim = trackDimFactor(t.trackIndex);
        const bool selected = t.id == selectedTransitionId_;

        QColor fill(0x2E, 0xA8, 0x4A, selected ? 150 : static_cast<int>(90 * dim));
        QColor border(0x3C, 0xBF, 0x5F, static_cast<int>(255 * dim));
        if (selected) {
            border = QColor(0xFF, 0xFF, 0xFF);
        }
        painter.setBrush(fill);
        painter.setPen(border);
        painter.drawRect(rect);
        // The X cross - the universal transition glyph.
        painter.setPen(QColor(0x10, 0x18, 0x10, static_cast<int>(255 * dim)));
        painter.drawLine(rect.topLeft() + QPoint(2, 2), rect.bottomRight() + QPoint(-2, -2));
        painter.drawLine(rect.topRight() + QPoint(-2, 2), rect.bottomLeft() + QPoint(2, -2));
    }
}

const fc::Transition *TimelinePanel::transitionAtPos(const QPoint &pos) const {
    if (!model_) {
        return nullptr;
    }
    const int row = trackRowAt(pos.y());
    if (row < 0 || pos.x() < kHeaderWidth) {
        return nullptr;
    }
    for (const fc::Transition &t : model_->transitions()) {
        if (t.trackIndex != row) {
            continue;
        }
        const QRect rect = transitionRect(t);
        // Central band only: the top/bottom strips stay reachable for clip
        // edge-drag trims and body moves around the marker.
        const int cy = (rect.top() + rect.bottom()) / 2;
        const int half = 10;
        // transitionRect (like every frameToX call) is CONTENT space, but
        // pos is a WIDGET coordinate - shift it by the scroll offset or a
        // scrolled click selects a transition living scrollX_ pixels away
        // (and misses the one actually under the cursor).
        const int contentX = pos.x() + scrollX_;
        if (contentX >= rect.left() && contentX <= rect.right() && pos.y() >= cy - half &&
            pos.y() <= cy + half) {
            return &t;
        }
    }
    return nullptr;
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
    // Locked lanes must never start the drag lifecycle (no ghost, no
    // capture): the press behaves as a no-clip interaction and falls
    // through to the playhead scrub. The release-side lock checks stay
    // as a safety net for models that change mid-drag.
    const fc::Track *track = model_->trackAt(clip->trackIndex);
    if (track && track->locked) {
        resetDrag();
        return;
    }
    dragClipId_ = clip->id;
    dragOriginStart_ = clip->timelineStart;
    dragOriginEnd_ = clip->timelineEnd();
    ghostStart_ = clip->timelineStart;
    ghostEnd_ = clip->timelineEnd();
    // Edge-grab comparisons run in CONTENT coordinates (frameToX is
    // content-space; the mouse x needs the same scroll shift).
    const int contentX = pos.x() + scrollX_;
    const int clipX0 = frameToX(clip->timelineStart);
    const int clipX1 = frameToX(clip->timelineEnd());
    // Adaptive edge zones: a clip narrower than 3 * kEdgeGrabPx would
    // otherwise be ALL edge (no grabbable body), so each zone shrinks
    // to a third of the clip's pixel width and the middle stays a
    // move handle.
    const int edgePx = std::min(kEdgeGrabPx, (clipX1 - clipX0) / 3);

    // Alt + edge press where two clips touch => rolling edit on that
    // boundary; plain edge press => trim; body press => move.
    const bool altHeld = QGuiApplication::queryKeyboardModifiers() & Qt::AltModifier;
    if (contentX - clipX0 <= edgePx && clipX1 - contentX > edgePx) {
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
    } else if (clipX1 - contentX <= edgePx) {
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
        dragRawStart_ = std::max<int64_t>(0, clip->timelineStart); // unsnapped reference (#14)
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
    dragRawStart_ = 0;
    stopAutoPage();
    update();
}

void TimelinePanel::mousePressEvent(QMouseEvent *event) {
    const int x = event->pos().x();
    const int y = event->pos().y();
    const int row = trackRowAt(y);

    // Header column: L/M/S toggles (left) and the track context menu
    // (right), both scroll-aware through laneRect/headerBoxAt (#13).
    if (row >= 0 && x >= 0 && x < kHeaderWidth && model_ && row < model_->trackCount()) {
        if (event->button() == Qt::LeftButton) {
            const int box = headerBoxAt(event->pos(), row);
            if (box >= 0) {
                emit trackStateToggleRequested(row, box);
            }
        } else if (event->button() == Qt::RightButton) {
            showTrackContextMenu(event->globalPos(), row);
        }
        return;
    }

    // The ruler band (between the tool row and the lanes) is a seek
    // surface: a press there falls through to the same playhead scrub
    // the empty-lane path uses below (and mouse-move dragging from the
    // ruler keeps scrubbing through the existing move handler).
    const bool inRuler = y >= contentTop() && y < contentTop() + kRulerHeight;
    if (x < kHeaderWidth || (row < 0 && !inRuler) || y > areaHeight() + contentTop()) {
        resetDrag();
        return;
    }

    const int64_t frame = xToFrame(x);
    if (razorMode_) {
        // Razor splits need a lane; a ruler click in razor mode stays a
        // no-op (as it was before the ruler learned to seek).
        if (row >= 0) {
            emit splitRequested(row, frame);
        }
        return;
    }

    if (event->button() == Qt::LeftButton && model_) {
        // clicking a transition marker selects it (the clip
        // selection yields to the editor's transition page).
        if (const fc::Transition *t = transitionAtPos(event->pos())) {
            selectedTransitionId_ = t->id;
            selectedClipId_ = -1;
            emit transitionSelected(t->id);
            update();
            return;
        }
        if (selectedTransitionId_ != -1) {
            selectedTransitionId_ = -1;
            emit transitionSelected(-1);
        }
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

void TimelinePanel::mouseDoubleClickEvent(QMouseEvent *event) {
    const QPoint pos = event->pos();

    // Track-name rename (#13): double-click the header name area.
    if (model_ && pos.x() >= 0 && pos.x() < kHeaderWidth) {
        const int row = trackRowAt(pos.y());
        if (row >= 0 && row < model_->trackCount()) {
            startRename(row);
            return;
        }
    }

    // Speed chip (#9): double-click opens the Speed / Duration dialog
    // for that clip - selection first (the same flow a body click
    // uses), then the wiring signal for MainWindow.
    if (model_) {
        int row = -1;
        if (const fc::Clip *clip = clipAtPos(pos, &row)) {
            if (row >= 0 && speedChipRect(*clip, row).contains(pos)) {
                selectedClipId_ = clip->id;
                selectedTransitionId_ = -1;
                emit clipSelected(clip->id);
                update();
                emit speedDialogRequested();
                return;
            }
        }
    }
    QWidget::mouseDoubleClickEvent(event);
}

void TimelinePanel::mouseMoveEvent(QMouseEvent *event) {
    const int x = event->pos().x();

    // Header hover affordances (#13): the L/M/S boxes and the name
    // surface advertise their actions on hover.
    if (x < kHeaderWidth && model_ && dragMode_ == DragMode::None) {
        const int hrow = trackRowAt(event->pos().y());
        if (hrow >= 0 && hrow < model_->trackCount()) {
            const fc::Track *ht = model_->trackAt(hrow);
            const int box = headerBoxAt(event->pos(), hrow);
            QString tip;
            if (box == 0) {
                tip = ht->locked ? tr("Unlock track") : tr("Lock track");
            } else if (box == 1) {
                tip = ht->muted ? tr("Unmute track") : tr("Mute track");
            } else if (box == 2) {
                tip = ht->solo ? tr("Unsolo track") : tr("Solo track");
            } else {
                tip = tr("Double-click to rename");
            }
            QToolTip::showText(event->globalPos(), tip, this);
        }
    }

    // Razor hover preview line.
    if (razorMode_ && !(event->buttons() & Qt::LeftButton)) {
        razorHoverX_ = x;
        update();
        return;
    }

    if (dragMode_ != DragMode::None && model_) {
        // The implicit grab can be lost (system gesture, window switch):
        // without the button there will be no release to commit from,
        // so resolve the drag exactly the way a release would instead
        // of ghosting forever.
        if (!(event->buttons() & Qt::LeftButton)) {
            finishDrag();
            return;
        }
        if (!model_->clipById(dragClipId_)) {
            resetDrag();
            return;
        }
        lastMousePos_ = event->pos();
        updateDragGhost(lastMousePos_);
        updateAutoPage(lastMousePos_);
        update();
        return;
    }

    // Playhead scrub.
    if ((event->buttons() & Qt::LeftButton) && x > kHeaderWidth &&
        event->pos().y() <= areaHeight() + contentTop() && !razorMode_) {
        playhead_ = static_cast<double>(xToFrame(x)) / fps_;
        update();
        emit playheadMoved(playhead_);
        return;
    }

    // Hover cursor affordance over clip edges + hover scrub tooltip
    // (#16, Alt+hover).
    if (!(event->buttons() & Qt::LeftButton) && !razorMode_ && model_) {
        const fc::Clip *clip = clipAtPos(event->pos());
        if (clip) {
            const int contentX = x + scrollX_; // frameToX is content-space
            const int x0 = frameToX(clip->timelineStart);
            const int x1 = frameToX(clip->timelineEnd());
            // Same adaptive zone as beginClipDrag: the cursor must not
            // advertise an edge drag where the body grab now lives.
            const int edgePx = std::min(kEdgeGrabPx, (x1 - x0) / 3);
            const bool nearEdge = (contentX - x0 <= edgePx) || (x1 - contentX <= edgePx);
            const fc::Track *track = model_->trackAt(clip->trackIndex);
            setCursor(track && track->locked ? Qt::ForbiddenCursor
                                             : (nearEdge ? Qt::SizeHorCursor : Qt::ArrowCursor));
            if (QGuiApplication::queryKeyboardModifiers() & Qt::AltModifier) {
                QString name = QString::fromStdString(clip->label);
                if (name.isEmpty()) {
                    name = QString::fromStdString(clip->sourcePath)
                               .replace('\\', '/')
                               .section('/', -1);
                }
                const int64_t dur = clip->durationFrames();
                QString tip = name + QLatin1Char('\n') +
                              tr("in %1 -> out %2")
                                  .arg(qlonglong(clip->sourceInFrames))
                                  .arg(qlonglong(clip->sourceOutFrames)) +
                              QLatin1Char('\n') +
                              tr("duration: %1 frames (%2 s)")
                                  .arg(qlonglong(dur))
                                  .arg(QString::number(dur / fps_, 'f', 2));
                if (std::fabs(clip->rate - 1.0) > 1e-3) {
                    tip += QLatin1Char('\n') +
                           tr("rate: %1\u00d7").arg(QString::number(clip->rate, 'f', 2));
                }
                QToolTip::showText(event->globalPos(), tip, this);
            }
        } else {
            setCursor(Qt::ArrowCursor);
        }
    }
}

// Drag ghost resolution, shared verbatim by mouse moves and drag
// auto-page steps so both paths compute the exact same ghost.
void TimelinePanel::updateDragGhost(const QPoint &pos) {
    const fc::Clip *clip = model_ ? model_->clipById(dragClipId_) : nullptr;
    if (!clip || dragMode_ == DragMode::None) {
        return;
    }
    const int64_t frame = xToFrame(pos.x());
    const int row = trackRowAt(pos.y());
    switch (dragMode_) {
    case DragMode::Move: {
        const int64_t desired = frame - dragGrabOffset_;
        const int64_t safeDesired = desired < 0 ? 0 : desired;
        dragRawStart_ = safeDesired; // unsnapped reference for the indicator (#14)
        const int targetRow = row >= 0 ? row : dragRow_;
        ghostRow_ = targetRow;
        if (QGuiApplication::queryKeyboardModifiers() & Qt::AltModifier) {
            // Alt suspends the magnet (#14): the ghost tracks the raw
            // pointer position and validity degrades to the lane-kind
            // check; an overlapping drop is rejected by MainWindow's
            // moveClipTo on release - by design.
            ghostStart_ = safeDesired;
            const fc::Track *target = model_->trackAt(targetRow);
            ghostValid_ = target && target->isText == clip->isText;
        } else {
            const int64_t snapped = model_->findDropPosition(targetRow, dragClipId_, safeDesired,
                                                             clip->durationFrames());
            if (snapped >= 0) {
                ghostStart_ = snapped;
                ghostValid_ = true;
            } else {
                ghostStart_ = safeDesired;
                ghostValid_ = false;
            }
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
}

// Drag auto-paging (#11): while a drag is active and the pointer sits
// within the edge zone of the lanes viewport, a repeating timer scrolls
// that way (vertical scrollY_ / horizontal scrollX_) and re-resolves
// the ghost exactly as a mouse move would.
void TimelinePanel::updateAutoPage(const QPoint &pos) {
    if (dragMode_ == DragMode::None) {
        stopAutoPage();
        return;
    }
    const QRect vp = lanesViewport();
    const int edge = 20;
    autoPageDx_ = 0;
    autoPageDy_ = 0;
    if (vp.width() > 0) {
        if (pos.x() < vp.left() + edge) {
            autoPageDx_ = -12;
        } else if (pos.x() > vp.right() - edge) {
            autoPageDx_ = 12;
        }
    }
    if (vp.height() > 0) {
        if (pos.y() < vp.top() + edge) {
            autoPageDy_ = -8;
        } else if (pos.y() > vp.bottom() - edge) {
            autoPageDy_ = 8;
        }
    }
    if ((autoPageDx_ || autoPageDy_) && !autoPageTimer_->isActive()) {
        autoPageTimer_->start();
    } else if (!autoPageDx_ && !autoPageDy_) {
        stopAutoPage();
    }
}

void TimelinePanel::stopAutoPage() {
    autoPageDx_ = 0;
    autoPageDy_ = 0;
    if (autoPageTimer_ && autoPageTimer_->isActive()) {
        autoPageTimer_->stop();
    }
}

void TimelinePanel::autoPageStep() {
    if (dragMode_ == DragMode::None) {
        stopAutoPage();
        return;
    }
    if (autoPageDx_ && hscroll_) {
        hscroll_->setValue(
            std::min(std::max(hscroll_->value() + autoPageDx_, 0), hscroll_->maximum()));
    }
    if (autoPageDy_ && vscroll_) {
        vscroll_->setValue(
            std::min(std::max(vscroll_->value() + autoPageDy_, 0), vscroll_->maximum()));
    }
    updateDragGhost(lastMousePos_);
    update();
}

void TimelinePanel::finishDrag() {
    if (dragMode_ == DragMode::None || !model_) {
        resetDrag();
        return;
    }
    const fc::Clip *clip = model_->clipById(dragClipId_);
    const fc::Track *track = clip ? model_->trackAt(clip->trackIndex) : nullptr;

    switch (dragMode_) {
    case DragMode::Move:
        // The ghost row may be a different (same-kind, valid) lane; the
        // lock check here mirrors MainWindow's safety net. A click
        // without movement emits nothing: emitting the origin-position
        // move bumped the revision and dirtied a saved project.
        if (clip && ghostRow_ >= 0) {
            const bool moved = ghostRow_ != clip->trackIndex || ghostStart_ != dragOriginStart_;
            const fc::Track *target = model_->trackAt(ghostRow_);
            if (moved && target && !target->locked && !(track && track->locked)) {
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

void TimelinePanel::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton || dragMode_ == DragMode::None || !model_) {
        resetDrag();
        return;
    }
    finishDrag();
}

void TimelinePanel::leaveEvent(QEvent *event) {
    razorHoverX_ = -1;
    QToolTip::hideText();
    stopAutoPage();
    update();
    QWidget::leaveEvent(event);
}

void TimelinePanel::wheelEvent(QWheelEvent *event) {
    if (event->modifiers() & Qt::ControlModifier) {
        // Ctrl+wheel = zoom around the cursor (#10): the frame under
        // the pointer stays under the pointer after the zoom. A purely
        // horizontal wheel delta (y == 0) must not zoom - the ternary
        // below would read it as "zoom out".
        if (event->angleDelta().y() != 0) {
            // QWheelEvent::pos() is deprecated from Qt 5.15 (replaced by
            // position(), which does not exist on the 5.12 build floor).
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
            const int anchorX = static_cast<int>(event->position().x());
#else
            const int anchorX = event->pos().x();
#endif
            applyZoom(pps_ + (event->angleDelta().y() > 0 ? 20.0 : -20.0), anchorX,
                      xToFrame(anchorX));
        }
        event->accept();
        return;
    }

    // Wheel routing (#12): Shift+wheel scrolls horizontally; plain
    // wheel scrolls VERTICALLY while the track stack overflows the
    // lanes viewport and falls back to the legacy horizontal scroll
    // when it does not. pixelDelta wins over angleDelta when present
    // (hiDPI precision scrolling).
    QPoint pix = event->pixelDelta();
    int dx = 0;
    int dy = 0;
    if (!pix.isNull()) {
        dx = pix.x();
        dy = pix.y();
    } else {
        const QPoint angle = event->angleDelta();
        constexpr double kPxPerNotch = 60.0;
        dx = static_cast<int>(std::llround(angle.x() / 120.0 * kPxPerNotch));
        dy = static_cast<int>(std::llround(angle.y() / 120.0 * kPxPerNotch));
    }
    if (event->modifiers() & Qt::ShiftModifier) {
        dy = 0; // Shift forces horizontal routing
    }

    if (dy != 0 && vscroll_ && vscroll_->maximum() > 0) {
        vscroll_->setValue(std::min(std::max(vscroll_->value() - dy, 0), vscroll_->maximum()));
        event->accept();
        return;
    }
    const int horizontal = dx != 0 ? dx : dy;
    if (horizontal != 0 && hscroll_ && hscroll_->maximum() > 0) {
        hscroll_->setValue(
            std::min(std::max(hscroll_->value() - horizontal, 0), hscroll_->maximum()));
        event->accept();
        return;
    }
    QWidget::wheelEvent(event);
}

void TimelinePanel::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    if (renameEditor_ && renameEditor_->isVisible()) {
        endRename(false); // geometry moved under the editor - cancel
    }
    layoutChrome();
    updateScrollRange();
    update();
}

// Position the manual children that live inside the painted band: the
// vertical scrollbar hugs the right edge of the lanes area.
void TimelinePanel::layoutChrome() {
    if (vscroll_) {
        vscroll_->setGeometry(std::max(0, width() - kHScrollHeight), contentTop() + kRulerHeight,
                              kHScrollHeight, std::max(0, areaHeight() - kRulerHeight));
    }
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

// The header cell's L/M/S box under pos (-1 = none), scroll-aware:
// the box rects mirror drawHeaderColumn's geometry exactly.
int TimelinePanel::headerBoxAt(const QPoint &pos, int row) const {
    const int cellTop = contentTop() + kRulerHeight + row * kTrackHeight - scrollY_;
    const int relY = pos.y() - cellTop;
    if (relY < 22 || relY > 38) {
        return -1; // the L/M/S boxes live in the lower half of the cell
    }
    if (pos.x() < 10 || pos.x() > 10 + 3 * 26 - 4) {
        return -1;
    }
    const int box = (pos.x() - 10) / 26;
    return box >= 0 && box < 3 ? box : -1;
}

// Track header context menu (right click, #13): checkable Lock/Mute/
// Solo entries mirroring the model state, emitting the SAME
// trackStateToggleRequested the L/M/S box clicks use - no parallel
// edit path.
void TimelinePanel::showTrackContextMenu(const QPoint &globalPos, int row) {
    const fc::Track *track = model_ ? model_->trackAt(row) : nullptr;
    if (!track) {
        return;
    }
    QMenu menu(this);
    const char *labels[3] = {"&Lock", "&Mute", "&Solo"};
    const bool states[3] = {track->locked, track->muted, track->solo};
    for (int c = 0; c < 3; ++c) {
        QAction *action = menu.addAction(tr(labels[c]));
        action->setCheckable(true);
        action->setChecked(states[c]);
        connect(action, &QAction::triggered, this,
                [this, row, c] { emit trackStateToggleRequested(row, c); });
    }
    menu.exec(globalPos);
}

// Inline track-name rename (#13): an editor rides the header cell's
// name rect; Enter commits through trackRenameRequested, Escape and
// focus-out cancel. The model has no rename mutator yet - the signal
// is the contract for the wave-2 wiring (renameTrack + persistence).
void TimelinePanel::startRename(int row) {
    const fc::Track *track = model_ ? model_->trackAt(row) : nullptr;
    if (!track) {
        return;
    }
    if (!renameEditor_) {
        renameEditor_ = new QLineEdit(this);
        renameEditor_->setFrame(false);
        renameEditor_->installEventFilter(this);
        connect(renameEditor_, &QLineEdit::returnPressed, this, [this] { endRename(true); });
    }
    renameRow_ = row;
    const QRect cell = laneRect(row);
    renameEditor_->setGeometry(10, cell.top() + 3, kHeaderWidth - 22, 18);
    renameEditor_->setText(QString::fromStdString(track->name));
    renameEditor_->show();
    renameEditor_->raise();
    renameEditor_->setFocus();
    renameEditor_->selectAll();
}

void TimelinePanel::endRename(bool commit) {
    if (!renameEditor_ || renameRow_ < 0) {
        return;
    }
    const QString text = renameEditor_->text().trimmed();
    const fc::Track *track = model_ ? model_->trackAt(renameRow_) : nullptr;
    if (commit && track && !text.isEmpty() && text != QString::fromStdString(track->name)) {
        emit trackRenameRequested(renameRow_, text);
    }
    renameRow_ = -1;
    renameEditor_->hide();
}

bool TimelinePanel::eventFilter(QObject *watched, QEvent *event) {
    // Focus-out cancels the inline rename (commit only ever happens on
    // Enter, so tabbing away cannot half-commit a name).
    if (watched == renameEditor_ && event->type() == QEvent::FocusOut) {
        endRename(false);
    }
    return QWidget::eventFilter(watched, event);
}
