#include "color_panel.h"

#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QVBoxLayout>

#include <cmath>

namespace {

constexpr int kSliderSteps = 1000;

// The corrector is THE Color Panel effect (catalog entry).
constexpr const char *kCorrectorId = "color.corrector";

struct ParamSpec {
    const char *key;
    const char *label;
};

// Descriptor order (matches the catalog).
const ParamSpec kParams[] = {
    {"exposure", "Exposure"},
    {"contrast", "Contrast"},
    {"highlights", "Highlights"},
    {"shadows", "Shadows"},
    {"saturation", "Saturation"},
    {"vibrance", "Vibrance"},
    {"temperature", "Temperature"},
    {"tint", "Tint"},
    {"hue", "Hue"},
};

QString formatValue(double v) {
    return QString::number(v, 'f', 2);
}

} // namespace

ColorPanel::ColorPanel(QWidget *parent) : QWidget(parent) {
    buildUi();
}

void ColorPanel::buildUi() {
    clipLabel_ = new QLabel(tr("No clip selected"), this);
    clipLabel_->setWordWrap(true);

    auto *host = new QWidget(this);
    auto *form = new QVBoxLayout(host);
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(4);

    const fc::EffectDescriptor *d = fc::findEffect(kCorrectorId);
    for (const ParamSpec &spec : kParams) {
        // The panel and the catalog agree on every key/range (unit-tested);
        // find the descriptor entry defensively anyway.
        const fc::EffectParamDescriptor *p = nullptr;
        if (d) {
            for (const fc::EffectParamDescriptor &cand : d->params) {
                if (cand.key == spec.key) {
                    p = &cand;
                    break;
                }
            }
        }

        auto *row = new QWidget(host);
        auto *rowLayout = new QVBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(0);

        auto *name = new QLabel(QString::fromUtf8(spec.label) + ":", row);
        auto *ctrlRow = new QWidget(row);
        auto *ctrlLayout = new QHBoxLayout(ctrlRow);
        ctrlLayout->setContentsMargins(0, 0, 0, 0);
        ctrlLayout->setSpacing(6);
        auto *slider = new QSlider(Qt::Horizontal, ctrlRow);
        slider->setRange(0, kSliderSteps);
        auto *value = new QLabel(ctrlRow);
        value->setMinimumWidth(56);
        ctrlLayout->addWidget(slider, 1);
        ctrlLayout->addWidget(value);

        rowLayout->addWidget(name);
        rowLayout->addWidget(ctrlRow);
        form->addWidget(row);

        Row r;
        r.key = spec.key;
        r.label = spec.label;
        r.slider = slider;
        r.value = value;
        rows_.push_back(r);

        const Row rowRef = r;
        const double minValue = p ? p->minValue : -1.0;
        const double maxValue = p ? p->maxValue : 1.0;
        connect(slider, &QSlider::valueChanged, this, [this, rowRef, minValue, maxValue](int pos) {
            if (clipId_ < 0) {
                return;
            }
            const double v =
                minValue + static_cast<double>(pos) / kSliderSteps * (maxValue - minValue);
            fc::EffectInstance *fx = corrector();
            if (!fx) {
                // First touch: create the grade on top of the stack.
                stack_.push_back(fc::makeEffectInstance(kCorrectorId));
                fx = &stack_.back();
            }
            const bool keyframed = fx->keyframeTrack(rowRef.key) != nullptr;
            fx->setParam(rowRef.key, v); // static value follows every edit
            if (keyframed && clipFrame_ >= 0) {
                fx->setKeyframe(rowRef.key, clipFrame_, v); // edit writes the keyframe
            }
            rowRef.value->setText(formatValue(v));
            emitStack();
        });
    }
    form->addStretch(1);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setWidget(host);

    resetButton_ = new QPushButton(tr("Reset Grade"), this);
    connect(resetButton_, &QPushButton::clicked, this, [this] {
        if (clipId_ < 0) {
            return;
        }
        for (auto it = stack_.begin(); it != stack_.end(); ++it) {
            if (it->effectId == kCorrectorId) {
                stack_.erase(it);
                break;
            }
        }
        refresh();
        emitStack();
    });

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->addWidget(clipLabel_);
    layout->addWidget(scroll, 1);
    layout->addWidget(resetButton_);

    refresh();
}

fc::EffectInstance *ColorPanel::corrector() {
    for (fc::EffectInstance &fx : stack_) {
        if (fx.effectId == kCorrectorId) {
            return &fx;
        }
    }
    return nullptr;
}

const fc::EffectInstance *ColorPanel::corrector() const {
    for (const fc::EffectInstance &fx : stack_) {
        if (fx.effectId == kCorrectorId) {
            return &fx;
        }
    }
    return nullptr;
}

void ColorPanel::setClip(int64_t clipId, const std::vector<fc::EffectInstance> &stack) {
    clipId_ = clipId;
    stack_ = stack;
    clipLabel_->setText(clipId < 0 ? tr("No clip selected")
                                   : tr("Clip %1 - grade").arg(qlonglong(clipId)));
    refresh();
}

void ColorPanel::setClipFrame(int64_t frame) {
    if (clipFrame_ == frame) {
        return;
    }
    clipFrame_ = frame;
    refresh();
}

void ColorPanel::refresh() {
    const fc::EffectInstance *fx = corrector();
    const fc::EffectDescriptor *d = fc::findEffect(kCorrectorId);
    for (Row &r : rows_) {
        const fc::EffectParamDescriptor *p = nullptr;
        if (d) {
            for (const fc::EffectParamDescriptor &cand : d->params) {
                if (cand.key == r.key) {
                    p = &cand;
                    break;
                }
            }
        }
        double v = p ? p->defaultValue : 0.0;
        if (fx) {
            v = (clipFrame_ >= 0) ? fx->paramAt(r.key, clipFrame_) : fx->param(r.key);
        }
        if (p) {
            const double t = (v - p->minValue) / (p->maxValue - p->minValue);
            r.slider->blockSignals(true);
            r.slider->setValue(static_cast<int>(std::lround(t * kSliderSteps)));
            r.slider->blockSignals(false);
        }
        r.value->setText(formatValue(v));
        // A keyframed param reads live at the playhead; dim nothing but
        // hint through the value label.
    }
}

void ColorPanel::emitStack() {
    if (clipId_ >= 0) {
        emit stackChanged(clipId_, stack_);
    }
}
