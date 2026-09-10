#include "effects_panel.h"

#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "catalog_tree.h"
#include "effects.h"

namespace {

// UserRole payload: the catalog effect id of a leaf item.
constexpr int kEffectIdRole = Qt::UserRole;

} // namespace

EffectsPanel::EffectsPanel(QWidget *parent) : QWidget(parent) {
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(tr("Search effects..."));
    search_->setClearButtonEnabled(true);

    tree_ = new QTreeWidget(this);
    tree_->setHeaderLabel(tr("Effects"));
    tree_->setColumnCount(1);
    rebuildTree(QString());

    auto *apply = new QPushButton(tr("Apply to Selected Clip"), this);
    apply->setToolTip(tr("Adds the selected effect to the clip selected in "
                         "the timeline (or double-click an effect)"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->addWidget(search_);
    layout->addWidget(tree_, 1);
    layout->addWidget(apply);

    connect(search_, &QLineEdit::textChanged, this,
            [this](const QString &text) { rebuildTree(text); });

    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        if (!item || item->data(0, kEffectIdRole).isNull()) {
            return; // category rows carry no effect id
        }
        emit effectAddRequested(item->data(0, kEffectIdRole).toString());
    });

    connect(apply, &QPushButton::clicked, this, [this, apply] {
        QTreeWidgetItem *item = tree_->currentItem();
        if (!item) {
            return;
        }
        // Walk up from a category selection to its first visible child.
        if (item->data(0, kEffectIdRole).isNull() && item->childCount() > 0) {
            item = item->child(0);
        }
        if (item && !item->data(0, kEffectIdRole).isNull()) {
            emit effectAddRequested(item->data(0, kEffectIdRole).toString());
        }
    });
}

void EffectsPanel::rebuildTree(const QString &filter) {
    QVector<fc::CatalogRow> rows;
    rows.reserve(static_cast<int>(fc::effectCatalog().size()));
    for (const fc::EffectDescriptor &d : fc::effectCatalog()) {
        rows.push_back({QString::fromStdString(d.category), QString::fromStdString(d.label),
                        QString::fromStdString(d.id)});
    }
    fc::rebuildCatalogTree(tree_, rows, filter, [this](const fc::CatalogRow &row) {
        return tr("%1 (%2) - double-click to add to the selected clip").arg(row.label, row.id);
    });
}
