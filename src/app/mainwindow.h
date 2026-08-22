#pragma once

#include <QMainWindow>

namespace fc {

// Pre-alpha desktop shell. Hosts the Module 2 menu structure, the FusionCut
// dark theme, and the Pro/Quick workspace toggle placeholder. Dockable
// panels (Pro Mode) and the streamlined timeline (Quick Mode) dock in
// Milestone 3.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private:
    void applyDarkTheme();
    void buildMenus();
    void buildCentralPlaceholder();
    void buildStatusBar();
};

} // namespace fc
