#pragma once

#include <functional>

#include <QString>
#include <QTreeWidget>
#include <QVector>

namespace fc {

// One display row of a searchable catalog (the effects and the
// transitions panels feed their catalogs in as row lists).
struct CatalogRow {
    QString category;
    QString label;
    QString id;
};

// The shared "searchable categorized catalog tree" used by the effects
// and the transitions panels - they were copy-paste twins of this
// logic. Groups CONTIGUOUS same-category rows into disabled category
// branches with selectable leaves (the same visual contract the
// panels always had); filters rows by case-insensitive substring
// matches across label, id, and category. Leaf ids land in
// Qt::UserRole. When leafToolTip is set, it formats each leaf's
// tooltip from its row (kept at the call site so tr() context and
// wording stay with each panel).
//
// Favorites support (suggestion #32, EffectsPanel): when isFavorite is
// set AND favoritesSection is non-empty, the rows it accepts are
// pulled OUT of their normal categories and pinned as the FIRST
// section under that title (so favorites float to the top without
// duplicating leaves). leafCountOut, when set, receives the total
// number of leaves the rebuild produced - panels use it to show a
// "no matches" hint when the filter empties the tree (suggestion #61).
void rebuildCatalogTree(QTreeWidget *tree, const QVector<CatalogRow> &rows, const QString &filter,
                        const std::function<QString(const CatalogRow &)> &leafToolTip = {},
                        const std::function<bool(const CatalogRow &)> &isFavorite = {},
                        const QString &favoritesSection = {}, int *leafCountOut = nullptr);

} // namespace fc
