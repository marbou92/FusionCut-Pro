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

// ui_theme.h tokens live in fc::ui; the panel addresses them as ui::...
using namespace fc;

namespace {

// UserRole payload: the catalog transition id of a leaf item.
constexpr int kTransitionIdRole = Qt::UserRole;

// Schematic preview card (suggestion #36): paints the currently hovered
// / selected transition kind as a two-rect A|B composite at the live
// flipbook progress. No Q_OBJECT needed - the panel drives it purely
// through setProgress()/setKind(); styles come from ui_theme tokens.
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
        // Two schematic "clips": A (outgoing, warm) and B (incoming,
        // accent) composed per kind at the live progress.
        const QColor a = ui::mix(ui::color(ui::kSurface3), ui::color(ui::kWarning), 0.35);
        const QColor b = ui::color(ui::kAccent);
        const double p = progress_;
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
        painter.setPen(ui::color(ui::kLine));
        painter.drawRect(band);
    }

private:
    QString kind_;
    double progress_ = 0.0;
};

} // namespace

TransitionsPanel::TransitionsPanel(QWidget *parent) : QWidget(parent) {
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(tr("Search transitions..."));
    search_->setClearButtonEnabled(true);
    search_->setAccessibleName(tr("Search transitions"));

    // Suggestion #36: schematic preview card + kind label on top.
    preview_ = new TransitionPreviewCard(this);
    previewLabel_ = new QLabel(tr("Hover a transition to preview it"), this);
    previewLabel_->setAlignment(Qt::AlignCenter);
    previewLabel_->setStyleSheet(QStringLiteral("color:%1;").arg(ui::color(ui::kTextDim).name()));

    tree_ = new QTreeWidget(this);
    tree_->setHeaderLabel(tr("Transitions"));
    tree_->setColumnCount(1);
    tree_->setMouseTracking(true);
    tree_->viewport()->setAttribute(Qt::WA_Hover, true);
    tree_->viewport()->installEventFilter(this);
    tree_->setAccessibleName(tr("Transitions catalog"));
    rebuildTree(QString());

    // Suggestion #61: the "no matches" hint lives inside the viewport.
    treeHint_ = new QLabel(this);
    treeHint_->setAlignment(Qt::AlignCenter);
    treeHint_->setWordWrap(true);
    treeHint_->setStyleSheet(QStringLiteral("color:%1;").arg(ui::color(ui::kTextDim).name()));
    treeHint_->hide();

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

    auto *apply = new QPushButton(tr("Apply to Selected Clip's Cut"), this);
    apply->setToolTip(tr("Adds the transition to the cut after the clip selected in "
                         "the timeline (or double-click a transition)"));
    apply->setAccessibleName(apply->text());

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);
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
    // Empty-state hint (#61): the filter emptied the catalog.
    treeHint_->setText(tr("No transitions match \"%1\"").arg(filter));
    treeHint_->setGeometry(tree_->viewport()->rect().adjusted(8, 8, -8, -8));
    treeHint_->raise();
    treeHint_->setVisible(leafCount == 0);
}
