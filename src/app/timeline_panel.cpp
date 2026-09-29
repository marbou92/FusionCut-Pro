#include "timeline_panel.h"

#include <QAction>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
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
#include "ui_widgets.h"

// ui_theme.h tokens live in fc::ui; the timeline addresses them as
// ui::... throughout its paint code, so the file pulls in the fc
// namespace (its class names never collide with the local ones).
using namespace fc;

namespace {
constexpr int kHeaderWidth = 96;
constexpr int kRulerHeight = 24; // #90: 24 px ruler band
constexpr int kTrackHeight = 56; // round 5: CapCut-proportion lanes
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
// selection accent come from the app palette; the round-5 CapCut-style
// palette below stays local to the timeline.
const QColor kPanelBg = ui::color(ui::kTimelineBg);
// Lane bands (drawLaneBands): near-black striping with a whisper of
// the lane kind - video slate, audio green, text violet - over the
// panel backdrop, CapCut-style.
const QColor kVideoTrack(0x17, 0x1A, 0x1F);
const QColor kAudioTrack(0x14, 0x1E, 0x1A);
const QColor kTextTrack(0x1A, 0x16, 0x20);
// Clip bodies. Video clips carry the filmstrip when one arrived, and
// fall back to this slate; audio clips are deep teal with the real
// waveform over them; text clips stay violet.
const QColor kClipFill(0x24, 0x2B, 0x36);
const QColor kAudioClipFill(0x13, 0x3A, 0x33);
const QColor kTextClipFill(0x4A, 0x33, 0x5E);
const QColor kGhostFill(0x6E, 0x9B, 0xB8);
const QColor kGhostBad(0xB0, 0x3A, 0x2E);
// Round 5 accents: the selection reads WHITE (CapCut), hover is white
// at low alpha, and the label band sits on a dark gradient over the
// strip so the clip name stays readable on any footage.
const QColor kSelectionWhite(0xF4, 0xF6, 0xF8);
const QColor kWaveform(0x7F, 0xE0, 0xC8);
const QColor kLabel(0xE8, 0xEA, 0xEC);

// #88: painted glyphs for the header L/M/S toggles (they stay painted -
// the press hit band in headerBoxAt() is untouched). ~10 px glyphs: dim
// outline when off, near-white on the accent tint when on.
constexpr double kPi = 3.14159265358979323846;

void paintLockGlyph(QPainter &p, const QPointF &c, const QColor &color, bool on) {
    QPen pen(color, 1.2);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    p.setBrush(on ? color : QBrush());
    p.drawArc(QRectF(c.x() - 2.5, c.y() - 4.5, 5.0, 5.0), 0, 180 * 16);
    p.drawRoundedRect(QRectF(c.x() - 3.5, c.y() - 0.5, 7.0, 5.0), 1, 1);
}

void paintMuteGlyph(QPainter &p, const QPointF &c, const QColor &color, bool on) {
    QPen pen(color, 1.2);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(on ? color : QBrush());
    QPolygonF speaker;
    speaker << QPointF(c.x() - 5.5, c.y() - 2.0) << QPointF(c.x() - 2.5, c.y() - 2.0)
            << QPointF(c.x() - 0.5, c.y() - 4.0) << QPointF(c.x() - 0.5, c.y() + 4.0)
            << QPointF(c.x() - 2.5, c.y() + 2.0) << QPointF(c.x() - 5.5, c.y() + 2.0);
    p.drawPolygon(speaker);
    p.setBrush(QBrush());
    p.drawLine(QPointF(c.x() + 1.5, c.y() - 2.5), QPointF(c.x() + 5.0, c.y() + 2.5));
    p.drawLine(QPointF(c.x() + 5.0, c.y() - 2.5), QPointF(c.x() + 1.5, c.y() + 2.5));
}

void paintSoloGlyph(QPainter &p, const QPointF &c, const QColor &color, bool on) {
    QPolygonF star;
    for (int i = 0; i < 10; ++i) {
        const qreal angle = -kPi / 2.0 + i * kPi / 5.0;
        const qreal radius = (i % 2 == 0) ? 5.5 : 2.4;
        star << QPointF(c.x() + radius * std::cos(angle), c.y() + radius * std::sin(angle));
    }
    QPen pen(color, 1.1);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(on ? color : QBrush());
    p.drawPolygon(star);
}

// #176: tabular figures for the ruler timecodes. Qt 5.12 has no
// OpenType-feature QFont API (QFont::setFeature is Qt 6.7+), so the
// ruler draws each glyph centered in a fixed-width cell (the widest
// glyph of its own string) - the same app-default font the transport
// timecode uses, with a stable per-glyph rhythm between ticks.
void paintTabularText(QPainter &p, const QRect &rect, const QString &text) {
    const QFontMetrics fm(p.font());
    int cell = 0;
    for (const QChar &c : text) {
        cell = std::max(cell, fm.horizontalAdvance(c));
    }
    if (cell <= 0) {
        p.drawText(rect, Qt::AlignLeft | Qt::AlignVCenter, text);
        return;
    }
    int x = rect.left();
    for (const QChar &c : text) {
        p.drawText(QRect(x, rect.top(), cell, rect.height()), Qt::AlignVCenter | Qt::AlignHCenter,
                   QString(c));
        x += cell;
    }
}
} // namespace

TimelinePanel::TimelinePanel(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // ---- tool row: [Select | Blade]  [Ripple] -----------------------
    auto *toolRow = new QWidget(this);
    toolRow->setFixedHeight(kToolRowHeight);
    auto *tools = new QHBoxLayout(toolRow);
    tools->setContentsMargins(kHeaderWidth + 8, 2, 12, 2);
    tools->setSpacing(8);

    // Tool segmented control (#86): the blue Select pill + gray siblings
    // become one NSSegmentedControl-style cluster (its buttons are
    // globally styled via the fcSegment property). Ripple stays a
    // separate toggle on purpose: it is an edit MODE, not a tool - it
    // must coexist with Select and Blade, and segment exclusivity would
    // silently un-check it.
    toolSegment_ = new fc::SegmentedControl(toolRow);
    const qreal toolIconDpr = devicePixelRatioF();
    const int selectSeg = toolSegment_->addSegment(
        tr("Select"),
        icons::makeIcon(QStringLiteral("cursor"), ui::color(ui::kText), 16, toolIconDpr),
        tr("Select / move / trim tool (V)"));
    const int bladeSeg = toolSegment_->addSegment(
        tr("Blade"),
        icons::makeIcon(QStringLiteral("blade"), ui::color(ui::kText), 16, toolIconDpr),
        tr("Blade tool: click a clip to split it at that frame (C)"));
    toolSegment_->button(selectSeg)->setShortcut(QKeySequence(Qt::Key_V));
    toolSegment_->button(selectSeg)->setAccessibleName(tr("Select tool"));
    toolSegment_->button(bladeSeg)->setShortcut(QKeySequence(Qt::Key_C));
    toolSegment_->button(bladeSeg)->setAccessibleName(tr("Blade tool"));
    // Same logic the old Select/Razor toggled handlers ran (only the
    // "checked" edge acts): setRazorMode feeds back through
    // syncToolButtons(), whose setCurrent is a no-op on the current
    // index, so the loop stays dead. addSegment pre-checks Select -
    // razorMode_ starts false and the handler is not installed yet.
    toolSegment_->onSelected([this](int index) { setRazorMode(index == 1); });
    tools->addWidget(toolSegment_);

    rippleTool_ = new QToolButton(toolRow);
    rippleTool_->setText(tr("Ripple"));
    rippleTool_->setToolTip(tr("Ripple edits: delete and tail-trim close the gap "
                               "instead of leaving a hole (R)"));
    rippleTool_->setCheckable(true);
    rippleTool_->setAccessibleName(tr("Ripple toggle"));
    rippleTool_->setObjectName(QStringLiteral("fcToolToggle"));
    // Token-consistent elevated toggle (one-accent rule #66: the accent
    // border appears only while the mode is on).
    rippleTool_->setStyleSheet(
        QStringLiteral("QToolButton{background:%1;border:1px solid %2;border-radius:%6px;"
                       "padding:3px 10px;color:%3;}"
                       "QToolButton:hover{background:%4;}"
                       "QToolButton:pressed{background:%5;}"
                       "QToolButton:checked{border-color:%7;}")
            .arg(ui::color(ui::kSurface3).name(), ui::color(ui::kLine).name(),
                 ui::color(ui::kText).name(), ui::color(ui::kSurfaceHover).name(),
                 ui::color(ui::kSurfacePress).name(), QString::number(ui::kRadiusControl),
                 ui::color(ui::kAccent).name()));
    // Tool keyboard switching (#15): V/C are the existing select/razor
    // bindings; R toggles ripple (plain R is free - Ctrl+R stays the
    // Speed / Duration dialog at window level).
    rippleTool_->setShortcut(QKeySequence(Qt::Key_R));
    connect(rippleTool_, &QToolButton::toggled, this, [this](bool on) { rippleEnabled_ = on; });

    tools->addWidget(rippleTool_);

    // Split-at-playhead button (#182): a ghost action button next to
    // the tool cluster (Ripple is a mode, this is an action - same
    // reason they are separate widgets). Hover arms the accent border;
    // the click emits the no-lane-context splitRequested() overload for
    // MainWindow::splitAtPlayhead. No key letter is advertised: the
    // Clip menu binds Split at Playhead to C, which is ALSO the Blade
    // tool shortcut inside this panel's context - a tooltip letter
    // would mislead one way or the other.
    splitTool_ = new QToolButton(toolRow);
    splitTool_->setIcon(
        icons::makeIcon(QStringLiteral("blade"), ui::color(ui::kText), 16, toolIconDpr));
    splitTool_->setToolTip(tr("Split at playhead"));
    splitTool_->setAccessibleName(tr("Split at playhead"));
    splitTool_->setFixedSize(28, 28);
    splitTool_->setCursor(Qt::PointingHandCursor);
    splitTool_->setStyleSheet(
        QStringLiteral("QToolButton{background:%1;border:1px solid %2;border-radius:%3px;}"
                       "QToolButton:hover{background:%4;border-color:%5;}"
                       "QToolButton:pressed{background:%6;}")
            .arg(ui::color(ui::kSurface3).name(), ui::color(ui::kLine).name(),
                 QString::number(ui::kRadiusControl), ui::color(ui::kSurfaceHover).name(),
                 ui::color(ui::kAccent).name(), ui::color(ui::kSurfacePress).name()));
    connect(splitTool_, &QToolButton::clicked, this, [this] { emit splitRequested(); });

    tools->addWidget(splitTool_);
    tools->addStretch(1);
    layout->addWidget(toolRow);

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
    zoom_->setMinimumWidth(150);
    connect(zoom_, &QSlider::valueChanged, this,
            [this](int value) { applyZoom(static_cast<double>(value)); });

    // Zoom cluster (#91): a right-aligned pill holding - / slider / +,
    // retiring the bare full-width slider + label. The buttons step the
    // same 1.25x ladder the = / - shortcuts use; the slider's value
    // mapping (5..400 -> applyZoom) is untouched.
    auto *zoomRow = new QWidget(this);
    zoomRow->setFixedHeight(kZoomBarHeight);
    auto *zoomLayout = new QHBoxLayout(zoomRow);
    zoomLayout->setContentsMargins(kHeaderWidth + 8, 0, 12, 0);
    zoomLayout->addStretch(1);

    auto *cluster = new QFrame(zoomRow);
    cluster->setObjectName(QStringLiteral("fcZoomCluster"));
    cluster->setStyleSheet(
        QStringLiteral("QFrame#fcZoomCluster{background:%1;border:1px solid %2;"
                       "border-radius:12px;}"
                       "QFrame#fcZoomCluster QToolButton{background:transparent;border:none;"
                       "border-radius:9px;color:%3;font-size:15px;font-weight:600;}"
                       "QFrame#fcZoomCluster QToolButton:hover{background:%4;}"
                       "QFrame#fcZoomCluster QToolButton:pressed{background:%5;}")
            .arg(ui::color(ui::kSurface2).name(), ui::color(ui::kLine).name(),
                 ui::color(ui::kText).name(), ui::color(ui::kSurfaceHover).name(),
                 ui::color(ui::kSurfacePress).name()));
    auto *clusterLayout = new QHBoxLayout(cluster);
    clusterLayout->setContentsMargins(4, 2, 4, 2);
    clusterLayout->setSpacing(2);

    // Fit button (#183): fits the whole sequence into the lanes
    // viewport through the SAME applyZoom path the "\\" shortcut uses
    // (the inverse of the slider mapping, clamped to the 5..400 range).
    // Text reads cleaner than any glyph here; the cluster sheet styles
    // the button, only the font size drops to the 11 px small scale.
    auto *zoomFit = new QToolButton(cluster);
    zoomFit->setText(tr("Fit"));
    zoomFit->setToolTip(tr("Zoom to fit the whole sequence (\\)"));
    zoomFit->setFixedSize(34, 20);
    zoomFit->setCursor(Qt::PointingHandCursor);
    zoomFit->setAccessibleName(tr("Fit sequence"));
    zoomFit->setStyleSheet(QStringLiteral("QToolButton{font-size:11px;}"));
    connect(zoomFit, &QToolButton::clicked, this, &TimelinePanel::fitToSequence);

    auto *zoomOut = new QToolButton(cluster);
    zoomOut->setText(QString(QChar(0x2212))); // minus sign
    zoomOut->setToolTip(tr("Zoom out (-)"));
    zoomOut->setFixedSize(20, 20);
    zoomOut->setCursor(Qt::PointingHandCursor);
    zoomOut->setAccessibleName(tr("Zoom out"));
    connect(zoomOut, &QToolButton::clicked, this, [this] { applyZoom(pps_ / 1.25); });
    auto *zoomIn = new QToolButton(cluster);
    zoomIn->setText(tr("+"));
    zoomIn->setToolTip(tr("Zoom in (=)"));
    zoomIn->setFixedSize(20, 20);
    zoomIn->setCursor(Qt::PointingHandCursor);
    zoomIn->setAccessibleName(tr("Zoom in"));
    connect(zoomIn, &QToolButton::clicked, this, [this] { applyZoom(pps_ * 1.25); });

    clusterLayout->addWidget(zoomFit);
    clusterLayout->addWidget(zoomOut);
    clusterLayout->addWidget(zoom_, 1);
    clusterLayout->addWidget(zoomIn);
    zoomLayout->addWidget(cluster);
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

void TimelinePanel::setMediaStrip(const QString &path, const QImage &strip,
                                  double durationSeconds) {
    StripVisual visual;
    visual.image = strip;
    visual.durationSec = durationSeconds > 0.0 ? durationSeconds : 0.0;
    strips_[path.toStdString()] = visual;
    update();
}

void TimelinePanel::setMediaWaveform(const QString &path, const QVector<float> &peaks,
                                     double durationSeconds) {
    WaveformVisual visual;
    visual.peaks = peaks;
    visual.durationSec = durationSeconds > 0.0 ? durationSeconds : 0.0;
    waveforms_[path.toStdString()] = visual;
    update();
}

void TimelinePanel::clearMediaVisuals() {
    strips_.clear();
    waveforms_.clear();
    update();
}

void TimelinePanel::setModel(const fc::TimelineModel *model) {
    model_ = model;
    // The selection ids belonged to the previous model content: a
    // replacement model may not contain them (or may reuse the ids for
    // different clips), so the highlight must not survive the swap.
    selectedClipId_ = -1;
    selectedTransitionId_ = -1;
    hoveredClipId_ = -1; // #200: ids belong to the previous model content
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

// #180: nudge the selected clip by `frames` timeline frames. The
// destination must be FREE: findDropPosition() (the same resolver the
// move ghost uses) must return the desired frame exactly - a snapped
// result means the spot is occupied and the nudge is dropped. The edit
// travels through the SAME clipMoveRequested signal a drag commits
// with, so MainWindow's validation, undo push and audio rebuild stay
// single-sourced there.
void TimelinePanel::nudgeSelectedClip(int64_t frames) {
    if (!model_ || frames == 0) {
        return;
    }
    const fc::Clip *clip = model_->clipById(selectedClipId_);
    if (!clip) {
        return;
    }
    const fc::Track *track = model_->trackAt(clip->trackIndex);
    if (!track || track->locked) {
        return; // same lock policy as the drag commit path
    }
    const int64_t desired = std::max<int64_t>(0, clip->timelineStart + frames);
    if (desired == clip->timelineStart) {
        return; // clamped at the timeline head - nothing to do
    }
    const int64_t drop =
        model_->findDropPosition(clip->trackIndex, clip->id, desired, clip->durationFrames());
    if (drop != desired) {
        return; // destination occupied (the resolver would snap) - no move
    }
    emit clipMoveRequested(clip->id, clip->trackIndex, desired);
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
    drawLaneBands(painter); // round 5: per-kind lane striping under the clips
    drawClips(painter);
    drawTransitions(painter);
    drawDragGhost(painter);
    // Drop feedback (#173) + snap indicator (#174, the old #14 line):
    // while a Move drag is live, a 2 px kAccent insertion line with a
    // small triangle cap marks the current drop frame - resolved from
    // the EXISTING ghost computation (ghostStart_), no new targeting.
    // When the magnet engages (ghostStart_ != the raw pointer start;
    // Alt-suspended drags never differ), the line switches to the 1 px
    // dashed 3/3 snap style at the snapped frame.
    if (dragMode_ == DragMode::Move && ghostRow_ >= 0) {
        const int sx = frameToX(ghostStart_);
        const bool snapped = ghostStart_ != dragRawStart_;
        if (snapped) {
            QPen snapPen(ui::color(ui::kAccentBright), 1);
            snapPen.setDashPattern(QVector<qreal>{3.0, 3.0});
            painter.setPen(snapPen);
        } else {
            painter.setPen(QPen(ui::color(ui::kAccent), 2));
        }
        painter.drawLine(sx, lanes.top(), sx, lanes.bottom());
        painter.setPen(Qt::NoPen);
        painter.setBrush(snapped ? ui::color(ui::kAccentBright) : ui::color(ui::kAccent));
        painter.drawPolygon(QPolygon() << QPoint(sx - 4, lanes.top()) << QPoint(sx + 4, lanes.top())
                                       << QPoint(sx, lanes.top() + 6));
    }
    painter.restore();

    // Empty-timeline hint (#177): exactly while the model holds no
    // clips the lanes show the next action; it disappears the moment a
    // clip exists (pure paintEvent state, nothing cached).
    if (!model_ || model_->clips().empty()) {
        const qreal dpr = devicePixelRatioF();
        const QPixmap film =
            icons::makeIcon(QStringLiteral("film"), ui::color(ui::kTextDisabled), 28, dpr)
                .pixmap(qRound(28 * dpr), qRound(28 * dpr));
        QFont hintFont = painter.font();
        hintFont.setPixelSize(12);
        const QFontMetrics metrics(hintFont);
        const int gap = 8;
        const int cy = lanes.center().y() - (28 + gap + metrics.height()) / 2;
        painter.save();
        painter.setClipRect(lanes);
        // The pixmap carries the devicePixelRatio, so it draws at its
        // logical 28 px on scaled displays too.
        painter.drawPixmap(lanes.left() + (lanes.width() - 28) / 2, cy, film);
        painter.setFont(hintFont);
        painter.setPen(ui::color(ui::kTextDim));
        painter.drawText(QRect(lanes.left(), cy + 28 + gap, lanes.width(), metrics.height()),
                         Qt::AlignHCenter | Qt::AlignVCenter,
                         tr("Drag media here, or double-click it in the library"));
        painter.restore();
    }

    // The PLAYHEAD spans ruler + lanes (never the header column).
    // Round 5 CapCut-style: a WHITE 1 px line under a white rounded
    // badge with a pointed tip in the ruler band.
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setClipRect(QRect(kHeaderWidth, contentTop(),
                              std::max(0, width() - kHeaderWidth - kHScrollHeight),
                              std::max(0, areaHeight())));
    painter.translate(-scrollX_, 0);
    const int x = frameToX(static_cast<int64_t>(playhead_ * fps_));
    if (x >= kHeaderWidth) {
        painter.setPen(QPen(kSelectionWhite, 1));
        painter.setBrush(kSelectionWhite);
        const int lineTop = contentTop() + 13; // below the badge tip
        painter.drawLine(x, lineTop, x, areaHeight() + contentTop());
        // Badge: rounded 10x9 head + a 5 px pointed tip (the classic
        // CapCut playhead), all white with a hairline dark edge so it
        // reads on the light ruler labels too.
        painter.setPen(QPen(QColor(0, 0, 0, 60), 1));
        painter.drawRoundedRect(QRectF(x - 5.0, contentTop() + 1.0, 10.0, 8.0), 2.5, 2.5);
        painter.setPen(Qt::NoPen);
        painter.drawPolygon(QPolygonF() << QPointF(x - 4.0, contentTop() + 8.5)
                                        << QPointF(x + 4.0, contentTop() + 8.5)
                                        << QPointF(x, contentTop() + 13.5));
    }
    painter.restore();

    // The razor hover line tracks the mouse in WIDGET coordinates.
    drawRazorHover(painter);
}

void TimelinePanel::drawHeaderColumn(QPainter &painter) const {
    const QRect headerArea(0, contentTop() + kRulerHeight, kHeaderWidth,
                           std::max(0, areaHeight() - kRulerHeight));
    painter.fillRect(headerArea, QColor(0x12, 0x12, 0x12));
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
        // Cell background mirrors the lane band so the header row and
        // the lane read as ONE dark stripe across the panel (round 5).
        painter.fillRect(cell,
                         track->isAudio ? kAudioTrack : (track->isText ? kTextTrack : kVideoTrack));
        painter.fillRect(0, cell.top(), 3, kTrackHeight, accent);
        painter.setPen(ui::withAlpha(ui::kLine, 120));
        painter.drawLine(cell.left(), cell.bottom(), cell.right(), cell.bottom());

        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);

        // Row 1: the lane-kind glyph in the track's identity color +
        // the plain name in primary text (round 5: no more filled name
        // chip - CapCut headers are dark and quiet).
        const qreal dpr = devicePixelRatioF();
        const QString glyph = track->isText    ? QStringLiteral("text")
                              : track->isAudio ? QStringLiteral("music")
                                               : QStringLiteral("film");
        const QPixmap typeIcon =
            icons::makeIcon(glyph, accent, 14, dpr).pixmap(qRound(14 * dpr), qRound(14 * dpr));
        painter.drawPixmap(QPoint(9, cell.top() + 5), typeIcon);

        QFont nameFont = painter.font();
        nameFont.setPixelSize(11);
        painter.setFont(nameFont);
        const QFontMetrics nameMetrics(nameFont);
        const QString name = nameMetrics.elidedText(QString::fromStdString(track->name),
                                                    Qt::ElideRight, kHeaderWidth - 30);
        painter.setPen(ui::color(ui::kText));
        painter.drawText(QRect(27, cell.top() + 4, kHeaderWidth - 31, 16),
                         Qt::AlignLeft | Qt::AlignVCenter, name);

        // Row 2: L/M/S toggles as painted glyphs (lock / speaker /
        // star) - the SAME hit geometry headerBoxAt() tests: three
        // 22x16 boxes on a 26 px pitch starting at x 10, band
        // top+32..top+48. Off = dim glyph, on = accent-tinted pill +
        // near-white glyph.
        const bool states[3] = {track->locked, track->muted, track->solo};
        for (int c = 0; c < 3; ++c) {
            const QRect box(10 + c * 26, cell.top() + 32, 22, 16);
            if (states[c]) {
                QColor onTint = accent;
                onTint.setAlpha(60);
                painter.setPen(QPen(accent, 1));
                painter.setBrush(onTint);
            } else {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QBrush());
            }
            painter.drawRoundedRect(box, 4, 4);
            const QPointF center(box.center() + QPointF(0.5, 0.5));
            const QColor glyphColor =
                states[c] ? ui::color(ui::kText) : ui::color(ui::kTextDisabled);
            if (c == 0) {
                paintLockGlyph(painter, center, glyphColor, states[c]);
            } else if (c == 1) {
                paintMuteGlyph(painter, center, glyphColor, states[c]);
            } else {
                paintSoloGlyph(painter, center, glyphColor, states[c]);
            }
        }
        painter.restore();
    }
}

// Round 5 lane striping: one band per row in the lane kind's near-black
// tint + a bottom hairline. Runs inside the lanes clip and the content
// translate, so plain content-space rects are enough (row bands use
// laneRect's y geometry; the x extent covers the full scrollable width).
void TimelinePanel::drawLaneBands(QPainter &painter) const {
    if (!model_) {
        return;
    }
    const int trackCount = model_->trackCount();
    for (int r = 0; r < trackCount; ++r) {
        const fc::Track *track = model_->trackAt(r);
        if (!track) {
            continue;
        }
        const QRect lane = laneRect(r);
        painter.fillRect(lane,
                         track->isAudio ? kAudioTrack : (track->isText ? kTextTrack : kVideoTrack));
        painter.setPen(ui::withAlpha(ui::kLine, 120));
        painter.drawLine(lane.left(), lane.bottom(), lane.right(), lane.bottom());
    }
}

void TimelinePanel::drawRuler(QPainter &painter) const {
    // Content coordinates: the ruler extends over the whole scrollable
    // extent (the viewport clip culls what is off-screen).
    const QRect rulerRect(kHeaderWidth, contentTop(), laneContentWidth(), kRulerHeight);
    painter.fillRect(rulerRect, QColor(0x12, 0x12, 0x12)); // round 5: darker band
    painter.setPen(ui::color(ui::kLine));
    painter.drawLine(rulerRect.left(), rulerRect.bottom(), rulerRect.right(), rulerRect.bottom());
    // Major ticks + 11 px labels (#90/#176).
    painter.setPen(QColor(0x52, 0x52, 0x52));
    QFont labelFont = painter.font();
    labelFont.setPixelSize(ui::kFontSmall);
    painter.setFont(labelFont);

    double step = 1.0;
    // Two label tiers (#176). One step is `step` seconds; its pixel
    // distance is step * fps * pps (pps_ is pixels-per-frame). Fine
    // tier: one-second labels (the old behavior whenever they clear
    // the 96 px label floor). Coarse tier: below that floor the labels
    // climb a human 5/10/30/60-s ladder instead of the old x5 ladder's
    // 25-s/125-s oddities, with the existing minor-tick rhythm in
    // between. The ladder always resolves: pxPerSec >= fps_ * 5 > 5,
    // and 3600 * 5 clears the floor with room to spare.
    const double pxPerSec = fps_ * pps_;
    if (pxPerSec < kRulerLabelMinPx) {
        static const double kCoarseSteps[] = {5.0,   10.0,  30.0,   60.0,  120.0,
                                              300.0, 600.0, 1800.0, 3600.0};
        step = kCoarseSteps[8];
        for (double coarse : kCoarseSteps) {
            if (coarse * pxPerSec >= kRulerLabelMinPx) {
                step = coarse;
                break;
            }
        }
    }
    const fc::FrameRate rate{static_cast<uint32_t>(std::lround(fps_ * 1000.0)), 1000, false};
    for (double t = 0.0; t <= duration_; t += step) {
        const int x = frameToX(static_cast<int64_t>(std::llround(t * fps_)));
        if (x < rulerRect.left() || x > rulerRect.right()) {
            continue;
        }
        painter.drawLine(x, rulerRect.bottom() - 8, x, rulerRect.bottom() - 1);
        const int64_t frames = static_cast<int64_t>(std::llround(t * fps_));
        // #176: tabular figures keep tick-adjacent timecodes in rhythm.
        painter.setPen(ui::color(ui::kTextDim));
        paintTabularText(painter, QRect(x + 3, rulerRect.top(), 96, rulerRect.height()),
                         QString::fromStdString(fc::Timecode::fromFrames(frames, rate).toString()));
        painter.setPen(QColor(0x52, 0x52, 0x52));
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
        painter.setPen(QColor(0x3A, 0x3A, 0x3A)); // round 5: quieter minor ticks
        for (double t = 0.0; t <= duration_; t += minor) {
            const int mx = frameToX(static_cast<int64_t>(std::llround(t * fps_)));
            if (mx < rulerRect.left() || mx > rulerRect.right()) {
                continue;
            }
            painter.drawLine(mx, rulerRect.bottom() - 4, mx, rulerRect.bottom() - 1);
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
    painter.setRenderHint(QPainter::Antialiasing, true); // #89: smooth 6 px clip corners

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
        const bool hovered = !selected && clip.id == hoveredClipId_; // #200

        // #89 selection language, round-5 palette: selection brightens
        // the fill 8% and draws the WHITE outline + corner handles; a
        // hovered clip brightens 4% (never stacked with selection).
        QColor fill = clip.isText ? kTextClipFill : (track->isAudio ? kAudioClipFill : kClipFill);
        if (selected) {
            fill = ui::mix(fill, QColor(0xFF, 0xFF, 0xFF), 0.08);
        } else if (hovered) {
            fill = ui::mix(fill, QColor(0xFF, 0xFF, 0xFF), 0.04);
        }
        // Locked / muted / non-solo lanes render dimmed: one painter
        // opacity covers body, content, outline and chips alike.
        const double dim = trackDimFactor(clip.trackIndex);
        painter.setOpacity(dim);

        // 6 px rounded body - the shape every content layer clips to.
        QPainterPath body;
        body.addRoundedRect(rect, 6, 6);
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawPath(body);

        // ---- content layers, clipped to the body ----
        painter.save();
        painter.setClipPath(body);
        if (clip.isText) {
            // Violet body with a soft vertical sheen.
            QLinearGradient sheen(rect.topLeft(), QPointF(rect.left(), rect.bottom()));
            sheen.setColorAt(0.0, QColor(0xFF, 0xFF, 0xFF, 26));
            sheen.setColorAt(1.0, QColor(0x00, 0x00, 0x00, 40));
            painter.fillRect(rect, sheen);
        } else if (track->isAudio) {
            // REAL waveform (round 5): the worker's peak buckets mapped
            // over the clip's SOURCE extent. At high zoom-out each pixel
            // column maxes every bucket it covers; at high zoom-in
            // buckets repeat - both reads stay honest to the data.
            auto it = waveforms_.find(clip.sourcePath);
            if (it != waveforms_.end() && !it->second.peaks.isEmpty() &&
                it->second.durationSec > 0.0) {
                const WaveformVisual &wf = it->second;
                const int n = wf.peaks.size();
                const double peaksPerSec = double(n) / std::max(0.001, wf.durationSec);
                const double t0 = double(clip.sourceInFrames) / fps_;
                const double t1 = t0 + double(clip.durationFrames()) * clip.rate / fps_;
                const double span = std::max(1e-9, t1 - t0);
                const int cy = (rect.top() + rect.bottom()) / 2;
                const int halfH = std::max(2, rect.height() / 2 - 3);
                painter.setPen(QPen(kWaveform, 1));
                const int bodyW = std::max(1, rect.width());
                for (int x = rect.left() + 1; x < rect.right(); x += 2) {
                    const double f0 = double(x - rect.left()) / double(bodyW);
                    const double f1 = double(x - rect.left() + 2) / double(bodyW);
                    const int i0 = qBound(0, int((t0 + f0 * span) * peaksPerSec), n - 1);
                    const int i1 = qBound(0, int((t0 + f1 * span) * peaksPerSec), n - 1);
                    float peak = 0.0f;
                    for (int i = i0; i <= i1; ++i) {
                        peak = std::max(peak, wf.peaks[i]);
                    }
                    const int h = qMax(1, int(std::lround(peak * halfH)));
                    painter.drawLine(x, cy - h, x, cy + h);
                }
            } else {
                // Fallback while the worker decodes: the old three
                // honest tone hairlines (a flat texture, NOT a fake
                // waveform).
                const QRectF band = QRectF(rect).adjusted(3, 8, -3, -8);
                painter.setPen(QPen(ui::withAlpha(ui::kText, 30), 1));
                for (int i = 0; i < 3; ++i) {
                    const qreal y = band.top() + band.height() * (2 * i + 1) / 6.0;
                    painter.drawLine(QPointF(band.left(), y), QPointF(band.right(), y));
                }
            }
        } else {
            // FILMSTRIP (round 5): the worker's strip tiles the WHOLE
            // source; the clip draws the segment its [in, out] range
            // actually shows, scrolled by the source in-point.
            auto it = strips_.find(clip.sourcePath);
            if (it != strips_.end() && !it->second.image.isNull()) {
                const StripVisual &vis = it->second;
                const qreal dpr =
                    vis.image.devicePixelRatio() > 0.0 ? vis.image.devicePixelRatio() : 1.0;
                const qreal logicalW = qreal(vis.image.width()) / dpr;
                double srcStart = 0.0;
                if (vis.durationSec > 0.0) {
                    srcStart = double(clip.sourceInFrames) / fps_;
                }
                const double pxPerSec = logicalW / std::max(0.001, vis.durationSec);
                int x = rect.left() - qRound(srcStart * pxPerSec);
                const int stride = qMax(1, qRound(logicalW));
                for (; x < rect.right(); x += stride) {
                    painter.drawImage(QPoint(x, rect.top()), vis.image);
                }
            }
            // Dark top gradient so the clip name reads on any footage.
            QLinearGradient grad(QPointF(rect.left(), rect.top()),
                                 QPointF(rect.left(), rect.top() + 18));
            grad.setColorAt(0.0, QColor(0, 0, 0, 150));
            grad.setColorAt(1.0, QColor(0, 0, 0, 0));
            painter.fillRect(rect, grad);
        }
        painter.restore();

        // 1 px inner hairline keeps the body edge on the lane band.
        QPainterPath innerHairline;
        innerHairline.addRoundedRect(rect.adjusted(1, 1, -1, -1), 5, 5);
        painter.setPen(QPen(ui::withAlpha(ui::kLine, 153), 1));
        painter.setBrush(QBrush());
        painter.drawPath(innerHairline);

        // Clip label: top-left, elided, over the content layers
        // (CapCut-style in-clip title).
        QFont labelFont = painter.font();
        labelFont.setPixelSize(10);
        painter.setFont(labelFont);
        const QFontMetrics labelMetrics(labelFont);
        const QString labelText = labelMetrics.elidedText(
            QString::fromStdString(clip.label), Qt::ElideRight, std::max(12, rect.width() - 12));
        painter.setPen(kLabel);
        painter.drawText(rect.adjusted(6, 1, -6, 0), Qt::AlignLeft | Qt::AlignVCenter, labelText);
        painter.setFont(QFont());

        if (selected) {
            // CapCut selection: 2 px WHITE outline + four corner
            // handles (the trim targets the cursor already finds).
            painter.setPen(QPen(kSelectionWhite, 2));
            painter.setBrush(QBrush());
            painter.drawPath(body);
            painter.setPen(QPen(QColor(0, 0, 0, 90), 1));
            painter.setBrush(kSelectionWhite);
            const QPointF corners[4] = {
                QPointF(rect.left(), rect.top()), QPointF(rect.right(), rect.top()),
                QPointF(rect.left(), rect.bottom()), QPointF(rect.right(), rect.bottom())};
            for (const QPointF &corner : corners) {
                painter.drawEllipse(corner, 3.5, 3.5);
            }
        } else if (hovered) {
            // #200: 1 px white outline at low alpha.
            painter.setPen(QPen(QColor(255, 255, 255, 96), 1));
            painter.setBrush(QBrush());
            painter.drawPath(body);
        }
        // Speed chip (#9): corner badge on rate-adjusted clips; double-
        // click opens the Speed / Duration dialog (mouseDoubleClickEvent).
        if (std::fabs(clip.rate - 1.0) > 1e-3) {
            const QRect chip = speedChipRect(clip, clip.trackIndex);
            painter.setBrush(QColor(0, 0, 0, 150));
            painter.setPen(Qt::NoPen);
            painter.drawRoundedRect(chip, 7, 7);
            painter.setPen(kLabel);
            painter.setFont(QFont(QString::fromLatin1("Segoe UI"), 7, QFont::Bold));
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
            painter.setBrush(QColor(0, 0, 0, 150));
            painter.setPen(Qt::NoPen);
            painter.drawRoundedRect(chip, 6, 6);
            painter.setPen(ui::color(ui::kTextDim));
            painter.setFont(QFont(QString::fromLatin1("Segoe UI"), 7, QFont::Bold));
            painter.drawText(chip, Qt::AlignCenter,
                             QStringLiteral("fx %1").arg(clip.effectStack.size()));
            painter.setFont(QFont()); // restore default for the next clip
        }
        // teal "T" badge on text clips (right-aligned, clear of the
        // speed chip above and the fx chip below).
        if (clip.isText) {
            QColor tColor(0x1A, 0xBC, 0x9C);
            painter.setPen(tColor);
            painter.setFont(QFont(QString::fromLatin1("Segoe UI"), 8, QFont::Bold));
            painter.drawText(QRect(rect.right() - 56, rect.top(), 26, rect.height()),
                             Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("T"));
            painter.setFont(QFont()); // restore default for the next clip
        }
        painter.setOpacity(1.0); // the dim factor is per-clip only
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
        // the current lane/spot cannot host the clip. Rounded to the
        // same 6 px corners the #89 clip body uses. #201: painted at
        // painter opacity 0.7 over the full-strength ghost color.
        const QColor &c = ghostValid_ ? kGhostFill : kGhostBad;
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setOpacity(0.7);
        painter.setBrush(c);
        painter.setPen(c);
        painter.drawRoundedRect(rect, 6, 6);
        painter.restore();
        return;
    }
    if (dragMode_ == DragMode::TrimStart || dragMode_ == DragMode::TrimEnd) {
        // Preview of the new in/out edge position. #172: the same 6 px
        // corner language as every other clip-shaped ghost.
        const QRect lane = laneRect(clip->trackIndex);
        const int64_t start = dragMode_ == DragMode::TrimStart ? ghostStart_ : clip->timelineStart;
        const int64_t end = dragMode_ == DragMode::TrimEnd ? ghostEnd_ : clip->timelineEnd();
        const int x0 = frameToX(start);
        const int x1 = frameToX(end);
        const QRect rect(x0, lane.top() + 2, std::max(8, x1 - x0), lane.height() - 4);
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setBrush(QColor(0x9F, 0xC7, 0xE0, 60));
        painter.setPen(QColor(0x9F, 0xC7, 0xE0, 200));
        painter.drawRoundedRect(rect, 6, 6);
        painter.restore();
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
    // #202: the ClosedHand cursor belongs to an active drag only; the
    // resting cursor follows the razor mode.
    setCursor(razorMode_ ? Qt::CrossCursor : Qt::ArrowCursor);
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
            hoveredClipId_ = -1;             // #200: no hover paint mid-drag
            setCursor(Qt::ClosedHandCursor); // #202: grabbing
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
        hoveredClipId_ = -1; // #200: razor mode shows the cut line, not hover
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
        // #200: remember the clip under the mouse for the hover paint.
        const int64_t hoverId = clip ? clip->id : -1;
        if (hoverId != hoveredClipId_) {
            hoveredClipId_ = hoverId;
            update();
        }
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
    hoveredClipId_ = -1; // #200: nothing is hovered off-widget
    if (dragMode_ == DragMode::None) {
        // #202: leaving the clip area resets the cursor (unless a drag
        // owns it - the implicit grab keeps ClosedHand until release).
        setCursor(razorMode_ ? Qt::CrossCursor : Qt::ArrowCursor);
    }
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

    // Wheel routing (#12 + #184): pixelDelta wins over angleDelta when
    // present (hiDPI precision scrolling). Over the LANES, plain Y
    // scrolls the timeline horizontally and Shift+Y scrolls the track
    // stack vertically; over the RULER band, Y is horizontal too. A
    // real horizontal wheel delta always scrolls horizontally. (The
    // Ctrl+wheel zoom above keeps precedence over all of this.)
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
    // QWheelEvent::pos() is deprecated from Qt 5.15 (replaced by
    // position(), which does not exist on the 5.12 build floor).
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    const int wheelY = static_cast<int>(event->position().y());
#else
    const int wheelY = event->pos().y();
#endif
    const bool overRuler = wheelY >= contentTop() && wheelY < contentTop() + kRulerHeight;
    const bool wantVertical = !overRuler && (event->modifiers() & Qt::ShiftModifier);

    if (wantVertical && dy != 0 && vscroll_ && vscroll_->maximum() > 0) {
        vscroll_->setValue(std::min(std::max(vscroll_->value() - dy, 0), vscroll_->maximum()));
        event->accept();
        return;
    }
    const int horizontal = dx != 0 ? dx : (wantVertical ? 0 : dy);
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
    // #86: the segmented control mirrors razorMode_ (0 = Select,
    // 1 = Blade). setCurrent on the already-current index is a no-op, so
    // the setRazorMode feedback cannot loop.
    if (toolSegment_) {
        toolSegment_->setCurrent(razorMode_ ? 1 : 0);
    }
    if (rippleTool_) {
        rippleTool_->setChecked(rippleEnabled_);
    }
}

// The header cell's L/M/S box under pos (-1 = none), scroll-aware:
// the box rects mirror drawHeaderColumn's geometry exactly (round 5:
// the boxes live at cell top+32..top+48).
int TimelinePanel::headerBoxAt(const QPoint &pos, int row) const {
    const int cellTop = contentTop() + kRulerHeight + row * kTrackHeight - scrollY_;
    const int relY = pos.y() - cellTop;
    if (relY < 32 || relY > 48) {
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
    // #179: the menu items carry the same glyphs the header L/M/S
    // boxes paint (no clip context menu exists in this panel - Delete
    // is a key and split is a razor click - so the existing items are
    // the ones that get icons).
    const char *glyphs[3] = {"lock", "mute", "star"};
    for (int c = 0; c < 3; ++c) {
        QAction *action =
            menu.addAction(icons::makeIcon(QString::fromLatin1(glyphs[c]), ui::color(ui::kText), 16,
                                           devicePixelRatioF()),
                           tr(labels[c]));
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
