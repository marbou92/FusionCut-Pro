#include "project_panel.h"

#include <QComboBox>
#include <QEvent>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QPainter>
#include <QPolygon>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>

#include "ui_theme.h"
#include "ui_widgets.h"

// ui_theme.h tokens live in fc::ui; the panel addresses them as ui::...
using namespace fc;

namespace {

constexpr int kThumbnailWidth = 96;
constexpr int kThumbnailHeight = 54;

// Item payload roles. The stub harness's Qt class predates the item
// data-role enums, so the canonical values are pinned here: 0x0100 is
// Qt::UserRole in real Qt and the usage count rides one slot above it.
constexpr int kPathRole = 0x0100;       // == Qt::UserRole
constexpr int kUsageCountRole = 0x0101; // == Qt::UserRole + 1

// fc::MediaItem carries no stream flags; mediaSummary() prints exactly
// "audio only" for video-less files, so the type filter keys off that
// (the only signal available without re-probing the file).
bool isAudioOnly(const fc::MediaItem &item) {
    return item.summary == QStringLiteral("audio only");
}

// Grid geometry (#28): the icon stays at the list thumbnail size and the
// grid cell adds breathing room for the label and the usage chip.
constexpr int kGridCellWidth = kThumbnailWidth + 16;
constexpr int kGridCellHeight = kThumbnailHeight + 22;

// Dark list + quiet rows (0-b/#82): a plain QListWidget paints the
// palette's light Base on Win7 inside this dark panel, so the viewport is
// forced to kSurface2 with 28px rows and the accent-tinted selection (no
// focus-rectangle clutter). Re-applied per view mode: grid cells round to
// 8px and inset their thumbnails a touch more so the selection reads like
// a card. The app-wide sheet styles the same states at 24px/6px; this
// local sheet wins where it is set, which is the intent here.
QString librarySheet(bool grid) {
    const QString radius = grid ? QStringLiteral("8px") : QStringLiteral("6px");
    const QString padding = grid ? QStringLiteral("2px 6px") : QStringLiteral("2px 4px");
    return QStringLiteral("QListWidget { background: %1; border: none; }"
                          "QListWidget::item { min-height: 28px; border-radius: %2; padding: %3; }"
                          "QListWidget::item:selected { background: %4; color: %5; }"
                          "QListWidget::item:hover:!selected { background: %6; }")
        .arg(ui::color(ui::kSurface2).name())
        .arg(radius)
        .arg(padding)
        .arg(ui::withAlpha(ui::kAccent, 46).name(QColor::HexArgb))
        .arg(ui::color(ui::kText).name())
        .arg(ui::color(ui::kSurface3).name());
}

// Usage chip painter (#29): draws a small right-edge rounded chip with
// the per-path usage count over the standard item rendering. Works in
// both List and Grid modes; chips only appear for count >= 1.
class UsageCountDelegate : public QStyledItemDelegate {
public:
    explicit UsageCountDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        QStyledItemDelegate::paint(painter, option, index);
        const int count = index.data(kUsageCountRole).toInt();
        if (count < 1) {
            return;
        }
        const QString text = QString::number(count);
        const QFontMetrics metrics(option.font);
        const int textWidth = metrics.horizontalAdvance(text);
        const int chipWidth = textWidth + 8;
        const int chipHeight = option.rect.height() - 4 < 14 ? option.rect.height() - 4 : 14;
        if (chipWidth < 14 || option.rect.width() < chipWidth + 6) {
            return; // no room for the chip (tiny grid cells)
        }
        const QRect chip(option.rect.right() - chipWidth - 2,
                         option.rect.top() + (option.rect.height() - chipHeight) / 2, chipWidth,
                         chipHeight);
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        painter->setBrush(fc::ui::tint(fc::ui::kAccent));
        painter->drawRoundedRect(chip, chipHeight / 2, chipHeight / 2);
        painter->setPen(fc::ui::color(fc::ui::kText));
        painter->drawText(chip, Qt::AlignCenter, text);
    }
};

} // namespace

ProjectPanel::ProjectPanel(QWidget *parent) : QWidget(parent) {
    list_ = new QListWidget(this);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    list_->setIconSize(QSize(kThumbnailWidth, kThumbnailHeight));
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    // Usage chips (#29): one delegate for both view modes.
    list_->setItemDelegate(new UsageCountDelegate(list_));

    importButton_ = new QPushButton(tr("Import Media..."), this);
    // #66: Import is this panel's one primary action - the app-wide sheet
    // lights fcAccent buttons with the accent fill + dark text.
    importButton_->setProperty("fcAccent", true);
    removeButton_ = new QPushButton(tr("Remove"), this);
    removeButton_->setEnabled(false);

    metaName_ = new QLabel(tr("-"), this);
    metaSummary_ = new QLabel(tr("-"), this);
    metaDuration_ = new QLabel(tr("-"), this);
    metaProxy_ = new QLabel(tr("-"), this);
    metaName_->setWordWrap(true);
    metaSummary_->setWordWrap(true);

    // View mode switch (#28/#81): one segmented List | Grid control
    // replaces the old checkable-button pair; applyViewMode() keeps it in
    // sync and persists QSettings "project/viewMode" exactly as before.
    viewSwitch_ = new fc::SegmentedControl(this);
    viewSwitch_->addSegment(
        tr("List"), icons::makeIcon("list", ui::color(ui::kTextDim), 16, devicePixelRatioF()),
        tr("Compact list view"));
    viewSwitch_->addSegment(
        tr("Grid"), icons::makeIcon("grid", ui::color(ui::kTextDim), 16, devicePixelRatioF()),
        tr("Thumbnail grid view"));

    // Filter row (#31): name filter + stream-type combo.
    filterEdit_ = new QLineEdit(this);
    filterEdit_->setPlaceholderText(tr("Filter..."));
    filterEdit_->setClearButtonEnabled(true);
    filterEdit_->setToolTip(tr("Filters the library by display name (press / to focus)"));
    filterEdit_->setAccessibleName(tr("Media filter"));
    filterEdit_->setFixedWidth(140); // #81: fixed-width search field, right-aligned
    typeCombo_ = new QComboBox(this);
    typeCombo_->addItem(tr("All types"));
    typeCombo_->addItem(tr("Video"));
    typeCombo_->addItem(tr("Audio"));
    typeCombo_->setToolTip(tr("Filter by stream type"));
    typeCombo_->setAccessibleName(tr("Media type filter"));

    // Import feedback (#30): a banner when files were skipped, a plain
    // line for plain additions; hidden again 4 s later.
    feedbackBanner_ = new fc::ErrorBanner(this);
    feedbackLabel_ = new QLabel(this);
    feedbackLabel_->setStyleSheet(
        QStringLiteral("color:%1;").arg(fc::ui::color(fc::ui::kSuccess).name()));
    feedbackLabel_->hide();
    feedbackTimer_ = new QTimer(this);
    feedbackTimer_->setSingleShot(true);
    connect(feedbackTimer_, &QTimer::timeout, this, [this] {
        feedbackBanner_->clear();
        feedbackLabel_->hide();
    });

    // One 40px header row (#81): segmented switch - stretch - filter field
    // and type combo (the global sheet styles both dark).
    auto *filterRow = new QWidget(this);
    filterRow->setFixedHeight(40);
    auto *filterLayout = new QHBoxLayout(filterRow);
    filterLayout->setContentsMargins(0, 0, 0, 0);
    filterLayout->setSpacing(8);
    filterLayout->addWidget(viewSwitch_);
    filterLayout->addStretch(1);
    filterLayout->addWidget(filterEdit_);
    filterLayout->addWidget(typeCombo_);

    // 0-c/#83: metadata as an elevated card - the old bold "Metadata"
    // label sat invisible on the light list background. Now a dim small
    // caption with label:value rows on the card tone (kCard, hairline,
    // 10px radius). The value labels keep their existing wiring.
    auto *metaCard = new QFrame(this);
    metaCard->setObjectName(QStringLiteral("fcMetadataCard"));
    metaCard->setStyleSheet(
        QStringLiteral("QFrame#fcMetadataCard { background: %1; border: 1px solid %2; "
                       "border-radius: %3px; }")
            .arg(ui::color(ui::kCard).name(), ui::color(ui::kLine).name(),
                 QString::number(ui::kRadiusCard)));
    auto *metaLayout = new QVBoxLayout(metaCard);
    metaLayout->setContentsMargins(10, 8, 10, 10);
    metaLayout->setSpacing(4);

    auto makeMetaCaption = [](const QString &text, QWidget *parent) {
        auto *cap = new QLabel(text, parent);
        cap->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: %2px; }")
                               .arg(ui::color(ui::kTextDim).name())
                               .arg(QString::number(ui::kFontSmall)));
        return cap;
    };
    auto styleMetaValue = [](QLabel *value) {
        value->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 12px; }")
                                 .arg(ui::color(ui::kText).name()));
    };
    auto makeMetaRow = [&](const QString &caption, QLabel *value) {
        auto *row = new QWidget(metaCard);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(8);
        auto *cap = makeMetaCaption(caption, row);
        cap->setMinimumWidth(64);
        rowLayout->addWidget(cap);
        rowLayout->addWidget(value, 1);
        styleMetaValue(value);
        metaLayout->addWidget(row);
    };
    metaLayout->addWidget(makeMetaCaption(tr("Metadata"), metaCard));
    makeMetaRow(tr("Name"), metaName_);
    makeMetaRow(tr("Format"), metaSummary_);
    makeMetaRow(tr("Duration"), metaDuration_);
    makeMetaRow(tr("Proxy"), metaProxy_);

    auto *buttons = new QHBoxLayout();
    buttons->addWidget(importButton_);
    buttons->addWidget(removeButton_, 1);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);
    layout->addWidget(feedbackBanner_);
    layout->addWidget(feedbackLabel_);
    layout->addWidget(filterRow);
    layout->addWidget(list_, 1);
    layout->addLayout(buttons);
    layout->addWidget(metaCard);

    // Lazy thumbnails (#28): batch the not-yet-requested paths through a
    // short debounce timer so a burst of imports emits one sweep. Created
    // BEFORE applyViewMode() below on purpose: applyViewMode's tail calls
    // requestMissingThumbnails(), which restarts this timer - constructing
    // it later made the ctor dereference a null QTimer inside Qt5Core
    // (QTimer::start on a null this, the 20260919 startup crash).
    thumbnailTimer_ = new QTimer(this);
    thumbnailTimer_->setSingleShot(true);
    connect(thumbnailTimer_, &QTimer::timeout, this, [this] { pumpThumbnailRequests(); });

    // Restore the persisted view mode (#28) before any rows arrive.
    const QString storedView = QSettings().value(QStringLiteral("project/viewMode")).toString();
    applyViewMode(storedView == QStringLiteral("grid"));

    // "/" focuses the filter (suggestion #31).
    auto *filterShortcut = new QShortcut(QKeySequence("/"), this);
    filterShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(filterShortcut, &QShortcut::activated, this, [this] {
        filterEdit_->setFocus();
        filterEdit_->selectAll();
    });

    // Empty library overlay (#61), pinned to the list viewport. Upgraded
    // to the EmptyState v2 language: image glyph + sentence-case copy.
    emptyState_ = new fc::EmptyState(list_->viewport());
    emptyState_->setIcon(
        icons::makeIcon("image", ui::color(ui::kTextDisabled), 36, devicePixelRatioF()), 36,
        devicePixelRatioF());
    emptyState_->setTitle(tr("No media yet"));
    emptyState_->setHint(tr("Import files or drop them here"));
    emptyState_->setAttribute(Qt::WA_TransparentForMouseEvents);
    emptyState_->hide();
    list_->viewport()->installEventFilter(this);

    connect(importButton_, &QPushButton::clicked, this, &ProjectPanel::importRequested);
    connect(removeButton_, &QPushButton::clicked, this, [this] {
        const int row = list_->currentRow();
        if (row >= 0) {
            // takeItem unparents the row WITHOUT deleting it - the
            // widget item must be freed here or every Remove leaks one
            // QListWidgetItem (the Qt idiom: takeItem + delete).
            const QString path = list_->item(row)->data(kPathRole).toString();
            delete list_->takeItem(row);
            library_.removeAt(row);
            // Allow a re-import of the same file to re-request its
            // thumbnail later in this session.
            thumbnailRequested_.remove(path);
            removeButton_->setEnabled(false);
            metaName_->setText(tr("-"));
            metaSummary_->setText(tr("-"));
            metaDuration_->setText(tr("-"));
            metaProxy_->setText(tr("-"));
            updateEmptyState();
        }
    });
    connect(list_, &QListWidget::itemSelectionChanged, this, &ProjectPanel::onSelectionChanged);
    connect(list_, &QListWidget::itemActivated, this, &ProjectPanel::onItemActivated);
    connect(list_, &QListWidget::customContextMenuRequested, this, &ProjectPanel::onContextMenu);
    viewSwitch_->onSelected([this](int index) { applyViewMode(index == 1); });
    connect(filterEdit_, &QLineEdit::textChanged, this, [this] { applyFilter(); });
    connect(typeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this] { applyFilter(); });
}

QIcon ProjectPanel::makePlaceholderIcon(bool audioOnly) const {
    QImage image(kThumbnailWidth, kThumbnailHeight, QImage::Format_ARGB32);
    image.fill(fc::ui::color(fc::ui::kSurface2));
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(fc::ui::color(fc::ui::kLine));
    painter.setBrush(fc::ui::color(fc::ui::kSurface3));
    painter.drawRoundedRect(QRect(0, 0, kThumbnailWidth - 1, kThumbnailHeight - 1), 4, 4);
    painter.setPen(Qt::NoPen);
    if (audioOnly) {
        // Audio glyph: a wave of bars over the chip.
        painter.setBrush(fc::ui::withAlpha(fc::ui::kTextDim, 170));
        const int centerY = kThumbnailHeight / 2;
        const int barWidth = 3;
        const int gap = 3;
        int x = kThumbnailWidth / 2 - 5 * (barWidth + gap) / 2 + gap;
        const int heights[5] = {8, 14, 20, 12, 6};
        for (int i = 0; i < 5; ++i, x += barWidth + gap) {
            painter.drawRect(QRect(x, centerY - heights[i] / 2, barWidth, heights[i]));
        }
    } else {
        // Film glyph: sprocket holes plus a centered play triangle.
        painter.setBrush(fc::ui::withAlpha(fc::ui::kTextDim, 150));
        for (int y = 4; y + 4 <= kThumbnailHeight; y += 10) {
            painter.drawRect(QRect(4, y, 3, 4));
            painter.drawRect(QRect(kThumbnailWidth - 7, y, 3, 4));
        }
        painter.setBrush(fc::ui::color(fc::ui::kTextDim));
        QPolygon play;
        play << QPoint(kThumbnailWidth / 2 - 7, kThumbnailHeight / 2 - 8)
             << QPoint(kThumbnailWidth / 2 + 9, kThumbnailHeight / 2)
             << QPoint(kThumbnailWidth / 2 - 7, kThumbnailHeight / 2 + 8);
        painter.drawPolygon(play);
    }
    return QIcon(QPixmap::fromImage(image));
}

bool ProjectPanel::eventFilter(QObject *watched, QEvent *event) {
    if (watched == list_->viewport() && event->type() == QEvent::Resize) {
        // Keep the empty state covering the viewport through resizes.
        emptyState_->setGeometry(emptyState_->parentWidget()->rect());
        emptyState_->raise();
    }
    return QWidget::eventFilter(watched, event);
}

void ProjectPanel::applyViewMode(bool grid) {
    // Sync the segmented control WITHOUT re-entering applyViewMode:
    // the target segment is signal-blocked while setCurrent() flips it,
    // and the sibling's auto-uncheck only reports "off" (which the
    // onSelected handler ignores).
    if (auto *target = viewSwitch_->button(grid ? 1 : 0)) {
        target->blockSignals(true);
        viewSwitch_->setCurrent(grid ? 1 : 0);
        target->blockSignals(false);
    }
    list_->setStyleSheet(librarySheet(grid));
    // ViewMode (ListMode/IconMode) is a QListView enum, NOT a
    // QAbstractItemView one (CI project_panel.cpp:288).
    list_->setViewMode(grid ? QListView::IconMode : QListView::ListMode);
    list_->setIconSize(QSize(kThumbnailWidth, kThumbnailHeight));
    list_->setUniformItemSizes(true);
    list_->setWrapping(true);
    list_->setSpacing(8);
    if (grid) {
        list_->setGridSize(QSize(kGridCellWidth, kGridCellHeight));
    } else {
        list_->setGridSize(QSize()); // invalid size = per-item height
    }
    QSettings().setValue(QStringLiteral("project/viewMode"),
                         grid ? QStringLiteral("grid") : QStringLiteral("list"));
    // Rows hidden until now may never have been announced; sweep again.
    requestMissingThumbnails();
}

void ProjectPanel::addMedia(const fc::MediaItem &item) {
    library_.add(item);

    auto *row = new QListWidgetItem(item.displayName, list_);
    row->setData(kPathRole, item.path);
    row->setToolTip(item.summary);
    // Placeholder icon (#28) until the coordinator's decoded thumbnail
    // arrives through setThumbnail().
    row->setIcon(makePlaceholderIcon(isAudioOnly(item)));
    applyFilter(); // respect an active filter for the new row
    list_->setCurrentItem(row);
    updateEmptyState();
    requestMissingThumbnails();
}

void ProjectPanel::setThumbnail(const QString &path, const QImage &thumbnail) {
    for (int i = 0; i < list_->count(); ++i) {
        if (list_->item(i)->data(kPathRole).toString() == path) {
            list_->item(i)->setIcon(QPixmap::fromImage(thumbnail.scaled(
                kThumbnailWidth, kThumbnailHeight, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
            return;
        }
    }
}

void ProjectPanel::setUsageCounts(const QHash<QString, int> &pathToCount) {
    for (int i = 0; i < list_->count(); ++i) {
        QListWidgetItem *row = list_->item(i);
        const QString path = row->data(kPathRole).toString();
        row->setData(kUsageCountRole, pathToCount.value(path, 0));
    }
}

void ProjectPanel::showImportFeedback(int added, int skipped) {
    if (skipped > 0) {
        feedbackLabel_->hide();
        feedbackBanner_->showError(tr("%1 file(s) skipped - already in the library").arg(skipped));
    } else {
        feedbackBanner_->clear();
        feedbackLabel_->setText(tr("Added %1 file(s)").arg(added));
        feedbackLabel_->show();
    }
    feedbackTimer_->start(4000); // auto-hide both surfaces
}

void ProjectPanel::requestMissingThumbnails() {
    thumbnailTimer_->start(250); // restarts the debounce window
}

void ProjectPanel::pumpThumbnailRequests() {
    // Emit thumbnailRequested at most once per path per session.
    for (int i = 0; i < list_->count(); ++i) {
        const QString path = list_->item(i)->data(kPathRole).toString();
        if (path.isEmpty() || thumbnailRequested_.contains(path)) {
            continue;
        }
        thumbnailRequested_.insert(path);
        emit thumbnailRequested(path);
    }
}

void ProjectPanel::applyFilter() {
    const QString needle = filterEdit_->text();
    const int type = typeCombo_->currentIndex(); // 0 all, 1 video, 2 audio
    for (int i = 0; i < list_->count(); ++i) {
        QListWidgetItem *row = list_->item(i);
        const fc::MediaItem *item =
            library_.at(library_.indexOfPath(row->data(kPathRole).toString()));
        bool visible = needle.isEmpty() || row->text().contains(needle, Qt::CaseInsensitive);
        if (visible && type == 1) {
            visible = item != nullptr && !isAudioOnly(*item);
        } else if (visible && type == 2) {
            visible = item != nullptr && isAudioOnly(*item);
        }
        row->setHidden(!visible);
    }
}

void ProjectPanel::updateEmptyState() {
    emptyState_->refresh(list_->count() == 0);
}

void ProjectPanel::onSelectionChanged() {
    const int row = list_->currentRow();
    fc::MediaItem *item = library_.at(row);
    removeButton_->setEnabled(item != nullptr);
    if (!item) {
        return;
    }
    metaName_->setText(QFileInfo(item->path).fileName());
    metaSummary_->setText(item->summary);
    // Duration readout with the source fps when the probe found video
    // (audio-only files have no frame rate to show).
    if (item->durationSeconds > 0.0) {
        metaDuration_->setText(item->fps > 0.0 ? tr("%1 s @ %2 fps")
                                                     .arg(item->durationSeconds, 0, 'f', 2)
                                                     .arg(item->fps, 0, 'f', 3)
                                               : tr("%1 s").arg(item->durationSeconds, 0, 'f', 2));
    } else {
        metaDuration_->setText(tr("-"));
    }
    metaProxy_->setText(item->hasProxy() ? tr("proxy: ready") : tr("proxy: none"));
}

void ProjectPanel::onItemActivated(QListWidgetItem *item) {
    if (item) {
        emit loadRequested(item->data(kPathRole).toString());
    }
}

void ProjectPanel::onContextMenu(const QPoint &pos) {
    QListWidgetItem *item = list_->itemAt(pos);
    if (!item) {
        return;
    }
    const QString path = item->data(kPathRole).toString();
    const int index = library_.indexOfPath(path);
    const bool hasProxy = index >= 0 && library_.at(index)->hasProxy();

    QMenu menu(this);
    QAction *load = menu.addAction(tr("Open in Program Monitor"));
    QAction *proxy = menu.addAction(hasProxy ? tr("Re-generate Proxy") : tr("Generate 360p Proxy"));
    QAction *chosen = menu.exec(list_->mapToGlobal(pos));
    if (chosen == load) {
        emit loadRequested(path);
    } else if (chosen == proxy) {
        emit proxyRequested(path);
    }
}
