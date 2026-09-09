#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;

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

private:
    void updateSummary();

    double fps_ = 24.0;
    int64_t totalFrames_ = 0;
    int sourceWidth_ = 0;
    int sourceHeight_ = 0;

    QComboBox *resolution_ = nullptr;
    QComboBox *quality_ = nullptr;
    QCheckBox *audio_ = nullptr;
    QLabel *summary_ = nullptr;
};

} // namespace fc
