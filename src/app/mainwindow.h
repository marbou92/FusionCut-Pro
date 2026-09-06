#pragma once

#include <QImage>
#include <QMainWindow>
#include <QTimer>

#include "media_item.h"
#include "timeline_model.h"

class QLabel;
class QThread;
class QStackedWidget;
class DecodeWorker;
class EffectsPanel;
class EffectControlsPanel;
class MixerPanel;
class PreviewCanvas;
class ProjectPanel;
class QuickModeView;
class TimelinePanel;
class TransitionsPanel;
class TransportBar;

namespace fc {

// Pre-alpha dual-mode shell (M3): Pro Mode dockable workspace + Quick
// Mode page, sharing one background decode worker and playback clock.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    // Workspace construction.
    void applyDarkTheme();
    void buildMenus();
    void buildStatusBar();
    void buildProWorkspace();
    void buildQuickWorkspace();
    void buildDecodeThread();

    // Mode switching.
    void setMode(bool pro);

    // Media + playback flow.
    void importMedia();
    void loadClip(const QString &sourcePath);
    void addPendingClip(const QString &sourcePath, int64_t sourceOutFrames);
    void splitAtPlayhead();
    void deleteSelectedClip();
    void generateProxy(const QString &sourcePath);
    void startPlayback(bool playing);
    void stepFrames(int frames);
    void requestFrameAt(double seconds);

    // M4b: timeline editing actions (drag-move / trim / roll / L-M-S).
    void moveClipTo(int64_t clipId, int trackIndex, int64_t startFrame);
    void trimClip(int64_t clipId, int edge, int64_t deltaFrames);
    void toggleTrackState(int row, int which);
    // M4b: sequence duration = timeline extent (falls back to the loaded
    // media duration for an empty timeline); refreshes panel + transport.
    void updateSequenceDuration();

    // M5: effects pipeline - raw-frame cache + live stack application.
    // frameReady stores the decoded frame (pre-effects) and the clip it
    // belongs to; applyProgramFrame copies it, runs the clip's effect
    // stack in place, and pushes the result to the program monitor.
    // Parameter edits re-run it instantly from the cache - no decode
    // round-trip.
    void applyProgramFrame();
    void addEffectToSelectedClip(const QString &effectId);

    // M5 Phase 2: cut transitions - add/remove/duration routing for the
    // panels, the held-frame fetch for the incoming clip, and the
    // cross-clip composite in applyProgramFrame.
    void addTransitionToSelectedClip(const QString &kind);
    void ensureHeldIncomingFrame(const fc::TransitionSample &sample);
    void pushTransitionToEditor(const fc::Transition &t);
    // After model mutations: drop the editor when the selected transition
    // was pruned, refresh its bounds when durations clamped.
    void syncTransitionEditor();

    // Restore/save panel layout.
    void restoreLayout();
    void saveLayout() const;

    DecodeWorker *worker_ = nullptr;
    QThread *decodeThread_ = nullptr;
    // M5 Phase 2: second decode context dedicated to the HELD first frame
    // of the incoming clip during a transition window. It never drives
    // the program source (worker_ owns that), so its mediaInfo is ignored
    // except to trigger the held-frame request.
    DecodeWorker *workerB_ = nullptr;
    QThread *decodeThreadB_ = nullptr;
    QTimer *playClock_;

    // Pro Mode widgets.
    ProjectPanel *projectPanel_ = nullptr;
    EffectsPanel *effectsPanel_ = nullptr;
    TransitionsPanel *transitionsPanel_ = nullptr; // M5 Phase 2
    TimelinePanel *timeline_ = nullptr;
    MixerPanel *mixer_ = nullptr;
    EffectControlsPanel *effectControls_ = nullptr;
    PreviewCanvas *sourceCanvas_ = nullptr;
    PreviewCanvas *programCanvas_ = nullptr;
    TransportBar *transport_ = nullptr;
    QStackedWidget *pages_ = nullptr;

    // Quick Mode widgets.
    QuickModeView *quickView_ = nullptr;

    // Editing model (Module 4).
    fc::TimelineModel model_;

    // Playback state.
    QString loadedPath_;
    QString proxySourcePath_;
    QString pendingAddClipPath_;
    int64_t selectedClipId_ = -1;
    int64_t lastProgramClipId_ = -1; // M4b: debounce for program source switches
    double playhead_ = 0.0;
    double duration_ = 0.0;
    double fps_ = 24.0;
    bool playing_ = false;
    bool captureThumbnail_ = false;
    bool pendingProgramSeek_ = false; // M4b: apply seek after a program source switch

    // M5: the last decoded program frame BEFORE effects (plus the clip it
    // was decoded for) - the instant-preview source for effect edits.
    QImage rawProgramFrame_;
    double rawFramePts_ = 0.0;
    int64_t frameClipId_ = -1;

    // M5 Phase 2 transition preview state: the HELD (incoming) frame and
    // the clip it belongs to, plus the worker-B source bookkeeping. The
    // held frame is decoded once per incoming clip and reused for every
    // frame of the window (see the model's window semantics).
    QImage heldIncomingFrame_;
    int64_t heldIncomingClipId_ = -1;
    int64_t pendingBFirstClipId_ = -1; // clip whose held frame is being fetched
    int64_t bFailedClipId_ = -1;       // clip whose fetch already failed
    bool pendingBFirst_ = false;       // waiting for openQuiet -> mediaInfo
    bool bRequestInFlight_ = false;    // an open or decode is queued on worker B
    QString bLoadedPath_;
    int64_t selectedTransitionId_ = -1;
};

} // namespace fc
