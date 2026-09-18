#include "speed_dialog.h"

#include <cmath>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace fc {

SpeedDialog::SpeedDialog(const QString &clipLabel, double currentRate, int64_t sourceExtentFrames,
                         int64_t currentDurationFrames, double fps, bool hasLinkedAudio,
                         QWidget *parent)
    : QDialog(parent), fps_(fps > 1.0 ? fps : 24.0), sourceExtentFrames_(sourceExtentFrames),
      currentDurationFrames_(currentDurationFrames) {
    setWindowTitle(tr("Clip Speed / Duration"));
    setModal(true);

    speed_ = new QDoubleSpinBox(this);
    speed_->setRange(1.0, 10000.0); // percent; 100 % = the source's own speed
    speed_->setDecimals(2);
    speed_->setSuffix(QStringLiteral("%"));
    speed_->setSingleStep(5.0);
    speed_->setValue(currentRate * 100.0);
    speed_->setToolTip(tr("Values above 100 % shorten the clip (faster playback), "
                          "values below stretch it."));

    // The static facts: the source extent that gets rescaled. The
    // length line below updates live from the percentage.
    const double sourceSeconds = static_cast<double>(sourceExtentFrames_) / fps_;
    auto *sourceInfo = new QLabel(tr("Source extent: %1 frames (%2 s of source media)")
                                      .arg(qlonglong(sourceExtentFrames_))
                                      .arg(QString::number(sourceSeconds, 'f', 2)),
                                  this);
    sourceInfo->setWordWrap(true);

    length_ = new QLabel(this);
    length_->setWordWrap(true);

    linkedAudio_ = new QCheckBox(tr("Apply to linked audio from the same source"), this);
    linkedAudio_->setChecked(true);
    linkedAudio_->setToolTip(tr("The audio clip a video+audio import placed on the audio "
                                "track keeps playing under its video sibling only when "
                                "it runs at the same speed."));

    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *form = new QFormLayout;
    form->addRow(tr("Speed:"), speed_);
    form->addRow(QString(), sourceInfo);
    if (hasLinkedAudio) {
        form->addRow(QString(), linkedAudio_); // offered only when a sibling exists
    }

    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(tr("Changes the playback speed of %1 - the clip's timeline length "
                                "rescales, the source media is untouched.")
                                 .arg(clipLabel),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    layout->addLayout(form);
    layout->addWidget(length_);
    layout->addWidget(buttons_);

    // Qt 5.15 still carries the deprecated valueChanged(const QString &)
    // overload, so the member pointer needs the explicit cast (the same
    // idiom text_panel.cpp uses for its QDoubleSpinBoxes).
    connect(speed_, static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged),
            this, &SpeedDialog::updateLength);
    updateLength();
}

double SpeedDialog::rate() const {
    return speed_->value() / 100.0;
}

bool SpeedDialog::includeLinkedAudio() const {
    return linkedAudio_ != nullptr && linkedAudio_->isChecked();
}

void SpeedDialog::updateLength() {
    const double rate = speed_->value() / 100.0;
    if (rate <= 0.0) {
        return; // cannot happen (the spinbox floor is 1.0 %); guard anyway
    }
    const int64_t frames =
        static_cast<int64_t>(std::llround(static_cast<double>(sourceExtentFrames_) / rate));
    const double seconds = static_cast<double>(frames) / fps_;
    length_->setText(tr("New timeline length: %1 frames (%2 s) - was %3 frames")
                         .arg(qlonglong(frames))
                         .arg(QString::number(seconds, 'f', 2))
                         .arg(qlonglong(currentDurationFrames_)));
    // A length below one frame cannot exist on the timeline: the model
    // would reject it, so the dialog refuses to accept the request.
    if (buttons_) {
        if (QPushButton *ok = buttons_->button(QDialogButtonBox::Ok)) {
            ok->setEnabled(frames >= 1);
        }
    }
}

} // namespace fc
