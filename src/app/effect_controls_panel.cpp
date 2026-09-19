#include "effect_controls_panel.h"

#include <QCheckBox>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

#include "effects.h"
#include "ui_theme.h"
#include "ui_widgets.h"

// ui_theme.h tokens live in fc::ui; the panel addresses them as ui::...
using namespace fc;

using fc::EffectInstance;

namespace {

constexpr int kSliderSteps = 1000;

QString formatValue(const fc::EffectParamDescriptor &p, double v) {
    const double span = p.maxValue - p.minValue;
    const int decimals = span > 30.0 ? 1 : 2;
    return QString::number(v, 'f', decimals);
}

// Keyframe mini-lane (#34): an 18px strip under an animated parameter's
// slider. Diamonds = the key points of the param's track over the clip
// frame domain; the thin line = the playhead inside the clip. Dragging
// a diamond retargets its frame, right-click deletes it. All state
// lives in the panel's working stack - the lane reads points through a
// callback at paint time, so refreshes never desynchronize. With a 0
// duration (no clip length known yet) the lane paints as a base line
// and ignores all input (read-only).
class KeyframeLane : public QWidget {
public:
    explicit KeyframeLane(QWidget *parent = nullptr) : QWidget(parent) {
        setFixedHeight(18);
        setMouseTracking(true);
    }

    void setDomain(int64_t duration) {
        duration_ = duration;
        update();
    }
    void setPlayhead(int64_t frame) {
        playhead_ = frame;
        update();
    }

    // Panel-provided hooks (all guarded internally; may be empty).
    std::function<std::vector<std::pair<int64_t, double>>()> readKeys;
    std::function<void(int64_t fromFrame, int64_t toFrame)> moveKey;
    std::function<void(int64_t frame)> deleteKey;

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), ui::color(ui::kSurface2));
        const QRect band = rect().adjusted(2, 2, -2, -2);
        painter.setPen(ui::color(ui::kLine));
        painter.drawLine(band.left(), band.center().y(), band.right(), band.center().y());
        if (duration_ <= 0) {
            return;
        }
        if (readKeys) {
            for (const auto &k : readKeys()) {
                const int x = xFor(k.first, band);
                QPolygon diamond;
                diamond << QPoint(x, band.center().y() - 5) << QPoint(x + 5, band.center().y())
                        << QPoint(x, band.center().y() + 5) << QPoint(x - 5, band.center().y());
                painter.setPen(ui::color(ui::kAccent));
                painter.setBrush(ui::color(ui::kAccent));
                painter.drawPolygon(diamond);
            }
        }
        if (playhead_ >= 0 && playhead_ <= duration_) {
            const int px = xFor(playhead_, band);
            painter.setPen(ui::color(ui::kText));
            painter.drawLine(px, band.top(), px, band.bottom());
        }
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (duration_ <= 0 || !readKeys) {
            return;
        }
        const QRect band = rect().adjusted(2, 2, -2, -2);
        if (event->button() == Qt::RightButton && deleteKey) {
            for (const auto &k : readKeys()) {
                if (std::abs(xFor(k.first, band) - event->pos().x()) <= 5) {
                    deleteKey(k.first);
                    return;
                }
            }
            return;
        }
        if (event->button() == Qt::LeftButton && moveKey) {
            for (const auto &k : readKeys()) {
                if (std::abs(xFor(k.first, band) - event->pos().x()) <= 5) {
                    dragging_ = true;
                    dragFrom_ = k.first;
                    grabOffset_ = k.first - frameFor(event->pos().x(), band);
                    return;
                }
            }
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        if (!dragging_ || !moveKey) {
            return;
        }
        const QRect band = rect().adjusted(2, 2, -2, -2);
        int64_t target = frameFor(event->pos().x(), band) + grabOffset_;
        target = std::min(std::max<int64_t>(0, target), duration_ - 1);
        if (target != dragFrom_) {
            moveKey(dragFrom_, target);
            dragFrom_ = target;
        }
    }

    void mouseReleaseEvent(QMouseEvent *) override { dragging_ = false; }

private:
    int xFor(int64_t frame, const QRect &band) const {
        return band.left() +
               static_cast<int>(std::llround(static_cast<double>(frame) /
                                             static_cast<double>(duration_) * band.width()));
    }
    int64_t frameFor(int x, const QRect &band) const {
        if (band.width() <= 0 || duration_ <= 0) {
            return 0;
        }
        double t = static_cast<double>(x - band.left()) / band.width();
        t = std::min(std::max(t, 0.0), 1.0);
        return static_cast<int64_t>(std::llround(t * static_cast<double>(duration_ - 1)));
    }

    int64_t duration_ = 0;
    int64_t playhead_ = -1;
    bool dragging_ = false;
    int64_t dragFrom_ = 0;
    int64_t grabOffset_ = 0;
};

} // namespace

EffectControlsPanel::EffectControlsPanel(QWidget *parent) : QWidget(parent) {
    // ---- Stack editor page ----
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

    // ---- Transition editor page ----
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

    // Empty-target overlay (#61): the pages hide behind it while no
    // clip AND no transition is being edited.
    emptyState_ = new fc::EmptyState(pages_);
    emptyState_->setGlyph(QString::fromUtf8("\u25AF"));
    emptyState_->setTitle(tr("Select a clip to edit its effects"));
    emptyState_->setHint(tr("Effect stacks, keyframes, transitions and audio fades "
                            "appear here"));
    emptyState_->hide();
    pages_->installEventFilter(this);
    // ---- Audio fade editor page ----
    fadesPage_ = new QWidget(this);
    fadesLabel_ = new QLabel(tr("No audio clip selected"), fadesPage_);
    fadesLabel_->setWordWrap(true);
    fadeInSpin_ = new QSpinBox(fadesPage_);
    fadeInSpin_->setRange(0, 1);
    fadeOutSpin_ = new QSpinBox(fadesPage_);
    fadeOutSpin_->setRange(0, 1);
    fadeOutSpin_->setValue(0);
    fadeInSpin_->setValue(0);
    auto *fadeInRow = new QWidget(fadesPage_);
    auto *inLayout = new QHBoxLayout(fadeInRow);
    inLayout->setContentsMargins(0, 0, 0, 0);
    inLayout->addWidget(new QLabel(tr("Fade in (frames):"), fadeInRow));
    inLayout->addWidget(fadeInSpin_, 1);
    auto *fadeOutRow = new QWidget(fadesPage_);
    auto *outLayout = new QHBoxLayout(fadeOutRow);
    outLayout->setContentsMargins(0, 0, 0, 0);
    outLayout->addWidget(new QLabel(tr("Fade out (frames):"), fadeOutRow));
    outLayout->addWidget(fadeOutSpin_, 1);
    auto *fadesNote = new QLabel(
        tr("Linear ramps at the clip's own head and tail - click-free cuts and simple audio "
           "dissolves. Applied identically in the preview and the export mix."),
        fadesPage_);
    fadesNote->setWordWrap(true);
    fadesNote->setStyleSheet("QLabel { color: #999; }");
    auto *fadesLayout = new QVBoxLayout(fadesPage_);
    fadesLayout->setContentsMargins(8, 8, 8, 8);
    fadesLayout->addWidget(fadesLabel_);
    fadesLayout->addWidget(fadeInRow);
    fadesLayout->addWidget(fadeOutRow);
    fadesLayout->addWidget(fadesNote);
    fadesLayout->addStretch(1);
    connect(fadeInSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int frames) {
        if (clipId_ > 0) {
            emit audioFadesChanged(clipId_, int64_t(frames), int64_t(fadeOutSpin_->value()));
        }
    });
    connect(fadeOutSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int frames) {
        if (clipId_ > 0) {
            emit audioFadesChanged(clipId_, int64_t(fadeInSpin_->value()), int64_t(frames));
        }
    });

    pages_->addWidget(stackPage_);
    pages_->addWidget(transitionPage_);
    pages_->addWidget(fadesPage_);

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

void EffectControlsPanel::setStack(int64_t clipId, const std::vector<fc::EffectInstance> &stack,
                                   int64_t durationFrames) {
    clipId_ = clipId;
    stack_ = stack;
    durationFrames_ = durationFrames < 0 ? 0 : durationFrames;
    clipLabel_->setText(clipId < 0 ? tr("No clip selected")
                                   : tr("Clip %1 - effect stack").arg(qlonglong(clipId)));
    rebuildList();
    if (!stack_.empty()) {
        list_->setCurrentRow(0);
    }
    rebuildParams();
    pages_->setCurrentWidget(stackPage_);
    updateEmptyState();
}

void EffectControlsPanel::setAudioFades(int64_t clipId, int64_t fadeIn, int64_t fadeOut,
                                        int64_t maxFrames) {
    clipId_ = clipId;
    if (fadeIn < 0) {
        fadeIn = 0;
    }
    if (fadeOut < 0) {
        fadeOut = 0;
    }
    if (maxFrames < 1) {
        maxFrames = 1;
    }
    const int hi = int(std::min<int64_t>(maxFrames, 100000));
    fadeInSpin_->setRange(0, hi);
    fadeOutSpin_->setRange(0, hi);
    if (clipId < 0) {
        fadesLabel_->setText(tr("No audio clip selected"));
        fadeInSpin_->blockSignals(true);
        fadeInSpin_->setValue(0);
        fadeInSpin_->blockSignals(false);
        fadeOutSpin_->blockSignals(true);
        fadeOutSpin_->setValue(0);
        fadeOutSpin_->blockSignals(false);
        pages_->setCurrentWidget(fadesPage_);
        updateEmptyState();
        return;
    }
    fadesLabel_->setText(tr("Clip %1 - audio fades").arg(qlonglong(clipId)));
    fadeInSpin_->blockSignals(true);
    fadeInSpin_->setValue(int(std::min(fadeIn, int64_t(hi))));
    fadeInSpin_->blockSignals(false);
    fadeOutSpin_->blockSignals(true);
    fadeOutSpin_->setValue(int(std::min(fadeOut, int64_t(hi))));
    fadeOutSpin_->blockSignals(false);
    pages_->setCurrentWidget(fadesPage_);
    updateEmptyState();
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
        updateEmptyState();
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
        QWidget *laneForForm = nullptr; // the row's keyframe lane, if any
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
            const int currentRowCapture = row;
            connect(box, &QCheckBox::toggled, this, [this, idx, currentRowCapture](bool on) {
                // the SAME guard the slider/diamond paths use:
                // only the row this widget was built for may
                // write (a stale row after a rebuild, or a
                // cleared list with currentRow() == -1, must
                // never index the stack).
                if (currentRowCapture != list_->currentRow() ||
                    currentRowCapture >= static_cast<int>(stack_.size())) {
                    return;
                }
                EffectInstance &fx = stack_[static_cast<size_t>(currentRowCapture)];
                if (idx >= fx.values.size()) {
                    return;
                }
                fx.values[idx] = on ? 1.0 : 0.0;
                emitStack();
            });
            rowLayout->addWidget(name);
            rowLayout->addWidget(box, 1);
        } else {
            auto *slider = new QSlider(Qt::Horizontal, rowWidget);
            slider->setRange(0, kSliderSteps);
            // A degenerate zero-span descriptor (max == min) would
            // divide by zero here; treat it as t = 0 (slider parks at
            // the minimum).
            const double span = p.maxValue - p.minValue;
            const double t = span > 0.0 ? (value - p.minValue) / span : 0.0;
            slider->setValue(static_cast<int>(std::lround(t * kSliderSteps)));
            auto *valueLabel = new QLabel(formatValue(p, value), rowWidget);
            valueLabel->setMinimumWidth(56);

            // the keyframe diamond. Toggles the keyframe at
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

            // Reset chip (#35): visible only while the resolved value
            // differs from the catalog default; the click reverts the
            // static value AND drops the keyframe track (a full reset).
            auto *reset = new QToolButton(rowWidget);
            reset->setText(QString::fromUtf8("\u21BA"));
            reset->setToolTip(tr("Reset to the catalog default (clears keyframes)"));
            reset->setAccessibleName(reset->toolTip());
            reset->setAutoRaise(true);
            reset->setFixedSize(16, 16);
            const double defaultValue = p.defaultValue;
            reset->setVisible(std::fabs(value - defaultValue) > 1e-9);
            connect(reset, &QToolButton::clicked, this,
                    [this, idx, currentRowCapture, p, defaultValue]() {
                        if (currentRowCapture != list_->currentRow() ||
                            currentRowCapture >= static_cast<int>(stack_.size())) {
                            return;
                        }
                        EffectInstance &fx2 = stack_[static_cast<size_t>(currentRowCapture)];
                        if (idx >= fx2.values.size()) {
                            return;
                        }
                        fx2.setParam(p.key, defaultValue);
                        fx2.clearKeyframes(p.key);
                        refreshParamValues();
                        emitStack();
                    });
            rowLayout->addWidget(reset);

            // Keyframe mini-lane (#34): diamonds over the clip's frame
            // domain; read-only until a duration arrives via setStack.
            auto *lane = new KeyframeLane(paramsHost_);
            lane->setDomain(durationFrames_);
            lane->setPlayhead(clipFrame_);
            lane->readKeys = [this, currentRowCapture, p]() {
                std::vector<std::pair<int64_t, double>> out;
                if (currentRowCapture < 0 || currentRowCapture >= static_cast<int>(stack_.size())) {
                    return out;
                }
                const EffectInstance &fx3 = stack_[static_cast<size_t>(currentRowCapture)];
                if (const std::vector<fc::EffectKeyframe> *track = fx3.keyframeTrack(p.key)) {
                    for (const fc::EffectKeyframe &k : *track) {
                        out.push_back({k.frame, k.value});
                    }
                }
                return out;
            };
            lane->moveKey = [this, currentRowCapture, p](int64_t fromFrame, int64_t toFrame) {
                if (currentRowCapture != list_->currentRow() ||
                    currentRowCapture >= static_cast<int>(stack_.size())) {
                    return;
                }
                EffectInstance &fx3 = stack_[static_cast<size_t>(currentRowCapture)];
                if (const fc::EffectKeyframe *pt = fx3.keyframeAt(p.key, fromFrame)) {
                    const double v = pt->value;
                    fx3.setKeyframe(p.key, toFrame, v);
                    if (toFrame != fromFrame) {
                        fx3.removeKeyframe(p.key, fromFrame);
                    }
                    refreshParamValues();
                    emitStack();
                }
            };
            lane->deleteKey = [this, currentRowCapture, p](int64_t frame) {
                if (currentRowCapture != list_->currentRow() ||
                    currentRowCapture >= static_cast<int>(stack_.size())) {
                    return;
                }
                EffectInstance &fx3 = stack_[static_cast<size_t>(currentRowCapture)];
                if (fx3.removeKeyframe(p.key, frame)) {
                    refreshParamValues();
                    emitStack();
                }
            };

            ParamRow liveRow;
            liveRow.key = p.key;
            liveRow.minValue = p.minValue;
            liveRow.maxValue = p.maxValue;
            liveRow.slider = slider;
            liveRow.value = valueLabel;
            liveRow.keyframe = diamond;
            liveRow.defaultValue = defaultValue;
            liveRow.reset = reset;
            liveRow.lane = lane;
            laneForForm = lane;
            rows_.push_back(liveRow);
        }
        form->addWidget(rowWidget);
        if (laneForForm) {
            form->addWidget(laneForForm);
        }
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
        // Same zero-span guard as the build path above (max == min
        // descriptors read as t = 0 instead of dividing by zero).
        const double span = r.maxValue - r.minValue;
        const double t = span > 0.0 ? (v - r.minValue) / span : 0.0;
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
        if (r.reset) {
            r.reset->setVisible(std::fabs(v - r.defaultValue) > 1e-9);
        }
        if (r.lane) {
            // ParamRow stores the lane as QWidget* because KeyframeLane
            // is a .cpp-local widget the header cannot name; the only
            // assignment is the build path above, so this downcast is
            // safe by construction.
            static_cast<KeyframeLane *>(r.lane)->setPlayhead(kfFrame);
        }
    }
}

void EffectControlsPanel::updateEmptyState() {
    if (!emptyState_) {
        return;
    }
    emptyState_->setGeometry(pages_->rect());
    emptyState_->refresh(clipId_ < 0 && transitionId_ < 0);
    emptyState_->raise();
}

bool EffectControlsPanel::eventFilter(QObject *watched, QEvent *event) {
    // Keep the empty-state overlay covering the pages on resize.
    if (watched == pages_ && event->type() == QEvent::Resize) {
        updateEmptyState();
    }
    return QWidget::eventFilter(watched, event);
}

void EffectControlsPanel::emitStack() {
    if (clipId_ >= 0) {
        emit stackChanged(clipId_, stack_);
    }
}
