#include "transitions_panel.h"

#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "catalog_tree.h"
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
    QVector<fc::CatalogRow> rows;
    rows.reserve(static_cast<int>(fc::transitionCatalog().size()));
    for (const fc::TransitionDescriptor &d : fc::transitionCatalog()) {
        rows.push_back({QString::fromStdString(d.category), QString::fromStdString(d.label),
                        QString::fromStdString(d.id)});
    }
    fc::rebuildCatalogTree(tree_, rows, filter, [this](const fc::CatalogRow &row) {
        return tr("%1 (%2) - double-click to add to the selected clip's cut")
            .arg(row.label, row.id);
    });
}
