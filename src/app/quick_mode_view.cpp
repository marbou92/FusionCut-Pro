#include "quick_mode_view.h"

#include <QComboBox>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSlider>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

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

// Floating tool bar (#75) + icon-first buttons (#76): 16px stroke
// glyphs beside the labels.
constexpr int kToolbarIconSize = 16;
// Circular transport buttons (#79): 32px rounds, 18px glyphs.
constexpr int kTransportSize = 32;
constexpr int kTransportIconSize = 18;
constexpr int kStepIconSize = 14;
constexpr int kEmptyIconSize = 44;
constexpr int kCardIconSize = 26;

// Circular transport style (#79): elevated fill, hairline, hover
// brighten, press inset - QuickTime's rounded transport buttons.
QString circularTransportStyle() {
    return QStringLiteral("QToolButton { background: %1; border: 1px solid %2; "
                          "border-radius: 16px; padding: 0; }"
                          "QToolButton:hover { background: %3; }"
                          "QToolButton:pressed { background: %4; }")
        .arg(ui::color(ui::kSurface3).name(), ui::color(ui::kLine).name(),
             ui::color(ui::kSurfaceHover).name(), ui::color(ui::kSurfacePress).name());
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
// chrome (no per-button light styling here). `fcAccent` on Add Media is
// set by the caller - it is the ONLY accent-filled control (#66).
QPushButton *toolbarButton(const QString &text, const QString &tooltip, const QString &iconName,
                           const QColor &iconColor, qreal dpr, QWidget *parent) {
    auto *button = new QPushButton(text, parent);
    button->setIcon(icons::makeIcon(iconName, iconColor, kToolbarIconSize, dpr));
    button->setIconSize(QSize(kToolbarIconSize, kToolbarIconSize));
    button->setToolTip(tooltip);
    return button;
}

// Thin vertical group separator inside the floating bar (#75).
QFrame *toolbarSeparator(QWidget *parent) {
    auto *separator = new QFrame(parent);
    separator->setFixedSize(1, 22);
    separator->setStyleSheet(QStringLiteral("background: %1;").arg(ui::color(ui::kLine).name()));
    return separator;
}

// Template card thumbnail (#77): rounded 96x54 tile, two-tone gradient,
// centered template glyph and a 1px hairline - drawn once, at dpr.
QPixmap templateCardPixmap(const QString &glyphName, qreal dpr) {
    const qreal w = 96.0;
    const qreal h = 54.0;
    const qreal ratio = dpr > 0 ? dpr : 1.0;
    QPixmap pm(qRound(w * ratio), qRound(h * ratio));
    pm.setDevicePixelRatio(ratio);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath card;
    card.addRoundedRect(QRectF(0.5, 0.5, w - 1.0, h - 1.0), ui::kRadiusControl, ui::kRadiusControl);
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
        icons::makeIcon(glyphName, ui::color(ui::kTextDim), kCardIconSize, ratio)
            .pixmap(QSize(qRound(kCardIconSize * ratio), qRound(kCardIconSize * ratio)));
    p.drawPixmap(QPointF((w - kCardIconSize) / 2.0, (h - kCardIconSize) / 2.0), glyph);
    return pm;
}
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

    playButton_ =
        circularTransportButton(QStringLiteral("play"), tr("Play (Space)"), tr("Play"), this);
    stepBack_ = circularTransportButton(QStringLiteral("step-back"), tr("Previous frame (Left)"),
                                        tr("Previous frame"), this);
    stepFwd_ = circularTransportButton(QStringLiteral("step-fwd"), tr("Next frame (Right)"),
                                       tr("Next frame"), this);
    position_ = new QSlider(Qt::Horizontal, this);
    position_->setRange(0, kMaxSlider);
    position_->setEnabled(false); // a duration arrives with setMedia()
    timecode_ = new QLabel("00:00:00:00", this);
    timecode_->setMinimumWidth(110);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 12, 16, 12);
    root->setSpacing(10);
    root->addWidget(buildStepRail());
    root->addWidget(buildTopBar());
    root->addWidget(canvas_, 1);

    // QuickTime order (#79): back - play - forward.
    auto *transport = new QHBoxLayout();
    transport->setSpacing(6);
    transport->addWidget(stepBack_);
    transport->addWidget(playButton_);
    transport->addWidget(stepFwd_);
    transport->addWidget(position_, 1);
    transport->addWidget(timecode_);
    root->addLayout(transport);
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

    // Children (canvas, buttons, labels, the empty state) would swallow
    // drag events before the page sees them: forward every drag event
    // that lands on ANY descendant widget back into this page's own
    // handlers via an event filter (#53). Called after the UI is fully
    // built.
    const QList<QWidget *> children = findChildren<QWidget *>();
    for (QWidget *child : children) {
        child->installEventFilter(this);
    }
}

QWidget *QuickModeView::buildTopBar() {
    auto *bar = new QWidget(this);
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(0, 0, 0, 0);

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

    // No standalone Import button anymore (0-a): the step rail's
    // current Import chip + the empty state's primary action own it.
    layout->addStretch(1);
    layout->addWidget(new QLabel(tr("Aspect:"), bar));
    layout->addWidget(aspectBox_);
    return bar;
}

QWidget *QuickModeView::buildStepRail() {
    auto *rail = new QWidget(this);
    auto *layout = new QHBoxLayout(rail);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    // Numbered progress rail (#73). applyStepStyles() picks the text
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
        chip->setMinimumHeight(28); // comfortable hit target
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
            link->setFixedSize(28, 1); // 1px connector (#73)
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
                               "border-radius: 13px; padding: 4px 16px; }"
                               "QPushButton:hover { background: %3; }")
                    .arg(accent, onAccent, accentBright));
        } else if (i < step_) {
            // Completed step: neutral chip, accent checkmark (#73).
            stepChips_[i]->setIcon(check);
            stepChips_[i]->setText(plain[i]);
            stepChips_[i]->setStyleSheet(
                QStringLiteral("QPushButton { background: %1; color: %2; border-radius: 13px; "
                               "padding: 4px 16px; }"
                               "QPushButton:hover { background: %3; }")
                    .arg(surface3, text, hover));
        } else {
            // Future step: neutral, dim.
            stepChips_[i]->setIcon(QIcon());
            stepChips_[i]->setText(numbered[i]);
            stepChips_[i]->setStyleSheet(
                QStringLiteral("QPushButton { background: %1; color: %2; border-radius: 13px; "
                               "padding: 4px 16px; }"
                               "QPushButton:hover { background: %3; color: %4; }")
                    .arg(surface3, textDim, hover, text));
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

    // Template cards (#77): 96x54 gradient tile with a glyph, label
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
        button->setIcon(QIcon(templateCardPixmap(QString::fromUtf8(card.glyph), dpr)));
        button->setIconSize(QSize(96, 54));
        button->setText(tr(card.label));
        button->setToolTip(tr(card.tooltip));
        button->setFixedSize(112, 86); // tile + label under it (#77)
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
    // Floating bottom bar (#75): the two former rows become one
    // centered elevated bar with hairline group separators; every tool
    // is icon-first (#76). Buttons stay placeholders ("Coming later")
    // with no connections, exactly as before.
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
    const QString later = tr("Coming later");

    // Group 1: media in - the single accent-filled control (#66).
    auto *addMedia = toolbarButton(tr("Add Media"), later, QStringLiteral("plus"),
                                   ui::color(ui::kOnAccent), dpr, bar);
    addMedia->setProperty("fcAccent", true);
    layout->addWidget(addMedia);

    // Group 2: overlays (text, stickers).
    layout->addWidget(toolbarSeparator(bar));
    layout->addWidget(
        toolbarButton(tr("Text"), later, QStringLiteral("text"), iconColor, dpr, bar));
    layout->addWidget(
        toolbarButton(tr("Stickers"), later, QStringLiteral("sticker"), iconColor, dpr, bar));

    // Group 3: clip effects.
    layout->addWidget(toolbarSeparator(bar));
    layout->addWidget(
        toolbarButton(tr("Effects"), later, QStringLiteral("effects"), iconColor, dpr, bar));
    layout->addWidget(toolbarButton(tr("Transitions"), later, QStringLiteral("transitions"),
                                    iconColor, dpr, bar));
    layout->addWidget(
        toolbarButton(tr("Filters"), later, QStringLiteral("filters"), iconColor, dpr, bar));

    // Group 4: AI assists.
    layout->addWidget(toolbarSeparator(bar));
    layout->addWidget(
        toolbarButton(tr("Auto-Captions"), later, QStringLiteral("captions"), iconColor, dpr, bar));
    layout->addWidget(
        toolbarButton(tr("Auto-Enhance"), later, QStringLiteral("enhance"), iconColor, dpr, bar));
    layout->addWidget(
        toolbarButton(tr("Smart Crop"), later, QStringLiteral("crop"), iconColor, dpr, bar));

    // Group 5: templates (the strip below holds the actual prefills).
    layout->addWidget(toolbarSeparator(bar));
    layout->addWidget(
        toolbarButton(tr("Templates"), later, QStringLiteral("templates"), iconColor, dpr, bar));

    centered->addStretch(1);
    centered->addWidget(bar);
    centered->addStretch(1);
    return wrap;
}

void QuickModeView::setMedia(double durationSeconds, double fps) {
    duration_ = durationSeconds > 0.0 ? durationSeconds : 0.0;
    fps_ = fps > 1.0 ? fps : 24.0;
    pos_ = 0.0;
    position_->setEnabled(duration_ > 0.0);
    position_->blockSignals(true);
    position_->setValue(0);
    position_->blockSignals(false);
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
    timecode_->setText(QString::fromStdString(fc::Timecode::fromFrames(frames, rate).toString()));
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
