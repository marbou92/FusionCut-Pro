#pragma once

#include <QWidget>

#include <QHash>
#include <QImage>
#include <QStringList>

#include "preview_canvas.h"

class QBoxLayout;
class QComboBox;
class QDragEnterEvent;
class QDragLeaveEvent;
class QDragMoveEvent;
class QDropEvent;
class QMouseEvent;
class QPaintEvent;
class QLabel;
class QPushButton;
class QScrollArea;
class QSlider;
class QToolButton;

namespace fc {
class EmptyState;
}

// Quick Mode page: CapCut-style simplified layout - large
// preview, prominent toolbar, aspect selector. Shares the decode worker
// and playback state with Pro Mode via signals routed through MainWindow.
// The transport row mirrors TransportBar's discipline: the position
// slider scrubs the program (enabled once a duration is known), the
// step buttons walk one frame, and the slider + timecode FOLLOW the
// playhead without re-emitting seek.
//
// Onboarding surfaces (suggestions #52-54 + round-2 #73-#79): a numbered
// progress rail at the top (1 Import -> 2 Arrange -> 3 Export; the
// current step is the accent pill and doubles as the step action, done
// steps show a checkmark), a hero empty state overlaid on the canvas
// (#74; hides once a program duration is known) with full-page
// drag-and-drop of media files (local files only, forwarded from every
// child via an event filter, #53), double-click-to-import while empty
// (#106), a floating icon-first tool bar (#75/#76), template cards
// (#77) and a circular QuickTime-style transport (#79).
//
// Round 3 (#135-#152) translates the CapCut structure: three zones -
// top bar (step rail | aspect pill | Export pill), the preview canvas,
// and a fixed-height bottom stack (painted position slider + timecode
// chip, centered transport circles, wired tool bar, media strip). The
// tool bar emits the new *ToolRequested/workspaceToolRequested signals;
// the media strip is coordinator-fed via setMediaItems/setStripThumbnail
// and reports activations through mediaActivated.
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
    // Media strip (#136): parallel name/path lists REPLACE the strip
    // contents (the accent Import card always stays first). Pairs are
    // matched by index; a leftover of the longer list is ignored.
    void setMediaItems(const QStringList &names, const QStringList &paths);
    // 16:9 cover for the strip card at `path`. Stored for the next
    // rebuild when the item is not on the strip yet.
    void setStripThumbnail(const QString &path, const QImage &thumb);

signals:
    // Routed to the same playback engine as Pro Mode.
    void playToggled(bool playing);
    void seekRequested(double seconds);
    void stepRequested(int frames); // +1 / -1
    // The step rail's Import chip + the hero empty state's primary
    // action (routed to MainWindow::importMedia).
    void importRequested();
    // The step-rail Export chip (routed to MainWindow::exportMedia).
    void exportRequested();
    // Local media files dropped anywhere on the page (#53).
    void filesDropped(const QStringList &paths);
    // A template card was clicked; templateId is "title-broll",
    // "vlog" or "slideshow" (#54).
    void templateRequested(const QString &templateId);
    // Tool bar #139: the Text tool (MainWindow adds a default title).
    void textToolRequested();
    // Tool bar #140: the Audio tool (import with an audio filter).
    void audioToolRequested();
    // Tool bar #141: the Captions tool (the existing SRT import flow).
    void captionsRequested();
    // Tool bar #142: Effects/Transitions/Filters; `tool` is "effects",
    // "transitions" or "filters" (MainWindow raises that Pro panel).
    void workspaceToolRequested(const QString &tool);
    // Media strip #136: a media card was activated.
    void mediaActivated(const QString &path);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    // Forwards drag events that land on child widgets (they would
    // otherwise be swallowed) to this page's own handlers.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QWidget *buildTopBar();
    QWidget *buildStepRail();
    QWidget *buildCanvasZone();
    QWidget *buildPositionRow();
    QWidget *buildTransportRow();
    QWidget *buildTemplateStrip();
    QWidget *buildToolbar();
    QWidget *buildMediaStrip();
    void rebuildMediaStrip();
    void applyStepStyles();
    void refreshTimecode();

    PreviewCanvas *canvas_ = nullptr;
    fc::EmptyState *emptyState_ = nullptr;
    QToolButton *playButton_ = nullptr;
    QToolButton *stepBack_ = nullptr;
    QToolButton *stepFwd_ = nullptr;
    QSlider *position_ = nullptr;
    QLabel *timecode_ = nullptr;
    QComboBox *aspectBox_ = nullptr;
    QPushButton *stepChips_[3] = {nullptr, nullptr, nullptr};
    QWidget *stepLinks_[2] = {nullptr, nullptr};
    QScrollArea *mediaStrip_ = nullptr;
    QWidget *mediaRow_ = nullptr;
    QBoxLayout *mediaRowLayout_ = nullptr;

    // Media strip model (#136): parallel lists + thumbnail cache keyed
    // by path (pruned when items leave the strip).
    QStringList mediaNames_;
    QStringList mediaPaths_;
    QHash<QString, QImage> thumbnails_;

    int step_ = 0;
    bool dragHover_ = false;

    double duration_ = 0.0;
    double fps_ = 24.0;
    double pos_ = 0.0;
};
