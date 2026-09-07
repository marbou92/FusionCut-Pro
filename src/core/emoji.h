#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace fc {

// ---------------------------------------------------------------------------
// The bundled color-emoji font: parser + cluster shaper.
//
// The app ships Google's Noto Color Emoji (resources/fonts/
// NotoColorEmoji.ttf, SIL OFL 1.1 - see THIRD_PARTY_NOTICES.md), a
// CBDT/CBLC bitmap font: every emoji is a 136x128 PNG with a strike
// ppem of 109. This module parses the font's binary tables DIRECTLY
// (sfnt directory, CBLC bitmap index, CBDT bitmap data, cmap, GSUB) so
// emoji render as full-color bitmaps everywhere - including Windows 7,
// which has no platform color-emoji support and no DirectWrite shaper
// for emoji sequences.
//
// Pure fc_core: no Qt, no FFmpeg, no allocations beyond load(). All
// parsing is bounds-checked big-endian reads; a malformed or truncated
// font makes load() fail and every lookup return "not found" - never a
// crash. Unit tested against the actual bundled font bytes in
// fc_emoji_tests (the file is committed, so the expectations are
// byte-stable).
//
// Shaping model (resolveCluster): an emoji cluster is either
//   * a SEQUENCE matched against the font's own GSUB ligature rules -
//     zero-width-joiner chains (families, professions, couples),
//     regional-indicator pairs (flags), keycaps, skin-tone modifiers -
//     resolved longest-match-first, or
//   * a SINGLE codepoint with emoji presentation: either forced by a
//     following U+FE0F (the emoji variation selector) or one whose
//     Unicode default presentation is emoji (isDefaultEmojiPresentation).
// U+FE0F / U+FE0E never map to glyphs themselves; they are dropped
// from the glyph stream used for ligature matching and are consumed
// into the cluster they follow.
//
// Determinism contract: load() followed by any sequence of const calls
// is a pure function of the font bytes. After load() succeeds the
// object is immutable, so concurrent const use (the GUI render thread
// and the export thread) is safe. The font does NOT own the bytes:
// keep the storage alive as long as the EmojiFont.
//
// Metrics are in STRIKE pixels (multiply by size/ppem to scale - see
// emojiScaleStrike, whose rounding is pinned by tests).
// ---------------------------------------------------------------------------

// Strike-metric scaling: round(v * size / ppem), never negative.
// With the bundled font (ppem 109, bitmap line 128 px, advance 136 px)
// a text pixel size S renders the bitmap at 128*S/109 px tall with a
// 136*S/109 px advance - exactly the font's own hmtx math (advance
// 2550/2048 units * 109/2048-per-px = 135.7 ~= 136 strike pixels).
inline int emojiScaleStrike(int v, int size, int ppem) {
    if (ppem <= 0) {
        return 0;
    }
    const int64_t scaled = (int64_t(v) * int64_t(size) + ppem / 2) / ppem;
    return scaled > 0 ? int(scaled) : 0;
}

// Variation selectors and the joiner: never rendered, never mapped to
// glyphs (the bundled font's cmap maps U+FE0F to .notdef); they only
// steer cluster resolution.
constexpr uint32_t kEmojiVS16 = 0xFE0Fu; // emoji presentation
constexpr uint32_t kEmojiVS15 = 0xFE0Eu; // text presentation
constexpr uint32_t kZeroWidthJoiner = 0x200Du;
constexpr uint32_t kCombiningKeycap = 0x20E3u;

class EmojiFont {
public:
    // Per-glyph strike metrics from the bitmap record (strike pixels).
    struct Metrics {
        int width = 0;    // bitmap width
        int height = 0;   // bitmap height
        int bearingX = 0; // left bearing (right of the pen)
        int bearingY = 0; // top bearing (UP from the baseline)
        int advance = 0;  // horizontal advance
    };

    // A resolved bitmap: the PNG payload is a VIEW into the font bytes.
    struct Bitmap {
        Metrics metrics;
        int ppem = 0; // the strike's ppem (scale denominator)
        const uint8_t *png = nullptr;
        size_t pngLen = 0;
    };

    // One resolved cluster starting at a codepoint position.
    struct Resolved {
        uint16_t glyph = 0; // glyph id in the emoji font
        int codepoints = 0; // cluster length in codepoints (>= 1)
    };

    // Parses the font. `data` must stay valid and unchanged for the
    // lifetime of this object (the app layer owns the file bytes).
    // Returns false on any structural problem (then every lookup
    // reports "not found" and the text pipeline falls back to the
    // platform font - exactly the no-font behavior).
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
    // CBDT/CBLC bitmap for a glyph (largest-ppem strike that has it).
    // Records must start with the PNG signature (89504e47 0d0a1a0a) -
    // that check is what turns table damage into "missing", not
    // garbage pixels.
    bool bitmapFor(uint16_t glyph, Bitmap *out) const;

    // ---- introspection ----
    int numGlyphs() const { return numGlyphs_; }
    int strikePpem() const; // the largest strike's ppem, 0 when absent
    int bitDepth() const;   // of the largest strike
    size_t ligatureRuleCount() const { return ligatures_.size(); }
    int maxLigatureSequence() const { return maxLigatureLen_; }

    // Unicode "default emoji presentation" policy for SINGLE
    // codepoints (no FE0F): the SMP emoji blocks as a range plus the
    // stable BMP set whose presentation is emoji by default. Codepoints
    // outside it (e.g. U+2764 HEAVY BLACK HEART) render as text unless
    // followed by U+FE0F - that is Unicode's own rule, and the emoji
    // font's cmap coverage gates the bitmap anyway.
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
    bool parseCmap();
    bool parseGsub();
    bool parseMaxp();

    const uint8_t *data_ = nullptr;
    size_t size_ = 0;
    bool loaded_ = false;

    size_t cbdtOff_ = 0;
    size_t cbdtLen_ = 0;
    size_t cblcOff_ = 0;
    size_t cblcLen_ = 0;

    std::vector<Strike> strikes_;
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
};

} // namespace fc
