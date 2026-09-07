#pragma once

#include <QImage>
#include <QString>

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "emoji.h"
#include "text.h"

class QCoreApplication;

namespace fc {

class EmojiPainter; // defined below; renderTextLayer takes one

// The app-layer text rasterizer: bridges REAL font metrics (Qt) into
// the pure core layout engine, then paints the laid-out slices with
// QPainter. Glyph PIXELS are platform-dependent (every font engine
// differs); every NUMBER the core produces (wrap, line breaks,
// alignment, box placement) is not.
//
// shapeTextQt: one ShapedRun per document run - QFontMetrics integer
// advances/ascent/descent/leading plus the UTF-8 byte offsets the core
// slices by. Runs with empty text are skipped. With an EmojiFont the
// bridge also resolves emoji clusters (see emoji.h): the cluster head
// carries the strike advance scaled to the run's pixel size, its
// continuation codepoints advance 0, and the ShapedRun annotations
// keep the layout engine from splitting a cluster mid-sequence.
//
// renderTextLayer: a TRANSPARENT RGBA8888 image (width x height) with
// the document's background box (when enabled), text painted with
// QPainter at the core layout positions, and emoji clusters painted as
// scaled Noto Color Emoji bitmaps. Safe to call from non-GUI threads
// (it paints on a QImage only - the export job does exactly that).
// The EmojiPainter caches decoded bitmaps and is NOT thread-safe:
// give the GUI side and the export job one instance each.
std::vector<ShapedRun> shapeTextQt(const TextDocument &doc, const EmojiFont *emoji = nullptr);
QImage renderTextLayer(const TextDocument &doc, int width, int height,
                       EmojiPainter *emoji = nullptr);

// The pixel size actually used for a run (clamped into the core's
// legal range so QFont::setPixelSize never sees a degenerate value).
int textPixelSizeClamped(const TextStyle &style);

// ---------------------------------------------------------------------------
// EmojiPainter: owns the bundled font bytes + the parsed EmojiFont +
// a per-pixel-size cache of decoded-and-scaled bitmaps.
//
// Lifetime rule: the font file bytes live in this object and the
// EmojiFont parses them IN PLACE, so load() must be the first call and
// the object must not be copied after loading (the EmojiFont holds
// raw views into bytes_).
//
// Thread rule: bitmap() mutates the cache. One instance per thread -
// MainWindow keeps the GUI one, the export job creates its own.
// ---------------------------------------------------------------------------
class EmojiPainter {
public:
    // Reads the font file and parses it. Returns false (and leaves the
    // painter empty) when the file is missing or malformed; text then
    // renders exactly as it would without the emoji font.
    bool load(const QString &path);

    bool loaded() const { return font_.loaded(); }
    const EmojiFont *font() const { return &font_; }

    // The cluster bitmap at a text pixel size: decodes the PNG (Qt's
    // built-in PNG handler - no fonts involved, thread-safe) and scales
    // it with a smooth transform to the strike-metric-scaled size. A
    // NULL image means "no bitmap for this glyph" (the caller skips
    // painting; the advance was already zeroed at shaping time only
    // when the font reported the glyph - see shapeTextQt).
    QImage bitmap(uint16_t glyph, int sizePx);

    void clearCache() { cache_.clear(); }

private:
    std::vector<uint8_t> bytes_;
    EmojiFont font_;
    std::map<std::pair<uint16_t, int>, QImage> cache_;
};

// Where the app looks for the bundled font (first existing candidate
// wins; the returned default may not exist - load() then just fails):
//   <appdir>/NotoColorEmoji.ttf                      (portable layout)
//   <appdir>/resources/fonts/NotoColorEmoji.ttf
//   <appdir>/../resources/fonts/... and ../../, ../../../ (dev builds)
QString emojiFontFilePath();

} // namespace fc
