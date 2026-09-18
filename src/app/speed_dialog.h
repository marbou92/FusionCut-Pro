#pragma once

#include <QDialog>

#include <cstdint>

class QCheckBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;

namespace fc {

// The clip speed / duration dialog. A percentage spinbox (100 % = the
// source's own speed) plus a live preview of the resulting timeline
// length (the source extent rescales by the rate; the source media
// itself is untouched). When the clip has linked audio siblings from
// the same source (a video+audio import creates them), a toggle offers
// to apply the same rate to them so picture and sound stay in step.
class SpeedDialog : public QDialog {
    Q_OBJECT

public:
    // clipLabel = the edited clip's name (shown in the intro),
    // currentRate = the clip's playback rate (the initial spinbox value
    // is currentRate * 100), sourceExtentFrames = the clip's source
    // extent (sourceOut - sourceIn), currentDurationFrames = its
    // timeline length right now, fps = the sequence rate (the seconds
    // display), hasLinkedAudio = whether the "apply to linked audio"
    // toggle is offered at all.
    explicit SpeedDialog(const QString &clipLabel, double currentRate, int64_t sourceExtentFrames,
                         int64_t currentDurationFrames, double fps, bool hasLinkedAudio,
                         QWidget *parent = nullptr);

    // The chosen playback rate (spinbox percent / 100).
    double rate() const;
    // Whether the linked audio siblings should receive the same rate.
    bool includeLinkedAudio() const;

private:
    // Recomputes the resulting timeline length from the current
    // percentage and enables/disables OK (a length that rounds below
    // one frame cannot exist on the timeline).
    void updateLength();

    double fps_ = 24.0;
    int64_t sourceExtentFrames_ = 0;
    int64_t currentDurationFrames_ = 0;

    QDoubleSpinBox *speed_ = nullptr;
    QCheckBox *linkedAudio_ = nullptr;
    QLabel *length_ = nullptr;
    QDialogButtonBox *buttons_ = nullptr;
};

} // namespace fc
