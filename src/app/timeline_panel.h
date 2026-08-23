#pragma once

#include <QWidget>

class QSlider;

// Pro Mode bottom panel: multi-track timeline shell, fully custom-painted
// (header column, ruler, lanes, playhead). M3 provides the structural
// layout from Module 2.1 - V2/V1/A1 placeholder tracks with lock/mute/solo
// cells, fc::Timecode ruler, playhead scrubbing, zoom. Clip objects and
// edit tools arrive in M4.
class TimelinePanel : public QWidget {
    Q_OBJECT

public:
    explicit TimelinePanel(QWidget *parent = nullptr);

    // Sequence duration in seconds (scales the ruler).
    void setSequenceDuration(double seconds);
    void setPlayhead(double seconds);

    // Frame rate used for the timecode ruler (from the loaded clip).
    void setFps(double fps);

signals:
    void playheadMoved(double seconds);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    struct TrackRow {
        QString name;
        bool isAudio = false;
        bool locked = false;
        bool muted = false;
        bool solo = false;
    };

    int areaHeight() const;
    QRect laneRect(int row) const;
    double xToSeconds(int x) const;
    int secondsToX(double seconds) const;
    void drawHeaderColumn(QPainter &painter) const;
    void drawRuler(QPainter &painter) const;

    double duration_ = 10.0;
    double playhead_ = 0.0;
    double pps_ = 60.0;
    double fps_ = 24.0;
    QList<TrackRow> rows_;
    QSlider *zoom_ = nullptr;
};
