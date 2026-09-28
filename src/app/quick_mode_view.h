#pragma once

#include <QWidget>

#include <QHash>
#include <QImage>
#include <QSize>
#include <QStringList>

#include "preview_canvas.h"

class QBoxLayout;
class QComboBox;
class QDragEnterEvent;
class QDragLeaveEvent;
class QDragMoveEvent;
class QDropEvent;
class QGridLayout;
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

// Quick Mode page: the CapCut editor layout (owner screenshots, round 4).
// Three columns over a shared top bar and bottom tool stack:
//   left   - the Import library: a dashed drag-and-drop tile, then the
//            imported media as a 2-column card grid (a card tap places
//            the clip, exactly like the old strip's tap);
//   center - the Player: header row (title + a details-panel toggle),
//            the preview canvas, the timecode + painted position slider,
//            and the circular transport;
//   right  - the Details panel: the project rows CapCut shows (Name,
//            Path, Aspect ratio, Resolution, Frame rate, Duration) with
//            a Modify action that hands off to Pro Mode.
// The bottom stack keeps the floating tool bar (now with undo/redo/split)
// and the template covers.
//
// Shares the decode worker and playback state with Pro Mode via signals
// routed through MainWindow. The transport row mirrors TransportBar's
// discipline: the position slider scrubs the program (enabled once a
// duration is known), the step buttons walk one frame, and the slider +
// timecode FOLLOW the playhead without re-emitting seek.
//
// Onboarding surfaces (suggestions #52-54 + round-2 #73-#79): a numbered
// progress rail at the top (1 Import -> 2 Arrange -> 3 Export; the
// current step is the accent pill and doubles as the step action, done
// steps show a checkmark), a hero empty state overlaid on the canvas
// (#74; hides once a program duration is known) with full-page
// drag-and-drop of media files (local files only, forwarded from every
// child via an event filter, #53), double-click-to-import while empty
// (#106), template cards (#77) and a circular QuickTime-style transport
// (#79).
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
    // Media library (#136, grid edition): parallel name/path lists
    // REPLACE the grid contents (the dashed Import tile above the grid
    // is permanent). Pairs are matched by index; a leftover of the
    // longer list is ignored.
    void setMediaItems(const QStringList &names, const QStringList &paths);
    // 16:9 cover for the library card at `path`. Stored for the next
    // rebuild when the item is not on the grid yet.
    void setStripThumbnail(const QString &path, const QImage &thumb);
    // Details panel (CapCut "Details"): the project's name, save path,
    // aspect string ("16:9" or "-"), preview resolution, frame rate and
    // program duration. Values land verbatim; "-" hides a row value.
    void setDetails(const QString &projectName, const QString &projectPath, const QString &aspect,
                    const QSize &resolution, double fps, double durationSeconds);

signals:
    // Routed to the same playback engine as Pro Mode.
    void playToggled(bool playing);
    void seekRequested(double seconds);
    void stepRequested(int frames); // +1 / -1
    // The step rail's Import chip + the hero empty state's primary
    // action + the dashed Import tile (routed to MainWindow::importMedia).
    void importRequested();
    // The step-rail Export chip (routed to MainWindow::exportMedia).
    void exportRequested();
    // Local media files dropped anywhere on the page (#53).
    void filesDropped(const QStringList &paths);
    // A template card was clicked; templateId is "title-broll",
    // "vlog" or "slideshow" (#54).
    void templateRequested(const QString &templateId);
    // Tool bar: the Text tool (MainWindow adds a default title).
    void textToolRequested();
    // Tool bar: the Audio tool (import with an audio filter).
    void audioToolRequested();
    // Tool bar: the Captions tool (the existing SRT import flow).
    void captionsRequested();
    // Tool bar: Effects/Transitions/Filters; `tool` is "effects",
    // "transitions" or "filters" (MainWindow raises that Pro panel).
    void workspaceToolRequested(const QString &tool);
    // Library grid: a media card was activated.
    void mediaActivated(const QString &path);
    // Tool bar edit tools (round 4): routed to MainWindow's undo, redo
    // and split-at-playhead engine entry points.
    void undoRequested();
    void redoRequested();
    void splitRequested();
    // Details panel "Modify": switch to Pro Mode (project settings).
    void detailsModifyRequested();

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
    QWidget *buildLibraryPanel();
    QWidget *buildPlayerColumn();
    QWidget *buildDetailsPanel();
    QWidget *buildPositionRow();
    QWidget *buildTransportRow();
    QWidget *buildTemplateStrip();
    QWidget *buildToolbar();
    void rebuildMediaGrid();
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

    // Three-column editor (round 4).
    QWidget *libraryPanel_ = nullptr;
    QWidget *detailsPanel_ = nullptr;
    QScrollArea *mediaGridArea_ = nullptr;
    QWidget *mediaGridWidget_ = nullptr;
    QGridLayout *mediaGridLayout_ = nullptr;
    QLabel *gridHint_ = nullptr;
    QToolButton *detailsToggle_ = nullptr;
    QLabel *detailName_ = nullptr;
    QLabel *detailPath_ = nullptr;
    QLabel *detailAspect_ = nullptr;
    QLabel *detailResolution_ = nullptr;
    QLabel *detailFps_ = nullptr;
    QLabel *detailDuration_ = nullptr;

    // Media grid model (#136): parallel lists + thumbnail cache keyed
    // by path (pruned when items leave the grid).
    QStringList mediaNames_;
    QStringList mediaPaths_;
    QHash<QString, QImage> thumbnails_;

    int step_ = 0;
    bool dragHover_ = false;

    double duration_ = 0.0;
    double fps_ = 24.0;
    double pos_ = 0.0;
};
