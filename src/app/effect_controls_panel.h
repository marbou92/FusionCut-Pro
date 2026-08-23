#pragma once

#include <QWidget>

// Pro Mode right panel: Effect Controls parameter tree (Module 2.1).
// Static transform parameters in M3; real effect/keyframe editing M5.
class EffectControlsPanel : public QWidget {
    Q_OBJECT

public:
    explicit EffectControlsPanel(QWidget *parent = nullptr);
};
