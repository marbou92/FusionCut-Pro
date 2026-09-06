#pragma once

#include <QWidget>

class QLineEdit;
class QTreeWidget;

// Pro Mode left panel (tabbed with Project): the M5 effects browser.
// Built from fc::effectCatalog() (the real engine catalog, not a static
// seed list): categories, search filter, and double-click / Apply to
// add an effect instance to the SELECTED timeline clip.
class EffectsPanel : public QWidget {
    Q_OBJECT

public:
    explicit EffectsPanel(QWidget *parent = nullptr);

signals:
    // Emitted on double-click of a catalog leaf or the Apply button.
    // MainWindow attaches a default-parameter instance to the selected
    // clip (status bar explains when no clip is selected).
    void effectAddRequested(const QString &effectId);

private:
    void rebuildTree(const QString &filter);

    QLineEdit *search_ = nullptr;
    QTreeWidget *tree_ = nullptr;
};
