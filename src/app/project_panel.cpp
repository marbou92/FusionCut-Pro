#include "project_panel.h"

#include <QApplication>
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
#include <QStyle>
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
// kRadiusCard (#0-k: grid cells are cards, same 10px language as the
// quick-mode template cards and the metadata card) and inset their
// thumbnails a touch more so the selection reads like a card; list rows
// stay at the 6px row rounding (rows are chips, not cards). The app-wide
// sheet styles the same states at 24px/6px; this local sheet wins where
// it is set, which is the intent here.
QString librarySheet(bool grid) {
    const QString radius = grid ? QString::number(ui::kRadiusCard) : QStringLiteral("6px");
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

// mm:ss for the duration chip (#168). A local two-liner on purpose: the
// fc::timecode engine speaks HH:MM:SS:FF over a rational FrameRate -
// more than a 10px thumbnail chip needs, and this TU never included
// core headers.
QString formatMmSs(double seconds) {
    if (seconds < 0.0) {
        seconds = 0.0;
    }
    const int total = static_cast<int>(seconds + 0.5);
    return QStringLiteral("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QLatin1Char('0'));
}

// #163: the number part of the frame-rate row. Full probe precision with
// trailing zeros trimmed, so 24.000 reads "24" and 23.976 stays "23.976"
// (rounding 29.97 to one decimal would print "30" and misreport the rate).
QString formatFpsNumber(double fps) {
    QString text = QString::number(fps, 'f', 3);
    while (text.endsWith(QLatin1Char('0'))) {
        text.chop(1);
    }
    if (text.endsWith(QLatin1Char('.'))) {
        text.chop(1);
    }
    return text;
}

// Item painter: the usage chip (#29, 22-b geometry kept verbatim) plus
// the round-3 additions - a duration chip pinned to the thumbnail's
// bottom-right in grid mode (#168), ONE proxy-pill language in both view
// modes (#169), and list rows that append the duration after the name in
// kTextDim (#168; the rows show the display name - the summary lives in
// the tooltip). Proxy and duration read LIVE from the library through
// the panel pointer on purpose: the coordinator writes proxyPath
// straight onto the MediaItem after a proxy transcode (no panel method
// is called), so per-row role copies would go stale. The lookup is a
// linear path scan bounded by the library size - trivial at library
// scales and always current.
class UsageCountDelegate : public QStyledItemDelegate {
public:
    explicit UsageCountDelegate(ProjectPanel *panel, QObject *parent = nullptr)
        : QStyledItemDelegate(parent), panel_(panel) {}

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        const fc::MediaItem *item = itemFor(index);
        const QString duration =
            (item && item->durationSeconds > 0.0) ? formatMmSs(item->durationSeconds) : QString();
        const bool proxy = item != nullptr && item->hasProxy();
        if (isGridView()) {
            paintGrid(painter, option, index, duration, proxy);
        } else {
            paintList(painter, option, index, duration, proxy);
        }
    }

private:
    bool isGridView() const {
        const auto *view = qobject_cast<const QListView *>(parent());
        return view != nullptr && view->viewMode() == QListView::IconMode;
    }

    const fc::MediaItem *itemFor(const QModelIndex &index) const {
        if (panel_ == nullptr) {
            return nullptr;
        }
        const int i = panel_->library().indexOfPath(index.data(kPathRole).toString());
        return panel_->library().at(i);
    }

    // The style's own thumbnail placement (falls back to the centered
    // 96x54 area the icon size implies on odd styles).
    QRect thumbnailRect(const QStyleOptionViewItem &option, const QModelIndex &index) const {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QWidget *widget = option.widget;
        QStyle *style = widget != nullptr ? widget->style() : QApplication::style();
        QRect rect = style->subElementRect(QStyle::SE_ItemViewItemDecoration, &opt, widget);
        if (rect.isEmpty()) {
            rect = QRect(option.rect.left() + (option.rect.width() - kThumbnailWidth) / 2,
                         option.rect.top(), kThumbnailWidth, kThumbnailHeight);
        }
        return rect;
    }

    void paintGrid(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index,
                   const QString &duration, bool proxy) const {
        QStyledItemDelegate::paint(painter, option, index); // background + thumbnail + caption
        paintUsageChip(painter, option, index);
        const QRect thumb = thumbnailRect(option, index);
        const qreal dpr = option.widget != nullptr ? option.widget->devicePixelRatioF() : 1.0;
        // #168: duration chip pinned to the thumbnail's bottom-right
        // (black 55% pill, 10px white text). Draws over the usage chip's
        // left edge in the rare both-present case; the usage chip keeps
        // its documented 22-b geometry.
        if (!duration.isEmpty()) {
            QFont chipFont = option.font;
            chipFont.setPixelSize(10);
            const QFontMetrics metrics(chipFont);
            const int textWidth = metrics.horizontalAdvance(duration);
            const QRect chip(thumb.right() - textWidth - 8 - 3, thumb.bottom() - 14 - 2,
                             textWidth + 8, 14);
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(0, 0, 0, 140)); // black @ 55%
            painter->drawRoundedRect(chip, 4, 4);
            painter->setFont(chipFont);
            painter->setPen(Qt::white);
            painter->drawText(chip, Qt::AlignCenter, duration);
            painter->restore();
        }
        // #169: proxy pill at the thumbnail's top-right, same pill
        // language as list mode.
        if (proxy) {
            bool withIcon = true;
            QSize pillSize = proxyPillSize(option.font, withIcon);
            if (pillSize.width() > thumb.width() - 6) {
                withIcon = false; // drop the check glyph when the pill has no room
                pillSize = proxyPillSize(option.font, withIcon);
            }
            const QRect pill(thumb.right() - pillSize.width() - 3, thumb.top() + 3,
                             pillSize.width(), pillSize.height());
            paintProxyPill(painter, pill, option.font, dpr, withIcon);
        }
    }

    void paintList(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index,
                   const QString &duration, bool proxy) const {
        // Background + icon through the style with the display text
        // cleared, so this delegate owns the text: nothing overlaps -
        // the name elides ahead of the duration (#168) and the right
        // strip parks the proxy pill (#169) and the usage chip (#29).
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QString text = opt.text;
        opt.text.clear();
        const QWidget *widget = option.widget;
        QStyle *style = widget != nullptr ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

        const QFontMetrics metrics(option.font);
        int textRight = option.rect.right() - 6;

        // Usage chip: the exact 22-b geometry (right edge, v-centered).
        const int count = index.data(kUsageCountRole).toInt();
        if (count >= 1) {
            const QString countText = QString::number(count);
            const int chipWidth = metrics.horizontalAdvance(countText) + 8;
            const int chipHeight = option.rect.height() - 4 < 14 ? option.rect.height() - 4 : 14;
            if (chipWidth >= 14 && option.rect.width() >= chipWidth + 6) {
                const QRect chip(option.rect.right() - chipWidth - 2,
                                 option.rect.top() + (option.rect.height() - chipHeight) / 2,
                                 chipWidth, chipHeight);
                painter->save();
                painter->setRenderHint(QPainter::Antialiasing);
                painter->setPen(Qt::NoPen);
                painter->setBrush(ui::tint(ui::kAccent));
                painter->drawRoundedRect(chip, chipHeight / 2, chipHeight / 2);
                painter->setPen(ui::color(ui::kText));
                painter->setFont(option.font);
                painter->drawText(chip, Qt::AlignCenter, countText);
                painter->restore();
                textRight = qMin(textRight, chip.left() - 8);
            }
        }

        // #169: the same proxy pill as grid mode, docked before the
        // usage chip (kept when the name would starve).
        if (proxy) {
            const qreal dpr = widget != nullptr ? widget->devicePixelRatioF() : 1.0;
            bool withIcon = true;
            QSize pillSize = proxyPillSize(option.font, withIcon);
            if (pillSize.width() > textRight - option.rect.left() - 24) {
                withIcon = false;
                pillSize = proxyPillSize(option.font, withIcon);
            }
            if (pillSize.width() <= textRight - option.rect.left() - 24) {
                const QRect pill(textRight - pillSize.width(),
                                 option.rect.top() + (option.rect.height() - pillSize.height()) / 2,
                                 pillSize.width(), pillSize.height());
                paintProxyPill(painter, pill, option.font, dpr, withIcon);
                textRight = qMin(textRight, pill.left() - 8);
            }
        }

        // The style's own text zone (right of the thumbnail).
        QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget);
        if (textRect.isEmpty()) {
            textRect = option.rect.adjusted(kThumbnailWidth + 8, 0, 0, 0);
        }
        if (textRight < textRect.right()) {
            textRect.setRight(textRight);
        }

        // #168: the duration rides after the (elided) name in kTextDim.
        QFont dimFont = option.font;
        dimFont.setPixelSize(ui::kFontSmall);
        const QFontMetrics dimMetrics(dimFont);
        const int durationWidth =
            duration.isEmpty() ? 0 : dimMetrics.horizontalAdvance(duration) + 8;
        const QString elided = text.isEmpty()
                                   ? QString()
                                   : metrics.elidedText(text, Qt::ElideRight,
                                                        qMax(0, textRect.width() - durationWidth));
        painter->save();
        painter->setFont(option.font);
        painter->setPen(option.state & QStyle::State_Selected
                            ? ui::color(ui::kText)
                            : option.palette.color(QPalette::Text));
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, elided);
        if (!duration.isEmpty()) {
            painter->setFont(dimFont);
            painter->setPen(ui::color(ui::kTextDim));
            const int dx =
                textRect.left() + metrics.horizontalAdvance(elided) + (elided.isEmpty() ? 0 : 6);
            painter->drawText(QRect(dx, textRect.top(), durationWidth, textRect.height()),
                              Qt::AlignLeft | Qt::AlignVCenter, duration);
        }
        painter->restore();
    }

    // Usage chip painter (#29): 22-b geometry kept verbatim for grid
    // mode; chips only appear for count >= 1.
    void paintUsageChip(QPainter *painter, const QStyleOptionViewItem &option,
                        const QModelIndex &index) const {
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
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        painter->setBrush(ui::tint(ui::kAccent));
        painter->drawRoundedRect(chip, chipHeight / 2, chipHeight / 2);
        painter->setPen(ui::color(ui::kText));
        painter->setFont(option.font);
        painter->drawText(chip, Qt::AlignCenter, text);
        painter->restore();
    }

    // #169: one pill language for both view modes - 10px kSuccess text
    // on a tint(kSuccess) fill, kRadiusPill rounding, check glyph when
    // the pill has room.
    QSize proxyPillSize(const QFont &baseFont, bool withIcon) const {
        QFont font = baseFont;
        font.setPixelSize(10);
        const QFontMetrics metrics(font);
        return QSize(10 + (withIcon ? 13 : 0) + metrics.horizontalAdvance(tr("Proxy")), 14);
    }

    void paintProxyPill(QPainter *painter, const QRect &rect, const QFont &baseFont, qreal dpr,
                        bool withIcon) const {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        painter->setBrush(ui::tint(ui::kSuccess));
        painter->drawRoundedRect(rect, ui::kRadiusPill, ui::kRadiusPill);
        QFont font = baseFont;
        font.setPixelSize(10);
        painter->setFont(font);
        painter->setPen(ui::color(ui::kSuccess));
        QRect textRect = rect;
        if (withIcon) {
            const qreal ratio = dpr > 0 ? dpr : 1.0;
            const QPixmap check = icons::makeIcon("check", ui::color(ui::kSuccess), 10, ratio)
                                      .pixmap(qRound(10 * ratio)); // DPR-tagged pixmap
            painter->drawPixmap(rect.left() + 5, rect.top() + (rect.height() - 10) / 2, check);
            textRect.setLeft(rect.left() + 5 + 13);
        }
        painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignHCenter, tr("Proxy"));
        painter->restore();
    }

    ProjectPanel *panel_ = nullptr;
};

} // namespace

ProjectPanel::ProjectPanel(QWidget *parent) : QWidget(parent) {
    list_ = new QListWidget(this);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    list_->setIconSize(QSize(kThumbnailWidth, kThumbnailHeight));
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    // Usage chips (#29): one delegate for both view modes; it also
    // paints the duration chip + proxy pill (#168/#169) from live
    // library state.
    list_->setItemDelegate(new UsageCountDelegate(this, list_));

    importButton_ = new QPushButton(tr("Import Media..."), this);
    // #66: Import is this panel's one primary action - the app-wide sheet
    // lights fcAccent buttons with the accent fill + dark text.
    importButton_->setProperty("fcAccent", true);
    removeButton_ = new QPushButton(tr("Remove"), this);
    removeButton_->setEnabled(false);

    metaName_ = new QLabel(tr("-"), this);
    metaSummary_ = new QLabel(tr("-"), this);
    metaDuration_ = new QLabel(tr("-"), this);
    metaFps_ = new QLabel(tr("-"), this); // #163
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
    makeMetaRow(tr("Frame rate"), metaFps_); // #163: "24 fps" style; fps moved out of Duration
    makeMetaRow(tr("Proxy"), metaProxy_);

    // Item-count caption (#162): a passive right-aligned label under
    // the header; refreshed from the library by updateItemCount().
    itemCount_ = new QLabel(this);
    itemCount_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    itemCount_->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: %2px; }")
                                  .arg(ui::color(ui::kTextDim).name())
                                  .arg(QString::number(ui::kFontSmall)));
    itemCount_->hide();

    auto *buttons = new QHBoxLayout();
    buttons->addWidget(importButton_);
    buttons->addWidget(removeButton_, 1);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);
    layout->addWidget(feedbackBanner_);
    layout->addWidget(feedbackLabel_);
    layout->addWidget(filterRow);
    layout->addWidget(itemCount_); // #162: under the header, right-aligned
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
            metaFps_->setText(tr("-"));
            metaProxy_->setText(tr("-"));
            updateEmptyState();
            updateItemCount(); // #162
        }
    });
    connect(list_, &QListWidget::itemSelectionChanged, this, &ProjectPanel::onSelectionChanged);
    connect(list_, &QListWidget::itemActivated, this, &ProjectPanel::onItemActivated);
    connect(list_, &QListWidget::customContextMenuRequested, this, &ProjectPanel::onContextMenu);
    viewSwitch_->onSelected([this](int index) { applyViewMode(index == 1); });
    connect(filterEdit_, &QLineEdit::textChanged, this, [this] { applyFilter(); });
    connect(typeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this] { applyFilter(); });

    // Boot states (#165 verification fix): the empty-state overlay only
    // refreshed from addMedia/remove before, so a fresh empty library
    // showed neither the rows nor "No media yet" until the first import.
    updateEmptyState();
    updateItemCount();
}

QIcon ProjectPanel::makePlaceholderIcon(bool audioOnly) const {
    // #206: rendered at the device pixel ratio and DPR-tagged, so the
    // stand-in stays crisp at 125/150% Windows scaling like the decoded
    // thumbnails are expected to.
    const qreal dpr = devicePixelRatioF() > 0.0 ? devicePixelRatioF() : 1.0;
    QImage image(qRound(kThumbnailWidth * dpr), qRound(kThumbnailHeight * dpr),
                 QImage::Format_ARGB32);
    image.fill(fc::ui::color(fc::ui::kSurface2));
    QPainter painter(&image);
    painter.scale(dpr, dpr);
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
    QPixmap pixmap = QPixmap::fromImage(image);
    pixmap.setDevicePixelRatio(dpr);
    return QIcon(pixmap);
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
    updateItemCount(); // #162
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

void ProjectPanel::updateItemCount() {
    const int count = library_.items().size();
    if (count <= 0) {
        itemCount_->setText(QString());
        itemCount_->hide(); // the empty state owns the "nothing here" voice
        return;
    }
    itemCount_->setText(count == 1 ? tr("1 item") : tr("%1 items").arg(count));
    itemCount_->show();
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
    updateItemCount();           // #162: import feedback path keeps the caption current
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
    // Duration readout in plain seconds; the frame rate moved to its own
    // row below (#163) so the card states each fact exactly once.
    metaDuration_->setText(
        item->durationSeconds > 0.0 ? tr("%1 s").arg(item->durationSeconds, 0, 'f', 2) : tr("-"));
    // #163: source fps when the probe found one (audio-only files have
    // no frame rate to show).
    metaFps_->setText(item->fps > 0.0 ? tr("%1 fps").arg(formatFpsNumber(item->fps)) : tr("-"));
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
    const qreal dpr = devicePixelRatioF();
    // #170: icons on the existing items only (play = open in the
    // monitor, film = the proxy transcode).
    QAction *load = menu.addAction(icons::makeIcon("play", ui::color(ui::kText), 14, dpr),
                                   tr("Open in Program Monitor"));
    QAction *proxy = menu.addAction(icons::makeIcon("film", ui::color(ui::kText), 14, dpr),
                                    hasProxy ? tr("Re-generate Proxy") : tr("Generate 360p Proxy"));
    QAction *chosen = menu.exec(list_->mapToGlobal(pos));
    if (chosen == load) {
        emit loadRequested(path);
    } else if (chosen == proxy) {
        emit proxyRequested(path);
    }
}
