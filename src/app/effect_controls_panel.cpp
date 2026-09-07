#include "effect_controls_panel.h"

#include <QCheckBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "effects.h"

#include <cmath>

using fc::EffectInstance;

namespace {

constexpr int kSliderSteps = 1000;

QString formatValue(const fc::EffectParamDescriptor &p, double v) {
    const double span = p.maxValue - p.minValue;
    const int decimals = span > 30.0 ? 1 : 2;
    return QString::number(v, 'f', decimals);
}

} // namespace

EffectControlsPanel::EffectControlsPanel(QWidget *parent) : QWidget(parent) {
    // ---- Stack editor page (M5 Phase 1) ----
    stackPage_ = new QWidget(this);
    clipLabel_ = new QLabel(tr("No clip selected"), stackPage_);
    clipLabel_->setWordWrap(true);

    list_ = new QListWidget(stackPage_);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);

    upButton_ = new QPushButton(tr("Up"), stackPage_);
    downButton_ = new QPushButton(tr("Down"), stackPage_);
    toggleButton_ = new QPushButton(tr("On/Off"), stackPage_);
    removeButton_ = new QPushButton(tr("Remove"), stackPage_);
    auto *buttonRow = new QWidget(stackPage_);
    auto *buttonLayout = new QHBoxLayout(buttonRow);
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->addWidget(upButton_);
    buttonLayout->addWidget(downButton_);
    buttonLayout->addWidget(toggleButton_);
    buttonLayout->addWidget(removeButton_);

    paramsHost_ = new QWidget(stackPage_);
    paramsScroll_ = new QScrollArea(stackPage_);
    paramsScroll_->setWidgetResizable(true);
    paramsScroll_->setWidget(paramsHost_);

    auto *stackLayout = new QVBoxLayout(stackPage_);
    stackLayout->setContentsMargins(0, 0, 0, 0);
    stackLayout->addWidget(clipLabel_);
    stackLayout->addWidget(list_, 1);
    stackLayout->addWidget(buttonRow);
    stackLayout->addWidget(paramsScroll_, 2);

    // ---- Transition editor page (M5 Phase 2) ----
    transitionPage_ = new QWidget(this);
    transitionLabel_ = new QLabel(tr("No transition selected"), transitionPage_);
    transitionLabel_->setWordWrap(true);
    transitionPair_ = new QLabel(QString(), transitionPage_);
    transitionPair_->setWordWrap(true);
    durationSlider_ = new QSlider(Qt::Horizontal, transitionPage_);
    durationSlider_->setRange(1, 1);
    durationValue_ = new QLabel(QString(), transitionPage_);
    transitionRemove_ = new QPushButton(tr("Remove Transition"), transitionPage_);

    auto *durRow = new QWidget(transitionPage_);
    auto *durLayout = new QHBoxLayout(durRow);
    durLayout->setContentsMargins(0, 0, 0, 0);
    auto *durName = new QLabel(tr("Duration:"), durRow);
    durName->setMinimumWidth(90);
    durLayout->addWidget(durName);
    durLayout->addWidget(durationSlider_, 1);
    durLayout->addWidget(durationValue_);

    auto *transLayout = new QVBoxLayout(transitionPage_);
    transLayout->setContentsMargins(0, 0, 0, 0);
    transLayout->addWidget(transitionLabel_);
    transLayout->addWidget(transitionPair_);
    transLayout->addWidget(durRow);
    transLayout->addStretch(1);
    transLayout->addWidget(transitionRemove_);

    pages_ = new QStackedWidget(this);
    pages_->addWidget(stackPage_);
    pages_->addWidget(transitionPage_);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->addWidget(pages_);

    connect(list_, &QListWidget::currentRowChanged, this, [this](int) { rebuildParams(); });

    connect(upButton_, &QPushButton::clicked, this, [this] {
        const int row = list_->currentRow();
        if (row <= 0 || row >= static_cast<int>(stack_.size())) {
            return;
        }
        std::swap(stack_[static_cast<size_t>(row)], stack_[static_cast<size_t>(row - 1)]);
        rebuildList();
        list_->setCurrentRow(row - 1);
        emitStack();
    });

    connect(downButton_, &QPushButton::clicked, this, [this] {
        const int row = list_->currentRow();
        if (row < 0 || row + 1 >= static_cast<int>(stack_.size())) {
            return;
        }
        std::swap(stack_[static_cast<size_t>(row)], stack_[static_cast<size_t>(row + 1)]);
        rebuildList();
        list_->setCurrentRow(row + 1);
        emitStack();
    });

    connect(toggleButton_, &QPushButton::clicked, this, [this] {
        const int row = list_->currentRow();
        if (row < 0 || row >= static_cast<int>(stack_.size())) {
            return;
        }
        stack_[static_cast<size_t>(row)].enabled = !stack_[static_cast<size_t>(row)].enabled;
        rebuildList();
        list_->setCurrentRow(row);
        emitStack();
    });

    connect(removeButton_, &QPushButton::clicked, this, [this] {
        const int row = list_->currentRow();
        if (row < 0 || row >= static_cast<int>(stack_.size())) {
            return;
        }
        stack_.erase(stack_.begin() + row);
        rebuildList();
        if (row < static_cast<int>(stack_.size())) {
            list_->setCurrentRow(row);
        } else if (!stack_.empty()) {
            list_->setCurrentRow(static_cast<int>(stack_.size()) - 1);
        }
        rebuildParams();
        emitStack();
    });

    // ---- Transition editor wiring ----
    connect(durationSlider_, &QSlider::valueChanged, this, [this](int frames) {
        if (transitionId_ < 0) {
            return;
        }
        updateDurationLabel();
        emit transitionDurationChanged(transitionId_, frames);
    });
    connect(transitionRemove_, &QPushButton::clicked, this, [this] {
        if (transitionId_ >= 0) {
            emit transitionRemoveRequested(transitionId_);
        }
    });
}

void EffectControlsPanel::setStack(int64_t clipId, const std::vector<fc::EffectInstance> &stack) {
    clipId_ = clipId;
    stack_ = stack;
    clipLabel_->setText(clipId < 0 ? tr("No clip selected")
                                   : tr("Clip %1 - effect stack").arg(qlonglong(clipId)));
    rebuildList();
    if (!stack_.empty()) {
        list_->setCurrentRow(0);
    }
    rebuildParams();
    pages_->setCurrentWidget(stackPage_);
}

void EffectControlsPanel::setClipFrame(int64_t frame) {
    if (clipFrame_ == frame || rows_.empty()) {
        clipFrame_ = frame;
        return;
    }
    clipFrame_ = frame;
    refreshParamValues();
}

void EffectControlsPanel::setTransition(int64_t transitionId, const QString &kindLabel,
                                        int64_t durationFrames, int64_t maxDurationFrames,
                                        const QString &pairLabel, double fps) {
    transitionId_ = transitionId;
    transitionFps_ = fps > 1.0 ? fps : 24.0;
    if (transitionId < 0) {
        transitionLabel_->setText(tr("No transition selected"));
        transitionPair_->clear();
        durationSlider_->setRange(1, 1);
        durationSlider_->setValue(1);
        durationValue_->clear();
        pages_->setCurrentWidget(transitionPage_);
        return;
    }
    transitionMaxFrames_ = maxDurationFrames < 1 ? 1 : maxDurationFrames;
    transitionLabel_->setText(tr("Transition: %1").arg(kindLabel));
    transitionPair_->setText(tr("Cut: %1").arg(pairLabel));
    durationSlider_->blockSignals(true);
    durationSlider_->setRange(1, static_cast<int>(transitionMaxFrames_));
    const int clamped = static_cast<int>(
        durationFrames < 1
            ? 1
            : (durationFrames > transitionMaxFrames_ ? transitionMaxFrames_ : durationFrames));
    durationSlider_->setValue(clamped);
    durationSlider_->blockSignals(false);
    updateDurationLabel();
    pages_->setCurrentWidget(transitionPage_);
}

void EffectControlsPanel::updateDurationLabel() {
    if (transitionId_ < 0) {
        durationValue_->clear();
        return;
    }
    const int64_t frames = durationSlider_->value();
    durationValue_->setText(
        tr("%1 f (%2 s)")
            .arg(qlonglong(frames))
            .arg(QString::number(static_cast<double>(frames) / transitionFps_, 'f', 2)));
}

void EffectControlsPanel::rebuildList() {
    list_->blockSignals(true);
    list_->clear();
    for (const EffectInstance &fx : stack_) {
        const fc::EffectDescriptor *d = fx.descriptor();
        QString label = d ? QString::fromStdString(d->label) : QString::fromStdString(fx.effectId);
        if (fx.effectId.empty()) {
            label = tr("(unknown effect)");
        } else if (!fx.enabled) {
            label += tr(" (off)");
        }
        list_->addItem(label);
    }
    list_->blockSignals(false);
}

void EffectControlsPanel::rebuildParams() {
    // Clear the old parameter widgets (and the live row registry).
    rows_.clear();
    delete paramsHost_;
    paramsHost_ = new QWidget(this);
    paramsScroll_->setWidget(paramsHost_);
    auto *form = new QVBoxLayout(paramsHost_);
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(6);

    const int row = list_->currentRow();
    if (row < 0 || row >= static_cast<int>(stack_.size())) {
        auto *hint = new QLabel(stack_.empty() ? tr("No effects on this clip.")
                                               : tr("Select an effect above."),
                                paramsHost_);
        hint->setWordWrap(true);
        form->addWidget(hint);
        form->addStretch(1);
        return;
    }

    EffectInstance &fx = stack_[static_cast<size_t>(row)];
    const fc::EffectDescriptor *d = fx.descriptor();
    if (!d) {
        auto *hint = new QLabel(
            tr("Unknown effect id: %1").arg(QString::fromStdString(fx.effectId)), paramsHost_);
        hint->setWordWrap(true);
        form->addWidget(hint);
        form->addStretch(1);
        return;
    }

    // Keyframe hint: the clip-relative frame the diamond buttons use.
    // (Signal handlers read clipFrame_ LIVE - the playhead moves without
    // a rebuild - so this value only seeds the initial widget state.)
    const int64_t kfFrame = clipFrame_;

    for (size_t i = 0; i < d->params.size(); ++i) {
        const fc::EffectParamDescriptor &p = d->params[i];
        // Ensure the value slot exists (legacy instances).
        if (i >= fx.values.size()) {
            fx.setParam(p.key, p.defaultValue); // resizes + fills defaults
        }
        const double value = (kfFrame >= 0) ? fx.paramAt(p.key, kfFrame) : fx.param(p.key);

        auto *rowWidget = new QWidget(paramsHost_);
        auto *rowLayout = new QHBoxLayout(rowWidget);
        rowLayout->setContentsMargins(0, 0, 0, 0);

        auto *name = new QLabel(QString::fromStdString(p.label) + ":", rowWidget);
        name->setMinimumWidth(90);

        if (p.type == fc::EffectParamType::Boolean) {
            auto *box = new QCheckBox(rowWidget);
            box->setChecked(value >= 0.5);
            const size_t idx = i;
            connect(box, &QCheckBox::toggled, this, [this, idx, box](bool on) {
                if (idx >= stack_[static_cast<size_t>(list_->currentRow())].values.size()) {
                    return;
                }
                stack_[static_cast<size_t>(list_->currentRow())].values[idx] = on ? 1.0 : 0.0;
                Q_UNUSED(box)
                emitStack();
            });
            rowLayout->addWidget(name);
            rowLayout->addWidget(box, 1);
        } else {
            auto *slider = new QSlider(Qt::Horizontal, rowWidget);
            slider->setRange(0, kSliderSteps);
            const double t = (value - p.minValue) / (p.maxValue - p.minValue);
            slider->setValue(static_cast<int>(std::lround(t * kSliderSteps)));
            auto *valueLabel = new QLabel(formatValue(p, value), rowWidget);
            valueLabel->setMinimumWidth(56);

            // M5 Phase 3: the keyframe diamond. Toggles the keyframe at
            // the current clip frame (filled when one exists there).
            auto *diamond = new QPushButton(QString::fromUtf8("\u25C6"), rowWidget);
            diamond->setToolTip(tr("Toggle a keyframe for this parameter at the current "
                                   "playhead position (inside the clip)"));
            diamond->setFlat(true);
            diamond->setFixedWidth(28);
            const bool has = fx.keyframeAt(p.key, kfFrame) != nullptr;
            diamond->setText(has ? QString::fromUtf8("\u25C6") : QString::fromUtf8("\u25C7"));

            const size_t idx = i;
            const int currentRowCapture = row;
            connect(diamond, &QPushButton::clicked, this,
                    [this, idx, currentRowCapture, p, slider, diamond]() {
                        if (currentRowCapture != list_->currentRow() ||
                            currentRowCapture >= static_cast<int>(stack_.size())) {
                            return;
                        }
                        const int64_t frame = clipFrame_; // live playhead-in-clip
                        if (frame < 0) {
                            return; // outside the clip: nothing to key
                        }
                        EffectInstance &efx = stack_[static_cast<size_t>(currentRowCapture)];
                        if (idx >= efx.values.size()) {
                            return;
                        }
                        if (efx.keyframeAt(p.key, frame)) {
                            efx.removeKeyframe(p.key, frame);
                        } else {
                            // Key the CURRENT value (slider position).
                            const double pos = static_cast<double>(slider->value());
                            const double v =
                                p.minValue + pos / kSliderSteps * (p.maxValue - p.minValue);
                            efx.setParam(p.key, v);
                            efx.setKeyframe(p.key, frame, v);
                        }
                        refreshParamValues();
                        emitStack();
                    });

            connect(slider, &QSlider::valueChanged, this,
                    [this, idx, p, slider, valueLabel, currentRowCapture](int sliderPos) {
                        if (currentRowCapture != list_->currentRow() ||
                            currentRowCapture >= static_cast<int>(stack_.size())) {
                            return;
                        }
                        EffectInstance &fx2 = stack_[static_cast<size_t>(currentRowCapture)];
                        if (idx >= fx2.values.size()) {
                            return;
                        }
                        const double v = p.minValue + static_cast<double>(sliderPos) /
                                                          kSliderSteps * (p.maxValue - p.minValue);
                        fx2.values[idx] = v; // static value follows every edit
                        if (fx2.keyframeTrack(p.key) && clipFrame_ >= 0) {
                            // The param is keyframed: edits write the
                            // keyframe at the playhead's frame (live).
                            fx2.setKeyframe(p.key, clipFrame_, v);
                        }
                        valueLabel->setText(formatValue(p, v));
                        Q_UNUSED(slider)
                        emitStack();
                    });
            rowLayout->addWidget(name);
            rowLayout->addWidget(slider, 1);
            rowLayout->addWidget(valueLabel);
            rowLayout->addWidget(diamond);

            ParamRow liveRow;
            liveRow.key = p.key;
            liveRow.minValue = p.minValue;
            liveRow.maxValue = p.maxValue;
            liveRow.slider = slider;
            liveRow.value = valueLabel;
            liveRow.keyframe = diamond;
            rows_.push_back(liveRow);
        }
        form->addWidget(rowWidget);
    }
    form->addStretch(1);
}

void EffectControlsPanel::refreshParamValues() {
    const int row = list_->currentRow();
    if (row < 0 || row >= static_cast<int>(stack_.size()) || rows_.empty()) {
        return;
    }
    const EffectInstance &fx = stack_[static_cast<size_t>(row)];
    const fc::EffectDescriptor *d = fx.descriptor();
    if (!d) {
        return;
    }
    const int64_t kfFrame = clipFrame_;
    for (const ParamRow &r : rows_) {
        // Locate the descriptor param for formatting precision.
        const fc::EffectParamDescriptor *p = nullptr;
        for (const fc::EffectParamDescriptor &cand : d->params) {
            if (cand.key == r.key) {
                p = &cand;
                break;
            }
        }
        if (!p || !r.slider) {
            continue;
        }
        const double v = (kfFrame >= 0) ? fx.paramAt(r.key, kfFrame) : fx.param(r.key);
        const double t = (v - r.minValue) / (r.maxValue - r.minValue);
        r.slider->blockSignals(true);
        r.slider->setValue(static_cast<int>(std::lround(t * kSliderSteps)));
        r.slider->blockSignals(false);
        if (r.value) {
            r.value->setText(formatValue(*p, v));
        }
        if (r.keyframe) {
            r.keyframe->setText(fx.keyframeAt(r.key, kfFrame) ? QString::fromUtf8("\u25C6")
                                                              : QString::fromUtf8("\u25C7"));
        }
    }
}

void EffectControlsPanel::emitStack() {
    if (clipId_ >= 0) {
        emit stackChanged(clipId_, stack_);
    }
}
