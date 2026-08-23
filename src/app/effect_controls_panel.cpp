#include "effect_controls_panel.h"

#include <QTreeWidget>
#include <QVBoxLayout>

EffectControlsPanel::EffectControlsPanel(QWidget *parent) : QWidget(parent) {
    auto *tree = new QTreeWidget(this);
    tree->setColumnCount(2);
    tree->setHeaderLabels(QStringList() << tr("Parameter") << tr("Value"));
    tree->setEditTriggers(QAbstractItemView::NoEditTriggers);

    auto *motion = new QTreeWidgetItem(tree, QStringList() << tr("Motion") << QString());
    new QTreeWidgetItem(motion, QStringList() << tr("Position") << "0, 0");
    new QTreeWidgetItem(motion, QStringList() << tr("Scale") << "100 %");
    new QTreeWidgetItem(motion, QStringList() << tr("Rotation") << "0.0");
    new QTreeWidgetItem(motion, QStringList() << tr("Opacity") << "100 %");
    tree->expandItem(motion);

    auto *color = new QTreeWidgetItem(tree, QStringList() << tr("Lumetri Color") << QString());
    new QTreeWidgetItem(color, QStringList() << tr("Exposure") << "0.0");
    new QTreeWidgetItem(color, QStringList() << tr("Contrast") << "0.0");
    new QTreeWidgetItem(color, QStringList() << tr("Highlights") << "0.0");
    new QTreeWidgetItem(color, QStringList() << tr("Shadows") << "0.0");
    new QTreeWidgetItem(color, QStringList() << tr("Temperature") << "0.0");
    new QTreeWidgetItem(color, QStringList() << tr("Tint") << "0.0");
    tree->expandItem(color);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->addWidget(tree);
}
