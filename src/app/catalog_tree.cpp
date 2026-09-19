#include "catalog_tree.h"

#include <QTreeWidgetItem>

namespace fc {

void rebuildCatalogTree(QTreeWidget *tree, const QVector<CatalogRow> &rows, const QString &filter,
                        const std::function<QString(const CatalogRow &)> &leafToolTip,
                        const std::function<bool(const CatalogRow &)> &isFavorite,
                        const QString &favoritesSection, int *leafCountOut) {
    if (!tree) {
        return;
    }
    tree->clear();
    const QString needle = filter.trimmed().toLower();
    int leafCount = 0;

    // Shared leaf contract: selectable, id in Qt::UserRole, optional
    // call-site-formatted tooltip (tr() context stays with the panel).
    const auto makeLeaf = [&](QTreeWidgetItem *parent, const CatalogRow &row) {
        auto *leaf = new QTreeWidgetItem(parent, QStringList(row.label));
        leaf->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        leaf->setData(0, Qt::UserRole, row.id);
        if (leafToolTip) {
            leaf->setToolTip(0, leafToolTip(row));
        }
        ++leafCount;
    };
    const auto matches = [&needle](const CatalogRow &row) {
        return needle.isEmpty() || row.label.toLower().contains(needle) ||
               row.id.toLower().contains(needle) || row.category.toLower().contains(needle);
    };

    // Favorites first (suggestion #32): accepted rows are grouped under
    // one pinned section and REMOVED from their normal categories below,
    // so every leaf appears exactly once. The section only appears when
    // at least one favorite matches the filter.
    if (isFavorite && !favoritesSection.isEmpty()) {
        QTreeWidgetItem *favorites = nullptr;
        for (const CatalogRow &row : rows) {
            if (!matches(row) || !isFavorite(row)) {
                continue;
            }
            if (favorites == nullptr) {
                favorites = new QTreeWidgetItem(tree, QStringList(favoritesSection));
                favorites->setFlags(Qt::ItemIsEnabled);
            }
            makeLeaf(favorites, row);
        }
    }

    QTreeWidgetItem *currentCategory = nullptr;
    QString currentCategoryName;
    for (const CatalogRow &row : rows) {
        if (!matches(row)) {
            continue;
        }
        if (isFavorite && isFavorite(row)) {
            continue; // already pinned in the favorites section above
        }
        if (currentCategory == nullptr || row.category != currentCategoryName) {
            currentCategory = new QTreeWidgetItem(tree, QStringList(row.category));
            currentCategory->setFlags(Qt::ItemIsEnabled);
            currentCategoryName = row.category;
        }
        makeLeaf(currentCategory, row);
    }
    tree->expandAll();
    if (leafCountOut) {
        *leafCountOut = leafCount;
    }
}

} // namespace fc
