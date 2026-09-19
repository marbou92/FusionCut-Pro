#pragma once

#include <QSet>
#include <QWidget>

class QEvent;
class QLabel;
class QLineEdit;
class QObject;
class QTreeWidget;

// Pro Mode left panel (tabbed with Project): the effects browser.
// Built from fc::effectCatalog() (the real engine catalog, not a static
// seed list): categories, search filter, and double-click / Apply to
// add an effect instance to the SELECTED timeline clip. Favorites
// (suggestion #32) persist under QSettings "effects/favorites" and float
// to the top in a pinned "Favorites" section; toggled from the leaf
// context menu. The tree is a drag SOURCE (suggestion #33): leaves are
// offered as "application/x-fc-effect-id" UTF-8 payloads with
// Qt::MoveAction for the timeline's drop targets.
class EffectsPanel : public QWidget {
    Q_OBJECT

public:
    explicit EffectsPanel(QWidget *parent = nullptr);

signals:
    // Emitted on double-click of a catalog leaf or the Apply button.
    // MainWindow attaches a default-parameter instance to the selected
    // clip (status bar explains when no clip is selected).
    void effectAddRequested(const QString &effectId);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void rebuildTree(const QString &filter);
    void toggleFavorite(const QString &effectId);
    void showLeafContextMenu(const QPoint &pos);

    QLineEdit *search_ = nullptr;
    QTreeWidget *tree_ = nullptr;
    QLabel *treeHint_ = nullptr; // "no matches" hint inside the viewport
    QSet<QString> favorites_;
};
