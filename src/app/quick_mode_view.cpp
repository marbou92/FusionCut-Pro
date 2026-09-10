#include "quick_mode_view.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

#include <cmath>

#include "timecode.h"

namespace {
constexpr int kMaxSlider = 100000;

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

    connect(playButton_, &QPushButton::clicked, this,
            [this] { emit playToggled(playButton_->text() == tr("Play")); });
    connect(stepBack_, &QPushButton::clicked, this, [this] { emit stepRequested(-1); });
    connect(stepFwd_, &QPushButton::clicked, this, [this] { emit stepRequested(1); });
    connect(position_, &QSlider::sliderMoved, this, [this](int value) {
        if (duration_ <= 0.0) {
            return;
        }
        const double seconds = static_cast<double>(value) / kMaxSlider * duration_;
        pos_ = seconds;
        refreshTimecode();
        emit seekRequested(seconds);
    });
    refreshTimecode();
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
    connect(aspectBox_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                const double aspects[] = {16.0 / 9.0, 9.0 / 16.0, 1.0, 4.0 / 3.0, 0.0};
                if (aspects[index] > 0.0) {
                    canvas_->setAspectHint(aspects[index]);
                }
            });

    layout->addWidget(import);
    layout->addStretch(1);
    layout->addWidget(new QLabel(tr("Aspect:"), bar));
    layout->addWidget(aspectBox_);
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
    playButton_->setText(playing ? tr("Pause") : tr("Play"));
}

void QuickModeView::refreshTimecode() {
    // Same math as TransportBar: fps as a milli-rational keeps
    // 23.976/29.97 display exact.
    const fc::FrameRate rate{static_cast<uint32_t>(std::lround(fps_ * 1000.0)), 1000, false};
    const int64_t frames = static_cast<int64_t>(std::llround(pos_ * fps_));
    timecode_->setText(QString::fromStdString(fc::Timecode::fromFrames(frames, rate).toString()));
}
