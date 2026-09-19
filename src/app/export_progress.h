#pragma once

#include <QDialog>

class QLabel;
class QProgressBar;
class QPushButton;

namespace fc {

// Compact export progress dialog (suggestion #49): one progress bar,
// percent readout, encode rate, ETA and a Cancel button. The
// MainWindow owns the lifetime (no WA_DeleteOnClose) and drives the
// slots from its export job; closing the dialog in any way (reject,
// window close, Cancel) emits cancelRequested() exactly once so the
// exporter's progress callback can bail out.
class ExportProgress : public QDialog {
    Q_OBJECT

public:
    explicit ExportProgress(QWidget *parent = nullptr);

public slots:
    // 0-100; values outside the range are clamped.
    void setProgress(int percent);
    // Observed encode rate, shown as "12.3 fps" ("--" when not finite
    // or negative).
    void setRate(double framesPerSecond);
    // Remaining time, shown as "~ 1 h 5 m left" / "~ 2 m 30 s left" /
    // "~ 45 s left" ("--" for NaN/negative).
    void setEtaSeconds(double seconds);

signals:
    // Emitted once when the user cancels (Cancel button, reject() or a
    // window-manager close). The owner stops the export job.
    void cancelRequested();

protected:
    void closeEvent(QCloseEvent *event) override;
    void reject() override;

private:
    // Emits cancelRequested() exactly once (guard).
    void notifyCancel();

    QProgressBar *bar_ = nullptr;
    QLabel *percent_ = nullptr;
    QLabel *rate_ = nullptr;
    QLabel *eta_ = nullptr;
    bool cancelEmitted_ = false;
};

} // namespace fc
