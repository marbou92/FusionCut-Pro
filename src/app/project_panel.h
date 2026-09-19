#pragma once

#include <QHash>
#include <QIcon>
#include <QImage>
#include <QListWidget>
#include <QSet>
#include <QWidget>

#include "media_item.h"

class QComboBox;
class QEvent;
class QLabel;
class QLineEdit;
class QListWidgetItem;
class QPushButton;
class QToolButton;
class QTimer;

namespace fc {
class EmptyState;
class ErrorBanner;
} // namespace fc

// Pro Mode left panel: imported media list with import/remove, and a
// metadata readout for the selected item. Double-click loads a clip into
// the program monitor; the context menu offers proxy generation.
//
// Views (#28): the list toggles between a compact List mode and a
// thumbnail Grid mode (persisted in QSettings "project/viewMode").
// Thumbnails (#28): rows appear immediately with a code-drawn
// placeholder icon; the panel emits thumbnailRequested(path) at most
// once per path per session (debounced 250 ms) so a coordinator can
// decode + hand the bitmap back through setThumbnail().
// Usage chips (#29): setUsageCounts() stores per-path clip counts that
// an item delegate paints as small right-edge chips in both view modes.
// Import feedback (#30): showImportFeedback() flashes a transient line
// above the list. Filtering (#31): name filter + type combo, "/" focuses
// the filter. Empty state (#61): an overlay when the library is empty.
class ProjectPanel : public QWidget {
    Q_OBJECT

public:
    explicit ProjectPanel(QWidget *parent = nullptr);

    void addMedia(const fc::MediaItem &item);
    void setThumbnail(const QString &path, const QImage &thumbnail);
    void setUsageCounts(const QHash<QString, int> &pathToCount);
    void showImportFeedback(int added, int skipped);
    fc::MediaLibrary &library() { return library_; }

signals:
    // Requests loading a clip into the program monitor (path chosen by
    // the receiver: proxy when available, else source).
    void loadRequested(const QString &sourcePath);
    void proxyRequested(const QString &sourcePath);
    void importRequested();
    // Asks the coordinator to decode a thumbnail for this library path.
    // Emitted at most once per path per session (setThumbnail delivers
    // the image back when ready).
    void thumbnailRequested(const QString &path);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onSelectionChanged();
    void onItemActivated(QListWidgetItem *item);
    void onContextMenu(const QPoint &pos);

private:
    void applyViewMode(bool grid);
    void requestMissingThumbnails();
    void pumpThumbnailRequests();
    void applyFilter();
    void updateEmptyState();
    // Code-drawn stand-in icon (#28): dark chip + film/audio glyph,
    // shown until the coordinator's decoded thumbnail arrives.
    QIcon makePlaceholderIcon(bool audioOnly) const;

    fc::MediaLibrary library_;
    QListWidget *list_ = nullptr;
    QPushButton *importButton_ = nullptr;
    QPushButton *removeButton_ = nullptr;
    QLabel *metaName_ = nullptr;
    QLabel *metaSummary_ = nullptr;
    QLabel *metaDuration_ = nullptr;
    QLabel *metaProxy_ = nullptr;

    // View mode toggle (#28) + filtering (#31) row above the list.
    QToolButton *listModeButton_ = nullptr;
    QToolButton *gridModeButton_ = nullptr;
    QLineEdit *filterEdit_ = nullptr;
    QComboBox *typeCombo_ = nullptr;

    // Import feedback (#30): one transient surface at a time, auto-hidden
    // by feedbackTimer_.
    fc::ErrorBanner *feedbackBanner_ = nullptr;
    QLabel *feedbackLabel_ = nullptr;
    QTimer *feedbackTimer_ = nullptr;

    // Lazy thumbnails (#28): paths already announced to the coordinator
    // this session + the 250 ms debounce timer batching the requests.
    QSet<QString> thumbnailRequested_;
    QTimer *thumbnailTimer_ = nullptr;

    // Empty library overlay (#61), parented to the list viewport.
    fc::EmptyState *emptyState_ = nullptr;
};
