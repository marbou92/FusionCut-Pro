#include "mixer_panel.h"

#include <cmath>

#include <QHBoxLayout>
#include <QScrollArea>
#include <QVBoxLayout>

#include "timeline_model.h"

namespace {

QString dbText(int db) {
    return db <= -60 ? QObject::tr("mute") : QString("%1 dB").arg(db);
}

QString panText(int pan) {
    if (pan == 0) {
        return QObject::tr("Pan C");
    }
    return pan < 0 ? QString("L%1").arg(std::abs(pan)) : QString("R%1").arg(std::abs(pan));
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

    // Strips live in a scroll area so an unusual number of audio lanes
    // never breaks the layout.
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    stripsHost_ = new QWidget(scroll);
    auto *stripsLayout = new QHBoxLayout(stripsHost_);
    stripsLayout->setContentsMargins(0, 0, 0, 0);
    stripsLayout->setSpacing(12);
    stripsLayout->addStretch(1);
    scroll->setWidget(stripsHost_);
    scroll->setMinimumHeight(210);
    layout->addWidget(scroll, 1);

    // Master strip (no pan, no mute/solo - it is everything summed).
    auto *master = new QWidget(this);
    auto *masterLayout = new QVBoxLayout(master);
    masterLayout->setContentsMargins(6, 6, 6, 6);
    auto *masterTitle = new QLabel(tr("Master"), master);
    masterTitle->setAlignment(Qt::AlignCenter);
    masterFader_ = new QSlider(Qt::Vertical, master);
    masterFader_->setRange(-60, 6);
    masterFader_->setValue(0);
    masterDbLabel_ = new QLabel(dbText(0), master);
    masterDbLabel_->setAlignment(Qt::AlignCenter);
    masterLayout->addWidget(masterTitle);
    masterLayout->addWidget(masterFader_, 1);
    masterLayout->addWidget(masterDbLabel_);
    master->setFixedWidth(84);
    layout->addWidget(master);

    connect(masterFader_, &QSlider::valueChanged, this, [this](int db) {
        masterDbLabel_->setText(dbText(db));
        if (model_) {
            model_->setMasterGainDb(double(db));
            emit mixerChanged();
        }
    });
}

void MixerPanel::buildStrip(const QString &name, bool withPan, Strip &out, QWidget * /*host*/) {
    auto *strip = new QWidget(stripsHost_);
    strip->setFixedWidth(84);
    auto *stripLayout = new QVBoxLayout(strip);
    stripLayout->setContentsMargins(6, 6, 6, 6);

    auto *title = new QLabel(name, strip);
    title->setAlignment(Qt::AlignCenter);

    out.fader = new QSlider(Qt::Vertical, strip);
    out.fader->setRange(-60, 6);
    out.fader->setValue(0);

    out.dbLabel = new QLabel(dbText(0), strip);
    out.dbLabel->setAlignment(Qt::AlignCenter);

    stripLayout->addWidget(title);
    stripLayout->addWidget(out.fader, 1);
    stripLayout->addWidget(out.dbLabel);

    if (withPan) {
        out.pan = new QSlider(Qt::Horizontal, strip);
        out.pan->setRange(-100, 100);
        out.pan->setValue(0);
        out.panLabel = new QLabel(panText(0), strip);
        out.panLabel->setAlignment(Qt::AlignCenter);
        stripLayout->addWidget(out.pan);
        stripLayout->addWidget(out.panLabel);
    }

    // Mute / solo pair (checkable, mutually aware at the model level -
    // the solo LOGIC lives in the flatten, not the buttons).
    auto *row = new QWidget(strip);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(4);
    out.mute = new QToolButton(row);
    out.mute->setText("M");
    out.mute->setCheckable(true);
    out.mute->setToolTip(tr("Mute this track"));
    out.solo = new QToolButton(row);
    out.solo->setText("S");
    out.solo->setCheckable(true);
    out.solo->setToolTip(tr("Solo this track (other audio tracks fall silent)"));
    for (QToolButton *cell : {out.mute, out.solo}) {
        cell->setMinimumWidth(32);
        rowLayout->addWidget(cell, 1);
    }
    stripLayout->addWidget(row);

    const int trackIndex = out.trackIndex;
    connect(out.fader, &QSlider::valueChanged, this, [this, out, trackIndex](int db) {
        out.dbLabel->setText(dbText(db));
        if (!model_) {
            return;
        }
        const fc::Track *track = model_->trackAt(trackIndex);
        if (!track) {
            return;
        }
        model_->setTrackAudio(trackIndex, double(db), track->pan);
        emit mixerChanged();
    });
    if (out.pan) {
        connect(out.pan, &QSlider::valueChanged, this, [this, out, trackIndex](int pan) {
            out.panLabel->setText(panText(pan));
            if (!model_) {
                return;
            }
            const fc::Track *track = model_->trackAt(trackIndex);
            if (!track) {
                return;
            }
            model_->setTrackAudio(trackIndex, track->gainDb, double(pan) / 100.0);
            emit mixerChanged();
        });
    }
    connect(out.mute, &QToolButton::toggled, this, [this, trackIndex](bool muted) {
        if (!model_) {
            return;
        }
        const fc::Track *track = model_->trackAt(trackIndex);
        if (!track) {
            return;
        }
        model_->setTrackState(trackIndex, track->locked, muted, track->solo);
        emit mixerChanged();
    });
    connect(out.solo, &QToolButton::toggled, this, [this, trackIndex](bool solo) {
        if (!model_) {
            return;
        }
        const fc::Track *track = model_->trackAt(trackIndex);
        if (!track) {
            return;
        }
        model_->setTrackState(trackIndex, track->locked, track->muted, solo);
        emit mixerChanged();
    });

    // Strips go before the trailing stretch (the layout keeps it last).
    if (auto *h = qobject_cast<QHBoxLayout *>(stripsHost_->layout())) {
        h->insertWidget(h->count() - 1, strip);
    }
}

void MixerPanel::refreshFromModel(const fc::TimelineModel *model) {
    model_ = const_cast<fc::TimelineModel *>(model);

    // Drop the old strips (every direct child of the strips host is a
    // strip; the layout's stretch is an item, not a widget).
    const auto olds = stripsHost_->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
    for (QWidget *w : olds) {
        w->deleteLater();
    }
    strips_.clear();

    if (model) {
        for (const fc::Track &track : model->tracks()) {
            if (!track.isAudio) {
                continue; // video/text lanes carry no strip
            }
            Strip strip;
            strip.trackIndex = track.index;
            buildStrip(QString::fromStdString(track.name), true, strip, stripsHost_);
            strip.fader->blockSignals(true);
            strip.fader->setValue(int(std::llround(track.gainDb)));
            strip.fader->blockSignals(false);
            strip.dbLabel->setText(dbText(int(std::llround(track.gainDb))));
            if (strip.pan) {
                strip.pan->blockSignals(true);
                strip.pan->setValue(int(std::llround(track.pan * 100.0)));
                strip.pan->blockSignals(false);
                strip.panLabel->setText(panText(int(std::llround(track.pan * 100.0))));
            }
            strip.mute->blockSignals(true);
            strip.mute->setChecked(track.muted);
            strip.mute->blockSignals(false);
            strip.solo->blockSignals(true);
            strip.solo->setChecked(track.solo);
            strip.solo->blockSignals(false);
            strips_.push_back(strip);
        }
    }

    if (masterFader_ && model) {
        masterFader_->blockSignals(true);
        masterFader_->setValue(int(std::llround(model->masterGainDb())));
        masterFader_->blockSignals(false);
        masterDbLabel_->setText(dbText(int(std::llround(model->masterGainDb()))));
    }
}
