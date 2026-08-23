#pragma once

#include <QImage>
#include <QMainWindow>
#include <QTimer>

#include "media_item.h"

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
    void generateProxy(const QString &sourcePath);
    void startPlayback(bool playing);
    void stepFrames(int frames);
    void requestFrameAt(double seconds);

    // Restore/save panel layout.
    void restoreLayout();
    void saveLayout() const;

    DecodeWorker *worker_ = nullptr;
    QThread *decodeThread_ = nullptr;
    QTimer *playClock_;

    // Pro Mode widgets.
    ProjectPanel *projectPanel_ = nullptr;
    EffectsPanel *effectsPanel_ = nullptr;
    TimelinePanel *timeline_ = nullptr;
    MixerPanel *mixer_ = nullptr;
    EffectControlsPanel *effectControls_ = nullptr;
    PreviewCanvas *sourceCanvas_ = nullptr;
    PreviewCanvas *programCanvas_ = nullptr;
    TransportBar *transport_ = nullptr;
    QStackedWidget *pages_ = nullptr;

    // Quick Mode widgets.
    QuickModeView *quickView_ = nullptr;

    // Playback state.
    QString loadedPath_;
    QString proxySourcePath_;
    double playhead_ = 0.0;
    double duration_ = 0.0;
    double fps_ = 24.0;
    bool playing_ = false;
    bool captureThumbnail_ = false;
};

} // namespace fc
