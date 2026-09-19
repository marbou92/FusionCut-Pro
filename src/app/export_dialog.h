#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QMenu;
class QToolButton;

namespace fc {

// the export configuration dialog. Resolution (the source's
// size or fixed presets), quality (CRF), the include-audio toggle,
// and a summary of what will be rendered (frames, duration, effects +
// transitions + audio - the exact program-monitor pipeline). Runs
// modeless work: the caller drives the actual export and shows its
// own progress UI.
class ExportDialog : public QDialog {
    Q_OBJECT

public:
    // sourceWidth/sourceHeight = the suggested "match source" size
    // (<= 0 hides that option).
    explicit ExportDialog(double fps, int64_t totalFrames, int sourceWidth, int sourceHeight,
                          QWidget *parent = nullptr);

    int outputWidth() const;
    int outputHeight() const;
    int crf() const;
    QString preset() const;
    bool includeAudio() const;
    // Destination typed/browsed/picked from Recent (empty when unset).
    QString outputPath() const;

private:
    void updateSummary();
    // Applies the chosen quick preset (index into presetBox_) to the
    // resolution / quality / audio widgets.
    void applyPreset(int index);
    // Any manual widget edit flips the preset combo back to Custom.
    void markCustomPreset();
    void setQualityByCrf(int crf);
    void rebuildRecentMenu();
    void rememberOutputPath();

    double fps_ = 24.0;
    int64_t totalFrames_ = 0;
    int sourceWidth_ = 0;
    int sourceHeight_ = 0;

    QComboBox *presetBox_ = nullptr;
    QComboBox *resolution_ = nullptr;
    QComboBox *quality_ = nullptr;
    QCheckBox *audio_ = nullptr;
    QLineEdit *pathEdit_ = nullptr;
    QToolButton *recentButton_ = nullptr;
    QMenu *recentMenu_ = nullptr;
    QLabel *summary_ = nullptr;
    // Guards against the preset application itself flipping the combo
    // back to Custom (the widget signals fire while the preset fills
    // the fields).
    bool updatingPreset_ = false;
};

} // namespace fc
