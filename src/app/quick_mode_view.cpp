#include "quick_mode_view.h"

#include <QComboBox>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QUrl>
#include <QVBoxLayout>

#include <cmath>

#include "timecode.h"
#include "ui_theme.h"

// ui_theme.h tokens live in fc::ui; the panel addresses them as ui::...
using namespace fc;

namespace {
constexpr int kMaxSlider = 100000;

// Dynamic QObject property on the play button that mirrors the playing
// state (see setPlaying) - state, not translated label text.
constexpr char kPlayingProperty[] = "fcPlaying";

const char *kAccentStyle = "QPushButton { background: #00A8FF; color: #101010; font-weight: bold; "
                           "border-radius: 4px; padding: 6px 18px; }"
                           "QPushButton:hover { background: #33B9FF; }";

struct ToolGroup {
    const char *const *labels;
    int count;
    const char *tooltip;
};

QPushButton *flatToolButton(const QString &text, const QString &tooltip, const char *color,
                            QWidget *parent) {
    auto *button = new QPushButton(text, parent);
    button->setFlat(true);
    button->setToolTip(tooltip);
    button->setStyleSheet(QString("QPushButton { color: %1; padding: 6px 10px; }"
                                  "QPushButton:hover { color: #00A8FF; }")
                              .arg(color));
    return button;
}
} // namespace

QuickModeView::QuickModeView(QWidget *parent) : QWidget(parent) {
    setAcceptDrops(true); // full-page media drop zone (#53)

    canvas_ = new PreviewCanvas(this);
    playButton_ = new QPushButton(tr("Play"), this);
    playButton_->setMinimumWidth(90);
    stepBack_ = new QPushButton(tr("|<"), this);
    stepFwd_ = new QPushButton(tr(">|"), this);
    stepBack_->setToolTip(tr("Previous frame (Left)"));
    stepFwd_->setToolTip(tr("Next frame (Right)"));
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

    auto *transport = new QHBoxLayout();
    transport->addWidget(playButton_);
    transport->addWidget(stepBack_);
    transport->addWidget(stepFwd_);
    transport->addWidget(position_, 1);
    transport->addWidget(timecode_);
    root->addLayout(transport);
    root->addWidget(buildToolbar());
    root->addWidget(buildTemplateStrip());

    connect(playButton_, &QPushButton::clicked, this, [this] {
        // The playing STATE lives on the button as a dynamic property
        // kept in sync by setPlaying(); the old text() == tr("Play")
        // comparison broke under translation (the label reads
        // tr("Pause") while playing, and either string can change).
        // An unset property reads false = not playing, matching the
        // initial "Play" label.
        const bool playing = playButton_->property(kPlayingProperty).toBool();
        emit playToggled(!playing);
    });
    connect(stepBack_, &QPushButton::clicked, this, [this] { emit stepRequested(-1); });
    connect(stepFwd_, &QPushButton::clicked, this, [this] { emit stepRequested(1); });
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

    // Children (canvas, buttons, labels) would swallow drag events
    // before the page sees them: forward every drag event that lands
    // on ANY descendant widget back into this page's own handlers via
    // an event filter (#53). Called after the UI is fully built.
    const QList<QWidget *> children = findChildren<QWidget *>();
    for (QWidget *child : children) {
        child->installEventFilter(this);
    }
}

QWidget *QuickModeView::buildTopBar() {
    auto *bar = new QWidget(this);
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(0, 0, 0, 0);

    auto *import = new QPushButton(tr("Import"), bar);
    import->setStyleSheet(kAccentStyle);
    import->setToolTip(tr("Import media files into the project (Ctrl+I)"));
    connect(import, &QPushButton::clicked, this, [this] { emit importRequested(); });

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

    layout->addWidget(import);
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
        connect(chip, &QPushButton::clicked, this, [this, i] {
            if (i == 0) {
                emit importRequested();
            } else if (i == 2) {
                emit exportRequested();
            }
            // Step 1 (arrange) is the timeline itself - no action.
        });
        stepChips_[i] = chip;
        layout->addWidget(chip);
        if (i < 2) {
            auto *link = new QWidget(rail);
            link->setFixedSize(28, 2);
            stepLinks_[i] = link;
            layout->addWidget(link);
        }
    }
    layout->addStretch(1);
    applyStepStyles();
    return rail;
}

void QuickModeView::applyStepStyles() {
    const QString accent = fc::ui::color(ui::kAccent).name();
    const QString accentBright = fc::ui::color(ui::kAccentBright).name();
    const QString onAccent = fc::ui::color(ui::kOnAccent).name();
    const QString surface3 = fc::ui::color(ui::kSurface3).name();
    const QString hover =
        fc::ui::mix(fc::ui::color(ui::kSurface3), fc::ui::color(ui::kLine), 0.5).name();
    const QString text = fc::ui::color(ui::kText).name();
    const QString line = fc::ui::color(ui::kLine).name();
    for (int i = 0; i < 3; ++i) {
        if (stepChips_[i] == nullptr) {
            continue;
        }
        if (i == step_) {
            stepChips_[i]->setStyleSheet(
                QStringLiteral("QPushButton { background: %1; color: %2; font-weight: bold; "
                               "border-radius: 13px; padding: 4px 16px; }"
                               "QPushButton:hover { background: %3; }")
                    .arg(accent, onAccent, accentBright));
        } else {
            stepChips_[i]->setStyleSheet(
                QStringLiteral("QPushButton { background: %1; color: %2; border-radius: 13px; "
                               "padding: 4px 16px; }"
                               "QPushButton:hover { background: %3; }")
                    .arg(surface3, text, hover));
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
                           .arg(fc::ui::color(ui::kSurface2).name()));
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(8);

    auto *caption = new QLabel(tr("Start from a template:"), bar);
    caption->setToolTip(tr("Prefills the timeline with a small starter layout - drop your "
                           "media onto the placeholders."));
    layout->addWidget(caption);

    struct TemplateChip {
        const char *label;
        const char *id;
        const char *tooltip;
    };
    const TemplateChip chips[] = {
        {"Title + B-roll", "title-broll",
         "Prefills a title card, one b-roll clip and a lower-third caption - swap in "
         "your own media."},
        {"Vlog intro", "vlog",
         "Prefills a vlog opening: intro title, three alternating clip slots and an "
         "outro card."},
        {"Slideshow", "slideshow",
         "Prefills evenly spaced photo slots with crossfade transitions between them."},
    };
    for (const TemplateChip &chip : chips) {
        auto *button = new QPushButton(tr(chip.label), bar);
        button->setFlat(true);
        button->setToolTip(tr(chip.tooltip));
        button->setMinimumHeight(28); // comfortable hit target
        button->setCursor(Qt::PointingHandCursor);
        button->setStyleSheet(QStringLiteral("QPushButton { color: %1; border: 1px solid %2; "
                                             "border-radius: 13px; padding: 4px 14px; }"
                                             "QPushButton:hover { color: %3; }")
                                  .arg(fc::ui::color(ui::kText).name(),
                                       fc::ui::color(ui::kLine).name(),
                                       fc::ui::color(ui::kAccent).name()));
        connect(button, &QPushButton::clicked, this,
                [this, id = QString::fromUtf8(chip.id)] { emit templateRequested(id); });
        layout->addWidget(button);
    }
    layout->addStretch(1);
    return bar;
}

QWidget *QuickModeView::buildToolbar() {
    auto *bar = new QWidget(this);
    bar->setStyleSheet("QWidget { background: #252525; border-radius: 6px; }");
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(8);

    // Main toolbar (shown when no clip is selected).
    static const char *kTools[] = {"Add Media", "Text",        "Stickers",
                                   "Effects",   "Transitions", "Filters"};
    for (const char *tool : kTools) {
        layout->addWidget(flatToolButton(tr(tool), tr("Coming later"), "#E8E8E8", bar));
    }
    layout->addStretch(1);

    // Quick Actions: AI one-click features.
    static const char *kActions[] = {"Auto-Captions", "Auto-Enhance", "Smart Crop", "Templates"};
    for (const char *action : kActions) {
        layout->addWidget(flatToolButton(tr(action), tr("Coming later"), "#9BB8C9", bar));
    }
    return bar;
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
    playButton_->setText(playing ? tr("Pause") : tr("Play"));
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
    // Full-panel 2 px accent border while a drag hovers over the page.
    QPainter painter(this);
    const QColor accent = fc::ui::color(ui::kAccent);
    const QRect bounds = rect();
    painter.fillRect(QRect(bounds.left(), bounds.top(), bounds.width(), 2), accent);
    painter.fillRect(QRect(bounds.left(), bounds.bottom() - 1, bounds.width(), 2), accent);
    painter.fillRect(QRect(bounds.left(), bounds.top(), 2, bounds.height()), accent);
    painter.fillRect(QRect(bounds.right() - 1, bounds.top(), 2, bounds.height()), accent);
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
    default:
        return QWidget::eventFilter(watched, event);
    }
}
