#include "effects_panel.h"

#include <QDrag>
#include <QEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPushButton>
#include <QSettings>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "catalog_tree.h"
#include "effects.h"
#include "ui_theme.h"

// ui_theme.h tokens live in fc::ui; the panel addresses them as ui::...
using namespace fc;

namespace {

// UserRole payload: the catalog effect id of a leaf item.
constexpr int kEffectIdRole = Qt::UserRole;

// Drag mime for cross-panel effect drops (suggestion #33). The timeline
// drop target implements against this EXACT string - do not change it
// without the timeline side (payload = effect id, UTF-8, Qt::MoveAction).
constexpr char kEffectIdMime[] = "application/x-fc-effect-id";

// Drag SOURCE: a QTreeWidget subclass that puts the current leaf's
// effect id on the drag as a UTF-8 payload under the fixed mime type.
// Category rows carry no id and never start a drag. (The small local
// subclass is the repo-idiomatic way to add one override without a new
// header; the panel keeps talking to it through the QTreeWidget*.)
class EffectDragTree : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;

protected:
    void startDrag(Qt::DropActions supportedActions) override {
        Q_UNUSED(supportedActions)
        QTreeWidgetItem *item = currentItem();
        if (!item) {
            return;
        }
        const QString id = item->data(0, kEffectIdRole).toString();
        if (id.isEmpty()) {
            return;
        }
        auto *mime = new QMimeData;
        mime->setData(QString::fromLatin1(kEffectIdMime), id.toUtf8());
        QDrag *drag = new QDrag(this);
        drag->setMimeData(mime);
        drag->exec(Qt::MoveAction);
    }
};

} // namespace

EffectsPanel::EffectsPanel(QWidget *parent) : QWidget(parent) {
    // Persisted favorites (suggestion #32): ids of catalog effects; a
    // favorite that no longer exists in the catalog simply never shows.
    const QStringList stored = QSettings().value("effects/favorites").toStringList();
    for (const QString &id : stored) {
        if (!id.isEmpty()) {
            favorites_.insert(id);
        }
    }

    search_ = new QLineEdit(this);
    search_->setPlaceholderText(tr("Search effects..."));
    search_->setClearButtonEnabled(true);
    search_->setToolTip(tr("Filters the catalog by label, id or category (plain text)"));

    tree_ = new EffectDragTree(this);
    tree_->setHeaderLabel(tr("Effects"));
    tree_->setColumnCount(1);
    // Drag SOURCE (suggestion #33): the tree offers effect ids to the
    // timeline's drop targets; it never accepts drops itself.
    tree_->setDragEnabled(true);
    tree_->setDragDropMode(QAbstractItemView::DragOnly);
    tree_->setDefaultDropAction(Qt::MoveAction);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu); // favorite toggling
    rebuildTree(QString());

    // Dim "no matches" hint inside the tree viewport (suggestion #61);
    // rebuildTree shows it only when the filter empties the tree.
    treeHint_ = new QLabel(tree_->viewport());
    treeHint_->setStyleSheet(QStringLiteral("color:%1;font-size:11px;background:transparent;")
                                 .arg(fc::ui::color(fc::ui::kTextDim).name()));
    treeHint_->setAlignment(Qt::AlignCenter);
    treeHint_->setWordWrap(true);
    treeHint_->hide();
    tree_->viewport()->installEventFilter(this);

    auto *apply = new QPushButton(tr("Apply to Selected Clip"), this);
    apply->setToolTip(tr("Adds the selected effect to the clip selected in "
                         "the timeline (or double-click an effect)"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8); // 8px rhythm (suggestion #56)
    layout->setSpacing(8);
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

    connect(apply, &QPushButton::clicked, this, [this] {
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

    connect(tree_, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint &pos) { showLeafContextMenu(pos); });
}

bool EffectsPanel::eventFilter(QObject *watched, QEvent *event) {
    if (watched == tree_->viewport() && event->type() == QEvent::Resize) {
        // Keep the hint covering the viewport through panel resizes.
        treeHint_->setGeometry(treeHint_->parentWidget()->rect());
    }
    return QWidget::eventFilter(watched, event);
}

void EffectsPanel::rebuildTree(const QString &filter) {
    QVector<fc::CatalogRow> rows;
    rows.reserve(static_cast<int>(fc::effectCatalog().size()));
    for (const fc::EffectDescriptor &d : fc::effectCatalog()) {
        rows.push_back({QString::fromStdString(d.category), QString::fromStdString(d.label),
                        QString::fromStdString(d.id)});
    }
    int leafCount = 0;
    fc::rebuildCatalogTree(
        tree_, rows, filter,
        [this](const fc::CatalogRow &row) {
            return tr("%1 (%2) - double-click to add to the selected clip, "
                      "drag onto a clip, or right-click to favorite")
                .arg(row.label, row.id);
        },
        [this](const fc::CatalogRow &row) { return favorites_.contains(row.id); }, tr("Favorites"),
        &leafCount);
    // Empty-state hint (suggestion #61): only when the FILTER yields
    // zero rows; an unfiltered empty catalog would say so instead.
    if (leafCount == 0) {
        const QString needle = filter.trimmed();
        treeHint_->setText(
            needle.isEmpty()
                ? tr("The catalog is empty")
                : tr("No matches for %1").arg(QStringLiteral("\u201C%1\u201D").arg(needle)));
        treeHint_->setGeometry(treeHint_->parentWidget()->rect());
        treeHint_->raise();
        treeHint_->show();
    } else {
        treeHint_->hide();
    }
}

void EffectsPanel::showLeafContextMenu(const QPoint &pos) {
    QTreeWidgetItem *item = tree_->itemAt(pos);
    if (!item) {
        return;
    }
    const QString id = item->data(0, kEffectIdRole).toString();
    if (id.isEmpty()) {
        return; // category rows (and the favorites section) carry no id
    }
    QMenu menu(this);
    QAction *toggle = menu.addAction(favorites_.contains(id) ? tr("Remove from Favorites")
                                                             : tr("Add to Favorites"));
    QAction *chosen = menu.exec(tree_->mapToGlobal(pos));
    if (chosen == toggle) {
        toggleFavorite(id);
    }
}

void EffectsPanel::toggleFavorite(const QString &effectId) {
    if (effectId.isEmpty()) {
        return;
    }
    if (favorites_.contains(effectId)) {
        favorites_.remove(effectId);
    } else {
        favorites_.insert(effectId);
    }
    // Sort for deterministic storage regardless of QSet's hash order.
    QStringList stored = favorites_.values();
    stored.sort();
    QSettings().setValue("effects/favorites", stored);
    rebuildTree(search_->text());
}
