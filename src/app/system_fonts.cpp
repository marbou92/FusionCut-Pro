#include "system_fonts.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include "emoji.h"
#include "emoji_clusters.h"

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

#ifdef Q_OS_WIN
namespace {

// Files registered in the Windows font registry (machine + user). A
// registered value is either a bare file name (relative to
// %WINDIR%\Fonts) or an absolute path; per-user installs are usually
// absolute. Returns the RESOLVED absolute paths, unsorted.
QStringList registeredFontFiles() {
    QStringList out;
    const QString windir = qEnvironmentVariable("WINDIR");
    const auto harvest = [&out, &windir](QSettings &settings) {
        const QStringList keys = settings.childKeys();
        for (const QString &key : keys) {
            QString value = settings.value(key).toString().trimmed();
            if (value.isEmpty()) {
                continue;
            }
            if (value.startsWith('"') && value.endsWith('"') && value.size() >= 2) {
                value = value.mid(1, value.size() - 2);
            }
            if (value.endsWith(".fon", Qt::CaseInsensitive)) {
                continue; // bitmap engine fonts, never sfnt
            }
            // A multi-value entry lists several files comma separated.
            const QStringList parts = value.split(',', Qt::SkipEmptyParts);
            for (QString part : parts) {
                part = part.trimmed();
                if (part.isEmpty()) {
                    continue;
                }
                if (!part.contains('/') && !part.contains('\\')) {
                    if (windir.isEmpty()) {
                        continue;
                    }
                    part = windir + "/Fonts/" + part;
                }
                out << QDir::cleanPath(part);
            }
        }
    };
    QSettings machine("HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts",
                      QSettings::NativeFormat);
    harvest(machine);
    QSettings user("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts",
                   QSettings::NativeFormat);
    harvest(user);
    return out;
}

} // namespace
#endif // Q_OS_WIN

namespace {

uint32_t be32(const uint8_t *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint16_t be16(const uint8_t *p) {
    return uint16_t((uint16_t(p[0]) << 8) | uint16_t(p[1]));
}

// FNV-1a 64 over a byte range (the content signature).
uint64_t fnv1a(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

// ---------------------------------------------------------------------------
// SfntFile: random-access sfnt / TrueType Collection reader.
//
// The discovery scan must inspect the table DIRECTORY, a cmap slice,
// and a name slice of every font on the machine without reading whole
// files (the color faces need the full payload anyway, but the
// outline probe would otherwise drag megabytes per candidate). This
// little reader parses each sub-font's directory (validating offsets
// against the real file length) and slices tables on demand.
// ---------------------------------------------------------------------------
class SfntFile {
public:
    bool open(const QString &path) {
        file_.setFileName(path);
        if (!file_.open(QIODevice::ReadOnly)) {
            return false;
        }
        fileSize_ = file_.size();
        if (fileSize_ < 12) {
            return false;
        }
        uint8_t head[12];
        if (readAt(0, head, sizeof(head)) != sizeof(head)) {
            return false;
        }
        const uint32_t tag = be32(head);
        if (tag == 0x74746366u) { // 'ttcf'
            uint8_t cnt[4];
            if (readAt(8, cnt, sizeof(cnt)) != sizeof(cnt)) {
                return false;
            }
            const uint32_t numFonts = be32(cnt);
            if (numFonts == 0 || numFonts > 64) {
                return false;
            }
            for (uint32_t f = 0; f < numFonts; ++f) {
                uint8_t off[4];
                if (readAt(12 + uint64_t(f) * 4, off, sizeof(off)) != sizeof(off)) {
                    continue;
                }
                const uint32_t base = be32(off);
                if (base >= 12 && uint64_t(base) + 12 <= uint64_t(fileSize_)) {
                    parseDirectory(uint64_t(base));
                }
            }
        } else if (tag == 0x00010000u || tag == 0x4F54544Fu /* 'OTTO' */ ||
                   tag == 0x74727565u /* 'true' */) {
            parseDirectory(0);
        }
        return !dirs_.empty();
    }

    qint64 fileSize() const { return fileSize_; }
    size_t subfontCount() const { return dirs_.size(); }

    // Finds the table's byte range within sub-font `sub` (offset from
    // file start + length). False when the sub-font lacks the table or
    // the directory entry lies about the file length.
    bool tableRange(const char tag[4], size_t sub, uint64_t *off, uint32_t *len) const {
        if (sub >= dirs_.size()) {
            return false;
        }
        const uint32_t want = (uint32_t(uint8_t(tag[0])) << 24) |
                              (uint32_t(uint8_t(tag[1])) << 16) | (uint32_t(uint8_t(tag[2])) << 8) |
                              uint32_t(uint8_t(tag[3]));
        for (const Entry &e : dirs_[sub].entries) {
            if (e.tag == want) {
                if (uint64_t(e.off) + uint64_t(e.len) > uint64_t(fileSize_)) {
                    return false; // lying record
                }
                *off = e.off;
                *len = e.len;
                return true;
            }
        }
        return false;
    }

    // Reads a table slice (bounded by `cap`; the caller knows how much
    // it needs - cmap and name tables are small, and a corrupt header
    // claiming gigabytes cannot make us allocate them).
    std::vector<uint8_t> readTable(const char tag[4], size_t sub, uint32_t cap) const {
        uint64_t off = 0;
        uint32_t len = 0;
        if (!tableRange(tag, sub, &off, &len)) {
            return std::vector<uint8_t>();
        }
        if (len > cap) {
            len = cap;
        }
        std::vector<uint8_t> out(len);
        if (len == 0) {
            return out;
        }
        if (readAt(off, out.data(), size_t(len)) != qint64(len)) {
            return std::vector<uint8_t>();
        }
        return out;
    }

    // Reads an arbitrary byte range (the content signature pieces).
    bool readRange(uint64_t off, size_t n, std::vector<uint8_t> &out) const {
        out.resize(n);
        return n == 0 || readAt(off, out.data(), n) == qint64(n);
    }

private:
    qint64 readAt(uint64_t off, uint8_t *dst, size_t n) const {
        if (off + n > uint64_t(fileSize_)) {
            return -1;
        }
        if (!file_.seek(qint64(off))) {
            return -1;
        }
        return file_.read(reinterpret_cast<char *>(dst), qint64(n));
    }

    void parseDirectory(uint64_t base) {
        uint8_t dir[12];
        if (readAt(base, dir, sizeof(dir)) != sizeof(dir)) {
            return;
        }
        const uint32_t version = be32(dir);
        if (version != 0x00010000u && version != 0x4F54544Fu && version != 0x74727565u) {
            return;
        }
        const uint16_t numTables = be16(dir + 4);
        if (numTables == 0 || numTables > 1024) {
            return;
        }
        const uint64_t entriesEnd = base + 12 + uint64_t(numTables) * 16;
        if (entriesEnd > uint64_t(fileSize_)) {
            return; // directory itself runs past the file
        }
        Dir d;
        d.base = base;
        std::vector<uint8_t> raw(size_t(numTables) * 16);
        if (readAt(base + 12, raw.data(), raw.size()) != qint64(raw.size())) {
            return;
        }
        for (uint16_t i = 0; i < numTables; ++i) {
            const uint8_t *rec = raw.data() + size_t(i) * 16;
            Entry e;
            e.tag = be32(rec);
            e.off = be32(rec + 8);
            e.len = be32(rec + 12);
            // Offsets are sub-font-directory-relative for a plain sfnt
            // (base 0) and file-absolute for collections rebased here.
            e.off += base;
            if (e.len == 0 || uint64_t(e.off) + uint64_t(e.len) > uint64_t(fileSize_)) {
                continue; // drop lying entries, keep the honest ones
            }
            d.entries.push_back(e);
        }
        if (!d.entries.empty()) {
            dirs_.push_back(std::move(d));
        }
    }

    struct Entry {
        uint32_t tag = 0;
        uint64_t off = 0;
        uint32_t len = 0;
    };
    struct Dir {
        uint64_t base = 0;
        std::vector<Entry> entries;
    };

    mutable QFile file_;
    qint64 fileSize_ = 0;
    std::vector<Dir> dirs_;
};

// The dedupe key for a family name: case-folded, trimmed, internal
// whitespace collapsed, a trailing "regular" style suffix dropped.
QString familyKey(const QString &family) {
    QString k = family.toLower().trimmed();
    k.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    if (k.endsWith(" regular")) {
        k.chop(8);
        k = k.trimmed();
    }
    return k;
}

// True when any sub-font carries the CBDT/CBLC or sbix tables.
bool colorBitmapTables(const SfntFile &sf) {
    for (size_t sub = 0; sub < sf.subfontCount(); ++sub) {
        uint64_t off = 0;
        uint32_t len = 0;
        if (sf.tableRange("CBLC", sub, &off, &len) && sf.tableRange("CBDT", sub, &off, &len)) {
            return true;
        }
        if (sf.tableRange("sbix", sub, &off, &len)) {
            return true;
        }
    }
    return false;
}

// Battery coverage through the emoji font (cmap + real bitmaps): the
// count of battery codepoints mapped, and whether at least one of
// them also has a decodable bitmap record.
struct FaceCoverage {
    int mapped = 0;
    bool anyBitmap = false;
};

FaceCoverage bitmapFaceCoverage(const EmojiFont &font) {
    FaceCoverage out;
    const uint32_t *battery = emojiCoverageBattery();
    const size_t n = emojiCoverageBatterySize();
    for (size_t i = 0; i < n; ++i) {
        if (font.codepointGlyph(battery[i]) != 0) {
            ++out.mapped;
            if (!out.anyBitmap && font.codepointHasBitmap(battery[i])) {
                out.anyBitmap = true;
            }
        }
    }
    return out;
}

// Battery coverage through a raw cmap slice (the outline probe).
int cmapCoverage(const SfntFile &sf) {
    for (size_t sub = 0; sub < sf.subfontCount(); ++sub) {
        const std::vector<uint8_t> cmap = sf.readTable("cmap", sub, 4u << 20);
        if (cmap.size() >= 4) {
            const int hits = sfntCmapEmojiCoverage(cmap.data(), cmap.size());
            if (hits > 0) {
                return hits;
            }
        }
    }
    return 0;
}

// Family name through the name-table slice (first sub-font that has
// one); falls back to an empty string.
QString sliceFamilyName(const SfntFile &sf) {
    for (size_t sub = 0; sub < sf.subfontCount(); ++sub) {
        const std::vector<uint8_t> name = sf.readTable("name", sub, 1u << 20);
        if (name.size() >= 6) {
            const std::string family = sfntNameFamily(name.data(), name.size());
            if (!family.empty()) {
                return QString::fromStdString(family);
            }
        }
    }
    return QString();
}

} // namespace

QVector<SystemEmojiFont> discoverEmojiFonts() {
    QVector<SystemEmojiFont> found;

    // ---- 1. Gather candidate files in a deterministic order ----
    QStringList candidates;
    std::set<QString> seenPaths; // canonical, case-folded on Windows
    const auto addCandidate = [&candidates, &seenPaths](const QString &path) {
        const QFileInfo fi(path);
        if (!fi.isFile()) {
            return;
        }
        QString canonical = QDir::cleanPath(fi.absoluteFilePath());
#ifdef Q_OS_WIN
        canonical = canonical.toLower(); // NTFS is case-insensitive
#endif
        if (seenPaths.count(canonical) > 0) {
            return;
        }
        seenPaths.insert(canonical);
        candidates << fi.absoluteFilePath();
    };
    const QStringList nameFilters = {"*.ttf", "*.ttc", "*.otf"};
    for (const QString &dir : systemFontDirectories()) {
        QDirIterator it(dir, nameFilters, QDir::Files, QDirIterator::Subdirectories);
        QFileInfoList infos;
        while (it.hasNext()) {
            infos << QFileInfo(it.next());
        }
        std::sort(infos.begin(), infos.end(), [](const QFileInfo &a, const QFileInfo &b) {
            return a.filePath() < b.filePath();
        });
        for (const QFileInfo &fi : infos) {
            addCandidate(fi.filePath());
        }
    }
#ifdef Q_OS_WIN
    {
        const QStringList registered = registeredFontFiles();
        QStringList sorted = registered;
        std::sort(sorted.begin(), sorted.end());
        for (const QString &path : sorted) {
            addCandidate(path);
        }
    }
#endif

    // ---- 2. Probe each candidate ----
    std::set<QString> seenFamilies; // normalized keys
    std::set<uint64_t> seenContent; // content signatures
    for (const QString &path : candidates) {
        const QFileInfo fi(path);
        if (fi.size() < 12 || fi.size() > 400 * 1024 * 1024) {
            continue; // not a font / absurdly large
        }

        SfntFile sf;
        if (!sf.open(path)) {
            continue; // not a parsable sfnt/TTC
        }

        QString family;
        QString format;

        if (colorBitmapTables(sf)) {
            // Color face: full parse (the bitmaps ARE the payload).
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                continue;
            }
            const QByteArray blob = file.readAll();
            if (blob.isEmpty()) {
                continue;
            }
            EmojiFont font;
            if (!font.load(reinterpret_cast<const uint8_t *>(blob.constData()),
                           size_t(blob.size()))) {
                continue;
            }
            const FaceCoverage coverage = bitmapFaceCoverage(font);
            if (coverage.mapped < int(kEmojiCoverageMinimum) || !coverage.anyBitmap) {
                continue; // bitmap tables but no real emoji behind them
            }
            family = QString::fromStdString(sfntFamilyName(
                reinterpret_cast<const uint8_t *>(blob.constData()), size_t(blob.size())));
            format = font.isSbix() ? "sbix" : "CBDT";
        } else {
            // Outline face: the emoji-ness lives in the cmap.
            if (cmapCoverage(sf) < int(kEmojiCoverageMinimum)) {
                continue; // no (real) emoji coverage
            }
            family = sliceFamilyName(sf);
            format = "outline";
        }

        if (family.isEmpty()) {
            family = fi.completeBaseName();
        }

        // No repeats: normalized family key...
        const QString key = familyKey(family);
        if (!key.isEmpty() && seenFamilies.count(key) > 0) {
            continue;
        }
        // ...and content signature (renamed byte-identical copies).
        {
            std::vector<uint8_t> head, tail;
            const uint64_t tailOff = sf.fileSize() > 4096 ? uint64_t(sf.fileSize()) - 4096 : 0;
            const size_t tailLen = size_t(sf.fileSize() - qint64(tailOff));
            if (!sf.readRange(0, qMin<qint64>(4096, sf.fileSize()), head) ||
                !sf.readRange(tailOff, tailLen, tail)) {
                continue; // unreadable file
            }
            const uint64_t sig = (uint64_t(sf.fileSize()) << 1) ^ fnv1a(head.data(), head.size()) ^
                                 (fnv1a(tail.data(), tail.size()) * 1099511628211ull);
            if (seenContent.count(sig) > 0) {
                continue;
            }
            seenContent.insert(sig);
        }
        if (!key.isEmpty()) {
            seenFamilies.insert(key);
        }

        SystemEmojiFont item;
        item.family = family;
        item.path = fi.absoluteFilePath();
        item.format = format;
        found.push_back(item);
    }
    return found;
}

} // namespace fc
