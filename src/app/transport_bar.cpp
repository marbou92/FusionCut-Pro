#include "transport_bar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>

#include <cmath>

namespace {
constexpr int kMaxSlider = 100000;
}

TransportBar::TransportBar(QWidget *parent) : QWidget(parent) {
    stepBack_ = new QPushButton(tr("|<"), this);
    playButton_ = new QPushButton(tr("Play"), this);
    stepFwd_ = new QPushButton(tr(">|"), this);
    position_ = new QSlider(Qt::Horizontal, this);
    timecode_ = new QLabel("00:00:00:00", this);

    stepBack_->setToolTip(tr("Previous frame (Left)"));
    stepFwd_->setToolTip(tr("Next frame (Right)"));
    playButton_->setToolTip(tr("Play / Pause (Space)"));
    timecode_->setMinimumWidth(110);

    position_->setRange(0, kMaxSlider);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->addWidget(stepBack_);
    layout->addWidget(playButton_);
    layout->addWidget(stepFwd_);
    layout->addWidget(position_, 1);
    layout->addWidget(timecode_);

    connect(playButton_, &QPushButton::clicked, this, &TransportBar::onPlayClicked);
    connect(stepBack_, &QPushButton::clicked, this, [this] { emit stepRequested(-1); });
    connect(stepFwd_, &QPushButton::clicked, this, [this] { emit stepRequested(1); });
    connect(position_, &QSlider::sliderMoved, this, &TransportBar::onSliderMoved);

    refreshTimecode();
}

void TransportBar::setMedia(double durationSeconds, double fps) {
    duration_ = durationSeconds > 0.0 ? durationSeconds : 0.0;
    fps_ = fps > 1.0 ? fps : 24.0;
    pos_ = 0.0;
    position_->setValue(0);
    refreshTimecode();
}

void TransportBar::setPosition(double seconds) {
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

void TransportBar::onSliderMoved(int value) {
    if (duration_ <= 0.0) {
        return;
    }
    const double seconds = static_cast<double>(value) / kMaxSlider * duration_;
    pos_ = seconds;
    refreshTimecode();
    emit seekRequested(seconds);
}

void TransportBar::setPlaying(bool playing) {
    if (playing_ == playing) {
        return;
    }
    playing_ = playing;
    playButton_->setText(playing_ ? tr("Pause") : tr("Play"));
    // No playToggled() emission: the state was set by the caller.
}

void TransportBar::onPlayClicked() {
    playing_ = !playing_;
    playButton_->setText(playing_ ? tr("Pause") : tr("Play"));
    emit playToggled(playing_);
}

void TransportBar::refreshTimecode() {
    // fps as a milli-rational keeps 23.976/29.97 display exact.
    const fc::FrameRate rate{static_cast<uint32_t>(std::lround(fps_ * 1000.0)), 1000, false};
    const int64_t frames = static_cast<int64_t>(std::llround(pos_ * fps_));
    timecode_->setText(QString::fromStdString(fc::Timecode::fromFrames(frames, rate).toString()));
}
