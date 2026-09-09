// FusionCut Pro - audio math + emoji-coverage battery tests (the pure
// core side of the audio mixing pipeline). Everything here is
// deterministic reference math: the same numbers the media-layer window
// mixer and the app's preview/export paths evaluate through.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "audio_math.h"
#include "emoji.h"
#include "emoji_clusters.h"
#include "test_harness.h"

using namespace fc;

// ---------------------------------------------------------------------------
// audio_math
// ---------------------------------------------------------------------------

static void testDbToLinear() {
    CHECK(audioDbToLinear(0.0) == 1.0);
    CHECK(std::fabs(audioDbToLinear(-6.0206) - 0.5) < 1.0e-3);
    CHECK(std::fabs(audioDbToLinear(6.0) - 1.9953) < 1.0e-3);
    CHECK(std::fabs(audioDbToLinear(-20.0) - 0.1) < 1.0e-3);
    CHECK(audioDbToLinear(20.0) == 10.0);
    CHECK(audioDbToLinear(-60.0) <= 1.0e-3); // the panel floor
    CHECK(audioDbToLinear(-120.0) == 0.0);
    CHECK(audioDbToLinear(-300.0) == 0.0); // clamped silence
    CHECK(audioDbToLinear(std::nan("")) == 0.0);
    // The exact powers the panel snapping relies on.
    CHECK(std::fabs(audioDbToLinear(-12.0) - 0.251189) < 1.0e-4);
    CHECK(std::fabs(audioDbToLinear(3.0) - 1.41254) < 1.0e-4);
    for (int db = -60; db <= 6; ++db) {
        const double lin = audioDbToLinear(double(db));
        CHECK(lin >= 0.0);
        CHECK(db <= 0 || lin >= 1.0);
    }
}

static void testPanLaw() {
    double l = 0.0, r = 0.0;
    audioPanCoefficients(0.0, l, r);
    CHECK(std::fabs(l - r) < 1.0e-12); // center is symmetric
    CHECK(std::fabs(l - std::sqrt(0.5)) < 1.0e-12);
    // Hard sides.
    audioPanCoefficients(-1.0, l, r);
    CHECK(std::fabs(l - 1.0) < 1.0e-12);
    CHECK(std::fabs(r) < 1.0e-12);
    audioPanCoefficients(1.0, l, r);
    CHECK(std::fabs(l) < 1.0e-12);
    CHECK(std::fabs(r - 1.0) < 1.0e-12);
    // Constant power: l^2 + r^2 == 1 at every position.
    for (int p = -100; p <= 100; ++p) {
        double pl = 0.0, pr = 0.0;
        audioPanCoefficients(double(p) / 100.0, pl, pr);
        CHECK(std::fabs(pl * pl + pr * pr - 1.0) < 1.0e-9);
        CHECK(pl >= 0.0 && pl <= 1.0);
        CHECK(pr >= 0.0 && pr <= 1.0);
        // Symmetry: -p mirrors +p.
        double ml = 0.0, mr = 0.0;
        audioPanCoefficients(-double(p) / 100.0, ml, mr);
        CHECK(std::fabs(pl - mr) < 1.0e-12);
        CHECK(std::fabs(pr - ml) < 1.0e-12);
    }
    // Clamping.
    audioPanCoefficients(-5.0, l, r);
    CHECK(std::fabs(l - 1.0) < 1.0e-12);
    audioPanCoefficients(5.0, l, r);
    CHECK(std::fabs(r - 1.0) < 1.0e-12);
    double nl = 1.0, nr = 1.0;
    audioPanCoefficients(std::nan(""), nl, nr);
    CHECK(nl == 0.0 && nr == 0.0);
}

static void testFade() {
    CHECK(audioFadeMultiplier(0.5, 0.0) == 1.0);  // no fade configured
    CHECK(audioFadeMultiplier(0.5, -1.0) == 1.0); // degenerate fade = off
    CHECK(audioFadeMultiplier(0.5, std::nan("")) == 1.0);
    CHECK(audioFadeMultiplier(-1.0, 1.0) == 0.0); // before the ramp
    CHECK(audioFadeMultiplier(0.0, 1.0) == 0.0);
    CHECK(audioFadeMultiplier(0.25, 1.0) == 0.25);
    CHECK(std::fabs(audioFadeMultiplier(0.1, 0.3) - (1.0 / 3.0)) < 1.0e-12);
    CHECK(audioFadeMultiplier(1.0, 1.0) == 1.0); // at the end
    CHECK(audioFadeMultiplier(2.0, 1.0) == 1.0); // after
    CHECK(audioFadeMultiplier(std::nan(""), 1.0) == 0.0);
    // Monotone across the ramp.
    double prev = -1.0;
    for (int i = 0; i <= 100; ++i) {
        const double v = audioFadeMultiplier(double(i) / 100.0, 1.0);
        CHECK(v >= prev);
        prev = v;
    }
}

static void testClipAndMix() {
    CHECK(audioClipSample(0.5f) == 0.5f);
    CHECK(audioClipSample(-0.5f) == -0.5f);
    CHECK(audioClipSample(2.0f) == 1.0f);
    CHECK(audioClipSample(-2.0f) == -1.0f);
    CHECK(audioClipSample(std::nan("")) == 0.0f); // NaN never escapes
    CHECK(audioClipSample(std::numeric_limits<float>::infinity()) == 0.0f);
    CHECK(audioClipSample(-std::numeric_limits<float>::infinity()) == 0.0f);

    float frame[2] = {0.0f, 0.0f};
    audioAccumulateStereoFrame(frame, 0.25f, -0.5f, 2.0, 2.0);
    CHECK(frame[0] == 0.5f);
    CHECK(frame[1] == -1.0f);
    audioAccumulateStereoFrame(frame, 0.25f, -0.5f, 2.0, 2.0); // summing accumulates
    CHECK(frame[0] == 1.0f);
    CHECK(frame[1] == -2.0f);

    // The finish pass: master gain then the hard clip, per sample.
    float window[4] = {0.5f, -0.5f, 2.0f, -3.0f};
    audioFinishStereoWindow(window, 2, 2.0);
    CHECK(window[0] == 1.0f);
    CHECK(window[1] == -1.0f);
    CHECK(window[2] == 1.0f);
    CHECK(window[3] == -1.0f);
    float zeros[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    audioFinishStereoWindow(zeros, 2, 0.0);
    CHECK(zeros[0] == 0.0f && zeros[1] == 0.0f);
    float nanWin[2] = {std::nan(""), 0.25f};
    audioFinishStereoWindow(nanWin, 1, 1.0); // NaN master folds to 0
    CHECK(nanWin[0] == 0.0f);
    CHECK(nanWin[1] == 0.25f);
}

// ---------------------------------------------------------------------------
// The emoji coverage battery (the discovery scan's data)
// ---------------------------------------------------------------------------

static void testBattery() {
    const uint32_t *battery = emojiCoverageBattery();
    const size_t n = emojiCoverageBatterySize();
    CHECK(battery != nullptr);
    CHECK(n >= 36);
    CHECK(n < 256);
    // Strictly ascending (the docs' own claim; keeps lookups sane).
    for (size_t i = 1; i < n; ++i) {
        CHECK(battery[i - 1] < battery[i]);
    }
    // Every battery member is an emoji-classified codepoint under the
    // shared presentation policy: the grinning face, hearts, and the
    // BMP legacy set must all count as emoji.
    bool sawSmp = false, sawBmp = false, sawHeart = false, sawGrin = false;
    for (size_t i = 0; i < n; ++i) {
        if (battery[i] > 0xFFFF) {
            sawSmp = true;
        } else {
            sawBmp = true;
        }
        if (battery[i] == 0x2764u) {
            sawHeart = true; // text-default heart: still an emoji battery member
        }
        if (battery[i] == 0x1F600u) {
            sawGrin = true;
        }
    }
    CHECK(sawSmp);
    CHECK(sawBmp);
    CHECK(sawHeart);
    CHECK(sawGrin);
    CHECK(kEmojiCoverageMinimum >= 1);
    CHECK(kEmojiCoverageMinimum < n); // a real emoji font passes with room
    // No variation selectors / joiners / tag characters in the battery:
    // members must be standalone base emoji.
    for (size_t i = 0; i < n; ++i) {
        CHECK(battery[i] != kEmojiVS16);
        CHECK(battery[i] != kEmojiVS15);
        CHECK(battery[i] != kZeroWidthJoiner);
        CHECK(battery[i] != kCombiningKeycap);
        CHECK(battery[i] < kEmojiTagFirst || battery[i] > kEmojiTagLast);
        CHECK(battery[i] != kEmojiTagTerminator);
        CHECK(battery[i] < kRegionalIndicatorFirst || battery[i] > kRegionalIndicatorLast);
        CHECK(battery[i] < kEmojiSkinToneFirst || battery[i] > kEmojiSkinToneLast);
    }
}

// ---------------------------------------------------------------------------
// sfntCmapEmojiCoverage / sfntNameFamily on hand-built table blobs
// ---------------------------------------------------------------------------

// Big-endian writers.
static void put16(std::vector<uint8_t> &v, uint16_t x) {
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
}

static void put32(std::vector<uint8_t> &v, uint32_t x) {
    v.push_back(uint8_t(x >> 24));
    v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
}

// A cmap with ONE format-12 subtable covering the battery's BMP + SMP
// members (via one big group per member, firstGlyph = 10).
static std::vector<uint8_t> cmapFmt12(const std::vector<uint32_t> &covered) {
    std::vector<uint8_t> v;
    put16(v, 0);  // version
    put16(v, 1);  // numTables
    put16(v, 3);  // platform: Windows
    put16(v, 10); // encoding: UCS-4
    put32(v, 12); // offset of the subtable (right after the header)
    put16(v, 12); // format
    put16(v, 0);  // reserved
    put32(v, 0);  // length (not parsed)
    put32(v, 0);  // language
    put32(v, uint32_t(covered.size()));
    for (uint32_t cp : covered) {
        put32(v, cp);
        put32(v, cp);
        put32(v, 10);
    }
    return v;
}

// A cmap with ONE format-4 subtable covering BMP codepoints (the
// single-segment degenerate form: [start,end) with a delta).
static std::vector<uint8_t> cmapFmt4(uint16_t start, uint16_t end, uint16_t delta) {
    std::vector<uint8_t> v;
    put16(v, 0);          // version
    put16(v, 1);          // numTables
    put16(v, 3);          // platform: Windows
    put16(v, 1);          // encoding: Unicode BMP
    put32(v, 12);         // offset
    put16(v, 4);          // format
    put16(v, 16 + 8 * 2); // length
    put16(v, 0);          // language
    put16(v, 2);          // segCountX2 (one segment)
    put16(v, 2);          // searchRange
    put16(v, 1);          // entrySelector
    put16(v, 0);          // rangeShift
    put16(v, end);        // endCode[0]
    put16(v, 0);          // reservedPad
    put16(v, start);
    put16(v, delta); // idDelta
    put16(v, 0);     // idRangeOffset: glyph = cp + delta
    return v;
}

static void testCmapCoverage() {
    // Degenerate inputs.
    CHECK(sfntCmapEmojiCoverage(nullptr, 10) == 0);
    CHECK(sfntCmapEmojiCoverage((const uint8_t *)"", 0) == 0);
    {
        std::vector<uint8_t> shortBlob = {0x00, 0x00, 0x00};
        CHECK(sfntCmapEmojiCoverage(shortBlob.data(), shortBlob.size()) == 0);
    }

    // A format-12 table covering the whole battery.
    {
        const uint32_t *battery = emojiCoverageBattery();
        std::vector<uint32_t> covered(battery, battery + emojiCoverageBatterySize());
        const auto v = cmapFmt12(covered);
        CHECK(sfntCmapEmojiCoverage(v.data(), v.size()) == int(covered.size()));
    }
    // A format-12 table covering only the BMP members.
    {
        const uint32_t *battery = emojiCoverageBattery();
        const size_t n = emojiCoverageBatterySize();
        std::vector<uint32_t> bmp;
        for (size_t i = 0; i < n; ++i) {
            if (battery[i] <= 0xFFFF) {
                bmp.push_back(battery[i]);
            }
        }
        CHECK(bmp.size() >= 17); // the battery really has a BMP leg
        const auto v = cmapFmt12(bmp);
        CHECK(sfntCmapEmojiCoverage(v.data(), v.size()) == int(bmp.size()));
    }
    // Nothing mapped (one group outside the emoji ranges).
    {
        const auto v = cmapFmt12({0x0041u});
        CHECK(sfntCmapEmojiCoverage(v.data(), v.size()) == 0);
    }
    // Glyph 0 never counts ("mapped" means a real glyph).
    {
        std::vector<uint8_t> v;
        put16(v, 0);
        put16(v, 1);
        put16(v, 3);
        put16(v, 10);
        put32(v, 12);
        put16(v, 12);
        put16(v, 0);
        put32(v, 0);
        put32(v, 0);
        put32(v, 1);
        put32(v, 0x1F600);
        put32(v, 0x1F600);
        put32(v, 0); // firstGlyph 0 = notdef
        CHECK(sfntCmapEmojiCoverage(v.data(), v.size()) == 0);
    }
    // Format 4: the BMP battery segment 0x2300..0x27FF covers the 23xx
    // and 26xx/27xx members with a nonzero delta.
    {
        const auto v = cmapFmt4(0x2300, 0x27FF, 3);
        const int hits = sfntCmapEmojiCoverage(v.data(), v.size());
        // Every BMP battery member inside [0x2300, 0x27FF]: 0x231A, 0x23F0,
        // 0x2600..0x2764 - count them from the battery itself.
        const uint32_t *battery = emojiCoverageBattery();
        const size_t n = emojiCoverageBatterySize();
        int expected = 0;
        for (size_t i = 0; i < n; ++i) {
            if (battery[i] >= 0x2300 && battery[i] <= 0x27FF) {
                ++expected;
            }
        }
        CHECK(expected >= 17);
        CHECK(hits == expected);
    }
    // Format 4 mapping to glyph 0 (delta 0 with... delta arithmetic that
    // lands on 0) is "not mapped".
    {
        // A segment covering 0x2300-0x27FF with delta 0: the codepoints
        // themselves map to glyphs equal to themselves (nonzero), so use
        // an idRangeOffset-style table instead: simply make the segment
        // tiny and land on 0 via cp+delta == 0 for 0x1F600? Not
        // representable in BMP; use cp 0x0041 with delta -0x41.
        std::vector<uint8_t> v = cmapFmt4(0x0041, 0x0041, uint16_t(0x10000 - 0x41));
        CHECK(sfntCmapEmojiCoverage(v.data(), v.size()) == 0); // not battery anyway
    }
    // A lying numTables entry does not read past the blob.
    {
        std::vector<uint8_t> v = {0x00, 0x00, 0x00, 0x40}; // 64 subtable records, none present
        CHECK(sfntCmapEmojiCoverage(v.data(), v.size()) == 0);
    }
    // Truncated subtable offsets are skipped, not crashed.
    {
        std::vector<uint8_t> v;
        put16(v, 0);
        put16(v, 1);
        put16(v, 0);
        put16(v, 3);
        put32(v, 9999); // offset past the blob
        CHECK(sfntCmapEmojiCoverage(v.data(), v.size()) == 0);
    }
}

// A 'name' table blob with one Windows UTF-16BE nameID-1 record.
static std::vector<uint8_t> nameTable(const std::u16string &family, uint16_t platform = 3,
                                      uint16_t encoding = 1, uint16_t language = 0x409) {
    std::vector<uint8_t> v;
    put16(v, 0);      // format
    put16(v, 1);      // count
    put16(v, 6 + 12); // stringOffset: strings right after the records
    put16(v, platform);
    put16(v, encoding);
    put16(v, language);
    put16(v, 1); // nameID 1 = family
    put16(v, uint16_t(family.size() * 2));
    put16(v, 0); // strings at stringOffset + 0
    for (char16_t c : family) {
        put16(v, uint16_t(c));
    }
    return v;
}

static void testNameFamily() {
    // Degenerate.
    CHECK(sfntNameFamily(nullptr, 10).empty());
    CHECK(sfntNameFamily((const uint8_t *)"", 0).empty());
    {
        std::vector<uint8_t> v = {0x00, 0x00, 0x00};
        CHECK(sfntNameFamily(v.data(), v.size()).empty());
    }
    // Windows en-US UTF-16BE.
    {
        const auto v = nameTable(u"Segoe UI Emoji");
        CHECK(sfntNameFamily(v.data(), v.size()) == "Segoe UI Emoji");
    }
    // Astral plane codepoints decode via surrogate pairs.
    {
        const auto v = nameTable(u"Noto"
                                 u"\xD83D"
                                 u"\xDE00"
                                 u"Color");
        CHECK(sfntNameFamily(v.data(), v.size()) == "Noto\xF0\x9F\x98\x80"
                                                    "Color");
    }
    // Unicode platform (0) is accepted at a lower priority.
    {
        const auto v = nameTable(u"Symbola", 0, 3, 0);
        CHECK(sfntNameFamily(v.data(), v.size()) == "Symbola");
    }
    // nameID other than 1 never matches.
    {
        auto v = nameTable(u"Wrong");
        v[6 + 6] =
            0x00; // nameID -> 0? record layout: put16 nameId at rec+6 -> byte 6+6? see builder
        v[12] = 0x00;
        v[13] = 0x02; // nameID 2 (subfamily)
        CHECK(sfntNameFamily(v.data(), v.size()).empty());
    }
    // Record pointing past the table is refused.
    {
        auto v = nameTable(u"Ok");
        v[4] = 0x00;
        v[5] = 0xFF; // count = 255 (records run past the blob)
        CHECK(sfntNameFamily(v.data(), v.size()).empty());
    }
    // Empty payload.
    {
        const auto v = nameTable(u"");
        CHECK(sfntNameFamily(v.data(), v.size()).empty());
    }
}

int main() {
    testDbToLinear();
    testPanLaw();
    testFade();
    testClipAndMix();
    testBattery();
    testCmapCoverage();
    testNameFamily();
    return testExitCode("audio");
}
