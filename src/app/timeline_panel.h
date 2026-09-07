#pragma once

#include <QWidget>

#include "timeline_model.h"

class QSlider;
class QToolButton;

// Pro Mode bottom panel: multi-track timeline, custom-painted.
//
// M4a: renders clips from a TimelineModel, click-to-select, razor (C)
// split at the clicked frame, Delete to remove.
//
// M4b: full mouse editing - drag-move clips with a magnetic-snap ghost
// (model findDropPosition), edge-drag trim (start/end), Alt+edge-drag
// rolling boundary edits, header L/M/S click toggling, a tool row
// (Select / Razor / Ripple toggle), razor hover preview line, and
// visual dimming for locked / muted / non-solo tracks.
class TimelinePanel : public QWidget {
    Q_OBJECT

public:
    explicit TimelinePanel(QWidget *parent = nullptr);

    void setModel(const fc::TimelineModel *model);
    void setSequenceDuration(double seconds);
    void setPlayhead(double seconds);
    void setFps(double fps);

    bool isRazorMode() const { return razorMode_; }
    // M4b: ripple edit toggle - when on, MainWindow routes delete /
    // end-trim through the model ripple variants (gaps close).
    bool isRippleEnabled() const { return rippleEnabled_; }

public slots:
    void setRazorMode(bool on);
    void clearSelection();
    // M5 Phase 2: drop the transition selection (the cut marker in the
    // lane, not a clip).
    void clearTransitionSelection();
    // M5 Phase 2: select a cut transition programmatically (after adding
    // one); emits transitionSelected so the editor follows.
    void selectTransition(int64_t transitionId);
    // M6 Phase 1: select a clip programmatically (after adding a text
    // clip); emits clipSelected so every panel follows the selection.
    void selectClip(int64_t clipId);

signals:
    void playheadMoved(double seconds);
    void clipSelected(int64_t clipId);
    // M5 Phase 2: a cut transition marker was clicked (id > 0) or the
    // selection was cleared by clicking elsewhere (-1). MainWindow routes
    // it to the transition editor in Effect Controls.
    void transitionSelected(int64_t transitionId);
    void splitRequested(int trackIndex, int64_t frame);
    void deleteRequested();
    // M4b:
    // Ghost-resolved drop: startFrame comes from findDropPosition
    // (already overlap-free); MainWindow still validates via moveClipTo.
    void clipMoveRequested(int64_t clipId, int trackIndex, int64_t startFrame);
    // edge: 0 = head (trimClipStart semantics), 1 = tail (trimClipEnd;
    // routed to rippleTrimClipEnd when the Ripple toggle is on).
    void clipTrimRequested(int64_t clipId, int edge, int64_t deltaFrames);
    // Alt+edge drag on the boundary between two adjacent clips.
    void rollEditRequested(int64_t leftId, int64_t rightId, int64_t deltaFrames);
    // Header box click: which 0 = L(ock), 1 = M(ute), 2 = S(olo).
    void trackStateToggleRequested(int trackIndex, int which);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    enum class DragMode {
        None,
        Move,
        TrimStart,
        TrimEnd,
        RollBoundary,
    };

    int areaHeight() const;
    int contentTop() const; // tool row height offset for all lane geometry
    QRect laneRect(int row) const;
    int64_t xToFrame(int x) const;
    int frameToX(int64_t frame) const;
    int trackRowAt(int y) const;
    void drawHeaderColumn(QPainter &painter) const;
    void drawRuler(QPainter &painter) const;
    void drawClips(QPainter &painter) const;
    // M5 Phase 2: cut transition markers (the window box on the boundary
    // between two adjacent clips, with an X cross).
    void drawTransitions(QPainter &painter) const;
    void drawDragGhost(QPainter &painter) const;
    void drawRazorHover(QPainter &painter) const;
    QColor trackColor(int index) const;
    // Visual dim factor for a track (locked / muted / non-solo-when-any-
    // solo). 1.0 = full, 0.45 = dimmed.
    double trackDimFactor(int index) const;
    // Hit-test the clip at a position; returns nullptr for empty space.
    const fc::Clip *clipAtPos(const QPoint &pos, int *rowOut = nullptr) const;
    // M5 Phase 2: hit-test the transition marker at a position (its
    // central band, so clip edge-drag trims keep working around it).
    const fc::Transition *transitionAtPos(const QPoint &pos) const;
    // The rect the transition marker occupies on its lane.
    QRect transitionRect(const fc::Transition &t) const;
    // Start a drag interaction from a press inside a clip.
    void beginClipDrag(const QPoint &pos);
    void resetDrag();
    // The adjacent right neighbor of a clip (for rolling), or nullptr.
    const fc::Clip *rightNeighborOf(const fc::Clip *clip) const;
    void syncToolButtons();

    const fc::TimelineModel *model_ = nullptr;
    double duration_ = 10.0;
    double playhead_ = 0.0;
    double pps_ = 60.0;
    double fps_ = 24.0;
    int64_t selectedClipId_ = -1;
    int64_t selectedTransitionId_ = -1; // M5 Phase 2
    bool razorMode_ = false;
    bool rippleEnabled_ = false;
    QSlider *zoom_ = nullptr;

    // ---- M4b interaction state ----
    DragMode dragMode_ = DragMode::None;
    int64_t dragClipId_ = -1;
    int64_t dragGrabOffset_ = 0;   // frames: press frame - clip start
    int64_t dragOriginStart_ = 0;  // clip timelineStart at press
    int64_t dragOriginEnd_ = 0;    // clip timelineEnd at press
    int64_t dragBoundary_ = 0;     // rolling: boundary frame at press
    int64_t dragRollLeftId_ = -1;  // rolling: left clip id
    int64_t dragRollRightId_ = -1; // rolling: right clip id
    int64_t dragRollMin_ = 0;      // rolling: boundary min frame (left keeps 1)
    int64_t dragRollMax_ = 0;      // rolling: boundary max frame (right keeps 1)
    int dragRow_ = -1;             // lane the press landed in
    bool ghostValid_ = false;      // move ghost resolved to a legal drop
    int64_t ghostStart_ = 0;       // ghost timeline start (snapped)
    int ghostRow_ = -1;            // ghost lane
    int64_t ghostEnd_ = 0;         // trim/roll ghost end frame
    int razorHoverX_ = -1;         // razor preview line x (-1 = hidden)

    QToolButton *selectTool_ = nullptr;
    QToolButton *razorTool_ = nullptr;
    QToolButton *rippleTool_ = nullptr;
};
