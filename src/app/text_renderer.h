#pragma once

#include <QImage>
#include <QString>

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "emoji.h"
#include "text.h"

namespace fc {

class EmojiPainter; // defined below; shapeTextQt/renderTextLayer take one

// The app-layer text rasterizer: bridges REAL font metrics (Qt) into
// the pure core layout engine, then paints the laid-out slices with
// QPainter.
//
// shapeTextQt: one ShapedRun per document run - QFontMetrics integer
// advances/ascent/descent/leading plus the UTF-8 byte offsets the core
// slices by. Runs with empty text are skipped. EMOJI CLUSTERS are
// segmented by the pure Unicode policy in emoji_clusters.h and stay
// atomic (the layout engine never splits one mid-sequence):
//   * with a loaded EmojiPainter (a color-emoji font the user picked
//     from the machine's installed fonts), a cluster that resolves to
//     a bitmap glyph - a GSUB ligature (flag, family, keycap) or a
//     single emoji-presentation codepoint - carries the strike advance
//     scaled to the run's pixel size on its head, its continuations
//     advance 0, and the bitmap's extents fold into the run metrics so
//     lines grow tall enough; a cluster the font cannot ligate falls
//     back to per-codepoint bitmaps (joiners paint nothing), and only
//     when no member has a bitmap at all does the cluster take the
//     platform path below;
//   * the platform path measures the cluster as ONE STRING
//     (QFontMetrics shapes it through the platform font stack,
//     including fallback to an emoji font) and the renderer draws it
//     as one drawText call - monochrome on platforms without color
//     emoji, but correctly ligated and never split.
//
// renderTextLayer: a TRANSPARENT RGBA8888 image (width x height) with
// the document's background box (when enabled), text painted with
// QPainter at the core layout positions, and emoji clusters painted as
// scaled color bitmaps from the selected font. Safe to call from
// non-GUI threads (it paints on a QImage only - the export job does
// exactly that; pass a per-thread EmojiPainter because its caches
// mutate).
//
// ANIMATION: pass the clip-relative frame (>= 0) and the clip duration
// and the layer renders THAT animation state - reveal (typewriter)
// truncation happens before layout, wipe/slide/scale/alpha apply to the
// painted layer afterwards. clipFrame < 0 (or a static document)
// renders the settled state, byte-identical to the pre-animation
// renderer.
std::vector<ShapedRun> shapeTextQt(const TextDocument &doc, EmojiPainter *emoji = nullptr);
QImage renderTextLayer(const TextDocument &doc, int width, int height, int64_t clipFrame = -1,
                       int64_t clipDuration = 0, EmojiPainter *emoji = nullptr);

// The pixel size actually used for a run (clamped into the core's
// legal range so QFont::setPixelSize never sees a degenerate value).
int textPixelSizeClamped(const TextStyle &style);

// ---------------------------------------------------------------------------
// EmojiPainter: owns the SELECTED emoji font file's bytes + the parsed
// EmojiFont + this thread's caches of decoded (and scaled) images.
//
// The font comes from the machine's installed fonts (see
// system_fonts.h) - the app bundles none. Picking a different file
// selects a different emoji SET (Microsoft, Apple, Google...).
//
// Lifetime rule: the font file bytes live in this object and the
// EmojiFont parses them IN PLACE, so load() must be the first call and
// the object must not be copied after loading (the EmojiFont holds
// raw views into bytes_).
//
// Thread rule: image()/scaledImage() mutate the caches. One instance
// per thread - MainWindow keeps the GUI one, the export job creates
// its own from the same file path.
// ---------------------------------------------------------------------------
class EmojiPainter {
public:
    // Reads the font file and parses it (any CBDT or sbix emoji font,
    // a plain sfnt or a .ttc collection). A file with NO bitmap tables
    // but a readable family name (an OUTLINE emoji face, discovered by
    // its cmap coverage) also succeeds: it contributes no bitmaps, but
    // its family routes emoji clusters through the platform text stack
    // (see emojiFamily). Returns false (and leaves the painter empty)
    // when the file is missing, malformed, or nameless; emoji then
    // render through the platform font stack - exactly the
    // no-selection behavior.
    bool load(const QString &path);

    bool loaded() const { return font_.loaded(); }
    const EmojiFont *font() const { return &font_; }
    QString filePath() const { return path_; }

    // The picked font's own family name (empty when unknown). The
    // shaper/renderer route emoji clusters that have no bitmap
    // through a QFont of this family - for an outline pick that is the
    // whole point (monochrome emoji from the picked font), for a
    // color pick it is a tighter fallback than Qt's own for the
    // clusters the bitmaps miss.
    QString emojiFamily() const { return emojiFamily_; }

    // The decoded image for a glyph at its native (strike) size -
    // decodes CBDT PNG and sbix png/jpg payloads via QImage::fromData
    // (no fonts, no QGuiApplication; safe on worker threads) and
    // applies the sbix 'flip' mirroring. NULL = no decodable image
    // (the caller skips painting that glyph). Caches; bounded.
    const QImage *image(uint16_t glyph);

    // The image smooth-scaled for a text pixel size (strike metrics
    // scaled by size/ppem, the same math as emojiScaleStrike). Caches;
    // bounded. NULL = not decodable.
    const QImage *scaledImage(uint16_t glyph, int sizePx);

    void clearCache() {
        raw_.clear();
        scaled_.clear();
    }

private:
    std::vector<uint8_t> bytes_;
    EmojiFont font_;
    QString path_;
    QString emojiFamily_;
    std::map<uint16_t, QImage> raw_;                    // decoded at strike size
    std::map<std::pair<uint16_t, int>, QImage> scaled_; // scaled to text size
};

} // namespace fc
