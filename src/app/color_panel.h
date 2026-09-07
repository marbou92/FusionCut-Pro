#pragma once

#include <QWidget>

#include <string>
#include <vector>

#include "effects.h"

class QLabel;
class QPushButton;
class QSlider;

// Pro Mode left panel : the colorist's grade view. One
// combined "Color Correction" instance per clip - the panel edits the
// selected clip's color.corrector (auto-created on first touch) through
// grouped sliders: exposure, contrast, highlights, shadows, saturation,
// vibrance, temperature, tint, hue. Every edit emits the clip's FULL new
// stack (the same protocol Effect Controls uses); MainWindow writes it
// into the model and re-renders the program monitor from the raw cache.
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
    void refresh();   // push instance values (resolved at clipFrame_) into the sliders
    void emitStack(); // send the working stack to the model
    fc::EffectInstance *corrector(); // working-copy instance or null
    const fc::EffectInstance *corrector() const;

    int64_t clipId_ = -1;
    std::vector<fc::EffectInstance> stack_; // working copy (see Effect Controls)
    int64_t clipFrame_ = -1;

    QLabel *clipLabel_ = nullptr;
    QPushButton *resetButton_ = nullptr;
    std::vector<Row> rows_;
};
