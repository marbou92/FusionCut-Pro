#include "mainwindow.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPalette>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabWidget>
#include <QThread>
#include <QVBoxLayout>

#include <fc/version.h>

#include "color_panel.h"
#include "decode_worker.h"
#include "effect_controls_panel.h"
#include "effects.h"
#include "effects_panel.h"
#include "export_dialog.h"
#include "exporter.h"
#include "ffmpeg_wrappers.h"
#include "media_probe.h"
#include "mixer_panel.h"
#include "preview_canvas.h"
#include "project_format.h"
#include "project_panel.h"
#include "quick_mode_view.h"
#include "srt.h"
#include "text.h"
#include "text_panel.h"
#include "text_renderer.h"

#include "audio_math.h"
#include "audio_mixer.h"
#include "timeline_panel.h"
#include "transitions.h"
#include "transitions_panel.h"
#include "transport_bar.h"
#include "video_decoder.h"

namespace fc {

namespace {

// FusionCut Pro design tokens (the shared design system).
constexpr unsigned int kWindowBg = 0x1E1E1E; // charcoal
constexpr unsigned int kPanelBg = 0x252525;  // panel background
constexpr unsigned int kButtonBg = 0x2E2E2E;
constexpr unsigned int kText = 0xE8E8E8;
constexpr unsigned int kAccent = 0x00A8FF; // FusionCut accent

QAction *addMenuAction(QMenu *menu, const QString &text,
                       const QKeySequence &shortcut = QKeySequence()) {
    QAction *action = menu->addAction(text);
    if (!shortcut.isEmpty()) {
        action->setShortcut(shortcut);
    }
    return action;
}

QDockWidget *makeDock(const QString &title, QWidget *inner, QMainWindow *parent) {
    auto *dock = new QDockWidget(title, parent);
    dock->setWidget(inner);
    dock->setObjectName(title.toLower().remove(' '));
    dock->setAllowedAreas(Qt::AllDockWidgetAreas);
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable |
                      QDockWidget::DockWidgetFloatable);
    parent->addDockWidget(Qt::LeftDockWidgetArea, dock);
    return dock;
}

QString proxyPathFor(const QString &sourcePath) {
    return sourcePath + QStringLiteral(".fcproxy.mp4");
}

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle(tr("FusionCut Pro"));
    resize(1280, 720);
    applyDarkTheme();

    buildDecodeThread();
    buildProWorkspace();
    buildQuickWorkspace();
    buildMenus();
    buildStatusBar();

    // Timeline model: three default tracks (two video, one audio).
    model_.setFps(fps_);
    model_.addTrack("V2", false);
    model_.addTrack("V1", false);
    model_.addTrack("A1", true);
    timeline_->setModel(&model_);

    playClock_ = new QTimer(this);
    playClock_->setTimerType(Qt::CoarseTimer);
    connect(playClock_, &QTimer::timeout, this, [this] {
        // While the preview audio device runs, ITS consumed position is
        // the playhead's clock (a QTimer drifts with load; the audio
        // stream cannot). Without audio the wall tick advances as
        // before. Track edits made during playback land in the mix via
        // the revision check.
        if (audioPreview_ && audioPreview_->running()) {
            if (model_.revision() != audioSpansRevision_) {
                rebuildAudioSnapshot();
            }
            playhead_ = audioStartSeconds_ + audioPreview_->playedSeconds();
        } else {
            playhead_ += 1.0 / fps_;
        }
        if (playhead_ >= duration_) {
            playhead_ = duration_;
            requestFrameAt(playhead_);
            startPlayback(false);
            return;
        }
        requestFrameAt(playhead_);
    });

    // Keyboard: space, arrow step (both modes).
    auto *space = new QShortcut(QKeySequence(Qt::Key_Space), this);
    connect(space, &QShortcut::activated, this, [this] { startPlayback(!playing_); });
    auto *left = new QShortcut(QKeySequence(Qt::Key_Left), this);
    connect(left, &QShortcut::activated, this, [this] { stepFrames(-1); });
    auto *right = new QShortcut(QKeySequence(Qt::Key_Right), this);
    connect(right, &QShortcut::activated, this, [this] { stepFrames(1); });
    auto *delKey = new QShortcut(QKeySequence(Qt::Key_Delete), this);
    connect(delKey, &QShortcut::activated, this, [this] { deleteSelectedClip(); });
    auto *backspace = new QShortcut(QKeySequence(Qt::Key_Backspace), this);
    connect(backspace, &QShortcut::activated, this, [this] { deleteSelectedClip(); });

    restoreLayout();
}

MainWindow::~MainWindow() {
    // Join the audio render thread before the members it reads
    // (previewMixer_) unwind - the unique_ptrs destruct in reverse
    // order and the mixer is declared AFTER the preview.
    stopAudioPreview();
    if (decodeThreadB_) {
        decodeThreadB_->quit();
        decodeThreadB_->wait(3000);
    }
    if (decodeThread_) {
        decodeThread_->quit();
        decodeThread_->wait(3000);
    }
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (!confirmSaveChanges()) {
        event->ignore();
        return;
    }
    saveLayout();
    QMainWindow::closeEvent(event);
}

void MainWindow::applyDarkTheme() {
    QPalette pal;
    pal.setColor(QPalette::Window, QColor(kWindowBg));
    pal.setColor(QPalette::WindowText, QColor(kText));
    pal.setColor(QPalette::Base, QColor(kPanelBg));
    pal.setColor(QPalette::AlternateBase, QColor(kWindowBg));
    pal.setColor(QPalette::Text, QColor(kText));
    pal.setColor(QPalette::Button, QColor(kButtonBg));
    pal.setColor(QPalette::ButtonText, QColor(kText));
    pal.setColor(QPalette::ToolTipBase, QColor(kPanelBg));
    pal.setColor(QPalette::ToolTipText, QColor(kText));
    pal.setColor(QPalette::Highlight, QColor(kAccent));
    pal.setColor(QPalette::HighlightedText, QColor(0x101010));
    pal.setColor(QPalette::Disabled, QPalette::Text, QColor(0x777777));
    setPalette(pal);
}

void MainWindow::buildDecodeThread() {
    decodeThread_ = new QThread(this);
    worker_ = new DecodeWorker; // no parent: moves to its own thread
    worker_->moveToThread(decodeThread_);

    connect(decodeThread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &DecodeWorker::mediaInfo, this,
            [this](const QString &summary, double duration, double fps, int64_t frameCount,
                   bool hasAudio) {
                duration_ = duration;
                fps_ = fps > 1.0 ? fps : 24.0;
                model_.setFps(fps_);
                transport_->setMedia(duration_, fps_);
                updateSequenceDuration();
                statusBar()->showMessage(summary, 8000);
                if (!pendingAddClipPath_.isEmpty()) {
                    const int64_t sourceOut = std::min<int64_t>(
                        frameCount > 0 ? frameCount
                                       : static_cast<int64_t>(std::llround(duration_ * fps_)),
                        static_cast<int64_t>(std::llround(5.0 * fps_)));
                    addPendingClip(pendingAddClipPath_, sourceOut, hasAudio);
                    pendingAddClipPath_.clear();
                }
                // a program-source switch was queued during a seek -
                // the new source is now open; re-resolve the timeline
                // position (it maps into the clip and seeks directly).
                if (pendingProgramSeek_) {
                    pendingProgramSeek_ = false;
                    requestFrameAt(playhead_);
                }
            });
    connect(worker_, &DecodeWorker::frameReady, this, [this](const QImage &frame, double pts) {
        // cache the RAW (pre-effects) frame + the clip it belongs to;
        // the program monitor render path (applyProgramFrame) applies
        // that clip's effect stack on the way to the canvas. Effect
        // parameter edits re-render instantly from this cache.
        rawProgramFrame_ = frame;
        rawFramePts_ = pts;
        frameClipId_ = lastProgramClipId_;
        if (!frame.isNull() && frame.width() > 0 && frame.height() > 0) {
            lastProgramSize_ = frame.size(); // text-over-black base geometry
        }
        // The timeline playhead + transport show the TIMELINE position
        // (playhead_), not the source-relative pts of the arriving
        // frame - with in-point offsets the two differ, and the
        // timeline position is what the user scrubbed.
        timeline_->setPlayhead(playhead_);
        transport_->setPosition(playhead_);
        if (captureThumbnail_) {
            // Thumbnails stay raw (pre-effects): they represent the source.
            sourceCanvas_->setFrame(frame, pts);
            projectPanel_->setThumbnail(loadedPath_, frame);
            captureThumbnail_ = false;
        }
        applyProgramFrame();
    });
    connect(worker_, &DecodeWorker::failed, this, [this](const QString &error) {
        statusBar()->showMessage(tr("Error: %1").arg(error), 8000);
    });
    connect(worker_, &DecodeWorker::proxyProgress, this, [this](int percent) {
        statusBar()->showMessage(tr("Generating proxy... %1%").arg(percent));
    });
    connect(worker_, &DecodeWorker::proxyDone, this, [this](bool ok, const QString &errorOrPath) {
        const int index = projectPanel_->library().indexOfPath(proxySourcePath_);
        if (auto *item = projectPanel_->library().at(index)) {
            if (ok) {
                item->proxyPath = errorOrPath;
                statusBar()->showMessage(tr("Proxy ready: %1").arg(errorOrPath), 8000);
            } else {
                statusBar()->showMessage(tr("Proxy failed: %1").arg(errorOrPath), 8000);
            }
            return;
        }
        if (!ok) {
            statusBar()->showMessage(tr("Proxy failed: %1").arg(errorOrPath), 8000);
        }
    });

    decodeThread_->start();

    // ---- the held-frame worker (worker B) ----
    // Serves ONLY the incoming clip's first frame during transition
    // windows. It never drives the program source: its mediaInfo feeds
    // nothing but the queued held-frame request, its failures degrade the
    // composite to A-only.
    decodeThreadB_ = new QThread(this);
    workerB_ = new DecodeWorker; // no parent: moves to its own thread
    workerB_->moveToThread(decodeThreadB_);
    connect(decodeThreadB_, &QThread::finished, workerB_, &QObject::deleteLater);
    connect(workerB_, &DecodeWorker::mediaInfo, this,
            [this](const QString &, double, double, int64_t) {
                if (pendingBFirst_) {
                    pendingBFirst_ = false;
                    const fc::Clip *clip = model_.clipById(pendingBFirstClipId_);
                    if (clip) {
                        QMetaObject::invokeMethod(
                            workerB_, "requestFrame",
                            Q_ARG(double, static_cast<double>(clip->sourceInFrames) / fps_));
                    }
                }
            });
    connect(workerB_, &DecodeWorker::frameReady, this, [this](const QImage &frame, double) {
        bRequestInFlight_ = false;
        if (pendingBFirstClipId_ > 0 && pendingBFirstClipId_ == heldIncomingClipId_) {
            heldIncomingFrame_ = frame;
            applyProgramFrame(); // complete the pair, composite now
        }
    });
    connect(workerB_, &DecodeWorker::failed, this, [this](const QString &error) {
        statusBar()->showMessage(
            tr("Transition preview: could not decode the incoming clip (%1)").arg(error), 8000);
        bRequestInFlight_ = false;
        bFailedClipId_ = heldIncomingClipId_; // do not retry per tick
        heldIncomingFrame_ = QImage();        // composite falls back to A
    });
    decodeThreadB_->start();
}

void MainWindow::buildProWorkspace() {
    // Left zone: Project | Effects | Transitions | Color | Text (tabbed).
    projectPanel_ = new ProjectPanel(this);
    effectsPanel_ = new EffectsPanel(this);
    transitionsPanel_ = new TransitionsPanel(this);
    colorPanel_ = new ColorPanel(this);
    textPanel_ = new TextPanel(this);
    auto *leftTabs = new QTabWidget(this);
    leftTabs->addTab(projectPanel_, tr("Project"));
    leftTabs->addTab(effectsPanel_, tr("Effects"));
    leftTabs->addTab(transitionsPanel_, tr("Transitions"));
    leftTabs->addTab(colorPanel_, tr("Color"));
    leftTabs->addTab(textPanel_, tr("Text"));
    auto *leftDock = makeDock(tr("Project"), leftTabs, this);
    addDockWidget(Qt::LeftDockWidgetArea, leftDock);

    // Bottom zone: Timeline | Audio Mixer (tabbed).
    timeline_ = new TimelinePanel(this);
    audioPreview_ = std::make_unique<fc::AudioPreview>();
    mixer_ = new MixerPanel(this);
    auto *bottomTabs = new QTabWidget(this);
    bottomTabs->addTab(timeline_, tr("Timeline"));
    bottomTabs->addTab(mixer_, tr("Audio Mixer"));
    mixer_->refreshFromModel(&model_);
    connect(mixer_, &MixerPanel::mixerChanged, this, [this] {
        markDirty();
        rebuildAudioSnapshot(); // fader/pan/mute/solo land in the live mix
    });
    auto *bottomDock = makeDock(tr("Timeline"), bottomTabs, this);
    addDockWidget(Qt::BottomDockWidgetArea, bottomDock);

    // Right zone: Effect Controls.
    effectControls_ = new EffectControlsPanel(this);
    auto *rightDock = makeDock(tr("Effect Controls"), effectControls_, this);
    addDockWidget(Qt::RightDockWidgetArea, rightDock);

    // Center: Source (left) | Program (right) monitors.
    auto *monitorSplit = new QSplitter(Qt::Horizontal, this);

    auto *sourceGroup = new QWidget(monitorSplit);
    auto *sourceLayout = new QVBoxLayout(sourceGroup);
    sourceLayout->setContentsMargins(0, 0, 0, 0);
    sourceLayout->setSpacing(2);
    auto *sourceTitle = new QLabel(tr("Source"), sourceGroup);
    sourceTitle->setAlignment(Qt::AlignCenter);
    sourceCanvas_ = new PreviewCanvas(sourceGroup);
    sourceLayout->addWidget(sourceTitle);
    sourceLayout->addWidget(sourceCanvas_, 1);

    auto *programGroup = new QWidget(monitorSplit);
    auto *programLayout = new QVBoxLayout(programGroup);
    programLayout->setContentsMargins(0, 0, 0, 0);
    programLayout->setSpacing(2);
    auto *programTitle = new QLabel(tr("Program"), programGroup);
    programTitle->setAlignment(Qt::AlignCenter);
    programCanvas_ = new PreviewCanvas(programGroup);
    transport_ = new TransportBar(programGroup);
    programLayout->addWidget(programTitle);
    programLayout->addWidget(programCanvas_, 1);
    programLayout->addWidget(transport_);

    monitorSplit->addWidget(sourceGroup);
    monitorSplit->addWidget(programGroup);
    monitorSplit->setStretchFactor(0, 1);
    monitorSplit->setStretchFactor(1, 2);

    pages_ = new QStackedWidget(this);
    pages_->addWidget(monitorSplit); // page 0: Pro Mode
    setCentralWidget(pages_);

    // Panel-to-engine wiring.
    connect(projectPanel_, &ProjectPanel::importRequested, this, [this] { importMedia(); });
    connect(projectPanel_, &ProjectPanel::loadRequested, this, [this](const QString &path) {
        pendingAddClipPath_ = path;
        loadClip(path);
    });
    connect(projectPanel_, &ProjectPanel::proxyRequested, this,
            [this](const QString &path) { generateProxy(path); });
    connect(transport_, &TransportBar::playToggled, this,
            [this](bool playing) { startPlayback(playing); });
    connect(transport_, &TransportBar::seekRequested, this,
            [this](double seconds) { requestFrameAt(seconds); });
    connect(transport_, &TransportBar::stepRequested, this,
            [this](int frames) { stepFrames(frames); });
    connect(timeline_, &TimelinePanel::playheadMoved, this, [this](double seconds) {
        startPlayback(false);
        requestFrameAt(seconds);
    });
    connect(timeline_, &TimelinePanel::clipSelected, this, [this](int64_t id) {
        selectedClipId_ = id;
        // the Effect Controls + Color panels edit the SELECTED clip.
        // An AUDIO-track clip gets the audio-fade editor instead of
        // the (video) effect stack.
        const fc::Clip *clip = id > 0 ? model_.clipById(id) : nullptr;
        const fc::Track *track = clip ? model_.trackAt(clip->trackIndex) : nullptr;
        const std::vector<fc::EffectInstance> stack =
            clip ? clip->effectStack : std::vector<fc::EffectInstance>();
        if (track && track->isAudio) {
            effectControls_->setAudioFades(id, clip ? clip->fadeInFrames : 0,
                                           clip ? clip->fadeOutFrames : 0,
                                           clip ? clip->durationFrames() : 1);
            colorPanel_->setClip(id, std::vector<fc::EffectInstance>());
        } else {
            effectControls_->setStack(id, stack);
            colorPanel_->setClip(id, stack);
        }
        // the Text panel edits the selected clip's document (null for
        // non-text clips clears it).
        textPanel_->setClip(id, clip && clip->isText ? &clip->text : nullptr);
        updateKeyframePanels();
    });
    connect(effectControls_, &EffectControlsPanel::audioFadesChanged, this,
            [this](int64_t clipId, int64_t fadeIn, int64_t fadeOut) {
                if (!model_.setClipAudioFades(clipId, fadeIn, fadeOut)) {
                    return;
                }
                markDirty();
                rebuildAudioSnapshot(); // the mix follows the fades live
                timeline_->update();
            });

    // ---- effects wiring ----
    connect(effectsPanel_, &EffectsPanel::effectAddRequested, this,
            [this](const QString &effectId) { addEffectToSelectedClip(effectId); });
    // Both stack editors (Effect Controls + Color) emit the FULL new
    // stack; the model write is shared. Only the OTHER panel re-syncs
    // (rebuilding the emitting panel mid-drag would destroy its slider
    // under the cursor); the emitter keeps its working copy, which the
    // model just absorbed.
    auto writeStack = [this](int64_t clipId, const std::vector<fc::EffectInstance> &stack) {
        fc::Clip *clip = model_.clipById(clipId);
        if (!clip) {
            return;
        }
        clip->effectStack = stack;
        markDirty();
        timeline_->update(); // fx badge visibility
        if (frameClipId_ == clipId) {
            applyProgramFrame(); // instant re-render from the raw cache
        }
    };
    connect(effectControls_, &EffectControlsPanel::stackChanged, this,
            [this, writeStack](int64_t clipId, const std::vector<fc::EffectInstance> &stack) {
                writeStack(clipId, stack);
                if (const fc::Clip *clip = model_.clipById(clipId)) {
                    colorPanel_->setClip(clipId, clip->effectStack); // cheap refresh
                }
            });
    connect(colorPanel_, &ColorPanel::stackChanged, this,
            [this, writeStack](int64_t clipId, const std::vector<fc::EffectInstance> &stack) {
                writeStack(clipId, stack);
                if (const fc::Clip *clip = model_.clipById(clipId)) {
                    effectControls_->setStack(clipId, clip->effectStack);
                }
            });

    // ---- text wiring ----
    connect(textPanel_, &TextPanel::addTextClipRequested, this, [this] { addTextClip(); });
    connect(
        textPanel_, &TextPanel::textEdited, this,
        [this](int64_t clipId, const fc::TextDocument &doc) { writeTextDocument(clipId, doc); });
    connect(textPanel_, &TextPanel::emojiFontPicked, this,
            [this](const QString &path) { setEmojiFont(path); });
    startupEmojiFont();

    // ---- transition wiring ----
    connect(transitionsPanel_, &TransitionsPanel::transitionAddRequested, this,
            [this](const QString &kind) { addTransitionToSelectedClip(kind); });
    connect(timeline_, &TimelinePanel::transitionSelected, this, [this](int64_t id) {
        selectedTransitionId_ = id;
        const fc::Transition *t = id > 0 ? model_.transitionById(id) : nullptr;
        if (t) {
            pushTransitionToEditor(*t);
        } else {
            effectControls_->setTransition(-1, QString(), 0, 1, QString(), fps_);
        }
    });
    connect(effectControls_, &EffectControlsPanel::transitionDurationChanged, this,
            [this](int64_t id, int64_t frames) {
                if (!model_.setTransitionDuration(id, frames)) {
                    statusBar()->showMessage(
                        tr("Duration rejected - it does not fit the outgoing clip."), 4000);
                }
                markDirty(); // transition edits count
                if (const fc::Transition *t = model_.transitionById(id)) {
                    pushTransitionToEditor(*t); // refresh slider bounds + value
                }
                timeline_->update(); // marker width follows the duration
                applyProgramFrame(); // live re-render inside the window
            });
    connect(effectControls_, &EffectControlsPanel::transitionRemoveRequested, this,
            [this](int64_t id) {
                if (!model_.removeTransition(id)) {
                    return;
                }
                selectedTransitionId_ = -1;
                timeline_->clearTransitionSelection();
                effectControls_->setTransition(-1, QString(), 0, 1, QString(), fps_);
                timeline_->update();
                applyProgramFrame();
                markDirty();
                statusBar()->showMessage(tr("Transition removed."), 4000);
            });
    connect(timeline_, &TimelinePanel::splitRequested, this, [this](int trackIndex, int64_t frame) {
        if (const fc::Track *track = model_.trackAt(trackIndex); track && track->locked) {
            statusBar()->showMessage(tr("Track %1 is locked - unlock it (header L) to split.")
                                         .arg(QString::fromStdString(track->name)));
            return;
        }
        if (model_.splitAt(frame, trackIndex)) {
            updateSequenceDuration();
        }
    });
    // ---- timeline editing wiring ----
    connect(timeline_, &TimelinePanel::clipMoveRequested, this,
            [this](int64_t clipId, int trackIndex, int64_t startFrame) {
                moveClipTo(clipId, trackIndex, startFrame);
            });
    connect(timeline_, &TimelinePanel::clipTrimRequested, this,
            [this](int64_t clipId, int edge, int64_t deltaFrames) {
                trimClip(clipId, edge, deltaFrames);
            });
    connect(timeline_, &TimelinePanel::rollEditRequested, this,
            [this](int64_t leftId, int64_t rightId, int64_t deltaFrames) {
                const fc::Clip *left = model_.clipById(leftId);
                if (!left) {
                    return;
                }
                if (const fc::Track *track = model_.trackAt(left->trackIndex);
                    track && track->locked) {
                    statusBar()->showMessage(tr("Track %1 is locked - rolling edit rejected.")
                                                 .arg(QString::fromStdString(track->name)));
                    return;
                }
                if (model_.rollEdit(leftId, rightId, deltaFrames)) {
                    updateSequenceDuration();
                } else {
                    statusBar()->showMessage(tr("Rolling edit rejected (boundary limit reached)."));
                }
            });
    connect(timeline_, &TimelinePanel::trackStateToggleRequested, this,
            [this](int trackIndex, int which) { toggleTrackState(trackIndex, which); });
}

void MainWindow::buildQuickWorkspace() {
    quickView_ = new QuickModeView(this);
    pages_->addWidget(quickView_); // page 1: Quick Mode

    connect(quickView_, &QuickModeView::playToggled, this,
            [this](bool playing) { startPlayback(playing); });
}

void MainWindow::buildMenus() {
    // ---- File ----
    QMenu *file = menuBar()->addMenu(tr("&File"));
    QAction *openProjectAction =
        addMenuAction(file, tr("&Open Project..."), QKeySequence(tr("Ctrl+O")));
    connect(openProjectAction, &QAction::triggered, this, [this] { openProject(); });
    QAction *saveProjectAction = addMenuAction(file, tr("&Save Project"), QKeySequence::Save);
    connect(saveProjectAction, &QAction::triggered, this, [this] { saveProject(); });
    QAction *saveAsAction =
        addMenuAction(file, tr("Save Project &As..."), QKeySequence(tr("Ctrl+Shift+S")));
    connect(saveAsAction, &QAction::triggered, this, [this] { saveProjectAs(); });
    file->addSeparator();
    QAction *importAction = addMenuAction(file, tr("&Import Media..."), QKeySequence(tr("Ctrl+I")));
    connect(importAction, &QAction::triggered, this, [this] { importMedia(); });
    QAction *exportAction = addMenuAction(file, tr("&Export Media..."), QKeySequence(tr("Ctrl+M")));
    exportAction->setToolTip(
        tr("Renders the timeline (effects and transitions included) to an MP4"));
    connect(exportAction, &QAction::triggered, this, [this] { exportMedia(); });
    file->addSeparator();
    QAction *importCaptionsAction = addMenuAction(file, tr("Import &Subtitles..."));
    importCaptionsAction->setToolTip(
        tr("Turns every cue of a .srt file into a text clip on a text track"));
    connect(importCaptionsAction, &QAction::triggered, this, [this] { importCaptions(); });
    QAction *exportCaptionsAction = addMenuAction(file, tr("Expo&rt Subtitles..."));
    exportCaptionsAction->setToolTip(
        tr("Writes the timeline's text clips to a .srt file (styles are flattened to text)"));
    connect(exportCaptionsAction, &QAction::triggered, this, [this] { exportCaptions(); });
    file->addSeparator();
    QAction *quitAction = addMenuAction(file, tr("E&xit"), QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);

    // ---- Edit ----
    QMenu *edit = menuBar()->addMenu(tr("&Edit"));
    addMenuAction(edit, tr("&Undo"), QKeySequence::Undo)->setEnabled(false);
    addMenuAction(edit, tr("&Redo"), QKeySequence::Redo)->setEnabled(false);
    edit->addSeparator();
    addMenuAction(edit, tr("&Keyboard Shortcuts..."))->setEnabled(false);

    // ---- Clip ----
    QMenu *clip = menuBar()->addMenu(tr("&Clip"));
    QAction *splitMenuAction = addMenuAction(clip, tr("Split at Playhead"), QKeySequence(tr("C")));
    splitMenuAction->setToolTip(
        tr("Splits the selected clip (or the clip under the playhead on V1) at "
           "the current playhead position"));
    connect(splitMenuAction, &QAction::triggered, this, [this] { splitAtPlayhead(); });
    addMenuAction(clip, tr("&Speed / Duration..."), QKeySequence(tr("Ctrl+R")))->setEnabled(false);
    addMenuAction(clip, tr("&Reverse Clip"))->setEnabled(false);
    clip->addSeparator();
    QAction *proxyMenuAction =
        addMenuAction(clip, tr("&Generate 360p Proxy"), QKeySequence(tr("Ctrl+P")));
    connect(proxyMenuAction, &QAction::triggered, this, [this] {
        if (!loadedPath_.isEmpty()) {
            generateProxy(loadedPath_);
        }
    });

    // ---- Title ----
    QMenu *title = menuBar()->addMenu(tr("&Title"));
    QAction *addTextAction = addMenuAction(title, tr("&Add Text Clip"), QKeySequence(tr("Ctrl+T")));
    addTextAction->setToolTip(tr("Creates a text clip at the playhead on a text track and opens it "
                                 "in the Text panel"));
    connect(addTextAction, &QAction::triggered, this, [this] { addTextClip(); });

    // ---- Sequence ----
    QMenu *sequence = menuBar()->addMenu(tr("Se&quence"));
    addMenuAction(sequence, tr("&Render In to Out"), QKeySequence(tr("Enter")))->setEnabled(false);

    // ---- Effects ----
    QMenu *effects = menuBar()->addMenu(tr("&Effects"));
    QAction *defaultTransition =
        addMenuAction(effects, tr("Apply &Default Transition"), QKeySequence(tr("Ctrl+D")));
    defaultTransition->setToolTip(
        tr("Adds a 1 s Cross Dissolve on the cut after the selected clip"));
    connect(defaultTransition, &QAction::triggered, this,
            [this] { addTransitionToSelectedClip(QStringLiteral("dissolve.cross")); });

    // ---- View ----
    QMenu *view = menuBar()->addMenu(tr("&View"));
    QAction *safeAction = addMenuAction(view, tr("Toggle &Safe Margins"));
    safeAction->setCheckable(true);
    safeAction->setEnabled(false);

    // ---- Window (dual-mode workspace switcher + panel toggles) ----
    QMenu *window = menuBar()->addMenu(tr("&Window"));
    QActionGroup *modes = new QActionGroup(this);
    QAction *proMode = modes->addAction(tr("&Pro Mode"));
    QAction *quickMode = modes->addAction(tr("&Quick Mode"));
    proMode->setCheckable(true);
    quickMode->setCheckable(true);
    proMode->setChecked(true);
    connect(proMode, &QAction::triggered, this, [this] { setMode(true); });
    connect(quickMode, &QAction::triggered, this, [this] { setMode(false); });
    window->addAction(proMode);
    window->addAction(quickMode);
    window->addSeparator();
    for (QDockWidget *dock : findChildren<QDockWidget *>()) {
        window->addAction(dock->toggleViewAction());
    }

    // ---- Help ----
    QMenu *help = menuBar()->addMenu(tr("&Help"));
    QAction *aboutAction = help->addAction(tr("&About FusionCut Pro"));
    connect(aboutAction, &QAction::triggered, this, [this] {
        QMessageBox::about(this, tr("About FusionCut Pro"),
                           tr("<b>FusionCut Pro %1</b><br/>Dual-mode editor shell."
                              "<br/><br/>Engine: %2"
                              "<br/>License: GPL-3.0-or-later"
                              "<br/><a href=\"https://github.com/marbou92/FusionCut-Pro\">"
                              "github.com/marbou92/FusionCut-Pro</a>")
                               .arg(FC_VERSION_STRING)
                               .arg(QString::fromStdString(ffmpegVersionInfo())));
    });
}

void MainWindow::buildStatusBar() {
    statusBar()->showMessage(tr("Pro Mode - import media to begin (Ctrl+I)"));
    auto *budget = new QLabel(tr("1 GB RAM target - Qt 5.15 - FFmpeg - Windows 7+"), this);
    statusBar()->addPermanentWidget(budget);
}

void MainWindow::setMode(bool pro) {
    pages_->setCurrentIndex(pro ? 0 : 1);
    for (QDockWidget *dock : findChildren<QDockWidget *>()) {
        dock->setVisible(pro);
    }
    statusBar()->showMessage(pro ? tr("Workspace: Pro Mode (Premiere-style panels)")
                                 : tr("Workspace: Quick Mode (simplified editing)"));
}

void MainWindow::importMedia() {
    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Import Media"), QString(),
        tr("Video/Audio (*.mp4 *.mov *.mkv *.avi *.webm *.m4v *.mts *.m2ts *.mpg "
           "*.mpeg *.wmv *.flv *.3gp *.ts *.wav *.mp3 *.aac *.flac *.ogg);;"
           "All Files (*)"));
    bool first = true;
    for (const QString &file : files) {
        MediaItem item;
        item.path = file;
        item.displayName = QFileInfo(file).completeBaseName();
        projectPanel_->addMedia(item);

        // Probe once up front: a file with audio but NO video becomes
        // an audio clip directly (the video decode path cannot open
        // it); a file with video follows the existing worker flow,
        // which places its video clip AND - when the probe found a
        // sound track - a matching audio clip on the audio lane.
        fc::MediaInfo info;
        std::string probeError;
        const bool probed = fc::MediaProbe::probe(file.toStdString(), info, probeError);
        const bool hasAudio = probed && !info.audioStreams.empty();
        const bool hasVideo = probed && info.hasVideo;
        if (hasAudio && !hasVideo) {
            startPlayback(false);
            const int audioTrack = ensureAudioTrack();
            const int64_t frames = std::max<int64_t>(
                1, static_cast<int64_t>(std::llround(info.durationSeconds() * fps_)));
            const int64_t start = static_cast<int64_t>(std::llround(playhead_ * fps_));
            const QString label = QFileInfo(file).completeBaseName();
            if (model_.addClip(audioTrack, file.toStdString(), label.toStdString(), 0, frames,
                               start) > 0) {
                statusBar()->showMessage(
                    tr("Imported audio-only file: %1 (clip on the audio track)")
                        .arg(item.displayName),
                    8000);
            }
            updateSequenceDuration();
            timeline_->setModel(&model_);
            timeline_->update();
            markDirty();
            rebuildAudioSnapshot();
            continue;
        }
        if (first) {
            loadClip(file);
            first = false;
        }
    }
}

void MainWindow::loadClip(const QString &sourcePath) {
    startPlayback(false);
    playhead_ = 0.0;
    loadedPath_ = sourcePath;
    captureThumbnail_ = true;
    lastProgramClipId_ = -1;

    const int index = projectPanel_->library().indexOfPath(sourcePath);
    const QString path = (index >= 0 && projectPanel_->library().at(index)->hasProxy())
                             ? projectPanel_->library().at(index)->proxyPath
                             : sourcePath;
    QMetaObject::invokeMethod(worker_, "open", Q_ARG(QString, path));
}

void MainWindow::addPendingClip(const QString &sourcePath, int64_t sourceOutFrames,
                                bool withAudio) {
    // place on V1 (track index 1) at the playhead; first 5s or full.
    const int64_t start = static_cast<int64_t>(std::llround(playhead_ * fps_));
    const int64_t out = std::max<int64_t>(1, sourceOutFrames);
    const QString label = QFileInfo(sourcePath).completeBaseName();
    model_.addClip(1, sourcePath.toStdString(), label.toStdString(), 0, out, start);
    if (withAudio) {
        // The sound track rides along: an audio clip with the SAME
        // source range and placement on the audio lane under the video
        // clip (moves/trims independently - the audio fades editor
        // lives in Effect Controls while it is selected).
        const int audioTrack = ensureAudioTrack();
        model_.addClip(audioTrack, sourcePath.toStdString(), label.toStdString(), 0, out, start);
    }
    updateSequenceDuration();
    rebuildAudioSnapshot();
}

void MainWindow::splitAtPlayhead() {
    const int64_t frame = static_cast<int64_t>(std::llround(playhead_ * fps_));
    int trackIndex = 1; // default to V1
    // Prefer the track of the currently selected clip.
    if (selectedClipId_ > 0) {
        if (const fc::Clip *clip = model_.clipById(selectedClipId_)) {
            trackIndex = clip->trackIndex;
        }
    }
    if (const fc::Track *track = model_.trackAt(trackIndex); track && track->locked) {
        statusBar()->showMessage(tr("Track %1 is locked - unlock it (header L) to split.")
                                     .arg(QString::fromStdString(track->name)));
        return;
    }
    if (model_.splitAt(frame, trackIndex)) {
        updateSequenceDuration();
    }
}

void MainWindow::deleteSelectedClip() {
    if (selectedClipId_ <= 0) {
        return;
    }
    if (const fc::Clip *clip = model_.clipById(selectedClipId_)) {
        if (const fc::Track *track = model_.trackAt(clip->trackIndex); track && track->locked) {
            statusBar()->showMessage(tr("Track %1 is locked - delete rejected.")
                                         .arg(QString::fromStdString(track->name)));
            return;
        }
    }
    // with the Ripple toggle on, the gap closes; otherwise classic.
    if (!(timeline_ && timeline_->isRippleEnabled() && model_.rippleDelete(selectedClipId_))) {
        model_.removeClip(selectedClipId_);
    }
    textLayerCache_.erase(selectedClipId_); // stale layers never resurrect
    selectedClipId_ = -1;
    lastProgramClipId_ = -1;
    timeline_->clearSelection();
    effectControls_->setStack(-1, {}); // the deleted clip's editor clears
    textPanel_->setClip(-1, nullptr);  // the text editor clears too
    updateSequenceDuration();
}

void MainWindow::moveClipTo(int64_t clipId, int trackIndex, int64_t startFrame) {
    const fc::Clip *clip = model_.clipById(clipId);
    if (!clip) {
        return;
    }
    if (const fc::Track *target = model_.trackAt(trackIndex); target && target->locked) {
        statusBar()->showMessage(
            tr("Track %1 is locked - move rejected.").arg(QString::fromStdString(target->name)));
        return;
    }
    if (const fc::Track *origin = model_.trackAt(clip->trackIndex); origin && origin->locked) {
        statusBar()->showMessage(tr("Track %1 is locked - unlock it (header L) to move clips.")
                                     .arg(QString::fromStdString(origin->name)));
        return;
    }
    if (model_.moveClipTo(clipId, trackIndex, startFrame)) {
        updateSequenceDuration();
    } else {
        statusBar()->showMessage(tr("Move rejected - the drop would overlap another clip."));
    }
}

void MainWindow::trimClip(int64_t clipId, int edge, int64_t deltaFrames) {
    const fc::Clip *clip = model_.clipById(clipId);
    if (!clip || deltaFrames == 0) {
        return;
    }
    if (const fc::Track *track = model_.trackAt(clip->trackIndex); track && track->locked) {
        statusBar()->showMessage(
            tr("Track %1 is locked - trim rejected.").arg(QString::fromStdString(track->name)));
        return;
    }
    bool ok = false;
    if (edge == 1 && timeline_ && timeline_->isRippleEnabled()) {
        ok = model_.rippleTrimClipEnd(clipId, deltaFrames);
    } else if (edge == 0) {
        ok = model_.trimClipStart(clipId, deltaFrames);
    } else {
        ok = model_.trimClipEnd(clipId, deltaFrames);
    }
    if (ok) {
        updateSequenceDuration();
    } else {
        statusBar()->showMessage(tr("Trim rejected - the clip cannot shrink/extend further."));
    }
}

void MainWindow::toggleTrackState(int row, int which) {
    fc::Track *track = model_.trackAt(row);
    if (!track) {
        return;
    }
    bool locked = track->locked;
    bool muted = track->muted;
    bool solo = track->solo;
    switch (which) {
    case 0:
        locked = !locked;
        break;
    case 1:
        muted = !muted;
        break;
    case 2:
        solo = !solo;
        break;
    default:
        return;
    }
    model_.setTrackState(row, locked, muted, solo);
    timeline_->update();
    QString state;
    switch (which) {
    case 0:
        state = locked ? tr("locked (edits blocked)") : tr("unlocked");
        break;
    case 1:
        state = muted ? tr("muted") : tr("unmuted");
        break;
    default:
        state = solo ? tr("solo") : tr("solo off");
        break;
    }
    statusBar()->showMessage(tr("Track %1: %2").arg(QString::fromStdString(track->name), state));
}

void MainWindow::updateSequenceDuration() {
    timeline_->setFps(fps_);
    const double seq = model_.durationSeconds();
    const double dur = seq > 0.0 ? seq : (duration_ > 0.0 ? duration_ : 10.0);
    timeline_->setSequenceDuration(dur);
    // The transport drives the PROGRAM (the timeline sequence); once
    // clips exist the sequence extent replaces the media duration.
    if (seq > 0.0) {
        transport_->setMedia(dur, fps_);
    }
    // every MainWindow model mutation funnels through here;
    // keep the transition editor in sync with pruned/clamped transitions.
    syncTransitionEditor();
    // same funnel marks the project dirty.
    markDirty();
}

void MainWindow::generateProxy(const QString &sourcePath) {
    proxySourcePath_ = sourcePath;
    statusBar()->showMessage(tr("Proxy job queued: %1").arg(QFileInfo(sourcePath).fileName()));
    QMetaObject::invokeMethod(worker_, "runProxyJob", Q_ARG(QString, sourcePath),
                              Q_ARG(QString, proxyPathFor(sourcePath)));
}

void MainWindow::startPlayback(bool playing) {
    if (playing_ == playing) {
        transport_->setPlaying(playing);
        quickView_->setPlaying(playing);
        return;
    }
    playing_ = playing;
    transport_->setPlaying(playing);
    quickView_->setPlaying(playing);
    if (playing) {
        if (duration_ > 0.0 && playhead_ >= duration_ - 1e-9) {
            playhead_ = 0.0;
            requestFrameAt(0.0);
        }
        playClock_->start(static_cast<int>(1000.0 / fps_));
        startAudioPreview();
    } else {
        playClock_->stop();
        stopAudioPreview();
    }
}

void MainWindow::stepFrames(int frames) {
    startPlayback(false);
    playhead_ += frames / fps_;
    if (playhead_ < 0.0) {
        playhead_ = 0.0;
    }
    if (duration_ > 0.0 && playhead_ > duration_) {
        playhead_ = duration_;
    }
    requestFrameAt(playhead_);
}

void MainWindow::requestFrameAt(double seconds) {
    playhead_ = seconds;
    // A seek/scrub while playing moves the playhead out from under
    // the running audio stream - re-anchor the preview at the new
    // position (the audio thread is the clock, so it cannot follow on
    // its own).
    if (playing_ && audioPreview_ && audioPreview_->running()) {
        const double audioPos = audioStartSeconds_ + audioPreview_->playedSeconds();
        if (std::abs(audioPos - playhead_) > 0.30) {
            startAudioPreview();
        }
    }
    // keyframed parameters display at the playhead's position
    // inside the SELECTED clip.
    updateKeyframePanels();
    // composite program monitor: resolve the topmost video clip
    // under the timeline playhead and map the position into that
    // clip's source (timeline frame - clip start + source in-point).
    // One decoder serves the whole timeline: moving the playhead into
    // a clip from a different source lazily switches the decode source
    // (the switch is debounced per clip, not per frame).
    const int64_t frame = static_cast<int64_t>(std::llround(playhead_ * fps_));
    const fc::Clip *clip = model_.activeVideoClipAt(frame);
    const int64_t clipId = clip ? clip->id : -1;
    const bool clipChanged = clipId != lastProgramClipId_;
    lastProgramClipId_ = clipId;
    if (!clip) {
        // text clips covering the playhead with NO video clip under
        // them render over BLACK - generated frames, no decode, no
        // stale video bleeding through the cache.
        if (!model_.textClipsAt(frame).empty()) {
            rawProgramFrame_ = QImage();
            frameClipId_ = -1;
            applyProgramFrame();
            return;
        }
        // Empty timeline region (or empty timeline): plain source-time
        // behavior ( single-media semantics).
        QMetaObject::invokeMethod(worker_, "requestFrame", Q_ARG(double, seconds));
        return;
    }

    // the cut under the playhead may carry a transition -
    // make sure the HELD first frame of the incoming clip is on hand
    // (fetched once per incoming clip on the dedicated worker B). The
    // outgoing clip keeps streaming through the regular path below.
    if (clip->trackIndex >= 0) {
        fc::TransitionSample sample;
        if (model_.resolveTransitionAt(frame, clip->trackIndex, sample)) {
            ensureHeldIncomingFrame(sample);
        }
    }
    const double srcSeconds =
        static_cast<double>(frame - clip->timelineStart + clip->sourceInFrames) / fps_;
    const QString clipSource = QString::fromStdString(clip->sourcePath);
    if (clipSource != loadedPath_) {
        // Source switch: open the clip's media (proxy when available);
        // the queued seek re-resolves through mediaInfo once the open
        // completes (pendingProgramSeek_), then maps directly.
        startPlayback(false);
        pendingProgramSeek_ = true;
        loadedPath_ = clipSource;
        captureThumbnail_ = true;
        const int index = projectPanel_->library().indexOfPath(clipSource);
        const QString path = (index >= 0 && projectPanel_->library().at(index)->hasProxy())
                                 ? projectPanel_->library().at(index)->proxyPath
                                 : clipSource;
        QMetaObject::invokeMethod(worker_, "open", Q_ARG(QString, path));
        return;
    }
    if (clipChanged) {
        statusBar()->showMessage(tr("Program: %1").arg(QString::fromStdString(clip->label)));
    }
    QMetaObject::invokeMethod(worker_, "requestFrame", Q_ARG(double, srcSeconds));
}

void MainWindow::applyProgramFrame() {
    const int64_t frame = static_cast<int64_t>(std::llround(playhead_ * fps_));
    // the text stack composites on top of the video frame; when no
    // video clip is active but text clips cover the playhead, the base
    // is a black frame at the last program geometry.
    const std::vector<const fc::Clip *> texts = model_.textClipsAt(frame);
    if (rawProgramFrame_.isNull() && texts.empty()) {
        return;
    }

    QImage out;
    if (rawProgramFrame_.isNull()) {
        out = QImage(lastProgramSize_, QImage::Format_RGBA8888);
        out.fill(Qt::black);
    } else {
        out = rawProgramFrame_; // shallow copy; bits() detaches below
        if (out.format() != QImage::Format_RGBA8888) {
            out = out.convertToFormat(QImage::Format_RGBA8888);
        }
    }
    const fc::Clip *clip = model_.clipById(frameClipId_);
    if (clip && !clip->effectStack.empty() && !rawProgramFrame_.isNull()) {
        // the frame's CLIP-RELATIVE position resolves
        // keyframed parameters (static values when the playhead sits
        // outside the clip).
        int64_t clipFrame = fc::kNoKeyframeTime;
        if (frame >= clip->timelineStart && frame < clip->timelineEnd()) {
            clipFrame = frame - clip->timelineStart;
        }
        fc::applyEffectStack(out.bits(), out.width(), out.height(), clip->effectStack, clipFrame);
    }

    // when the playhead sits inside a transition window AND
    // the cached raw frame belongs to the outgoing (left) clip AND the
    // held (incoming) frame is available, composite the cut live. The
    // held frame runs the incoming clip's effect stack first (standard
    // order: per-clip effects, then the transition blend), and is scaled
    // to the outgoing frame's geometry when the sources differ.
    if (clip) {
        fc::TransitionSample sample;
        if (model_.resolveTransitionAt(frame, clip->trackIndex, sample) &&
            sample.leftClipId == frameClipId_ && heldIncomingClipId_ == sample.rightClipId &&
            !heldIncomingFrame_.isNull()) {
            const fc::Clip *right = model_.clipById(sample.rightClipId);
            QImage held = heldIncomingFrame_; // shallow; detaches on convert/scale
            if (held.format() != QImage::Format_RGBA8888) {
                held = held.convertToFormat(QImage::Format_RGBA8888);
            }
            if (right && !right->effectStack.empty()) {
                // The HELD frame is the incoming clip's FIRST frame: its
                // clip-relative position is 0.
                fc::applyEffectStack(held.bits(), held.width(), held.height(), right->effectStack,
                                     0);
            }
            if (held.size() != out.size()) {
                held = held.scaled(out.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation);
            }
            if (held.size() == out.size() && out.width() > 0 && out.height() > 0) {
                QImage blended(out.width(), out.height(), QImage::Format_RGBA8888);
                fc::applyTransition(out.bits(), held.bits(), blended.bits(), out.width(),
                                    out.height(), sample.kind, sample.progress);
                out = blended;
            }
        }
    }

    // Composite every text clip covering the playhead ON TOP (paint
    // order = bottom text lane first). A static clip's layer is cached
    // per clip; an ANIMATED clip re-renders at its clip-relative frame
    // (the same evaluator the export job runs, so preview and export
    // stay identical). The text clip's own effect stack runs on a COPY
    // so keyframed params resolve per frame without touching the cache.
    for (const fc::Clip *textClip : texts) {
        if (!textClip || !textClip->isText) {
            continue;
        }
        int64_t textFrame = textClip->timelineStart; // always >= 0 below
        if (frame >= textClip->timelineStart && frame < textClip->timelineEnd()) {
            textFrame = frame - textClip->timelineStart;
        } else {
            textFrame = 0; // outside (cannot happen via textClipsAt) - clamp
        }
        QImage layer = textLayerForClip(textClip, out.width(), out.height(), textFrame);
        if (layer.isNull() || layer.format() != QImage::Format_RGBA8888 ||
            layer.size() != out.size()) {
            continue;
        }
        if (!textClip->effectStack.empty()) {
            int64_t textFrame = fc::kNoKeyframeTime;
            if (frame >= textClip->timelineStart && frame < textClip->timelineEnd()) {
                textFrame = frame - textClip->timelineStart;
            }
            QImage effected = layer.copy();
            fc::applyEffectStack(effected.bits(), effected.width(), effected.height(),
                                 textClip->effectStack, textFrame);
            layer = effected;
        }
        if (out.width() > 0 && out.height() > 0 &&
            out.bytesPerLine() == static_cast<int>(out.width()) * 4 &&
            layer.bytesPerLine() == static_cast<int>(layer.width()) * 4) {
            fc::compositeOver(out.bits(), layer.constBits(), out.width(), out.height());
        }
    }

    programCanvas_->setFrame(out, rawFramePts_);
    quickView_->canvas()->setFrame(out, rawFramePts_);
}

void MainWindow::addEffectToSelectedClip(const QString &effectId) {
    fc::Clip *clip = selectedClipId_ > 0 ? model_.clipById(selectedClipId_) : nullptr;
    if (!clip) {
        statusBar()->showMessage(
            tr("Select a timeline clip first (click a clip, then add the effect)."), 6000);
        return;
    }
    clip->effectStack.push_back(fc::makeEffectInstance(effectId.toStdString()));
    markDirty();
    const fc::EffectDescriptor *d = fc::findEffect(effectId.toStdString());
    statusBar()->showMessage(tr("Added %1 to %2 (edit it in Effect Controls)")
                                 .arg(d ? QString::fromStdString(d->label) : effectId,
                                      QString::fromStdString(clip->label)),
                             6000);
    effectControls_->setStack(selectedClipId_, clip->effectStack);
    colorPanel_->setClip(selectedClipId_, clip->effectStack);
    timeline_->update(); // fx badge
    if (frameClipId_ == selectedClipId_) {
        applyProgramFrame();
    }
}

// ---------------------------------------------------------------------------
// Text engine: clip creation, panel sync, layer rendering, captions.
// ---------------------------------------------------------------------------

// Finds the topmost text lane, or inserts one above everything (T1).
// Returns the track index (a text lane by construction).
int MainWindow::ensureTextTrack() {
    int textTrack = -1;
    int textTrackCount = 0;
    for (const fc::Track &track : model_.tracks()) {
        if (track.isText) {
            if (textTrack < 0) {
                textTrack = track.index;
            }
            ++textTrackCount;
        }
    }
    if (textTrack < 0) {
        const std::string name = "T" + std::to_string(textTrackCount + 1);
        textTrack = model_.insertTrack(0, name, /*isAudio=*/false, /*isText=*/true);
        timeline_->update(); // the new lane appears at the top
        statusBar()->showMessage(
            tr("Added text track %1 (above the video lanes).").arg(QString::fromStdString(name)),
            4000);
    }
    return textTrack;
}

void MainWindow::addTextClip() {
    const int textTrack = ensureTextTrack();

    // Default document: a centered 72 px white title for 4 s.
    fc::TextDocument doc;
    fc::TextRun run;
    run.text = "Title";
    run.style.size = 72;
    doc.runs.push_back(run);
    doc.align = fc::TextAlign::Center;
    doc.box.anchorX = 0.5;
    doc.box.anchorY = 0.5;
    doc.box.wrap = 0.8;

    const int64_t duration = std::max<int64_t>(1, static_cast<int64_t>(std::llround(4.0 * fps_)));
    const int64_t playFrame = static_cast<int64_t>(std::llround(playhead_ * fps_));
    // clipId 0 = a NEW clip: the kind was validated above (text lane),
    // the drop snaps to the nearest free gap around the playhead.
    int64_t start = model_.findDropPosition(textTrack, 0, playFrame, duration);
    if (start < 0) {
        start = 0;
    }
    const int64_t id = model_.addTextClip(textTrack, doc, start, duration);
    if (id <= 0) {
        statusBar()->showMessage(tr("Could not add the text clip (the text track is full)."), 4000);
        return;
    }
    selectedClipId_ = id;
    timeline_->selectClip(id); // selects everywhere (panels included)
    updateSequenceDuration();
    markDirty();
    requestFrameAt(playhead_);
    statusBar()->showMessage(tr("Text clip added - type in the Text panel to edit it."), 6000);
}

void MainWindow::writeTextDocument(int64_t clipId, const fc::TextDocument &doc) {
    fc::Clip *clip = model_.clipById(clipId);
    if (!clip || !clip->isText) {
        return;
    }
    clip->text = doc;
    fc::normalizeTextDocument(clip->text);
    clip->label = fc::textPreviewLabel(clip->text);
    textLayerCache_.erase(clipId); // the layer is text-derived; re-render
    markDirty();
    timeline_->update(); // the clip label preview
    applyProgramFrame(); // instant re-render (video cache or black)
}

QImage MainWindow::textLayerForClip(const fc::Clip *clip, int width, int height,
                                    int64_t clipFrame) {
    if (!clip || !clip->isText || width <= 0 || height <= 0) {
        return QImage();
    }
    // An animated clip's layer varies with the clip-relative frame, so
    // it never touches the cache (rendered fresh per frame); a static
    // clip caches exactly as before.
    const bool animated = clipFrame >= 0 && fc::hasTextAnimation(clip->text.animation);
    if (!animated) {
        auto it = textLayerCache_.find(clip->id);
        if (it != textLayerCache_.end() && it->second.width() == width &&
            it->second.height() == height && it->second.format() == QImage::Format_RGBA8888) {
            return it->second;
        }
    }
    QImage layer = fc::renderTextLayer(clip->text, width, height, animated ? clipFrame : -1,
                                       clip->durationFrames(), &emojiPainter_);
    if (layer.format() != QImage::Format_RGBA8888) {
        layer = layer.convertToFormat(QImage::Format_RGBA8888);
    }
    if (!animated) {
        textLayerCache_[clip->id] = layer;
    }
    return layer;
}

// ---------------------------------------------------------------------------
// Emoji font selection: the machine's installed emoji-capable fonts are
// scanned once at startup (a cheap directory probe rejects the ~99.9%
// without bitmap-emoji tables), the persisted user pick wins over the
// platform-preferred auto-pick, and the Text panel's combo reflects
// the list. Picking a different font switches the emoji ARTWORK.
// ---------------------------------------------------------------------------

void MainWindow::startupEmojiFont() {
    emojiFonts_ = fc::discoverEmojiFonts();
    QSettings settings;
    QString pick = settings.value("text/emojiFont").toString();
    if (pick.isEmpty() || !QFile::exists(pick)) {
        // No stored pick (or the file went away): auto-pick by the
        // platform's preferred family order.
        pick = QString();
        for (const QString &want : fc::preferredEmojiFontFamilies()) {
            for (const fc::SystemEmojiFont &f : emojiFonts_) {
                if (f.family.compare(want, Qt::CaseInsensitive) == 0) {
                    pick = f.path;
                    break;
                }
            }
            if (!pick.isEmpty()) {
                break;
            }
        }
    }
    if (!pick.isEmpty() && !emojiPainter_.load(pick)) {
        qWarning("FusionCut Pro: emoji font %s did not load - emoji render via the platform font",
                 qPrintable(pick));
        pick = QString(); // the combo shows System default then
    }
    emojiFontPath_ = pick;
    textPanel_->setEmojiFonts(emojiFonts_, pick);
}

void MainWindow::setEmojiFont(const QString &path) {
    emojiFontPath_ = path;
    QSettings settings;
    if (path.isEmpty()) {
        settings.remove("text/emojiFont");
        emojiPainter_ = fc::EmojiPainter(); // drop the loaded font
    } else {
        settings.setValue("text/emojiFont", path);
        if (!emojiPainter_.load(path)) {
            statusBar()->showMessage(
                tr("Could not load %1 - emoji keep rendering through the platform font.")
                    .arg(QFileInfo(path).fileName()),
                6000);
        }
    }
    // The cached text layers embed the OLD font's bitmaps: drop them
    // all and re-render the program monitor.
    textLayerCache_.clear();
    applyProgramFrame();
}

// ---------------------------------------------------------------------------
// Captions: .srt cues in (one text clip per cue, caption styling),
// text clips out (flattened plain text, frame times -> milliseconds).
// ---------------------------------------------------------------------------

namespace {
// Caption defaults: centered white 56 px near the bottom of the frame
// with a soft dark box - readable over almost any footage.
constexpr int kCaptionSizePx = 56;

double captionAnchorY() {
    return 0.9;
}
} // namespace

int MainWindow::ensureAudioTrack() {
    for (const fc::Track &track : model_.tracks()) {
        if (track.isAudio) {
            return track.index;
        }
    }
    const int index = model_.addTrack("A1", true);
    timeline_->setModel(&model_); // the lane appears in the timeline UI
    return index;
}

void MainWindow::rebuildAudioSnapshot() {
    audioSpansRevision_ = model_.revision();
    // Proxy resolution: the decode path for every source (the same
    // rule the video path uses; runs on the GUI thread - the audio
    // thread only reads the published snapshot).
    auto resolver = [this](const std::string &src) -> std::string {
        const QString source = QString::fromStdString(src);
        const int index = projectPanel_->library().indexOfPath(source);
        return (index >= 0 && projectPanel_->library().at(index)->hasProxy())
                   ? projectPanel_->library().at(index)->proxyPath.toStdString()
                   : src;
    };
    audioSpans_.publish(fc::flattenAudioSpans(model_, fps_, resolver),
                        fc::audioDbToLinear(model_.masterGainDb()));
}

void MainWindow::startAudioPreview() {
    stopAudioPreview();
    rebuildAudioSnapshot();
    if (!audioSpans_.hasAudio()) {
        return; // nothing to hear: no device, no mixer, the tick keeps time
    }
    int rate = 0, channels = 0;
    if (!audioPreview_->probe(rate, channels)) {
        return; // no usable audio output on this machine
    }
    previewMixer_ = std::make_unique<fc::AudioWindowMixer>(rate, channels);
    previewMixer_->setMasterGain(fc::audioDbToLinear(model_.masterGainDb()));
    audioStartSeconds_ = playhead_;
    audioStartSample_ = static_cast<int64_t>(std::llround(playhead_ * double(rate)));
    audioPulled_ = 0;
    fc::AudioWindowMixer *mixer = previewMixer_.get();
    const int64_t startSample = audioStartSample_;
    audioPreview_->begin([this, mixer, startSample, rate](float *dst, int frames) {
        // Audio render thread: ONLY thread-safe state is touched here
        // (the snapshot mutex + the mixer, which this thread alone
        // owns while running).
        std::vector<fc::AudioSpan> spans;
        double master = 1.0;
        audioSpans_.take(spans, master);
        mixer->setMasterGain(master); // live fader moves
        const int64_t position = startSample + audioPulled_.load();
        std::string pullError;
        mixer->pull(spans, position, frames, dst, pullError);
        audioPulled_ += frames;
    });
}

void MainWindow::stopAudioPreview() {
    if (audioPreview_) {
        audioPreview_->stop();
    }
    previewMixer_.reset();
}

void MainWindow::importCaptions() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import Subtitles"), QString(), tr("SubRip subtitles (*.srt);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Import Subtitles"),
                             tr("Could not open %1 for reading.").arg(path));
        return;
    }
    const QByteArray raw = file.readAll();
    const std::string body(raw.constData(), raw.constData() + raw.size());
    std::vector<fc::SrtCue> cues;
    std::string parseError;
    if (!fc::parseSrt(body, cues, parseError)) {
        QMessageBox::warning(
            this, tr("Import Subtitles"),
            tr("The file is not valid SubRip:%1").arg("\n" + QString::fromStdString(parseError)));
        return;
    }
    if (cues.empty()) {
        statusBar()->showMessage(tr("No caption cues found in %1.").arg(path), 6000);
        return;
    }

    const int track = ensureTextTrack();
    int added = 0;
    int skipped = 0;
    for (const fc::SrtCue &cue : cues) {
        const int64_t start =
            static_cast<int64_t>(std::llround(static_cast<double>(cue.startMs) * fps_ / 1000.0));
        const int64_t duration = static_cast<int64_t>(
            std::llround(static_cast<double>(cue.endMs - cue.startMs) * fps_ / 1000.0));
        if (start < 0 || duration < 1) {
            ++skipped; // zero-length or reversed cue
            continue;
        }
        fc::TextDocument doc;
        fc::TextRun run;
        run.text = fc::stripSrtMarkup(cue.text);
        run.style.size = kCaptionSizePx;
        run.style.colorRgba = 0xFFFFFFFFu;
        doc.runs.push_back(std::move(run));
        doc.align = fc::TextAlign::Center;
        doc.box.anchorX = 0.5;
        doc.box.anchorY = captionAnchorY();
        doc.box.wrap = 0.8;
        doc.box.background = true; // classic caption backing box
        if (model_.addTextClip(track, doc, start, duration) <= 0) {
            ++skipped; // overlapped another caption (exact placement only)
            continue;
        }
        ++added;
    }
    updateSequenceDuration();
    timeline_->update();
    markDirty();
    applyProgramFrame(); // in case the playhead already sits on a caption
    if (skipped > 0) {
        statusBar()->showMessage(tr("Imported %1 caption(s) from %2 (%3 skipped - too short or "
                                    "overlapping).")
                                     .arg(added)
                                     .arg(QFileInfo(path).fileName())
                                     .arg(skipped),
                                 8000);
    } else {
        statusBar()->showMessage(
            tr("Imported %1 caption(s) from %2.").arg(added).arg(QFileInfo(path).fileName()), 8000);
    }
}

void MainWindow::exportCaptions() {
    // Collect every text clip as a cue: chronological, ties broken by
    // track order (the loop walks the tracks in model order).
    struct Pending {
        int64_t start;
        int trackIndex;
        fc::SrtCue cue;
    };
    std::vector<Pending> pending;
    for (const fc::Track &track : model_.tracks()) {
        if (!track.isText) {
            continue;
        }
        for (const fc::Clip &clip : model_.clips()) {
            if (clip.trackIndex != track.index || !clip.isText) {
                continue;
            }
            std::string text;
            for (const fc::TextRun &run : clip.text.runs) {
                text += run.text; // embedded '\n's are the cue's line breaks
            }
            if (text.empty()) {
                continue; // blank clips produce no cue
            }
            fc::SrtCue cue;
            cue.startMs = static_cast<int64_t>(
                std::llround(static_cast<double>(clip.timelineStart) * 1000.0 / fps_));
            cue.endMs = static_cast<int64_t>(
                std::llround(static_cast<double>(clip.timelineEnd()) * 1000.0 / fps_));
            cue.text = std::move(text);
            pending.push_back(Pending{clip.timelineStart, track.index, std::move(cue)});
        }
    }
    if (pending.empty()) {
        statusBar()->showMessage(tr("No text clips on the timeline to export as subtitles."), 6000);
        return;
    }
    std::stable_sort(pending.begin(), pending.end(),
                     [](const Pending &a, const Pending &b) { return a.start < b.start; });
    std::vector<fc::SrtCue> cues;
    cues.reserve(pending.size());
    for (Pending &p : pending) {
        cues.push_back(std::move(p.cue));
    }

    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export Subtitles"), QString(), tr("SubRip subtitles (*.srt);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, tr("Export Subtitles"),
                             tr("Could not open %1 for writing.").arg(path));
        return;
    }
    const std::string out = fc::writeSrt(cues);
    if (file.write(out.data(), static_cast<qint64>(out.size())) !=
        static_cast<qint64>(out.size())) {
        QMessageBox::warning(this, tr("Export Subtitles"),
                             tr("Writing %1 failed (disk full or permissions?).").arg(path));
        return;
    }
    statusBar()->showMessage(tr("Exported %1 subtitle cue(s) to %2.")
                                 .arg(static_cast<int>(cues.size()))
                                 .arg(QFileInfo(path).fileName()),
                             8000);
}

void MainWindow::restoreLayout() {
    QSettings settings;
    restoreGeometry(settings.value("main/geometry").toByteArray());
    restoreState(settings.value("main/state").toByteArray());
}

// ---------------------------------------------------------------------------
// cut transitions.
// ---------------------------------------------------------------------------

void MainWindow::addTransitionToSelectedClip(const QString &kind) {
    fc::Clip *clip = selectedClipId_ > 0 ? model_.clipById(selectedClipId_) : nullptr;
    if (!clip) {
        statusBar()->showMessage(
            tr("Select a timeline clip first (the transition goes on its outgoing cut)."), 6000);
        return;
    }
    if (const fc::Track *track = model_.trackAt(clip->trackIndex); track && track->locked) {
        statusBar()->showMessage(tr("Track %1 is locked - transition rejected.")
                                     .arg(QString::fromStdString(track->name)));
        return;
    }
    // The right neighbor sharing the cut (the boundary = this clip's end).
    const fc::Clip *right = nullptr;
    for (const fc::Clip &other : model_.clips()) {
        if (other.trackIndex == clip->trackIndex && other.id != clip->id &&
            other.timelineStart == clip->timelineEnd()) {
            right = &other;
            break;
        }
    }
    if (!right) {
        statusBar()->showMessage(
            tr("The clip must touch another clip - transitions live on the cut between "
               "two adjacent clips."),
            6000);
        return;
    }
    const int64_t cap = model_.maxTransitionDuration(clip->id, right->id);
    if (cap < 1) {
        statusBar()->showMessage(
            tr("This cut cannot carry a transition (it may already have one, or the clip "
               "is too short)."),
            6000);
        return;
    }
    // Default: 1 second of timeline frames, clamped to the clip extent.
    int64_t want = static_cast<int64_t>(std::llround(1.0 * fps_));
    if (want < 1) {
        want = 1;
    }
    if (want > cap) {
        want = cap;
    }
    const int64_t id = model_.addTransition(clip->id, right->id, kind.toStdString(), want);
    if (id <= 0) {
        statusBar()->showMessage(tr("Transition rejected - unknown kind or invalid duration."),
                                 6000);
        return;
    }
    const fc::TransitionDescriptor *d = fc::findTransition(kind.toStdString());
    statusBar()->showMessage(tr("Added %1 on the cut %2 -> %3 (click its green marker to edit)")
                                 .arg(d ? QString::fromStdString(d->label) : kind,
                                      QString::fromStdString(clip->label),
                                      QString::fromStdString(right->label)),
                             6000);
    updateSequenceDuration();
    timeline_->selectTransition(id); // opens the editor on the new marker
    applyProgramFrame();             // live when the playhead is in the window
}

void MainWindow::ensureHeldIncomingFrame(const fc::TransitionSample &sample) {
    // Cached, in flight, or already failed for exactly this clip?
    if (heldIncomingClipId_ == sample.rightClipId &&
        (!heldIncomingFrame_.isNull() || bRequestInFlight_ ||
         bFailedClipId_ == sample.rightClipId)) {
        return;
    }
    const fc::Clip *right = model_.clipById(sample.rightClipId);
    if (!right) {
        return;
    }
    heldIncomingClipId_ = sample.rightClipId;
    heldIncomingFrame_ = QImage();
    pendingBFirstClipId_ = sample.rightClipId;
    bRequestInFlight_ = true;

    const QString clipSource = QString::fromStdString(right->sourcePath);
    const int index = projectPanel_->library().indexOfPath(clipSource);
    const QString path = (index >= 0 && projectPanel_->library().at(index)->hasProxy())
                             ? projectPanel_->library().at(index)->proxyPath
                             : clipSource;
    if (path != bLoadedPath_) {
        bLoadedPath_ = path;
        pendingBFirst_ = true; // the mediaInfo handler fires the request
        QMetaObject::invokeMethod(workerB_, "openQuiet", Q_ARG(QString, path));
    } else {
        pendingBFirst_ = false;
        QMetaObject::invokeMethod(workerB_, "requestFrame",
                                  Q_ARG(double, static_cast<double>(right->sourceInFrames) / fps_));
    }
}

void MainWindow::pushTransitionToEditor(const fc::Transition &t) {
    const fc::Clip *left = model_.clipById(t.leftClipId);
    const fc::Clip *right = model_.clipById(t.rightClipId);
    const fc::TransitionDescriptor *d = fc::findTransition(t.kind);
    const QString label = d ? QString::fromStdString(d->label) : QString::fromStdString(t.kind);
    const QString pair = tr("%1 -> %2")
                             .arg(left ? QString::fromStdString(left->label) : tr("(removed)"),
                                  right ? QString::fromStdString(right->label) : tr("(removed)"));
    effectControls_->setTransition(t.id, label, t.durationFrames,
                                   left ? left->durationFrames() : t.durationFrames, pair, fps_);
}

void MainWindow::syncTransitionEditor() {
    if (selectedTransitionId_ < 0) {
        return;
    }
    const fc::Transition *t = model_.transitionById(selectedTransitionId_);
    if (!t) {
        // Pruned by a mutation (clip removed / moved / trimmed apart).
        selectedTransitionId_ = -1;
        timeline_->clearTransitionSelection();
        effectControls_->setTransition(-1, QString(), 0, 1, QString(), fps_);
        return;
    }
    pushTransitionToEditor(*t); // durations may have clamped
}

// ---------------------------------------------------------------------------
// keyframe panel sync + project persistence + export.
// ---------------------------------------------------------------------------

void MainWindow::updateKeyframePanels() {
    if (selectedClipId_ <= 0) {
        return;
    }
    const fc::Clip *clip = model_.clipById(selectedClipId_);
    if (!clip) {
        return;
    }
    const int64_t frame = static_cast<int64_t>(std::llround(playhead_ * fps_));
    if (frame >= clip->timelineStart && frame < clip->timelineEnd()) {
        const int64_t inClip = frame - clip->timelineStart;
        effectControls_->setClipFrame(inClip);
        colorPanel_->setClipFrame(inClip);
    } else {
        effectControls_->setClipFrame(-1);
        colorPanel_->setClipFrame(-1);
    }
}

void MainWindow::markDirty() {
    if (!dirty_) {
        dirty_ = true;
        updateWindowTitle();
    }
}

void MainWindow::updateWindowTitle() {
    const QString name =
        projectPath_.isEmpty() ? tr("Untitled") : QFileInfo(projectPath_).fileName();
    setWindowTitle(tr("FusionCut Pro - %1[*]").arg(name));
    setWindowModified(dirty_);
}

void MainWindow::saveProject() {
    if (projectPath_.isEmpty()) {
        saveProjectAs();
        return;
    }
    const std::string text = fc::serializeProject(model_);
    QFile file(projectPath_);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, tr("Save Project"),
                             tr("Could not write %1: %2").arg(projectPath_, file.errorString()));
        return;
    }
    const qint64 written = file.write(text.data(), static_cast<qint64>(text.size()));
    file.close();
    if (written != static_cast<qint64>(text.size())) {
        QMessageBox::warning(this, tr("Save Project"),
                             tr("Could not write %1: the file is incomplete.").arg(projectPath_));
        return;
    }
    dirty_ = false;
    updateWindowTitle();
    statusBar()->showMessage(tr("Project saved: %1").arg(projectPath_), 6000);
}

bool MainWindow::saveProjectAs() {
    QString suggested = projectPath_;
    if (suggested.isEmpty()) {
        suggested = QStringLiteral("untitled.fcp");
    }
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save Project As"), suggested, tr("FusionCut Project (*.fcp);;All Files (*)"));
    if (path.isEmpty()) {
        return false;
    }
    projectPath_ = path;
    saveProject();
    return !dirty_;
}

void MainWindow::openProject() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open Project"), QString(), tr("FusionCut Project (*.fcp);;All Files (*)"));
    if (path.isEmpty()) {
        return;
    }
    if (!confirmSaveChanges()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Open Project"),
                             tr("Could not read %1: %2").arg(path, file.errorString()));
        return;
    }
    const QByteArray raw = file.readAll();
    const std::string text(raw.constData(), static_cast<size_t>(raw.size()));
    std::string error;
    if (!fc::parseProject(text, model_, error)) {
        QMessageBox::warning(this, tr("Open Project"),
                             tr("This is not a valid FusionCut project:%1")
                                 .arg(QString::fromStdString("\n" + error)));
        return;
    }

    // Reset the session around the loaded model.
    startPlayback(false);
    projectPath_ = path;
    dirty_ = false;
    updateWindowTitle();
    selectedClipId_ = -1;
    selectedTransitionId_ = -1;
    lastProgramClipId_ = -1;
    frameClipId_ = -1;
    pendingProgramSeek_ = false;
    pendingAddClipPath_.clear();
    rawProgramFrame_ = QImage();
    textLayerCache_.clear(); // layers belong to the OLD model's clips
    heldIncomingFrame_ = QImage();
    heldIncomingClipId_ = -1;
    pendingBFirstClipId_ = -1;
    pendingBFirst_ = false;
    bRequestInFlight_ = false;
    bFailedClipId_ = -1;
    bLoadedPath_.clear();
    playhead_ = 0.0;
    timeline_->clearSelection();
    timeline_->clearTransitionSelection();
    effectControls_->setStack(-1, {});
    effectControls_->setClipFrame(-1);
    effectControls_->setTransition(-1, QString(), 0, 1, QString(), fps_);
    colorPanel_->setClip(-1, {});
    colorPanel_->setClipFrame(-1);
    textPanel_->setClip(-1, nullptr);
    mixer_->refreshFromModel(&model_); // the loaded project's strips
    rebuildAudioSnapshot();

    // Rebuild the media library from the clip sources (order of first
    // use); proxies survive when their generated file still exists.
    projectPanel_->library().clear();
    for (const fc::Clip &clip : model_.clips()) {
        const QString src = QString::fromStdString(clip.sourcePath);
        if (projectPanel_->library().indexOfPath(src) >= 0) {
            continue;
        }
        fc::MediaItem item;
        item.path = src;
        item.displayName = QFileInfo(src).completeBaseName();
        const QString proxy = proxyPathFor(src);
        if (QFileInfo::exists(proxy)) {
            item.proxyPath = proxy;
        }
        projectPanel_->addMedia(item);
    }

    duration_ = model_.durationSeconds();
    transport_->setMedia(duration_ > 0.0 ? duration_ : 10.0, fps_);
    updateSequenceDuration();
    timeline_->setModel(&model_); // content refresh (same model object)
    timeline_->update();

    // Load the first video clip into the monitor.
    for (const fc::Clip &clip : model_.clips()) {
        const fc::Track *track = model_.trackAt(clip.trackIndex);
        if (track && !track->isAudio) {
            loadClip(QString::fromStdString(clip.sourcePath));
            break;
        }
    }
    statusBar()->showMessage(tr("Project loaded: %1 (%2 clips, %3 transitions)")
                                 .arg(path)
                                 .arg(model_.clips().size())
                                 .arg(model_.transitions().size()),
                             8000);
}

bool MainWindow::confirmSaveChanges() {
    if (!dirty_) {
        return true;
    }
    const QMessageBox::StandardButton choice = QMessageBox::question(
        this, tr("Save Project?"), tr("The project has unsaved changes."),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (choice == QMessageBox::Save) {
        saveProject();
        return !dirty_; // a failed save keeps the window open
    }
    return choice == QMessageBox::Discard;
}

void MainWindow::exportMedia() {
    if (model_.clips().empty()) {
        statusBar()->showMessage(tr("Nothing to export - import media and build a timeline first."),
                                 6000);
        return;
    }
    if (exportRunning_) {
        return;
    }
    const QString suggested = QStringLiteral("FusionCutPro-Export.mp4");
    const QString path = QFileDialog::getSaveFileName(this, tr("Export Media"), suggested,
                                                      tr("MP4 Video (*.mp4);;All Files (*)"));
    if (path.isEmpty()) {
        return;
    }

    // Suggest the first video clip's native size.
    int srcW = 0;
    int srcH = 0;
    for (const fc::Clip &clip : model_.clips()) {
        const fc::Track *track = model_.trackAt(clip.trackIndex);
        if (!track || track->isAudio) {
            continue;
        }
        const int index =
            projectPanel_->library().indexOfPath(QString::fromStdString(clip.sourcePath));
        const QString probePath = (index >= 0 && projectPanel_->library().at(index)->hasProxy())
                                      ? projectPanel_->library().at(index)->proxyPath
                                      : QString::fromStdString(clip.sourcePath);
        fc::MediaInfo info;
        std::string probeError;
        if (fc::MediaProbe::probe(probePath.toStdString(), info, probeError) && info.hasVideo) {
            srcW = info.video.width;
            srcH = info.video.height;
        }
        break;
    }

    const int64_t totalFrames = std::max<int64_t>(1, model_.durationFrames());
    ExportDialog dialog(fps_, totalFrames, srcW, srcH, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    startPlayback(false);
    exportCancel_ = false;
    exportRunning_ = true;

    // Modal progress dialog; the export itself runs on the decode worker
    // thread (see runExportJob) and closes it via finishExport.
    exportDialog_ = new QDialog(this);
    exportDialog_->setWindowTitle(tr("Exporting Media"));
    exportDialog_->setModal(true);
    exportBar_ = new QProgressBar(exportDialog_);
    exportBar_->setRange(0, 100);
    exportBar_->setValue(0);
    auto *cancel = new QPushButton(tr("Cancel"), exportDialog_);
    connect(cancel, &QPushButton::clicked, this, [this, cancel] {
        exportCancel_ = true;
        cancel->setEnabled(false);
        cancel->setText(tr("Cancelling..."));
    });
    auto *layout = new QVBoxLayout(exportDialog_);
    layout->addWidget(exportBar_);
    layout->addWidget(cancel, 0, Qt::AlignRight);

    // The worker-thread job is queued behind anything the decoder is
    // doing; the dialog spins the UI event loop in the meantime.
    const int width = dialog.outputWidth();
    const int height = dialog.outputHeight();
    const int crf = dialog.crf();
    const QString preset = dialog.preset();
    const bool withAudio = dialog.includeAudio();
    exportDialog_->show();
    QMetaObject::invokeMethod(worker_, [this, path, width, height, crf, preset, withAudio] {
        runExportJob(path, width, height, crf, preset, withAudio);
    });
}

void MainWindow::runExportJob(const QString &path, int width, int height, int crf,
                              const QString &preset, bool withAudio) {
    // ---- Runs on the decode worker's thread. The modal progress dialog
    // blocks all UI input, so the timeline model is effectively frozen
    // while this job reads it. ----
    const int64_t totalFrames = model_.durationFrames();
    const double fps = fps_ > 1.0 ? fps_ : 24.0;

    // Source path -> decode path (proxy when one exists), frozen up front.
    std::map<std::string, std::string> decodePaths;
    for (const fc::Clip &clip : model_.clips()) {
        if (decodePaths.count(clip.sourcePath)) {
            continue;
        }
        const int index =
            projectPanel_->library().indexOfPath(QString::fromStdString(clip.sourcePath));
        decodePaths[clip.sourcePath] =
            (index >= 0 && projectPanel_->library().at(index)->hasProxy())
                ? projectPanel_->library().at(index)->proxyPath.toStdString()
                : clip.sourcePath;
    }

    // Per-source decoders + last-decoded position (sequential reads,
    // seeks only on jumps); held transition frames cached per right clip.
    std::map<std::string, std::unique_ptr<fc::VideoDecoder>> decoders;
    std::map<std::string, double> nextPts;
    std::map<int64_t, QImage> held;
    // Rendered text layers cached per clip (time-invariant docs only)
    // at the export resolution. Animated clips render per frame. The
    // emoji painter is job-local: its caches are not thread-safe and
    // the worker must not touch the GUI-side painter (the path is
    // snapshotted when the job starts).
    std::map<int64_t, QImage> textLayers;
    fc::EmojiPainter exportEmoji;
    if (!emojiFontPath_.isEmpty()) {
        exportEmoji.load(emojiFontPath_);
    }

    // ---- Audio: the timeline flattened ONCE (frozen for the job's
    // lifetime, like every other model read here) into a job-local
    // mixer at 48 kHz stereo. The provider mirrors the video side's
    // tolerance: a broken source mixes as silence and names itself in
    // the note surfaced after the job.
    std::unique_ptr<fc::AudioWindowMixer> audioMixer;
    std::vector<fc::AudioSpan> audioSpans;
    fc::ExportAudioProvider audioProvider;
    exportAudioNote_.clear();
    if (withAudio) {
        auto resolver = [this](const std::string &src) -> std::string {
            const QString source = QString::fromStdString(src);
            const int index = projectPanel_->library().indexOfPath(source);
            return (index >= 0 && projectPanel_->library().at(index)->hasProxy())
                       ? projectPanel_->library().at(index)->proxyPath.toStdString()
                       : src;
        };
        audioSpans = fc::flattenAudioSpans(model_, fps_, resolver);
        if (!audioSpans.empty()) {
            audioMixer = std::make_unique<fc::AudioWindowMixer>(48000, 2);
            audioMixer->setMasterGain(fc::audioDbToLinear(model_.masterGainDb()));
            fc::AudioWindowMixer *mixer = audioMixer.get();
            QString *note = &exportAudioNote_;
            audioProvider = [mixer, &audioSpans, note](int64_t startSample, int sampleCount,
                                                       float *dst) -> bool {
                std::string pullError;
                if (!mixer->pull(audioSpans, startSample, sampleCount, dst, pullError)) {
                    return false; // invalid arguments - treat as cancellation
                }
                if (!pullError.empty() && note->isEmpty()) {
                    *note = QString::fromStdString(pullError);
                }
                return true;
            };
        }
    }

    auto fetchFrame = [&](const fc::Clip &clip, int64_t timelineFrame, QImage &out) -> bool {
        const double srcSec =
            static_cast<double>(timelineFrame - clip.timelineStart + clip.sourceInFrames) / fps;
        const std::string &decPath = decodePaths[clip.sourcePath];
        auto it = decoders.find(decPath);
        if (it == decoders.end()) {
            std::string openError;
            auto decoder = std::make_unique<fc::VideoDecoder>();
            if (!decoder->open(decPath, openError)) {
                return false;
            }
            it = decoders.emplace(decPath, std::move(decoder)).first;
            nextPts[decPath] = -1.0e9;
        }
        fc::VideoDecoder *dec = it->second.get();
        const double expect = nextPts[decPath];
        std::string err;
        if (srcSec < expect - 0.5 / fps || srcSec > expect + 2.5 / fps) {
            if (!dec->seekToSeconds(srcSec, err)) {
                return false;
            }
        }
        fc::DecodedFrame df;
        if (!dec->readFrame(df, err)) {
            return false;
        }
        nextPts[decPath] = df.ptsSeconds + 1.0 / fps;
        out = QImage(df.rgba.data(), df.width, df.height, QImage::Format_RGBA8888).copy();
        return true;
    };

    fc::ExportConfig config;
    config.width = width;
    config.height = height;
    config.fps = fps;
    config.totalFrames = totalFrames;
    config.crf = crf;
    config.preset = preset.toStdString();

    auto provider = [&](int64_t f, uint8_t *rgba) -> bool {
        if (exportCancel_.load()) {
            return false;
        }
        QImage out(width, height, QImage::Format_RGBA8888);
        out.fill(Qt::black);
        const fc::Clip *clip = model_.activeVideoClipAt(f);
        if (clip) {
            QImage live;
            if (fetchFrame(*clip, f, live)) {
                if (live.format() != QImage::Format_RGBA8888) {
                    live = live.convertToFormat(QImage::Format_RGBA8888);
                }
                fc::applyEffectStack(live.bits(), live.width(), live.height(), clip->effectStack,
                                     f - clip->timelineStart);
                bool composited = false;
                fc::TransitionSample sample;
                if (model_.resolveTransitionAt(f, clip->trackIndex, sample) &&
                    sample.leftClipId == clip->id) {
                    auto heldIt = held.find(sample.rightClipId);
                    if (heldIt == held.end()) {
                        const fc::Clip *right = model_.clipById(sample.rightClipId);
                        QImage hf;
                        if (right && fetchFrame(*right, right->timelineStart, hf)) {
                            heldIt = held.emplace(sample.rightClipId, hf).first;
                        } else {
                            heldIt = held.emplace(sample.rightClipId, QImage()).first;
                        }
                    }
                    if (!heldIt->second.isNull()) {
                        QImage heldFrame = heldIt->second;
                        if (heldFrame.format() != QImage::Format_RGBA8888) {
                            heldFrame = heldFrame.convertToFormat(QImage::Format_RGBA8888);
                        }
                        const fc::Clip *right = model_.clipById(sample.rightClipId);
                        if (right && !right->effectStack.empty()) {
                            // The held frame is the incoming clip's first
                            // frame: clip-relative position 0.
                            fc::applyEffectStack(heldFrame.bits(), heldFrame.width(),
                                                 heldFrame.height(), right->effectStack, 0);
                        }
                        QImage a = live.scaled(width, height, Qt::IgnoreAspectRatio,
                                               Qt::SmoothTransformation);
                        QImage b = heldFrame.scaled(width, height, Qt::IgnoreAspectRatio,
                                                    Qt::SmoothTransformation);
                        if (a.format() == QImage::Format_RGBA8888 &&
                            b.format() == QImage::Format_RGBA8888 && width > 0 && height > 0) {
                            fc::applyTransition(a.bits(), b.bits(), out.bits(), width, height,
                                                sample.kind, sample.progress);
                            composited = true;
                        }
                    }
                }
                if (!composited) {
                    out =
                        live.scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                            .convertToFormat(QImage::Format_RGBA8888);
                }
            }
        }

        // Composite the text stack ON TOP (same paint order as the
        // program monitor: bottom text lane first). A static text layer
        // is rendered at the export resolution and cached; an ANIMATED
        // clip re-renders per frame through the same evaluator the
        // preview uses. The text clip's own effect stack runs on a copy
        // per frame so keyframed parameters resolve exactly like the
        // preview.
        for (const fc::Clip *textClip : model_.textClipsAt(f)) {
            if (!textClip || !textClip->isText) {
                continue;
            }
            const bool animated = fc::hasTextAnimation(textClip->text.animation);
            QImage layer;
            if (animated) {
                layer =
                    fc::renderTextLayer(textClip->text, width, height, f - textClip->timelineStart,
                                        textClip->durationFrames(), &exportEmoji);
            } else {
                auto layerIt = textLayers.find(textClip->id);
                if (layerIt == textLayers.end()) {
                    layerIt =
                        textLayers
                            .emplace(textClip->id, fc::renderTextLayer(textClip->text, width,
                                                                       height, -1, 0, &exportEmoji))
                            .first;
                }
                layer = layerIt->second;
            }
            if (layer.isNull() || layer.format() != QImage::Format_RGBA8888 ||
                layer.size() != out.size()) {
                continue;
            }
            if (!textClip->effectStack.empty()) {
                QImage effected = layer.copy();
                fc::applyEffectStack(effected.bits(), effected.width(), effected.height(),
                                     textClip->effectStack, f - textClip->timelineStart);
                layer = effected;
            }
            if (width > 0 && height > 0 && out.bytesPerLine() == static_cast<int>(width) * 4 &&
                layer.bytesPerLine() == static_cast<int>(width) * 4) {
                fc::compositeOver(out.bits(), layer.constBits(), width, height);
            }
        }
        if (out.format() == QImage::Format_RGBA8888 &&
            out.bytesPerLine() == static_cast<int>(width) * 4) {
            std::memcpy(rgba, out.constBits(), static_cast<size_t>(width) * height * 4);
        }
        return true;
    };

    auto progressCb = [&](double fraction) -> bool {
        if (exportCancel_.load()) {
            return false;
        }
        const int percent = static_cast<int>(std::lround(fraction * 100.0));
        QMetaObject::invokeMethod(
            this, [this, percent] { updateExportProgress(percent); }, Qt::QueuedConnection);
        return true;
    };

    std::string error;
    const bool ok =
        fc::Exporter::run(path.toStdString(), config, provider, progressCb, error, audioProvider);
    const QString errorText = QString::fromStdString(error);
    QMetaObject::invokeMethod(
        this, [this, ok, errorText, path] { finishExport(ok, errorText, path); },
        Qt::QueuedConnection);
}

void MainWindow::updateExportProgress(int percent) {
    if (exportBar_) {
        exportBar_->setValue(percent);
    }
    statusBar()->showMessage(tr("Exporting... %1%").arg(percent));
}

void MainWindow::finishExport(bool ok, const QString &error, const QString &path) {
    // A successful export may still carry an audio note (a source that
    // would not decode mixed as silence instead of killing the job).
    const QString audioNote = exportAudioNote_;
    exportAudioNote_.clear();
    exportRunning_ = false;
    if (exportDialog_) {
        exportDialog_->deleteLater();
        exportDialog_ = nullptr;
        exportBar_ = nullptr;
    }
    if (ok) {
        if (!audioNote.isEmpty()) {
            statusBar()->showMessage(
                tr("Export complete: %1 (audio note: %2)").arg(path, audioNote), 12000);
        } else {
            statusBar()->showMessage(tr("Export complete: %1").arg(path), 8000);
        }
    } else if (error.isEmpty()) {
        statusBar()->showMessage(tr("Export cancelled - no file was written."), 6000);
    } else {
        QMessageBox::warning(this, tr("Export Media"),
                             tr("The export failed:%1").arg(QString("\n" + error)));
    }
}

void MainWindow::saveLayout() const {
    QSettings settings;
    settings.setValue("main/geometry", saveGeometry());
    settings.setValue("main/state", saveState());
}

} // namespace fc
