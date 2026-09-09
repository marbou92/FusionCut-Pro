#pragma once

#include <QLabel>
#include <QSlider>
#include <QToolButton>
#include <QWidget>

#include <vector>

namespace fc {
class TimelineModel; // model type is fc-namespaced; the panel itself is
} // namespace fc

// Pro Mode bottom panel (tabbed with Timeline): the audio mixer.
// One strip per AUDIO track (fader -60..+6 dB, pan, mute, solo) plus a
// master strip (fader). Edits write straight into the timeline model
// (setTrackAudio / setTrackState / setMasterGainDb - all revision-
// bumping mutators) and emit mixerChanged() so the window can dirty
// the project and refresh the preview's audio snapshot. refreshFromModel
// rebuilds the strips (call whenever the track set changes: new
// project, project load, track insert/remove).
class MixerPanel : public QWidget {
    Q_OBJECT

public:
    explicit MixerPanel(QWidget *parent = nullptr);

    void refreshFromModel(const fc::TimelineModel *model);

signals:
    void mixerChanged();

private:
    struct Strip {
        int trackIndex = -1;
        QSlider *fader = nullptr;
        QLabel *dbLabel = nullptr;
        QSlider *pan = nullptr;
        QLabel *panLabel = nullptr;
        QToolButton *mute = nullptr;
        QToolButton *solo = nullptr;
    };

    void buildStrip(const QString &name, bool withPan, Strip &out, QWidget *host);

    // Non-const: the strip edits write straight into the model
    // (setTrackAudio / setTrackState / setMasterGainDb - all
    // revision-bumping mutators).
    fc::TimelineModel *model_ = nullptr;
    std::vector<Strip> strips_;
    QSlider *masterFader_ = nullptr;
    QLabel *masterDbLabel_ = nullptr;
    QWidget *stripsHost_ = nullptr;
};
