#include "mixer_panel.h"

#include <cmath>

#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QVBoxLayout>

namespace {

QWidget *makeStrip(const QString &name, bool withPan, QWidget *parent) {
    auto *strip = new QWidget(parent);
    strip->setFixedWidth(84);
    auto *layout = new QVBoxLayout(strip);
    layout->setContentsMargins(6, 6, 6, 6);

    auto *title = new QLabel(name, strip);
    title->setAlignment(Qt::AlignCenter);

    auto *fader = new QSlider(Qt::Vertical, strip);
    fader->setRange(-60, 6); // dB
    fader->setValue(0);

    auto *value = new QLabel("0.0 dB", strip);
    value->setAlignment(Qt::AlignCenter);

    QObject::connect(fader, &QSlider::valueChanged, value,
                     [value](int db) { value->setText(QString("%1 dB").arg(db)); });

    layout->addWidget(title);
    layout->addWidget(fader, 1);
    layout->addWidget(value);

    if (withPan) {
        auto *pan = new QSlider(Qt::Horizontal, strip);
        pan->setRange(-100, 100);
        pan->setValue(0);
        auto *panLabel = new QLabel(QObject::tr("Pan C"), strip);
        panLabel->setAlignment(Qt::AlignCenter);
        QObject::connect(pan, &QSlider::valueChanged, panLabel, [panLabel](int v) {
            panLabel->setText(
                v == 0 ? QObject::tr("Pan C")
                       : QString(QObject::tr("Pan %1%2")).arg(v < 0 ? "L" : "R").arg(std::abs(v)));
        });
        layout->addWidget(pan);
        layout->addWidget(panLabel);
    }

    // Mute / solo pair.
    auto *row = new QWidget(strip);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    auto *mute = new QLabel("M", row);
    auto *solo = new QLabel("S", row);
    for (QLabel *cell : {mute, solo}) {
        cell->setAlignment(Qt::AlignCenter);
        cell->setStyleSheet("QLabel { background: #242424; color: #999; "
                            "border: 1px solid #555; border-radius: 3px; padding: 2px 10px; }");
    }
    rowLayout->addWidget(mute);
    rowLayout->addWidget(solo);
    layout->addWidget(row);

    return strip;
}

} // namespace

MixerPanel::MixerPanel(QWidget *parent) : QWidget(parent) {
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(12);

    auto *title = new QLabel(tr("<b>Audio Mixer</b>"), this);
    auto *titleLayout = new QVBoxLayout;
    titleLayout->addWidget(title);
    titleLayout->addStretch(1);
    layout->addLayout(titleLayout);

    // A1 + master strips (video tracks carry no audio strips in M3).
    layout->addWidget(makeStrip(tr("A1"), true, this));
    layout->addWidget(makeStrip(tr("Master"), false, this));
    layout->addStretch(1);
}
