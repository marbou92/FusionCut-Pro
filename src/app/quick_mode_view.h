#pragma once

#include <QWidget>

#include "preview_canvas.h"

class QComboBox;
class QDragEnterEvent;
class QDragLeaveEvent;
class QDragMoveEvent;
class QDropEvent;
class QLabel;
class QPaintEvent;
class QPushButton;
class QSlider;
class QStringList;

// Quick Mode page: CapCut-style simplified layout - large
// preview, prominent toolbar, aspect selector. Shares the decode worker
// and playback state with Pro Mode via signals routed through MainWindow.
// The transport row mirrors TransportBar's discipline: the position
// slider scrubs the program (enabled once a duration is known), the
// step buttons walk one frame, and the slider + timecode FOLLOW the
// playhead without re-emitting seek.
//
// Onboarding surfaces (suggestions #52-54): a three-step rail at the
// top (Import -> Arrange -> Export), full-page drag-and-drop of media
// files (local files only, forwarded from every child via an event
// filter), and a template strip at the bottom that emits prefills.
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
    // Highlights the current step in the rail: 0 = import,
    // 1 = arrange, 2 = export (values outside 0-2 are clamped).
    void setStep(int step);

signals:
    // Routed to the same playback engine as Pro Mode.
    void playToggled(bool playing);
    void seekRequested(double seconds);
    void stepRequested(int frames); // +1 / -1
    // The top-bar Import button (routed to MainWindow::importMedia).
    void importRequested();
    // The step-rail Export chip (routed to MainWindow::exportMedia).
    void exportRequested();
    // Local media files dropped anywhere on the page (#53).
    void filesDropped(const QStringList &paths);
    // A template chip was clicked; templateId is "title-broll",
    // "vlog" or "slideshow" (#54).
    void templateRequested(const QString &templateId);

protected:
    void paintEvent(QPaintEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    // Forwards drag events that land on child widgets (they would
    // otherwise be swallowed) to this page's own handlers.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QWidget *buildToolbar();
    QWidget *buildTopBar();
    QWidget *buildStepRail();
    QWidget *buildTemplateStrip();
    void applyStepStyles();
    void refreshTimecode();

    PreviewCanvas *canvas_ = nullptr;
    QPushButton *playButton_ = nullptr;
    QPushButton *stepBack_ = nullptr;
    QPushButton *stepFwd_ = nullptr;
    QSlider *position_ = nullptr;
    QLabel *timecode_ = nullptr;
    QComboBox *aspectBox_ = nullptr;
    QPushButton *stepChips_[3] = {nullptr, nullptr, nullptr};
    QWidget *stepLinks_[2] = {nullptr, nullptr};

    int step_ = 0;
    bool dragHover_ = false;

    double duration_ = 0.0;
    double fps_ = 24.0;
    double pos_ = 0.0;
};
