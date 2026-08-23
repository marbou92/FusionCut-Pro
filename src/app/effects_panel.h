#pragma once

#include <QWidget>

class QTreeWidget;

// Pro Mode left panel (tabbed with Project): categorized effects browser.
// Items are drag-enabled so the timeline (M4) can accept drops; the M3
// timeline does not consume them yet.
class EffectsPanel : public QWidget {
    Q_OBJECT

public:
    explicit EffectsPanel(QWidget *parent = nullptr);
};
