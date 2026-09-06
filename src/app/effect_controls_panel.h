#pragma once

#include <QWidget>

#include <vector>

#include "effects.h"

class QLabel;
class QListWidget;
class QPushButton;
class QScrollArea;
class QSlider;
class QStackedWidget;

// Pro Mode right panel: the M5 effect stack editor for ONE clip, plus the
// M5 Phase 2 transition editor. MainWindow pushes the selected clip's
// stack in (setStack) or the selected cut transition in (setTransition);
// the panel shows one of the two editors via a stacked layout.
//
// Stack mode: every edit (parameter slider, enable toggle, reorder,
// remove) emits the FULL new stack back (stackChanged) - MainWindow writes
// it into the model and re-renders the program monitor instantly from the
// cached raw frame.
//
// Transition mode: a live duration slider (1..max frames, clamped by the
// left clip's extent) and a Remove button; every change emits
// transitionDurationChanged, the button emits transitionRemoveRequested.
class EffectControlsPanel : public QWidget {
    Q_OBJECT

public:
    explicit EffectControlsPanel(QWidget *parent = nullptr);

    // Shows the stack of the clip with this id (clipId < 0 or an empty
    // stack clears the panel).
    void setStack(int64_t clipId, const std::vector<fc::EffectInstance> &stack);

    // Shows the transition editor for the cut transition with this id
    // (transitionId < 0 clears the panel). maxDurationFrames comes from
    // TimelineModel::maxTransitionDuration; fps feeds the seconds display.
    void setTransition(int64_t transitionId, const QString &kindLabel, int64_t durationFrames,
                       int64_t maxDurationFrames, const QString &pairLabel, double fps);

signals:
    void stackChanged(int64_t clipId, const std::vector<fc::EffectInstance> &stack);
    void transitionDurationChanged(int64_t transitionId, int64_t durationFrames);
    void transitionRemoveRequested(int64_t transitionId);

private:
    void rebuildList();
    void rebuildParams();
    void emitStack();
    void updateDurationLabel();

    // Stack editor state: owns the working copy of the stack; MainWindow
    // owns the truth in fc::Clip::effectStack and mirrors edits back
    // through setStack only on selection changes (not per slider tick).
    int64_t clipId_ = -1;
    std::vector<fc::EffectInstance> stack_;

    // Transition editor state.
    int64_t transitionId_ = -1;
    int64_t transitionMaxFrames_ = 1;
    double transitionFps_ = 24.0;

    QLabel *clipLabel_ = nullptr;
    QListWidget *list_ = nullptr;
    QPushButton *upButton_ = nullptr;
    QPushButton *downButton_ = nullptr;
    QPushButton *toggleButton_ = nullptr;
    QPushButton *removeButton_ = nullptr;
    QScrollArea *paramsScroll_ = nullptr;
    QWidget *paramsHost_ = nullptr;

    QStackedWidget *pages_ = nullptr;
    QWidget *stackPage_ = nullptr;
    QWidget *transitionPage_ = nullptr;
    QLabel *transitionLabel_ = nullptr;
    QLabel *transitionPair_ = nullptr;
    QSlider *durationSlider_ = nullptr;
    QLabel *durationValue_ = nullptr;
    QPushButton *transitionRemove_ = nullptr;
};
