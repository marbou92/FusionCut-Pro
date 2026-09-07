// FusionCut Pro - color-emoji engine unit tests.
// Pure table parsing + cluster shaping math; no Qt, no FFmpeg - runs
// anywhere ctest runs. The expectations are pinned against the actual
// bundled font bytes (resources/fonts/NotoColorEmoji.ttf, Noto Color
// Emoji 2.047, CBDT/CBLC, sha256 72a635cb...): the file is committed,
// so every number below is byte-stable. Covers: the strike/index
// tables, the cmap, the GSUB ligature map (ZWJ chains, flags, keycaps,
// skin tones), the FE0F presentation policy, bitmap record decoding,
// the strike-scaling math, malformed-input robustness, and the layout
// engine's cluster-atomicity rule.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "emoji.h"
#include "test_harness.h"
#include "text.h"

using namespace fc;

#ifndef FC_EMOJI_FONT_PATH
#define FC_EMOJI_FONT_PATH "NotoColorEmoji.ttf"
#endif

// ---------------------------------------------------------------------------
// Fixture: the real bundled font, loaded once.
// ---------------------------------------------------------------------------

static std::vector<uint8_t> loadFontBytes() {
    std::ifstream in(FC_EMOJI_FONT_PATH, std::ios::binary);
    if (!in) {
        std::printf("FATAL: cannot open %s\n", FC_EMOJI_FONT_PATH);
        return {};
    }
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

static const std::vector<uint8_t> &fontBytes() {
    static const std::vector<uint8_t> bytes = loadFontBytes();
    return bytes;
}

static EmojiFont &theFont() {
    static EmojiFont font = [] {
        EmojiFont f;
        const std::vector<uint8_t> &b = fontBytes();
        f.load(b.data(), b.size());
        return f;
    }();
    return font;
}

// Codepoint helpers for readable cluster fixtures.
static std::vector<uint32_t> cps(std::initializer_list<uint32_t> in) {
    return std::vector<uint32_t>(in);
}

static void expectCluster(const char *what, const std::vector<uint32_t> &text, size_t pos,
                          bool shouldResolve, uint16_t glyph, int clusterLen) {
    const EmojiFont &font = theFont();
    EmojiFont::Resolved r;
    const bool ok = font.resolveCluster(text.data(), text.size(), pos, &r);
    CHECK(ok == shouldResolve);
    if (ok && shouldResolve) {
        if (r.glyph != glyph || r.codepoints != clusterLen) {
            std::printf("FAIL %s: glyph %u len %d, expected glyph %u len %d\n", what,
                        unsigned(r.glyph), r.codepoints, unsigned(glyph), clusterLen);
        }
        CHECK(r.glyph == glyph);
        CHECK(r.codepoints == clusterLen);
    }
}

// ---------------------------------------------------------------------------
// Load + introspection
// ---------------------------------------------------------------------------

static void testLoad() {
    const std::vector<uint8_t> &b = fontBytes();
    CHECK(b.size() == 10673480); // the committed file, byte for byte

    EmojiFont font;
    CHECK(theFont().loaded());
    CHECK(theFont().strikePpem() == 109);
    CHECK(theFont().bitDepth() == 32);
    CHECK(theFont().numGlyphs() == 4027);
    CHECK(theFont().ligatureRuleCount() == 4166);
    CHECK(theFont().maxLigatureSequence() == 9);

    // A default-constructed font resolves nothing.
    EmojiFont empty;
    EmojiFont::Resolved r;
    const std::vector<uint32_t> t = cps({0x1F600u});
    CHECK(!empty.resolveCluster(t.data(), t.size(), 0, &r));
    CHECK(!empty.bitmapFor(883, nullptr));
}

static void testMalformed() {
    const std::vector<uint8_t> &b = fontBytes();

    EmojiFont font;
    CHECK(!font.load(nullptr, 0));
    CHECK(!font.load(nullptr, 1000));
    CHECK(!font.load(b.data(), 0));
    CHECK(!font.load(b.data(), 12));           // header only
    CHECK(!font.load(b.data(), 100));          // table directory cut off
    CHECK(!font.load(b.data(), b.size() / 2)); // CBDT cut in half
    CHECK(!font.loaded());

    // Garbage that never parses: every load failure leaves the font in
    // the resolve-nothing state (and never crashes).
    std::vector<uint8_t> junk(1 << 20, 0xABu);
    CHECK(!font.load(junk.data(), junk.size()));
    EmojiFont::Resolved r;
    std::vector<uint32_t> t = cps({0x1F600u});
    CHECK(!font.resolveCluster(t.data(), t.size(), 0, &r));

    std::vector<uint8_t> ones(1 << 16, 0xFFu);
    CHECK(!font.load(ones.data(), ones.size()));

    // A valid header with a lying table count must not walk the file.
    std::vector<uint8_t> head(b.begin(), b.begin() + 12);
    head[4] = 0xFFu; // numTables = 0xFFFF
    head[5] = 0xFFu;
    CHECK(!font.load(head.data(), head.size()));

    // Reloading a good font over a failed one recovers.
    CHECK(font.load(b.data(), b.size()));
    CHECK(font.loaded());
}

// ---------------------------------------------------------------------------
// cmap
// ---------------------------------------------------------------------------

static void testCmap() {
    const EmojiFont &font = theFont();
    // Pinned glyph ids (byte-stable for the committed font).
    CHECK(font.codepointGlyph(0x1F600u) == 883); // grinning face
    CHECK(font.codepointGlyph(0x2764u) == 168);  // heavy black heart
    CHECK(font.codepointGlyph(0x1F469u) == 597); // woman
    CHECK(font.codepointGlyph(0x1F468u) == 596); // man
    CHECK(font.codepointGlyph(0x1F467u) == 595); // girl
    CHECK(font.codepointGlyph(0x1F466u) == 594); // boy
    CHECK(font.codepointGlyph(0x1F1FAu) == 225); // regional indicator U
    CHECK(font.codepointGlyph(0x1F1F8u) == 223); // regional indicator S
    CHECK(font.codepointGlyph(0x1F1EBu) == 210); // regional indicator F
    CHECK(font.codepointGlyph(0x1F1F4u) == 219); // regional indicator R
    CHECK(font.codepointGlyph(0x1F1E9u) == 208); // regional indicator D
    CHECK(font.codepointGlyph(0x1F1EAu) == 209); // regional indicator E
    CHECK(font.codepointGlyph(0x200Du) == 18);   // zero-width joiner (mapped, no bitmap)
    CHECK(font.codepointGlyph(0xFE0Fu) == 0);    // VS16: unmapped (it never renders)
    CHECK(font.codepointGlyph(0x20E3u) == 21);   // combining enclosing keycap
    CHECK(font.codepointGlyph(0x31u) == 7);      // digit 1 (keycap component)
    CHECK(font.codepointGlyph(0x1F3FBu) == 487); // light skin tone
    CHECK(font.codepointGlyph(0x263Au) == 79);   // white smiling face
    CHECK(font.codepointGlyph(0x1F9D1u) == 1277);
    CHECK(font.codepointGlyph(0x1FAF0u) == 1435);
    CHECK(font.codepointGlyph(0x1F5A4u) == 857); // black heart

    // Not in the emoji font at all -> plain text.
    CHECK(font.codepointGlyph(0x41u) == 0);
    CHECK(font.codepointGlyph(0x4E00u) == 0);
    CHECK(font.codepointGlyph(0x1F777u) == 0);

    // This font carries no format-14 non-default UVS mappings.
    CHECK(font.variantGlyph(0x2764u, 0xFE0Fu) == 0);
    CHECK(font.variantGlyph(0x1F600u, 0xFE0Fu) == 0);
}

// ---------------------------------------------------------------------------
// Ligature rules (the GSUB map)
// ---------------------------------------------------------------------------

static void testLigatures() {
    const EmojiFont &font = theFont();
    const uint16_t us[] = {225, 223};
    CHECK(font.ligatureFor(us, 2) == 1772); // US flag
    const uint16_t fr[] = {210, 219};
    CHECK(font.ligatureFor(fr, 2) == 1614); // FR flag
    const uint16_t de[] = {208, 209};
    CHECK(font.ligatureFor(de, 2) == 1596); // DE flag
    const uint16_t keycap[] = {7, 21};
    CHECK(font.ligatureFor(keycap, 2) == 1485); // keycap 1
    const uint16_t skin[] = {569, 487};
    CHECK(font.ligatureFor(skin, 2) == 1967); // thumbs up + light skin
    const uint16_t family5[] = {596, 18, 597, 18, 595};
    CHECK(font.ligatureFor(family5, 5) == 2022); // family: man, woman, girl
    const uint16_t family7[] = {596, 18, 597, 18, 595, 18, 594};
    CHECK(font.ligatureFor(family7, 7) == 2023); // family: man, woman, girl, boy
    const uint16_t couple[] = {597, 18, 168, 18, 596};
    CHECK(font.ligatureFor(couple, 5) == 2294); // couple with heart

    // Non-rules: singles are not ligatures, and these sequences do not
    // exist in this font's GSUB (invalid input classes).
    const uint16_t single[] = {883};
    CHECK(font.ligatureFor(single, 1) == 0);
    const uint16_t pair[] = {883, 883};
    CHECK(font.ligatureFor(pair, 2) == 0); // two grinning faces
    const uint16_t mw[] = {597, 18, 596};
    CHECK(font.ligatureFor(mw, 3) == 0); // woman ZWJ man: not an emoji
    const uint16_t joined[] = {596, 18, 597};
    CHECK(font.ligatureFor(joined, 3) == 0);
    CHECK(font.ligatureFor(nullptr, 2) == 0);
}

// ---------------------------------------------------------------------------
// Cluster resolution (the shaping policy)
// ---------------------------------------------------------------------------

static void testResolveSingles() {
    // SMP singles: default emoji presentation.
    expectCluster("grin", cps({0x1F600u}), 0, true, 883, 1);
    expectCluster("rocket", cps({0x1F680u}), 0, true, 963, 1);
    expectCluster("skin swatch", cps({0x1F3FBu}), 0, true, 487, 1);

    // VS16 forces presentation and is consumed by the cluster.
    expectCluster("grin+VS16", cps({0x1F600u, 0xFE0Fu}), 0, true, 883, 2);
    expectCluster("heart+VS16", cps({0x2764u, 0xFE0Fu}), 0, true, 168, 2);
    expectCluster("smile+VS16", cps({0x263Au, 0xFE0Fu}), 0, true, 79, 2);
    expectCluster("digit+VS16", cps({0x31u, 0xFE0Fu}), 0, true, 7, 2);

    // Without VS16 these are TEXT presentation (Unicode's own rule) -
    // the platform font renders them.
    expectCluster("heart alone", cps({0x2764u}), 0, false, 0, 0);
    expectCluster("smile alone", cps({0x263Au}), 0, false, 0, 0);
    expectCluster("digit alone", cps({0x31u}), 0, false, 0, 0);
    expectCluster("letter", cps({0x41u}), 0, false, 0, 0);
    expectCluster("cjk", cps({0x4E00u}), 0, false, 0, 0);

    // The joiner and VS16 alone are plain text (VS16 is unmapped).
    expectCluster("lone ZWJ", cps({0x200Du}), 0, false, 0, 0);
    expectCluster("lone VS16", cps({0xFE0Fu}), 0, false, 0, 0);

    // Separators never start a cluster and terminate the scan.
    expectCluster("space", cps({0x20u, 0x1F600u}), 0, false, 0, 0);
    expectCluster("newline", cps({0x0Au, 0x1F600u}), 0, false, 0, 0);
    expectCluster("tab", cps({0x09u, 0x1F600u}), 0, false, 0, 0);
}

static void testResolveSequences() {
    // Flags: regional indicator pairs are ligature rules; two adjacent
    // pairs resolve independently (US then FR - no 4-glyph rule).
    expectCluster("US flag", cps({0x1F1FAu, 0x1F1F8u}), 0, true, 1772, 2);
    expectCluster("US+FR", cps({0x1F1FAu, 0x1F1F8u, 0x1F1EBu, 0x1F1F4u}), 0, true, 1772, 2);
    expectCluster("FR tail", cps({0x1F1FAu, 0x1F1F8u, 0x1F1EBu, 0x1F1F4u}), 2, true, 1614, 2);

    // Keycaps: VS16 inside the sequence is skipped by glyph matching
    // and consumed by the cluster.
    expectCluster("keycap 1", cps({0x31u, 0xFE0Fu, 0x20E3u}), 0, true, 1485, 3);
    expectCluster("keycap 1 no VS", cps({0x31u, 0x20E3u}), 0, true, 1485, 2);

    // Skin tone modifier.
    expectCluster("skin", cps({0x1F44Du, 0x1F3FBu}), 0, true, 1967, 2);

    // ZWJ chains.
    expectCluster("family 5", cps({0x1F468u, 0x200Du, 0x1F469u, 0x200Du, 0x1F467u}), 0, true, 2022,
                  5);
    expectCluster("family 7",
                  cps({0x1F468u, 0x200Du, 0x1F469u, 0x200Du, 0x1F467u, 0x200Du, 0x1F466u}), 0, true,
                  2023, 7);
    // The heart of the couple carries a VS16 in the wild; it is
    // swallowed mid-sequence.
    expectCluster("couple with VS16", cps({0x1F469u, 0x200Du, 0x2764u, 0xFE0Fu, 0x200Du, 0x1F468u}),
                  0, true, 2294, 6);
    // A trailing VS16 after a matched sequence joins the cluster.
    expectCluster("family 7 + VS16",
                  cps({0x1F468u, 0x200Du, 0x1F469u, 0x200Du, 0x1F467u, 0x200Du, 0x1F466u, 0xFE0Fu}),
                  0, true, 2023, 8);

    // Invalid sequence classes: the man resolves alone; the stray
    // joiner and woman render as text.
    expectCluster("man ZWJ woman", cps({0x1F468u, 0x200Du, 0x1F469u}), 0, true, 596, 1);
    expectCluster("two grins", cps({0x1F600u, 0x1F600u}), 0, true, 883, 1);
    expectCluster("two grins tail", cps({0x1F600u, 0x1F600u}), 1, true, 883, 1);

    // The sequence stops at a separator.
    expectCluster("grin space grin", cps({0x1F600u, 0x20u, 0x1F600u}), 0, true, 883, 1);
    // ... and at an unmapped codepoint.
    expectCluster("grin letter", cps({0x1F600u, 0x41u}), 0, true, 883, 1);

    // Degenerate calls.
    EmojiFont::Resolved r;
    const std::vector<uint32_t> t = cps({0x1F600u});
    CHECK(!theFont().resolveCluster(nullptr, 1, 0, &r));
    CHECK(!theFont().resolveCluster(t.data(), t.size(), 1, &r)); // pos past end
    CHECK(!theFont().resolveCluster(t.data(), 0, 0, &r));
    CHECK(!theFont().resolveCluster(t.data(), t.size(), 0, nullptr));
}

// ---------------------------------------------------------------------------
// Bitmap records
// ---------------------------------------------------------------------------

static void testBitmaps() {
    const EmojiFont &font = theFont();

    EmojiFont::Bitmap b;
    CHECK(font.bitmapFor(883, &b)); // grinning face
    CHECK(b.metrics.width == 136);
    CHECK(b.metrics.height == 128);
    CHECK(b.metrics.bearingX == 0);
    CHECK(b.metrics.bearingY == 101);
    CHECK(b.metrics.advance == 136);
    CHECK(b.ppem == 109);
    CHECK(b.pngLen == 3390);
    // The payload IS a PNG (the acceptance rule for every record).
    CHECK(b.png != nullptr);
    CHECK(b.png[0] == 0x89 && b.png[1] == 0x50 && b.png[2] == 0x4E && b.png[3] == 0x47);
    CHECK(b.png[4] == 0x0D && b.png[5] == 0x0A && b.png[6] == 0x1A && b.png[7] == 0x0A);

    // A ligature glyph's bitmap (family of four).
    CHECK(font.bitmapFor(2023, &b));
    CHECK(b.pngLen == 1645);
    CHECK(b.metrics.width == 136 && b.metrics.height == 128);

    // Flag and heart records.
    CHECK(font.bitmapFor(1772, &b)); // the US flag ligature glyph
    CHECK(b.pngLen > 0);
    CHECK(b.metrics.width == 136 && b.metrics.height == 128);
    CHECK(b.png[0] == 0x89 && b.png[1] == 0x50); // PNG signature
    CHECK(font.bitmapFor(168, &b));
    CHECK(b.pngLen == 1145);

    // Glyphs without bitmaps: .notdef, the joiner, the range gap, and
    // ids outside the font.
    CHECK(!font.bitmapFor(0, &b));
    CHECK(!font.bitmapFor(18, &b));   // ZWJ glyph: mapped, never drawn
    CHECK(!font.bitmapFor(1444, &b)); // hole between subtable ranges
    CHECK(!font.bitmapFor(4027, &b)); // == numGlyphs: out of range
    CHECK(!font.bitmapFor(5000, &b));
    CHECK(!font.bitmapFor(883, nullptr));
}

// ---------------------------------------------------------------------------
// Strike scaling
// ---------------------------------------------------------------------------

static void testScaling() {
    // (v * size + ppem/2) / ppem, floored at 0 - hand-derived.
    CHECK(emojiScaleStrike(136, 72, 109) == 90); // (9792+54)/109
    CHECK(emojiScaleStrike(101, 72, 109) == 67); // (7272+54)/109
    CHECK(emojiScaleStrike(27, 72, 109) == 18);  // (1944+54)/109
    CHECK(emojiScaleStrike(136, 109, 109) == 136);
    CHECK(emojiScaleStrike(136, 54, 109) == 67); // (7344+54)/109 = 67
    CHECK(emojiScaleStrike(136, 1, 109) == 1);
    CHECK(emojiScaleStrike(50, 218, 109) == 100); // (10900+54)/109
    CHECK(emojiScaleStrike(0, 72, 109) == 0);
    CHECK(emojiScaleStrike(136, 0, 109) == 0);
    CHECK(emojiScaleStrike(136, 72, 0) == 0);
    CHECK(emojiScaleStrike(136, -5, 109) == 0);
    // Font-math consistency: the strike advance (136 px at ppem 109)
    // equals the hmtx advance (2550/2048 em) evaluated at 109 px:
    // 2550*109/2048 = 135.7 -> 136; scaling the STRIKE to the same
    // size is the identity, which is the property the renderer relies
    // on (one scale factor, metrics and pixels agree).
    CHECK(emojiScaleStrike(136, 109, 109) == 136);
}

// ---------------------------------------------------------------------------
// Presentation policy
// ---------------------------------------------------------------------------

static void testPresentationPolicy() {
    CHECK(EmojiFont::isDefaultEmojiPresentation(0x1F600u));
    CHECK(EmojiFont::isDefaultEmojiPresentation(0x1F1E6u)); // lone regional indicator
    CHECK(EmojiFont::isDefaultEmojiPresentation(0x1F3FBu));
    CHECK(EmojiFont::isDefaultEmojiPresentation(0x1FAFFu));
    CHECK(!EmojiFont::isDefaultEmojiPresentation(0x1FB00u));
    CHECK(!EmojiFont::isDefaultEmojiPresentation(0x1EFFFu));
    CHECK(EmojiFont::isDefaultEmojiPresentation(0x231Au)); // watch
    CHECK(EmojiFont::isDefaultEmojiPresentation(0x2615u)); // coffee
    CHECK(EmojiFont::isDefaultEmojiPresentation(0x2648u)); // zodiac
    CHECK(EmojiFont::isDefaultEmojiPresentation(0x2653u));
    CHECK(!EmojiFont::isDefaultEmojiPresentation(0x2654u));
    CHECK(EmojiFont::isDefaultEmojiPresentation(0x2B50u)); // star
    CHECK(EmojiFont::isDefaultEmojiPresentation(0x2705u));
    CHECK(EmojiFont::isDefaultEmojiPresentation(0x2757u));
    CHECK(EmojiFont::isDefaultEmojiPresentation(0x26A1u));
    // Text-presentation BMP symbols need VS16 (Unicode's rule).
    CHECK(!EmojiFont::isDefaultEmojiPresentation(0x2764u));
    CHECK(!EmojiFont::isDefaultEmojiPresentation(0x263Au));
    CHECK(!EmojiFont::isDefaultEmojiPresentation(0x41u));
    CHECK(!EmojiFont::isDefaultEmojiPresentation(0xFE0Fu));
    CHECK(!EmojiFont::isDefaultEmojiPresentation(0x200Du));
}

// ---------------------------------------------------------------------------
// Layout: clusters are atomic under hard splits
// ---------------------------------------------------------------------------

// A shaped run of n codepoints with the given advances + cluster
// annotation (a single cluster covering [0, n) when clustered=true).
static ShapedRun shapeClustered(int n, const std::vector<int> &advances, bool clustered) {
    ShapedRun run;
    run.runIndex = 0;
    run.codepoints.resize(n);
    for (int i = 0; i < n; ++i) {
        run.codepoints[size_t(i)] = 0x100u + uint32_t(i); // plain non-space codepoints
    }
    run.advances = advances;
    run.byteStarts.resize(size_t(n) + 1);
    for (int i = 0; i <= n; ++i) {
        run.byteStarts[size_t(i)] = i * 2; // pretend 2 bytes per codepoint
    }
    run.ascent = 8;
    run.descent = 2;
    run.lineGap = 0;
    if (clustered) {
        run.clusterStarts.assign(size_t(n), 1);
        run.clusterStarts[0] = 1;
        for (int i = 1; i < n; ++i) {
            run.clusterStarts[size_t(i)] = 0;
        }
        run.emojiGlyphs.assign(size_t(n), 0);
        run.emojiGlyphs[0] = 883; // a pretend emoji cluster head
    }
    return run;
}

static TextDocument oneRunDoc() {
    TextDocument doc;
    TextRun run;
    run.text = "placeholder"; // content irrelevant: shaping is synthetic
    doc.runs.push_back(run);
    return doc;
}

static void testLayoutClusterAtomic() {
    const TextDocument doc = oneRunDoc();

    // A 3-codepoint cluster (advances 50+40+0 = 90) that overflows a
    // 60px wrap width: the split point the width arithmetic picks (after
    // the first codepoint) is MID-CLUSTER, so the whole cluster moves
    // to the line and renders alone, overflowing - one line, one slice.
    {
        std::vector<ShapedRun> shaped;
        shaped.push_back(shapeClustered(3, {50, 40, 0}, true));
        const TextLayout lay = layoutText(doc, shaped, 600, 100); // wrap = 600*0.8 = 480
        CHECK(lay.lineCount == 1);                                // 90 px word: fits, no split
        CHECK(lay.textWidth == 90);
    }
    {
        TextDocument d = oneRunDoc();
        d.box.wrap = 0.1; // 60 px wrap width
        std::vector<ShapedRun> shaped;
        shaped.push_back(shapeClustered(3, {50, 40, 0}, true));
        const TextLayout lay = layoutText(d, shaped, 600, 100);
        CHECK(lay.lineCount == 1);  // the cluster stayed whole
        CHECK(lay.textWidth == 90); // 50 + 40 + 0
        CHECK(lay.slices.size() == 1);
        CHECK(lay.slices[0].cpCount == 3); // one slice covers the cluster
        CHECK(lay.slices[0].cpStart == 0);
    }
    // Same advances WITHOUT the cluster annotation: the width
    // arithmetic splits after codepoint 0 (50 <= 60, 90 > 60).
    {
        TextDocument d = oneRunDoc();
        d.box.wrap = 0.1;
        std::vector<ShapedRun> shaped;
        shaped.push_back(shapeClustered(3, {50, 40, 0}, false));
        const TextLayout lay = layoutText(d, shaped, 600, 100);
        CHECK(lay.lineCount == 2);
        CHECK(lay.textWidth == 50);
        CHECK(lay.slices.size() == 2);
        CHECK(lay.slices[0].cpCount == 1);
        CHECK(lay.slices[1].cpCount == 2);
    }
    // A cluster that FITS the wrap width: no split either way (the
    // annotation changes nothing).
    {
        TextDocument d = oneRunDoc();
        d.box.wrap = 0.1;
        std::vector<ShapedRun> shaped;
        shaped.push_back(shapeClustered(3, {10, 10, 0}, true));
        const TextLayout lay = layoutText(d, shaped, 600, 100);
        CHECK(lay.lineCount == 1);
        CHECK(lay.textWidth == 20);
    }
    // Mismatched annotation sizes make the run unshapable (the same
    // rule as mismatched advance tables).
    {
        TextDocument d = oneRunDoc();
        std::vector<ShapedRun> shaped;
        shaped.push_back(shapeClustered(3, {50, 40, 0}, true));
        shaped[0].clusterStarts.resize(2); // wrong size
        const TextLayout lay = layoutText(d, shaped, 600, 100);
        CHECK(lay.slices.empty());
        CHECK(lay.lineCount == 0);
    }
    {
        TextDocument d = oneRunDoc();
        std::vector<ShapedRun> shaped;
        shaped.push_back(shapeClustered(3, {50, 40, 0}, true));
        shaped[0].emojiGlyphs.resize(4); // wrong size
        const TextLayout lay = layoutText(d, shaped, 600, 100);
        CHECK(lay.slices.empty());
    }
    // clusterStarts alone (no emojiGlyphs) is a legal combination.
    {
        TextDocument d = oneRunDoc();
        d.box.wrap = 0.1;
        std::vector<ShapedRun> shaped;
        shaped.push_back(shapeClustered(3, {50, 40, 0}, true));
        shaped[0].emojiGlyphs.clear();
        const TextLayout lay = layoutText(d, shaped, 600, 100);
        CHECK(lay.lineCount == 1);
        CHECK(lay.textWidth == 90);
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    if (fontBytes().empty()) {
        return 1; // loadFontBytes already printed the fatal message
    }
    testLoad();
    testMalformed();
    testCmap();
    testLigatures();
    testResolveSingles();
    testResolveSequences();
    testBitmaps();
    testScaling();
    testPresentationPolicy();
    testLayoutClusterAtomic();
    return testExitCode("emoji");
}
