#include "catalog_tree.h"

#include <QTreeWidgetItem>

namespace fc {

void rebuildCatalogTree(QTreeWidget *tree, const QVector<CatalogRow> &rows, const QString &filter,
                        const std::function<QString(const CatalogRow &)> &leafToolTip) {
    if (!tree) {
        return;
    }
    tree->clear();
    const QString needle = filter.trimmed().toLower();

    QTreeWidgetItem *currentCategory = nullptr;
    QString currentCategoryName;
    for (const CatalogRow &row : rows) {
        if (!needle.isEmpty() && !row.label.toLower().contains(needle) &&
            !row.id.toLower().contains(needle) && !row.category.toLower().contains(needle)) {
            continue;
        }
        if (currentCategory == nullptr || row.category != currentCategoryName) {
            currentCategory = new QTreeWidgetItem(tree, QStringList(row.category));
            currentCategory->setFlags(Qt::ItemIsEnabled);
            currentCategoryName = row.category;
        }
        auto *leaf = new QTreeWidgetItem(currentCategory, QStringList(row.label));
        leaf->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        leaf->setData(0, Qt::UserRole, row.id);
        if (leafToolTip) {
            leaf->setToolTip(0, leafToolTip(row));
        }
    }
    tree->expandAll();
}

} // namespace fc
