#pragma once

#include <QWidget>

#include <vector>

#include "effects.h"

class QLabel;
class QListWidget;
class QPushButton;
class QScrollArea;

// Pro Mode right panel: the M5 effect stack editor for ONE clip.
// MainWindow pushes the selected clip's stack in (setStack); every edit
// (parameter slider, enable toggle, reorder, remove) emits the FULL new
// stack back (stackChanged) - MainWindow writes it into the model and
// re-renders the program monitor instantly from the cached raw frame.
//
// Layout: clip label + instance list (up/down/toggle/remove buttons) +
// parameter controls of the selected instance (slider per Number param,
// checkbox per Boolean param). Sliders update the preview live.
class EffectControlsPanel : public QWidget {
    Q_OBJECT

public:
    explicit EffectControlsPanel(QWidget *parent = nullptr);

    // Shows the stack of the clip with this id (clipId < 0 or an empty
    // stack clears the panel).
    void setStack(int64_t clipId, const std::vector<fc::EffectInstance> &stack);

signals:
    void stackChanged(int64_t clipId, const std::vector<fc::EffectInstance> &stack);

private:
    void rebuildList();
    void rebuildParams();
    void emitStack();

    // Owns the working copy of the stack; MainWindow owns the truth in
    // fc::Clip::effectStack and mirrors edits back through setStack
    // only on selection changes (not per slider tick).
    int64_t clipId_ = -1;
    std::vector<fc::EffectInstance> stack_;

    QLabel *clipLabel_ = nullptr;
    QListWidget *list_ = nullptr;
    QPushButton *upButton_ = nullptr;
    QPushButton *downButton_ = nullptr;
    QPushButton *toggleButton_ = nullptr;
    QPushButton *removeButton_ = nullptr;
    QScrollArea *paramsScroll_ = nullptr;
    QWidget *paramsHost_ = nullptr;
};
