#pragma once

#include <QWidget>

class QEvent;
class QLabel;
class QLineEdit;
class QObject;
class QTimer;
class QTreeWidget;
class TransitionPreviewCard;

// Pro Mode left panel : the transitions catalog browser.
// Mirrors EffectsPanel: a search box filters the catalog by label / id /
// category; double-click or the Apply button requests the transition on
// the OUTGOING cut of the clip selected in the timeline (MainWindow
// resolves the adjacent right neighbor and validates the pair).
// Suggestion #36: a schematic preview card on top shows the hovered /
// selected transition kind; hovering a row plays a QTimer flipbook
// (progress 0 -> 1 over ~1s), selection and mouse-leave stop it.
class TransitionsPanel : public QWidget {
    Q_OBJECT

public:
    explicit TransitionsPanel(QWidget *parent = nullptr);

signals:
    void transitionAddRequested(const QString &kind);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void rebuildTree(const QString &filter);
    void setPreviewKind(const QString &kind, const QString &label, bool animate);

    QLineEdit *search_ = nullptr;
    QTreeWidget *tree_ = nullptr;
    TransitionPreviewCard *preview_ = nullptr;
    QLabel *previewLabel_ = nullptr;
    QLabel *treeHint_ = nullptr; // "no matches" hint inside the viewport
    QTimer *flipTimer_ = nullptr;
    double flipProgress_ = 0.0;
};
