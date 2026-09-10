#include "emoji.h"

#include <algorithm>
#include <cstring>

#include "emoji_clusters.h"

namespace fc {

namespace {

// The PNG signature: every CBDT record we accept must start with it
// (image formats 17/18/19 wrap PNG payloads in the bitmap color fonts).
// The check converts table corruption into "missing glyph" instead of
// garbage pixels, and it is what disambiguates the "first glyph sits
// exactly at sbitOffset" convention of index subtable format 1.
constexpr uint8_t kPngMagic[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

// sbix graphicType tags (big-endian 4cc, compared as u32 values).
constexpr uint32_t kTagPng = 0x706E6720u;  // 'png '
constexpr uint32_t kTagJpg = 0x6A706720u;  // 'jpg '
constexpr uint32_t kTagDupe = 0x64757065u; // 'dupe'
constexpr uint32_t kTagFlip = 0x666C6970u; // 'flip'

constexpr uint32_t kTagTtcf = 0x74746366u;     // 'ttcf'
constexpr uint32_t kSfntTrue = 0x00010000u;    // TrueType sfnt version
constexpr uint32_t kSfntOtto = 0x4F54544Fu;    // 'OTTO'
constexpr uint32_t kSfntTrueTag = 0x74727565u; // 'true'

bool isVariationSelector(uint32_t cp) {
    return cp == kEmojiVS16 || cp == kEmojiVS15;
}

// Mac Roman high bytes (0x80..0xFF) -> Unicode (the classic mapping;
// family names are ASCII in practice, but the fallback stays faithful).
constexpr uint16_t kMacRoman[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3,
    0x00E5, 0x00E7, 0x00E9, 0x00E8, 0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3,
    0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FF, 0x2020, 0x00B0, 0x00A2, 0x00A3,
    0x00A7, 0x2022, 0x00B6, 0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
    0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA,
    0x00BA, 0x03A9, 0x00E6, 0x00F8, 0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB,
    0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153, 0x2013, 0x2014, 0x201C, 0x201D,
    0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, 0x00CB, 0x00C8, 0x00CD, 0x00CE,
    0x00CF, 0x00CC, 0x00D3, 0x00D4, 0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC,
    0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
};

void appendUtf8(std::string &out, uint32_t cp) {
    if (cp <= 0x7Fu) {
        out.push_back(char(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(char(0xC0u | (cp >> 6)));
        out.push_back(char(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(char(0xE0u | (cp >> 12)));
        out.push_back(char(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(char(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(char(0xF0u | (cp >> 18)));
        out.push_back(char(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(char(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(char(0x80u | (cp & 0x3Fu)));
    }
}

// ---- stateless directory access (the free-function helpers) ----

bool probeU16(const uint8_t *data, size_t size, size_t off, uint16_t *v) {
    if (off + 2 > size) {
        return false;
    }
    *v = uint16_t((uint16_t(data[off]) << 8) | uint16_t(data[off + 1]));
    return true;
}

bool probeU32(const uint8_t *data, size_t size, size_t off, uint32_t *v) {
    if (off + 4 > size) {
        return false;
    }
    *v = (uint32_t(data[off]) << 24) | (uint32_t(data[off + 1]) << 16) |
         (uint32_t(data[off + 2]) << 8) | uint32_t(data[off + 3]);
    return true;
}

// One table record found in the directory at dirBase, or false.
// `limit` is the byte length the offsets are validated against (the
// real file size when only a head was read; `size` bounds the reads).
bool probeTable(const uint8_t *data, size_t size, size_t dirBase, size_t limit, const char tag[4],
                uint32_t *off, uint32_t *len) {
    uint16_t numTables = 0;
    if (dirBase + 12 > size || !probeU16(data, size, dirBase + 4, &numTables)) {
        return false;
    }
    for (uint16_t i = 0; i < numTables; ++i) {
        const size_t rec = dirBase + 12 + size_t(i) * 16;
        if (rec + 16 > size) {
            return false;
        }
        if (std::memcmp(data + rec, tag, 4) != 0) {
            continue;
        }
        if (!probeU32(data, size, rec + 8, off) || !probeU32(data, size, rec + 12, len)) {
            return false;
        }
        return size_t(*off) + *len <= limit; // claims to run past the file: refuse
    }
    return false;
}

// The sub-font directory offsets of a TrueType Collection (or just {0}
// for a plain sfnt). Empty when the wrapper itself is unreadable.
std::vector<size_t> fontDirectories(const uint8_t *data, size_t size) {
    std::vector<size_t> dirs;
    if (!data || size < 12) {
        return dirs;
    }
    uint32_t tag = 0;
    if (!probeU32(data, size, 0, &tag)) {
        return dirs;
    }
    if (tag != kTagTtcf) {
        if (tag == kSfntTrue || tag == kSfntOtto || tag == kSfntTrueTag) {
            dirs.push_back(0);
        }
        return dirs;
    }
    uint32_t numFonts = 0;
    if (!probeU32(data, size, 8, &numFonts) || numFonts == 0 || numFonts > 64) {
        return dirs;
    }
    for (uint32_t f = 0; f < numFonts; ++f) {
        uint32_t off = 0;
        if (!probeU32(data, size, 12 + size_t(f) * 4, &off) || off < 12 ||
            size_t(off) + 12 > size) {
            continue;
        }
        dirs.push_back(size_t(off));
    }
    return dirs;
}

// Family name from a 'name' TABLE blob (offsets are table-relative,
// exactly as the spec defines them; the whole-file sfntFamilyName
// slices the table out and lands here, and the discovery scan feeds
// the sliced table directly). Empty string = no readable name.
std::string nameFamilyFromTable(const uint8_t *table, size_t tableLen) {
    if (table == nullptr || tableLen < 6) {
        return std::string();
    }
    uint16_t format = 0, count = 0, strOff = 0;
    if (!probeU16(table, tableLen, 0, &format) || format > 1 ||
        !probeU16(table, tableLen, 2, &count) || !probeU16(table, tableLen, 4, &strOff) ||
        count > 4096) {
        return std::string();
    }
    // Find the best-scoring nameID-1 record (deterministic: the first
    // record at the best priority wins). Priority: Windows en-US,
    // Windows any language, Unicode UTF-16, Mac Roman.
    int bestScore = 0;
    size_t bestRec = 0, bestLen = 0;
    int bestPlatform = 0;
    for (uint16_t i = 0; i < count; ++i) {
        const size_t rec = 6 + size_t(i) * 12;
        if (rec + 12 > tableLen) {
            break;
        }
        uint16_t platform = 0, encoding = 0, language = 0, nameId = 0, length = 0, stroff = 0;
        if (!probeU16(table, tableLen, rec, &platform) ||
            !probeU16(table, tableLen, rec + 2, &encoding) ||
            !probeU16(table, tableLen, rec + 4, &language) ||
            !probeU16(table, tableLen, rec + 6, &nameId) ||
            !probeU16(table, tableLen, rec + 8, &length) ||
            !probeU16(table, tableLen, rec + 10, &stroff)) {
            continue;
        }
        if (nameId != 1 || length == 0 || length > 2048) {
            continue;
        }
        int score = 0;
        if (platform == 3 && (encoding == 1 || encoding == 10)) {
            score = language == 0x409 ? 4 : 3;
        } else if (platform == 0 && (encoding == 1 || encoding == 3)) {
            score = 2;
        } else if (platform == 1 && encoding == 0) {
            score = 1;
        }
        if (score > bestScore) {
            bestScore = score;
            bestRec = size_t(strOff) + size_t(stroff);
            bestLen = size_t(length);
            bestPlatform = platform;
        }
    }
    if (bestScore == 0 || bestRec + bestLen > tableLen) {
        return std::string();
    }
    std::string out;
    out.reserve(bestLen);
    if (bestPlatform == 1) { // Mac Roman bytes
        for (size_t b = 0; b < bestLen; ++b) {
            const uint8_t byte = table[bestRec + b];
            appendUtf8(out, byte < 0x80 ? uint32_t(byte) : uint32_t(kMacRoman[byte - 0x80]));
        }
    } else { // UTF-16BE (Windows / Unicode platform)
        for (size_t b = 0; b + 1 < bestLen; b += 2) {
            const uint16_t u =
                uint16_t((uint16_t(table[bestRec + b]) << 8) | uint16_t(table[bestRec + b + 1]));
            if (u >= 0xD800 && u <= 0xDBFF && b + 3 < bestLen) {
                const uint16_t v = uint16_t((uint16_t(table[bestRec + b + 2]) << 8) |
                                            uint16_t(table[bestRec + b + 3]));
                if (v >= 0xDC00 && v <= 0xDFFF) {
                    appendUtf8(out, 0x10000u + (uint32_t(u - 0xD800) << 10) + uint32_t(v - 0xDC00));
                    // Skip the LOW surrogate unit as well: +2 here plus
                    // the loop's +2 steps a whole pair (a lone ++b would
                    // re-read the pair's second byte as a new unit).
                    b += 2;
                    continue;
                }
            }
            appendUtf8(out, (u >= 0xD800 && u <= 0xDFFF) ? 0xFFFDu : uint32_t(u));
        }
    }
    return out;
}

} // namespace

// ---- bounds-checked readers ----

bool EmojiFont::rdU8(size_t off, uint8_t *v) const {
    if (off + 1 > size_) {
        return false;
    }
    *v = data_[off];
    return true;
}

bool EmojiFont::rdS8(size_t off, int8_t *v) const {
    if (off + 1 > size_) {
        return false;
    }
    *v = static_cast<int8_t>(data_[off]);
    return true;
}

bool EmojiFont::rdU16(size_t off, uint16_t *v) const {
    if (off + 2 > size_) {
        return false;
    }
    *v = uint16_t((uint16_t(data_[off]) << 8) | uint16_t(data_[off + 1]));
    return true;
}

bool EmojiFont::rdU32(size_t off, uint32_t *v) const {
    if (off + 4 > size_) {
        return false;
    }
    *v = (uint32_t(data_[off]) << 24) | (uint32_t(data_[off + 1]) << 16) |
         (uint32_t(data_[off + 2]) << 8) | uint32_t(data_[off + 3]);
    return true;
}

const uint8_t *EmojiFont::table(const char tag[4], size_t *len) const {
    uint16_t numTables = 0;
    if (dirBase_ + 12 > size_ || !rdU16(dirBase_ + 4, &numTables)) {
        return nullptr;
    }
    for (uint16_t i = 0; i < numTables; ++i) {
        const size_t rec = dirBase_ + 12 + size_t(i) * 16;
        if (rec + 16 > size_) {
            return nullptr;
        }
        if (std::memcmp(data_ + rec, tag, 4) != 0) {
            continue;
        }
        uint32_t off = 0, l = 0;
        if (!rdU32(rec + 8, &off) || !rdU32(rec + 12, &l)) {
            return nullptr;
        }
        if (size_t(off) + l > size_ || l == 0) {
            return nullptr; // table claims to run past the file: refuse it
        }
        if (len) {
            *len = size_t(l);
        }
        return data_ + off;
    }
    return nullptr;
}

// ---- CBLC (bitmap index) ----

bool EmojiFont::parseCbloc() {
    size_t len = 0;
    const uint8_t *p = table("CBLC", &len);
    if (!p) {
        return false;
    }
    cblcOff_ = size_t(p - data_);
    cblcLen_ = len;
    uint32_t numSizes = 0;
    if (len < 8 || !rdU32(cblcOff_ + 4, &numSizes) || numSizes == 0 || numSizes > 32) {
        return false;
    }
    for (uint32_t s = 0; s < numSizes; ++s) {
        const size_t b = cblcOff_ + 8 + size_t(s) * 48;
        if (b + 48 > cblcOff_ + cblcLen_) {
            return false;
        }
        Strike strike;
        uint32_t arrayOffset = 0, nSub = 0;
        if (!rdU32(b, &arrayOffset) || !rdU32(b + 8, &nSub) || nSub == 0 || nSub > 1024) {
            continue; // unusable size table; other strikes may still work
        }
        uint8_t ppemX = 0, ppemY = 0, bitDepth = 0;
        rdU8(b + 44, &ppemX);
        rdU8(b + 45, &ppemY);
        rdU8(b + 46, &bitDepth);
        strike.ppem = ppemX > 0 ? int(ppemX) : int(ppemY);
        strike.bitDepth = int(bitDepth);
        strike.arrayOffset = arrayOffset;
        const size_t arr = cblcOff_ + size_t(arrayOffset);
        if (arrayOffset == 0 || arr + size_t(nSub) * 8 > cblcOff_ + cblcLen_) {
            continue;
        }
        for (uint32_t e = 0; e < nSub; ++e) {
            const size_t ent = arr + size_t(e) * 8;
            SubTable sub;
            uint16_t first = 0, last = 0;
            uint32_t off = 0;
            if (!rdU16(ent, &first) || !rdU16(ent + 2, &last) || !rdU32(ent + 4, &off)) {
                continue;
            }
            if (first == 0 || last < first) {
                continue; // glyph 0 (.notdef) never carries a bitmap
            }
            const size_t hdr = arr + size_t(off);
            if (off == 0 || hdr + 8 > cblcOff_ + cblcLen_) {
                continue;
            }
            sub.first = first;
            sub.last = last;
            sub.offset = off;
            strike.subtables.push_back(sub);
        }
        if (strike.ppem > 0 && !strike.subtables.empty()) {
            strikes_.push_back(strike);
        }
    }
    size_t cbdtLen = 0;
    const uint8_t *cbdt = table("CBDT", &cbdtLen);
    if (!cbdt) {
        return false;
    }
    cbdtOff_ = size_t(cbdt - data_);
    cbdtLen_ = cbdtLen;
    return !strikes_.empty();
}

// ---- sbix (Apple bitmap strikes) ----

bool EmojiFont::parseSbix() {
    size_t len = 0;
    const uint8_t *p = table("sbix", &len);
    if (!p) {
        return false;
    }
    sbixOff_ = size_t(p - data_);
    sbixLen_ = len;
    if (len < 8) {
        return false;
    }
    uint16_t version = 0, flags = 0;
    uint32_t numStrikes = 0;
    if (!rdU16(sbixOff_, &version) || !rdU16(sbixOff_ + 2, &flags) ||
        !rdU32(sbixOff_ + 4, &numStrikes)) {
        return false;
    }
    if (version < 1 || numStrikes == 0 || numStrikes > 256) {
        return false;
    }
    const size_t tableEnd = sbixOff_ + sbixLen_;
    for (uint32_t s = 0; s < numStrikes; ++s) {
        uint32_t strikeOff = 0;
        if (!rdU32(sbixOff_ + 8 + size_t(s) * 4, &strikeOff) || strikeOff == 0) {
            continue;
        }
        const size_t base = sbixOff_ + size_t(strikeOff);
        if (base + 12 > tableEnd) {
            continue; // not even one offset pair fits
        }
        uint16_t ppem = 0, ppi = 0;
        if (!rdU16(base, &ppem) || !rdU16(base + 2, &ppi) || ppem == 0) {
            continue;
        }
        if (numGlyphs_ <= 0) {
            continue; // no glyph ids -> no records
        }
        // The glyph-offset array runs [base+4, base+4+(numGlyphs+1)*4);
        // the strike's records may extend past it, but never past the
        // table. A short array simply leaves later glyphs "missing".
        SbixStrike strike;
        strike.ppem = int(ppem);
        strike.base = base;
        strike.end = tableEnd;
        sbixStrikes_.push_back(strike);
    }
    return !sbixStrikes_.empty();
}

// ---- cmap ----

bool EmojiFont::parseCmap() {
    size_t len = 0;
    const uint8_t *p = table("cmap", &len);
    if (!p) {
        return false;
    }
    const size_t base = size_t(p - data_);
    uint16_t numTables = 0;
    if (len < 4 || !rdU16(base + 2, &numTables)) {
        return false;
    }
    for (uint16_t i = 0; i < numTables; ++i) {
        const size_t rec = base + 4 + size_t(i) * 8;
        if (rec + 8 > base + len) {
            break;
        }
        uint16_t platform = 0, encoding = 0;
        uint32_t off = 0;
        if (!rdU16(rec, &platform) || !rdU16(rec + 2, &encoding) || !rdU32(rec + 4, &off)) {
            continue;
        }
        const size_t sub = base + size_t(off);
        if (off == 0 || sub + 2 > base + len) {
            continue;
        }
        uint16_t fmt = 0;
        if (!rdU16(sub, &fmt)) {
            continue;
        }
        if (fmt == 12) {
            uint32_t nGroups = 0;
            if (sub + 16 > base + len || !rdU32(sub + 12, &nGroups) || nGroups > 100000) {
                continue;
            }
            for (uint32_t g = 0; g < nGroups; ++g) {
                const size_t gr = sub + 16 + size_t(g) * 12;
                if (gr + 12 > base + len) {
                    break;
                }
                CmapRange r;
                uint32_t start = 0, end = 0, firstGlyph = 0;
                if (!rdU32(gr, &start) || !rdU32(gr + 4, &end) || !rdU32(gr + 8, &firstGlyph)) {
                    continue;
                }
                if (end < start) {
                    continue;
                }
                r.start = start;
                r.end = end;
                r.firstGlyph = firstGlyph;
                cmapRanges_.push_back(r);
            }
        } else if (fmt == 4 && !hasCmap4_) {
            // keep the first BMP subtable; lookups parse it on demand
            hasCmap4_ = true;
            cmap4Off_ = sub;
        } else if (fmt == 14) {
            // non-default Unicode Variation Selector mappings
            uint32_t nRec = 0;
            if (sub + 10 > base + len || !rdU32(sub + 6, &nRec) || nRec > 4096) {
                continue;
            }
            for (uint32_t r = 0; r < nRec; ++r) {
                const size_t rec14 = sub + 10 + size_t(r) * 11;
                if (rec14 + 11 > base + len) {
                    break;
                }
                const uint32_t selector = (uint32_t(data_[rec14]) << 16) |
                                          (uint32_t(data_[rec14 + 1]) << 8) |
                                          uint32_t(data_[rec14 + 2]);
                uint32_t ndOff = 0;
                if (!rdU32(rec14 + 7, &ndOff) || ndOff == 0) {
                    continue;
                }
                const size_t nd = sub + size_t(ndOff);
                if (nd + 4 > base + len) {
                    continue;
                }
                uint32_t nMap = 0;
                if (!rdU32(nd, &nMap) || nMap > 100000) {
                    continue;
                }
                for (uint32_t m = 0; m < nMap; ++m) {
                    const size_t mr = nd + 4 + size_t(m) * 5;
                    if (mr + 5 > base + len) {
                        break;
                    }
                    const uint32_t baseCp = (uint32_t(data_[mr]) << 16) |
                                            (uint32_t(data_[mr + 1]) << 8) |
                                            uint32_t(data_[mr + 2]);
                    uint16_t gid = 0;
                    rdU16(mr + 3, &gid);
                    if (gid != 0) {
                        variants_[{baseCp, selector}] = gid;
                    }
                }
            }
        }
    }
    // The spec says format 12 groups are sorted, non-overlapping; if a
    // font violates that, fall back to a linear scan at lookup time.
    for (size_t i = 1; i < cmapRanges_.size(); ++i) {
        if (cmapRanges_[i].start < cmapRanges_[i - 1].start ||
            cmapRanges_[i].start <= cmapRanges_[i - 1].end) {
            cmapUnsorted_ = true;
            break;
        }
    }
    return !cmapRanges_.empty() || hasCmap4_;
}

uint16_t EmojiFont::codepointGlyph(uint32_t cp) const {
    if (!loaded_) {
        return 0;
    }
    if (!cmapRanges_.empty()) {
        if (cmapUnsorted_) {
            for (const CmapRange &r : cmapRanges_) {
                if (cp >= r.start && cp <= r.end) {
                    return uint16_t(r.firstGlyph + (cp - r.start));
                }
            }
        } else {
            size_t lo = 0, hi = cmapRanges_.size();
            while (lo < hi) {
                const size_t mid = lo + (hi - lo) / 2;
                if (cmapRanges_[mid].end < cp) {
                    lo = mid + 1;
                } else if (cmapRanges_[mid].start > cp) {
                    hi = mid;
                } else {
                    return uint16_t(cmapRanges_[mid].firstGlyph + (cp - cmapRanges_[mid].start));
                }
            }
        }
        // format 12 (when present) is the complete mapping; a miss here
        // means the font genuinely does not map the codepoint.
        return 0;
    }
    if (!hasCmap4_) {
        return 0;
    }
    // format 4 (only when no format 12 table exists)
    uint16_t segX2 = 0;
    if (!rdU16(cmap4Off_ + 6, &segX2) || segX2 == 0 || (segX2 & 1) != 0) {
        return 0;
    }
    const size_t segCount = segX2 / 2;
    const size_t endCode = cmap4Off_ + 14;
    const size_t startCode = endCode + size_t(segCount) * 2 + 2;
    const size_t idDelta = startCode + size_t(segCount) * 2;
    const size_t idRange = idDelta + size_t(segCount) * 2;
    for (size_t i = 0; i < segCount; ++i) {
        uint16_t end = 0;
        if (!rdU16(endCode + i * 2, &end) || end < cp) {
            continue;
        }
        uint16_t start = 0;
        if (!rdU16(startCode + i * 2, &start) || cp < start) {
            return 0; // past every segment covering cp: unmapped
        }
        uint16_t delta = 0, rangeOff = 0;
        if (!rdU16(idDelta + i * 2, &delta) || !rdU16(idRange + i * 2, &rangeOff)) {
            return 0;
        }
        if (rangeOff == 0) {
            return uint16_t(cp + delta);
        }
        const size_t gidAddr = idRange + i * 2 + size_t(rangeOff) + size_t(cp - start) * 2;
        uint16_t gid = 0;
        if (!rdU16(gidAddr, &gid)) {
            return 0;
        }
        return gid == 0 ? 0 : uint16_t(gid + delta);
    }
    return 0;
}

uint16_t EmojiFont::variantGlyph(uint32_t base, uint32_t selector) const {
    const auto it = variants_.find({base, selector});
    return it == variants_.end() ? 0 : it->second;
}

// ---- GSUB ligature map ----

bool EmojiFont::parseGsub() {
    size_t len = 0;
    const uint8_t *p = table("GSUB", &len);
    if (!p) {
        return true; // ligature rules are optional (singles still work)
    }
    const size_t gsub = size_t(p - data_);
    uint16_t lookupListOff = 0;
    if (len < 10 || !rdU16(gsub + 8, &lookupListOff)) {
        return true;
    }
    const size_t ll = gsub + size_t(lookupListOff);
    uint16_t lookupCount = 0;
    if (lookupListOff == 0 || !rdU16(ll, &lookupCount) || lookupCount > 512) {
        return true;
    }
    for (uint16_t i = 0; i < lookupCount; ++i) {
        uint16_t lookupOff = 0;
        if (!rdU16(ll + 2 + size_t(i) * 2, &lookupOff)) {
            break;
        }
        const size_t lOff = ll + size_t(lookupOff);
        uint16_t lookupType = 0, subCount = 0;
        if (lookupOff == 0 || !rdU16(lOff, &lookupType) || !rdU16(lOff + 4, &subCount) ||
            subCount > 512) {
            continue;
        }
        for (uint16_t s = 0; s < subCount; ++s) {
            uint16_t subOff = 0;
            if (!rdU16(lOff + 6 + size_t(s) * 2, &subOff) || subOff == 0) {
                continue;
            }
            size_t st = lOff + size_t(subOff);
            uint16_t type = lookupType;
            if (type == 7) { // extension: wraps one real subtable
                uint16_t extType = 0;
                uint32_t extOff = 0;
                if (!rdU16(st + 2, &extType) || !rdU32(st + 4, &extOff) || extType != 4) {
                    continue;
                }
                type = 4;
                st = st + size_t(extOff);
            }
            if (type != 4) {
                continue; // only ligature substitution feeds the map
            }
            uint16_t fmt = 0, covOff = 0, setCount = 0;
            if (!rdU16(st, &fmt) || fmt != 1 || !rdU16(st + 2, &covOff) ||
                !rdU16(st + 4, &setCount) || covOff == 0 || setCount > 4096) {
                continue;
            }
            // coverage: dense glyph array indexed by ligature-set index
            const size_t cov = st + size_t(covOff);
            uint16_t covFmt = 0;
            if (!rdU16(cov, &covFmt)) {
                continue;
            }
            std::vector<uint16_t> coverage;
            std::vector<uint8_t> present;
            if (covFmt == 1) {
                uint16_t n = 0;
                if (!rdU16(cov + 2, &n) || n != setCount) {
                    continue;
                }
                coverage.resize(n);
                present.resize(n);
                for (uint16_t g = 0; g < n; ++g) {
                    present[g] = rdU16(cov + 4 + size_t(g) * 2, &coverage[g]) ? 1 : 0;
                }
            } else if (covFmt == 2) {
                uint16_t n = 0;
                if (!rdU16(cov + 2, &n) || n > 4096) {
                    continue;
                }
                coverage.assign(size_t(setCount) + 16, 0);
                present.assign(size_t(setCount) + 16, 0);
                bool ok = true;
                for (uint16_t r = 0; r < n && ok; ++r) {
                    const size_t rr = cov + 4 + size_t(r) * 6;
                    uint16_t s0 = 0, e0 = 0, si = 0;
                    if (!rdU16(rr, &s0) || !rdU16(rr + 2, &e0) || !rdU16(rr + 4, &si) || e0 < s0 ||
                        size_t(si) + (e0 - s0) + 1 > coverage.size()) {
                        ok = false;
                        break;
                    }
                    for (uint32_t g = s0; g <= e0; ++g) {
                        const size_t idx = size_t(si) + (g - s0);
                        coverage[idx] = uint16_t(g);
                        present[idx] = 1;
                    }
                }
                if (!ok) {
                    continue;
                }
            } else {
                continue;
            }
            for (uint16_t k = 0; k < setCount; ++k) {
                if (k >= coverage.size() || !present[k]) {
                    continue;
                }
                uint16_t setOff = 0;
                if (!rdU16(st + 6 + size_t(k) * 2, &setOff) || setOff == 0) {
                    continue;
                }
                const size_t set = st + size_t(setOff);
                uint16_t ligCount = 0;
                if (!rdU16(set, &ligCount) || ligCount > 4096) {
                    continue;
                }
                for (uint16_t l = 0; l < ligCount; ++l) {
                    uint16_t ligOff = 0;
                    if (!rdU16(set + 2 + size_t(l) * 2, &ligOff) || ligOff == 0) {
                        continue;
                    }
                    const size_t lig = set + size_t(ligOff);
                    uint16_t ligGlyph = 0, compCount = 0;
                    if (!rdU16(lig, &ligGlyph) || !rdU16(lig + 2, &compCount) || compCount < 2 ||
                        compCount > 16 || ligGlyph == 0) {
                        continue;
                    }
                    std::vector<uint16_t> seq;
                    seq.reserve(compCount);
                    seq.push_back(coverage[k]);
                    bool ok = true;
                    for (uint16_t c = 1; c < compCount; ++c) {
                        uint16_t comp = 0;
                        if (!rdU16(lig + 4 + size_t(c - 1) * 2, &comp)) {
                            ok = false;
                            break;
                        }
                        seq.push_back(comp);
                    }
                    if (ok) {
                        ligatures_[seq] = ligGlyph;
                        maxLigatureLen_ = std::max(maxLigatureLen_, int(compCount));
                    }
                }
            }
        }
    }
    return true;
}

uint16_t EmojiFont::ligatureFor(const uint16_t *glyphs, size_t count) const {
    if (glyphs == nullptr || count < 2) {
        return 0;
    }
    const auto it = ligatures_.find(std::vector<uint16_t>(glyphs, glyphs + count));
    return it == ligatures_.end() ? 0 : it->second;
}

// ---- maxp / head / hmtx ----

bool EmojiFont::parseMaxp() {
    size_t len = 0;
    const uint8_t *p = table("maxp", &len);
    if (!p || len < 6) {
        numGlyphs_ = 0xFFFF; // absent: assume every glyph id is valid
        return true;
    }
    uint16_t n = 0;
    if (!rdU16(size_t(p - data_) + 4, &n)) {
        numGlyphs_ = 0xFFFF;
        return true;
    }
    numGlyphs_ = int(n);
    return true;
}

bool EmojiFont::parseHead() {
    size_t len = 0;
    const uint8_t *p = table("head", &len);
    if (!p || len < 20) {
        return false;
    }
    uint16_t upem = 0;
    if (!rdU16(size_t(p - data_) + 18, &upem) || upem < 16 || upem > 16384) {
        return false;
    }
    unitsPerEm_ = int(upem);
    return true;
}

bool EmojiFont::parseHmtx() {
    size_t hheaLen = 0;
    const uint8_t *hhea = table("hhea", &hheaLen);
    if (!hhea || hheaLen < 36) {
        return false;
    }
    uint16_t numOfMetrics = 0;
    if (!rdU16(size_t(hhea - data_) + 34, &numOfMetrics) || numOfMetrics == 0) {
        return false;
    }
    size_t hmtxLen = 0;
    const uint8_t *hmtx = table("hmtx", &hmtxLen);
    if (!hmtx) {
        return false;
    }
    const size_t base = size_t(hmtx - data_);
    const size_t readable = hmtxLen / 4; // advance+lsb pairs
    const size_t count = std::min(size_t(numOfMetrics), std::min(readable, size_t(numGlyphs_)));
    hmtxAdvances_.clear();
    hmtxAdvances_.reserve(count);
    uint16_t last = 0;
    for (size_t i = 0; i < count; ++i) {
        uint16_t adv = 0;
        if (!rdU16(base + i * 4, &adv)) {
            break;
        }
        last = adv;
        hmtxAdvances_.push_back(adv);
    }
    if (hmtxAdvances_.empty()) {
        return false;
    }
    // Glyphs past numberOfHMetrics repeat the last advance (hmtx rule).
    hmtxTailAdvance_ = last;
    return true;
}

int EmojiFont::sbixAdvance(uint16_t glyph, int ppem) const {
    // The record carries no advance: hmtx units scaled into strike
    // pixels (the same math FreeType runs: units * ppem / upem).
    const uint16_t units = glyph < hmtxAdvances_.size() ? hmtxAdvances_[glyph] : hmtxTailAdvance_;
    const int64_t scaled =
        (int64_t(units) * int64_t(ppem) + unitsPerEm_ / 2) / int64_t(unitsPerEm_);
    return scaled > 0 ? int(scaled) : 0;
}

// ---- load ----

// Every parse attempt starts from a clean slate: data_/size_ survive
// (they describe the blob, not a face), everything else is cleared so a
// sub-font that failed mid-parse cannot leak its strikes, cmap ranges,
// or hmtx metrics into the next face's attempt.
void EmojiFont::resetState() {
    loaded_ = false;
    strikes_.clear();
    sbixStrikes_.clear();
    cmapRanges_.clear();
    cmapUnsorted_ = false;
    hasCmap4_ = false;
    cmap4Off_ = 0;
    variants_.clear();
    ligatures_.clear();
    maxLigatureLen_ = 0;
    numGlyphs_ = 0;
    unitsPerEm_ = 1000;
    hmtxAdvances_.clear();
    hmtxTailAdvance_ = 0;
    dirBase_ = 0;
    cbdtOff_ = cbdtLen_ = cblcOff_ = cblcLen_ = sbixOff_ = sbixLen_ = 0;
}

bool EmojiFont::parseFontAt(size_t dirBase) {
    resetState();
    dirBase_ = dirBase;
    uint16_t numTables = 0;
    if (dirBase_ + 12 > size_ || !rdU16(dirBase_ + 4, &numTables) || numTables == 0 ||
        dirBase_ + 12 + size_t(numTables) * 16 > size_) {
        return false;
    }
    parseMaxp(); // glyph count gates the sbix offset arrays
    bool haveBitmaps = false;
    if (parseCbloc()) {
        haveBitmaps = true; // CBDT route (CBLC also wins over sbix)
    } else if (parseSbix()) {
        // The sbix route needs hmtx advances and the units-per-em for
        // its placement math; a font missing them is unusable.
        if (!parseHead() || !parseHmtx()) {
            return false;
        }
        haveBitmaps = true;
    }
    if (!haveBitmaps) {
        return false;
    }
    if (!parseCmap()) {
        return false;
    }
    parseGsub();
    return true;
}

bool EmojiFont::load(const uint8_t *data, size_t size) {
    resetState();
    data_ = data;
    size_ = size;
    if (!data || size < 12) {
        return false;
    }
    uint32_t tag = 0;
    if (!rdU32(0, &tag)) {
        return false;
    }
    if (tag == kTagTtcf) {
        // TrueType Collection: keep the first sub-font that parses as
        // a usable emoji face (Apple Color Emoji.ttc carries two).
        uint32_t numFonts = 0;
        if (size < 12 || !rdU32(8, &numFonts) || numFonts == 0 || numFonts > 64) {
            return false;
        }
        for (uint32_t f = 0; f < numFonts; ++f) {
            uint32_t off = 0;
            if (!rdU32(12 + size_t(f) * 4, &off) || off < 12 || size_t(off) + 12 > size) {
                continue;
            }
            if (parseFontAt(size_t(off))) {
                loaded_ = true;
                return true;
            }
        }
        return false;
    }
    if (tag != kSfntTrue && tag != kSfntOtto && tag != kSfntTrueTag) {
        return false; // not a font we can read
    }
    if (!parseFontAt(0)) {
        return false;
    }
    loaded_ = true;
    return true;
}

// ---- strikes / bitmaps ----

int EmojiFont::strikePpem() const {
    int best = 0;
    for (const Strike &s : strikes_) {
        best = std::max(best, s.ppem);
    }
    for (const SbixStrike &s : sbixStrikes_) {
        best = std::max(best, s.ppem);
    }
    return best;
}

int EmojiFont::bitDepth() const {
    const Strike *best = nullptr;
    for (const Strike &s : strikes_) {
        if (!best || s.ppem > best->ppem) {
            best = &s;
        }
    }
    return best ? best->bitDepth : 0;
}

bool EmojiFont::bitmapFor(uint16_t glyph, Bitmap *out) const {
    if (!strikes_.empty()) {
        return bitmapForCbdt(glyph, out);
    }
    return bitmapForSbix(glyph, out);
}

bool EmojiFont::bitmapForCbdt(uint16_t glyph, Bitmap *out) const {
    if (!loaded_ || !out || glyph == 0 || glyph >= numGlyphs_) {
        return false;
    }
    // Largest strike wins (deterministic; most CBDT fonts have one).
    // Resolved in one pass: strike, subtable header, glyph index in it.
    const Strike *best = nullptr;
    size_t hdr = 0;
    int idx = -1;
    for (const Strike &s : strikes_) {
        if (s.ppem <= 0) {
            continue;
        }
        for (const SubTable &sub : s.subtables) {
            if (glyph < sub.first || glyph > sub.last) {
                continue;
            }
            const size_t cand = cblcOff_ + size_t(s.arrayOffset) + size_t(sub.offset);
            if (cand + 8 > cblcOff_ + cblcLen_) {
                continue;
            }
            if (!best || s.ppem > best->ppem) {
                best = &s;
                hdr = cand;
                idx = int(glyph) - int(sub.first);
            }
            break; // a glyph lives in at most one subtable per strike
        }
    }
    if (!best || idx < 0) {
        return false;
    }
    uint32_t sbit = 0;
    if (!rdU32(hdr + 4, &sbit)) {
        return false;
    }
    uint16_t indexFormat = 0, imageFormat = 0;
    if (!rdU16(hdr, &indexFormat) || !rdU16(hdr + 2, &imageFormat)) {
        return false;
    }
    size_t dataOff = 0;
    if (indexFormat == 1) {
        // sbitOffset + offsetArray[i], i = 0..numGlyphs (the last entry
        // is a one-past-the-end sentinel). offsetArray[0] is
        // conventionally 0 (the first glyph sits exactly at sbitOffset);
        // 0 at any other index is the explicit no-bitmap marker.
        uint32_t entry = 0;
        if (!rdU32(hdr + 8 + size_t(idx) * 4, &entry)) {
            return false;
        }
        if (entry == 0 && idx != 0) {
            return false; // explicit no-bitmap marker
        }
        dataOff = size_t(sbit) + size_t(entry);
    } else if (indexFormat == 2) {
        // uniform glyph size: data = sbitOffset + index * imageSize
        uint32_t imageSize = 0;
        if (!rdU32(hdr + 8, &imageSize) || imageSize == 0) {
            return false;
        }
        dataOff = size_t(sbit) + size_t(idx) * size_t(imageSize);
    } else if (indexFormat == 3) {
        uint32_t entry = 0;
        if (!rdU32(hdr + 8 + size_t(idx) * 4, &entry)) {
            return false;
        }
        if (entry == 0) {
            return false;
        }
        dataOff = size_t(sbit) + size_t(entry);
    } else {
        return false; // formats 4/5 (per-glyph metrics variants) unused here
    }
    // Decode the CBDT record.
    if (dataOff + 6 > cbdtOff_ + cbdtLen_) {
        return false;
    }
    const size_t rec = cbdtOff_ + dataOff;
    Metrics m;
    uint8_t h = 0, w = 0, adv = 0;
    int8_t bx = 0, by = 0;
    uint32_t pngLen = 0;
    size_t pngStart = 0;
    if (imageFormat == 17) {
        if (!rdU8(rec, &h) || !rdU8(rec + 1, &w) || !rdS8(rec + 2, &bx) || !rdS8(rec + 3, &by) ||
            !rdU8(rec + 4, &adv) || !rdU32(rec + 5, &pngLen)) {
            return false;
        }
        pngStart = rec + 9;
    } else if (imageFormat == 19) {
        uint8_t len8 = 0;
        if (!rdU8(rec, &h) || !rdU8(rec + 1, &w) || !rdS8(rec + 2, &bx) || !rdS8(rec + 3, &by) ||
            !rdU8(rec + 4, &adv) || !rdU8(rec + 5, &len8)) {
            return false;
        }
        pngLen = len8;
        pngStart = rec + 6;
    } else if (imageFormat == 18) {
        if (!rdU8(rec, &h) || !rdU8(rec + 1, &w) || !rdS8(rec + 2, &bx) || !rdS8(rec + 3, &by) ||
            !rdU8(rec + 4, &adv) || !rdU32(rec + 12, &pngLen)) {
            return false;
        }
        pngStart = rec + 16;
    } else {
        return false; // raw bit depths (1/2/4/8) are not color formats
    }
    if (pngLen == 0 || pngLen > cbdtLen_ || pngStart + pngLen > cbdtOff_ + cbdtLen_) {
        return false;
    }
    if (std::memcmp(data_ + pngStart, kPngMagic, 8) != 0) {
        return false; // not a PNG payload: treat as missing, never garbage
    }
    m.width = int(w);
    m.height = int(h);
    m.bearingX = int(bx);
    m.bearingY = int(by);
    m.advance = int(adv);
    out->metrics = m;
    out->ppem = best->ppem;
    out->data = data_ + pngStart;
    out->dataLen = size_t(pngLen);
    out->format = 0; // CBDT records are PNG with complete metrics
    out->originY = 0;
    out->mirror = false;
    return true;
}

bool EmojiFont::bitmapForSbix(uint16_t glyph, Bitmap *out) const {
    if (!loaded_ || !out || glyph == 0 || (numGlyphs_ > 0 && glyph >= numGlyphs_)) {
        return false;
    }
    const size_t tableEnd = sbixOff_ + sbixLen_;
    // Largest strike that has a record for the glyph wins.
    const SbixStrike *best = nullptr;
    for (const SbixStrike &s : sbixStrikes_) {
        if (s.ppem <= 0) {
            continue;
        }
        if (s.base + 4 + (size_t(glyph) + 1) * 4 > s.end) {
            continue; // the offset array does not cover this glyph
        }
        uint32_t g0 = 0, g1 = 0;
        if (!rdU32(s.base + 4 + size_t(glyph) * 4, &g0) ||
            !rdU32(s.base + 4 + (size_t(glyph) + 1) * 4, &g1) || g0 >= g1) {
            continue; // zero-length or unreadable record = missing here
        }
        if (!best || s.ppem > best->ppem) {
            best = &s;
        }
    }
    if (!best) {
        return false;
    }
    // 'dupe'/'flip' record indirection, resolved in place with a depth
    // guard (FreeType allows 4 hops; a cycle then reads as missing).
    uint16_t g = glyph;
    bool mirror = false;
    for (int depth = 0; depth < 4; ++depth) {
        if (best->base + 4 + (size_t(g) + 1) * 4 > best->end) {
            return false;
        }
        uint32_t g0 = 0, g1 = 0;
        if (!rdU32(best->base + 4 + size_t(g) * 4, &g0) ||
            !rdU32(best->base + 4 + (size_t(g) + 1) * 4, &g1) || g0 >= g1 || g1 - g0 < 8) {
            return false;
        }
        if (size_t(g1) > tableEnd - best->base) {
            return false; // record claims to run past the table
        }
        const size_t rec = best->base + size_t(g0);
        uint16_t ox = 0, oy = 0;
        uint32_t tag = 0;
        if (!rdU16(rec, &ox) || !rdU16(rec + 2, &oy) || !rdU32(rec + 4, &tag)) {
            return false;
        }
        if (tag == kTagDupe || tag == kTagFlip) {
            if (g1 - g0 < 10) {
                return false; // no room for the target glyph id
            }
            uint16_t target = 0;
            if (!rdU16(rec + 8, &target) || target == 0) {
                return false;
            }
            if (tag == kTagFlip) {
                mirror = !mirror;
            }
            g = target;
            continue;
        }
        if (tag != kTagPng && tag != kTagJpg) {
            return false; // 'tiff'/'rgbl'/unknown: not decodable here
        }
        out->metrics.width = 0; // the image carries its own size
        out->metrics.height = 0;
        out->metrics.bearingX = int(int16_t(ox));
        out->metrics.bearingY = 0;
        out->metrics.advance = sbixAdvance(g, best->ppem);
        out->ppem = best->ppem;
        out->data = data_ + rec + 8;
        out->dataLen = size_t(g1 - g0) - 8;
        out->format = tag;
        out->originY = int(int16_t(oy));
        out->mirror = mirror;
        return true;
    }
    return false; // indirection cycle
}

// ---- cluster resolution ----

bool EmojiFont::resolveCluster(const uint32_t *cps, size_t count, size_t pos, Resolved *out) const {
    if (!loaded_ || !cps || pos >= count || !out) {
        return false;
    }
    const uint32_t base = cps[pos];
    if (base == 0x0Au || base == 0x20u || base == 0x09u) {
        return false; // separators are never emoji
    }
    const uint16_t baseGlyph = codepointGlyph(base);
    if (baseGlyph == 0) {
        return false; // the emoji font does not map it at all
    }
    // A variation selector right after the base: consumed into the
    // cluster; VS16 also forces emoji presentation for the base.
    bool forced = false;
    size_t i = pos + 1;
    if (i < count && isVariationSelector(cps[i])) {
        forced = cps[i] == kEmojiVS16;
        ++i;
    }
    // Build the glyph stream for ligature matching: FE0F/FE0E dropped,
    // stops at separators, unmapped codepoints, or maxLigatureLen_
    // glyphs. `after[k]` is the codepoint index just past the k-th
    // glyph's own codepoint (skipped selectors in between are covered
    // by the NEXT glyph's entry).
    std::vector<uint16_t> seq;
    std::vector<size_t> after;
    seq.reserve(size_t(maxLigatureLen_) + 1);
    after.reserve(size_t(maxLigatureLen_) + 1);
    seq.push_back(baseGlyph);
    after.push_back(pos + 1);
    size_t j = i;
    while (seq.size() < size_t(maxLigatureLen_) && j < count) {
        const uint32_t cp = cps[j];
        if (cp == 0x0Au || cp == 0x20u || cp == 0x09u) {
            break;
        }
        if (isVariationSelector(cp)) {
            ++j;
            continue;
        }
        const uint16_t g = codepointGlyph(cp);
        if (g == 0) {
            break;
        }
        seq.push_back(g);
        after.push_back(j + 1);
        ++j;
    }
    // Longest-match ligature first (families with skin tones are up to
    // 9 glyphs; flags are pairs; keycaps 2-3).
    for (size_t len = seq.size(); len >= 2; --len) {
        const uint16_t lig = ligatureFor(seq.data(), len);
        if (lig != 0) {
            size_t end = after[len - 1];
            // consume a trailing variation selector into the cluster
            while (end < count && isVariationSelector(cps[end])) {
                ++end;
            }
            out->glyph = lig;
            out->codepoints = int(end - pos);
            return true;
        }
    }
    // A format-14 mapping for base+VS16 (the standard mechanism when
    // the font carries one; Noto has an empty table).
    if (forced) {
        const uint16_t v = variantGlyph(base, kEmojiVS16);
        if (v != 0) {
            out->glyph = v;
            out->codepoints = int(i - pos);
            return true;
        }
    }
    // Single codepoint with emoji presentation.
    if (forced || isDefaultEmojiPresentation(base)) {
        out->glyph = baseGlyph;
        out->codepoints = int(i - pos);
        return true;
    }
    return false;
}

// ---- presentation policy (the shared table in emoji_clusters.h) ----

bool EmojiFont::isDefaultEmojiPresentation(uint32_t cp) {
    return fc::isDefaultEmojiPresentation(cp);
}

bool EmojiFont::codepointHasBitmap(uint32_t cp) const {
    if (!loaded_) {
        return false;
    }
    const uint16_t glyph = codepointGlyph(cp);
    if (glyph == 0) {
        return false;
    }
    Bitmap bm;
    return bitmapFor(glyph, &bm);
}

// ---- standalone helpers ----

EmojiFontFormat sfntBitmapEmojiFormat(const uint8_t *data, size_t size, int64_t fileSize) {
    const size_t limit = fileSize > int64_t(size) ? size_t(fileSize) : size; // max(avail, real)
    for (size_t dirBase : fontDirectories(data, size)) {
        uint32_t off = 0, len = 0;
        if (probeTable(data, size, dirBase, limit, "CBLC", &off, &len) &&
            probeTable(data, size, dirBase, limit, "CBDT", &off, &len)) {
            return EmojiFontFormat::Cbdt;
        }
        if (probeTable(data, size, dirBase, limit, "sbix", &off, &len)) {
            return EmojiFontFormat::Sbix;
        }
    }
    return EmojiFontFormat::None;
}

std::string sfntFamilyName(const uint8_t *data, size_t size) {
    for (size_t dirBase : fontDirectories(data, size)) {
        uint32_t off = 0, len = 0;
        if (!probeTable(data, size, dirBase, size, "name", &off, &len) || len < 6) {
            continue;
        }
        std::string name = nameFamilyFromTable(data + size_t(off), size_t(len));
        if (!name.empty()) {
            return name;
        }
    }
    return std::string();
}

std::string sfntNameFamily(const uint8_t *name, size_t size) {
    if (name == nullptr || size < 6) {
        return std::string();
    }
    return nameFamilyFromTable(name, size);
}

// Counts battery codepoints mapped by a 'cmap' TABLE blob. Standalone
// bounds-checked readers (the blob is not part of any EmojiFont).
namespace {

inline bool covU16(const uint8_t *p, size_t size, size_t off, uint16_t *v) {
    if (off + 2 > size) {
        return false;
    }
    *v = uint16_t((uint16_t(p[off]) << 8) | uint16_t(p[off + 1]));
    return true;
}

inline bool covU32(const uint8_t *p, size_t size, size_t off, uint32_t *v) {
    if (off + 4 > size) {
        return false;
    }
    *v = (uint32_t(p[off]) << 24) | (uint32_t(p[off + 1]) << 16) | (uint32_t(p[off + 2]) << 8) |
         uint32_t(p[off + 3]);
    return true;
}

// True when the format-4 subtable at `sub` maps `cp` to a nonzero glyph.
bool covFormat4(const uint8_t *p, size_t size, size_t sub, uint32_t cp) {
    uint16_t segX2 = 0;
    if (!covU16(p, size, sub + 6, &segX2) || segX2 == 0 || (segX2 & 1) != 0) {
        return false;
    }
    const size_t segCount = segX2 / 2;
    const size_t endCode = sub + 14;
    const size_t startCode = endCode + segCount * 2 + 2;
    const size_t idDelta = startCode + segCount * 2;
    const size_t idRange = idDelta + segCount * 2;
    if (idRange > size) {
        return false;
    }
    for (size_t i = 0; i < segCount; ++i) {
        uint16_t end = 0;
        if (!covU16(p, size, endCode + i * 2, &end) || end < cp) {
            continue;
        }
        uint16_t start = 0;
        if (!covU16(p, size, startCode + i * 2, &start) || cp < start) {
            return false; // past every segment covering cp: unmapped
        }
        uint16_t delta = 0, rangeOff = 0;
        if (!covU16(p, size, idDelta + i * 2, &delta) ||
            !covU16(p, size, idRange + i * 2, &rangeOff)) {
            return false;
        }
        if (rangeOff == 0) {
            return uint16_t(cp + delta) != 0;
        }
        const size_t gidAddr = idRange + i * 2 + size_t(rangeOff) + size_t(cp - start) * 2;
        uint16_t gid = 0;
        if (!covU16(p, size, gidAddr, &gid)) {
            return false;
        }
        return gid != 0 && uint16_t(gid + delta) != 0;
    }
    return false;
}

// True when the format-12 subtable at `sub` maps `cp` to a nonzero glyph.
bool covFormat12(const uint8_t *p, size_t size, size_t sub, uint32_t cp) {
    uint32_t nGroups = 0;
    if (sub + 16 > size || !covU32(p, size, sub + 12, &nGroups) || nGroups > 100000) {
        return false;
    }
    // Groups are sorted by spec; a font that violates that still gets a
    // linear scan (same recovery philosophy as EmojiFont::parseCmap).
    for (uint32_t g = 0; g < nGroups; ++g) {
        const size_t gr = sub + 16 + size_t(g) * 12;
        if (gr + 12 > size) {
            break;
        }
        uint32_t start = 0, end = 0, firstGlyph = 0;
        if (!covU32(p, size, gr, &start) || !covU32(p, size, gr + 4, &end) ||
            !covU32(p, size, gr + 8, &firstGlyph)) {
            continue;
        }
        if (end < start) {
            continue;
        }
        if (cp >= start && cp <= end) {
            return uint16_t(firstGlyph + (cp - start)) != 0;
        }
    }
    return false;
}

} // namespace

int sfntCmapEmojiCoverage(const uint8_t *cmap, size_t size) {
    if (cmap == nullptr || size < 4) {
        return 0;
    }
    const uint32_t *battery = emojiCoverageBattery();
    const size_t batterySize = emojiCoverageBatterySize();
    uint16_t numTables = 0;
    if (!covU16(cmap, size, 2, &numTables)) {
        return 0;
    }
    int hits = 0;
    for (size_t b = 0; b < batterySize; ++b) {
        const uint32_t cp = battery[b];
        bool mapped = false;
        for (uint16_t i = 0; i < numTables && !mapped; ++i) {
            const size_t rec = 4 + size_t(i) * 8;
            if (rec + 8 > size) {
                break;
            }
            uint16_t platform = 0, encoding = 0;
            uint32_t off = 0;
            if (!covU16(cmap, size, rec, &platform) || !covU16(cmap, size, rec + 2, &encoding) ||
                !covU32(cmap, size, rec + 4, &off)) {
                continue;
            }
            if (off == 0) {
                continue;
            }
            const size_t sub = size_t(off);
            if (sub + 2 > size) {
                continue;
            }
            uint16_t fmt = 0;
            if (!covU16(cmap, size, sub, &fmt)) {
                continue;
            }
            // The BMP battery codepoints resolve through format 4; the
            // SMP ones need format 12. Either answer counts.
            if (fmt == 4) {
                mapped = covFormat4(cmap, size, sub, cp);
            } else if (fmt == 12) {
                mapped = covFormat12(cmap, size, sub, cp);
            }
        }
        if (mapped) {
            ++hits;
        }
    }
    return hits;
}

} // namespace fc
