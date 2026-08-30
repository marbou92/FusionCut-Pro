#pragma once

#include <QWidget>

#include "timeline_model.h"

class QSlider;

// Pro Mode bottom panel: multi-track timeline, custom-painted. M4a renders
// real clips from a TimelineModel, supports click-to-select, razor (C) split
// at the playhead or clicked frame, and Delete to remove. Header L/M/S
// cells are read-only state display in this phase (M4b wires toggling).
class TimelinePanel : public QWidget {
    Q_OBJECT

public:
    explicit TimelinePanel(QWidget *parent = nullptr);

    void setModel(const fc::TimelineModel *model);
    void setSequenceDuration(double seconds);
    void setPlayhead(double seconds);
    void setFps(double fps);

public slots:
    void setRazorMode(bool on);
    void clearSelection();

signals:
    void playheadMoved(double seconds);
    void clipSelected(int64_t clipId);
    void splitRequested(int trackIndex, int64_t frame);
    void deleteRequested();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    int areaHeight() const;
    QRect laneRect(int row) const;
    int64_t xToFrame(int x) const;
    int frameToX(int64_t frame) const;
    int trackRowAt(int y) const;
    void drawHeaderColumn(QPainter &painter) const;
    void drawRuler(QPainter &painter) const;
    void drawClips(QPainter &painter) const;
    QColor trackColor(int index) const;

    const fc::TimelineModel *model_ = nullptr;
    double duration_ = 10.0;
    double playhead_ = 0.0;
    double pps_ = 60.0;
    double fps_ = 24.0;
    int64_t selectedClipId_ = -1;
    bool razorMode_ = false;
    QSlider *zoom_ = nullptr;
};
