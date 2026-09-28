#include "transitions_panel.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QHoverEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cmath>

#include "catalog_tree.h"
#include "transitions.h"
#include "ui_theme.h"
#include "ui_widgets.h"

// ui_theme.h tokens live in fc::ui; the panel addresses them as ui::...
using namespace fc;

namespace {

// UserRole payload: the catalog transition id of a leaf item.
constexpr int kTransitionIdRole = Qt::UserRole;

} // namespace

// Schematic preview card (suggestion #36): paints the currently hovered
// / selected transition kind as a two-rect A|B composite at the live
// flipbook progress, rounded to the 8px tile radius (#155's corner
// language - the A/B colors stay semantic: warm outgoing, accent
// incoming). No Q_OBJECT needed - the panel drives it purely through
// setProgress()/setKind(); styles come from ui_theme tokens.
// Defined at GLOBAL scope ON PURPOSE: transitions_panel.h forward-
// declares this class at global scope for the preview_ member, and an
// anonymous-namespace definition here would be a DIFFERENT, unrelated
// type (the same bug class as the ScopesPanel CI failure).
class TransitionPreviewCard : public QWidget {
public:
    explicit TransitionPreviewCard(QWidget *parent = nullptr) : QWidget(parent) {
        setFixedHeight(76);
        setMinimumWidth(120);
        setAutoFillBackground(false);
    }

    void setKind(const QString &kind) {
        kind_ = kind;
        update();
    }
    void setProgress(double p) {
        progress_ = p < 0.0 ? 0.0 : (p > 1.0 ? 1.0 : p);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), ui::color(ui::kCanvas));

        const QRect band = rect().adjusted(8, 8, -8, -8);
        if (band.width() < 8 || band.height() < 8) {
            return;
        }
        // Rounded 8px tile (kRadiusControl) holding the two schematic
        // "clips": A (outgoing, warm) and B (incoming, accent) composed
        // per kind at the live progress.
        painter.setRenderHint(QPainter::Antialiasing, true);
        const qreal radius = ui::kRadiusControl;
        QPainterPath tile;
        tile.addRoundedRect(QRectF(band), radius, radius);
        const QColor a = ui::mix(ui::color(ui::kSurface3), ui::color(ui::kWarning), 0.35);
        const QColor b = ui::color(ui::kAccent);
        const double p = progress_;
        painter.save();
        painter.setClipPath(tile);
        if (kind_.startsWith(QLatin1String("wipe"))) {
            const int split = band.left() + static_cast<int>(band.width() * p);
            painter.fillRect(QRect(band.left(), band.top(), split - band.left(), band.height()), a);
            painter.fillRect(QRect(split, band.top(), band.right() - split + 1, band.height()), b);
        } else if (kind_.startsWith(QLatin1String("slide"))) {
            const int off = static_cast<int>(band.width() * (1.0 - p));
            painter.fillRect(band, a);
            painter.fillRect(QRect(band.left() + off, band.top(), band.width(), band.height()), b);
        } else if (kind_.startsWith(QLatin1String("push"))) {
            const int off = static_cast<int>(band.width() * p);
            painter.fillRect(QRect(band.left(), band.top(), band.width(), band.height()), a);
            painter.fillRect(
                QRect(band.left() - band.width() + off, band.top(), band.width(), band.height()),
                b);
        } else {
            // everything else reads as a dissolve family member: a plain
            // alpha blend (the catalog's dominant shape)
            painter.fillRect(band, a);
            painter.setOpacity(p);
            painter.fillRect(band, b);
            painter.setOpacity(1.0);
        }
        painter.restore();
        painter.setPen(ui::color(ui::kLine));
        painter.drawPath(tile);
    }

private:
    QString kind_;
    double progress_ = 0.0;
};

TransitionsPanel::TransitionsPanel(QWidget *parent) : QWidget(parent) {
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(tr("Search transitions..."));
    search_->setClearButtonEnabled(true);
    search_->setAccessibleName(tr("Search transitions"));

    // Suggestion #36: schematic preview card + kind label on top.
    preview_ = new TransitionPreviewCard(this);
    previewLabel_ = new QLabel(tr("Hover a transition to preview it"), this);
    previewLabel_->setAlignment(Qt::AlignCenter);
    // #155: the card's caption reads as an 11px kTextDim annotation.
    previewLabel_->setStyleSheet(
        QStringLiteral("font-size:11px;color:%1;").arg(ui::color(ui::kTextDim).name()));

    tree_ = new QTreeWidget(this);
    tree_->setHeaderLabel(tr("Transitions"));
    tree_->setColumnCount(1);
    tree_->setMouseTracking(true);
    tree_->viewport()->setAttribute(Qt::WA_Hover, true);
    tree_->viewport()->installEventFilter(this);
    tree_->setAccessibleName(tr("Transitions catalog"));
    // #155: the catalog is a plain tree with no delegate, so the honest
    // card conversion is the row fallback - 44px card-rows (kCard tile,
    // 1px kLine, 8px radius, accent selection) via the shared sheet.
    styleCatalogTree(tree_, 44, true);
    // Empty-state overlay (#165, same pattern as Effect Controls): owns
    // the "no matches" message inside the viewport. Created BEFORE
    // rebuildTree() below: rebuildTree unconditionally updates the
    // overlay (it must exist even to stay hidden) - constructing it
    // after the call made the ctor dereference a null widget (the next
    // startup crash behind the 20260919 QTimer one).
    emptyState_ = new fc::EmptyState(tree_->viewport());
    emptyState_->setObjectName(QStringLiteral("fcEmptyOverlay"));
    emptyState_->setAttribute(Qt::WA_StyledBackground, true);
    emptyState_->setStyleSheet(QStringLiteral("QWidget#fcEmptyOverlay { background: %1; }")
                                   .arg(ui::color(ui::kSurface2).name()));
    emptyState_->hide();
    rebuildTree(QString());

    flipTimer_ = new QTimer(this);
    flipTimer_->setInterval(33);
    connect(flipTimer_, &QTimer::timeout, this, [this] {
        flipProgress_ += 0.033;
        if (flipProgress_ >= 1.0) {
            flipProgress_ = 1.0;
            flipTimer_->stop();
        }
        preview_->setProgress(flipProgress_);
    });

    // Panel header (#166): 40px row - 16px glyph in kTextDim + 13px
    // semibold title in kText + the catalog count caption.
    const qreal dpr = devicePixelRatioF();
    auto *header = new QWidget(this);
    header->setFixedHeight(40);
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(8);
    auto *headerGlyph = new QLabel(header);
    headerGlyph->setPixmap(icons::makeIcon("transitions", ui::color(ui::kTextDim), 16, dpr)
                               .pixmap(qRound(16 * dpr), qRound(16 * dpr)));
    auto *headerTitle = new QLabel(tr("Transitions"), header);
    headerTitle->setStyleSheet(QStringLiteral("font-size:13px;font-weight:600;color:%1;")
                                   .arg(ui::color(ui::kText).name()));
    auto *headerCount = new QLabel(header);
    headerCount->setStyleSheet(
        QStringLiteral("font-size:11px;color:%1;").arg(ui::color(ui::kTextDim).name()));
    headerCount->setText(
        tr("%1 transitions").arg(static_cast<int>(fc::transitionCatalog().size())));
    headerLayout->addWidget(headerGlyph);
    headerLayout->addWidget(headerTitle);
    headerLayout->addStretch(1);
    headerLayout->addWidget(headerCount);

    auto *apply = new QPushButton(tr("Apply to Selected Clip's Cut"), this);
    apply->setToolTip(tr("Adds the transition to the cut after the clip selected in "
                         "the timeline (or double-click a transition)"));
    apply->setAccessibleName(apply->text());
    // The panel's ONE accent action (#166): applying a transition is the
    // primary action; everything else stays neutral.
    apply->setProperty("fcAccent", true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12); // 12px rhythm (#164)
    layout->setSpacing(8);
    layout->addWidget(header);
    layout->addWidget(search_);
    layout->addWidget(preview_);
    layout->addWidget(previewLabel_);
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

    connect(tree_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem *item, QTreeWidgetItem *) {
                if (!item || item->data(0, kTransitionIdRole).isNull()) {
                    return;
                }
                // Selection switches the card without re-running the
                // flipbook (the row is not under the mouse anymore).
                preview_->setKind(item->data(0, kTransitionIdRole).toString());
                preview_->setProgress(0.5);
            });

    connect(apply, &QPushButton::clicked, this, [this] {
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
    });
}

bool TransitionsPanel::eventFilter(QObject *watched, QEvent *event) {
    if (watched == tree_->viewport()) {
        if (event->type() == QEvent::HoverMove) {
            const QPoint pos = static_cast<QHoverEvent *>(event)->pos();
            QTreeWidgetItem *item = tree_->itemAt(pos);
            if (item && !item->data(0, kTransitionIdRole).isNull()) {
                setPreviewKind(item->data(0, kTransitionIdRole).toString(), item->text(0), true);
            }
        } else if (event->type() == QEvent::HoverLeave) {
            // Park the card instead of blanking it - the last hovered
            // kind stays readable; the flipbook stops.
            flipTimer_->stop();
        }
    }
    return QWidget::eventFilter(watched, event);
}

void TransitionsPanel::setPreviewKind(const QString &kind, const QString &label, bool animate) {
    preview_->setKind(kind);
    previewLabel_->setText(label);
    if (animate) {
        flipProgress_ = 0.0;
        preview_->setProgress(0.0);
        if (!flipTimer_->isActive()) {
            flipTimer_->start();
        }
    }
}

void TransitionsPanel::rebuildTree(const QString &filter) {
    QVector<fc::CatalogRow> rows;
    rows.reserve(static_cast<int>(fc::transitionCatalog().size()));
    for (const fc::TransitionDescriptor &d : fc::transitionCatalog()) {
        rows.push_back({QString::fromStdString(d.category), QString::fromStdString(d.label),
                        QString::fromStdString(d.id)});
    }
    int leafCount = 0;
    fc::rebuildCatalogTree(
        tree_, rows, filter,
        [this](const fc::CatalogRow &row) {
            return tr("%1 (%2) - double-click to add to the selected clip's cut")
                .arg(row.label, row.id);
        },
        {}, {}, &leafCount);
    // Empty state (#165): the filter emptied the catalog - the overlay
    // owns the message (search glyph + title + hint).
    if (leafCount == 0) {
        const qreal dpr = devicePixelRatioF();
        emptyState_->setIcon(icons::makeIcon("search", ui::color(ui::kTextDisabled), 36, dpr), 36,
                             dpr);
        emptyState_->setTitle(tr("No matches"));
        emptyState_->setHint(tr("No transitions match \"%1\"").arg(filter));
        emptyState_->setGeometry(emptyState_->parentWidget()->rect());
        emptyState_->raise();
        emptyState_->refresh(true);
    } else {
        emptyState_->refresh(false);
    }
}
