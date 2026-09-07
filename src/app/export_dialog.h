#pragma once

#include <QDialog>

class QComboBox;
class QLabel;

namespace fc {

// the export configuration dialog. Resolution (the source's
// size or fixed presets), quality (CRF), and a summary of what will be
// rendered (frames, duration, effects + transitions included - the exact
// program-monitor pipeline). Runs modeless work: the caller drives the
// actual export and shows its own progress UI.
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

private:
    void updateSummary();

    double fps_ = 24.0;
    int64_t totalFrames_ = 0;
    int sourceWidth_ = 0;
    int sourceHeight_ = 0;

    QComboBox *resolution_ = nullptr;
    QComboBox *quality_ = nullptr;
    QLabel *summary_ = nullptr;
};

} // namespace fc
