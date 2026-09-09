// FusionCut Pro - text engine unit tests.
// Pure data + math; no Qt, no FFmpeg - runs anywhere ctest runs.
// Covers: UTF-8 decoding (valid + invalid classes), the rich text model
// (normalize, preview labels), the layout engine (wrap, hard splits,
// newlines, mixed metrics, alignment, box/anchor/background math), the
// source-over compositor, the text-clip timeline model semantics, and
// the project codec round-trip for text tracks and clips.

#include <cmath>
#include <string>
#include <vector>

#include "project_format.h"
#include "test_harness.h"
#include "text.h"
#include "timeline_model.h"

using namespace fc;

// ---------------------------------------------------------------------------
// Helpers: a synthetic metrics shaper (deterministic, hand-derivable).
// ---------------------------------------------------------------------------

// Every non-space codepoint advances `adv`; spaces advance `spaceAdv`;
// '\n' advances 0. Vertical metrics are uniform per run.
static ShapedRun shape(const std::string &text, size_t runIndex, int adv, int spaceAdv, int ascent,
                       int descent, int lineGap) {
    ShapedRun run;
    run.runIndex = runIndex;
    utf8DecodeDetailed(text, run.codepoints, run.byteStarts);
    run.advances.reserve(run.codepoints.size());
    for (uint32_t cp : run.codepoints) {
        if (cp == 0x0Au) {
            run.advances.push_back(0);
        } else if (cp == 0x20u) {
            run.advances.push_back(spaceAdv);
        } else {
            run.advances.push_back(adv);
        }
    }
    run.ascent = ascent;
    run.descent = descent;
    run.lineGap = lineGap;
    return run;
}

// 10px glyph cell, 5px space, 8/2/0 vertical metrics: the reference font.
static ShapedRun shapeMono(const std::string &text, size_t runIndex = 0) {
    return shape(text, runIndex, 10, 5, 8, 2, 0);
}

static TextDocument singleRunDoc(const std::string &text) {
    TextDocument doc;
    TextRun run;
    run.text = text;
    doc.runs.push_back(run);
    return doc;
}

static LaidOutSlice *findSlice(TextLayout &layout, size_t index) {
    return index < layout.slices.size() ? &layout.slices[index] : nullptr;
}

// ---------------------------------------------------------------------------
// UTF-8
// ---------------------------------------------------------------------------

static void testUtf8Valid() {
    std::vector<uint32_t> cps;
    std::vector<int> byteStarts;

    utf8DecodeDetailed("Hello", cps, byteStarts);
    CHECK(cps.size() == 5);
    CHECK(cps[0] == 'H' && cps[4] == 'o');
    CHECK(byteStarts.size() == 6);
    CHECK(byteStarts[0] == 0 && byteStarts[5] == 5);

    utf8DecodeDetailed(std::string("\xC3\xA9"
                                   "x"), // U+00E9 then 'x'
                       cps, byteStarts);
    CHECK(cps.size() == 2);
    CHECK(cps[0] == 0xE9u);
    CHECK(cps[1] == 'x');
    CHECK(byteStarts.size() == 3);
    CHECK(byteStarts[1] == 2); // 'x' starts after the 2-byte sequence

    utf8DecodeDetailed("\xE6\xBC\xA2", cps, byteStarts); // U+6F22 (CJK)
    CHECK(cps.size() == 1);
    CHECK(cps[0] == 0x6F22u);
    CHECK(byteStarts[1] == 3);

    utf8DecodeDetailed("\xF0\x9F\x98\x80", cps, byteStarts); // U+1F600
    CHECK(cps.size() == 1);
    CHECK(cps[0] == 0x1F600u);
    CHECK(byteStarts[1] == 4);

    // Mixed multi-byte string: byte offsets track every prefix.
    utf8DecodeDetailed(std::string("a") + "\xC3\xA9" + "b" + "\xF0\x9F\x98\x80", cps, byteStarts);
    CHECK(cps.size() == 4);
    CHECK(cps[0] == 'a' && cps[1] == 0xE9u && cps[2] == 'b' && cps[3] == 0x1F600u);
    CHECK(byteStarts.size() == 5);
    CHECK(byteStarts[1] == 1 && byteStarts[2] == 3 && byteStarts[3] == 4 && byteStarts[4] == 8);

    // The plain wrapper matches the detailed one.
    std::vector<uint32_t> plain;
    utf8Decode("Hi", plain);
    CHECK(plain.size() == 2 && plain[0] == 'H');

    // Empty input.
    utf8DecodeDetailed("", cps, byteStarts);
    CHECK(cps.empty());
    CHECK(byteStarts.size() == 1);
}

static void testUtf8Invalid() {
    std::vector<uint32_t> cps;
    std::vector<int> byteStarts;

    // Stray continuation byte: one replacement, one byte consumed.
    utf8Decode("\x80", cps);
    CHECK(cps.size() == 1 && cps[0] == 0xFFFDu);

    // Valid lead, invalid continuation: FFFD, then the offending byte
    // re-decodes on its own ("C3 41" -> FFFD, 'A').
    utf8Decode(std::string("\xC3") + "A", cps);
    CHECK(cps.size() == 2);
    CHECK(cps[0] == 0xFFFDu);
    CHECK(cps[1] == 'A');

    // Truncated sequence at end of input.
    utf8Decode("\xC3", cps);
    CHECK(cps.size() == 1 && cps[0] == 0xFFFDu);

    // Overlong encodings (C0/C1 leads are invalid leads; E0/F0 forms
    // decode then fail the minimum-value check).
    utf8Decode("\xC0\xAF", cps); // classic overlong '/'
    CHECK(cps.size() == 2);
    CHECK(cps[0] == 0xFFFDu && cps[1] == 0xFFFDu);
    utf8Decode("\xE0\x80\x80", cps); // overlong NUL
    CHECK(cps.size() == 3);
    CHECK(cps[0] == 0xFFFDu && cps[1] == 0xFFFDu && cps[2] == 0xFFFDu);
    utf8Decode("\xF0\x80\x80\x80", cps); // overlong via 4 bytes
    CHECK(cps.size() == 4);
    for (uint32_t cp : cps) {
        CHECK(cp == 0xFFFDu);
    }

    // Surrogate halves never decode.
    utf8Decode("\xED\xA0\x80", cps); // CESU-8 D800
    CHECK(cps.size() == 3);
    for (uint32_t cp : cps) {
        CHECK(cp == 0xFFFDu);
    }

    // Past Unicode range.
    utf8Decode("\xF4\x90\x80\x80", cps); // U+110000
    CHECK(cps.size() == 4);
    for (uint32_t cp : cps) {
        CHECK(cp == 0xFFFDu);
    }

    // Invalid leads 0xF5+ and 0xFE/0xFF.
    utf8Decode("\xF5\x80\x80\x80", cps);
    CHECK(cps.size() == 4);
    for (uint32_t cp : cps) {
        CHECK(cp == 0xFFFDu);
    }
    utf8Decode("\xFF", cps);
    CHECK(cps.size() == 1 && cps[0] == 0xFFFDu);

    // A valid string stays valid around an invalid byte.
    utf8Decode(std::string("a") + "\x80" + "b", cps);
    CHECK(cps.size() == 3);
    CHECK(cps[0] == 'a' && cps[1] == 0xFFFDu && cps[2] == 'b');
}

// ---------------------------------------------------------------------------
// Color helpers
// ---------------------------------------------------------------------------

static void testColorHelpers() {
    const uint32_t rgba = 0xFF8844C0u;
    CHECK(textRed(rgba) == 0xFF);
    CHECK(textGreen(rgba) == 0x88);
    CHECK(textBlue(rgba) == 0x44);
    CHECK(textAlpha(rgba) == 0xC0);
    CHECK(textRgba(0xFF, 0x88, 0x44, 0xC0) == rgba);
    CHECK(textRgba(textRed(rgba), textGreen(rgba), textBlue(rgba), textAlpha(rgba)) == rgba);
}

// ---------------------------------------------------------------------------
// normalize + preview labels
// ---------------------------------------------------------------------------

static void testNormalize() {
    // Empty stays empty.
    TextDocument doc;
    normalizeTextDocument(doc);
    CHECK(doc.runs.empty());

    // Empty runs dropped.
    doc = singleRunDoc("");
    TextRun r2;
    r2.text = "AB";
    doc.runs.push_back(r2);
    normalizeTextDocument(doc);
    CHECK(doc.runs.size() == 1);
    CHECK(doc.runs[0].text == "AB");

    // Adjacent equal styles merge (text concatenated).
    doc = singleRunDoc("AB");
    TextRun same;
    same.text = "CD";
    doc.runs.push_back(same);
    normalizeTextDocument(doc);
    CHECK(doc.runs.size() == 1);
    CHECK(doc.runs[0].text == "ABCD");

    // Non-adjacent same styles stay separate (order preserved).
    doc = singleRunDoc("AB");
    TextRun otherStyle;
    otherStyle.text = "X";
    otherStyle.style.bold = true;
    doc.runs.push_back(otherStyle);
    TextRun tail;
    tail.text = "CD";
    doc.runs.push_back(tail);
    normalizeTextDocument(doc);
    CHECK(doc.runs.size() == 3);
    CHECK(doc.runs[1].text == "X" && doc.runs[1].style.bold);
    CHECK(doc.runs[2].text == "CD");

    // Sizes clamp into range.
    doc = singleRunDoc("A");
    doc.runs[0].style.size = 0;
    normalizeTextDocument(doc);
    CHECK(doc.runs[0].style.size == kTextMinSize);
    doc.runs[0].style.size = 100000;
    normalizeTextDocument(doc);
    CHECK(doc.runs[0].style.size == kTextMaxSize);

    // Different colors do not merge.
    doc = singleRunDoc("A");
    TextRun colorRun;
    colorRun.text = "B";
    colorRun.style.colorRgba = 0xFF0000FFu;
    doc.runs.push_back(colorRun);
    normalizeTextDocument(doc);
    CHECK(doc.runs.size() == 2);

    // Idempotent.
    doc = singleRunDoc("AB");
    TextRun tail2;
    tail2.text = "CD";
    tail2.style.underline = true;
    doc.runs.push_back(tail2);
    normalizeTextDocument(doc);
    const TextDocument once = doc;
    normalizeTextDocument(doc);
    CHECK(doc.runs == once.runs);
}

static void testPreviewLabel() {
    CHECK(textPreviewLabel(singleRunDoc("Hello World")) == "Hello World");
    CHECK(textPreviewLabel(singleRunDoc("  Leading")) == "Leading");
    CHECK(textPreviewLabel(singleRunDoc("A\nB")) == "A B");
    CHECK(textPreviewLabel(singleRunDoc("0123456789012345678901234567")) ==
          "012345678901234567890123"); // 24 codepoint cap
    CHECK(textPreviewLabel(singleRunDoc("")) == "Text");
    CHECK(textPreviewLabel(singleRunDoc("   \n  ")) == "Text");
    CHECK(textPreviewLabel(singleRunDoc("\xC3\xA9t\xC3\xA9")) == "\xC3\xA9t\xC3\xA9"); // UTF-8
    // Multi-run concatenation.
    TextDocument doc = singleRunDoc("Fu");
    TextRun run;
    run.text = "sion";
    doc.runs.push_back(run);
    CHECK(textPreviewLabel(doc) == "Fusion");
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

static void testLayoutSingleLine() {
    // "AA" in a 1000x1000 frame, wrap 0.8 -> wrapW 800, no background:
    // textWidth 20, textHeight 10, anchor center -> box (490, 495),
    // baseline 495+8.
    TextDocument doc = singleRunDoc("AA");
    doc.box.anchorX = 0.5;
    doc.box.anchorY = 0.5;
    doc.box.wrap = 0.8;
    doc.box.background = false;

    TextLayout layout = layoutText(doc, {shapeMono("AA")}, 1000, 1000);
    CHECK(layout.lineCount == 1);
    CHECK(layout.wrapWidth == 800);
    CHECK(layout.textWidth == 20);
    CHECK(layout.textHeight == 10);
    CHECK(layout.bgW == 0 && layout.bgH == 0);
    CHECK(layout.slices.size() == 1);
    const LaidOutSlice *slice = findSlice(layout, 0);
    CHECK(slice != nullptr);
    CHECK(slice->runIndex == 0);
    CHECK(slice->cpStart == 0 && slice->cpCount == 2);
    CHECK(slice->x == 490);
    CHECK(slice->baselineY == 495 + 8);
    CHECK(slice->byteStart == 0 && slice->byteLen == 2);
}

static void testLayoutAlignment() {
    // Two lines ("AA" then "A"), textWidth 20: center -> line 2 offset 5,
    // right -> offset 10, left -> 0. Same doc wrapped by width.
    TextDocument doc = singleRunDoc("AA A");
    doc.box.wrap = 0.025; // wrapW = 25
    doc.align = TextAlign::Center;

    TextLayout layout = layoutText(doc, {shapeMono("AA A")}, 1000, 1000);
    CHECK(layout.lineCount == 2);
    CHECK(layout.textWidth == 20);
    CHECK(layout.textHeight == 20);
    // Line 1 "AA" at x0, line 2 "A" centered by (20-10)/2 = 5.
    CHECK(layout.slices[0].x == layout.slices[1].x - 5);
    CHECK(layout.slices[1].cpStart == 3 && layout.slices[1].cpCount == 1);
    CHECK(layout.slices[1].byteStart == 3 && layout.slices[1].byteLen == 1);

    doc.align = TextAlign::Right;
    layout = layoutText(doc, {shapeMono("AA A")}, 1000, 1000);
    CHECK(layout.slices[1].x - layout.slices[0].x == 10);

    doc.align = TextAlign::Left;
    layout = layoutText(doc, {shapeMono("AA A")}, 1000, 1000);
    CHECK(layout.slices[1].x == layout.slices[0].x);
}

static void testLayoutWordWrap() {
    // "AA BB" at wrapW 25: line 1 "AA" (trailing space trimmed), line 2
    // "BB". Neither line includes the space.
    TextDocument doc = singleRunDoc("AA BB");
    doc.box.wrap = 0.025;

    TextLayout layout = layoutText(doc, {shapeMono("AA BB")}, 1000, 1000);
    CHECK(layout.lineCount == 2);
    CHECK(layout.textWidth == 20); // NOT 25: trailing space trimmed
    CHECK(layout.slices.size() == 2);
    CHECK(layout.slices[0].cpCount == 2);
    CHECK(layout.slices[0].byteStart == 0 && layout.slices[0].byteLen == 2);
    CHECK(layout.slices[1].cpStart == 3 && layout.slices[1].cpCount == 2);
    CHECK(layout.slices[1].byteStart == 3 && layout.slices[1].byteLen == 2);
    // Line 2 sits one line height below line 1.
    CHECK(layout.slices[1].baselineY - layout.slices[0].baselineY == 10);

    // Leading spaces render (mid-line, not trailing): width includes
    // the leading space cell.
    doc = singleRunDoc(" AA");
    layout = layoutText(doc, {shapeMono(" AA")}, 1000, 1000);
    CHECK(layout.lineCount == 1);
    CHECK(layout.textWidth == 25);
    CHECK(layout.slices[0].cpCount == 3);
}

static void testLayoutHardSplit() {
    // "AAAAAA" (60 wide) at wrapW 25: three lines of "AA" (20 each).
    TextDocument doc = singleRunDoc("AAAAAA");
    doc.box.wrap = 0.025;

    TextLayout layout = layoutText(doc, {shapeMono("AAAAAA")}, 1000, 1000);
    CHECK(layout.lineCount == 3);
    CHECK(layout.textWidth == 20);
    CHECK(layout.slices.size() == 3);
    for (const LaidOutSlice &slice : layout.slices) {
        CHECK(slice.cpCount == 2);
    }
    CHECK(layout.slices[0].cpStart == 0);
    CHECK(layout.slices[1].cpStart == 2);
    CHECK(layout.slices[2].cpStart == 4);

    // A single glyph wider than the wrap width renders alone (overflow).
    ShapedRun wide = shapeMono("AA");
    wide.advances[0] = 40;
    wide.advances[1] = 40;
    layout = layoutText(doc, {wide}, 1000, 1000);
    CHECK(layout.lineCount == 2);
    CHECK(layout.textWidth == 40);
    CHECK(layout.slices.size() == 2);
    CHECK(layout.slices[0].cpCount == 1 && layout.slices[1].cpCount == 1);
}

static void testLayoutNewlines() {
    // Explicit breaks.
    TextDocument doc = singleRunDoc("A\nB");
    TextLayout layout = layoutText(doc, {shapeMono("A\nB")}, 1000, 1000);
    CHECK(layout.lineCount == 2);
    CHECK(layout.textWidth == 10);
    CHECK(layout.textHeight == 20);
    CHECK(layout.slices.size() == 2);
    // The newline codepoint never renders: both slices are 1 glyph.
    CHECK(layout.slices[0].cpCount == 1 && layout.slices[1].cpCount == 1);
    CHECK(layout.slices[1].cpStart == 2);
    CHECK(layout.slices[1].byteStart == 2);

    // Trailing newline adds NO blank line ("A\n" is one line).
    doc = singleRunDoc("A\n");
    layout = layoutText(doc, {shapeMono("A\n")}, 1000, 1000);
    CHECK(layout.lineCount == 1);

    // Double newline: a blank paragraph line between the text lines,
    // with the document-default metrics.
    doc = singleRunDoc("A\n\nB");
    layout = layoutText(doc, {shapeMono("A\n\nB")}, 1000, 1000);
    CHECK(layout.lineCount == 3);
    CHECK(layout.slices.size() == 2); // blank line has no glyphs
    CHECK(layout.textHeight == 30);   // 10 + 10 + 10 (default metrics)
    CHECK(layout.slices[1].baselineY - layout.slices[0].baselineY == 20);
}

static void testLayoutMixedMetrics() {
    // Two runs on one line: big text (16/4/0) then small (8/2/0). Line
    // height = 20 (big), ascent = 16 (big), both share the baseline.
    TextDocument doc = singleRunDoc("AB");
    TextRun small;
    small.text = "cd";
    doc.runs.push_back(small);

    std::vector<ShapedRun> shaped;
    shaped.push_back(shape("AB", 0, 10, 5, 16, 4, 0));
    shaped.push_back(shape("cd", 1, 6, 3, 8, 2, 0));

    TextLayout layout = layoutText(doc, shaped, 1000, 1000);
    CHECK(layout.lineCount == 1);
    CHECK(layout.textHeight == 20);
    CHECK(layout.textWidth == 20 + 12);
    CHECK(layout.slices.size() == 2);
    CHECK(layout.slices[0].runIndex == 0 && layout.slices[1].runIndex == 1);
    CHECK(layout.slices[0].baselineY == layout.slices[1].baselineY);
    CHECK(layout.slices[1].x == layout.slices[0].x + 20);
    // Byte ranges stay per-run.
    CHECK(layout.slices[1].byteStart == 0 && layout.slices[1].byteLen == 2);

    // Words split across run boundaries hard-split together.
    shaped.clear();
    shaped.push_back(shape("AB", 0, 10, 5, 8, 2, 0));
    shaped.push_back(shape("CD", 1, 10, 5, 8, 2, 0));
    doc = singleRunDoc("AB");
    TextRun second;
    second.text = "CD";
    doc.runs.push_back(second);
    doc.box.wrap = 0.025; // wrapW 25
    layout = layoutText(doc, shaped, 1000, 1000);
    CHECK(layout.lineCount == 2);
    CHECK(layout.slices.size() == 2);
    CHECK(layout.slices[0].runIndex == 0 && layout.slices[0].cpCount == 2);
    CHECK(layout.slices[1].runIndex == 1 && layout.slices[1].cpStart == 0);
}

static void testLayoutBoxAndBackground() {
    // frameH 600 -> padding = max(2, 600/60) = 10. Text 20x10 -> outer
    // 40x30, centered at (500, 300): bg (480, 285, 40, 30), text origin
    // (490, 295), baseline 295+8.
    TextDocument doc = singleRunDoc("AA");
    doc.box.wrap = 0.8;
    doc.box.background = true;
    doc.box.backgroundRgba = 0x11223344u;

    TextLayout layout = layoutText(doc, {shapeMono("AA")}, 1000, 600);
    CHECK(layout.bgX == 480);
    CHECK(layout.bgY == 285);
    CHECK(layout.bgW == 40);
    CHECK(layout.bgH == 30);
    CHECK(layout.slices[0].x == 490);
    CHECK(layout.slices[0].baselineY == 295 + 8);

    // Anchor at the top-left corner: the box hangs off the frame (no
    // clamping - placement is the user's choice).
    doc.box.anchorX = 0.0;
    doc.box.anchorY = 0.0;
    layout = layoutText(doc, {shapeMono("AA")}, 1000, 600);
    CHECK(layout.bgX == 0 - 20);
    CHECK(layout.bgY == 0 - 15);

    // Padding floors at 2 for tiny frames (frameH 60 -> 1 -> clamp 2).
    layout = layoutText(doc, {shapeMono("AA")}, 1000, 60);
    CHECK(layout.bgW == 20 + 2 * 2);
    CHECK(layout.bgH == 10 + 2 * 2);
}

static void testLayoutDegenerate() {
    TextLayout layout;

    // Empty document.
    layout = layoutText(TextDocument(), {}, 1000, 1000);
    CHECK(layout.lineCount == 0);
    CHECK(layout.textWidth == 0 && layout.textHeight == 0);
    CHECK(layout.slices.empty());
    CHECK(layout.bgW == 0 && layout.bgH == 0);

    // Shaped runs but an empty document (mismatched inputs).
    TextDocument doc;
    layout = layoutText(doc, {shapeMono("AA")}, 1000, 1000);
    CHECK(layout.lineCount == 0);

    // Empty shaped input with a non-empty document.
    layout = layoutText(singleRunDoc("AA"), {}, 1000, 1000);
    CHECK(layout.lineCount == 0);

    // Zero / negative frame.
    layout = layoutText(singleRunDoc("AA"), {shapeMono("AA")}, 0, 1000);
    CHECK(layout.lineCount == 0);
    layout = layoutText(singleRunDoc("AA"), {shapeMono("AA")}, 1000, 0);
    CHECK(layout.lineCount == 0);

    // Mismatched advance/byte tables make a run unshapable (skipped).
    ShapedRun broken = shapeMono("AA");
    broken.advances.pop_back();
    layout = layoutText(singleRunDoc("AA"), {broken}, 1000, 1000);
    CHECK(layout.lineCount == 0);

    // wrap clamps: > 1 -> frameW, tiny -> at least 1.
    doc = singleRunDoc("AA");
    doc.box.wrap = 2.0;
    layout = layoutText(doc, {shapeMono("AA")}, 1000, 1000);
    CHECK(layout.wrapWidth == 1000);
    doc.box.wrap = 0.0001;
    layout = layoutText(doc, {shapeMono("AA")}, 1000, 1000);
    CHECK(layout.wrapWidth == 1);

    // Spaces-only document: one blank line, zero ink width.
    layout = layoutText(singleRunDoc(" "), {shapeMono(" ")}, 1000, 1000);
    CHECK(layout.lineCount == 1);
    CHECK(layout.textWidth == 0);

    // Multi-byte codepoints keep byte ranges correct through wrapping.
    doc = singleRunDoc(std::string("a\xC3\xA9") + "b");
    doc.box.wrap = 0.025; // wrapW 25: "aéb" = 30 wide -> hard split at 2
    layout = layoutText(doc, {shapeMono(std::string("a\xC3\xA9") + "b")}, 1000, 1000);
    CHECK(layout.lineCount == 2);
    CHECK(layout.slices[0].cpCount == 2);
    CHECK(layout.slices[0].byteLen == 3); // 'a' + 2-byte 'é'
    CHECK(layout.slices[1].byteStart == 3 && layout.slices[1].byteLen == 1);
}

// ---------------------------------------------------------------------------
// compositeOver
// ---------------------------------------------------------------------------

static void testCompositeOver() {
    uint8_t dst[4 * 4];
    uint8_t src[4 * 4];
    const int w = 2;
    const int h = 2;

    // Opaque source replaces everything (fast path).
    for (int i = 0; i < 4; ++i) {
        dst[i * 4 + 0] = 10;
        dst[i * 4 + 1] = 20;
        dst[i * 4 + 2] = 30;
        dst[i * 4 + 3] = 255;
        src[i * 4 + 0] = 200;
        src[i * 4 + 1] = 100;
        src[i * 4 + 2] = 50;
        src[i * 4 + 3] = 255;
    }
    compositeOver(dst, src, w, h);
    for (int i = 0; i < 4; ++i) {
        CHECK(dst[i * 4 + 0] == 200);
        CHECK(dst[i * 4 + 1] == 100);
        CHECK(dst[i * 4 + 2] == 50);
        CHECK(dst[i * 4 + 3] == 255);
    }

    // Fully transparent source leaves dst untouched.
    for (int i = 0; i < 4; ++i) {
        dst[i * 4 + 0] = 10;
        dst[i * 4 + 1] = 20;
        dst[i * 4 + 2] = 30;
        dst[i * 4 + 3] = 120;
        src[i * 4 + 0] = 255;
        src[i * 4 + 1] = 255;
        src[i * 4 + 2] = 255;
        src[i * 4 + 3] = 0;
    }
    compositeOver(dst, src, w, h);
    CHECK(dst[0] == 10 && dst[1] == 20 && dst[2] == 30 && dst[3] == 120);

    // 50% white (alpha 128) over opaque black:
    // outA = (128*255 + 255*127)/255 = 255; outC = round(128*128*255 /
    // (255*255)) = 64.
    for (int i = 0; i < 4; ++i) {
        dst[i * 4 + 0] = 0;
        dst[i * 4 + 1] = 0;
        dst[i * 4 + 2] = 0;
        dst[i * 4 + 3] = 255;
        src[i * 4 + 0] = 128;
        src[i * 4 + 1] = 128;
        src[i * 4 + 2] = 128;
        src[i * 4 + 3] = 128;
    }
    compositeOver(dst, src, w, h);
    for (int i = 0; i < 4; ++i) {
        CHECK(dst[i * 4 + 0] == 64);
        CHECK(dst[i * 4 + 1] == 64);
        CHECK(dst[i * 4 + 2] == 64);
        CHECK(dst[i * 4 + 3] == 255);
    }

    // 50% white over 50% white: outA = round((128*255 + 128*127)/255)
    // = 192; outC = round((128*128*255 + 128*128*127) / (255*192)) = 128
    // (both channels equal, so the true blend is exactly 128).
    for (int i = 0; i < 4; ++i) {
        dst[i * 4 + 0] = 128;
        dst[i * 4 + 1] = 128;
        dst[i * 4 + 2] = 128;
        dst[i * 4 + 3] = 128;
    }
    compositeOver(dst, src, w, h);
    for (int i = 0; i < 4; ++i) {
        CHECK(dst[i * 4 + 0] == 128);
        CHECK(dst[i * 4 + 3] == 192);
    }

    // Channel independence: semi-red over semi-blue.
    dst[0] = 0;
    dst[1] = 0;
    dst[2] = 255;
    dst[3] = 200;
    src[0] = 255;
    src[1] = 0;
    src[2] = 0;
    src[3] = 100;
    compositeOver(dst, src, 1, 1);
    // outA = round((100*255 + 200*155)/255) = 222.
    const int outA = (100 * 255 + 200 * 155 + 127) / 255;
    CHECK(outA == 222);
    CHECK(dst[3] == outA);
    // outR = round((255*100*255 + 0) / (255*222)) = 115.
    CHECK(dst[0] == (255 * 100 * 255 + (255 * outA) / 2) / (255 * outA));
    CHECK(dst[0] == 115);
    CHECK(dst[1] == 0);
    // outB = round((0 + 255*200*155) / (255*222)) = 140.
    CHECK(dst[2] == (255 * 200 * 155 + (255 * outA) / 2) / (255 * outA));
    CHECK(dst[2] == 140);

    // Degenerate sizes are no-ops (no crash).
    compositeOver(nullptr, nullptr, 0, 0);
    compositeOver(dst, src, -1, 5);
}

// ---------------------------------------------------------------------------
// Timeline model: text tracks + text clips
// ---------------------------------------------------------------------------

static void testInsertTrack() {
    TimelineModel model;
    model.addTrack("V2", false);
    model.addTrack("V1", false);
    model.addTrack("A1", true);
    model.setFps(24.0);
    const int64_t video = model.addClip(1, "v.mp4", "V", 0, 240, 0);
    const int64_t left = model.addClip(1, "v.mp4", "L", 0, 48, 0);
    const int64_t right = model.addClip(1, "v.mp4", "R", 0, 48, 48);
    const int64_t trans = model.addTransition(left, right, "dissolve.cross", 12);
    CHECK(video > 0 && left > 0 && right > 0 && trans > 0);

    // Insert T1 at the top: everything shifts down one lane.
    const int t1 = model.insertTrack(0, "T1", false, true);
    CHECK(t1 == 0);
    CHECK(model.trackCount() == 4);
    CHECK(model.trackAt(0)->name == "T1" && model.trackAt(0)->isText);
    CHECK(model.trackAt(1)->name == "V2");
    CHECK(model.trackAt(2)->name == "V1");
    CHECK(model.trackAt(3)->name == "A1" && model.trackAt(3)->isAudio);
    for (int i = 0; i < 4; ++i) {
        CHECK(model.trackAt(i)->index == i);
    }
    const Clip *v = model.clipById(video);
    const Clip *l = model.clipById(left);
    CHECK(v->trackIndex == 2);
    CHECK(l->trackIndex == 2);
    const Transition *t = model.transitionById(trans);
    CHECK(t->trackIndex == 2);
    // The transition survived the renumber (pair intact).
    CHECK(model.transitionById(trans) != nullptr);
    CHECK(t->leftClipId == left && t->rightClipId == right);

    // Insert in the middle.
    const int t2 = model.insertTrack(2, "T2", false, true);
    CHECK(t2 == 2);
    CHECK(model.trackAt(2)->name == "T2");
    CHECK(model.trackAt(3)->name == "V1");
    CHECK(model.clipById(video)->trackIndex == 3);

    // Clamped positions and the audio+text contradiction.
    CHECK(model.insertTrack(-5, "X", false, true) == 0);
    CHECK(model.insertTrack(99, "Y", false, false) == model.trackCount() - 1);
    CHECK(model.insertTrack(0, "Z", true, true) == -1);
    // A plain addTrack is an insert at the end.
    const int a2 = model.addTrack("A2", true);
    CHECK(a2 == model.trackCount() - 1);
    CHECK(!model.trackAt(a2)->isText && model.trackAt(a2)->isAudio);
}

static void testAddTextClip() {
    TimelineModel model;
    model.addTrack("V1", false);
    model.addTrack("A1", true);
    model.insertTrack(0, "T1", false, true);
    model.setFps(24.0);

    TextDocument doc = singleRunDoc("Hello");
    doc.runs[0].style.size = 72;
    doc.runs[0].style.colorRgba = 0xFF8844EEu;
    doc.align = TextAlign::Center;
    doc.box.anchorX = 0.5;
    doc.box.anchorY = 0.9;
    doc.box.background = true;

    const int64_t id = model.addTextClip(0, doc, 24, 96);
    CHECK(id > 0);
    const Clip *clip = model.clipById(id);
    CHECK(clip != nullptr);
    CHECK(clip->isText);
    CHECK(clip->trackIndex == 0);
    CHECK(clip->timelineStart == 24);
    CHECK(clip->durationFrames() == 96);
    CHECK(clip->sourcePath.empty());
    CHECK(clip->sourceInFrames == 0);
    CHECK(clip->sourceOutFrames == 96);
    CHECK(clip->rate == 1.0);
    CHECK(clip->label == "Hello");
    CHECK(clip->text == doc); // round-trips through the value copy

    // Rejections: wrong kind of track, bad geometry, overlap.
    CHECK(model.addTextClip(1, doc, 0, 10) == 0);   // video track
    CHECK(model.addTextClip(2, doc, 0, 10) == 0);   // audio track
    CHECK(model.addTextClip(9, doc, 0, 10) == 0);   // no such track
    CHECK(model.addTextClip(0, doc, 0, 0) == 0);    // duration < 1
    CHECK(model.addTextClip(0, doc, -1, 10) == 0);  // negative start
    CHECK(model.addTextClip(0, doc, 100, 10) == 0); // [100,110) overlaps [24,120)
    CHECK(model.addTextClip(0, doc, 120, 10) > 0);  // fits after

    // Media clips cannot land on text tracks.
    CHECK(model.addClip(0, "v.mp4", "V", 0, 48, 200) == 0);
    CHECK(model.addClip(1, "v.mp4", "V", 0, 48, 0) > 0); // video track OK

    // The document normalizes on the way in.
    TextDocument unnormalized;
    TextRun a;
    a.text = "AB";
    TextRun empty;
    TextRun b;
    b.text = "CD";
    unnormalized.runs = {a, empty, b};
    const int64_t id2 = model.addTextClip(0, unnormalized, 200, 10);
    CHECK(id2 > 0);
    CHECK(model.clipById(id2)->text.runs.size() == 1);
    CHECK(model.clipById(id2)->text.runs[0].text == "ABCD");
}

static void testTextClipsAt() {
    TimelineModel model;
    model.addTrack("V1", false);
    model.insertTrack(0, "T1", false, true); // top text lane (index 0)
    model.insertTrack(1, "T2", false, true); // lower text lane (index 1)
    model.setFps(24.0);

    const int64_t top = model.addTextClip(0, singleRunDoc("top"), 0, 200);
    const int64_t bottom = model.addTextClip(1, singleRunDoc("bottom"), 0, 100);
    const int64_t bottomLater = model.addTextClip(1, singleRunDoc("later"), 100, 100);
    const int64_t video = model.addClip(2, "v.mp4", "V", 0, 240, 0);
    CHECK(top > 0 && bottom > 0 && bottomLater > 0 && video > 0);

    // Both text clips cover frame 50; the LOWER lane (T2) paints first.
    std::vector<const Clip *> clips = model.textClipsAt(50);
    CHECK(clips.size() == 2);
    CHECK(clips[0]->id == bottom);
    CHECK(clips[1]->id == top);

    // Only T1 covers frame 150 (T2's later clip does, but not the first).
    clips = model.textClipsAt(150);
    CHECK(clips.size() == 2);
    CHECK(clips[0]->id == bottomLater);
    CHECK(clips[1]->id == top);

    // Gaps on all text lanes -> empty.
    clips = model.textClipsAt(250);
    CHECK(clips.empty());

    // The video stack ignores text lanes (topmost VIDEO wins, text is not
    // "under" the video just because it sits in the track order).
    const Clip *active = model.activeVideoClipAt(50);
    CHECK(active != nullptr);
    CHECK(active->id == video);
}

static void testTextClipMutations() {
    TimelineModel model;
    model.addTrack("V1", false);
    model.insertTrack(0, "T1", false, true);
    model.insertTrack(1, "T2", false, true);
    model.setFps(24.0);
    const int64_t id = model.addTextClip(0, singleRunDoc("Title"), 0, 100);
    const int64_t id2 = model.addTextClip(1, singleRunDoc("Other"), 50, 50);
    CHECK(id > 0 && id2 > 0);

    // trimClipStart: shrink the head (content-independent).
    Clip *clip = model.clipById(id);
    CHECK(model.trimClipStart(id, 10));
    CHECK(clip->timelineStart == 10);
    CHECK(clip->sourceInFrames == 0); // invariant kept
    CHECK(clip->durationFrames() == 90);
    // Extend the head freely (no source bound).
    CHECK(model.trimClipStart(id, -10));
    CHECK(clip->timelineStart == 0);
    CHECK(clip->durationFrames() == 100);
    // Head cannot pass frame 0 or empty the clip.
    CHECK(!model.trimClipStart(id, -1)); // start would go to -1
    CHECK(model.trimClipStart(id, 99));
    CHECK(!model.trimClipStart(id, 1)); // would empty (duration 0)
    model.trimClipStart(id, -99);

    // trimClipEnd: same as media (extent grows/shrinks).
    CHECK(model.trimClipEnd(id, 20));
    CHECK(clip->durationFrames() == 120);
    CHECK(model.trimClipEnd(id, -20));
    CHECK(!model.trimClipEnd(id, -120)); // would empty

    // splitAt: the right half restarts its synthetic extent at 0.
    CHECK(model.splitAt(60, 0));
    CHECK(model.clips().size() == 3);
    const Clip *left = model.clipById(id);
    const Clip *right = nullptr;
    for (const Clip &c : model.clips()) {
        if (c.id != id && c.isText && c.timelineStart == 60) {
            right = &c;
        }
    }
    CHECK(right != nullptr);
    CHECK(left->isText && right->isText);
    CHECK(left->durationFrames() == 60);
    CHECK(left->sourceInFrames == 0 && left->sourceOutFrames == 60);
    CHECK(right->timelineStart == 60);
    CHECK(right->sourceInFrames == 0);   // THE text invariant
    CHECK(right->sourceOutFrames == 40); // 100 - 60
    CHECK(right->durationFrames() == 40);
    CHECK(right->text == left->text); // both halves show the same text
    CHECK(right->label == "Title (2)");

    // rollEdit between two text clips (same text lane).
    const int64_t t1 = model.addTextClip(0, singleRunDoc("A"), 300, 100);
    const int64_t t2 = model.addTextClip(0, singleRunDoc("B"), 400, 100);
    CHECK(t1 > 0 && t2 > 0);
    CHECK(model.rollEdit(t1, t2, 10)); // boundary slides right
    CHECK(model.clipById(t1)->durationFrames() == 110);
    CHECK(model.clipById(t2)->durationFrames() == 90);
    CHECK(model.clipById(t2)->timelineStart == 410);
    CHECK(model.clipById(t2)->sourceInFrames == 0);
    CHECK(model.rollEdit(t1, t2, -20)); // slides left
    CHECK(model.clipById(t1)->durationFrames() == 90);
    CHECK(model.clipById(t2)->timelineStart == 390);
    CHECK(!model.rollEdit(t1, t2, -91)); // would empty the left clip
    CHECK(!model.rollEdit(t1, t2, 120)); // would empty the right clip

    // moveClipTo: kind matrix.
    CHECK(model.moveClipTo(id2, 0, 200)); // T2 -> T1 (text to text OK)
    CHECK(!model.moveClipTo(id2, 2, 0));  // text -> video rejected
    const int64_t video = model.addClip(2, "v.mp4", "V", 0, 48, 0);
    CHECK(video > 0);
    CHECK(!model.moveClipTo(video, 0, 500)); // video -> text rejected
    CHECK(!model.moveClipTo(video, 1, 500)); // video -> text rejected
    CHECK(model.moveClipTo(video, 2, 10));   // video -> video OK

    // findDropPosition with clipId 0 (a NEW clip, kind pre-validated by
    // the caller): resolves a free gap on a text lane. T1 spans at this
    // point: [0,60) [60,100) [200,250) [300,390) [390,500).
    CHECK(model.findDropPosition(0, 0, 0, 40) == 100);   // nearest gap edge
    CHECK(model.findDropPosition(0, 0, 200, 40) == 160); // clamped into gap
    CHECK(model.findDropPosition(2, 0, 0, 48) == 58);    // video lane trailing gap

    // rippleDelete on a text lane closes the gap.
    const int64_t first = model.addTextClip(0, singleRunDoc("F"), 1000, 50);
    const int64_t second = model.addTextClip(0, singleRunDoc("S"), 1050, 50);
    CHECK(model.rippleDelete(first));
    CHECK(model.clipById(second)->timelineStart == 1000);

    // Transitions are rejected on text lanes.
    CHECK(model.addTransition(t1, t2, "dissolve.cross", 5) == 0); // adjacent text pair
    CHECK(model.maxTransitionDuration(t1, t2) == 0);

    // durationFrames includes text clips.
    model.addTextClip(0, singleRunDoc("far"), 100000, 10);
    CHECK(model.durationFrames() == 100010);
}

// ---------------------------------------------------------------------------
// Project codec: text tracks + text clips
// ---------------------------------------------------------------------------

static void testProjectTextRoundTrip() {
    TimelineModel model;
    model.insertTrack(0, "T1", false, true);
    model.addTrack("V1", false);
    model.addTrack("A1", true);
    model.setFps(30.0);

    TextDocument doc;
    TextRun run;
    run.text = "Fusion \xC3\xA9"
               "Cut";
    run.style.family = "Segoe UI";
    run.style.size = 96;
    run.style.bold = true;
    run.style.italic = true;
    run.style.underline = true;
    run.style.colorRgba = 0xFF8844C0u;
    doc.runs.push_back(run);
    TextRun run2;
    run2.text = "second";
    run2.style.size = 48;
    run2.style.colorRgba = 0x00FF00FFu;
    doc.runs.push_back(run2);
    doc.align = TextAlign::Right;
    doc.box.anchorX = 0.25;
    doc.box.anchorY = 0.75;
    doc.box.wrap = 0.5;
    doc.box.background = true;
    doc.box.backgroundRgba = 0x10203040u;

    const int64_t textId = model.addTextClip(0, doc, 30, 120);
    const int64_t videoId = model.addClip(1, "clip.mp4", "Clip", 10, 250, 0);
    CHECK(textId > 0 && videoId > 0);
    // An effect stack on a text clip rides along.
    model.clipById(textId)->effectStack.push_back(makeEffectInstance("color.brightness"));
    const int64_t left = model.addClip(1, "a.mp4", "A", 0, 48, 300);
    const int64_t right = model.addClip(1, "b.mp4", "B", 0, 48, 348);
    CHECK(model.addTransition(left, right, "dissolve.cross", 12) > 0);

    const std::string text = serializeProject(model);

    // Deterministic: same model, same bytes.
    CHECK(serializeProject(model) == text);

    // Round-trip into a fresh model.
    TimelineModel loaded;
    std::string error;
    CHECK(parseProject(text, loaded, error));
    CHECK(error.empty());
    CHECK(loaded.fps() == 30.0);
    CHECK(loaded.trackCount() == 3);
    CHECK(loaded.trackAt(0)->isText && !loaded.trackAt(0)->isAudio);
    CHECK(loaded.trackAt(0)->name == "T1");
    CHECK(!loaded.trackAt(1)->isText && !loaded.trackAt(1)->isAudio);
    CHECK(loaded.trackAt(2)->isAudio && !loaded.trackAt(2)->isText);

    const Clip *textClip = loaded.clipById(textId);
    CHECK(textClip != nullptr);
    CHECK(textClip->isText);
    CHECK(textClip->timelineStart == 30);
    CHECK(textClip->durationFrames() == 120);
    CHECK(textClip->sourcePath.empty());
    CHECK(textClip->sourceInFrames == 0);
    CHECK(textClip->rate == 1.0);
    CHECK(textClip->text == doc);
    CHECK(textClip->effectStack.size() == 1);
    CHECK(textClip->effectStack[0].effectId == "color.brightness");

    const Clip *videoClip = loaded.clipById(videoId);
    CHECK(videoClip != nullptr);
    CHECK(!videoClip->isText);
    CHECK(videoClip->sourcePath == "clip.mp4");
    CHECK(videoClip->sourceInFrames == 10 && videoClip->sourceOutFrames == 250);
    CHECK(loaded.transitions().size() == 1);

    // Re-serialize -> identical bytes (the codec is an involution on
    // valid models).
    CHECK(serializeProject(loaded) == text);

    // addTextClip keeps minting ids above the loaded set.
    CHECK(loaded.addTextClip(0, singleRunDoc("more"), 0, 10) > textId);
}

static void testProjectOldFormat() {
    // A pre-text-era file (no "text" anywhere) still loads.
    const std::string old =
        R"({"format":1,"fps":24,"tracks":[)"
        R"({"name":"V1","audio":false,"locked":false,"muted":false,"solo":false},)"
        R"({"name":"A1","audio":true,"locked":false,"muted":false,"solo":false}],)"
        R"("clips":[{"id":1,"track":0,"source":"a.mp4","label":"A","in":0,"out":48,"start":0,"rate":1,)"
        R"("effects":[]}],"transitions":[]})";
    TimelineModel model;
    std::string error;
    CHECK(parseProject(old, model, error));
    CHECK(model.trackCount() == 2);
    CHECK(!model.trackAt(0)->isText);
    CHECK(model.clips().size() == 1);
    CHECK(!model.clips()[0].isText);

    // A text track written with "text":true parses (and round-trips).
    const std::string withTextTrack =
        R"({"format":1,"fps":24,"tracks":[)"
        R"({"name":"T1","audio":false,"text":true,"locked":false,"muted":false,"solo":false},)"
        R"({"name":"V1","audio":false,"locked":false,"muted":false,"solo":false}],)"
        R"("clips":[],"transitions":[]})";
    CHECK(parseProject(withTextTrack, model, error));
    CHECK(model.trackAt(0)->isText);
    CHECK(serializeProject(model).find("\"text\":true") != std::string::npos);
}

static void testProjectTextRejections() {
    const std::string head =
        R"({"format":1,"fps":24,"tracks":[)"
        R"({"name":"T1","audio":false,"text":true,"locked":false,"muted":false,"solo":false},)"
        R"({"name":"V1","audio":false,"locked":false,"muted":false,"solo":false}],)"
        R"("clips":[)";
    const std::string tail = R"(],"transitions":[]})";
    const std::string textDoc =
        R"("text":{"align":"center","anchorX":0.5,"anchorY":0.5,"wrap":0.8,)"
        R"("background":false,"bgColor":"000000B4","runs":[)"
        R"({"text":"Hi","family":"","size":64,"bold":false,"italic":false,)"
        R"("underline":false,"color":"FFFFFFFF"}]})";

    TimelineModel model;
    std::string error;

    // Valid baseline.
    CHECK(parseProject(head + R"({"id":1,"track":0,"label":"Hi","start":0,"duration":48,)" +
                           textDoc + R"(,"effects":[]})" + tail,
                       model, error));

    // Text clip on a non-text track.
    CHECK(!parseProject(head + R"({"id":2,"track":1,"label":"Hi","start":0,"duration":48,)" +
                            textDoc + R"(,"effects":[]})" + tail,
                        model, error));

    // Media clip on a text track.
    CHECK(!parseProject(head +
                            R"({"id":3,"track":0,"source":"a.mp4","label":"A","in":0,"out":48,)"
                            R"("start":0,"rate":1,"effects":[]})" +
                            tail,
                        model, error));

    // Duration below 1.
    CHECK(!parseProject(head + R"({"id":4,"track":0,"label":"Hi","start":0,"duration":0,)" +
                            textDoc + R"(,"effects":[]})" + tail,
                        model, error));

    // Bad colors.
    CHECK(!parseProject(head + R"({"id":5,"track":0,"label":"Hi","start":0,"duration":48,)" +
                            R"("text":{"align":"center","anchorX":0.5,"anchorY":0.5,"wrap":0.8,)"
                            R"("background":false,"bgColor":"XYZ","runs":[]})" +
                            R"(,"effects":[]})" + tail,
                        model, error));
    CHECK(!parseProject(head + R"({"id":6,"track":0,"label":"Hi","start":0,"duration":48,)" +
                            R"("text":{"align":"center","anchorX":0.5,"anchorY":0.5,"wrap":0.8,)"
                            R"("background":false,"bgColor":"000000B4","runs":[)"
                            R"({"text":"Hi","family":"","size":64,"bold":false,"italic":false,)"
                            R"("underline":false,"color":"12345"}]})" +
                            R"(,"effects":[]})" + tail,
                        model, error));

    // Out-of-range fields.
    CHECK(!parseProject(head + R"({"id":7,"track":0,"label":"Hi","start":0,"duration":48,)" +
                            R"("text":{"align":"center","anchorX":1.5,"anchorY":0.5,"wrap":0.8,)"
                            R"("background":false,"bgColor":"000000B4","runs":[]})" +
                            R"(,"effects":[]})" + tail,
                        model, error));
    CHECK(!parseProject(head + R"({"id":8,"track":0,"label":"Hi","start":0,"duration":48,)" +
                            R"("text":{"align":"center","anchorX":0.5,"anchorY":0.5,"wrap":0,)"
                            R"("background":false,"bgColor":"000000B4","runs":[]})" +
                            R"(,"effects":[]})" + tail,
                        model, error));
    CHECK(!parseProject(head + R"({"id":9,"track":0,"label":"Hi","start":0,"duration":48,)" +
                            R"("text":{"align":"middle","anchorX":0.5,"anchorY":0.5,"wrap":0.8,)"
                            R"("background":false,"bgColor":"000000B4","runs":[]})" +
                            R"(,"effects":[]})" + tail,
                        model, error));

    // Run size out of range.
    CHECK(!parseProject(head + R"({"id":11,"track":0,"label":"Hi","start":0,"duration":48,)" +
                            R"("text":{"align":"center","anchorX":0.5,"anchorY":0.5,"wrap":0.8,)"
                            R"("background":false,"bgColor":"000000B4","runs":[)"
                            R"({"text":"Hi","family":"","size":999,"bold":false,"italic":false,)"
                            R"("underline":false,"color":"FFFFFFFF"}]})" +
                            R"(,"effects":[]})" + tail,
                        model, error));

    // A run missing a field (strict schema).
    CHECK(!parseProject(head + R"({"id":12,"track":0,"label":"Hi","start":0,"duration":48,)" +
                            R"("text":{"align":"center","anchorX":0.5,"anchorY":0.5,"wrap":0.8,)"
                            R"("background":false,"bgColor":"000000B4","runs":[)"
                            R"({"text":"Hi","family":"","size":64,"bold":false,"italic":false}]})" +
                            R"(,"effects":[]})" + tail,
                        model, error));

    // The audio+text track contradiction.
    CHECK(!parseProject(
        R"({"format":1,"fps":24,"tracks":[)"
        R"({"name":"X","audio":true,"text":true,"locked":false,"muted":false,"solo":false}],)"
        R"("clips":[],"transitions":[]})",
        model, error));
}

// ---------------------------------------------------------------------------
// Animation: the evaluator (progress, easing, clamping) and the block
// rect the box animations anchor to. Hand-derived expectations from the
// exact formulas: smoothstep(t) = t^2 (3-2t), easeOutBack(0.5) =
// 1 + 2.70158*(-0.125) + 1.70158*0.25 = 1.087697.
// ---------------------------------------------------------------------------

static void testAnimationIdentity() {
    TextAnimation none;
    CHECK(!hasTextAnimation(none));
    const TextAnimState st = textAnimationAt(none, 0, 100, 10);
    CHECK(st.alpha == 1.0);
    CHECK(st.offsetX == 0.0);
    CHECK(st.offsetY == 0.0);
    CHECK(st.scale == 1.0);
    CHECK(st.revealCodepoints == -1);
    CHECK(st.wipe == 1.0);

    // A kind with zero duration is inert.
    TextAnimation inert;
    inert.inKind = TextAnimKind::Fade;
    inert.outKind = TextAnimKind::Slide;
    CHECK(!hasTextAnimation(inert));
    const TextAnimState st2 = textAnimationAt(inert, 5, 100, 10);
    CHECK(st2.alpha == 1.0);
    CHECK(st2.offsetX == 0.0);

    // clipFrame < 0 renders the settled state.
    TextAnimation fade;
    fade.inKind = TextAnimKind::Fade;
    fade.inFrames = 10;
    const TextAnimState st3 = textAnimationAt(fade, -1, 100, 10);
    CHECK(st3.alpha == 0.0); // clamped: before the clip = pre-animation
    // duration 0 = identity.
    const TextAnimState st4 = textAnimationAt(fade, 5, 0, 10);
    CHECK(st4.alpha == 1.0);
}

static void testAnimationFade() {
    TextAnimation anim;
    anim.inKind = TextAnimKind::Fade;
    anim.inFrames = 10;
    anim.outKind = TextAnimKind::Fade;
    anim.outFrames = 10;
    const int64_t dur = 100;

    // Entrance: alpha = smoothstep(frame/10).
    CHECK(textAnimationAt(anim, 0, dur, 0).alpha == 0.0);
    CHECK(std::fabs(textAnimationAt(anim, 5, dur, 0).alpha - 0.5) < 1.0e-12);
    CHECK(textAnimationAt(anim, 10, dur, 0).alpha == 1.0);
    CHECK(textAnimationAt(anim, 50, dur, 0).alpha == 1.0);
    // Exit: alpha = smoothstep((dur-frame)/10) at the tail.
    CHECK(textAnimationAt(anim, 95, dur, 0).alpha == 0.5);
    CHECK(textAnimationAt(anim, 100, dur, 0).alpha == 0.0);
    // Fifth of the entrance: smoothstep(0.2) = 0.04 * 2.6 = 0.104.
    CHECK(std::fabs(textAnimationAt(anim, 2, dur, 0).alpha - 0.104) < 1.0e-12);
    // Both sides multiply: frame 5 with a 10-frame exit window from 15.
    TextAnimation both;
    both.inKind = TextAnimKind::Fade;
    both.inFrames = 10;
    both.outKind = TextAnimKind::Fade;
    both.outFrames = 10;
    // inT at frame 7 = 0.7 -> smoothstep = 0.49*1.6 = 0.784; outT=1.
    CHECK(std::fabs(textAnimationAt(both, 7, dur, 0).alpha - 0.784) < 1.0e-12);
}

static void testAnimationSlide() {
    TextAnimation anim;
    anim.inKind = TextAnimKind::Slide;
    anim.inFrames = 10;
    anim.outKind = TextAnimKind::Slide;
    anim.outFrames = 10;
    anim.dir = TextAnimDir::Left;
    const int64_t dur = 100;

    // Enter from the LEFT: offset starts one full frame-width left.
    const TextAnimState s0 = textAnimationAt(anim, 0, dur, 0);
    CHECK(s0.offsetX == -1.0);
    CHECK(s0.offsetY == 0.0);
    // Halfway (smoothstep 0.5): half a frame off.
    CHECK(std::fabs(textAnimationAt(anim, 5, dur, 0).offsetX + 0.5) < 1.0e-12);
    // Settled.
    CHECK(textAnimationAt(anim, 10, dur, 0).offsetX == 0.0);
    // Exit toward the OPPOSITE (right) edge.
    CHECK(std::fabs(textAnimationAt(anim, 95, dur, 0).offsetX - 0.5) < 1.0e-12);
    CHECK(textAnimationAt(anim, 100, dur, 0).offsetX == 1.0);

    // Direction map: Right mirrors X; Up enters from BELOW (+Y);
    // Down enters from above (-Y).
    anim.dir = TextAnimDir::Right;
    CHECK(textAnimationAt(anim, 0, dur, 0).offsetX == 1.0);
    CHECK(textAnimationAt(anim, 0, dur, 0).offsetY == 0.0);
    anim.dir = TextAnimDir::Up;
    CHECK(textAnimationAt(anim, 0, dur, 0).offsetY == 1.0);
    CHECK(textAnimationAt(anim, 0, dur, 0).offsetX == 0.0);
    anim.dir = TextAnimDir::Down;
    CHECK(textAnimationAt(anim, 0, dur, 0).offsetY == -1.0);
    // Exit toward the opposite edge of Up = downward is... Up's opposite
    // is Down: exit offset = -(+1) = -1 at the very end.
    anim.dir = TextAnimDir::Up;
    CHECK(textAnimationAt(anim, 100, dur, 0).offsetY == -1.0);
}

static void testAnimationPop() {
    TextAnimation anim;
    anim.inKind = TextAnimKind::Pop;
    anim.inFrames = 10;
    anim.outKind = TextAnimKind::Pop;
    anim.outFrames = 10;
    const int64_t dur = 100;

    CHECK(textAnimationAt(anim, 10, dur, 0).scale == 1.0);
    // easeOutBack(0) is 1 - c3 + c1, which cancels to 0 in exact math
    // but leaves a tiny float residual - compare with tolerance.
    CHECK(std::fabs(textAnimationAt(anim, 0, dur, 0).scale) < 1.0e-9);
    // The overshoot: easeOutBack(0.5) > 1.
    const double mid = textAnimationAt(anim, 5, dur, 0).scale;
    CHECK(std::fabs(mid - 1.087697) < 1.0e-5);
    CHECK(mid > 1.0);
    // Pop-out shrinks smoothly (no undershoot).
    CHECK(textAnimationAt(anim, 95, dur, 0).scale == 0.5);
    CHECK(textAnimationAt(anim, 100, dur, 0).scale == 0.0);
    // In+out multiply (settled in, half out).
    CHECK(std::fabs(textAnimationAt(anim, 50, dur, 0).scale - 1.0) < 1.0e-12);
}

static void testAnimationTypewriter() {
    TextAnimation anim;
    anim.inKind = TextAnimKind::Typewriter;
    anim.inFrames = 10;
    anim.outKind = TextAnimKind::Typewriter;
    anim.outFrames = 5;
    const int64_t dur = 20;
    const int64_t total = 20;

    // Entrance: linear reveal.
    CHECK(textAnimationAt(anim, 0, dur, total).revealCodepoints == 0);
    CHECK(textAnimationAt(anim, 5, dur, total).revealCodepoints == 10);
    CHECK(textAnimationAt(anim, 10, dur, total).revealCodepoints == -1); // all
    CHECK(textAnimationAt(anim, 15, dur, total).revealCodepoints == -1);
    // Exit: characters vanish from the end backward (fraction of total).
    CHECK(textAnimationAt(anim, 18, dur, total).revealCodepoints == 8);
    CHECK(textAnimationAt(anim, 19, dur, total).revealCodepoints == 4);
    CHECK(textAnimationAt(anim, 20, dur, total).revealCodepoints == 0);
    // Both sides: the minimum fraction wins (in 10, out 5, frame 2:
    // inT=0.2 -> 4 codepoints; outT = 18/5 clamped to 1).
    CHECK(textAnimationAt(anim, 2, dur, total).revealCodepoints == 4);
    // Non-integer reveal rounds: total 7, inT 0.5 -> llround(3.5) = 4.
    CHECK(textAnimationAt(anim, 5, dur, 7).revealCodepoints == 4);
    // The reveal never sets alpha/wipe/scale.
    const TextAnimState st = textAnimationAt(anim, 5, dur, total);
    CHECK(st.alpha == 1.0 && st.wipe == 1.0 && st.scale == 1.0);
    CHECK(st.offsetX == 0.0 && st.offsetY == 0.0);
}

static void testAnimationWipe() {
    TextAnimation anim;
    anim.inKind = TextAnimKind::Wipe;
    anim.inFrames = 10;
    anim.outKind = TextAnimKind::Wipe;
    anim.outFrames = 10;
    const int64_t dur = 100;

    CHECK(textAnimationAt(anim, 0, dur, 0).wipe == 0.0);
    CHECK(textAnimationAt(anim, 5, dur, 0).wipe == 0.5);
    CHECK(textAnimationAt(anim, 10, dur, 0).wipe == 1.0);
    CHECK(textAnimationAt(anim, 95, dur, 0).wipe == 0.5);
    CHECK(textAnimationAt(anim, 100, dur, 0).wipe == 0.0);
}

static void testAnimationDocumentEquality() {
    TextDocument a = singleRunDoc("Hi");
    TextDocument b = singleRunDoc("Hi");
    CHECK(a == b);
    b.animation.inKind = TextAnimKind::Fade;
    b.animation.inFrames = 12;
    CHECK(a != b);
    a.animation.inKind = TextAnimKind::Fade;
    a.animation.inFrames = 12;
    CHECK(a == b);
    a.animation.dir = TextAnimDir::Up;
    CHECK(a != b);
    b.animation.dir = TextAnimDir::Up;
    a.animation.outKind = TextAnimKind::Pop;
    a.animation.outFrames = 6;
    CHECK(a != b);
}

static void testLayoutBlockRect() {
    // Without a background the block rect is the text rect itself.
    {
        TextDocument doc = singleRunDoc("AB");
        std::vector<ShapedRun> shaped;
        shaped.push_back(shapeMono("AB"));
        const TextLayout lay = layoutText(doc, shaped, 100, 100);
        CHECK(lay.blockW == lay.textWidth);
        CHECK(lay.blockH == lay.textHeight);
        CHECK(lay.blockX + lay.blockW / 2 == 100 * 0.5); // centered anchor
        CHECK(lay.bgW == 0);                             // bg fields stay background-gated
    }
    // With a background the block rect includes the padding (600/60=10).
    {
        TextDocument doc = singleRunDoc("AB");
        doc.box.background = true;
        std::vector<ShapedRun> shaped;
        shaped.push_back(shapeMono("AB"));
        const TextLayout lay = layoutText(doc, shaped, 600, 600);
        CHECK(lay.blockW == lay.bgW);
        CHECK(lay.blockH == lay.bgH);
        CHECK(lay.blockX == lay.bgX);
        CHECK(lay.blockY == lay.bgY);
        CHECK(lay.bgW == lay.textWidth + 20);
        CHECK(lay.bgH == lay.textHeight + 20);
    }
    // Off-frame anchor: the block rect follows without clamping.
    {
        TextDocument doc = singleRunDoc("AB");
        doc.box.anchorX = 0.0;
        doc.box.anchorY = 0.0;
        std::vector<ShapedRun> shaped;
        shaped.push_back(shapeMono("AB"));
        const TextLayout lay = layoutText(doc, shaped, 100, 100);
        CHECK(lay.blockX == 0 - lay.blockW / 2);
        CHECK(lay.blockY == 0 - lay.blockH / 2);
    }
}

// ---------------------------------------------------------------------------

int main() {
    testUtf8Valid();
    testUtf8Invalid();
    testColorHelpers();
    testNormalize();
    testPreviewLabel();
    testLayoutSingleLine();
    testLayoutAlignment();
    testLayoutWordWrap();
    testLayoutHardSplit();
    testLayoutNewlines();
    testLayoutMixedMetrics();
    testLayoutBoxAndBackground();
    testLayoutDegenerate();
    testCompositeOver();
    testInsertTrack();
    testAddTextClip();
    testTextClipsAt();
    testTextClipMutations();
    testProjectTextRoundTrip();
    testProjectOldFormat();
    testProjectTextRejections();
    testAnimationIdentity();
    testAnimationFade();
    testAnimationSlide();
    testAnimationPop();
    testAnimationTypewriter();
    testAnimationWipe();
    testAnimationDocumentEquality();
    testLayoutBlockRect();
    return testExitCode("text");
}
