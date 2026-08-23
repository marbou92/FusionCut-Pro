#include "mainwindow.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPalette>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QThread>
#include <QVBoxLayout>

#include <fc/version.h>

#include "decode_worker.h"
#include "effect_controls_panel.h"
#include "effects_panel.h"
#include "ffmpeg_wrappers.h"
#include "mixer_panel.h"
#include "preview_canvas.h"
#include "project_panel.h"
#include "quick_mode_view.h"
#include "timeline_panel.h"
#include "transport_bar.h"

namespace fc {

namespace {

// FusionCut Pro design tokens (Module 2.3 design system).
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

    playClock_ = new QTimer(this);
    playClock_->setTimerType(Qt::CoarseTimer);
    connect(playClock_, &QTimer::timeout, this, [this] {
        playhead_ += 1.0 / fps_;
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

    restoreLayout();
}

MainWindow::~MainWindow() {
    if (decodeThread_) {
        decodeThread_->quit();
        decodeThread_->wait(3000);
    }
}

void MainWindow::closeEvent(QCloseEvent *event) {
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
            [this](const QString &summary, double duration, double fps, int64_t) {
                duration_ = duration;
                fps_ = fps > 1.0 ? fps : 24.0;
                transport_->setMedia(duration_, fps_);
                timeline_->setSequenceDuration(duration_);
                timeline_->setFps(fps_);
                statusBar()->showMessage(summary, 8000);
            });
    connect(worker_, &DecodeWorker::frameReady, this, [this](const QImage &frame, double pts) {
        programCanvas_->setFrame(frame, pts);
        quickView_->canvas()->setFrame(frame, pts);
        timeline_->setPlayhead(pts);
        transport_->setPosition(pts);
        if (captureThumbnail_) {
            sourceCanvas_->setFrame(frame, pts);
            projectPanel_->setThumbnail(loadedPath_, frame);
            captureThumbnail_ = false;
        }
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
}

void MainWindow::buildProWorkspace() {
    // Left zone: Project | Effects (tabbed).
    projectPanel_ = new ProjectPanel(this);
    effectsPanel_ = new EffectsPanel(this);
    auto *leftTabs = new QTabWidget(this);
    leftTabs->addTab(projectPanel_, tr("Project"));
    leftTabs->addTab(effectsPanel_, tr("Effects"));
    auto *leftDock = makeDock(tr("Project"), leftTabs, this);
    addDockWidget(Qt::LeftDockWidgetArea, leftDock);

    // Bottom zone: Timeline | Audio Mixer (tabbed).
    timeline_ = new TimelinePanel(this);
    mixer_ = new MixerPanel(this);
    auto *bottomTabs = new QTabWidget(this);
    bottomTabs->addTab(timeline_, tr("Timeline"));
    bottomTabs->addTab(mixer_, tr("Audio Mixer"));
    auto *bottomDock = makeDock(tr("Timeline"), bottomTabs, this);
    addDockWidget(Qt::BottomDockWidgetArea, bottomDock);

    // Right zone: Effect Controls.
    effectControls_ = new EffectControlsPanel(this);
    auto *rightDock = makeDock(tr("Effect Controls"), effectControls_, this);
    addDockWidget(Qt::RightDockWidgetArea, rightDock);

    // Center: Source (left) | Program (right) monitors (Module 2.1).
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
    connect(projectPanel_, &ProjectPanel::loadRequested, this,
            [this](const QString &path) { loadClip(path); });
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
    QAction *importAction = addMenuAction(file, tr("&Import Media..."), QKeySequence(tr("Ctrl+I")));
    connect(importAction, &QAction::triggered, this, [this] { importMedia(); });
    QAction *exportAction = addMenuAction(file, tr("&Export Media..."), QKeySequence(tr("Ctrl+M")));
    exportAction->setEnabled(false);
    exportAction->setToolTip(tr("Export ships with the M4+ editing core"));
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
    addMenuAction(clip, tr("Split at Playhead"), QKeySequence(tr("C")))
        ->setToolTip(tr("Timeline editing ships in M4"));
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

    // ---- Sequence ----
    QMenu *sequence = menuBar()->addMenu(tr("Se&quence"));
    addMenuAction(sequence, tr("&Render In to Out"), QKeySequence(tr("Enter")))->setEnabled(false);

    // ---- Effects ----
    QMenu *effects = menuBar()->addMenu(tr("&Effects"));
    addMenuAction(effects, tr("Apply &Default Transition"), QKeySequence(tr("Ctrl+D")))
        ->setEnabled(false);

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

    const int index = projectPanel_->library().indexOfPath(sourcePath);
    const QString path = (index >= 0 && projectPanel_->library().at(index)->hasProxy())
                             ? projectPanel_->library().at(index)->proxyPath
                             : sourcePath;
    QMetaObject::invokeMethod(worker_, "open", Q_ARG(QString, path));
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
    } else {
        playClock_->stop();
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
    QMetaObject::invokeMethod(worker_, "requestFrame", Q_ARG(double, seconds));
}

void MainWindow::restoreLayout() {
    QSettings settings;
    restoreGeometry(settings.value("main/geometry").toByteArray());
    restoreState(settings.value("main/state").toByteArray());
}

void MainWindow::saveLayout() const {
    QSettings settings;
    settings.setValue("main/geometry", saveGeometry());
    settings.setValue("main/state", saveState());
}

} // namespace fc
