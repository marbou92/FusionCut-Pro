#pragma once

#include <QWidget>

#include "timecode.h"

class QLabel;
class QPushButton;
class QSlider;

// Program-monitor transport: play/pause, frame step, scrub, and a
// timecode readout driven by fc::Timecode from the core engine.
class TransportBar : public QWidget {
    Q_OBJECT

public:
    explicit TransportBar(QWidget *parent = nullptr);

    // Configures the range/timecode rate for the loaded clip.
    void setMedia(double durationSeconds, double fps);

    // Position in seconds (slider follows without emitting seekRequested).
    void setPosition(double seconds);

    // Reflects external play-state changes (e.g. pause at end of file).
    void setPlaying(bool playing);

signals:
    void playToggled(bool playing);
    void seekRequested(double seconds);
    void stepRequested(int frames); // +1 / -1

private slots:
    void onSliderMoved(int value);
    void onPlayClicked();

private:
    void refreshTimecode();

    QPushButton *playButton_ = nullptr;
    QPushButton *stepBack_ = nullptr;
    QPushButton *stepFwd_ = nullptr;
    QSlider *position_ = nullptr;
    QLabel *timecode_ = nullptr;

    double duration_ = 0.0;
    double fps_ = 24.0;
    double pos_ = 0.0;
    bool playing_ = false;
};
