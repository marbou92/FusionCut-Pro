#pragma once

#include <QList>
#include <QString>

#include "media_info.h"

namespace fc {

// Human-readable one-liner for a probed file (the library item's
// summary, the status-bar note on load). Shared by the decode worker's
// mediaInfo handoff and the import probe so the two can never drift.
inline QString mediaSummary(const fc::MediaInfo &info) {
    if (!info.hasVideo) {
        return QString::fromStdString("audio only");
    }
    return QString("%1x%2 %3, %4 fps, %5s")
        .arg(info.video.width)
        .arg(info.video.height)
        .arg(QString::fromStdString(info.video.codecName))
        .arg(QString::number(info.video.frameRate.toDouble(), 'f', 3))
        .arg(QString::number(info.durationSeconds(), 'f', 1));
}

// One imported media asset in the project library.
struct MediaItem {
    QString path;        // absolute source path
    QString displayName; // file base name
    QString proxyPath;   // generated 360p proxy (empty until created)
    QString summary;     // human-readable stream summary
    double durationSeconds = 0.0;
    double fps = 0.0;

    bool hasProxy() const { return !proxyPath.isEmpty(); }
};

// Simple ownership list of imported media. No signals of its own; panels
// that mutate it emit their own notifications.
class MediaLibrary {
public:
    const QList<MediaItem> &items() const { return items_; }

    int indexOfPath(const QString &path) const {
        for (int i = 0; i < items_.size(); ++i) {
            if (items_[i].path == path) {
                return i;
            }
        }
        return -1;
    }

    void add(const MediaItem &item) { items_.append(item); }

    void removeAt(int index) {
        if (index >= 0 && index < items_.size()) {
            items_.removeAt(index);
        }
    }

    MediaItem *at(int index) {
        return (index >= 0 && index < items_.size()) ? &items_[index] : nullptr;
    }

    void clear() { items_.clear(); }

private:
    QList<MediaItem> items_;
};

} // namespace fc
