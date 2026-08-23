#pragma once

#include <QWidget>

#include "preview_canvas.h"

class QComboBox;
class QPushButton;
class QSlider;

// Quick Mode page (Module 2.2): CapCut-style simplified layout - large
// preview, prominent toolbar, aspect selector. Shares the decode worker
// and playback state with Pro Mode via signals routed through MainWindow.
class QuickModeView : public QWidget {
    Q_OBJECT

public:
    explicit QuickModeView(QWidget *parent = nullptr);

    PreviewCanvas *canvas() const { return canvas_; }

signals:
    // Routed to the same playback engine as Pro Mode.
    void playToggled(bool playing);
    void seekRequested(double seconds);
    void stepRequested(int frames);

public slots:
    void setPlaying(bool playing);

private:
    QWidget *buildToolbar();
    QWidget *buildTopBar();

    PreviewCanvas *canvas_ = nullptr;
    QPushButton *playButton_ = nullptr;
    QSlider *position_ = nullptr;
    QComboBox *aspectBox_ = nullptr;
};
