#pragma once

#include <QImage>
#include <QListWidget>
#include <QWidget>

#include "media_item.h"

class QLabel;
class QListWidgetItem;
class QPushButton;

// Pro Mode left panel: imported media list with import/remove, and a
// metadata readout for the selected item. Double-click loads a clip into
// the program monitor; the context menu offers proxy generation.
class ProjectPanel : public QWidget {
    Q_OBJECT

public:
    explicit ProjectPanel(QWidget *parent = nullptr);

    void addMedia(const fc::MediaItem &item);
    void setThumbnail(const QString &path, const QImage &thumbnail);
    fc::MediaLibrary &library() { return library_; }

signals:
    // Requests loading a clip into the program monitor (path chosen by
    // the receiver: proxy when available, else source).
    void loadRequested(const QString &sourcePath);
    void proxyRequested(const QString &sourcePath);
    void importRequested();

private slots:
    void onSelectionChanged();
    void onItemActivated(QListWidgetItem *item);
    void onContextMenu(const QPoint &pos);

private:
    fc::MediaLibrary library_;
    QListWidget *list_ = nullptr;
    QPushButton *importButton_ = nullptr;
    QPushButton *removeButton_ = nullptr;
    QLabel *metaName_ = nullptr;
    QLabel *metaSummary_ = nullptr;
    QLabel *metaDuration_ = nullptr;
    QLabel *metaProxy_ = nullptr;
};
