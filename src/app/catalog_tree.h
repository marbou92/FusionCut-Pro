#pragma once

#include <functional>

#include <QString>
#include <QTreeWidget>
#include <QVector>

namespace fc {

// Leaf favorite flag (suggestion #154): set on every leaf when the
// isFavorite predicate is provided, so a delegate can render the star
// without re-running the predicate (Qt::UserRole carries the id).
constexpr int kCatalogFavoriteRole = Qt::UserRole + 1;

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

// Shared catalog chrome (#156): dark viewport (0-b bug class), quiet
// row height, kSurfaceHover hover and the accent-tinted selection with
// kText - one language for both catalog panels. `cardRows` lifts the
// rows into card tiles (#153 effect cards / #155 transition rows:
// kCard background, 1px kLine border, 8px radius, 3px/6px margins) and
// switches hover to a border brighten only (#154's "hover raise").
// `rowHeight` is the row min-height in px (#156 base: 26; the effects
// panel passes 28, the transitions panel 44 for its card rows).
// The tree's own header row is hidden - the panel header (#166) titles
// the catalog. Branch disclosure arrows stay NATIVE: the app-wide
// stylesheet deliberately carries no QTreeView::branch rules.
void styleCatalogTree(QTreeWidget *tree, int rowHeight = 26, bool cardRows = false);

} // namespace fc
