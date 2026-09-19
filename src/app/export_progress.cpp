#include "export_progress.h"

#include <cmath>

#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

#include "ui_theme.h"

// ui_theme.h tokens live in fc::ui; the panel addresses them as ui::...
using namespace fc;

namespace fc {

namespace {

// "Xh Ym" / "Ym Zs" / "Zs"; NaN/negative => "--" (the caller has no
// trustworthy measurement yet).
QString formatEta(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        return QStringLiteral("--");
    }
    const auto total = static_cast<long long>(std::llround(seconds));
    const long long hours = total / 3600;
    const long long minutes = (total % 3600) / 60;
    const long long secs = total % 60;
    if (hours > 0) {
        return QStringLiteral("%1 h %2 m").arg(hours).arg(minutes);
    }
    if (minutes > 0) {
        return QStringLiteral("%1 m %2 s").arg(minutes).arg(secs);
    }
    return QStringLiteral("%1 s").arg(secs);
}

} // namespace

ExportProgress::ExportProgress(QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("Exporting…"));
    setModal(true);
    setFixedHeight(150); // compact: bar + readouts + cancel, nothing else
    // MainWindow owns the lifetime (it closes/deletes the dialog from
    // finishExport), so WA_DeleteOnClose is deliberately NOT set.

    percent_ = new QLabel(tr("0%"), this);
    percent_->setStyleSheet(
        QStringLiteral("color: %1; font-weight: bold;").arg(ui::color(ui::kAccent).name()));
    percent_->setToolTip(tr("Overall export progress."));

    bar_ = new QProgressBar(this);
    bar_->setRange(0, 100);
    bar_->setValue(0);
    bar_->setToolTip(tr("Renders the timeline through the full program pipeline."));

    rate_ = new QLabel(QStringLiteral("--"), this);
    rate_->setStyleSheet(QStringLiteral("color: %1;").arg(ui::color(ui::kTextDim).name()));
    rate_->setToolTip(tr("Encode speed in frames per second."));

    eta_ = new QLabel(QStringLiteral("--"), this);
    eta_->setStyleSheet(QStringLiteral("color: %1;").arg(ui::color(ui::kTextDim).name()));
    eta_->setToolTip(tr("Estimated time until the export finishes."));

    auto *cancel = new QPushButton(tr("Cancel"), this);
    cancel->setToolTip(tr("Stops the export; already written output may be incomplete."));
    cancel->setMinimumHeight(28); // comfortable hit target
    connect(cancel, &QPushButton::clicked, this, &ExportProgress::reject);

    auto *readouts = new QHBoxLayout;
    readouts->addWidget(rate_);
    readouts->addStretch(1);
    readouts->addWidget(eta_);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(percent_);
    layout->addWidget(bar_);
    layout->addLayout(readouts);
    layout->addWidget(cancel);
}

void ExportProgress::setProgress(int percent) {
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    bar_->setValue(percent);
    percent_->setText(tr("%1%").arg(percent));
}

void ExportProgress::setRate(double framesPerSecond) {
    if (!std::isfinite(framesPerSecond) || framesPerSecond < 0.0) {
        rate_->setText(QStringLiteral("--"));
        return;
    }
    rate_->setText(tr("%1 fps").arg(QString::number(framesPerSecond, 'f', 1)));
}

void ExportProgress::setEtaSeconds(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        eta_->setText(QStringLiteral("--"));
        return;
    }
    eta_->setText(tr("~ %1 left").arg(formatEta(seconds)));
}

void ExportProgress::notifyCancel() {
    if (cancelEmitted_) {
        return;
    }
    cancelEmitted_ = true;
    emit cancelRequested();
}

void ExportProgress::closeEvent(QCloseEvent *event) {
    notifyCancel();
    event->accept();
}

void ExportProgress::reject() {
    notifyCancel();
    QDialog::reject();
}

} // namespace fc
