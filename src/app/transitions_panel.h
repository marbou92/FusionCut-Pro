#pragma once

#include <QWidget>

class QLineEdit;
class QTreeWidget;

// Pro Mode left panel (M5 Phase 2): the transitions catalog browser.
// Mirrors EffectsPanel: a search box filters the catalog by label / id /
// category; double-click or the Apply button requests the transition on
// the OUTGOING cut of the clip selected in the timeline (MainWindow
// resolves the adjacent right neighbor and validates the pair).
class TransitionsPanel : public QWidget {
    Q_OBJECT

public:
    explicit TransitionsPanel(QWidget *parent = nullptr);

signals:
    void transitionAddRequested(const QString &kind);

private:
    void rebuildTree(const QString &filter);

    QLineEdit *search_ = nullptr;
    QTreeWidget *tree_ = nullptr;
};
