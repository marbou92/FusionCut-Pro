#pragma once

#include <QWidget>

#include "timeline_model.h"

class QResizeEvent;
class QLineEdit;
class QScrollBar;
class QSlider;
class QTimer;
class QToolButton;

// Pro Mode bottom panel: multi-track timeline, custom-painted.
//
// renders clips from a TimelineModel, click-to-select, razor (C)
// split at the clicked frame, Delete to remove.
//
// full mouse editing - drag-move clips with a magnetic-snap ghost
// (model findDropPosition), edge-drag trim (start/end), Alt+edge-drag
// rolling boundary edits, header L/M/S click toggling, a tool row
// (Select / Razor / Ripple toggle), razor hover preview line, and
// visual dimming for locked / muted / non-solo tracks.
//
// horizontal scrolling: the lane content is drawn in CONTENT
// coordinates and shifted by one scrollX_ offset under a clip rect
// (the header column + panel chrome stay fixed). The scrollbar, plain
// wheel, zoom changes, and playhead-follow all drive that one offset;
// every hit-test converts widget x through xToFrame().
//
// vertical scrolling (#12): the same scheme runs vertically - scrollY_
// shifts every lane row (and the header cells) while the ruler row and
// the header column stay frozen. The wheel routes VERTICALLY when the
// track stack overflows the lanes viewport and horizontally otherwise.
// Ctrl+wheel zooms around the cursor (#10), playback follows the
// playhead (#11), drags auto-page at the viewport edges, and the panel
// carries a minimumSizeHint so the bottom dock can no longer shrink it
// below two usable track rows.
class TimelinePanel : public QWidget {
    Q_OBJECT

public:
    explicit TimelinePanel(QWidget *parent = nullptr);

    void setModel(const fc::TimelineModel *model);
    void setSequenceDuration(double seconds);
    void setPlayhead(double seconds);
    void setFps(double fps);

    bool isRazorMode() const { return razorMode_; }
    // ripple edit toggle - when on, MainWindow routes delete /
    // end-trim through the model ripple variants (gaps close).
    bool isRippleEnabled() const { return rippleEnabled_; }

    // #12: the panel never shrinks below the tool row + ruler + two
    // track rows + the scroll/zoom chrome. QDockWidget honors the
    // inner widget's minimumSizeHint, so the bottom dock respects it.
    QSize minimumSizeHint() const override;

public slots:
    void setRazorMode(bool on);
    void clearSelection();
    // drop the transition selection (the cut marker in the
    // lane, not a clip).
    void clearTransitionSelection();
    // select a cut transition programmatically (after adding
    // one); emits transitionSelected so the editor follows.
    void selectTransition(int64_t transitionId);
    // select a clip programmatically (after adding a text
    // clip); emits clipSelected so every panel follows the selection.
    void selectClip(int64_t clipId);
    // playback follow mode (#11): MainWindow calls this from
    // startPlayback. While active, setPlayhead keeps the playhead in
    // view horizontally and the selected clip's lane in view
    // vertically; ANY user scroll disables the follow until the next
    // false -> true edge re-arms it.
    void setPlaybackActive(bool active);

signals:
    void playheadMoved(double seconds);
    void clipSelected(int64_t clipId);
    // a cut transition marker was clicked (id > 0) or the
    // selection was cleared by clicking elsewhere (-1). MainWindow routes
    // it to the transition editor in Effect Controls.
    void transitionSelected(int64_t transitionId);
    void splitRequested(int trackIndex, int64_t frame);
    void deleteRequested();
    // :
    // Ghost-resolved drop: startFrame comes from findDropPosition
    // (already overlap-free); MainWindow still validates via moveClipTo.
    // An Alt-suspended drag (#14) emits the UNSNAPPED raw start here -
    // MainWindow's validation is the safety net, by design.
    void clipMoveRequested(int64_t clipId, int trackIndex, int64_t startFrame);
    // edge: 0 = head (trimClipStart semantics), 1 = tail (trimClipEnd;
    // routed to rippleTrimClipEnd when the Ripple toggle is on).
    void clipTrimRequested(int64_t clipId, int edge, int64_t deltaFrames);
    // Alt+edge drag on the boundary between two adjacent clips.
    void rollEditRequested(int64_t leftId, int64_t rightId, int64_t deltaFrames);
    // Header box click: which 0 = L(ock), 1 = M(ute), 2 = S(olo).
    void trackStateToggleRequested(int trackIndex, int which);
    // the speed chip (top-right corner of a rate-adjusted clip) was
    // double-clicked: the clip has ALREADY been selected via
    // clipSelected - MainWindow opens the Speed / Duration dialog for
    // the current selection.
    void speedDialogRequested();
    // inline track rename committed (Enter over the header name area).
    // The model has no rename mutator yet - wave 2 adds renameTrack;
    // this signal is the UI-side contract for that wiring.
    void trackRenameRequested(int trackIndex, const QString &name);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

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
    // The lanes viewport in WIDGET coordinates: right of the header
    // column, below the ruler, left of the vertical scrollbar, above
    // the horizontal scrollbar. Lane painting clips to this rect and
    // the edge hit-tests (auto-page, wheel routing) use it too.
    QRect lanesViewport() const;
    QRect laneRect(int row) const;
    // Width of the scrollable lane content in CONTENT pixels: the
    // sequence extent at the current zoom plus tail padding.
    int laneContentWidth() const;
    int64_t xToFrame(int x) const;
    int frameToX(int64_t frame) const;
    // Lane row under a widget y, or -1 outside the lanes viewport band.
    // Rows past the last track stay valid indices ("empty space" -
    // callers treat them as no-clip / playhead scrub, as before).
    int trackRowAt(int y) const;
    // Scroll plumbing: recompute both scrollbar ranges for the current
    // duration/zoom/size and clamp scrollX_/scrollY_ into them; keep the
    // playhead in view after zoom/position changes; one zoom entry
    // point for the slider and Ctrl+wheel.
    void updateScrollRange();
    void ensurePlayheadVisible();
    // Playback vertical follow (#11): scroll the MINIMUM amount that
    // brings the selected clip's lane back into the lanes viewport.
    void ensureSelectedLaneVisible();
    void applyZoom(double pps);
    // Single zoom entry point with an anchor (#10): after pps_ changes,
    // the content frame `anchorFrame` is placed back under the widget
    // x `anchorX`. The pps-only overload anchors at the playhead's
    // current position (slider zoom keeps the legacy behavior).
    void applyZoom(double pps, int anchorX, int64_t anchorFrame);
    // Fit-to-sequence ("\"): the zoom level at which the whole sequence
    // spans the lanes viewport, scrolled to the start.
    void fitToSequence();
    void drawHeaderColumn(QPainter &painter) const;
    void drawRuler(QPainter &painter) const;
    void drawClips(QPainter &painter) const;
    // cut transition markers (the window box on the boundary
    // between two adjacent clips, with an X cross).
    void drawTransitions(QPainter &painter) const;
    void drawDragGhost(QPainter &painter) const;
    void drawRazorHover(QPainter &painter) const;
    QColor trackColor(int index) const;
    // Visual dim factor for a track (locked / muted / non-solo-when-any-
    // solo). 1.0 = full, 0.45 = dimmed.
    double trackDimFactor(int index) const;
    // The clip body rect in content coordinates (the one drawClips
    // paints); shared with the chip helpers so hit-tests match the
    // paint exactly.
    QRect clipBodyRect(const fc::Clip &clip, int row) const;
    // Speed chip (#9): the top-right corner chip on clips whose rate
    // deviates from 1.0. Hit-tested by the double-click handler.
    QRect speedChipRect(const fc::Clip &clip, int row) const;
    // Hit-test the clip at a position; returns nullptr for empty space.
    const fc::Clip *clipAtPos(const QPoint &pos, int *rowOut = nullptr) const;
    // hit-test the transition marker at a position (its
    // central band, so clip edge-drag trims keep working around it).
    const fc::Transition *transitionAtPos(const QPoint &pos) const;
    // The rect the transition marker occupies on its lane.
    QRect transitionRect(const fc::Transition &t) const;
    // The header cell's L/M/S box under pos (-1 = none), scroll-aware.
    int headerBoxAt(const QPoint &pos, int row) const;
    // Track header context menu (right click): checkable
    // Lock/Mute/Solo emitting the existing trackStateToggleRequested.
    void showTrackContextMenu(const QPoint &globalPos, int row);
    // Inline track-name rename editor (#13): created on demand over the
    // name rect, Enter commits via trackRenameRequested, Escape /
    // focus-out cancels.
    void startRename(int row);
    void endRename(bool commit);
    // Start a drag interaction from a press inside a clip.
    void beginClipDrag(const QPoint &pos);
    void resetDrag();
    // Commit a live drag exactly as mouseReleaseEvent would (emits the
    // move / trim / roll request when the ghost differs from the
    // origin), then reset. The release handler and the mouse-move
    // implicit-grab-lost guard share this path.
    void finishDrag();
    // Drag ghost resolution, shared verbatim by mouse moves and drag
    // auto-page steps so both paths compute the exact same ghost.
    void updateDragGhost(const QPoint &pos);
    // Drag auto-paging: while a drag is active and the pointer sits
    // within the edge zone of the lanes viewport, a repeating timer
    // scrolls that way (vertical scrollY_ / horizontal scrollX_) and
    // re-resolves the ghost exactly as a mouse move would.
    void updateAutoPage(const QPoint &pos);
    void stopAutoPage();
    void autoPageStep();
    // The adjacent right neighbor of a clip (for rolling), or nullptr.
    const fc::Clip *rightNeighborOf(const fc::Clip *clip) const;
    void syncToolButtons();
    // Position the manual children that live inside the painted band:
    // the vertical scrollbar hugs the right edge of the lanes area.
    void layoutChrome();

    const fc::TimelineModel *model_ = nullptr;
    double duration_ = 10.0;
    double playhead_ = 0.0;
    double pps_ = 60.0;
    double fps_ = 24.0;
    int scrollX_ = 0; // horizontal scroll offset (CONTENT px past the fixed header)
    int scrollY_ = 0; // vertical scroll offset (px past the first lane's top)
    int64_t selectedClipId_ = -1;
    int64_t selectedTransitionId_ = -1;
    bool razorMode_ = false;
    bool rippleEnabled_ = false;
    bool followPlayhead_ = true;  // #11: playback auto-follow is armed
    bool playbackActive_ = false; // set by setPlaybackActive (MainWindow)
    bool scrollGuard_ = true;     // true during programmatic scroll syncs
    QSlider *zoom_ = nullptr;
    QScrollBar *hscroll_ = nullptr;
    QScrollBar *vscroll_ = nullptr;

    // ---- interaction state ----
    DragMode dragMode_ = DragMode::None;
    int64_t dragClipId_ = -1;
    int64_t dragGrabOffset_ = 0;   // frames: press frame - clip start
    int64_t dragOriginStart_ = 0;  // clip timelineStart at press
    int64_t dragOriginEnd_ = 0;    // clip timelineEnd at press
    int64_t dragRawStart_ = 0;     // move drag: UNSNAPPED start (snap indicator)
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
    QPoint lastMousePos_;          // drag pointer pos (auto-page ghost recompute)
    QTimer *autoPageTimer_ = nullptr;
    int autoPageDx_ = 0; // auto-page direction per tick (px, 0 = idle)
    int autoPageDy_ = 0;

    // ---- inline track rename (#13) ----
    QLineEdit *renameEditor_ = nullptr;
    int renameRow_ = -1;

    QToolButton *selectTool_ = nullptr;
    QToolButton *razorTool_ = nullptr;
    QToolButton *rippleTool_ = nullptr;
};
