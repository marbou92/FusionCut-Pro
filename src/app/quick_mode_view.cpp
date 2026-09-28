#include "quick_mode_view.h"

#include <QComboBox>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLinearGradient>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QStyleOptionSlider>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <cmath>

#include "timecode.h"
#include "ui_theme.h"
#include "ui_widgets.h"

// ui_theme.h tokens live in fc::ui; the panel addresses them as ui::...
using namespace fc;

namespace {
constexpr int kMaxSlider = 100000;

// Dynamic QObject property on the play button that mirrors the playing
// state (see setPlaying) - state, not a translated label.
constexpr char kPlayingProperty[] = "fcPlaying";

// Floating tool bar (#75/#137): 16px stroke glyphs beside the labels.
constexpr int kToolbarIconSize = 16;
// Circular transport buttons (#147): 36px rounds, 18px glyphs.
constexpr int kTransportSize = 36;
constexpr int kTransportIconSize = 18;
constexpr int kStepIconSize = 14;
constexpr int kEmptyIconSize = 44;
constexpr int kCardIconSize = 26;
// Library grid (round 4): 88x50 covers in 104x74 cards, 2 columns in a
// 248px panel. The old horizontal strip became CapCut's library grid.
constexpr int kTileW = 88;
constexpr int kTileH = 50;
constexpr int kGridCardW = 104;
constexpr int kGridCardH = 74;
constexpr int kLibraryWidth = 248;
constexpr int kDetailsWidth = 236;
constexpr int kLibraryColumns = 2;
constexpr int kGridGlyphSize = 20;
constexpr int kImportCircleSize = 28;
// Export pill (#143): chevron on the right of the text.
constexpr int kExportChevronSize = 14;

// Circular transport style (#147): elevated fill, hairline, hover
// brighten, press inset - QuickTime's rounded transport buttons.
QString circularTransportStyle() {
    return QStringLiteral("QToolButton { background: %1; border: 1px solid %2; "
                          "border-radius: %5px; padding: 0; }"
                          "QToolButton:hover { background: %3; }"
                          "QToolButton:pressed { background: %4; }")
        .arg(ui::color(ui::kSurface3).name(), ui::color(ui::kLine).name(),
             ui::color(ui::kSurfaceHover).name(), ui::color(ui::kSurfacePress).name())
        .arg(QString::number(kTransportSize / 2));
}

QToolButton *circularTransportButton(const QString &iconName, const QString &tooltip,
                                     const QString &accessibleName, QWidget *parent) {
    auto *button = new QToolButton(parent);
    button->setIcon(icons::makeIcon(iconName, ui::color(ui::kText), kTransportIconSize,
                                    parent->devicePixelRatioF()));
    button->setIconSize(QSize(kTransportIconSize, kTransportIconSize));
    button->setFixedSize(kTransportSize, kTransportSize);
    button->setStyleSheet(circularTransportStyle());
    button->setToolTip(tooltip);
    button->setAccessibleName(accessibleName); // #96: icon-only needs a name
    return button;
}

// Icon-first tool button (#76): the app-wide sheet supplies the neutral
// chrome (no per-button light styling here). `fcAccent` on Import Media…
// is set by the caller - it is the ONLY accent-filled control in the
// zone (#138).
QPushButton *toolbarButton(const QString &text, const QString &tooltip, const QString &iconName,
                           const QColor &iconColor, qreal dpr, QWidget *parent) {
    auto *button = new QPushButton(text, parent);
    button->setIcon(icons::makeIcon(iconName, iconColor, kToolbarIconSize, dpr));
    button->setIconSize(QSize(kToolbarIconSize, kToolbarIconSize));
    button->setToolTip(tooltip);
    button->setAccessibleName(text);
    return button;
}

// Thin separator between the tool bar's tools (#137).
QFrame *toolbarSeparator(QWidget *parent) {
    auto *separator = new QFrame(parent);
    separator->setFixedSize(1, 22);
    separator->setStyleSheet(QStringLiteral("background: %1;").arg(ui::color(ui::kLine).name()));
    return separator;
}

// CapCut import tile's accent circle: a filled accent disc with a white
// plus glyph - the tile's whole point is being findable on a dark page.
QPixmap accentPlusPixmap(qreal dpr) {
    const qreal ratio = dpr > 0 ? dpr : 1.0;
    QPixmap pm(qRound(kImportCircleSize * ratio), qRound(kImportCircleSize * ratio));
    pm.setDevicePixelRatio(ratio);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(ui::color(ui::kAccent));
    p.drawEllipse(QRectF(0, 0, kImportCircleSize, kImportCircleSize));
    const QPixmap glyph = icons::makeIcon(QStringLiteral("plus"), QColor(Qt::white), 14, ratio)
                              .pixmap(qRound(14 * ratio), qRound(14 * ratio));
    p.drawPixmap(QPointF((kImportCircleSize - 14) / 2.0, (kImportCircleSize - 14) / 2.0), glyph);
    return pm;
}

// Shared 88x50 cover tile (#77/#136): rounded kSurface3->kCard gradient
// with a centered dim glyph and a 1px hairline - drawn once, at dpr.
QPixmap gradientTilePixmap(const QString &glyphName, qreal dpr, int radius) {
    const qreal w = kTileW;
    const qreal h = kTileH;
    const qreal ratio = dpr > 0 ? dpr : 1.0;
    QPixmap pm(qRound(w * ratio), qRound(h * ratio));
    pm.setDevicePixelRatio(ratio);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath card;
    card.addRoundedRect(QRectF(0.5, 0.5, w - 1.0, h - 1.0), radius, radius);
    QLinearGradient grad(0, 0, 0, h);
    grad.setColorAt(0.0, ui::color(ui::kSurface3));
    grad.setColorAt(1.0, ui::color(ui::kCard));
    p.fillPath(card, grad);
    p.setPen(QPen(ui::color(ui::kLine), 1));
    p.setBrush(Qt::NoBrush);
    p.drawPath(card);
    // Request the stored pixmap at its native pixel size so it comes
    // back with the devicePixelRatio tag intact (the DPR-aware
    // QIcon::pixmap(QSize, qreal) overload is Qt 6 only).
    const QPixmap glyph =
        icons::makeIcon(glyphName, ui::color(ui::kTextDim), kGridGlyphSize, ratio)
            .pixmap(QSize(qRound(kGridGlyphSize * ratio), qRound(kGridGlyphSize * ratio)));
    p.drawPixmap(QPointF((w - kGridGlyphSize) / 2.0, (h - kGridGlyphSize) / 2.0), glyph);
    return pm;
}

// Template cover (#146): the gradient tile plus a CapCut-style name
// chip - a black 55% scrim with 10px near-white text - pinned to the
// bottom-left. The chip draws over the centered glyph on purpose
// (scrim look); the glyph stays centered as required.
QPixmap templateCardPixmap(const QString &glyphName, const QString &name, qreal dpr) {
    QPixmap pm = gradientTilePixmap(glyphName, dpr, ui::kRadiusControl);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QFont chipFont = p.font();
    chipFont.setPixelSize(10);
    const QFontMetrics fm(chipFont);
    const QString elided = fm.elidedText(name, Qt::ElideRight, kTileW - 18);
    const qreal chipW = qMin<qreal>(kTileW - 8.0, fm.horizontalAdvance(elided) + 10.0);
    const QRectF chip(4.0, kTileH - 20.0, chipW, 16.0);
    QPainterPath chipPath;
    chipPath.addRoundedRect(chip, 4, 4);
    QColor scrim(Qt::black); // #146: black 55% alpha (not a hex literal)
    scrim.setAlpha(140);
    p.fillPath(chipPath, scrim);
    p.setFont(chipFont);
    p.setPen(ui::color(ui::kText)); // near-white, token-pure
    p.drawText(chip.adjusted(5, 0, -5, 0), Qt::AlignVCenter | Qt::AlignLeft, elided);
    return pm;
}

// Library card cover (#136): the real thumbnail cropped to fill the
// tile, DPR-aware, clipped to a 6px rounded rect by the painter.
QPixmap mediaCoverPixmap(const QImage &thumb, qreal dpr) {
    const qreal w = kTileW;
    const qreal h = kTileH;
    const qreal ratio = dpr > 0 ? dpr : 1.0;
    QPixmap pm(qRound(w * ratio), qRound(h * ratio));
    pm.setDevicePixelRatio(ratio);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(0, 0, w, h), 6, 6);
    p.setClipPath(clip);
    if (thumb.width() > 0 && thumb.height() > 0) {
        // Cover-fit: scale until both edges reach the tile, center the
        // overflow, and let the rounded clip trim the rest.
        const qreal scale = qMax(w / thumb.width(), h / thumb.height());
        const qreal dw = thumb.width() * scale;
        const qreal dh = thumb.height() * scale;
        p.drawImage(QRectF((w - dw) / 2.0, (h - dh) / 2.0, dw, dh), thumb);
    }
    p.setClipping(false);
    p.setPen(QPen(ui::color(ui::kLine), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(0.5, 0.5, w - 1.0, h - 1.0), 6, 6);
    return pm;
}

// Details panel cells (round 4): dim 11px keys, light 11px values - the
// token pair keeps the panel readable on kSurface2.
QLabel *detailKeyLabel(const QString &text) {
    auto *label = new QLabel(text);
    label->setStyleSheet(QStringLiteral("color: %1; font-size: %2px;")
                             .arg(ui::color(ui::kTextDim).name())
                             .arg(QString::number(ui::kFontSmall)));
    label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    return label;
}

QLabel *detailValueLabel(bool wrap) {
    auto *value = new QLabel(QStringLiteral("-"));
    value->setStyleSheet(QStringLiteral("color: %1; font-size: %2px;")
                             .arg(ui::color(ui::kText).name())
                             .arg(QString::number(ui::kFontSmall)));
    value->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    value->setWordWrap(wrap);
    if (wrap) {
        // The save path is the one value worth copying out of the panel.
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    }
    return value;
}

// Painted position slider (#148): a 2px rounded groove across the full
// width, an accent fill from 0 to the value, and a 12px kText handle
// with a 1px kLine border centered in the 20px hit area. Paint-only on
// purpose: QSlider keeps every interaction (click, drag, keyboard) and
// the handle rect is read from the style so painted geometry and the
// base-class hit geometry can never drift apart. No animation.
class fcQuickSlider : public QSlider {
public:
    explicit fcQuickSlider(QWidget *parent = nullptr) : QSlider(Qt::Horizontal, parent) {
        setFixedHeight(20); // the hit area the handle is centered in
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        QStyleOptionSlider opt;
        initStyleOption(&opt);
        const QRect handleRect =
            style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
        const qreal centerY = height() / 2.0;

        const qreal grooveH = 2.0;
        QPainterPath groovePath;
        groovePath.addRoundedRect(QRectF(0.0, centerY - grooveH / 2.0, width(), grooveH), 1.0, 1.0);
        painter.fillPath(groovePath, ui::color(ui::kLine));

        if (handleRect.isValid()) {
            const qreal handleCenterX = handleRect.center().x();
            if (value() > minimum()) {
                QPainterPath fillPath;
                fillPath.addRoundedRect(
                    QRectF(0.0, centerY - grooveH / 2.0, handleCenterX, grooveH), 1.0, 1.0);
                painter.fillPath(fillPath, isEnabled() ? ui::color(ui::kAccent)
                                                       : ui::mix(ui::color(ui::kLine),
                                                                 ui::color(ui::kTextDim), 0.5));
            }
            const QRectF handle(handleCenterX - 6.0, centerY - 6.0, 12.0, 12.0);
            QPainterPath handlePath;
            handlePath.addEllipse(handle);
            painter.fillPath(handlePath,
                             isEnabled() ? ui::color(ui::kText) : ui::color(ui::kTextDisabled));
            painter.setPen(QPen(ui::color(ui::kLine), 1));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(handle);
        }
    }
};
} // namespace

QuickModeView::QuickModeView(QWidget *parent) : QWidget(parent) {
    setAcceptDrops(true); // full-page media drop zone (#53)

    canvas_ = new PreviewCanvas(this);

    // Hero empty state (#74): overlays the canvas and hides once a
    // program duration is known (see setMedia). It is opaque in the
    // canvas tone, so it also covers the dim "No media loaded" text the
    // shared PreviewCanvas paints underneath. lower() keeps the
    // canvas's quality dropdown above it and clickable.
    emptyState_ = new fc::EmptyState(canvas_);
    emptyState_->setObjectName(QStringLiteral("fcQuickEmpty"));
    emptyState_->setAttribute(Qt::WA_StyledBackground, true);
    emptyState_->setStyleSheet(QStringLiteral("QWidget#fcQuickEmpty { background: %1; }")
                                   .arg(ui::color(ui::kCanvas).name()));
    emptyState_->setIcon(
        icons::makeIcon("film", ui::color(ui::kTextDisabled), kEmptyIconSize, devicePixelRatioF()));
    emptyState_->setTitle(tr("Start your project"));
    emptyState_->setHint(tr("Drop files anywhere, or bring in media to begin"));
    emptyState_->addAction(tr("Import Media…"), true, [this] { emit importRequested(); });
    emptyState_->addAction(tr("Use a Template"), false, [this] {
        // Same handler the template cards use: the title-broll prefill
        // is the only one MainWindow fully realizes today.
        emit templateRequested(QStringLiteral("title-broll"));
    });
    auto *canvasLayout = new QVBoxLayout(canvas_);
    canvasLayout->setContentsMargins(0, 0, 0, 0);
    canvasLayout->addWidget(emptyState_);
    emptyState_->lower();

    // Transport buttons (#147): 36px circles, QuickTime order, built
    // here and placed by buildTransportRow().
    playButton_ =
        circularTransportButton(QStringLiteral("play"), tr("Play (Space)"), tr("Play"), this);
    stepBack_ = circularTransportButton(QStringLiteral("step-back"), tr("Previous frame (Left)"),
                                        tr("Previous frame"), this);
    stepFwd_ = circularTransportButton(QStringLiteral("step-fwd"), tr("Next frame (Right)"),
                                       tr("Next frame"), this);

    // Painted position slider (#148) + timecode chip (#149).
    position_ = new fcQuickSlider(this);
    position_->setRange(0, kMaxSlider);
    position_->setEnabled(false); // a duration arrives with setMedia()
    timecode_ = new QLabel("00:00:00:00", this);
    timecode_->setStyleSheet(QStringLiteral("font-size: %1px; color: %2;")
                                 .arg(QString::number(ui::kFontSmall))
                                 .arg(ui::color(ui::kTextDim).name()));
    timecode_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    // Tabular figures stand-in: Qt 5 has no font-feature API, so the
    // chip is sized to its widest possible text ("current / total")
    // and never jitters while playing.
    timecode_->setMinimumWidth(
        timecode_->fontMetrics().horizontalAdvance(QStringLiteral("00:00:00:00 / 00:00:00:00")) +
        2);
    timecode_->setEnabled(false);

    // Round 4: the CapCut editor columns - Import library (left),
    // Player (center), Details (right) - over the shared top bar, with
    // the tool bar + template covers as the bottom stack.
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 10, 12, 10);
    root->setSpacing(8);
    root->addWidget(buildTopBar());
    auto *columns = new QHBoxLayout;
    columns->setSpacing(8);
    columns->addWidget(buildLibraryPanel());
    columns->addWidget(buildPlayerColumn(), 1);
    columns->addWidget(buildDetailsPanel());
    root->addLayout(columns, 1);
    root->addWidget(buildToolbar());
    root->addWidget(buildTemplateStrip());

    connect(playButton_, &QToolButton::clicked, this, [this] {
        // The playing STATE lives on the button as a dynamic property
        // kept in sync by setPlaying(); the icon swap follows the same
        // state. An unset property reads false = not playing, matching
        // the initial play glyph.
        const bool playing = playButton_->property(kPlayingProperty).toBool();
        emit playToggled(!playing);
    });
    connect(stepBack_, &QToolButton::clicked, this, [this] { emit stepRequested(-1); });
    connect(stepFwd_, &QToolButton::clicked, this, [this] { emit stepRequested(1); });
    auto seekBySlider = [this](int value) {
        if (duration_ <= 0.0) {
            return;
        }
        const double seconds = static_cast<double>(value) / kMaxSlider * duration_;
        pos_ = seconds;
        refreshTimecode();
        emit seekRequested(seconds);
    };
    connect(position_, &QSlider::sliderMoved, this, seekBySlider);
    // Groove clicks / keyboard moves fire valueChanged without a
    // sliderMoved; setPosition()'s blockSignals keeps programmatic
    // updates silent, and during a drag the handle is "down".
    connect(position_, &QSlider::valueChanged, this, [this, seekBySlider](int value) {
        if (!position_->isSliderDown()) {
            seekBySlider(value);
        }
    });
    refreshTimecode();

    // Children (canvas, buttons, labels, the empty state, the library
    // grid's viewport and cards) would swallow drag events before the
    // page sees them: forward every drag event that lands on ANY
    // descendant widget back into this page's own handlers via an
    // event filter (#53). Cards built later (rebuildMediaGrid) install
    // the filter when they are created. Called after the UI is built.
    const QList<QWidget *> children = findChildren<QWidget *>();
    for (QWidget *child : children) {
        child->installEventFilter(this);
    }
}

QWidget *QuickModeView::buildTopBar() {
    auto *bar = new QWidget(this);
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    // #135: the step rail leads the top bar; its trailing stretch
    // pushes the aspect pill + Export pill to the right edge.
    layout->addWidget(buildStepRail());

    aspectBox_ = new QComboBox(bar);
    aspectBox_->addItem(tr("16:9"));
    aspectBox_->addItem(tr("9:16"));
    aspectBox_->addItem(tr("1:1"));
    aspectBox_->addItem(tr("4:3"));
    aspectBox_->addItem(tr("Custom"));
    // Quick Mode has no way to enter a custom aspect, so the entry is
    // DISABLED (with a tooltip) instead of silently doing nothing when
    // chosen. QComboBox's default model is a QStandardItemModel; the
    // cast is guarded so an exotic model only degrades to the old
    // inert-choice behavior. The aspects[] > 0 check below stays as the
    // belt-and-suspenders no-op for a programmatic selection.
    if (auto *items = qobject_cast<QStandardItemModel *>(aspectBox_->model())) {
        if (QStandardItem *custom = items->item(aspectBox_->count() - 1)) {
            custom->setEnabled(false);
            custom->setToolTip(tr("Custom aspect ratios are configured in Pro Mode"));
        }
    }
    connect(aspectBox_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                const double aspects[] = {16.0 / 9.0, 9.0 / 16.0, 1.0, 4.0 / 3.0, 0.0};
                if (index >= 0 && index < 5 && aspects[index] > 0.0) {
                    canvas_->setAspectHint(aspects[index]);
                }
            });

    // #144: pill restyle - local QSS built from tokens only (height 28,
    // pill radius, kSurface3 fill, kLine hairline, accent border while
    // open/hovered). The global sheet keeps styling the popup.
    aspectBox_->setObjectName(QStringLiteral("fcAspectPill"));
    aspectBox_->setFixedHeight(28);
    aspectBox_->setStyleSheet(
        QStringLiteral("QComboBox#fcAspectPill { background: %1; border: 1px solid %2; "
                       "border-radius: %5px; padding: 4px 12px; color: %3; }"
                       "QComboBox#fcAspectPill:hover, QComboBox#fcAspectPill:on "
                       "{ border-color: %4; }"
                       "QComboBox#fcAspectPill::drop-down { border: none; width: 18px; }")
            .arg(ui::color(ui::kSurface3).name(), ui::color(ui::kLine).name(),
                 ui::color(ui::kText).name(), ui::color(ui::kAccent).name())
            .arg(QString::number(ui::kRadiusPill)));

    layout->addWidget(new QLabel(tr("Aspect:"), bar));
    layout->addWidget(aspectBox_);

    // #143: the Export pill - the top bar's one accent control. The
    // chevron sits on the RIGHT of the text (CapCut), so the pill hosts
    // two mouse-transparent labels inside the button; clicks and the
    // button role stay on the QToolButton itself.
    const qreal dpr = devicePixelRatioF() > 0 ? devicePixelRatioF() : 1.0;
    auto *exportPill = new QToolButton(bar);
    exportPill->setObjectName(QStringLiteral("fcExportPill"));
    exportPill->setFixedHeight(28);
    exportPill->setCursor(Qt::PointingHandCursor);
    exportPill->setToolTip(tr("Export the movie (Ctrl+M)"));
    exportPill->setAccessibleName(tr("Export"));
    exportPill->setStyleSheet(
        QStringLiteral("QToolButton#fcExportPill { background: %1; border: none; "
                       "border-radius: %3px; }"
                       "QToolButton#fcExportPill:hover { background: %2; }"
                       "QToolButton#fcExportPill:pressed { background: %1; }")
            .arg(ui::color(ui::kAccent).name(), ui::color(ui::kAccentBright).name())
            .arg(QString::number(ui::kRadiusPill)));
    auto *pillLayout = new QHBoxLayout(exportPill);
    pillLayout->setContentsMargins(16, 0, 12, 0);
    pillLayout->setSpacing(4);
    auto *pillText = new QLabel(tr("Export"), exportPill);
    pillText->setAttribute(Qt::WA_TransparentForMouseEvents);
    pillText->setStyleSheet(
        QStringLiteral("color: %1; font-weight: 600;").arg(ui::color(ui::kOnAccent).name()));
    auto *pillChevron = new QLabel(exportPill);
    pillChevron->setAttribute(Qt::WA_TransparentForMouseEvents);
    pillChevron->setPixmap(
        icons::makeIcon(QStringLiteral("chevron-right"), ui::color(ui::kOnAccent),
                        kExportChevronSize, dpr)
            .pixmap(qRound(kExportChevronSize * dpr), qRound(kExportChevronSize * dpr)));
    pillLayout->addWidget(pillText);
    pillLayout->addWidget(pillChevron);
    connect(exportPill, &QToolButton::clicked, this, [this] { emit exportRequested(); });
    layout->addWidget(exportPill);
    return bar;
}

QWidget *QuickModeView::buildStepRail() {
    auto *rail = new QWidget(this);
    auto *layout = new QHBoxLayout(rail);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    // Numbered progress rail (#73, tightened by #145): 26px chips,
    // 11px text, 1px connectors. applyStepStyles() picks the text
    // per state: numbered while current/future, plain under the
    // checkmark once completed.
    const QString labels[] = {tr("Import"), tr("Arrange"), tr("Export")};
    const QString tooltips[] = {tr("Step 1: import media files (opens the import dialog)."),
                                tr("Arrange clips on the timeline"),
                                tr("Step 3: open the export dialog to render the timeline.")};
    for (int i = 0; i < 3; ++i) {
        auto *chip = new QPushButton(labels[i], rail);
        chip->setFlat(true);
        chip->setToolTip(tooltips[i]);
        chip->setFixedHeight(26); // #145: compact chips
        chip->setCursor(Qt::PointingHandCursor);
        chip->setIconSize(QSize(kStepIconSize, kStepIconSize));
        connect(chip, &QPushButton::clicked, this, [this, i] {
            // Signal wiring unchanged from the round-1 rail: Import
            // opens the import dialog, Export opens the export dialog,
            // Arrange is the timeline itself. MainWindow keeps driving
            // the rail state via setStep().
            if (i == 0) {
                emit importRequested();
            } else if (i == 2) {
                emit exportRequested();
            }
        });
        stepChips_[i] = chip;
        layout->addWidget(chip);
        if (i < 2) {
            auto *link = new QWidget(rail);
            link->setFixedSize(16, 1); // #145: 1px connector, tightened
            stepLinks_[i] = link;
            layout->addWidget(link);
        }
    }
    layout->addStretch(1);
    applyStepStyles();
    return rail;
}

void QuickModeView::applyStepStyles() {
    const QString accent = ui::color(ui::kAccent).name();
    const QString accentBright = ui::color(ui::kAccentBright).name();
    const QString onAccent = ui::color(ui::kOnAccent).name();
    const QString surface3 = ui::color(ui::kSurface3).name();
    const QString hover = ui::mix(ui::color(ui::kSurface3), ui::color(ui::kLine), 0.5).name();
    const QString text = ui::color(ui::kText).name();
    const QString textDim = ui::color(ui::kTextDim).name();
    const QString line = ui::color(ui::kLine).name();
    const QString numbered[] = {tr("1 · Import"), tr("2 · Arrange"), tr("3 · Export")};
    const QString plain[] = {tr("Import"), tr("Arrange"), tr("Export")};
    const QIcon check =
        icons::makeIcon("check", ui::color(ui::kAccent), kStepIconSize, devicePixelRatioF());
    for (int i = 0; i < 3; ++i) {
        if (stepChips_[i] == nullptr) {
            continue;
        }
        if (i == step_) {
            // Current step: the one accent pill, and the action itself.
            stepChips_[i]->setIcon(QIcon());
            stepChips_[i]->setText(numbered[i]);
            stepChips_[i]->setStyleSheet(
                QStringLiteral("QPushButton { background: %1; color: %2; font-weight: bold; "
                               "font-size: %4px; border-radius: 13px; padding: 2px 12px; }"
                               "QPushButton:hover { background: %3; }")
                    .arg(accent, onAccent, accentBright, QString::number(ui::kFontSmall)));
        } else if (i < step_) {
            // Completed step: neutral chip, accent checkmark (#73).
            stepChips_[i]->setIcon(check);
            stepChips_[i]->setText(plain[i]);
            stepChips_[i]->setStyleSheet(
                QStringLiteral("QPushButton { background: %1; color: %2; font-size: %4px; "
                               "border-radius: 13px; padding: 2px 12px; }"
                               "QPushButton:hover { background: %3; }")
                    .arg(surface3, text, hover, QString::number(ui::kFontSmall)));
        } else {
            // Future step: neutral, dim.
            stepChips_[i]->setIcon(QIcon());
            stepChips_[i]->setText(numbered[i]);
            stepChips_[i]->setStyleSheet(
                QStringLiteral("QPushButton { background: %1; color: %2; font-size: %4px; "
                               "border-radius: 13px; padding: 2px 12px; }"
                               "QPushButton:hover { background: %3; color: %5; }")
                    .arg(surface3, textDim, hover, QString::number(ui::kFontSmall), text));
        }
    }
    for (int i = 0; i < 2; ++i) {
        if (stepLinks_[i] != nullptr) {
            stepLinks_[i]->setStyleSheet(QStringLiteral("background: %1;").arg(line));
        }
    }
}

void QuickModeView::setStep(int step) {
    if (step < 0) {
        step = 0;
    }
    if (step > 2) {
        step = 2;
    }
    step_ = step;
    applyStepStyles();
}

QWidget *QuickModeView::buildLibraryPanel() {
    // CapCut's Import panel (owner screenshot 4): a dashed drop tile
    // over the media grid. The panel is a fixed-width card so the
    // player keeps the center of the window at every dock size.
    libraryPanel_ = new QFrame(this);
    libraryPanel_->setObjectName(QStringLiteral("fcQuickLibrary"));
    libraryPanel_->setStyleSheet(
        QStringLiteral("QFrame#fcQuickLibrary { background: %1; border: 1px solid %2; "
                       "border-radius: %3px; }")
            .arg(ui::color(ui::kSurface2).name(), ui::color(ui::kLine).name())
            .arg(QString::number(ui::kRadiusCard)));
    libraryPanel_->setFixedWidth(kLibraryWidth);

    auto *layout = new QVBoxLayout(libraryPanel_);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    // The dashed Import tile: accent circle + label (CapCut's blue
    // plus disc), the panel's only interactive element above the grid.
    auto *tile = new QToolButton(libraryPanel_);
    tile->setObjectName(QStringLiteral("fcQuickImportTile"));
    tile->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    tile->setIcon(QIcon(accentPlusPixmap(devicePixelRatioF())));
    tile->setIconSize(QSize(kImportCircleSize, kImportCircleSize));
    tile->setText(tr("Import"));
    tile->setToolTip(tr("Drag and drop videos, photos, and audio files here, or click to browse"));
    tile->setAccessibleName(tr("Import"));
    tile->setFixedHeight(78);
    tile->setCursor(Qt::PointingHandCursor);
    tile->setStyleSheet(
        QStringLiteral("QToolButton#fcQuickImportTile { background: transparent; "
                       "border: 1px dashed %1; border-radius: %3px; color: %2; "
                       "font-weight: 600; font-size: %4px; padding: 4px 6px 2px 6px; }"
                       "QToolButton#fcQuickImportTile:hover { border-color: %5; background: %6; }"
                       "QToolButton#fcQuickImportTile:pressed { background: transparent; }")
            .arg(ui::color(ui::kLine).name(), ui::color(ui::kText).name())
            .arg(QString::number(ui::kRadiusCard))
            .arg(QString::number(ui::kFontBody))
            .arg(ui::color(ui::kAccent).name(), ui::color(ui::kSurface3).name()));
    connect(tile, &QToolButton::clicked, this, [this] { emit importRequested(); });
    layout->addWidget(tile);

    auto *hint =
        new QLabel(tr("Drag and drop videos, photos, and audio files here"), libraryPanel_);
    hint->setStyleSheet(QStringLiteral("color: %1; font-size: %2px;")
                            .arg(ui::color(ui::kTextDim).name())
                            .arg(QString::number(ui::kFontSmall)));
    hint->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    // The media grid: 2 columns of cover cards (the old strip's cards,
    // re-homed; a tap is still the library double-click flow).
    mediaGridArea_ = new QScrollArea(libraryPanel_);
    mediaGridArea_->setObjectName(QStringLiteral("fcQuickMediaGrid"));
    mediaGridArea_->setFrameShape(QFrame::NoFrame);
    mediaGridArea_->setWidgetResizable(true);
    mediaGridArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    mediaGridArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    mediaGridArea_->setStyleSheet(
        QStringLiteral("QScrollArea#fcQuickMediaGrid, QScrollArea#fcQuickMediaGrid > QWidget "
                       "{ background: transparent; border: none; }"));

    mediaGridWidget_ = new QWidget;
    mediaGridLayout_ = new QGridLayout(mediaGridWidget_);
    mediaGridLayout_->setContentsMargins(0, 0, 4, 0);
    mediaGridLayout_->setHorizontalSpacing(8);
    mediaGridLayout_->setVerticalSpacing(8);
    mediaGridLayout_->setColumnStretch(0, 1);
    mediaGridLayout_->setColumnStretch(1, 1);
    gridHint_ = new QLabel(tr("Imported media appears here"), mediaGridWidget_);
    gridHint_->setStyleSheet(QStringLiteral("color: %1; font-size: %2px;")
                                 .arg(ui::color(ui::kTextDisabled).name())
                                 .arg(QString::number(ui::kFontSmall)));
    gridHint_->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    gridHint_->setWordWrap(true);
    mediaGridLayout_->addWidget(gridHint_, 0, 0, 1, kLibraryColumns);
    mediaGridArea_->setWidget(mediaGridWidget_);

    layout->addWidget(mediaGridArea_, 1);
    rebuildMediaGrid();
    return libraryPanel_;
}

QWidget *QuickModeView::buildPlayerColumn() {
    auto *column = new QWidget(this);
    auto *layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    // Player header (CapCut): the title on the left, a details-panel
    // toggle on the right. The toggle is real chrome - it shows or
    // hides the Details column for narrow windows.
    auto *header = new QWidget(column);
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(2, 0, 2, 0);
    headerLayout->setSpacing(6);
    auto *title = new QLabel(tr("Player"), header);
    title->setStyleSheet(QStringLiteral("color: %1; font-size: %2px; font-weight: 600;")
                             .arg(ui::color(ui::kText).name())
                             .arg(QString::number(ui::kFontBody)));
    headerLayout->addWidget(title);
    headerLayout->addStretch(1);
    detailsToggle_ = new QToolButton(header);
    detailsToggle_->setIcon(icons::makeIcon(QStringLiteral("list"), ui::color(ui::kTextDim),
                                            kToolbarIconSize, devicePixelRatioF()));
    detailsToggle_->setIconSize(QSize(kToolbarIconSize, kToolbarIconSize));
    detailsToggle_->setAutoRaise(true);
    detailsToggle_->setToolTip(tr("Show or hide the details panel"));
    detailsToggle_->setAccessibleName(tr("Toggle details panel"));
    connect(detailsToggle_, &QToolButton::clicked, this, [this](bool on) {
        if (detailsPanel_ != nullptr) {
            detailsPanel_->setVisible(on);
        }
    });
    headerLayout->addWidget(detailsToggle_);
    layout->addWidget(header);

    // #152: the canvas lives in a kCanvas-toned frame with a 1px kLine
    // border and radius 12. The 10px margins keep the square canvas
    // corners well inside the rounded frame.
    auto *zone = new QFrame(column);
    zone->setObjectName(QStringLiteral("fcQuickCanvasZone"));
    zone->setStyleSheet(
        QStringLiteral("QFrame#fcQuickCanvasZone { background: %1; border: 1px solid %2; "
                       "border-radius: 12px; }")
            .arg(ui::color(ui::kCanvas).name(), ui::color(ui::kLine).name()));
    auto *zoneLayout = new QVBoxLayout(zone);
    zoneLayout->setContentsMargins(10, 10, 10, 10);
    zoneLayout->addWidget(canvas_);
    layout->addWidget(zone, 1);

    layout->addWidget(buildPositionRow());
    layout->addWidget(buildTransportRow());
    return column;
}

QWidget *QuickModeView::buildDetailsPanel() {
    // CapCut's Details panel (owner screenshots): project rows over a
    // card surface, with the Modify action handing off to Pro Mode.
    detailsPanel_ = new QFrame(this);
    detailsPanel_->setObjectName(QStringLiteral("fcQuickDetails"));
    detailsPanel_->setStyleSheet(
        QStringLiteral("QFrame#fcQuickDetails { background: %1; border: 1px solid %2; "
                       "border-radius: %3px; }")
            .arg(ui::color(ui::kSurface2).name(), ui::color(ui::kLine).name())
            .arg(QString::number(ui::kRadiusCard)));
    detailsPanel_->setFixedWidth(kDetailsWidth);

    auto *layout = new QVBoxLayout(detailsPanel_);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);

    auto *title = new QLabel(tr("Details"), detailsPanel_);
    title->setStyleSheet(QStringLiteral("color: %1; font-size: %2px; font-weight: 600;")
                             .arg(ui::color(ui::kText).name())
                             .arg(QString::number(ui::kFontBody)));
    layout->addWidget(title);

    auto *form = new QGridLayout;
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(8);
    QLabel *keys[] = {detailKeyLabel(tr("Name:")),         detailKeyLabel(tr("Path:")),
                      detailKeyLabel(tr("Aspect ratio:")), detailKeyLabel(tr("Resolution:")),
                      detailKeyLabel(tr("Frame rate:")),   detailKeyLabel(tr("Duration:"))};
    QLabel *values[] = {
        detailName_ = detailValueLabel(false),   detailPath_ = detailValueLabel(true),
        detailAspect_ = detailValueLabel(false), detailResolution_ = detailValueLabel(false),
        detailFps_ = detailValueLabel(false),    detailDuration_ = detailValueLabel(false)};
    for (int row = 0; row < 6; ++row) {
        form->addWidget(keys[row], row, 0);
        form->addWidget(values[row], row, 1);
    }
    form->setColumnStretch(1, 1);
    layout->addLayout(form);

    layout->addStretch(1);
    auto *footer = new QHBoxLayout;
    footer->addStretch(1);
    auto *modify = new QPushButton(tr("Modify"), detailsPanel_);
    modify->setToolTip(tr("Aspect, resolution and rate live in Pro Mode"));
    modify->setAccessibleName(tr("Modify project settings"));
    modify->setFixedHeight(26);
    modify->setCursor(Qt::PointingHandCursor);
    connect(modify, &QPushButton::clicked, this, [this] { emit detailsModifyRequested(); });
    footer->addWidget(modify);
    layout->addLayout(footer);
    return detailsPanel_;
}

QWidget *QuickModeView::buildPositionRow() {
    // #148/#149: the timecode chip rides above-left of the painted
    // position slider; the row keeps a fixed height for the bottom stack.
    auto *row = new QWidget(this);
    auto *layout = new QVBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addWidget(timecode_);
    layout->addWidget(position_);
    row->setFixedHeight(38);
    return row;
}

QWidget *QuickModeView::buildTransportRow() {
    // #147: QuickTime order (#79) centered under the preview.
    auto *row = new QWidget(this);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 2, 0, 2);
    layout->addStretch(1);
    layout->addWidget(stepBack_);
    layout->addWidget(playButton_);
    layout->addWidget(stepFwd_);
    layout->addStretch(1);
    row->setFixedHeight(40);
    return row;
}

QWidget *QuickModeView::buildTemplateStrip() {
    auto *bar = new QWidget(this);
    bar->setStyleSheet(QStringLiteral("QWidget { background: %1; border-radius: 6px; }")
                           .arg(ui::color(ui::kSurface2).name()));
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(8);

    auto *caption = new QLabel(tr("Start from a template:"), bar);
    caption->setToolTip(tr("Prefills the timeline with a small starter layout - drop your "
                           "media onto the placeholders."));
    layout->addWidget(caption);

    // Template cards (#77, covers upgraded by #146): 88x50 gradient
    // tile with a centered glyph and a name chip bottom-left, label
    // underneath, accent hover. The ids/signals are unchanged - the
    // template strip is only a face lift.
    struct TemplateCard {
        const char *label;
        const char *id;
        const char *glyph;
        const char *tooltip;
    };
    const TemplateCard cards[] = {
        {"Title + B-roll", "title-broll", "text",
         "Prefills a title card, one b-roll clip and a lower-third caption - swap in "
         "your own media."},
        {"Vlog intro", "vlog", "film",
         "Prefills a vlog opening: intro title, three alternating clip slots and an "
         "outro card."},
        {"Slideshow", "slideshow", "image",
         "Prefills evenly spaced photo slots with crossfade transitions between them."},
    };
    const qreal dpr = devicePixelRatioF();
    for (const TemplateCard &card : cards) {
        auto *button = new QToolButton(bar);
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setAutoRaise(true);
        button->setIcon(
            QIcon(templateCardPixmap(QString::fromUtf8(card.glyph), tr(card.label), dpr)));
        button->setIconSize(QSize(kTileW, kTileH));
        button->setText(tr(card.label));
        button->setToolTip(tr(card.tooltip));
        button->setFixedSize(108, 82); // tile + label under it (#77)
        button->setCursor(Qt::PointingHandCursor);
        button->setStyleSheet(
            QStringLiteral("QToolButton { border: 1px solid %1; border-radius: 8px; "
                           "background: transparent; padding: 3px 6px; color: %2; "
                           "font-size: %3px; }"
                           "QToolButton:hover { border-color: %4; background: %5; }"
                           "QToolButton:pressed { background: %6; }")
                .arg(ui::color(ui::kLine).name(), ui::color(ui::kText).name())
                .arg(QString::number(ui::kFontSmall))
                .arg(ui::color(ui::kAccent).name(), ui::color(ui::kSurface3).name(),
                     ui::color(ui::kSurfacePress).name()));
        connect(button, &QToolButton::clicked, this,
                [this, id = QString::fromUtf8(card.id)] { emit templateRequested(id); });
        layout->addWidget(button);
    }
    layout->addStretch(1);
    return bar;
}

QWidget *QuickModeView::buildToolbar() {
    // Floating tool bar (#137): CapCut's tool order in one centered
    // elevated frame, hairline separators between the tools. Round 4
    // adds the edit trio from the owner screenshots (undo / redo /
    // split) - they EMIT and MainWindow routes them into the same
    // engine entry points the Pro menus use. The former inert
    // sticker/enhance/crop placeholders stay deleted: dead controls
    // with a "Coming later" promise are not honest chrome.
    auto *wrap = new QWidget(this);
    auto *centered = new QHBoxLayout(wrap);
    centered->setContentsMargins(0, 0, 0, 0);

    auto *bar = new QFrame(wrap);
    bar->setObjectName(QStringLiteral("fcQuickToolbar"));
    bar->setStyleSheet(QStringLiteral("QFrame#fcQuickToolbar { background: %1; "
                                      "border: 1px solid %2; border-radius: 12px; }")
                           .arg(ui::color(ui::kSurface2).name(), ui::color(ui::kLine).name()));
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(6, 8, 6, 8);
    layout->setSpacing(4);

    const qreal dpr = devicePixelRatioF();
    const QColor iconColor = ui::color(ui::kText);

    // Edit trio (round 4): undo, redo, split-at-playhead.
    auto *undoTool = toolbarButton(tr("Undo"), tr("Undo the last edit (Ctrl+Z)"),
                                   QStringLiteral("reset"), iconColor, dpr, bar);
    connect(undoTool, &QPushButton::clicked, this, [this] { emit undoRequested(); });
    layout->addWidget(undoTool);
    auto *redoTool = toolbarButton(tr("Redo"), tr("Redo the undone edit (Ctrl+Y)"),
                                   QStringLiteral("redo"), iconColor, dpr, bar);
    connect(redoTool, &QPushButton::clicked, this, [this] { emit redoRequested(); });
    layout->addWidget(redoTool);
    layout->addWidget(toolbarSeparator(bar));
    auto *splitTool = toolbarButton(tr("Split"), tr("Split the clip at the playhead"),
                                    QStringLiteral("blade"), iconColor, dpr, bar);
    connect(splitTool, &QPushButton::clicked, this, [this] { emit splitRequested(); });
    layout->addWidget(splitTool);

    // #138: Import Media… is the only accent-filled control in the zone.
    layout->addWidget(toolbarSeparator(bar));
    auto *addMedia = toolbarButton(tr("Import Media…"), tr("Import media files"),
                                   QStringLiteral("plus"), ui::color(ui::kOnAccent), dpr, bar);
    addMedia->setProperty("fcAccent", true);
    connect(addMedia, &QPushButton::clicked, this, [this] { emit importRequested(); });
    layout->addWidget(addMedia);

    // #139: Text.
    layout->addWidget(toolbarSeparator(bar));
    auto *textTool = toolbarButton(tr("Text"), tr("Add a title at the playhead"),
                                   QStringLiteral("text"), iconColor, dpr, bar);
    connect(textTool, &QPushButton::clicked, this, [this] { emit textToolRequested(); });
    layout->addWidget(textTool);

    // #140: Audio.
    layout->addWidget(toolbarSeparator(bar));
    auto *audioTool = toolbarButton(tr("Audio"), tr("Import music or sound effects"),
                                    QStringLiteral("music"), iconColor, dpr, bar);
    connect(audioTool, &QPushButton::clicked, this, [this] { emit audioToolRequested(); });
    layout->addWidget(audioTool);

    // #142: Effects/Transitions/Filters hand off to the Pro workspace.
    layout->addWidget(toolbarSeparator(bar));
    auto *effectsTool = toolbarButton(tr("Effects"), tr("Open the effects workspace in Pro Mode"),
                                      QStringLiteral("effects"), iconColor, dpr, bar);
    connect(effectsTool, &QPushButton::clicked, this,
            [this] { emit workspaceToolRequested(QStringLiteral("effects")); });
    layout->addWidget(effectsTool);

    layout->addWidget(toolbarSeparator(bar));
    auto *transitionsTool =
        toolbarButton(tr("Transitions"), tr("Open the transitions workspace in Pro Mode"),
                      QStringLiteral("transitions"), iconColor, dpr, bar);
    connect(transitionsTool, &QPushButton::clicked, this,
            [this] { emit workspaceToolRequested(QStringLiteral("transitions")); });
    layout->addWidget(transitionsTool);

    layout->addWidget(toolbarSeparator(bar));
    auto *filtersTool = toolbarButton(tr("Filters"), tr("Open the filters workspace in Pro Mode"),
                                      QStringLiteral("filters"), iconColor, dpr, bar);
    connect(filtersTool, &QPushButton::clicked, this,
            [this] { emit workspaceToolRequested(QStringLiteral("filters")); });
    layout->addWidget(filtersTool);

    // #141: Captions.
    layout->addWidget(toolbarSeparator(bar));
    auto *captionsTool = toolbarButton(tr("Captions"), tr("Import an SRT caption file"),
                                       QStringLiteral("captions"), iconColor, dpr, bar);
    connect(captionsTool, &QPushButton::clicked, this, [this] { emit captionsRequested(); });
    layout->addWidget(captionsTool);

    // Templates: same handler the template cards + empty state use
    // (the title-broll prefill is the only one MainWindow realizes).
    layout->addWidget(toolbarSeparator(bar));
    auto *templatesTool = toolbarButton(tr("Templates"), tr("Apply the starter template"),
                                        QStringLiteral("templates"), iconColor, dpr, bar);
    connect(templatesTool, &QPushButton::clicked, this,
            [this] { emit templateRequested(QStringLiteral("title-broll")); });
    layout->addWidget(templatesTool);

    centered->addStretch(1);
    centered->addWidget(bar);
    centered->addStretch(1);
    return wrap;
}

void QuickModeView::rebuildMediaGrid() {
    if (mediaGridLayout_ == nullptr) {
        return;
    }
    // Replace the grid contents (#136, grid edition): delete the
    // previous cards, then lay the current library back out. The hint
    // label is a permanent grid member (row 0, spanning) and only
    // shows while the library is empty.
    QLayoutItem *item = nullptr;
    while ((item = mediaGridLayout_->takeAt(0)) != nullptr) {
        if (item->widget() != nullptr && item->widget() != gridHint_) {
            item->widget()->deleteLater();
        }
        delete item;
    }

    const int count = qMin(mediaNames_.size(), mediaPaths_.size());
    gridHint_->setVisible(count == 0);
    if (count == 0) {
        mediaGridLayout_->addWidget(gridHint_, 0, 0, 1, kLibraryColumns);
        return;
    }

    const qreal dpr = devicePixelRatioF();
    const QString cardStyle =
        QStringLiteral("QToolButton { border: 1px solid %1; border-radius: 6px; "
                       "background: transparent; padding: 3px 5px; color: %2; "
                       "font-size: %3px; }"
                       "QToolButton:hover { border-color: %4; background: %5; }"
                       "QToolButton:pressed { background: %6; }")
            .arg(ui::color(ui::kLine).name(), ui::color(ui::kText).name())
            .arg(QString::number(ui::kFontSmall))
            .arg(ui::color(ui::kAccent).name(), ui::color(ui::kSurface3).name(),
                 ui::color(ui::kSurfacePress).name());

    // One card per media item: real thumbnail when one arrived via
    // setStripThumbnail, gradient film tile otherwise; the name is
    // elided to the tile width below in 11px.
    QFont nameFont = font();
    nameFont.setPixelSize(ui::kFontSmall);
    const QFontMetrics nameMetrics(nameFont);
    for (int i = 0; i < count; ++i) {
        const QString &name = mediaNames_.at(i);
        const QString &path = mediaPaths_.at(i);
        const QImage thumb = thumbnails_.value(path);
        auto *card = new QToolButton(mediaGridWidget_);
        card->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        card->setAutoRaise(true);
        card->setIcon(QIcon(thumb.isNull() ? gradientTilePixmap(QStringLiteral("film"), dpr, 6)
                                           : mediaCoverPixmap(thumb, dpr)));
        card->setIconSize(QSize(kTileW, kTileH));
        card->setText(nameMetrics.elidedText(name, Qt::ElideRight, kTileW - 8));
        card->setToolTip(name);
        card->setAccessibleName(name);
        card->setFixedSize(kGridCardW, kGridCardH);
        card->setCursor(Qt::PointingHandCursor);
        card->setStyleSheet(cardStyle);
        connect(card, &QToolButton::clicked, this, [this, path] { emit mediaActivated(path); });
        // The page-level event filter is built once in the constructor;
        // cards are built here, so they opt in one by one (full-page
        // drop forwarding keeps working over the grid, #53).
        card->installEventFilter(this);
        mediaGridLayout_->addWidget(card, i / kLibraryColumns, i % kLibraryColumns,
                                    Qt::AlignHCenter | Qt::AlignTop);
    }
    // Keep the grid packed to the top when the library is short.
    mediaGridLayout_->setRowStretch(count / kLibraryColumns + 1, 1);
}

void QuickModeView::setMediaItems(const QStringList &names, const QStringList &paths) {
    mediaNames_ = names;
    mediaPaths_ = paths;
    // Prune thumbnails of items that left the grid (1 GB RAM budget).
    for (auto it = thumbnails_.begin(); it != thumbnails_.end();) {
        if (!mediaPaths_.contains(it.key())) {
            it = thumbnails_.erase(it);
        } else {
            ++it;
        }
    }
    rebuildMediaGrid();
}

void QuickModeView::setStripThumbnail(const QString &path, const QImage &thumb) {
    if (path.isEmpty() || thumb.isNull()) {
        return;
    }
    thumbnails_.insert(path, thumb);
    if (!mediaPaths_.contains(path)) {
        return; // kept for the item's arrival, not drawn yet
    }
    rebuildMediaGrid();
}

void QuickModeView::setDetails(const QString &projectName, const QString &projectPath,
                               const QString &aspect, const QSize &resolution, double fps,
                               double durationSeconds) {
    if (detailName_ == nullptr) {
        return;
    }
    detailName_->setText(projectName.isEmpty() ? QStringLiteral("-") : projectName);
    detailName_->setToolTip(projectName);
    detailPath_->setText(projectPath.isEmpty() ? tr("Not saved yet") : projectPath);
    detailPath_->setToolTip(projectPath);
    detailAspect_->setText(aspect.isEmpty() ? QStringLiteral("-") : aspect);
    detailResolution_->setText(
        resolution.width() > 0 && resolution.height() > 0
            ? QStringLiteral("%1 × %2").arg(resolution.width()).arg(resolution.height())
            : QStringLiteral("-"));
    detailFps_->setText(fps > 1.0 ? tr("%1 fps").arg(fps, 0, 'f', 2) : QStringLiteral("-"));
    if (durationSeconds > 0.0 && fps_ > 1.0) {
        // Same math as the timecode chip: fps as a milli-rational.
        const fc::FrameRate rate{static_cast<uint32_t>(std::lround(fps_ * 1000.0)), 1000, false};
        const int64_t frames = static_cast<int64_t>(std::llround(durationSeconds * fps_));
        detailDuration_->setText(
            QString::fromStdString(fc::Timecode::fromFrames(frames, rate).toString()));
    } else {
        detailDuration_->setText(QStringLiteral("-"));
    }
}

void QuickModeView::setMedia(double durationSeconds, double fps) {
    duration_ = durationSeconds > 0.0 ? durationSeconds : 0.0;
    fps_ = fps > 1.0 ? fps : 24.0;
    pos_ = 0.0;
    position_->setEnabled(duration_ > 0.0);
    position_->blockSignals(true);
    position_->setValue(0);
    position_->blockSignals(false);
    timecode_->setEnabled(duration_ > 0.0);
    refreshTimecode();
    // #74: the hero empty state lives exactly while there is no program.
    if (emptyState_ != nullptr) {
        emptyState_->refresh(duration_ <= 0.0);
    }
}

void QuickModeView::setPosition(double seconds) {
    if (seconds < 0.0) {
        seconds = 0.0;
    }
    if (duration_ > 0.0 && seconds > duration_) {
        seconds = duration_;
    }
    pos_ = seconds;
    position_->blockSignals(true);
    position_->setValue(duration_ > 0.0 ? static_cast<int>(seconds / duration_ * kMaxSlider) : 0);
    position_->blockSignals(false);
    refreshTimecode();
}

void QuickModeView::setPlaying(bool playing) {
    playButton_->setProperty(kPlayingProperty, playing);
    // #79: icon-only transport - the glyph carries the state.
    playButton_->setIcon(icons::makeIcon(playing ? QStringLiteral("pause") : QStringLiteral("play"),
                                         ui::color(ui::kText), kTransportIconSize,
                                         devicePixelRatioF()));
    playButton_->setToolTip(playing ? tr("Pause (Space)") : tr("Play (Space)"));
    playButton_->setAccessibleName(playing ? tr("Pause") : tr("Play"));
}

void QuickModeView::refreshTimecode() {
    // Same math as TransportBar: fps as a milli-rational keeps
    // 23.976/29.97 display exact.
    const fc::FrameRate rate{static_cast<uint32_t>(std::lround(fps_ * 1000.0)), 1000, false};
    const int64_t frames = static_cast<int64_t>(std::llround(pos_ * fps_));
    const QString current =
        QString::fromStdString(fc::Timecode::fromFrames(frames, rate).toString());
    if (duration_ > 0.0) {
        // #149: one chip, "current / total".
        const int64_t totalFrames = static_cast<int64_t>(std::llround(duration_ * fps_));
        const QString total =
            QString::fromStdString(fc::Timecode::fromFrames(totalFrames, rate).toString());
        timecode_->setText(current + QStringLiteral(" / ") + total);
    } else {
        timecode_->setText(current);
    }
}

void QuickModeView::paintEvent(QPaintEvent *event) {
    QWidget::paintEvent(event);
    if (!dragHover_) {
        return;
    }
    // #107: 2px rounded accent outline, inset 8px, while a drag hovers
    // anywhere on the page.
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(ui::color(ui::kAccent), 2);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(rect()).adjusted(8, 8, -8, -8), ui::kRadiusCard,
                            ui::kRadiusCard);
}

void QuickModeView::mouseDoubleClickEvent(QMouseEvent *event) {
    QWidget::mouseDoubleClickEvent(event);
    // #106: with nothing loaded yet, double-clicking the page is the
    // same as pressing Import.
    if (duration_ <= 0.0 && event->button() == Qt::LeftButton) {
        emit importRequested();
    }
}

void QuickModeView::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
        dragHover_ = true;
        update();
    } else {
        event->ignore();
    }
}

void QuickModeView::dragMoveEvent(QDragMoveEvent *event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void QuickModeView::dragLeaveEvent(QDragLeaveEvent *event) {
    dragHover_ = false;
    update();
    event->accept();
}

void QuickModeView::dropEvent(QDropEvent *event) {
    dragHover_ = false;
    update();
    QStringList paths;
    if (event->mimeData()->hasUrls()) {
        const QList<QUrl> urls = event->mimeData()->urls();
        for (const QUrl &url : urls) {
            if (!url.isLocalFile()) {
                continue; // local files only
            }
            const QString path = url.toLocalFile();
            if (!path.isEmpty()) {
                paths.append(path);
            }
        }
    }
    if (paths.isEmpty()) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
    emit filesDropped(paths);
}

bool QuickModeView::eventFilter(QObject *watched, QEvent *event) {
    switch (event->type()) {
    case QEvent::DragEnter:
        dragEnterEvent(static_cast<QDragEnterEvent *>(event));
        return true;
    case QEvent::DragMove:
        dragMoveEvent(static_cast<QDragMoveEvent *>(event));
        return true;
    case QEvent::DragLeave:
        dragLeaveEvent(static_cast<QDragLeaveEvent *>(event));
        return true;
    case QEvent::Drop:
        dropEvent(static_cast<QDropEvent *>(event));
        return true;
    case QEvent::MouseButtonDblClick:
        // #106: a double-click on the canvas area of an empty page
        // starts the import flow. Non-empty canvases keep their
        // default behavior.
        if (watched == canvas_ && duration_ <= 0.0) {
            mouseDoubleClickEvent(static_cast<QMouseEvent *>(event));
            return true;
        }
        break;
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}
