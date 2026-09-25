#include "transport_bar.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QResizeEvent>
#include <QSettings>
#include <QSlider>
#include <QToolButton>

#include <algorithm>
#include <cmath>

#include "timecode_display.h"
#include "ui_theme.h"
#include "ui_widgets.h"

// ui_theme.h tokens live in fc::ui; the panel addresses them as ui::...
using namespace fc;

// Shared design tokens (suggestion #55) live in fc::ui; alias once so the
// chrome-styling call sites stay readable.
namespace ui = fc::ui;

namespace {
constexpr int kMaxSlider = 100000;
// Frame-step feedback (#25) and the timecode shake share the rhythm.
constexpr int kStepFlashMs = 700;
constexpr int kShakeMs = 180;
// #79 circular transport buttons: 30 px circles (clears the 28 px #63
// touch target) carrying 16 px stroke icons from fc::icons.
constexpr int kTransportButton = 30;
constexpr int kTransportIcon = 16;

QIcon transportIcon(const QString &name, qreal dpr) {
    return icons::makeIcon(name, ui::color(ui::kText), kTransportIcon, dpr > 0.0 ? dpr : 1.0);
}
} // namespace

TransportBar::TransportBar(QWidget *parent) : QWidget(parent) {
    // #79 (0-e): circular icon transport. The anonymous gray "|<" / ">|" /
    // "Play" text buttons become QuickTime-style 30 px circles with
    // DPR-rendered stroke icons; the play and mute icons swap with state.
    // Every signal, shortcut and persisted setting below is untouched.
    const QString circleButtonStyle =
        QStringLiteral("QPushButton{background:%1;border:1px solid %2;border-radius:15px;}"
                       "QPushButton:hover{background:%3;}"
                       "QPushButton:pressed{background:%4;}")
            .arg(ui::color(ui::kSurface3).name(), ui::color(ui::kLine).name(),
                 ui::color(ui::kSurfaceHover).name(), ui::color(ui::kSurfacePress).name());
    const QString circleToolStyle =
        QStringLiteral("QToolButton{background:%1;border:1px solid %2;border-radius:15px;}"
                       "QToolButton:hover{background:%3;}"
                       "QToolButton:pressed{background:%4;}"
                       "QToolButton:checked{border-color:%5;}")
            .arg(ui::color(ui::kSurface3).name(), ui::color(ui::kLine).name(),
                 ui::color(ui::kSurfaceHover).name(), ui::color(ui::kSurfacePress).name(),
                 ui::color(ui::kText).name());

    stepBack_ = new QPushButton(this);
    playButton_ = new QPushButton(this);
    stepFwd_ = new QPushButton(this);
    position_ = new QSlider(Qt::Horizontal, this);
    timecode_ = new QLabel("00:00:00:00", this);
    muteButton_ = new QToolButton(this);
    volume_ = new QSlider(Qt::Horizontal, this);
    stepFlash_ = new QLabel(this);

    for (QPushButton *button : {stepBack_, playButton_, stepFwd_}) {
        button->setStyleSheet(circleButtonStyle);
        button->setFixedSize(kTransportButton, kTransportButton); // #63 touch target
        button->setIconSize(QSize(kTransportIcon, kTransportIcon));
    }
    stepBack_->setIcon(transportIcon(QStringLiteral("step-back"), devicePixelRatioF()));
    stepFwd_->setIcon(transportIcon(QStringLiteral("step-fwd"), devicePixelRatioF()));
    refreshPlayIcon(); // play / pause glyph + accessible name by state

    stepBack_->setToolTip(tr("Previous frame (Left)"));
    stepFwd_->setToolTip(tr("Next frame (Right)"));
    playButton_->setToolTip(tr("Play / Pause (Space)"));
    stepBack_->setAccessibleName(tr("Previous frame"));
    stepFwd_->setAccessibleName(tr("Next frame"));
    timecode_->setMinimumWidth(110);
    // #62/#59: the timecode is a control (click to edit) - say so, and
    // give the assistive stack a stable name.
    timecode_->setToolTip(tr("Click to type a timecode (Enter applies, Esc cancels)"));
    timecode_->setAccessibleName(tr("Timecode"));
    timecode_->setCursor(Qt::PointingHandCursor);
    timecode_->installEventFilter(this);

    // Mute + volume (#27): persisted panel-side, audio hookup is wave 2.
    // #79: icon-only mute toggle (volume / mute glyph per state).
    muteButton_->setStyleSheet(circleToolStyle);
    muteButton_->setFixedSize(kTransportButton, kTransportButton); // #63 touch target
    muteButton_->setIconSize(QSize(kTransportIcon, kTransportIcon));
    muteButton_->setCheckable(true);
    muteButton_->setToolTip(tr("Mute preview"));
    muteButton_->setAccessibleName(tr("Mute preview"));
    refreshMuteIcon();
    volume_->setRange(0, 100);
    volume_->setFixedWidth(90);
    volume_->setToolTip(tr("Preview volume"));
    volume_->setAccessibleName(tr("Preview volume"));

    position_->setRange(0, kMaxSlider);

    stepFlash_->hide();
    stepFlash_->setStyleSheet(QStringLiteral("QLabel{background:%1;color:%2;border:1px solid %3;"
                                             "border-radius:3px;padding:2px 8px;}")
                                  .arg(ui::withAlpha(ui::kSurface3, 235).name(),
                                       ui::color(ui::kText).name(), ui::color(ui::kLine).name()));
    stepFlashTimer_.setSingleShot(true);
    connect(&stepFlashTimer_, &QTimer::timeout, this, [this] { stepFlash_->hide(); });

    // Click-to-edit timecode (#23): an inline editor stacked on the label.
    timecodeEditor_ = new QLineEdit(this);
    timecodeEditor_->setFrame(false);
    timecodeEditor_->hide();
    timecodeEditor_->installEventFilter(this);
    timecodeEditor_->setStyleSheet(
        QStringLiteral("QLineEdit{background:%1;color:%2;border:1px solid %3;padding:0 2px;}"
                       "QLineEdit:focus{border-color:%4;}")
            .arg(ui::color(ui::kSurface3).name(), ui::color(ui::kText).name(),
                 ui::color(ui::kLine).name(), ui::color(ui::kAccent).name()));
    timecodeShake_ = new QPropertyAnimation(timecodeEditor_, "pos", this);
    timecodeShake_->setDuration(kShakeMs);

    // #56: one 4/8 px padding rhythm across the bar.
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);
    layout->addWidget(stepBack_);
    layout->addWidget(playButton_);
    layout->addWidget(stepFwd_);
    layout->addWidget(position_, 1);
    layout->addWidget(timecode_);
    layout->addWidget(muteButton_);
    layout->addWidget(volume_);

    connect(playButton_, &QPushButton::clicked, this, &TransportBar::onPlayClicked);
    connect(stepBack_, &QPushButton::clicked, this, [this] {
        emit stepRequested(-1);
        flashStepFrame();
    });
    connect(stepFwd_, &QPushButton::clicked, this, [this] {
        emit stepRequested(1);
        flashStepFrame();
    });
    connect(position_, &QSlider::sliderMoved, this, &TransportBar::onSliderMoved);
    // Groove clicks and keyboard moves fire valueChanged without a
    // sliderMoved - the handle jumped but no seek was emitted and the
    // next setPosition() snapped it back. setPosition()'s blockSignals
    // keeps programmatic updates silent; during a DRAG the handle is
    // "down" and sliderMoved already covers it.
    connect(position_, &QSlider::valueChanged, this, [this](int value) {
        if (!position_->isSliderDown()) {
            onSliderMoved(value);
        }
    });
    // Volume (#27): persistence is panel-side (main.cpp sets the QSettings
    // organization/application), the audio path itself is wave-2 wiring.
    connect(volume_, &QSlider::valueChanged, this, [this](int percent) {
        QSettings settings;
        settings.setValue(QStringLiteral("transport/volumePercent"), percent);
        emit volumeChanged(percent);
    });
    connect(muteButton_, &QToolButton::clicked, this, [this](bool muted) {
        QSettings settings;
        settings.setValue(QStringLiteral("transport/muted"), muted);
        refreshMuteIcon(); // icon-only button: the glyph is the state
        emit muteToggled(muted);
    });

    // Restore persisted volume/mute (defaults 80 / not muted). The
    // initial slider write must not emit volumeChanged.
    QSettings settings;
    volume_->blockSignals(true);
    volume_->setValue(
        qBound(0, settings.value(QStringLiteral("transport/volumePercent"), 80).toInt(), 100));
    volume_->blockSignals(false);
    muteButton_->setChecked(settings.value(QStringLiteral("transport/muted"), false).toBool());
    refreshMuteIcon(); // glyph follows the persisted state

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
    refreshPlayIcon(); // icon-only button: the glyph is the state
    // No playToggled() emission: the state was set by the caller.
}

void TransportBar::onPlayClicked() {
    playing_ = !playing_;
    refreshPlayIcon();
    emit playToggled(playing_);
}

void TransportBar::refreshTimecode() {
    // #23: while the inline editor is open the label is "owned" by the
    // editor - position updates must not overwrite (or visually fight
    // with) the text being typed.
    if (editing_) {
        return;
    }
    // #26 drop-frame display: DISPLAY-SIDE ONLY - the model stays in
    // frames. format() emits ';' SMPTE drop-frame numbering for real
    // 29.97/59.94 rates and plain ':' numbering for every other rate.
    timecode_->setText(fc::timecode_display::format(static_cast<int64_t>(std::llround(pos_ * fps_)),
                                                    fps_,
                                                    fc::timecode_display::isDropCapable(fps_)));
}

void TransportBar::beginTimecodeEdit() {
    if (editing_ || fps_ <= 0.0) {
        return;
    }
    editing_ = true;
    timecodeEditor_->setGeometry(timecode_->geometry());
    timecodeEditor_->setText(timecode_->text());
    timecodeEditor_->selectAll();
    timecodeEditor_->show();
    timecodeEditor_->raise();
    timecodeEditor_->setFocus(Qt::MouseFocusReason);
}

void TransportBar::commitTimecodeEdit() {
    if (!editing_) {
        return;
    }
    int64_t frames = 0;
    if (fc::timecode_display::parse(timecodeEditor_->text(), fps_,
                                    fc::timecode_display::isDropCapable(fps_), &frames)) {
        editing_ = false;
        timecodeEditor_->hide();
        // Back to the label; the seek flows back through setPosition().
        emit seekRequested(static_cast<double>(frames) / fps_);
    } else {
        // Parse errors shake, never apply (#23) - stay in edit mode.
        shakeTimecodeEditor();
    }
}

void TransportBar::cancelTimecodeEdit() {
    if (!editing_) {
        return;
    }
    editing_ = false;
    timecodeShake_->stop();
    // Put the editor back where the layout expects it for the next edit.
    timecodeEditor_->setGeometry(timecode_->geometry());
    timecodeEditor_->hide();
}

void TransportBar::shakeTimecodeEditor() {
    // Shake in place: x offsets +4/-4/+2/-2/0 over ~180 ms, y fixed.
    timecodeEditorHome_ = timecodeEditor_->pos();
    timecodeShake_->stop();
    timecodeShake_->setKeyValueAt(0.0, timecodeEditorHome_ + QPoint(4, 0));
    timecodeShake_->setKeyValueAt(0.25, timecodeEditorHome_ + QPoint(-4, 0));
    timecodeShake_->setKeyValueAt(0.50, timecodeEditorHome_ + QPoint(2, 0));
    timecodeShake_->setKeyValueAt(0.75, timecodeEditorHome_ + QPoint(-2, 0));
    timecodeShake_->setKeyValueAt(1.0, timecodeEditorHome_);
    timecodeShake_->start();
}

void TransportBar::repositionStepFlash() {
    stepFlash_->adjustSize();
    stepFlash_->move(width() - stepFlash_->width() - 8, height() - stepFlash_->height() - 8);
}

void TransportBar::flashStepFrame() {
    // #25 frame-step feedback: transient "frame N of M" chip at the
    // bar's bottom-right. N is the CURRENT display frame (1-based) and
    // M the clip's total frame count (>= 1 while no media is loaded).
    const int64_t frame = static_cast<int64_t>(std::llround(pos_ * fps_)) + 1;
    const int64_t total =
        std::max<int64_t>(1, static_cast<int64_t>(std::llround(duration_ * fps_)));
    stepFlash_->setText(
        tr("frame %1 of %2").arg(static_cast<qlonglong>(frame)).arg(static_cast<qlonglong>(total)));
    repositionStepFlash();
    stepFlash_->show();
    stepFlash_->raise();
    // A member single-shot timer restarts cleanly on rapid stepping.
    stepFlashTimer_.start(kStepFlashMs);
}

bool TransportBar::eventFilter(QObject *watched, QEvent *event) {
    if (watched == timecode_) {
        // Clicking the timecode flips it to the inline editor (#23).
        if (event->type() == QEvent::MouseButtonPress) {
            beginTimecodeEdit();
            return true;
        }
    } else if (watched == timecodeEditor_) {
        if (event->type() == QEvent::KeyPress) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            if (keyEvent->key() == Qt::Key_Escape) {
                cancelTimecodeEdit();
                return true; // Esc cancels back to the label
            }
            if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
                commitTimecodeEdit();
                return true;
            }
        } else if (event->type() == QEvent::FocusOut) {
            cancelTimecodeEdit(); // focus-out cancels back to the label
        }
    }
    return QWidget::eventFilter(watched, event);
}

void TransportBar::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    if (stepFlash_ != nullptr && stepFlash_->isVisible()) {
        repositionStepFlash();
    }
    if (editing_ && timecodeEditor_ != nullptr) {
        // Keep the editor glued to the label across layout resizes.
        timecodeEditor_->setGeometry(timecode_->geometry());
    }
}

// #79: the play/pause glyph tracks the playing state (QuickTime pattern:
// one neutral circle whose icon swaps). The accessible name follows so
// assistive tech announces the action, not the glyph.
void TransportBar::refreshPlayIcon() {
    if (playButton_ == nullptr) {
        return;
    }
    playButton_->setIcon(transportIcon(playing_ ? QStringLiteral("pause") : QStringLiteral("play"),
                                       devicePixelRatioF()));
    playButton_->setAccessibleName(playing_ ? tr("Pause") : tr("Play"));
}

// #79: mute toggle glyph (speaker = audible, speaker + cross = muted).
void TransportBar::refreshMuteIcon() {
    if (muteButton_ == nullptr) {
        return;
    }
    muteButton_->setIcon(
        transportIcon(muteButton_->isChecked() ? QStringLiteral("mute") : QStringLiteral("volume"),
                      devicePixelRatioF()));
}

// #64: the bar paints itself as a kSurface2 elevation plane with rounded
// top corners (it meets the monitor canvas above); the bottom edge stays
// square against the window chrome. The children draw on top as usual.
void TransportBar::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath strip;
    strip.addRoundedRect(QRectF(0, 0, width(), height() + ui::kRadiusControl), ui::kRadiusControl,
                         ui::kRadiusControl);
    painter.fillPath(strip, ui::color(ui::kSurface2));
}

// The DPR-aware icons are first grabbed in the ctor, before the bar has a
// screen; re-grab them on the first show where devicePixelRatioF() is the
// real ratio (keeps 125 / 150% Windows scaling crisp).
void TransportBar::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    refreshPlayIcon();
    refreshMuteIcon();
}
