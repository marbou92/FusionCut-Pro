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
void rebuildCatalogTree(QTreeWidget *tree, const QVector<CatalogRow> &rows, const QString &filter,
                        const std::function<QString(const CatalogRow &)> &leafToolTip = {});

} // namespace fc
