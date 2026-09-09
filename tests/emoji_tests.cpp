// FusionCut Pro - emoji cluster segmentation unit tests.
// Pure Unicode policy: no fonts, no files - the codepoint tables below
// pin every rule of emojiClusterLength (joiner chains, flag pairs,
// keycaps, skin tones, variation selectors, tag sequences) and the
// default-emoji-presentation set, plus the cluster-aware typewriter
// truncation that buildTextDocument documents.

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "emoji_clusters.h"
#include "test_harness.h"
#include "text.h"

using namespace fc;

// ---------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------

// Builds a codepoint vector from an initializer list.
static std::vector<uint32_t> cps(std::initializer_list<uint32_t> list) {
    return std::vector<uint32_t>(list);
}

// The cluster length at `pos` for the given codepoints.
static int cl(const std::vector<uint32_t> &v, size_t pos) {
    return emojiClusterLength(v.data(), v.size(), pos);
}

static TextDocument docOf(const std::string &utf8) {
    TextDocument doc;
    TextRun run;
    run.text = utf8;
    doc.runs.push_back(run);
    return doc;
}

// ---------------------------------------------------------------------------
// Single codepoints and forced presentation.
// ---------------------------------------------------------------------------

static void testSingles() {
    // Default emoji presentation: the cluster is the codepoint itself.
    CHECK(cl(cps({0x1F600}), 0) == 1); // grinning face
    CHECK(cl(cps({0x1F44D}), 0) == 1); // thumbs up
    CHECK(cl(cps({0x231Au}), 0) == 1); // watch (BMP default)
    CHECK(cl(cps({0x1F3F4}), 0) == 1); // black flag (no tags)
    CHECK(cl(cps({0x1F1FA}), 0) == 1); // lone regional indicator

    // Text-default symbols render as text without a forcing follower.
    CHECK(cl(cps({0x2764u}), 0) == 0); // heavy heart, no FE0F
    CHECK(cl(cps({0x2600u}), 0) == 0); // sun, no FE0F
    CHECK(cl(cps({'a'}), 0) == 0);     // plain letter
    CHECK(cl(cps({'1'}), 0) == 0);     // plain digit
    CHECK(cl(cps({0x2122u}), 0) == 0); // trademark

    // Variation selectors force a cluster on the base.
    CHECK(cl(cps({0x2764u, 0xFE0Fu}), 0) == 2); // heart + VS16
    CHECK(cl(cps({0x2764u, 0xFE0Eu}), 0) == 2); // heart + VS15 (text form)
    CHECK(cl(cps({0x2122u, 0xFE0Fu}), 0) == 2); // trademark + VS16
    CHECK(cl(cps({'a', 0xFE0Fu}), 0) == 2);     // even a letter (harmless)

    // VS16 after a default-emoji codepoint: attaches, same cluster.
    CHECK(cl(cps({0x1F600, 0xFE0Fu}), 0) == 2);
    // Two variation selectors in a row both attach.
    CHECK(cl(cps({0x1F600, 0xFE0Fu, 0xFE0Fu}), 0) == 3);

    // Joiner/selector codepoints never START a cluster.
    CHECK(cl(cps({0xFE0Fu}), 0) == 0);
    CHECK(cl(cps({0xFE0Eu}), 0) == 0);
    CHECK(cl(cps({0x200Du}), 0) == 0);
    CHECK(cl(cps({0x20E3u}), 0) == 0);
    CHECK(cl(cps({0x1F3FBu}), 0) == 0);          // skin tone alone
    CHECK(cl(cps({0xFE0Fu, 0x1F600u}), 0) == 0); // VS16 before emoji
}

// ---------------------------------------------------------------------------
// Keycaps.
// ---------------------------------------------------------------------------

static void testKeycaps() {
    CHECK(cl(cps({'1', 0xFE0F, 0x20E3}), 0) == 3); // 1 + VS16 + keycap
    CHECK(cl(cps({'1', 0x20E3}), 0) == 2);         // keycap without VS16
    CHECK(cl(cps({'#', 0xFE0F, 0x20E3}), 0) == 3);
    CHECK(cl(cps({'*', 0x20E3}), 0) == 2);
    CHECK(cl(cps({'9', 0xFE0F, 0x20E3}), 0) == 3);
    // A keycap base without any forcing follower is plain text.
    CHECK(cl(cps({'1', '2'}), 0) == 0);
    CHECK(cl(cps({'#', ' '}), 0) == 0);
}

// ---------------------------------------------------------------------------
// Skin tones.
// ---------------------------------------------------------------------------

static void testSkinTones() {
    CHECK(cl(cps({0x1F44D, 0x1F3FB}), 0) == 2); // thumbs up + light skin
    CHECK(cl(cps({0x1F44D, 0x1F3FF}), 0) == 2); // thumbs up + dark skin
    // Skin tone attaches to a default-emoji base.
    CHECK(cl(cps({0x1F9D1, 0x1F3FC}), 0) == 2);
    // A skin tone on a text-default base without VS16: no cluster.
    CHECK(cl(cps({0x2764, 0x1F3FB}), 0) == 0);
    // ... but with VS16 the whole sequence clusters.
    CHECK(cl(cps({0x2764, 0xFE0F, 0x1F3FB}), 0) == 3);
}

// ---------------------------------------------------------------------------
// Zero-width-joiner chains.
// ---------------------------------------------------------------------------

static void testJoinerChains() {
    // Family: man + ZWJ + woman + ZWJ + girl.
    CHECK(cl(cps({0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467}), 0) == 5);
    // Family with a trailing VS16 on the last member.
    CHECK(cl(cps({0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467, 0xFE0F}), 0) == 6);
    // Couple with heart: woman + ZWJ + heart(VS16) + ZWJ + man.
    CHECK(cl(cps({0x1F469, 0x200D, 0x2764, 0xFE0F, 0x200D, 0x1F468}), 0) == 6);
    // Kiss: man + ZWJ + kiss mark + ZWJ + man (no VS16 on the heart).
    CHECK(cl(cps({0x1F468, 0x200D, 0x1F48B, 0x200D, 0x1F468}), 0) == 5);
    // Handshake with skin tones on BOTH sides.
    CHECK(cl(cps({0x1F91A, 0x1F3FB, 0x200D, 0x1F91A, 0x1F3FF}), 0) == 5);
    // Dangling joiner at end of text: the cluster ends before it.
    CHECK(cl(cps({0x1F468, 0x200D}), 0) == 1);
    // Joiner followed by plain text: not consumed.
    CHECK(cl(cps({0x1F600, 0x200D, 'a'}), 0) == 1);
    // Joiner + text-default heart (no VS16): not a cluster start, stops.
    CHECK(cl(cps({0x1F600, 0x200D, 0x2764}), 0) == 1);
    // Two adjacent families scan as two five-codepoint clusters.
    const std::vector<uint32_t> two =
        cps({0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467, 0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467});
    CHECK(cl(two, 0) == 5);
    CHECK(cl(two, 5) == 5);
}

// ---------------------------------------------------------------------------
// Regional indicators (flags).
// ---------------------------------------------------------------------------

static void testFlags() {
    // US flag: 1F1FA + 1F1F8.
    CHECK(cl(cps({0x1F1FA, 0x1F1F8}), 0) == 2);
    // The second indicator of a pair is not a continuation of a NEW scan
    // starting there when it consumed one already: a lone indicator
    // after a pair starts its own (single) cluster.
    const std::vector<uint32_t> three = cps({0x1F1FA, 0x1F1F8, 0x1F1EB});
    CHECK(cl(three, 0) == 2);
    CHECK(cl(three, 2) == 1);
    // Four indicators: two flag pairs.
    const std::vector<uint32_t> four = cps({0x1F1FA, 0x1F1F8, 0x1F1EB, 0x1F1F7});
    CHECK(cl(four, 0) == 2);
    CHECK(cl(four, 2) == 2);
    // Indicator followed by a non-indicator: single cluster.
    CHECK(cl(cps({0x1F1FA, 'x'}), 0) == 1);
    // Indicator + VS16 (some files carry flags with VS16): attaches.
    CHECK(cl(cps({0x1F1FA, 0xFE0F, 0x1F1F8}), 0) == 3);
}

// ---------------------------------------------------------------------------
// Tag sequences (subdivision flags).
// ---------------------------------------------------------------------------

static void testTags() {
    // England: 1F3F4 + E0067 E0062 E0065 E006E E0067 E007F.
    const std::vector<uint32_t> eng =
        cps({0x1F3F4, 0xE0067, 0xE0062, 0xE0065, 0xE006E, 0xE0067, 0xE007F});
    CHECK(cl(eng, 0) == 7);
    // Scotland: 1F3F4 + E0067 E0062 E0073 E0063 E0074 E007F.
    const std::vector<uint32_t> sco =
        cps({0x1F3F4, 0xE0067, 0xE0062, 0xE0073, 0xE0063, 0xE0074, 0xE007F});
    CHECK(cl(sco, 0) == 7);
    // Tag characters directly after a NON-flag base are plain text.
    CHECK(cl(cps({'a', 0xE0067}), 0) == 0);
    // A malformed tag run (no terminator) is still swallowed.
    CHECK(cl(cps({0x1F3F4, 0xE0067, 0xE0062, 'x'}), 0) == 3);
    // Stray tag characters alone do not cluster.
    CHECK(cl(cps({0xE0067}), 0) == 0);
}

// ---------------------------------------------------------------------------
// Mixed streams and boundaries.
// ---------------------------------------------------------------------------

static void testMixedStreams() {
    // Plain text, cluster, plain text: three separate scans.
    const std::vector<uint32_t> mix = cps({'a', 'b', 0x1F600, 'c'});
    CHECK(cl(mix, 0) == 0);
    CHECK(cl(mix, 1) == 0);
    CHECK(cl(mix, 2) == 1);
    CHECK(cl(mix, 3) == 0);
    // Newlines are plain separators, never cluster members.
    const std::vector<uint32_t> nl = cps({0x1F600, 0x0A, 0x1F600});
    CHECK(cl(nl, 0) == 1);
    CHECK(cl(nl, 1) == 0);
    CHECK(cl(nl, 2) == 1);
    // Space between two emoji: two clusters.
    const std::vector<uint32_t> sp = cps({0x1F600, ' ', 0x1F44D});
    CHECK(cl(sp, 0) == 1);
    CHECK(cl(sp, 1) == 0);
    CHECK(cl(sp, 2) == 1);
    // Text + keycap + text.
    const std::vector<uint32_t> key = cps({'x', '1', 0xFE0F, 0x20E3, 'y'});
    CHECK(cl(key, 0) == 0);
    CHECK(cl(key, 1) == 3);
    CHECK(cl(key, 2) == 0); // continuation positions report plain
    CHECK(cl(key, 4) == 0);
    // Degenerate calls.
    CHECK(emojiClusterLength(nullptr, 3, 0) == 0);
    CHECK(emojiClusterLength(mix.data(), 0, 0) == 0);
    CHECK(emojiClusterLength(mix.data(), mix.size(), mix.size()) == 0);
    CHECK(emojiClusterLength(mix.data(), mix.size(), 99) == 0);
}

// ---------------------------------------------------------------------------
// Default emoji presentation policy.
// ---------------------------------------------------------------------------

static void testDefaultPresentation() {
    // SMP emoji range: wholesale.
    CHECK(isDefaultEmojiPresentation(0x1F000));
    CHECK(isDefaultEmojiPresentation(0x1F600));
    CHECK(isDefaultEmojiPresentation(0x1FAFF));
    CHECK(!isDefaultEmojiPresentation(0x1FB00));
    CHECK(!isDefaultEmojiPresentation(0x10FFFF));
    // BMP defaults (the stable Emoji_Presentation set).
    CHECK(isDefaultEmojiPresentation(0x231A));
    CHECK(isDefaultEmojiPresentation(0x23E9));
    CHECK(isDefaultEmojiPresentation(0x25B6));
    CHECK(isDefaultEmojiPresentation(0x2648));
    CHECK(isDefaultEmojiPresentation(0x26BD));
    CHECK(isDefaultEmojiPresentation(0x2705));
    CHECK(isDefaultEmojiPresentation(0x2757));
    CHECK(isDefaultEmojiPresentation(0x27BF));
    CHECK(isDefaultEmojiPresentation(0x2B50));
    // BMP text-default symbols (need VS16).
    CHECK(!isDefaultEmojiPresentation(0x2764));
    CHECK(!isDefaultEmojiPresentation(0x2600));
    CHECK(!isDefaultEmojiPresentation(0x00A9));
    CHECK(!isDefaultEmojiPresentation(0x203C));
    CHECK(!isDefaultEmojiPresentation(0x2194));
    // 0x231C sits in the gap right after the watch pair (231A-231B).
    CHECK(!isDefaultEmojiPresentation(0x231C));
    CHECK(isDefaultEmojiPresentation(0x231B));
    // Modifiers and regional indicators fall inside the wholesale SMP
    // range (over-inclusive on purpose); the SCANNER rules (never lead
    // a cluster, pair up, attach as continuations) are what keep them
    // in line, not the presentation table.
    CHECK(isDefaultEmojiPresentation(0x1F3FB));
    CHECK(isDefaultEmojiPresentation(0x1F3FF));
    CHECK(isDefaultEmojiPresentation(0x1F1FA));
    // ... and they still never START clusters:
    CHECK(cl(cps({0x1F3FB}), 0) == 0);
    CHECK(cl(cps({0x1F3FB, 0x1F3FC}), 0) == 0);
}

// ---------------------------------------------------------------------------
// Cluster-aware typewriter truncation.
// ---------------------------------------------------------------------------

static void testTruncate() {
    // Plain text: exact codepoint cut.
    TextDocument out;
    truncateTextDocument(docOf("Hello"), 3, out);
    CHECK(out.runs.size() == 1);
    CHECK(out.runs[0].text == "Hel");
    // Budget beyond the text: full copy.
    truncateTextDocument(docOf("Hello"), 99, out);
    CHECK(out.runs.size() == 1);
    CHECK(out.runs[0].text == "Hello");
    // Zero/negative budget: the document truncates to nothing (zero
    // runs - nothing visible).
    truncateTextDocument(docOf("Hello"), 0, out);
    CHECK(out.runs.empty());
    // Negative budget clamps to zero.
    truncateTextDocument(docOf("Hello"), -5, out);
    CHECK(out.runs.empty());

    // Astral codepoints count once: "Hi \xF0\x9F\x98\x80!" (Hi grin!)
    // = H i space grin ! = 5 codepoints.
    const std::string grin = "Hi \xF0\x9F\x98\x80!";
    truncateTextDocument(docOf(grin), 3, out);
    CHECK(out.runs.size() == 1);
    CHECK(out.runs[0].text == "Hi ");
    truncateTextDocument(docOf(grin), 4, out);
    CHECK(out.runs[0].text == "Hi \xF0\x9F\x98\x80");
    truncateTextDocument(docOf(grin), 5, out);
    CHECK(out.runs[0].text == grin);

    // A cut INSIDE a family emoji shrinks to the cluster start.
    const std::string family = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D"
                               "\xF0\x9F\x91\xA7"; // 5 codepoints
    truncateTextDocument(docOf(family), 3, out);
    CHECK(out.runs.empty());
    truncateTextDocument(docOf(family), 5, out);
    CHECK(out.runs[0].text == family);
    // Text + family: cut at 6 keeps the text + nothing (family needs 5
    // more); cut at 7 keeps everything.
    const std::string mix = "ab" + family;
    truncateTextDocument(docOf(mix), 6, out);
    CHECK(out.runs[0].text == "ab");
    truncateTextDocument(docOf(mix), 7, out);
    CHECK(out.runs[0].text == mix);

    // Multi-run documents: the budget spans runs; box + align + the
    // animation carry over verbatim. Runs AFTER the cut point are
    // dropped entirely (not even an empty stub remains).
    TextDocument two = docOf("one ");
    TextRun second;
    second.text = "two";
    two.runs.push_back(second);
    two.align = TextAlign::Left;
    two.box.anchorX = 0.25;
    truncateTextDocument(two, 5, out);
    CHECK(out.runs.size() == 2);
    CHECK(out.runs[0].text == "one ");
    CHECK(out.runs[1].text == "t");
    CHECK(out.align == TextAlign::Left);
    CHECK(out.box.anchorX == 0.25);
    // Everything after the cut is dropped entirely.
    truncateTextDocument(two, 2, out);
    CHECK(out.runs.size() == 1);
    CHECK(out.runs[0].text == "on");
    truncateTextDocument(two, 4, out);
    CHECK(out.runs.size() == 1);
    CHECK(out.runs[0].text == "one ");
    // Newlines count as codepoints.
    truncateTextDocument(docOf("a\nb"), 2, out);
    CHECK(out.runs[0].text == "a\n");
}

static void testCount() {
    CHECK(countTextCodepoints(docOf("Hello")) == 5);
    CHECK(countTextCodepoints(docOf("")) == 0);
    CHECK(countTextCodepoints(docOf("Hi \xF0\x9F\x98\x80")) == 4); // astral = 1
    TextDocument two = docOf("ab");
    TextRun second;
    second.text = "c\nd";
    two.runs.push_back(second);
    CHECK(countTextCodepoints(two) == 5);
    // Invalid bytes decode to U+FFFD (one per maximal prefix).
    CHECK(countTextCodepoints(docOf("\xFF\xFE")) == 2);
    CHECK(countTextCodepoints(docOf("\xE0\x80")) == 2);
}

// ---------------------------------------------------------------------------
// Synthetic emoji fonts: the parsers + shaper + probes pinned against
// hand-built bytes (a minimal CBDT face, a minimal sbix face, and a
// TrueType Collection wrapping a junk face + the CBDT face). No font
// file is needed - the suite stays hermetic.
// ---------------------------------------------------------------------------

#include "emoji.h"
#include "emoji_fixture.h"

static void testCbdtFixtureLoad() {
    const std::vector<uint8_t> f = fixture::buildCbdtFont(
        fixture::fakePng(8), fixture::fakePng(7), fixture::fakePng(12), fixture::fakePng(10));
    EmojiFont font;
    CHECK(font.load(f.data(), f.size()));
    CHECK(font.loaded());
    CHECK(font.numGlyphs() == 8);
    CHECK(font.strikePpem() == 10);
    CHECK(font.bitDepth() == 32);
    CHECK(font.ligatureRuleCount() == 1);
    CHECK(font.maxLigatureSequence() == 2);
    CHECK(!font.isSbix());
    CHECK(font.codepointGlyph(0x1F600) == 1);
    CHECK(font.codepointGlyph(0x1F44D) == 2);
    CHECK(font.codepointGlyph(0x1F1FA) == 3);
    CHECK(font.codepointGlyph(0x1F1F8) == 4);
    CHECK(font.codepointGlyph(0x2764) == 6);
    CHECK(font.codepointGlyph(0x41) == 8);
    CHECK(font.codepointGlyph(0x1F601) == 0); // not mapped
    CHECK(font.variantGlyph(0x2764, 0xFE0F) == 7);
    CHECK(font.variantGlyph(0x2764, 0xFE0E) == 0);
    const uint16_t flag[2] = {3, 4};
    CHECK(font.ligatureFor(flag, 2) == 5);
    const uint16_t tooLong[3] = {3, 4, 9};
    CHECK(font.ligatureFor(tooLong, 3) == 0);
    const uint16_t single[1] = {3};
    CHECK(font.ligatureFor(single, 1) == 0);
}

static void testCbdtBitmaps() {
    const std::vector<uint8_t> grin = fixture::fakePng(8);
    const std::vector<uint8_t> thumbs = fixture::fakePng(7);
    const std::vector<uint8_t> flag = fixture::fakePng(12);
    const std::vector<uint8_t> heart = fixture::fakePng(10);
    const std::vector<uint8_t> f = fixture::buildCbdtFont(grin, thumbs, flag, heart);
    EmojiFont font;
    CHECK(font.load(f.data(), f.size()));
    EmojiFont::Bitmap bm;
    CHECK(font.bitmapFor(1, &bm));
    CHECK(bm.metrics.width == 10);
    CHECK(bm.metrics.height == 10);
    CHECK(bm.metrics.bearingX == 1);
    CHECK(bm.metrics.bearingY == 9);
    CHECK(bm.metrics.advance == 10);
    CHECK(bm.ppem == 10);
    CHECK(bm.format == 0);
    CHECK(bm.dataLen == grin.size());
    CHECK(bm.data != nullptr);
    CHECK(std::memcmp(bm.data, grin.data(), grin.size()) == 0);
    CHECK(bm.mirror == false);
    CHECK(font.bitmapFor(2, &bm));
    CHECK(bm.metrics.width == 9 && bm.metrics.height == 9 && bm.metrics.advance == 9);
    CHECK(bm.dataLen == thumbs.size());
    CHECK(font.bitmapFor(5, &bm));
    CHECK(bm.metrics.width == 12 && bm.metrics.height == 8 && bm.metrics.advance == 12);
    CHECK(bm.dataLen == flag.size());
    CHECK(font.bitmapFor(7, &bm));
    CHECK(bm.metrics.width == 6 && bm.metrics.height == 6 && bm.metrics.advance == 6);
    CHECK(bm.dataLen == heart.size());
    // Missing classes: mapped glyphs without records, glyph 0, past
    // numGlyphs, and a degenerate call.
    CHECK(!font.bitmapFor(3, &bm)); // regional-U: no record
    CHECK(!font.bitmapFor(4, &bm)); // regional-S: no record
    CHECK(!font.bitmapFor(6, &bm)); // heart base: no record
    CHECK(!font.bitmapFor(8, &bm)); // 'A': mapped, never a bitmap
    CHECK(!font.bitmapFor(0, &bm)); // .notdef
    CHECK(!font.bitmapFor(9, &bm)); // == numGlyphs
    CHECK(!font.bitmapFor(1, nullptr));
}

static void testCbdtResolve() {
    const std::vector<uint8_t> f = fixture::buildCbdtFont(
        fixture::fakePng(8), fixture::fakePng(7), fixture::fakePng(12), fixture::fakePng(10));
    EmojiFont font;
    CHECK(font.load(f.data(), f.size()));
    EmojiFont::Resolved r;
    // A single default-emoji-presentation codepoint.
    const uint32_t grin[1] = {0x1F600};
    CHECK(font.resolveCluster(grin, 1, 0, &r));
    CHECK(r.glyph == 1);
    CHECK(r.codepoints == 1);
    // The flag ligature.
    const uint32_t flag[2] = {0x1F1FA, 0x1F1F8};
    CHECK(font.resolveCluster(flag, 2, 0, &r));
    CHECK(r.glyph == 5);
    CHECK(r.codepoints == 2);
    // The flag ligature with a TRAILING VS16 (consumed into the cluster).
    const uint32_t flagVs[3] = {0x1F1FA, 0x1F1F8, 0xFE0F};
    CHECK(font.resolveCluster(flagVs, 3, 0, &r));
    CHECK(r.glyph == 5);
    CHECK(r.codepoints == 3);
    // VS16 forcing presentation via the fmt14 variant mapping.
    const uint32_t heart[2] = {0x2764, 0xFE0F};
    CHECK(font.resolveCluster(heart, 2, 0, &r));
    CHECK(r.glyph == 7);
    CHECK(r.codepoints == 2);
    // Text presentation selector does NOT force emoji.
    const uint32_t heartText[2] = {0x2764, 0xFE0E};
    CHECK(!font.resolveCluster(heartText, 2, 0, &r));
    // Plain text and separators never resolve.
    const uint32_t letter[1] = {0x41};
    CHECK(!font.resolveCluster(letter, 1, 0, &r));
    const uint32_t space[1] = {0x20};
    CHECK(!font.resolveCluster(space, 1, 0, &r));
    // An unmapped skin-tone modifier after a mapped base: the cluster
    // ends at the base (the modifier is text the caller handles).
    const uint32_t skin[2] = {0x1F44D, 0x1F3FB};
    CHECK(font.resolveCluster(skin, 2, 0, &r));
    CHECK(r.glyph == 2);
    CHECK(r.codepoints == 1);
    // Degenerate calls.
    CHECK(!font.resolveCluster(nullptr, 1, 0, &r));
    CHECK(!font.resolveCluster(grin, 1, 1, &r));
    CHECK(!font.resolveCluster(grin, 0, 0, &r));
    CHECK(!font.resolveCluster(grin, 1, 0, nullptr));
}

static void testSbixFixture() {
    const std::vector<uint8_t> png = fixture::fakePng(20);
    const std::vector<uint8_t> f = fixture::buildSbixFont(png);
    EmojiFont font;
    CHECK(font.load(f.data(), f.size()));
    CHECK(font.loaded());
    CHECK(font.isSbix());
    CHECK(font.numGlyphs() == 6);
    CHECK(font.strikePpem() == 20);
    CHECK(font.ligatureRuleCount() == 0); // no GSUB table at all
    CHECK(font.codepointGlyph(0x1F600) == 1);
    CHECK(font.codepointGlyph(0x1F601) == 2);
    CHECK(font.codepointGlyph(0x1F602) == 3);
    CHECK(font.codepointGlyph(0x1F603) == 4);
    CHECK(font.codepointGlyph(0x1F604) == 5);
    CHECK(font.codepointGlyph(0x1F605) == 0);

    // glyph 1: the 'png ' record with hmtx-scaled advance.
    EmojiFont::Bitmap bm;
    CHECK(font.bitmapFor(1, &bm));
    CHECK(bm.format == 0x706E6720u); // 'png '
    CHECK(bm.metrics.width == 0);    // decode-dependent, unknown here
    CHECK(bm.metrics.height == 0);
    CHECK(bm.metrics.bearingX == 1); // originOffsetX
    CHECK(bm.metrics.advance == 20); // hmtx 1000 units * 20 / 1000
    CHECK(bm.ppem == 20);
    CHECK(bm.originY == -2); // originOffsetY
    CHECK(bm.mirror == false);
    CHECK(bm.dataLen == png.size());
    CHECK(std::memcmp(bm.data, png.data(), png.size()) == 0);
    // glyph 2: 'dupe' -> glyph 1's record.
    CHECK(font.bitmapFor(2, &bm));
    CHECK(bm.format == 0x706E6720u);
    CHECK(bm.metrics.bearingX == 1);
    CHECK(bm.metrics.advance == 20); // dupe resolves THEN scales hmtx
    CHECK(bm.dataLen == png.size());
    CHECK(bm.mirror == false);
    // glyph 3: 'flip' + 'dupe' -> mirrored glyph 1.
    CHECK(font.bitmapFor(3, &bm));
    CHECK(bm.format == 0x706E6720u);
    CHECK(bm.mirror == true);
    CHECK(bm.dataLen == png.size());
    // glyph 4 (zero-length) / glyph 5 ('tiff') / degenerates: missing.
    CHECK(!font.bitmapFor(4, &bm));
    CHECK(!font.bitmapFor(5, &bm));
    CHECK(!font.bitmapFor(0, &bm));
    CHECK(!font.bitmapFor(6, &bm)); // == numGlyphs

    // No GSUB: the shaper resolves SINGLES only (a family sequence
    // falls back to per-codepoint bitmaps in the app layer).
    EmojiFont::Resolved r;
    const uint32_t grin[1] = {0x1F600};
    CHECK(font.resolveCluster(grin, 1, 0, &r));
    CHECK(r.glyph == 1);
    CHECK(r.codepoints == 1);
    const uint32_t duo[2] = {0x1F600, 0x1F601};
    CHECK(font.resolveCluster(duo, 2, 0, &r)); // base single, then stops
    CHECK(r.glyph == 1);
    CHECK(r.codepoints == 1);
    const uint32_t unmapped[1] = {0x1F1FA};
    CHECK(!font.resolveCluster(unmapped, 1, 0, &r));
}

static void testSbixDegenerate() {
    const std::vector<uint8_t> png = fixture::fakePng(20);
    const std::vector<uint8_t> f = fixture::buildSbixFont(png);
    // Rename 'hmtx' in the directory: the sbix route then refuses to
    // load (no advances, no placement math).
    {
        std::vector<uint8_t> g = f;
        bool patched = false;
        for (size_t i = 12; i + 16 <= g.size(); i += 16) {
            if (std::memcmp(g.data() + i, "hmtx", 4) == 0) {
                g[i + 3] = 'z';
                patched = true;
                break;
            }
        }
        CHECK(patched);
        EmojiFont font;
        CHECK(!font.load(g.data(), g.size()));
    }
    // Self-referencing 'dupe' cycle: the depth guard reads it as
    // missing (never hangs, never garbage).
    {
        std::vector<uint8_t> g = f;
        const uint8_t pat[4] = {'d', 'u', 'p', 'e'};
        const auto it = std::search(g.begin(), g.end(), pat, pat + 4);
        CHECK(it != g.end());
        const size_t target = size_t(it - g.begin()) + 4; // u16 right after the tag
        CHECK(g[target] == 0 && g[target + 1] == 1);
        g[target + 1] = 2; // dupe -> glyph 2 (itself)
        EmojiFont font;
        CHECK(font.load(g.data(), g.size()));
        EmojiFont::Bitmap bm;
        CHECK(!font.bitmapFor(2, &bm)); // cycle: missing
        CHECK(font.bitmapFor(1, &bm));  // untouched
    }
    // Truncations: load fails (never crashes).
    {
        for (size_t cut : {size_t(0), size_t(11), size_t(12), size_t(64), f.size() / 2}) {
            EmojiFont font;
            std::vector<uint8_t> g(f.begin(), f.begin() + long(std::min(cut, f.size())));
            const bool ok = font.load(g.data(), g.size());
            CHECK(ok == (cut == f.size()));
            CHECK(font.codepointGlyph(0x1F600) == 0 || cut == f.size());
        }
        EmojiFont font;
        CHECK(font.load(f.data(), f.size())); // recovery: a good re-load works
    }
}

static void testTtcAndProbes() {
    const std::vector<uint8_t> cbdt = fixture::buildCbdtFont(
        fixture::fakePng(8), fixture::fakePng(7), fixture::fakePng(12), fixture::fakePng(10));
    const std::vector<uint8_t> sbix = fixture::buildSbixFont(fixture::fakePng(20));
    const std::vector<uint8_t> junk = fixture::buildJunkSfnt();
    const std::vector<uint8_t> ttc = fixture::wrapTtc(junk, cbdt);

    // The collection: load skips the junk face and keeps the emoji one.
    EmojiFont font;
    CHECK(font.load(ttc.data(), ttc.size()));
    CHECK(font.codepointGlyph(0x1F600) == 1);
    CHECK(font.ligatureRuleCount() == 1);
    CHECK(font.bitmapFor(1, nullptr) == false);
    EmojiFont::Bitmap bm;
    CHECK(font.bitmapFor(5, &bm));
    CHECK(bm.metrics.advance == 12);

    // A collection whose EVERY sub-font is junk fails outright.
    const std::vector<uint8_t> ttcJunk = fixture::wrapTtc(junk, junk);
    EmojiFont none;
    CHECK(!none.load(ttcJunk.data(), ttcJunk.size()));

    // The light directory probe.
    CHECK(sfntBitmapEmojiFormat(cbdt.data(), cbdt.size()) == EmojiFontFormat::Cbdt);
    // A short HEAD with the real file size as the limit: the tables
    // sit past the head, but the directory knows they exist (this is
    // exactly how discovery probes a 10 MB font with a 64 KB read).
    CHECK(sfntBitmapEmojiFormat(cbdt.data(), 160, int64_t(cbdt.size())) == EmojiFontFormat::Cbdt);
    CHECK(sfntBitmapEmojiFormat(sbix.data(), 160, int64_t(sbix.size())) == EmojiFontFormat::Sbix);
    // Without the limit, a short head sees tables "past the file".
    CHECK(sfntBitmapEmojiFormat(cbdt.data(), 160) == EmojiFontFormat::None);
    CHECK(sfntBitmapEmojiFormat(sbix.data(), sbix.size()) == EmojiFontFormat::Sbix);
    CHECK(sfntBitmapEmojiFormat(ttc.data(), ttc.size()) == EmojiFontFormat::Cbdt);
    CHECK(sfntBitmapEmojiFormat(junk.data(), junk.size()) == EmojiFontFormat::None);
    CHECK(sfntBitmapEmojiFormat(nullptr, 0) == EmojiFontFormat::None);
    const std::vector<uint8_t> garbage(64, 0xAB);
    CHECK(sfntBitmapEmojiFormat(garbage.data(), garbage.size()) == EmojiFontFormat::None);
    // A lying numTables: the light probe is lenient (a match stops the
    // scan before the records run past the file); the full load
    // rejects it - the malformed battery pins that).
    {
        std::vector<uint8_t> g = cbdt;
        g[4] = 0;
        g[5] = 250; // numTables = 250, way past the file
        CHECK(sfntBitmapEmojiFormat(g.data(), g.size()) == EmojiFontFormat::Cbdt);
    }

    // Family names.
    CHECK(sfntFamilyName(cbdt.data(), cbdt.size()) == "Fixture CBDT");
    CHECK(sfntFamilyName(sbix.data(), sbix.size()) == "Fixture SBIX");
    CHECK(sfntFamilyName(ttc.data(), ttc.size()) == "Fixture CBDT");
    CHECK(sfntFamilyName(junk.data(), junk.size()).empty());
    CHECK(sfntFamilyName(nullptr, 0).empty());
}

static void testCbdtMalformedBattery() {
    const std::vector<uint8_t> f = fixture::buildCbdtFont(
        fixture::fakePng(8), fixture::fakePng(7), fixture::fakePng(12), fixture::fakePng(10));
    // Truncation at strategic offsets never crashes; lookups fail.
    for (size_t cut : {size_t(0), size_t(11), size_t(12), size_t(28), size_t(100), size_t(200),
                       f.size() / 3, f.size() / 2}) {
        EmojiFont font;
        std::vector<uint8_t> g(f.begin(), f.begin() + long(std::min(cut, f.size())));
        font.load(g.data(), g.size());
        CHECK(font.codepointGlyph(0x1F600) == 0);
        CHECK(font.ligatureRuleCount() == 0);
        EmojiFont::Bitmap bm;
        CHECK(!font.bitmapFor(1, &bm));
        EmojiFont::Resolved r;
        const uint32_t grin[1] = {0x1F600};
        CHECK(!font.resolveCluster(grin, 1, 0, &r));
    }
    // All-0xFF and a lying header.
    {
        std::vector<uint8_t> g(f.size(), 0xFF);
        EmojiFont font;
        CHECK(!font.load(g.data(), g.size()));
    }
    {
        std::vector<uint8_t> g = f;
        g[5] = 250; // numTables beyond the file
        EmojiFont font;
        CHECK(!font.load(g.data(), g.size()));
    }
    // Recovery: after any failure, a good load on the SAME object works.
    {
        EmojiFont font;
        std::vector<uint8_t> bad(f.size(), 0xFF);
        CHECK(!font.load(bad.data(), bad.size()));
        CHECK(font.load(f.data(), f.size()));
        CHECK(font.codepointGlyph(0x1F600) == 1);
    }
    // Degenerate load arguments.
    {
        EmojiFont font;
        CHECK(!font.load(nullptr, 100));
        CHECK(!font.load(f.data(), 0));
        CHECK(!font.load(nullptr, 0));
    }
}

static void testScaling() {
    CHECK(emojiScaleStrike(0, 40, 20) == 0);
    CHECK(emojiScaleStrike(20, 40, 20) == 40);
    CHECK(emojiScaleStrike(10, 40, 10) == 40);
    CHECK(emojiScaleStrike(11, 40, 10) == 44);   // (440 + 5) / 10
    CHECK(emojiScaleStrike(100, 25, 200) == 13); // (2500 + 100) / 200
    CHECK(emojiScaleStrike(-5, 40, 10) == 0);
    CHECK(emojiScaleStrike(5, 40, 0) == 0);
    CHECK(emojiScaleStrike(5, 0, 10) == 0);
}

// ---------------------------------------------------------------------------
// Layout cluster atomicity: the ShapedRun annotations keep the layout
// engine from splitting a cluster mid-sequence, and mismatched
// annotation tables make a run unshapable (the same rule as mismatched
// advance tables).
// ---------------------------------------------------------------------------

static ShapedRun shapeClustered(int n, const std::vector<int> &advances, bool clustered) {
    ShapedRun run;
    run.runIndex = 0;
    run.codepoints.resize(size_t(n));
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

// ---------------------------------------------------------------------------
// Coverage + name-table slice parsers + the bitmap-backed lookup the
// font discovery scan counts with.
// ---------------------------------------------------------------------------

static void testCoverageHelpers() {
    const std::vector<uint8_t> cbdt = fixture::buildCbdtFont(
        fixture::fakePng(8), fixture::fakePng(7), fixture::fakePng(12), fixture::fakePng(10));

    // codepointHasBitmap: a mapped codepoint whose glyph carries a
    // record, an unmapped one, and a not-even-loaded font.
    EmojiFont font;
    CHECK(font.load(cbdt.data(), cbdt.size()));
    CHECK(font.codepointHasBitmap(0x1F600)); // grin -> glyph 1, record
    CHECK(font.codepointHasBitmap(0x1F44D)); // thumbs -> glyph 2, record
    CHECK(!font.codepointHasBitmap(0x2600)); // battery member, not mapped
    CHECK(!font.codepointHasBitmap(0x0041));
    EmojiFont unloaded;
    CHECK(!unloaded.codepointHasBitmap(0x1F600));

    // sfntNameFamily: the table BLOB directly (the discovery scan
    // slices exactly this out of a file instead of reading it whole).
    const std::vector<uint8_t> name = fixture::nameTable("Slice Family");
    CHECK(sfntNameFamily(name.data(), name.size()) == "Slice Family");
    // The whole-file path agrees (sfntFamilyName finds the same table).
    CHECK(sfntFamilyName(cbdt.data(), cbdt.size()) == "Fixture CBDT");

    // The astral-plane regression: a family name with a surrogate pair
    // decodes whole (an off-by-one in the pair skip used to re-read the
    // pair's second byte as a fresh unit and corrupt everything after).
    {
        std::vector<uint8_t> table;
        auto u16 = [&table](uint16_t v) {
            table.push_back(uint8_t(v >> 8));
            table.push_back(uint8_t(v));
        };
        u16(0);      // format
        u16(1);      // count
        u16(6 + 12); // stringOffset
        u16(3);      // platform: Windows
        u16(1);      // encoding: UTF-16BE
        u16(0x409);  // language: en-US
        u16(1);      // nameID: family
        u16(16);     // length: 8 code units
        u16(0);      // strings at stringOffset + 0
        const uint16_t units[] = {'A', 0xD83D, 0xDE00, 'B', 'C', 'D', 'E', 'F'};
        for (uint16_t u : units) {
            u16(u);
        }
        CHECK(sfntNameFamily(table.data(), table.size()) == "A\xF0\x9F\x98\x80"
                                                            "BCDEF");
    }
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
    // A cluster that FITS the wrap width: no split either way.
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
    // emojiGlyphs alone (no clusterStarts) is legal too: every
    // codepoint is its own cluster, the bitmap positions ride along.
    {
        TextDocument d = oneRunDoc();
        std::vector<ShapedRun> shaped;
        shaped.push_back(shapeClustered(3, {50, 40, 0}, false));
        shaped[0].emojiGlyphs.assign(3, 0);
        shaped[0].emojiGlyphs[1] = 42;
        const TextLayout lay = layoutText(d, shaped, 600, 100);
        CHECK(lay.lineCount == 1);
        CHECK(lay.textWidth == 90);
        CHECK(lay.slices.size() == 1);
    }
}

int main() {
    testSingles();
    testKeycaps();
    testSkinTones();
    testJoinerChains();
    testFlags();
    testTags();
    testMixedStreams();
    testDefaultPresentation();
    testTruncate();
    testCount();
    testCbdtFixtureLoad();
    testCbdtBitmaps();
    testCbdtResolve();
    testSbixFixture();
    testSbixDegenerate();
    testTtcAndProbes();
    testCbdtMalformedBattery();
    testScaling();
    testLayoutClusterAtomic();
    testCoverageHelpers();

    return testExitCode("emoji");
}
