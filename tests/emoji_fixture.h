#pragma once

// Synthetic emoji fonts, hand-built byte by byte: the exact tables the
// EmojiFont parsers read (CBLC/CBDT, sbix, cmap 12/14, GSUB, maxp,
// head, hhea, hmtx, name) with hand-derivable expectations. Pure C++,
// no Qt - the PNG payloads are CALLER-PROVIDED bytes: the unit suite
// passes the PNG magic + padding (the core only checks the magic), the
// sandbox runtime smoke passes a real decodable PNG so the app-side
// decode/scale/paint path gets exercised too.
//
// The CBDT face "Fixture CBDT" (8 glyphs):
//   glyph 1 = U+1F600 grin        (record: 10x10, bearingX 1, bearingY 9,
//                                  advance 10, PNG)
//   glyph 2 = U+1F44D thumbs-up   (record: 9x9, bearingY 8, advance 9)
//   glyph 3 = U+1F1FA regional-U  (mapped, NO record)
//   glyph 4 = U+1F1F8 regional-S  (mapped, NO record)
//   glyph 5 = US-flag ligature    (GSUB [3,4] -> 5; record: 12x8,
//                                  bearingY 8, advance 12)
//   glyph 6 = U+2764 heart base   (mapped, NO record; the fmt14 variant
//                                  (U+2764, FE0F) -> 7)
//   glyph 7 = heart + VS16        (record: 6x6, bearingY 6, advance 6)
//   glyph 8 = 'A'                 (mapped, plain text, no bitmap)
//   CBLC strike: ppem 10, bitDepth 32.
//
// The sbix face "Fixture SBIX" (6 glyphs, ppem 20, upem 1000):
//   glyph 1 = U+1F600  (record 'png ', originX 1, originY -2, hmtx 1000)
//   glyph 2 = U+1F601  (record 'dupe' -> 1)
//   glyph 3 = U+1F602  (record 'flip' + 'dupe' -> 1: mirrored bitmap)
//   glyph 4 = U+1F603  (zero-length record: missing)
//   glyph 5 = U+1F604  (record 'tiff': undecodable tag -> missing)
//   hmtx advance 1000 units -> 1000 * 20 / 1000 = 20 strike pixels.
//
// The collection wraps a 12-byte junk face + the CBDT face: load()
// must skip the junk and keep the emoji sub-font.

#include <cstdint>
#include <cstring>
#include <vector>

namespace fc {
namespace fixture {

// ---- byte builder ----

struct Bytes {
    std::vector<uint8_t> b;

    void u8(uint8_t v) { b.push_back(v); }
    void s8(int8_t v) { b.push_back(uint8_t(v)); }
    void u16(uint16_t v) {
        b.push_back(uint8_t(v >> 8));
        b.push_back(uint8_t(v & 0xFF));
    }
    void s16(int16_t v) { u16(uint16_t(v)); }
    void u32(uint32_t v) {
        u16(uint16_t(v >> 16));
        u16(uint16_t(v));
    }
    void tag(const char t[4]) { b.insert(b.end(), t, t + 4); }
    void raw(const uint8_t *p, size_t n) { b.insert(b.end(), p, p + n); }
};

struct TableRec {
    const char *tag = "";
    std::vector<uint8_t> payload;
};

// Assembles an sfnt file: 12-byte header + directory + 4-byte-aligned
// payloads. Directory lengths are the UNPADDED payload sizes.
inline std::vector<uint8_t> assembleSfnt(const std::vector<TableRec> &tables) {
    const uint16_t numTables = uint16_t(tables.size());
    size_t off = 12 + size_t(numTables) * 16;
    std::vector<uint8_t> out;
    out.assign(off, 0);
    auto put16 = [&out](size_t at, uint16_t v) {
        out[at] = uint8_t(v >> 8);
        out[at + 1] = uint8_t(v);
    };
    auto put32 = [&out, &put16](size_t at, uint32_t v) {
        put16(at, uint16_t(v >> 16));
        put16(at + 2, uint16_t(v));
    };
    put32(0, 0x00010000u); // sfnt version: TrueType
    put16(4, numTables);
    put16(6, uint16_t(16 * numTables)); // searchRange (unused by parsers)
    put16(8, 0);
    put16(10, 0);
    for (size_t i = 0; i < tables.size(); ++i) {
        const size_t rec = 12 + i * 16;
        std::memcpy(out.data() + rec, tables[i].tag, 4);
        put32(rec + 4, 0); // checksum: parsers never read it
        put32(rec + 8, uint32_t(off));
        put32(rec + 12, uint32_t(tables[i].payload.size()));
        out.insert(out.end(), tables[i].payload.begin(), tables[i].payload.end());
        while (out.size() % 4 != 0) {
            out.push_back(0);
        }
        off = out.size();
    }
    return out;
}

// The PNG payloads: the 8-byte magic followed by `padLen` filler
// bytes (the core checks only the magic; a real decode happens in the
// sandbox smoke, which passes an actual PNG).
inline std::vector<uint8_t> fakePng(size_t padLen) {
    const uint8_t magic[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> out(magic, magic + 8);
    out.insert(out.end(), padLen, 0x5Au);
    return out;
}

// ---- shared table builders ----

inline std::vector<uint8_t> maxpTable(uint16_t numGlyphs) {
    Bytes b;
    b.u32(0x00010000u);
    b.u16(numGlyphs);
    return b.b;
}

inline std::vector<uint8_t> headTable(uint16_t unitsPerEm) {
    Bytes b;
    b.u32(0x00010000u); // version
    b.u32(0x00010000u); // fontRevision
    b.u32(0);           // checkSumAdjustment
    b.u32(0x5F0F3CF5u); // magicNumber
    b.u16(0);           // flags
    b.u16(unitsPerEm);  // offset 18
    b.u32(0);
    b.u32(0); // created / modified
    b.s16(0);
    b.s16(0);
    b.s16(0);
    b.s16(0); // bbox
    b.u16(0); // macStyle
    b.u16(8); // lowestRecPpmReadable
    b.s16(2); // fontDirectionHint
    b.s16(0); // indexToLocFormat
    b.s16(0); // glyphDataFormat
    return b.b;
}

inline std::vector<uint8_t> hheaTable(uint16_t numMetrics) {
    // The real 36-byte layout: numberOfHMetrics sits at offset 34.
    Bytes b;
    b.u32(0x00010000u); // version
    b.s16(800);         // ascender
    b.s16(-200);        // descender
    b.s16(0);           // lineGap
    b.u16(1000);        // advanceWidthMax
    b.s16(0);           // minLeftSideBearing
    b.s16(0);           // minRightSideBearing
    b.s16(1000);        // xMaxExtent
    b.s16(1);           // caretSlopeRise
    b.s16(0);           // caretSlopeRun
    b.s16(0);           // caretOffset
    b.s16(0);           // reserved 1
    b.s16(0);           // reserved 2
    b.s16(0);           // reserved 3
    b.s16(0);           // reserved 4
    b.s16(0);           // metricDataFormat
    b.u16(numMetrics);  // numberOfHMetrics @34
    return b.b;
}

inline std::vector<uint8_t> hmtxTable(const std::vector<uint16_t> &advances) {
    Bytes b;
    for (uint16_t a : advances) {
        b.u16(a); // advanceWidth
        b.s16(0); // lsb
    }
    return b.b;
}

// cmap format 12 groups {start, end, firstGlyph} + an optional format
// 14 non-default UVS mapping (base, FE0F) -> variantGlyph.
inline std::vector<uint8_t> cmapTable(const std::vector<std::vector<uint32_t>> &groups,
                                      const std::vector<std::vector<uint32_t>> &uvs) {
    Bytes sub12;
    sub12.u16(12); // format
    sub12.u16(0);  // reserved
    sub12.u32(0);  // length (parsers do not read it)
    sub12.u32(0);  // language
    sub12.u32(uint32_t(groups.size()));
    for (const auto &g : groups) {
        sub12.u32(g[0]);
        sub12.u32(g[1]);
        sub12.u32(g[2]);
    }
    Bytes sub14;
    if (!uvs.empty()) {
        sub14.u16(14);
        sub14.u32(0); // length
        sub14.u32(uint32_t(uvs.size()));
        // non-default UVS data block goes first (offset computed from
        // the fmt14 subtable start; both offsets are relative to it)
        const size_t ndOffset = 2 + 4 + 4 + 11 * uvs.size();
        Bytes nd;
        nd.u32(uint32_t(uvs.size()));
        for (const auto &m : uvs) {
            nd.u8(uint8_t(m[0] >> 16));
            nd.u8(uint8_t(m[0] >> 8));
            nd.u8(uint8_t(m[0]));
            nd.u16(uint16_t(m[1]));
        }
        for (size_t i = 0; i < uvs.size(); ++i) {
            sub14.u8(0x00); // varSelector u24 = FE0F
            sub14.u8(0xFE);
            sub14.u8(0x0F);
            sub14.u32(0);                  // defaultUVS: absent
            sub14.u32(uint32_t(ndOffset)); // nonDefaultUVS
        }
        sub14.raw(nd.b.data(), nd.b.size());
    }
    Bytes b;
    b.u16(0); // cmap version
    b.u16(uvs.empty() ? 1 : 2);
    b.u16(3);
    b.u16(10);
    b.u32(4 + (uvs.empty() ? 8 : 16)); // fmt12 offset
    if (!uvs.empty()) {
        b.u16(0);
        b.u16(5);
        b.u32(4 + 16 + uint32_t(sub12.b.size())); // fmt14 offset
    }
    b.raw(sub12.b.data(), sub12.b.size());
    if (!uvs.empty()) {
        b.raw(sub14.b.data(), sub14.b.size());
    }
    return b.b;
}

// A name table with ONE Windows en-US family (nameID 1) record.
inline std::vector<uint8_t> nameTable(const char *familyAscii) {
    std::vector<uint16_t> utf16;
    for (const char *p = familyAscii; *p != 0; ++p) {
        utf16.push_back(uint16_t(uint8_t(*p)));
    }
    Bytes strings;
    for (uint16_t u : utf16) {
        strings.u16(u);
    }
    Bytes b;
    b.u16(0);                          // format
    b.u16(1);                          // count
    b.u16(6 + 12);                     // stringOffset (from name start)
    b.u16(3);                          // platform: Windows
    b.u16(1);                          // encoding: Unicode BMP
    b.u16(0x409);                      // language: en-US
    b.u16(1);                          // nameID: family
    b.u16(uint16_t(strings.b.size())); // length
    b.u16(0);                          // offset (from stringOffset)
    b.raw(strings.b.data(), strings.b.size());
    return b.b;
}

// GSUB with exactly ONE type-4 ligature rule: sequence -> ligature.
inline std::vector<uint8_t> gsubTable(const std::vector<uint16_t> &sequence,
                                      uint16_t ligatureGlyph) {
    Bytes scriptList;
    scriptList.u16(0); // no scripts
    Bytes featureList;
    featureList.u16(0); // no features
    // lookupList { count, offsets[1] } then the lookup itself.
    const size_t lookupListSize = 2 + 2;                        // count + one offset
    const size_t lookupOff = lookupListSize;                    // from lookupList
    const size_t lookupSize = 2 + 2 + 2 + 2;                    // type/flag/subCount/off
    const size_t subtableOff = lookupOff + lookupSize;          // from lookupList
    const size_t covSize = 2 + 2 + 2;                           // format/count/one glyph
    const size_t subtableSize = 2 + 2 + 2 + 2;                  // fmt/covOff/setCount/setOff
    const size_t setOff = subtableOff + subtableSize + covSize; // from lookupList
    const size_t setSize = 2 + 2;                               // count + one offset
    const size_t ligOff = setOff + setSize;                     // from lookupList

    Bytes b;
    b.u32(0x00010000u); // version
    b.u16(10);          // scriptList offset (right after the header)
    b.u16(10 + 2);      // featureList offset
    b.u16(10 + 2 + 2);  // lookupList offset
    b.raw(scriptList.b.data(), scriptList.b.size());
    b.raw(featureList.b.data(), featureList.b.size());
    // lookupList
    b.u16(1);
    b.u16(uint16_t(lookupOff));
    // lookup: type 4, flag 0, 1 subtable
    b.u16(4);
    b.u16(0);
    b.u16(1);
    b.u16(uint16_t(subtableOff - lookupOff));
    // ligature subtable: format 1, coverage offset (from subtable),
    // 1 ligature set, set offset (from subtable)
    b.u16(1);
    b.u16(uint16_t(subtableSize)); // coverage right after the subtable
    b.u16(1);
    b.u16(uint16_t(setOff - subtableOff));
    // coverage: format 1, count 1, first component of the sequence
    b.u16(1);
    b.u16(1);
    b.u16(sequence[0]);
    // ligature set: 1 ligature
    b.u16(1);
    b.u16(uint16_t(ligOff - setOff));
    // ligature: glyph, componentCount, components
    b.u16(ligatureGlyph);
    b.u16(uint16_t(sequence.size()));
    for (size_t i = 1; i < sequence.size(); ++i) {
        b.u16(sequence[i]);
    }
    return b.b;
}

// ---- the CBDT face ----

inline std::vector<uint8_t> buildCbdtFont(const std::vector<uint8_t> &pngGrin,
                                          const std::vector<uint8_t> &pngThumbs,
                                          const std::vector<uint8_t> &pngFlag,
                                          const std::vector<uint8_t> &pngHeart) {
    // CBDT records for glyphs 1, 2, 5, 7 (format 17), in glyph order.
    auto fmt17 = [](uint8_t h, uint8_t w, int8_t bx, int8_t by, uint8_t adv,
                    const std::vector<uint8_t> &png) {
        Bytes r;
        r.u8(h);
        r.u8(w);
        r.s8(bx);
        r.s8(by);
        r.u8(adv);
        r.u32(uint32_t(png.size()));
        r.raw(png.data(), png.size());
        return r;
    };
    const Bytes r1 = fmt17(10, 10, 1, 9, 10, pngGrin);
    const Bytes r2 = fmt17(9, 9, 0, 8, 9, pngThumbs);
    const Bytes r5 = fmt17(8, 12, 0, 8, 12, pngFlag);
    const Bytes r7 = fmt17(6, 6, 0, 6, 6, pngHeart);
    Bytes cbdt;
    cbdt.raw(r1.b.data(), r1.b.size());
    cbdt.raw(r2.b.data(), r2.b.size());
    cbdt.raw(r5.b.data(), r5.b.size());
    cbdt.raw(r7.b.data(), r7.b.size());

    // CBLC: version 2.0, one 48-byte size record (ppem 10, depth 32),
    // an 8-byte indexSubTableArray entry {first=1, last=7, offset=8},
    // then the format-1 subtable. offsetArray is indexed by glyph-1
    // (7 entries + the sentinel; 0 = explicit missing except entry 0,
    // which means "the first glyph sits AT sbitOffset" = 0 here).
    const uint32_t len1 = uint32_t(r1.b.size());
    const uint32_t len2 = uint32_t(r2.b.size());
    const uint32_t len5 = uint32_t(r5.b.size());
    const uint32_t len7 = uint32_t(r7.b.size());
    const uint32_t arrayOffset = 8 + 48; // right after the size record
    Bytes cblc;
    cblc.u32(0x00020000u); // version 2.0
    cblc.u32(1);           // numSizes
    // BitmapSizeTable (48 bytes)
    cblc.u32(arrayOffset); // indexSubTableArrayOffset (from CBLC start)
    cblc.u32(8 + 12);      // indexTablesSize (informational)
    cblc.u32(1);           // numberIndexSubTables
    cblc.u32(0);           // colorRef
    for (int i = 0; i < 14; ++i) {
        cblc.s16(0); // hori + vert sbitLineMetrics (28 bytes)
    }
    cblc.u8(10); // ppemX @44
    cblc.u8(10); // ppemY @45
    cblc.u8(32); // bitDepth @46
    cblc.u8(0);  // flags @47
    // IndexSubTableArray entry: {first, last, offset-from-array-start}
    cblc.u16(1);
    cblc.u16(7);
    cblc.u32(8);
    // IndexSubTable format 1 (at array start + 8)
    cblc.u16(1);                         // indexFormat
    cblc.u16(17);                        // imageFormat
    cblc.u32(0);                         // sbitOffset: CBDT data starts at CBDT+0
    cblc.u32(0);                         // glyph 1: entry 0 = AT sbitOffset
    cblc.u32(len1);                      // glyph 2
    cblc.u32(0);                         // glyph 3: missing
    cblc.u32(0);                         // glyph 4: missing
    cblc.u32(len1 + len2);               // glyph 5
    cblc.u32(0);                         // glyph 6: missing
    cblc.u32(len1 + len2 + len5);        // glyph 7
    cblc.u32(len1 + len2 + len5 + len7); // sentinel (one past end)

    std::vector<TableRec> tables;
    TableRec maxp;
    maxp.tag = "maxp";
    maxp.payload = maxpTable(8);
    TableRec cmap;
    cmap.tag = "cmap";
    cmap.payload = cmapTable(
        {
            {0x00000041u, 0x00000041u, 8}, // 'A' -> glyph 8
            {0x00002764u, 0x00002764u, 6}, // heart base -> glyph 6
            {0x1F1F8u, 0x1F1F8u, 4},       // regional-S
            {0x1F1FAu, 0x1F1FAu, 3},       // regional-U
            {0x1F44Du, 0x1F44Du, 2},       // thumbs-up
            {0x1F600u, 0x1F600u, 1},       // grin
        },
        {{0x00002764u, 7u}}); // (heart, FE0F) -> glyph 7
    TableRec gsub;
    gsub.tag = "GSUB";
    gsub.payload = gsubTable({3, 4}, 5); // [regional-U, regional-S] -> flag
    TableRec cblcRec;
    cblcRec.tag = "CBLC";
    cblcRec.payload = cblc.b;
    TableRec cbdtRec;
    cbdtRec.tag = "CBDT";
    cbdtRec.payload = cbdt.b;
    TableRec name;
    name.tag = "name";
    name.payload = nameTable("Fixture CBDT");
    tables = {maxp, cmap, gsub, cblcRec, cbdtRec, name};
    return assembleSfnt(tables);
}

// ---- the sbix face ----

inline std::vector<uint8_t> buildSbixFont(const std::vector<uint8_t> &pngGrin) {
    // Strike: ppem 20, one 'png ' record (glyph 1), a 'dupe' (glyph 2),
    // a 'flip'+'dupe' (glyph 3), zero-length (glyph 4), 'tiff' (glyph 5).
    const size_t numGlyphs = 6;
    const size_t strikeRel = 8 + 4;          // header + one strike offset
    const size_t arrayStart = strikeRel + 4; // after ppem/ppi
    const size_t recordsStart = arrayStart + (numGlyphs + 1) * 4;
    Bytes sbix;
    sbix.u16(1);                   // version
    sbix.u16(1);                   // flags (bit 0)
    sbix.u32(1);                   // numStrikes
    sbix.u32(uint32_t(strikeRel)); // strike offset (from sbix start)
    // Strike header + offset array. The glyph offsets are STRIKE-RELATIVE
    // ("from the beginning of the strike data header" - the spec and
    // FreeType agree), so subtract the strike base.
    sbix.u16(20); // ppem
    sbix.u16(72); // ppi
    const uint32_t r1 = uint32_t(recordsStart - strikeRel);
    const uint32_t r1End = r1 + uint32_t(8 + pngGrin.size());
    const uint32_t r2 = r1End; // 'dupe' record: 10 bytes
    const uint32_t r2End = r2 + 10;
    const uint32_t r3 = r2End; // 'flip'+'dupe': 10 bytes
    const uint32_t r3End = r3 + 10;
    const uint32_t r5 = r3End; // 'tiff': 12 bytes
    const uint32_t r5End = r5 + 12;
    // The offset array: numGlyphs + 1 entries (glyph starts 0..5 plus
    // the one-past-end sentinel). Glyph 4 is zero-length, so its start
    // equals glyph 5's.
    sbix.u32(0);     // glyph 0: no record
    sbix.u32(r1);    // glyph 1 start ('png ')
    sbix.u32(r1End); // glyph 2 start ('dupe')
    sbix.u32(r2End); // glyph 3 start ('flip')
    sbix.u32(r3End); // glyph 4 start (zero-length)
    sbix.u32(r3End); // glyph 5 start ('tiff')
    sbix.u32(r5End); // sentinel
    // glyph 1 record: 'png ' (a 4cc tag WITH the trailing space)
    sbix.s16(1);  // originOffsetX
    sbix.s16(-2); // originOffsetY
    sbix.u8('p');
    sbix.u8('n');
    sbix.u8('g');
    sbix.u8(' ');
    sbix.raw(pngGrin.data(), pngGrin.size());
    // glyph 2 record: 'dupe' -> 1
    sbix.s16(0);
    sbix.s16(0);
    sbix.tag("dupe");
    sbix.u16(1);
    // glyph 3 record: 'flip' then 'dupe' -> 1
    sbix.s16(0);
    sbix.s16(0);
    sbix.tag("flip");
    sbix.u16(1);
    // glyph 5 record: 'tiff' junk
    sbix.s16(0);
    sbix.s16(0);
    sbix.u8('t');
    sbix.u8('i');
    sbix.u8('f');
    sbix.u8('f');
    sbix.u32(0x11223344u);

    std::vector<TableRec> tables;
    TableRec maxp;
    maxp.tag = "maxp";
    maxp.payload = maxpTable(6);
    TableRec head;
    head.tag = "head";
    head.payload = headTable(1000);
    TableRec hhea;
    hhea.tag = "hhea";
    hhea.payload = hheaTable(6);
    TableRec hmtx;
    hmtx.tag = "hmtx";
    hmtx.payload = hmtxTable({0, 1000, 1000, 1000, 1000, 900});
    TableRec cmap;
    cmap.tag = "cmap";
    cmap.payload = cmapTable(
        {
            {0x1F600u, 0x1F600u, 1},
            {0x1F601u, 0x1F601u, 2},
            {0x1F602u, 0x1F602u, 3},
            {0x1F603u, 0x1F603u, 4},
            {0x1F604u, 0x1F604u, 5},
        },
        {});
    TableRec sbixRec;
    sbixRec.tag = "sbix";
    sbixRec.payload = sbix.b;
    TableRec name;
    name.tag = "name";
    name.payload = nameTable("Fixture SBIX");
    tables = {maxp, head, hhea, hmtx, cmap, sbixRec, name};
    return assembleSfnt(tables);
}

// An OUTLINE emoji face: a plain sfnt with a cmap covering the given
// codepoints and a name table, NO bitmap tables (the discovery scan's
// outline class - Segoe UI Symbol / Symbola / Noto Emoji shapes). The
// glyphs themselves are never rendered through this engine; only the
// cmap coverage and the family name matter.
inline std::vector<uint8_t> buildOutlineFont(const std::vector<uint32_t> &cps, const char *family) {
    std::vector<TableRec> tables;
    TableRec maxp;
    maxp.tag = "maxp";
    maxp.payload = maxpTable(uint16_t(cps.size() + 1));
    TableRec head;
    head.tag = "head";
    head.payload = headTable(1000);
    TableRec hhea;
    hhea.tag = "hhea";
    hhea.payload = hheaTable(uint16_t(cps.size() + 1));
    TableRec hmtx;
    hmtx.tag = "hmtx";
    std::vector<uint16_t> advances(cps.size() + 1, 500);
    hmtx.payload = hmtxTable(advances);
    // cmap: one group per codepoint, glyph ids 1..n.
    std::vector<std::vector<uint32_t>> groups;
    for (size_t i = 0; i < cps.size(); ++i) {
        groups.push_back({cps[i], cps[i], uint32_t(i + 1)});
    }
    TableRec cmap;
    cmap.tag = "cmap";
    cmap.payload = cmapTable(groups, {});
    TableRec name;
    name.tag = "name";
    name.payload = nameTable(family);
    // glyf + loca: glyph 0 is a contour-less header (numberOfContours
    // 0 + its bbox - FreeType's legal blank), every other glyph EMPTY
    // (equal consecutive loca offsets). Enough for the face to
    // register with the platform font stack; the ink is the
    // platform's business.
    TableRec loca;
    loca.tag = "loca";
    Bytes locaB;
    locaB.u16(0);  // glyph 0 starts at 0
    locaB.u16(10); // glyph 0 is 10 bytes long
    for (size_t i = 1; i <= cps.size(); ++i) {
        locaB.u16(10); // every other glyph: empty
    }
    loca.payload = locaB.b;
    TableRec glyf;
    glyf.tag = "glyf";
    Bytes glyfB;
    glyfB.u16(0);   // numberOfContours = 0
    glyfB.s16(0);   // xMin
    glyfB.s16(0);   // yMin
    glyfB.s16(500); // xMax
    glyfB.s16(500); // yMax
    glyf.payload = glyfB.b;
    tables = {maxp, head, hhea, hmtx, cmap, name, loca, glyf};
    return assembleSfnt(tables);
}

// ---- the collection wrapper ----

// A 12-byte "font" with zero tables (parseFontAt refuses it; the
// directory probe sees no emoji tables).
inline std::vector<uint8_t> buildJunkSfnt() {
    Bytes b;
    b.u32(0x00010000u); // sfnt version
    b.u16(0);           // numTables = 0
    b.u16(0);
    b.u16(0);
    b.u16(0);
    return b.b;
}

// A TTC's sub-font directories store FILE-ABSOLUTE table offsets, so
// every wrapped font's directory needs its offsets rebased.
inline void rebaseTableOffsets(std::vector<uint8_t> *font, size_t base) {
    if (font->size() < 12) {
        return;
    }
    const uint16_t numTables = uint16_t((uint16_t((*font)[4]) << 8) | (*font)[5]);
    for (uint16_t i = 0; i < numTables; ++i) {
        const size_t rec = 12 + size_t(i) * 16;
        if (rec + 16 > font->size()) {
            break;
        }
        const uint32_t off = (uint32_t((*font)[rec + 8]) << 24) |
                             (uint32_t((*font)[rec + 9]) << 16) |
                             (uint32_t((*font)[rec + 10]) << 8) | uint32_t((*font)[rec + 11]);
        const uint32_t rebased = uint32_t(base) + off;
        (*font)[rec + 8] = uint8_t(rebased >> 24);
        (*font)[rec + 9] = uint8_t(rebased >> 16);
        (*font)[rec + 10] = uint8_t(rebased >> 8);
        (*font)[rec + 11] = uint8_t(rebased);
    }
}

inline std::vector<uint8_t> wrapTtc(const std::vector<uint8_t> &firstIn,
                                    const std::vector<uint8_t> &secondIn) {
    std::vector<uint8_t> first = firstIn;
    std::vector<uint8_t> second = secondIn;
    const size_t header = 12 + 8; // ttcf header + two offsets
    const size_t firstEnd = header + first.size();
    const size_t pad = (4 - (firstEnd % 4)) % 4;
    rebaseTableOffsets(&first, header);
    rebaseTableOffsets(&second, firstEnd + pad);
    Bytes b;
    b.tag("ttcf");
    b.u32(0x00010000u); // version
    b.u32(2);           // numFonts
    b.u32(uint32_t(header));
    b.u32(uint32_t(firstEnd + pad));
    b.raw(first.data(), first.size());
    for (size_t i = 0; i < pad; ++i) {
        b.u8(0);
    }
    b.raw(second.data(), second.size());
    return b.b;
}

} // namespace fixture
} // namespace fc
