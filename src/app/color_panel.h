#pragma once

#include <QWidget>

#include <string>
#include <utility>
#include <vector>

#include "effects.h"

class QLabel;
class QPushButton;
class QScrollArea;
class QSlider;
class QToolButton;

namespace fc {
class EmptyState;
} // namespace fc

// Pro Mode left panel : the colorist's grade view. One
// combined "Color Correction" instance per clip - the panel edits the
// selected clip's color.corrector (auto-created on first touch) through
// grouped sliders: exposure, contrast, highlights, shadows, saturation,
// vibrance, temperature, tint, hue. Every edit emits the clip's FULL new
// stack (the same protocol Effect Controls uses); MainWindow writes it
// into the model and re-renders the program monitor from the raw cache.
//
// Preset chips (suggestion #40): a row of flat one-click looks above the
// parameter rows - Neutral / Warm / Cool / Film. Each chip applies a
// parameter OFFSET to the working corrector instance (real catalog keys,
// clamped by setParam) and emits through the SAME stackChanged path as
// the sliders; no new model API.
//
// Keyframes: when a corrector param carries a keyframe track (created in
// Effect Controls), the sliders display the resolved value at the current
// clip frame and edits write a keyframe at that frame.
class ColorPanel : public QWidget {
    Q_OBJECT

public:
    explicit ColorPanel(QWidget *parent = nullptr);

    // Shows the clip's grade (clipId < 0 or an empty stack clears the
    // panel to neutral defaults). stack is the clip's FULL effect stack.
    void setClip(int64_t clipId, const std::vector<fc::EffectInstance> &stack);

    // The playhead's position within the selected clip (frames since its
    // start); -1 = outside the clip (static values shown).
    void setClipFrame(int64_t frame);

signals:
    void stackChanged(int64_t clipId, const std::vector<fc::EffectInstance> &stack);

private:
    struct Row {
        std::string key;
        std::string label;
        QSlider *slider = nullptr;
        QLabel *value = nullptr;
    };

    void buildUi();
    void buildChips();
    void refresh();          // push instance values (resolved at clipFrame_) into the sliders
    void emitStack();        // send the working stack to the model
    void updateEmptyState(); // empty-state visibility (#61), driven by clipId_
    fc::EffectInstance *corrector(); // working-copy instance or null
    const fc::EffectInstance *corrector() const;
    // Applies the preset offsets to the working corrector (creating it on
    // first touch); resetToDefaults first restores every catalog default
    // (and clears its keyframe tracks - documented on the chip tooltip).
    void applyChip(const std::vector<std::pair<const char *, double>> &offsets,
                   bool resetToDefaults);

    int64_t clipId_ = -1;
    std::vector<fc::EffectInstance> stack_; // working copy (see Effect Controls)
    int64_t clipFrame_ = -1;

    QLabel *clipLabel_ = nullptr;
    QPushButton *resetButton_ = nullptr;
    std::vector<Row> rows_;

    QToolButton *chipNeutral_ = nullptr;
    QToolButton *chipWarm_ = nullptr;
    QToolButton *chipCool_ = nullptr;
    QToolButton *chipFilm_ = nullptr;
    QWidget *chipsRow_ = nullptr;
    fc::EmptyState *emptyState_ = nullptr;
    QScrollArea *scroll_ = nullptr; // kept for the empty-state visibility swap
};
