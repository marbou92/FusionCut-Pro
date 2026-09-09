#include "system_fonts.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <set>
#include <vector>

#include "emoji.h"

namespace fc {

QStringList systemFontDirectories() {
    QStringList dirs;
#ifdef Q_OS_WIN
    const QString windir = qEnvironmentVariable("WINDIR");
    if (!windir.isEmpty()) {
        dirs << windir + "/Fonts";
    }
    const QString local = qEnvironmentVariable("LOCALAPPDATA");
    if (!local.isEmpty()) {
        dirs << local + "/Microsoft/Windows/Fonts";
    }
#elif defined(Q_OS_MAC)
    dirs << "/System/Library/Fonts"
         << "/Library/Fonts" << QDir::homePath() + "/Library/Fonts";
#else
    dirs << "/usr/share/fonts"
         << "/usr/local/share/fonts" << QDir::homePath() + "/.local/share/fonts"
         << QDir::homePath() + "/.fonts";
#endif
    return dirs;
}

QStringList preferredEmojiFontFamilies() {
#ifdef Q_OS_WIN
    return {"Segoe UI Emoji", "Noto Color Emoji", "JoyPixels", "Twemoji Mozilla"};
#elif defined(Q_OS_MAC)
    return {"Apple Color Emoji", "Noto Color Emoji", "Segoe UI Emoji"};
#else
    return {"Noto Color Emoji", "Emoji One", "JoyPixels", "Twemoji Mozilla"};
#endif
}

namespace {

// Reads the first `cap` bytes of a file (the sfnt table directory of
// even a maximal font lives in the first ~4 KB; a collection header
// adds a handful more). Empty on any open/read failure.
std::vector<uint8_t> readHead(const QString &path, qint64 cap) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::vector<uint8_t>();
    }
    const QByteArray blob = file.read(cap);
    return std::vector<uint8_t>(blob.constData(), blob.constData() + blob.size());
}

// Reads the whole file into bytes (the coverage check parses the
// tables, so the full payload is needed).
std::vector<uint8_t> readAll(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::vector<uint8_t>();
    }
    const QByteArray blob = file.readAll();
    return std::vector<uint8_t>(blob.constData(), blob.constData() + blob.size());
}

} // namespace

QVector<SystemEmojiFont> discoverEmojiFonts() {
    QVector<SystemEmojiFont> found;
    std::set<QString> seenFamilies;
    const QStringList nameFilters = {"*.ttf", "*.ttc", "*.otf"};
    for (const QString &dir : systemFontDirectories()) {
        QDirIterator it(dir, nameFilters, QDir::Files, QDirIterator::Subdirectories);
        QFileInfoList infos;
        while (it.hasNext()) {
            infos << QFileInfo(it.next());
        }
        // Filesystem iteration order is not stable: sort by full path.
        std::sort(infos.begin(), infos.end(), [](const QFileInfo &a, const QFileInfo &b) {
            return a.filePath() < b.filePath();
        });
        for (const QFileInfo &fi : infos) {
            if (fi.size() < 12 || fi.size() > 400 * 1024 * 1024) {
                continue; // not a font / absurdly large
            }
            // Cheap probe first: no bitmap-emoji tables in the
            // directory -> skip without reading the whole file.
            const std::vector<uint8_t> head = readHead(fi.filePath(), 65536);
            const EmojiFontFormat fmt = sfntBitmapEmojiFormat(head.data(), head.size(), fi.size());
            if (fmt == EmojiFontFormat::None) {
                continue;
            }
            // Full parse + emoji coverage check (U+1F600 grinning face
            // must map to a glyph in the font).
            const std::vector<uint8_t> bytes = readAll(fi.filePath());
            if (bytes.empty()) {
                continue;
            }
            EmojiFont font;
            if (!font.load(bytes.data(), bytes.size())) {
                continue; // structurally unusable (or not emoji at all)
            }
            if (font.codepointGlyph(0x1F600) == 0) {
                continue; // bitmap strikes but no emoji coverage
            }
            const QString family =
                QString::fromStdString(sfntFamilyName(bytes.data(), bytes.size()));
            if (!family.isEmpty()) {
                if (seenFamilies.count(family) > 0) {
                    continue; // first file per family name wins
                }
                seenFamilies.insert(family);
            }
            SystemEmojiFont item;
            item.family = family.isEmpty() ? fi.completeBaseName() : family;
            item.path = fi.filePath();
            item.format = fmt == EmojiFontFormat::Cbdt ? "CBDT" : "sbix";
            found.push_back(item);
        }
    }
    return found;
}

} // namespace fc
