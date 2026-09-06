#include "transitions_panel.h"

#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "transitions.h"

namespace {

// UserRole payload: the catalog transition id of a leaf item.
constexpr int kTransitionIdRole = Qt::UserRole;

} // namespace

TransitionsPanel::TransitionsPanel(QWidget *parent) : QWidget(parent) {
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(tr("Search transitions..."));
    search_->setClearButtonEnabled(true);

    tree_ = new QTreeWidget(this);
    tree_->setHeaderLabel(tr("Transitions"));
    tree_->setColumnCount(1);
    rebuildTree(QString());

    auto *apply = new QPushButton(tr("Apply to Selected Clip's Cut"), this);
    apply->setToolTip(tr("Adds the transition to the cut after the clip selected in "
                         "the timeline (or double-click a transition)"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->addWidget(search_);
    layout->addWidget(tree_, 1);
    layout->addWidget(apply);

    connect(search_, &QLineEdit::textChanged, this,
            [this](const QString &text) { rebuildTree(text); });

    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        if (!item || item->data(0, kTransitionIdRole).isNull()) {
            return; // category rows carry no transition id
        }
        emit transitionAddRequested(item->data(0, kTransitionIdRole).toString());
    });

    connect(apply, &QPushButton::clicked, this, [this, apply] {
        QTreeWidgetItem *item = tree_->currentItem();
        if (!item) {
            return;
        }
        // Walk up from a category selection to its first visible child.
        if (item->data(0, kTransitionIdRole).isNull() && item->childCount() > 0) {
            item = item->child(0);
        }
        if (item && !item->data(0, kTransitionIdRole).isNull()) {
            emit transitionAddRequested(item->data(0, kTransitionIdRole).toString());
        }
        Q_UNUSED(apply)
    });
}

void TransitionsPanel::rebuildTree(const QString &filter) {
    tree_->clear();
    const QString needle = filter.trimmed().toLower();

    QTreeWidgetItem *currentCategory = nullptr;
    QString currentCategoryName;
    for (const fc::TransitionDescriptor &d : fc::transitionCatalog()) {
        const QString label = QString::fromStdString(d.label);
        const QString id = QString::fromStdString(d.id);
        const QString category = QString::fromStdString(d.category);
        if (!needle.isEmpty() && !label.toLower().contains(needle) &&
            !id.toLower().contains(needle) && !category.toLower().contains(needle)) {
            continue;
        }
        if (currentCategory == nullptr || category != currentCategoryName) {
            currentCategory = new QTreeWidgetItem(tree_, QStringList(category));
            currentCategory->setFlags(Qt::ItemIsEnabled);
            currentCategoryName = category;
        }
        auto *leaf = new QTreeWidgetItem(currentCategory, QStringList(label));
        leaf->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        leaf->setData(0, kTransitionIdRole, id);
        leaf->setToolTip(
            0, tr("%1 (%2) - double-click to add to the selected clip's cut").arg(label, id));
    }
    tree_->expandAll();
}
