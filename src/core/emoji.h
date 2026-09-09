#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace fc {

// ---------------------------------------------------------------------------
// The color-emoji font: parser + cluster shaper for the fonts ALREADY
// INSTALLED on the machine (nothing is bundled).
//
// The app discovers the user's emoji-capable font files (Segoe UI Emoji
// on Windows, Apple Color Emoji on macOS, Noto Color Emoji / JoyPixels /
// whatever the desktop has) and parses their binary tables DIRECTLY so
// emoji render as full-color bitmaps everywhere - including Windows 7,
// which has no platform color-emoji support at all. Picking a different
// font in the Text panel selects a different emoji SET: the Microsoft
// artwork, the Apple artwork, the Google artwork...
//
// Two bitmap formats cover every common emoji font:
//   * CBDT/CBLC + GSUB (Noto, Segoe UI Emoji, JoyPixels): strike
//     indexed bitmaps with per-record metrics, PNG payloads, and
//     ligature rules that fold sequences (flags, families, keycaps)
//     into single glyphs;
//   * sbix (Apple Color Emoji): strikes of inline PNG/JPEG records
//     addressed per glyph id, advances from hmtx, 'dupe'/'flip'
//     record indirection (the layout verified against the OpenType
//     spec and FreeType's implementation).
// TrueType COLLECTIONS (.ttc - Apple Color Emoji ships as one) load by
// trying each sub-font and keeping the first usable emoji face.
//
// Pure fc_core: no Qt, no FFmpeg, no allocations beyond load(). All
// parsing is bounds-checked big-endian reads; a malformed or truncated
// font makes load() fail and every lookup return "not found" - never a
// crash. The unit tests pin the parser against hand-built synthetic
// fonts (a minimal CBDT face, a minimal sbix face, and a collection
// wrapping both) so the suite stays hermetic - no font file needed.
//
// Shaping model (resolveCluster): an emoji cluster is either
//   * a SEQUENCE matched against the font's own GSUB ligature rules -
//     zero-width-joiner chains (families, professions, couples),
//     regional-indicator pairs (flags), keycaps, skin-tone modifiers -
//     resolved longest-match-first, or
//   * a SINGLE codepoint with emoji presentation: either forced by a
//     following U+FE0F (the emoji variation selector) or one whose
//     Unicode default presentation is emoji (isDefaultEmojiPresentation
//     - the same policy table emoji_clusters.h pins for the layout).
// U+FE0F / U+FE0E never map to glyphs themselves; they are dropped
// from the glyph stream used for ligature matching and are consumed
// into the cluster they follow. A font without GSUB rules (the sbix
// faces) simply never ligates - the caller falls back to per-codepoint
// bitmaps for the cluster's members.
//
// Determinism contract: load() followed by any sequence of const calls
// is a pure function of the font bytes. After load() succeeds the
// object is immutable, so concurrent const use (the GUI render thread
// and the export thread) is safe. The font does NOT own the bytes:
// keep the storage alive as long as the EmojiFont.
//
// Metrics are in STRIKE pixels (multiply by size/ppem to scale - see
// emojiScaleStrike, whose rounding is pinned by tests). For sbix
// records the image WIDTH/HEIGHT are unknown until the image is
// decoded (the table does not store them) - the app layer decodes and
// completes the extents; bearingX and the hmtx-scaled advance are
// always known.
// ---------------------------------------------------------------------------

// Strike-metric scaling: round(v * size / ppem), never negative.
inline int emojiScaleStrike(int v, int size, int ppem) {
    if (ppem <= 0) {
        return 0;
    }
    const int64_t scaled = (int64_t(v) * int64_t(size) + ppem / 2) / ppem;
    return scaled > 0 ? int(scaled) : 0;
}

// Which bitmap-emoji tables a font file carries (light directory
// probe - no table contents parsed).
enum class EmojiFontFormat : uint8_t {
    None = 0,
    Cbdt = 1, // CBLC + CBDT strikes
    Sbix = 2, // sbix strikes
};

class EmojiFont {
public:
    // Per-glyph strike metrics from the bitmap record (strike pixels).
    struct Metrics {
        int width = 0;    // bitmap width (0 for sbix: decode-dependent)
        int height = 0;   // bitmap height (0 for sbix: decode-dependent)
        int bearingX = 0; // left bearing (right of the pen)
        int bearingY = 0; // top bearing (UP from the baseline; 0 for sbix)
        int advance = 0;  // horizontal advance (CBDT record / sbix hmtx)
    };

    // A resolved bitmap: the image payload is a VIEW into the font
    // bytes (PNG for CBDT and sbix 'png ', JPEG for sbix 'jpg ').
    struct Bitmap {
        Metrics metrics;
        int ppem = 0; // the strike's ppem (scale denominator)
        const uint8_t *data = nullptr;
        size_t dataLen = 0;
        uint32_t format = 0; // 0 = CBDT record (PNG, complete metrics);
                             // else the sbix graphicType 4cc, e.g. 'png '
        int originY = 0;     // sbix: the record's originOffsetY (bottom
                             // edge, strike px, UP-positive); 0 for CBDT
        bool mirror = false; // sbix 'flip' chains mirror the image
    };

    // One resolved cluster starting at a codepoint position.
    struct Resolved {
        uint16_t glyph = 0; // glyph id in the emoji font
        int codepoints = 0; // cluster length in codepoints (>= 1)
    };

    // Parses the font (a plain sfnt or a TrueType Collection; for a
    // collection the first sub-font that parses as an emoji font wins).
    // `data` must stay valid and unchanged for the lifetime of this
    // object (the app layer owns the file bytes). Returns false on any
    // structural problem (then every lookup reports "not found" and
    // the text pipeline falls back to the platform font - exactly the
    // no-font behavior).
    bool load(const uint8_t *data, size_t size);

    bool loaded() const { return loaded_; }

    // ---- shaping ----
    // Resolves the emoji cluster starting at cps[pos]. Returns false
    // when those codepoints are plain text (the caller renders them
    // with the text font). `codepoints` in the result tells the caller
    // how far to advance; the cluster's bitmap is drawn at the pen
    // position of its FIRST codepoint.
    bool resolveCluster(const uint32_t *cps, size_t count, size_t pos, Resolved *out) const;

    // ---- lookups (also the test surface) ----
    // cmap lookup (formats 12 and 4); 0 = the font does not map it.
    uint16_t codepointGlyph(uint32_t cp) const;
    // cmap format 14 non-default mapping for (base, selector); 0 = none.
    uint16_t variantGlyph(uint32_t base, uint32_t selector) const;
    // GSUB ligature rule lookup: the glyph for a full sequence, else 0.
    uint16_t ligatureFor(const uint16_t *glyphs, size_t count) const;
    // Bitmap for a glyph (largest-ppem strike that has it, CBDT wins
    // over sbix when a font carries both). CBDT records must start
    // with the PNG signature (89504e47 0d0a1a0a) - that check is what
    // turns table damage into "missing", not garbage pixels.
    bool bitmapFor(uint16_t glyph, Bitmap *out) const;

    // ---- introspection ----
    int numGlyphs() const { return numGlyphs_; }
    int strikePpem() const; // the largest strike's ppem, 0 when absent
    int bitDepth() const;   // of the largest CBDT strike
    size_t ligatureRuleCount() const { return ligatures_.size(); }
    int maxLigatureSequence() const { return maxLigatureLen_; }
    bool isSbix() const { return !sbixStrikes_.empty(); }

    // Unicode "default emoji presentation" policy for SINGLE
    // codepoints (no FE0F) - the shared table in emoji_clusters.h.
    static bool isDefaultEmojiPresentation(uint32_t cp);

private:
    struct SubTable {
        uint16_t first = 0;
        uint16_t last = 0;
        uint32_t offset = 0; // from the strike's indexSubTableArray
    };
    struct Strike {
        int ppem = 0;
        int bitDepth = 0;
        uint32_t arrayOffset = 0; // from CBLC start
        std::vector<SubTable> subtables;
    };
    struct SbixStrike {
        int ppem = 0;
        size_t base = 0; // from data_ start: the strike header (ppem)
        size_t end = 0;  // one past the glyph-offset array (bounds guard)
    };
    struct CmapRange {
        uint32_t start = 0;
        uint32_t end = 0;
        uint32_t firstGlyph = 0;
    };

    // Bounds-checked big-endian readers over the whole sfnt blob.
    bool rdU16(size_t off, uint16_t *v) const;
    bool rdU32(size_t off, uint32_t *v) const;
    bool rdU8(size_t off, uint8_t *v) const;
    bool rdS8(size_t off, int8_t *v) const;
    const uint8_t *table(const char tag[4], size_t *len) const;

    bool parseCbloc();
    bool parseSbix();
    bool parseCmap();
    bool parseGsub();
    bool parseMaxp();
    bool parseHead();
    bool parseHmtx();
    bool parseFontAt(size_t dirBase);                // one sub-font's full parse
    int sbixAdvance(uint16_t glyph, int ppem) const; // hmtx units -> strike px
    bool bitmapForCbdt(uint16_t glyph, Bitmap *out) const;
    bool bitmapForSbix(uint16_t glyph, Bitmap *out) const;

    const uint8_t *data_ = nullptr;
    size_t size_ = 0;
    size_t dirBase_ = 0; // table directory base (sub-font offset in a ttc)
    bool loaded_ = false;

    size_t cbdtOff_ = 0;
    size_t cbdtLen_ = 0;
    size_t cblcOff_ = 0;
    size_t cblcLen_ = 0;
    size_t sbixOff_ = 0;
    size_t sbixLen_ = 0;

    std::vector<Strike> strikes_; // CBDT/CBLC
    std::vector<SbixStrike> sbixStrikes_;
    std::vector<CmapRange> cmapRanges_; // format 12, sorted by start
    bool cmapUnsorted_ = false;
    bool hasCmap4_ = false;
    size_t cmap4Off_ = 0;
    // (base, selector) -> glyph; only non-default format 14 mappings.
    std::map<std::pair<uint32_t, uint32_t>, uint16_t> variants_;
    // glyph sequence -> ligature glyph (the font's GSUB type-4 rules).
    std::map<std::vector<uint16_t>, uint16_t> ligatures_;
    int maxLigatureLen_ = 0;
    int numGlyphs_ = 0;
    int unitsPerEm_ = 1000;
    std::vector<uint16_t> hmtxAdvances_; // font units, numberOfHMetrics entries
    uint16_t hmtxTailAdvance_ = 0;       // advance of glyphs past hmtxAdvances_
};

// ---- standalone file-level helpers (no EmojiFont state needed) ----

// Light probe: reads only the table directory (the first ~4 KB) of an
// sfnt file or a TrueType Collection and reports which bitmap-emoji
// table family it carries. Used by font discovery to skip the ~99.9%
// of system fonts that have none without parsing them. `size` is how
// many bytes of the file are actually at `data` (the directory lives
// in the first ~4 KB); `fileSize` (-1 when unknown) is the file's real
// length - table records routinely point far past a short head, so
// the offset bounds check uses it, not `size`.
EmojiFontFormat sfntBitmapEmojiFormat(const uint8_t *data, size_t size, int64_t fileSize = -1);

// The font's family name from its 'name' table (Windows UTF-16BE
// preferred, Unicode-platform next, Mac Roman as the last resort;
// empty string when the font has no readable name). Works on sfnt
// files and collections (first sub-font with a name wins).
std::string sfntFamilyName(const uint8_t *data, size_t size);

} // namespace fc
