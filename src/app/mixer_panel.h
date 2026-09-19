#pragma once

#include <QLabel>
#include <QSlider>
#include <QToolButton>
#include <QWidget>

#include <QVector>

#include <vector>

class QTimer;

class LevelMeter; // the internal peak-hold meter widget (mixer_panel.cpp)

namespace fc {
class TimelineModel; // model type is fc-namespaced; the panel itself is
} // namespace fc

// Pro Mode bottom panel (tabbed with Timeline): the audio mixer.
// One strip per AUDIO track (fader -60..+6 dB, pan, mute, solo) plus a
// master strip (fader, rightmost, accent-framed). Edits write straight
// into the timeline model (setTrackAudio / setTrackState /
// setMasterGainDb - all revision-bumping mutators) and emit
// mixerChanged() so the window can dirty the project and refresh the
// preview's audio snapshot. refreshFromModel rebuilds the strips (call
// whenever the track set changes: new project, project load, track
// insert/remove).
//
// Meters (suggestion #45): every strip carries a compact peak-hold
// level meter under its fader (the audio pull path feeds them via
// setLevels / setMasterLevel; a click on a meter clears its hold).
class MixerPanel : public QWidget {
    Q_OBJECT

public:
    explicit MixerPanel(QWidget *parent = nullptr);

    void refreshFromModel(const fc::TimelineModel *model);

public slots:
    // Per-strip dBFS levels (-60..0, clamped; -60 or below = silence).
    // An empty vector idles every strip; strips beyond the vector's
    // length idle too. Feeds the meters only - no model interaction.
    void setLevels(const QVector<double> &dbfsPerTrack);
    // The master strip's dBFS level (same clamp/silence semantics).
    void setMasterLevel(double dbfs);

signals:
    void mixerChanged();

protected:
    // Double-click on any fader (strips + master) resets it to 0 dB -
    // through the SAME valueChanged -> model-write path a drag uses.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Strip {
        int trackIndex = -1;
        QSlider *fader = nullptr;
        QLabel *dbLabel = nullptr;
        QSlider *pan = nullptr;
        QLabel *panLabel = nullptr;
        QToolButton *mute = nullptr;
        QToolButton *solo = nullptr;
        LevelMeter *meter = nullptr;
    };

    void buildStrip(const QString &name, bool withPan, Strip &out, QWidget *host);
    void startHoldTimer(); // the 10 Hz hold-decay tick (levels are live)

    // Non-const: the strip edits write straight into the model
    // (setTrackAudio / setTrackState / setMasterGainDb - all
    // revision-bumping mutators).
    fc::TimelineModel *model_ = nullptr;
    std::vector<Strip> strips_;
    QSlider *masterFader_ = nullptr;
    QLabel *masterDbLabel_ = nullptr;
    LevelMeter *masterMeter_ = nullptr;
    QTimer *holdTimer_ = nullptr;
    QWidget *stripsHost_ = nullptr;
};
