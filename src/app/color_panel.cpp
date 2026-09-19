#include "color_panel.h"

#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>
#include <utility>

#include "ui_theme.h"
#include "ui_widgets.h"

namespace {

constexpr int kSliderSteps = 1000;
constexpr int kChipMinHeight = 28; // >= 28 px hit targets on small buttons (#63)

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

// Preset chip offsets (suggestion #40) - small, tasteful deltas on real
// catalog keys. setParam() clamps into the descriptor range, so repeated
// clicks saturate instead of running away.
const std::vector<std::pair<const char *, double>> kWarmOffsets = {
    {"temperature", 0.20}, // toward warm (the corrector scales this by 40 luma units)
    {"tint", 0.05},        // a whisper of magenta warmth
    {"exposure", 0.04},
};
const std::vector<std::pair<const char *, double>> kCoolOffsets = {
    {"temperature", -0.20}, // the inverse of the warm look
    {"tint", -0.05},        // a whisper of green coolness
    {"exposure", -0.04},
};
const std::vector<std::pair<const char *, double>> kFilmOffsets = {
    {"contrast", 0.15},    // heavier blacks
    {"saturation", -0.12}, // muted, print-like color
    {"exposure", -0.06},   // slight underexposure
};

} // namespace

ColorPanel::ColorPanel(QWidget *parent) : QWidget(parent) {
    buildUi();
    buildChips();
    updateEmptyState();
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

    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(true);
    scroll_->setWidget(host);

    resetButton_ = new QPushButton(tr("Reset Grade"), this);
    resetButton_->setToolTip(tr("Removes the color correction from this clip"));
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
    layout->addWidget(scroll_, 1);
    layout->addWidget(resetButton_);

    refresh();
}

// Preset chips (suggestion #40): flat one-click looks above the param
// rows. They mutate ONLY the working corrector instance and emit through
// the existing emitStack() path - the model/edit protocol is untouched.
void ColorPanel::buildChips() {
    chipsRow_ = new QWidget(this);
    auto *rowLayout = new QHBoxLayout(chipsRow_);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(4);

    struct ChipSpec {
        const char *label;
        QToolButton **slot;
        QString tip;
    };
    const ChipSpec specs[] = {
        {"Neutral", &chipNeutral_,
         tr("Resets every grade parameter to its catalog default "
            "(keyframe tracks are cleared as well)")},
        {"Warm", &chipWarm_, tr("Pushes temperature/tint warm with a touch of exposure")},
        {"Cool", &chipCool_, tr("Pushes temperature/tint cool with a touch of underexposure")},
        {"Film", &chipFilm_, tr("Filmic response: more contrast, muted color, slightly darker")},
    };
    for (const ChipSpec &spec : specs) {
        auto *chip = new QToolButton(chipsRow_);
        chip->setText(tr(spec.label));
        chip->setToolTip(spec.tip);
        chip->setAccessibleName(tr("%1 color preset").arg(tr(spec.label)));
        chip->setFlat(true);
        chip->setMinimumHeight(kChipMinHeight);
        rowLayout->addWidget(chip);
        *spec.slot = chip;
    }
    rowLayout->addStretch(1);

    connect(chipNeutral_, &QToolButton::clicked, this, [this] { applyChip({}, true); });
    connect(chipWarm_, &QToolButton::clicked, this, [this] { applyChip(kWarmOffsets, false); });
    connect(chipCool_, &QToolButton::clicked, this, [this] { applyChip(kCoolOffsets, false); });
    connect(chipFilm_, &QToolButton::clicked, this, [this] { applyChip(kFilmOffsets, false); });

    // The chips sit directly above the parameter rows.
    auto *panelLayout = layout();
    if (panelLayout) {
        qobject_cast<QVBoxLayout *>(panelLayout)->insertWidget(1, chipsRow_);
    }
}

void ColorPanel::applyChip(const std::vector<std::pair<const char *, double>> &offsets,
                           bool resetToDefaults) {
    if (clipId_ < 0) {
        return;
    }
    fc::EffectInstance *fx = corrector();
    if (resetToDefaults) {
        if (!fx) {
            return; // nothing to neutralize (an absent grade IS neutral)
        }
        const fc::EffectDescriptor *d = fc::findEffect(kCorrectorId);
        if (!d) {
            return;
        }
        for (const fc::EffectParamDescriptor &p : d->params) {
            fx->setParam(p.key, p.defaultValue);
            fx->clearKeyframes(p.key); // a neutral grade has no animation
        }
    } else {
        if (!fx) {
            // First touch: create the grade, then offset it from defaults.
            stack_.push_back(fc::makeEffectInstance(kCorrectorId));
            fx = &stack_.back();
        }
        for (const auto &off : offsets) {
            fx->setParam(off.first, fx->param(off.first) + off.second); // clamped by setParam
        }
    }
    refresh();
    emitStack();
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
    updateEmptyState();
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

// Empty state (#61): when no clip is selected the editing affordances
// step aside for the hint. Driven from the existing setClip path.
void ColorPanel::updateEmptyState() {
    const bool empty = clipId_ < 0;
    if (!emptyState_) {
        emptyState_ = new fc::EmptyState(this);
        emptyState_->setGlyph(tr("\u25D0")); // half-filled circle: the grade split
        emptyState_->setTitle(tr("Select a clip to grade"));
        emptyState_->setHint(tr("Click a video clip in the timeline - its color correction "
                                "appears here."));
        auto *panelLayout = layout();
        if (panelLayout) {
            qobject_cast<QVBoxLayout *>(panelLayout)->insertWidget(1, emptyState_);
        }
    }
    emptyState_->refresh(empty);
    scroll_->setVisible(!empty);
    chipsRow_->setVisible(!empty);
    resetButton_->setVisible(!empty);
}

void ColorPanel::emitStack() {
    if (clipId_ >= 0) {
        emit stackChanged(clipId_, stack_);
    }
}
