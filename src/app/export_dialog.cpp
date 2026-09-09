#include "export_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace fc {

namespace {

struct ResolutionPreset {
    int w;
    int h;
    const char *label;
};

} // namespace

ExportDialog::ExportDialog(double fps, int64_t totalFrames, int sourceWidth, int sourceHeight,
                           QWidget *parent)
    : QDialog(parent), fps_(fps > 1.0 ? fps : 24.0), totalFrames_(totalFrames),
      sourceWidth_(sourceWidth), sourceHeight_(sourceHeight) {
    setWindowTitle(tr("Export Media"));
    setModal(true);

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

    summary_ = new QLabel(this);
    summary_->setWordWrap(true);

    auto *form = new QFormLayout;
    form->addRow(tr("Resolution:"), resolution_);
    form->addRow(tr("Quality:"), quality_);
    form->addRow(QString(), audio_);

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

    connect(resolution_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { updateSummary(); });
    connect(quality_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { updateSummary(); });
    connect(audio_, &QCheckBox::toggled, this, [this](bool) { updateSummary(); });
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

void ExportDialog::updateSummary() {
    const double seconds = static_cast<double>(totalFrames_) / fps_;
    const QString audioNote = includeAudio() ? tr(" (audio mixed)") : QString();
    summary_->setText(tr("%1 frames @ %2 fps = %3 s - %4x%5 H.264 MP4 (effects and "
                         "transitions applied)")
                          .arg(qlonglong(totalFrames_))
                          .arg(QString::number(fps_, 'f', 2))
                          .arg(QString::number(seconds, 'f', 1))
                          .arg(outputWidth())
                          .arg(outputHeight()) +
                      audioNote);
}

} // namespace fc
