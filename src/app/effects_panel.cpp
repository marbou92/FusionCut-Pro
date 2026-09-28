#include "effects_panel.h"

#include <QDrag>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "catalog_tree.h"
#include "effects.h"
#include "ui_theme.h"
#include "ui_widgets.h"

// ui_theme.h tokens live in fc::ui; the panel addresses them as ui::...
using namespace fc;

namespace {

// UserRole payload: the catalog effect id of a leaf item.
constexpr int kEffectIdRole = Qt::UserRole;

// Drag mime for cross-panel effect drops (suggestion #33). The timeline
// drop target implements against this EXACT string - do not change it
// without the timeline side (payload = effect id, UTF-8, Qt::MoveAction).
constexpr char kEffectIdMime[] = "application/x-fc-effect-id";

// DPR-marked glyph pixmap (#206): a makeIcon rasterized at the host's
// device pixel ratio so 125/150% Windows scaling stays crisp (#116).
QPixmap glyphPixmap(const QString &name, const QColor &color, int logicalSize, qreal dpr) {
    return icons::makeIcon(name, color, logicalSize, dpr)
        .pixmap(qRound(logicalSize * dpr), qRound(logicalSize * dpr));
}

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

// Effect card rows (#153/#154): the row chrome (kCard tile, kLine
// hairline, 8px radius) comes from the shared catalog sheet; this
// delegate paints the two tokens a QSS row cannot carry - the catalog
// sparkle in front of the label and the favorite star on the right
// edge (kTextDim when idle, kAccent when favorited). A press inside
// the star rect toggles the favorite (the panel's eventFilter routes
// it); the context menu keeps working unchanged.
class EffectCardDelegate : public QStyledItemDelegate {
public:
    explicit EffectCardDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

    // The star's hit zone within a row rect (shared with the panel's
    // click handler so paint and hit-testing never drift).
    static QRect starRect(const QRect &row) {
        return QRect(row.right() - 22, row.top() + (row.height() - 16) / 2, 16, 16);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        const QString id = index.data(kEffectIdRole).toString();
        refreshCache(option.widget ? option.widget->devicePixelRatioF() : 1.0);

        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        if (!id.isEmpty()) {
            // The catalog carries no per-effect glyphs (#116): every
            // row leads with the shared "effects" sparkle.
            opt.icon = cache_.sparkle;
            opt.decorationSize = QSize(16, 16);
            opt.features |= QStyleOptionViewItem::HasDecoration;
        }
        opt.widget->style()->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

        if (!id.isEmpty()) {
            // Star toggle indicator (#154): dim star invites the click,
            // accent star marks the favorite (also pinned in the
            // Favorites section by the shared rebuild).
            const bool favorited = index.data(kCatalogFavoriteRole).toBool();
            painter->drawPixmap(starRect(option.rect).adjusted(1, 1, -1, -1).topLeft(),
                                favorited ? cache_.starOn : cache_.starDim);
        }
    }

private:
    // Built lazily per device pixel ratio - the panel is constructed
    // before it lands on its final screen, so the ratio can still move.
    struct GlyphCache {
        qreal dpr = 0.0;
        QIcon sparkle;
        QPixmap starDim;
        QPixmap starOn;
    };
    mutable GlyphCache cache_;

    void refreshCache(qreal dpr) const {
        if (cache_.dpr == dpr && !cache_.starDim.isNull()) {
            return;
        }
        cache_.dpr = dpr;
        cache_.sparkle = icons::makeIcon("effects", ui::color(ui::kTextDim), 16, dpr);
        cache_.starDim = glyphPixmap("star", ui::color(ui::kTextDim), 14, dpr);
        cache_.starOn = glyphPixmap("star", ui::color(ui::kAccent), 14, dpr);
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
    search_->setAccessibleName(tr("Search effects"));

    tree_ = new EffectDragTree(this);
    tree_->setHeaderLabel(tr("Effects"));
    tree_->setColumnCount(1);
    // Drag SOURCE (suggestion #33): the tree offers effect ids to the
    // timeline's drop targets; it never accepts drops itself.
    tree_->setDragEnabled(true);
    tree_->setDragDropMode(QAbstractItemView::DragOnly);
    tree_->setDefaultDropAction(Qt::MoveAction);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu); // favorite toggling
    // #153/#156: card rows (28px, kCard tile + kLine hairline, 8px
    // radius, accent-tinted selection) with the shared chrome; the row
    // delegate paints the sparkle + favorite star (#154).
    styleCatalogTree(tree_, 28, true);
    tree_->setItemDelegate(new EffectCardDelegate(tree_));
    tree_->setAccessibleName(tr("Effects catalog"));

    // Empty-state overlay (#165, same pattern as Effect Controls): a
    // fc::EmptyState owns the "no matches" / "empty catalog" message
    // inside the viewport. Created BEFORE rebuildTree() below: whenever
    // that filtered tree ends up empty it dereferences the overlay -
    // constructing it later left a null pointer one catalog change away
    // from the same crash class as the 20260919 startup QTimer null
    // deref.
    emptyState_ = new fc::EmptyState(tree_->viewport());
    emptyState_->setObjectName(QStringLiteral("fcEmptyOverlay"));
    emptyState_->setAttribute(Qt::WA_StyledBackground, true);
    emptyState_->setStyleSheet(QStringLiteral("QWidget#fcEmptyOverlay { background: %1; }")
                                   .arg(ui::color(ui::kSurface2).name()));
    emptyState_->hide();
    tree_->viewport()->installEventFilter(this);

    rebuildTree(QString());

    // Panel header (#166): 40px row - 16px glyph in kTextDim + 13px
    // semibold title in kText + the catalog count caption (#166's count
    // is trivially available here).
    const qreal dpr = devicePixelRatioF();
    auto *header = new QWidget(this);
    header->setFixedHeight(40);
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(8);
    auto *headerGlyph = new QLabel(header);
    headerGlyph->setPixmap(glyphPixmap("effects", ui::color(ui::kTextDim), 16, dpr));
    auto *headerTitle = new QLabel(tr("Effects"), header);
    headerTitle->setStyleSheet(QStringLiteral("font-size:13px;font-weight:600;color:%1;")
                                   .arg(ui::color(ui::kText).name()));
    auto *headerCount = new QLabel(header);
    headerCount->setStyleSheet(
        QStringLiteral("font-size:11px;color:%1;").arg(ui::color(ui::kTextDim).name()));
    headerCount->setText(tr("%1 effects").arg(static_cast<int>(fc::effectCatalog().size())));
    headerLayout->addWidget(headerGlyph);
    headerLayout->addWidget(headerTitle);
    headerLayout->addStretch(1);
    headerLayout->addWidget(headerCount);

    auto *apply = new QPushButton(tr("Apply to Selected Clip"), this);
    apply->setToolTip(tr("Adds the selected effect to the clip selected in "
                         "the timeline (or double-click an effect)"));
    // The panel's ONE accent action (#166): applying an effect is the
    // primary action; everything else stays neutral.
    apply->setProperty("fcAccent", true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12); // 12px rhythm (#164)
    layout->setSpacing(8);
    layout->addWidget(header);
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
    if (watched == tree_->viewport()) {
        if (event->type() == QEvent::Resize) {
            // Keep the empty-state overlay covering the viewport through
            // panel resizes.
            emptyState_->setGeometry(emptyState_->parentWidget()->rect());
        } else if (event->type() == QEvent::MouseButtonPress) {
            // #154: the star is a real toggle - a press inside its rect
            // flips the favorite and never reaches the tree (no selection
            // change, no drag arm). Double-click and the context menu on
            // the rest of the row are untouched.
            auto *press = static_cast<QMouseEvent *>(event);
            QTreeWidgetItem *item = tree_->itemAt(press->pos());
            if (item && !item->data(0, kEffectIdRole).isNull() &&
                EffectCardDelegate::starRect(tree_->visualItemRect(item)).contains(press->pos())) {
                toggleFavorite(item->data(0, kEffectIdRole).toString());
                return true; // consumed: the star handled the click
            }
        }
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
    // Empty state (#165): the overlay owns both "nothing to show"
    // states - an unfiltered empty catalog says so, a filter that
    // emptied the tree offers the search glyph instead.
    if (leafCount == 0) {
        const QString needle = filter.trimmed();
        const qreal dpr = devicePixelRatioF();
        if (needle.isEmpty()) {
            emptyState_->setIcon(icons::makeIcon("effects", ui::color(ui::kTextDisabled), 36, dpr),
                                 36, dpr);
            emptyState_->setTitle(tr("The catalog is empty"));
            emptyState_->setHint(tr("Effects appear here once the catalog has entries."));
        } else {
            emptyState_->setIcon(icons::makeIcon("search", ui::color(ui::kTextDisabled), 36, dpr),
                                 36, dpr);
            emptyState_->setTitle(tr("No matches"));
            emptyState_->setHint(
                tr("No matches for %1").arg(QStringLiteral("\u201C%1\u201D").arg(needle)));
        }
        emptyState_->setGeometry(emptyState_->parentWidget()->rect());
        emptyState_->raise();
        emptyState_->refresh(true);
    } else {
        emptyState_->refresh(false);
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
