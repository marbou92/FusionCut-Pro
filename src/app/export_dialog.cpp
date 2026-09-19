#include "export_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>

namespace fc {

namespace {

struct ResolutionPreset {
    int w;
    int h;
    const char *label;
};

// Bits per pixel per frame, keyed to the CRF values the quality combo
// actually offers (18 = highest, 23 = balanced default, 28 = compact).
// Rough by design: real H.264 bitrate varies enormously with content.
constexpr double kBppHigh = 0.10;
constexpr double kBppDefault = 0.07;
constexpr double kBppLow = 0.045;
// The include-audio toggle documents a 128 kbps AAC mix.
constexpr double kAudioBitsPerSecond = 128000.0;

} // namespace

ExportDialog::ExportDialog(double fps, int64_t totalFrames, int sourceWidth, int sourceHeight,
                           QWidget *parent)
    : QDialog(parent), fps_(fps > 1.0 ? fps : 24.0), totalFrames_(totalFrames),
      sourceWidth_(sourceWidth), sourceHeight_(sourceHeight) {
    setWindowTitle(tr("Export Media"));
    setModal(true);

    presetBox_ = new QComboBox(this);
    presetBox_->addItem(tr("Custom"), QStringLiteral("custom"));
    presetBox_->addItem(tr("YouTube 1080p"), QStringLiteral("youtube"));
    presetBox_->addItem(tr("Social 9:16"), QStringLiteral("social"));
    presetBox_->addItem(tr("Proxy"), QStringLiteral("proxy"));
    presetBox_->setToolTip(
        tr("Quick presets fill the fields below; any manual change switches back to Custom."));

    resolution_ = new QComboBox(this);
    if (sourceWidth_ > 0 && sourceHeight_ > 0) {
        // Evened down (yuv420p needs even geometry).
        const int w = sourceWidth_ - (sourceWidth_ % 2);
        const int h = sourceHeight_ - (sourceHeight_ % 2);
        resolution_->addItem(tr("Match source (%1x%2)").arg(w).arg(h), QSize(w, h));
    }
    const ResolutionPreset presets[] = {
        {1920, 1080, "1920 x 1080 (Full HD)"},
        {1280, 720, "1280 x 720 (HD)"},
        {854, 480, "854 x 480 (SD)"},
        {640, 360, "640 x 360 (Web)"},
    };
    for (const ResolutionPreset &p : presets) {
        resolution_->addItem(QString::fromUtf8(p.label), QSize(p.w, p.h));
    }
    // Vertical option so the Social 9:16 preset has a real target
    // (portrait exports keep the even-geometry rule too).
    resolution_->addItem(tr("1080 x 1920 (Vertical 9:16)"), QSize(1080, 1920));
    if (sourceWidth_ <= 0) {
        resolution_->setCurrentIndex(1); // 720p default without a source hint
    }

    quality_ = new QComboBox(this);
    quality_->addItem(tr("High quality (CRF 18)"), 18);
    quality_->addItem(tr("Balanced (CRF 23)"), 23);
    quality_->addItem(tr("Compact (CRF 28)"), 28);
    quality_->setCurrentIndex(1);

    audio_ = new QCheckBox(tr("Include audio (timeline mix, AAC)"), this);
    audio_->setChecked(true);
    audio_->setToolTip(tr("Mixes every audio-track clip (track faders, pan, mute/solo, "
                          "clip fades) into an AAC track - the exact audio the preview "
                          "plays."));

    // Output destination (#48): typed path, browse button and the
    // recent-locations menu, remembered across runs via QSettings.
    pathEdit_ = new QLineEdit(this);
    pathEdit_->setPlaceholderText(tr("Choose where to save the MP4..."));
    pathEdit_->setClearButtonEnabled(true);
    pathEdit_->setToolTip(tr("Full path of the exported MP4 file."));

    auto *browse = new QToolButton(this);
    browse->setText(tr("Browse..."));
    browse->setToolTip(tr("Pick the destination file (MP4)."));
    browse->setMinimumHeight(28); // comfortable hit target next to the edit
    connect(browse, &QToolButton::clicked, this, [this] {
        const QString current = pathEdit_->text().trimmed();
        const QString path = QFileDialog::getSaveFileName(this, tr("Export Media"), current,
                                                          tr("MP4 video (*.mp4)"));
        if (!path.isEmpty()) {
            pathEdit_->setText(path);
        }
    });

    recentButton_ = new QToolButton(this);
    recentButton_->setText(tr("Recent"));
    recentButton_->setPopupMode(QToolButton::InstantPopup);
    recentButton_->setToolTip(tr("Reopen one of the last five export destinations."));
    recentButton_->setMinimumHeight(28);
    recentMenu_ = new QMenu(recentButton_);
    recentButton_->setMenu(recentMenu_);
    connect(recentMenu_, &QMenu::triggered, this, [this](QAction *action) {
        const QString path = action->data().toString();
        if (!path.isEmpty()) {
            pathEdit_->setText(path);
        }
    });

    auto *pathRow = new QWidget(this);
    auto *pathLayout = new QHBoxLayout(pathRow);
    pathLayout->setContentsMargins(0, 0, 0, 0);
    pathLayout->setSpacing(6);
    pathLayout->addWidget(pathEdit_, 1);
    pathLayout->addWidget(browse);
    pathLayout->addWidget(recentButton_);

    summary_ = new QLabel(this);
    summary_->setWordWrap(true);

    auto *form = new QFormLayout;
    form->addRow(tr("Preset:"), presetBox_);
    form->addRow(tr("Resolution:"), resolution_);
    form->addRow(tr("Quality:"), quality_);
    form->addRow(QString(), audio_);
    form->addRow(tr("Save to:"), pathRow);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    auto *intro =
        new QLabel(tr("Renders the timeline through the full program pipeline - every effect "
                      "(with keyframes) and every cut transition, frame-accurate at export "
                      "resolution, plus the timeline's audio mix when included."),
                   this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    layout->addLayout(form);
    layout->addWidget(summary_);
    layout->addWidget(buttons);

    connect(presetBox_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) { applyPreset(index); });
    connect(resolution_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        markCustomPreset();
        updateSummary();
    });
    connect(quality_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        markCustomPreset();
        updateSummary();
    });
    connect(audio_, &QCheckBox::toggled, this, [this](bool) {
        markCustomPreset();
        updateSummary();
    });
    // Remember the destination for the Recent menu once the export is
    // actually accepted (an empty path or a Cancel stores nothing).
    connect(this, &QDialog::accepted, this, [this] { rememberOutputPath(); });
    rebuildRecentMenu();
    updateSummary();
}

int ExportDialog::outputWidth() const {
    return resolution_->currentData().toSize().width();
}

int ExportDialog::outputHeight() const {
    return resolution_->currentData().toSize().height();
}

int ExportDialog::crf() const {
    return quality_->currentData().toInt();
}

bool ExportDialog::includeAudio() const {
    return audio_ != nullptr && audio_->isChecked();
}

QString ExportDialog::preset() const {
    // Keep the encode fast enough for 1 GB machines; quality comes from CRF.
    return QStringLiteral("veryfast");
}

QString ExportDialog::outputPath() const {
    return pathEdit_ != nullptr ? pathEdit_->text().trimmed() : QString();
}

void ExportDialog::applyPreset(int index) {
    if (updatingPreset_) {
        return; // preset application in progress, not a user choice
    }
    updatingPreset_ = true;
    switch (index) {
    case 1: // YouTube 1080p: standard landscape full HD, balanced CRF.
        setQualityByCrf(23);
        audio_->setChecked(true);
        for (int i = 0; i < resolution_->count(); ++i) {
            if (resolution_->itemData(i).toSize() == QSize(1920, 1080)) {
                resolution_->setCurrentIndex(i);
                break;
            }
        }
        break;
    case 2: // Social 9:16: portrait full HD for shorts/reels/stories.
        setQualityByCrf(23);
        audio_->setChecked(true);
        for (int i = 0; i < resolution_->count(); ++i) {
            if (resolution_->itemData(i).toSize() == QSize(1080, 1920)) {
                resolution_->setCurrentIndex(i);
                break;
            }
        }
        break;
    case 3: { // Proxy: the smallest resolution offered, no audio.
        int smallest = 0;
        qint64 smallestArea = -1;
        for (int i = 0; i < resolution_->count(); ++i) {
            const QSize size = resolution_->itemData(i).toSize();
            const qint64 area = static_cast<qint64>(size.width()) * size.height();
            if (smallestArea < 0 || area < smallestArea) {
                smallestArea = area;
                smallest = i;
            }
        }
        resolution_->setCurrentIndex(smallest);
        setQualityByCrf(28);
        audio_->setChecked(false);
        break;
    }
    default: // Custom: user-managed fields, nothing to fill.
        break;
    }
    updatingPreset_ = false;
    updateSummary();
}

void ExportDialog::markCustomPreset() {
    if (updatingPreset_ || presetBox_ == nullptr || presetBox_->currentIndex() == 0) {
        return;
    }
    presetBox_->setCurrentIndex(0); // fires applyPreset(0) = no-op
}

void ExportDialog::setQualityByCrf(int crf) {
    for (int i = 0; i < quality_->count(); ++i) {
        if (quality_->itemData(i).toInt() == crf) {
            quality_->setCurrentIndex(i);
            return;
        }
    }
}

void ExportDialog::rebuildRecentMenu() {
    if (recentMenu_ == nullptr || recentButton_ == nullptr) {
        return;
    }
    recentMenu_->clear();
    bool any = false;
    const QStringList paths =
        QSettings().value(QStringLiteral("export/recentPaths")).toStringList();
    for (const QString &path : paths) {
        if (path.isEmpty()) {
            continue;
        }
        QAction *action = recentMenu_->addAction(path);
        action->setData(path);
        any = true;
    }
    recentButton_->setEnabled(any);
}

void ExportDialog::rememberOutputPath() {
    const QString path = outputPath();
    if (path.isEmpty()) {
        return;
    }
    QSettings settings;
    QStringList paths = settings.value(QStringLiteral("export/recentPaths")).toStringList();
    paths.removeAll(path);
    paths.prepend(path);
    while (paths.size() > 5) {
        paths.removeLast();
    }
    settings.setValue(QStringLiteral("export/recentPaths"), paths);
}

void ExportDialog::updateSummary() {
    const double seconds = static_cast<double>(totalFrames_) / fps_;
    const QString audioNote = includeAudio() ? tr(" (audio mixed)") : QString();

    // Rough size estimate (#48): frame size * fps * duration * bits per
    // pixel, keyed to the CRF actually offered, plus the 128 kbps AAC
    // mix when included. Labeled as an estimate on purpose.
    const double crf = quality_->currentData().toInt();
    double bpp = kBppDefault;
    if (crf <= 18) {
        bpp = kBppHigh;
    } else if (crf >= 28) {
        bpp = kBppLow;
    }
    const double bits = static_cast<double>(outputWidth()) * static_cast<double>(outputHeight()) *
                            fps_ * seconds * bpp +
                        (includeAudio() ? kAudioBitsPerSecond * seconds : 0.0);
    const double mb = bits / 8.0 / (1024.0 * 1024.0);
    QString sizeText;
    if (mb >= 1024.0) {
        sizeText = QString::number(mb / 1024.0, 'f', 2) + QStringLiteral(" GB");
    } else {
        sizeText = QString::number(mb, 'f', 1) + QStringLiteral(" MB");
    }

    summary_->setText(tr("%1 frames @ %2 fps = %3 s - %4x%5 H.264 MP4 (effects and "
                         "transitions applied)")
                          .arg(qlonglong(totalFrames_))
                          .arg(QString::number(fps_, 'f', 2))
                          .arg(QString::number(seconds, 'f', 1))
                          .arg(outputWidth())
                          .arg(outputHeight()) +
                      audioNote + tr("\n~ estimated size: %1 (rough estimate)").arg(sizeText));
}

} // namespace fc
