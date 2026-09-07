#pragma once

#include <QWidget>

// Pro Mode bottom panel (tabbed with Timeline): audio mixer shell with
// per-track strips (fader, pan, mute, solo) and a master strip. Wired to
// real audio in ; renders the control surface.
class MixerPanel : public QWidget {
    Q_OBJECT

public:
    explicit MixerPanel(QWidget *parent = nullptr);
};
