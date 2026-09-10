#pragma once

#include <QWidget>

#include "preview_canvas.h"

class QComboBox;
class QLabel;
class QPushButton;
class QSlider;

// Quick Mode page: CapCut-style simplified layout - large
// preview, prominent toolbar, aspect selector. Shares the decode worker
// and playback state with Pro Mode via signals routed through MainWindow.
// The transport row mirrors TransportBar's discipline: the position
// slider scrubs the program (enabled once a duration is known), the
// step buttons walk one frame, and the slider + timecode FOLLOW the
// playhead without re-emitting seek.
class QuickModeView : public QWidget {
    Q_OBJECT

public:
    explicit QuickModeView(QWidget *parent = nullptr);

    PreviewCanvas *canvas() const { return canvas_; }

public slots:
    void setPlaying(bool playing);
    // Range + timecode rate for the PROGRAM sequence (a duration <= 0
    // keeps the scrub slider disabled - nothing to scrub yet).
    void setMedia(double durationSeconds, double fps);
    // Position in seconds (slider follows without emitting seekRequested).
    void setPosition(double seconds);

signals:
    // Routed to the same playback engine as Pro Mode.
    void playToggled(bool playing);
    void seekRequested(double seconds);
    void stepRequested(int frames); // +1 / -1
    // The top-bar Import button (routed to MainWindow::importMedia).
    void importRequested();

private:
    QWidget *buildToolbar();
    QWidget *buildTopBar();
    void refreshTimecode();

    PreviewCanvas *canvas_ = nullptr;
    QPushButton *playButton_ = nullptr;
    QPushButton *stepBack_ = nullptr;
    QPushButton *stepFwd_ = nullptr;
    QSlider *position_ = nullptr;
    QLabel *timecode_ = nullptr;
    QComboBox *aspectBox_ = nullptr;

    double duration_ = 0.0;
    double fps_ = 24.0;
    double pos_ = 0.0;
};
