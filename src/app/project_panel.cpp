#include "project_panel.h"

#include <QAction>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QVBoxLayout>

namespace {
constexpr int kThumbnailWidth = 96;
constexpr int kThumbnailHeight = 54;
} // namespace

ProjectPanel::ProjectPanel(QWidget *parent) : QWidget(parent) {
    list_ = new QListWidget(this);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    list_->setIconSize(QSize(kThumbnailWidth, kThumbnailHeight));

    importButton_ = new QPushButton(tr("Import Media..."), this);
    removeButton_ = new QPushButton(tr("Remove"), this);
    removeButton_->setEnabled(false);

    metaName_ = new QLabel(tr("-"), this);
    metaSummary_ = new QLabel(tr("-"), this);
    metaDuration_ = new QLabel(tr("-"), this);
    metaProxy_ = new QLabel(tr("-"), this);
    metaName_->setWordWrap(true);
    metaSummary_->setWordWrap(true);

    auto *metaBox = new QWidget(this);
    auto *metaLayout = new QVBoxLayout(metaBox);
    metaLayout->setContentsMargins(0, 0, 0, 0);
    metaLayout->setSpacing(2);
    metaLayout->addWidget(new QLabel(tr("<b>Metadata</b>"), this));
    metaLayout->addWidget(metaName_);
    metaLayout->addWidget(metaSummary_);
    metaLayout->addWidget(metaDuration_);
    metaLayout->addWidget(metaProxy_);
    metaLayout->addStretch(1);

    auto *buttons = new QHBoxLayout();
    buttons->addWidget(importButton_);
    buttons->addWidget(removeButton_, 1);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->addWidget(list_, 1);
    layout->addLayout(buttons);
    layout->addWidget(metaBox);

    connect(importButton_, &QPushButton::clicked, this, &ProjectPanel::importRequested);
    connect(removeButton_, &QPushButton::clicked, this, [this] {
        const int row = list_->currentRow();
        if (row >= 0) {
            list_->takeItem(row);
            library_.removeAt(row);
            removeButton_->setEnabled(false);
            metaName_->setText(tr("-"));
            metaSummary_->setText(tr("-"));
            metaDuration_->setText(tr("-"));
            metaProxy_->setText(tr("-"));
        }
    });
    connect(list_, &QListWidget::itemSelectionChanged, this, &ProjectPanel::onSelectionChanged);
    connect(list_, &QListWidget::itemActivated, this, &ProjectPanel::onItemActivated);
    connect(list_, &QListWidget::customContextMenuRequested, this, &ProjectPanel::onContextMenu);
}

void ProjectPanel::addMedia(const fc::MediaItem &item) {
    library_.add(item);

    auto *row = new QListWidgetItem(item.displayName, list_);
    row->setData(Qt::UserRole, item.path);
    row->setToolTip(item.summary);
    list_->setCurrentItem(row);
}

void ProjectPanel::setThumbnail(const QString &path, const QImage &thumbnail) {
    for (int i = 0; i < list_->count(); ++i) {
        if (list_->item(i)->data(Qt::UserRole).toString() == path) {
            list_->item(i)->setIcon(QPixmap::fromImage(thumbnail.scaled(
                kThumbnailWidth, kThumbnailHeight, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
            return;
        }
    }
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
    metaDuration_->setText(QString("%1 s").arg(item->durationSeconds, 0, 'f', 2));
    metaProxy_->setText(item->hasProxy() ? tr("proxy: ready") : tr("proxy: none"));
}

void ProjectPanel::onItemActivated(QListWidgetItem *item) {
    if (item) {
        emit loadRequested(item->data(Qt::UserRole).toString());
    }
}

void ProjectPanel::onContextMenu(const QPoint &pos) {
    QListWidgetItem *item = list_->itemAt(pos);
    if (!item) {
        return;
    }
    const QString path = item->data(Qt::UserRole).toString();
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
