#include "catalog_tree.h"

#include <QTreeWidgetItem>

#include "ui_theme.h"

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

    // Shared leaf contract: selectable, id in Qt::UserRole, favorite
    // flag in kCatalogFavoriteRole (#154 - the effects panel's star
    // delegate reads it), optional call-site-formatted tooltip (tr()
    // context stays with the panel).
    const auto makeLeaf = [&](QTreeWidgetItem *parent, const CatalogRow &row) {
        auto *leaf = new QTreeWidgetItem(parent, QStringList(row.label));
        leaf->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        leaf->setData(0, Qt::UserRole, row.id);
        if (isFavorite) {
            leaf->setData(0, kCatalogFavoriteRole, isFavorite(row));
        }
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

void styleCatalogTree(QTreeWidget *tree, int rowHeight, bool cardRows) {
    if (!tree) {
        return;
    }
    tree->setHeaderHidden(true); // the panel header (#166) titles the catalog

    // Tokens once, then two item dialects: flat rows (the #156 base -
    // hover fills the row) and card rows (#153/#155 - hover brightens
    // the border only, per #154's "hover raise"). Selection keeps the
    // round-2 librarySheet contract: accent @ 46 alpha + kText.
    const QString surface2 = ui::color(ui::kSurface2).name();
    const QString card = ui::color(ui::kCard).name();
    const QString line = ui::color(ui::kLine).name();
    const QString text = ui::color(ui::kText).name();
    const QString hover = ui::color(ui::kSurfaceHover).name();
    const QString selectFill = ui::withAlpha(ui::kAccent, 46).name(QColor::HexArgb);
    const QString selectEdge = ui::withAlpha(ui::kAccent, 120).name(QColor::HexArgb);
    const QString accent = ui::color(ui::kAccent).name();

    QString sheet = QStringLiteral("QTreeView { background: %1; border: none; }"
                                   "QTreeView::item { min-height: %2px; color: %3;")
                        .arg(surface2)
                        .arg(rowHeight)
                        .arg(text);
    if (cardRows) {
        sheet += QStringLiteral(" background: %1; border: 1px solid %2; border-radius: 8px;"
                                " margin: 3px 6px;")
                     .arg(card, line);
    } else {
        sheet += QStringLiteral(" border-radius: 6px;");
    }
    sheet += QStringLiteral(" }");
    if (cardRows) {
        sheet +=
            QStringLiteral("QTreeView::item:hover:!selected { border-color: %1; }").arg(selectEdge);
        sheet += QStringLiteral("QTreeView::item:selected { background: %1; border-color: %2;"
                                " border-width: 2px; color: %3; }")
                     .arg(selectFill, accent, text);
    } else {
        sheet += QStringLiteral("QTreeView::item:hover:!selected { background: %1; }").arg(hover);
        sheet += QStringLiteral("QTreeView::item:selected { background: %1; color: %2; }")
                     .arg(selectFill, text);
    }
    tree->setStyleSheet(sheet);
}

} // namespace fc
