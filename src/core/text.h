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
//   TextAnimation + textAnimationAt() - the ANIMATION model: how a text
//   clip's first/last frames animate (fade, slide, pop, typewriter,
//   wipe). Pure math over the clip-relative frame number, so preview
//   and export evaluate IDENTICAL animation states (the same invariant
//   the keyframe engine keeps for effects).
//
//   SRT captions live in srt.h (parse + write + markup stripping) and
//   import as ordinary text clips - see TimelineModel::addTextClip.
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

// ---- Animation ----

// What the clip's entrance (the first inFrames frames) and exit (the
// last outFrames frames) do. `dir` is shared by Slide and Wipe:
//   Slide: the edge the text enters FROM (the exit leaves toward the
//     OPPOSITE edge - enter from the left, leave to the right);
//   Wipe:  the edge the reveal starts AT (Left wipes left-to-right,
//     Up reveals from the bottom edge upward).
// A kind with a duration of 0 (or beyond the clip) never completes its
// progress and stays clamped - it does not break anything.
enum class TextAnimKind : uint8_t {
    None = 0,
    Fade = 1,       // alpha ramp
    Slide = 2,      // translate in from an edge, out to the opposite
    Pop = 3,        // scale 0 -> 1 with a small overshoot (in), 1 -> 0 (out)
    Typewriter = 4, // reveal the first N codepoints (in), hide from the
                    // END backward (out)
    Wipe = 5,       // reveal/cover the block along one axis
};

enum class TextAnimDir : uint8_t { Left = 0, Right = 1, Up = 2, Down = 3 };

struct TextAnimation {
    TextAnimKind inKind = TextAnimKind::None;
    int64_t inFrames = 0;
    TextAnimKind outKind = TextAnimKind::None;
    int64_t outFrames = 0;
    TextAnimDir dir = TextAnimDir::Left;

    bool operator==(const TextAnimation &other) const {
        return inKind == other.inKind && inFrames == other.inFrames && outKind == other.outKind &&
               outFrames == other.outFrames && dir == other.dir;
    }
    bool operator!=(const TextAnimation &other) const { return !(*this == other); }
};

// True when either side animates (a kind beyond None with frames > 0).
bool hasTextAnimation(const TextAnimation &anim);

// The animation state for a given clip-relative frame (0 = the clip's
// first frame). Everything the renderer needs, all normalized:
//   alpha             global opacity multiplier [0, 1]
//   offsetX/offsetY   translation in FRACTIONS of the frame size
//   scale             uniform scale about the block center (>= 0)
//   revealCodepoints  -1 = all; else only the first N codepoints of the
//                      document render (already cluster-aligned - see
//                      truncateTextDocument)
//   wipe              visible fraction of the block [0, 1] along `dir`
// Easing: fade/slide/wipe use smoothstep; pop-in uses easeOutBack (the
// small overshoot); pop-out and typewriter reveal are linear/smoothed
// progress. clipFrame outside [0, duration] clamps (before the clip:
// fully pre-animation; at/after the end: fully out).
struct TextAnimState {
    double alpha = 1.0;
    double offsetX = 0.0;
    double offsetY = 0.0;
    double scale = 1.0;
    int64_t revealCodepoints = -1;
    double wipe = 1.0;
};

TextAnimState textAnimationAt(const TextAnimation &anim, int64_t clipFrame, int64_t duration,
                              int64_t totalCodepoints);

struct TextDocument {
    std::vector<TextRun> runs;
    TextAlign align = TextAlign::Center;
    TextBox box;
    TextAnimation animation;

    bool operator==(const TextDocument &other) const {
        return runs == other.runs && align == other.align && box == other.box &&
               animation == other.animation;
    }
    bool operator!=(const TextDocument &other) const { return !(*this == other); }
};

// Total codepoint count across every run (newlines count; one astral
// codepoint counts once; invalid bytes count as their U+FFFDs).
int64_t countTextCodepoints(const TextDocument &doc);

// Copies `doc` keeping only the first `maxCodepoints` codepoints - cut
// CLUSTER-ALIGNED: a cut landing inside a multi-codepoint emoji cluster
// shrinks to the cluster's start, so a typewriter reveal never shows
// half a flag or half a family. A run that loses all its text is
// dropped (a zero-budget truncation yields an empty run list);
// everything else is copied verbatim.
void truncateTextDocument(const TextDocument &doc, int64_t maxCodepoints, TextDocument &out);

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
// clusterStarts (optional, but the app layer always fills it) carries
// the emoji cluster boundaries from the Unicode policy in
// emoji_clusters.h: 1 = a cluster begins at this codepoint, 0 = this
// codepoint continues the previous cluster. The layout engine never
// hard-splits inside a cluster (an over-wide cluster renders alone,
// overflowing, exactly like a single over-wide glyph). Must be either
// EMPTY or exactly codepoints.size() long; a mismatched run is treated
// as unshapable (skipped), like mismatched advances. A multi-codepoint
// cluster's head codepoint carries the whole cluster's advance; its
// continuations carry 0 by convention.
//
// emojiGlyphs (optional; the app layer fills it when an emoji font is
// loaded) marks the codepoints whose bitmap the renderer draws:
// non-zero = the glyph id in the selected emoji font whose bitmap
// paints AT THIS codepoint's pen position (a ligature cluster marks
// only its head - the single bitmap spans the cluster - while an
// UNRESOLVED cluster marks each member that has its own bitmap).
// Joiners and codepoints without bitmaps carry 0 and paint nothing.
// Must be either EMPTY or exactly codepoints.size() long; a mismatched
// run is treated as unshapable (skipped), like clusterStarts.
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
    // The block rect (text extents + background padding) in frame
    // coordinates, ALWAYS meaningful when the layout is not empty -
    // the box animations anchor here (wipe rectangles, scale center).
    int blockX = 0, blockY = 0, blockW = 0, blockH = 0;
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
