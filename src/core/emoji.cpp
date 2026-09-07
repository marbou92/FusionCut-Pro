#include "emoji.h"

#include <algorithm>
#include <cstring>

namespace fc {

namespace {

// The PNG signature: every CBDT record we accept must start with it
// (image formats 17/18/19 wrap PNG payloads in the bitmap color fonts).
// The check converts table corruption into "missing glyph" instead of
// garbage pixels, and it is what disambiguates the "first glyph sits
// exactly at sbitOffset" convention of index subtable format 1.
constexpr uint8_t kPngMagic[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

bool isVariationSelector(uint32_t cp) {
    return cp == kEmojiVS16 || cp == kEmojiVS15;
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
    if (!rdU16(4, &numTables)) {
        return nullptr;
    }
    for (uint16_t i = 0; i < numTables; ++i) {
        const size_t rec = 12 + size_t(i) * 16;
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

// ---- maxp ----

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

// ---- load ----

bool EmojiFont::load(const uint8_t *data, size_t size) {
    loaded_ = false;
    strikes_.clear();
    cmapRanges_.clear();
    cmapUnsorted_ = false;
    hasCmap4_ = false;
    cmap4Off_ = 0;
    variants_.clear();
    ligatures_.clear();
    maxLigatureLen_ = 0;
    numGlyphs_ = 0;
    data_ = data;
    size_ = size;
    if (!data || size < 12) {
        return false;
    }
    uint16_t numTables = 0;
    if (!rdU16(4, &numTables) || numTables == 0 || 12 + size_t(numTables) * 16 > size) {
        return false;
    }
    if (!parseCbloc() || !parseCmap()) {
        return false;
    }
    parseGsub();
    parseMaxp();
    loaded_ = true;
    return true;
}

// ---- strikes / bitmaps ----

int EmojiFont::strikePpem() const {
    int best = 0;
    for (const Strike &s : strikes_) {
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
    if (!loaded_ || !out || glyph == 0 || glyph >= numGlyphs_) {
        return false;
    }
    // Largest strike wins (deterministic; the bundled font has one).
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
    out->png = data_ + pngStart;
    out->pngLen = size_t(pngLen);
    return true;
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
    // A format-14 mapping for base+VS16 (not used by the bundled font,
    // but it is the standard mechanism when present).
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

// ---- presentation policy ----

bool EmojiFont::isDefaultEmojiPresentation(uint32_t cp) {
    // The SMP emoji blocks: single glyphs there default to emoji
    // presentation (includes lone regional indicators - the font has
    // letter-box glyphs for them, which beats tofu).
    if (cp >= 0x1F000u && cp <= 0x1FAFFu) {
        return true;
    }
    // The stable BMP set with default emoji presentation (the
    // Emoji_Presentation=Yes ranges; stable across Unicode versions).
    struct Range {
        uint32_t lo, hi;
    };
    static const Range kRanges[] = {
        {0x231A, 0x231B}, {0x23E9, 0x23EC}, {0x23F0, 0x23F0}, {0x23F3, 0x23F3}, {0x25FD, 0x25FE},
        {0x2614, 0x2615}, {0x2648, 0x2653}, {0x267F, 0x267F}, {0x2693, 0x2693}, {0x26A1, 0x26A1},
        {0x26AA, 0x26AB}, {0x26BD, 0x26BE}, {0x26C4, 0x26C5}, {0x26CE, 0x26CE}, {0x26D4, 0x26D4},
        {0x26EA, 0x26EA}, {0x26F2, 0x26F3}, {0x26F5, 0x26F5}, {0x26FA, 0x26FA}, {0x26FD, 0x26FD},
        {0x2705, 0x2705}, {0x270A, 0x270B}, {0x2728, 0x2728}, {0x274C, 0x274C}, {0x274E, 0x274E},
        {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2795, 0x2797}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF},
        {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55},
    };
    for (const Range &r : kRanges) {
        if (cp >= r.lo && cp <= r.hi) {
            return true;
        }
    }
    return false;
}

} // namespace fc
