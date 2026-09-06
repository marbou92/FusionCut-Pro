#include "effect_controls_panel.h"

#include <QCheckBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
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
    clipLabel_ = new QLabel(tr("No clip selected"), this);
    clipLabel_->setWordWrap(true);

    list_ = new QListWidget(this);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);

    upButton_ = new QPushButton(tr("Up"), this);
    downButton_ = new QPushButton(tr("Down"), this);
    toggleButton_ = new QPushButton(tr("On/Off"), this);
    removeButton_ = new QPushButton(tr("Remove"), this);
    auto *buttonRow = new QWidget(this);
    auto *buttonLayout = new QHBoxLayout(buttonRow);
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->addWidget(upButton_);
    buttonLayout->addWidget(downButton_);
    buttonLayout->addWidget(toggleButton_);
    buttonLayout->addWidget(removeButton_);

    paramsHost_ = new QWidget(this);
    paramsScroll_ = new QScrollArea(this);
    paramsScroll_->setWidgetResizable(true);
    paramsScroll_->setWidget(paramsHost_);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->addWidget(clipLabel_);
    layout->addWidget(list_, 1);
    layout->addWidget(buttonRow);
    layout->addWidget(paramsScroll_, 2);

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
    // Clear the old parameter widgets.
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

    for (size_t i = 0; i < d->params.size(); ++i) {
        const fc::EffectParamDescriptor &p = d->params[i];
        // Ensure the value slot exists (legacy instances).
        if (i >= fx.values.size()) {
            fx.setParam(p.key, p.defaultValue); // resizes + fills defaults
        }
        const double value = fx.values[i];

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
            const size_t idx = i;
            connect(slider, &QSlider::valueChanged, this,
                    [this, idx, p, slider, valueLabel](int sliderPos) {
                        const int row = list_->currentRow();
                        if (row < 0 || row >= static_cast<int>(stack_.size())) {
                            return;
                        }
                        EffectInstance &fx = stack_[static_cast<size_t>(row)];
                        if (idx >= fx.values.size()) {
                            return;
                        }
                        const double v = p.minValue + static_cast<double>(sliderPos) /
                                                          kSliderSteps * (p.maxValue - p.minValue);
                        fx.values[idx] = v;
                        valueLabel->setText(formatValue(p, v));
                        Q_UNUSED(slider)
                        emitStack();
                    });
            rowLayout->addWidget(name);
            rowLayout->addWidget(slider, 1);
            rowLayout->addWidget(valueLabel);
        }
        form->addWidget(rowWidget);
    }
    form->addStretch(1);
}

void EffectControlsPanel::emitStack() {
    if (clipId_ >= 0) {
        emit stackChanged(clipId_, stack_);
    }
}
