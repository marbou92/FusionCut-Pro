#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fc {

// ---------------------------------------------------------------------------
// The text engine (rich text model + deterministic layout).
//
// Pure data + pure math: no Qt, no FFmpeg - unit tested in fc_text_tests.
// Three pieces live here:
//
//   TextDocument / TextRun / TextStyle - the rich text MODEL: a flat
//   sequence of styled UTF-8 runs (Qt's QTextEdit fragment model maps
//   onto it 1:1), plus the box that places the block in the frame.
//
//   layoutText() - the LAYOUT engine. Word wrap, line heights, per-line
//   alignment, and frame placement are computed with INTEGER math from
//   metrics the caller injects (ShapedRun). Real font metrics come from
//   the app layer (QFontMetrics bridge in text_renderer); the tests feed
//   synthetic metrics, which is what makes the layout exactly checkable.
//
//   compositeOver() - source-over alpha compositing of two RGBA8888
//   buffers (the text layer over the program frame), integer math,
//   rounded, identical on every platform.
//
// Determinism contract: identical (doc, shaped, frame size) always
// produce identical layout numbers; identical buffers produce identical
// composites. Glyph RASTERIZATION is the app layer's job (QPainter) and
// is platform-dependent - every number this file produces is not.
//
// Colors are packed 0xRRGGBBAA (R in the most significant byte - the
// memory order of the RGBA8888 buffers, so packing/unpacking is shifts).
// ---------------------------------------------------------------------------

// A text color's components (0..255 each).
inline uint8_t textRed(uint32_t rgba) {
    return static_cast<uint8_t>((rgba >> 24) & 0xFFu);
}
inline uint8_t textGreen(uint32_t rgba) {
    return static_cast<uint8_t>((rgba >> 16) & 0xFFu);
}
inline uint8_t textBlue(uint32_t rgba) {
    return static_cast<uint8_t>((rgba >> 8) & 0xFFu);
}
inline uint8_t textAlpha(uint32_t rgba) {
    return static_cast<uint8_t>(rgba & 0xFFu);
}
inline uint32_t textRgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (uint32_t(r) << 24) | (uint32_t(g) << 16) | (uint32_t(b) << 8) | uint32_t(a);
}

enum class TextAlign : uint8_t { Left = 0, Center = 1, Right = 2 };

// Style limits (validated by normalizeTextDocument / the project parser).
constexpr int kTextMinSize = 1;
constexpr int kTextMaxSize = 512;

struct TextStyle {
    std::string family; // "" = application default font
    int size = 64;      // pixel size [kTextMinSize, kTextMaxSize]
    bool bold = false;
    bool italic = false;
    bool underline = false;
    uint32_t colorRgba = 0xFFFFFFFFu; // opaque white

    bool operator==(const TextStyle &other) const {
        return family == other.family && size == other.size && bold == other.bold &&
               italic == other.italic && underline == other.underline &&
               colorRgba == other.colorRgba;
    }
    bool operator!=(const TextStyle &other) const { return !(*this == other); }
};

struct TextRun {
    std::string text; // UTF-8; '\n' is a hard line break
    TextStyle style;

    bool operator==(const TextRun &other) const {
        return text == other.text && style == other.style;
    }
    bool operator!=(const TextRun &other) const { return !(*this == other); }
};

// Where the text block sits in the frame: (anchorX, anchorY) is the
// CENTER of the laid-out block, in normalized frame coordinates [0, 1].
// `wrap` is the maximum line width as a fraction of the frame width.
struct TextBox {
    double anchorX = 0.5;
    double anchorY = 0.5;
    double wrap = 0.8;                     // (0, 1]
    bool background = false;               // draw a solid box behind the text
    uint32_t backgroundRgba = 0x000000B4u; // ~70% black

    bool operator==(const TextBox &other) const {
        return anchorX == other.anchorX && anchorY == other.anchorY && wrap == other.wrap &&
               background == other.background && backgroundRgba == other.backgroundRgba;
    }
    bool operator!=(const TextBox &other) const { return !(*this == other); }
};

struct TextDocument {
    std::vector<TextRun> runs;
    TextAlign align = TextAlign::Center;
    TextBox box;

    bool operator==(const TextDocument &other) const {
        return runs == other.runs && align == other.align && box == other.box;
    }
    bool operator!=(const TextDocument &other) const { return !(*this == other); }
};

// Drops empty runs, merges ADJACENT runs with equal styles, and clamps
// sizes into [kTextMinSize, kTextMaxSize]. Idempotent: a normalized
// document normalizes to itself. Runs stay in order; empty documents
// stay empty (zero runs).
void normalizeTextDocument(TextDocument &doc);

// Concatenated plain-text preview for clip labels: the first up-to-24
// codepoints with leading whitespace skipped, newlines turned into
// spaces; "Text" when the document is blank. UTF-8 in, UTF-8 out.
std::string textPreviewLabel(const TextDocument &doc);

// ---- UTF-8 ----

// Decodes UTF-8 to codepoints. Invalid bytes decode to U+FFFD (one per
// maximal invalid prefix, the WHATWG replacement policy). Appends to
// `codepoints` (cleared first).
void utf8Decode(const std::string &utf8, std::vector<uint32_t> &codepoints);

// Detailed decode: also appends the UTF-8 byte offset of every codepoint
// plus one PAST-THE-END entry (byteStarts.size() == codepoints.size() + 1)
// - the renderer slices run text by codepoint ranges with these.
void utf8DecodeDetailed(const std::string &utf8, std::vector<uint32_t> &codepoints,
                        std::vector<int> &byteStarts);

// ---- Layout ----

// One run shaped by the metrics provider (the app layer fills these from
// REAL font metrics; tests fill them with synthetic numbers). All values
// are integers: horizontal advances, vertical metrics, and byte offsets.
// codepoints may contain U+000A (newline) - a hard line break with
// advance 0 that never renders.
//
// The two optional vectors carry emoji cluster annotations from the
// app layer (the EmojiFont bridge). Both must be either EMPTY or
// exactly codepoints.size() long; runs with mismatched sizes are
// treated as unshapable (skipped), same as mismatched advances.
//   clusterStarts: 1 = a cluster begins at this codepoint, 0 = this
//     codepoint continues the previous cluster. The layout engine
//     never hard-splits inside a cluster (an over-wide cluster renders
//     alone, overflowing, exactly like a single over-wide glyph).
//   emojiGlyphs: non-zero = an emoji cluster head; the value is the
//     glyph id in the emoji font whose bitmap the renderer draws for
//     the WHOLE cluster. Continuation codepoints carry 0 and (by
//     convention) 0 advance; the layout engine does not interpret
//     them beyond the cluster-boundary rule above.
struct ShapedRun {
    size_t runIndex = 0;                // index into TextDocument::runs
    std::vector<uint32_t> codepoints;   // decoded text (incl. '\n' markers)
    std::vector<int> advances;          // per codepoint; '\n' must be 0
    std::vector<int> byteStarts;        // codepoints.size() + 1 entries
    int ascent = 0;                     // pixels above the baseline
    int descent = 0;                    // pixels below the baseline
    int lineGap = 0;                    // extra leading between lines
    std::vector<uint8_t> clusterStarts; // optional; see above
    std::vector<uint16_t> emojiGlyphs;  // optional; see above
};

// One draw call: a CONTIGUOUS codepoint slice of one shaped run, placed
// at (x, baselineY) with its UTF-8 byte range pre-resolved.
struct LaidOutSlice {
    size_t runIndex = 0; // index into the SHAPED vector
    int cpStart = 0;     // first codepoint in the slice
    int cpCount = 0;     // codepoints in the slice (>= 1)
    int x = 0;           // left edge of the first glyph's cell
    int baselineY = 0;   // the slice's baseline
    int byteStart = 0;   // UTF-8 byte offset into runs[run].text
    int byteLen = 0;     // UTF-8 byte length
};

struct TextLayout {
    std::vector<LaidOutSlice> slices;
    int lineCount = 0;
    int textWidth = 0;  // widest line's advance width (trailing spaces trimmed)
    int textHeight = 0; // sum of the line heights
    // The padded background rect in frame coordinates; meaningful when
    // doc.box.background (and the document is not empty).
    int bgX = 0, bgY = 0, bgW = 0, bgH = 0;
    // The content (wrap) width the layout wrapped at.
    int wrapWidth = 0;
};

// Lays the document out in a frameW x frameH frame.
//
// Semantics (all integer, all deterministic):
//   * wrapWidth = frameW * box.wrap, rounded, clamped to [1, frameW].
//   * Lines break at '\n' and after spaces (greedy word wrap). A word
//     wider than the wrap width on an empty line hard-splits at
//     codepoint boundaries. Trailing spaces at a break are dropped from
//     the line's width (they render on the previous line, invisible).
//   * A line's height = max(ascent + descent + lineGap) over the runs
//     with glyphs on it; the line's baseline sits at its top + that
//     line's max ascent.
//   * Line alignment is within textWidth (the widest line): left at 0,
//     centered on (textWidth - lineWidth) / 2, right at the far edge.
//   * The block's outer size = (textWidth, textHeight) + 2 * padding
//     when box.background (padding = max(2, frameH / 60)), else the text
//     size itself. Its CENTER sits at (frameW * anchorX, frameH *
//     anchorY), rounded.
// Degenerate inputs (empty document, no shaped runs, frameW/frameH <= 0)
// return an empty layout with all-zero extents.
TextLayout layoutText(const TextDocument &doc, const std::vector<ShapedRun> &shaped, int frameW,
                      int frameH);

// ---- Compositing ----

// Source-over composites `src` ONTO `dst` in place (both RGBA8888, w*h*4
// bytes, no padding). Transparent source pixels leave dst untouched;
// opaque source pixels replace dst. Integer math, rounded, deterministic.
void compositeOver(uint8_t *dst, const uint8_t *src, int width, int height);

} // namespace fc
