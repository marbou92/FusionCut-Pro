#include "text.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fc {

namespace {

// ---- UTF-8 helpers ----

constexpr uint32_t kReplacement = 0xFFFDu;

// Decodes ONE codepoint starting at byte i; on error returns false and
// sets *consumed = 1 (the WHATWG maximal-subpart policy: one U+FFFD per
// invalid prefix, then decoding resumes at the byte that broke it).
bool decodeOne(const std::string &s, size_t i, uint32_t &cp, size_t &consumed) {
    const auto byte = [&s](size_t k) -> unsigned char { return static_cast<unsigned char>(s[k]); };
    if (i >= s.size()) {
        return false;
    }
    const unsigned char b0 = byte(i);
    size_t len = 0;
    uint32_t value = 0;
    if (b0 < 0x80u) {
        cp = b0;
        consumed = 1;
        return true;
    }
    if (b0 >= 0xC2u && b0 <= 0xDFu) {
        len = 2;
        value = b0 & 0x1Fu;
    } else if (b0 >= 0xE0u && b0 <= 0xEFu) {
        len = 3;
        value = b0 & 0x0Fu;
    } else if (b0 >= 0xF0u && b0 <= 0xF4u) {
        len = 4;
        value = b0 & 0x07u;
    } else {
        // 0x80..0xBF (stray continuation) and 0xF5..0xFF: invalid leads.
        cp = kReplacement;
        consumed = 1;
        return true;
    }
    for (size_t k = 1; k < len; ++k) {
        if (i + k >= s.size() || (byte(i + k) & 0xC0u) != 0x80u) {
            cp = kReplacement;
            consumed = 1; // re-examine the offending byte as a fresh lead
            return true;
        }
        value = (value << 6) | (byte(i + k) & 0x3Fu);
    }
    // Overlong encodings, surrogates, and past-Unicode values are
    // invalid (the lead-byte ranges above exclude most of these; this
    // check owns the rest: E0 80 80, F0 80 80 80, ED A0 80, F4 90+).
    if (value < (len == 2 ? 0x80u : (len == 3 ? 0x800u : 0x10000u)) ||
        (value >= 0xD800u && value <= 0xDFFFu) || value > 0x10FFFFu) {
        cp = kReplacement;
        consumed = 1;
        return true;
    }
    cp = value;
    consumed = len;
    return true;
}

void utf8EncodeOne(uint32_t cp, std::string &out) {
    if (cp < 0x80u) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

// ---- layout internals ----

struct Glyph {
    size_t shaped = 0; // index into the shaped vector
    int cp = 0;        // index into shaped[shaped].codepoints
    int advance = 0;
};

// A hard-split break is legal only where a CLUSTER starts (or the run
// carries no cluster table, in which case every codepoint is its own
// cluster). Returns the smallest index b in [idx, end) that is a legal
// break; `end` when none is. b == idx means idx itself is legal.
size_t nextClusterBoundary(const std::vector<ShapedRun> &shaped, const std::vector<Glyph> &stream,
                           size_t idx, size_t end) {
    size_t b = idx;
    while (b < end) {
        const ShapedRun &run = shaped[stream[b].shaped];
        if (run.clusterStarts.empty() || run.clusterStarts[size_t(stream[b].cp)] != 0) {
            break; // cluster boundary (or legacy run: everywhere is one)
        }
        ++b;
    }
    return b;
}

struct Line {
    std::vector<Glyph> glyphs;
    int width = 0;
    int ascent = 0;
    int height = 0;
};

int runLineHeight(const ShapedRun &run) {
    return run.ascent + run.descent + run.lineGap;
}

bool isSpace(uint32_t cp) {
    return cp == 0x20u || cp == 0x09u;
}

} // namespace

void utf8Decode(const std::string &utf8, std::vector<uint32_t> &codepoints) {
    std::vector<int> unused;
    utf8DecodeDetailed(utf8, codepoints, unused);
}

void utf8DecodeDetailed(const std::string &utf8, std::vector<uint32_t> &codepoints,
                        std::vector<int> &byteStarts) {
    codepoints.clear();
    byteStarts.clear();
    byteStarts.push_back(0);
    size_t i = 0;
    while (i < utf8.size()) {
        uint32_t cp = kReplacement;
        size_t consumed = 1;
        decodeOne(utf8, i, cp, consumed);
        codepoints.push_back(cp);
        i += consumed;
        byteStarts.push_back(static_cast<int>(i));
    }
}

void normalizeTextDocument(TextDocument &doc) {
    std::vector<TextRun> merged;
    merged.reserve(doc.runs.size());
    for (TextRun &run : doc.runs) {
        if (run.text.empty()) {
            continue;
        }
        run.style.size = std::min(std::max(run.style.size, kTextMinSize), kTextMaxSize);
        if (!merged.empty() && merged.back().style == run.style) {
            merged.back().text += run.text;
            continue;
        }
        merged.push_back(run);
    }
    doc.runs = std::move(merged);
}

std::string textPreviewLabel(const TextDocument &doc) {
    std::vector<uint32_t> cps;
    std::vector<uint32_t> one;
    for (const TextRun &run : doc.runs) {
        utf8Decode(run.text, one); // clears `one`; accumulate across runs
        cps.insert(cps.end(), one.begin(), one.end());
    }
    std::string out;
    constexpr int kMaxCodepoints = 24;
    bool skipping = true;
    int taken = 0;
    for (uint32_t cp : cps) {
        const bool whitespace = cp == 0x20u || cp == 0x09u || cp == 0x0Au || cp == 0x0Du;
        if (skipping) {
            if (whitespace) {
                continue;
            }
            skipping = false;
        }
        if (taken >= kMaxCodepoints) {
            break;
        }
        utf8EncodeOne(cp == 0x0Au ? 0x20u : cp, out);
        ++taken;
    }
    if (out.empty()) {
        return "Text";
    }
    return out;
}

TextLayout layoutText(const TextDocument &doc, const std::vector<ShapedRun> &shaped, int frameW,
                      int frameH) {
    TextLayout out;
    if (frameW <= 0 || frameH <= 0) {
        return out;
    }

    // Wrap width: frameW * box.wrap, rounded, clamped to [1, frameW].
    int wrapW = static_cast<int>(
        std::llround(static_cast<double>(frameW) * std::min(std::max(doc.box.wrap, 0.0), 1.0)));
    wrapW = std::min(std::max(wrapW, 1), frameW);
    out.wrapWidth = wrapW;

    // Flatten the shaped runs into one glyph stream (validated entries
    // only: mismatched advance/byte tables make a run unshapable; the
    // optional cluster vectors must be all-or-nothing per run).
    std::vector<Glyph> stream;
    for (size_t s = 0; s < shaped.size(); ++s) {
        const ShapedRun &run = shaped[s];
        if (run.runIndex >= doc.runs.size() || run.codepoints.empty()) {
            continue;
        }
        if (run.advances.size() != run.codepoints.size() ||
            run.byteStarts.size() != run.codepoints.size() + 1) {
            continue;
        }
        if ((!run.clusterStarts.empty() && run.clusterStarts.size() != run.codepoints.size()) ||
            (!run.emojiGlyphs.empty() && run.emojiGlyphs.size() != run.codepoints.size())) {
            continue;
        }
        for (size_t c = 0; c < run.codepoints.size(); ++c) {
            Glyph g;
            g.shaped = s;
            g.cp = static_cast<int>(c);
            g.advance = run.codepoints[c] == 0x0Au ? 0 : run.advances[c];
            stream.push_back(g);
        }
    }

    // Document-default metrics for blank lines (no glyphs to measure).
    int docAscent = 0;
    int docHeight = 0;
    for (const ShapedRun &run : shaped) {
        if (run.runIndex >= doc.runs.size()) {
            continue;
        }
        docAscent = std::max(docAscent, run.ascent);
        docHeight = std::max(docHeight, runLineHeight(run));
    }

    auto lineMetrics = [&shaped](Line &line) {
        line.ascent = 0;
        line.height = 0;
        for (const Glyph &g : line.glyphs) {
            const ShapedRun &run = shaped[g.shaped];
            line.ascent = std::max(line.ascent, run.ascent);
            line.height = std::max(line.height, runLineHeight(run));
        }
    };

    std::vector<Line> lines;
    Line cur;
    bool curHasWord = false;

    auto flushLine = [&](bool trimTrailingSpaces) {
        if (trimTrailingSpaces) {
            while (!cur.glyphs.empty() &&
                   isSpace(shaped[cur.glyphs.back().shaped]
                               .codepoints[static_cast<size_t>(cur.glyphs.back().cp)])) {
                cur.width -= cur.glyphs.back().advance;
                cur.glyphs.pop_back();
            }
        }
        if (cur.glyphs.empty()) {
            // Blank line (double newline, or an all-space line that
            // wrapped): keep it as paragraph spacing with the document
            // default metrics.
            cur.glyphs.clear();
            cur.width = 0;
            cur.ascent = docAscent;
            cur.height = docHeight;
            lines.push_back(cur);
            cur = Line();
            curHasWord = false;
            return;
        }
        lineMetrics(cur);
        lines.push_back(cur);
        cur = Line();
        curHasWord = false;
    };

    size_t i = 0;
    while (i < stream.size()) {
        const uint32_t cp = shaped[stream[i].shaped].codepoints[static_cast<size_t>(stream[i].cp)];
        if (cp == 0x0Au) {
            flushLine(true);
            ++i;
            continue;
        }
        if (isSpace(cp)) {
            cur.glyphs.push_back(stream[i]);
            cur.width += stream[i].advance;
            ++i;
            continue;
        }
        // A word: maximal run of non-space, non-newline codepoints
        // (crossing shaped-run boundaries).
        size_t j = i;
        int wordWidth = 0;
        while (j < stream.size()) {
            const uint32_t c =
                shaped[stream[j].shaped].codepoints[static_cast<size_t>(stream[j].cp)];
            if (isSpace(c) || c == 0x0Au) {
                break;
            }
            wordWidth += stream[j].advance;
            ++j;
        }
        if (curHasWord && cur.width + wordWidth > wrapW) {
            flushLine(true);
        }
        // Hard split: the word alone exceeds the wrap width. Clusters
        // (emoji sequences) are atomic: the split point moves to the
        // next cluster boundary, so an over-wide cluster renders alone
        // and overflows, exactly like a single over-wide codepoint.
        while (wordWidth > wrapW && j - i > 1) {
            int piece = 0;
            size_t k = i;
            while (k < j) {
                const int next = piece + stream[k].advance;
                if (k > i && next > wrapW) {
                    break;
                }
                piece = next;
                ++k;
            }
            if (k < j) {
                const size_t k0 = k;
                k = nextClusterBoundary(shaped, stream, k, j);
                if (k != k0) {
                    // include the cluster extension in the piece sum so
                    // the wordWidth bookkeeping below stays exact
                    piece = 0;
                    for (size_t g = i; g < k; ++g) {
                        piece += stream[g].advance;
                    }
                }
            }
            for (size_t g = i; g < k; ++g) {
                cur.glyphs.push_back(stream[g]);
                cur.width += stream[g].advance;
            }
            curHasWord = true;
            flushLine(false);
            i = k;
            wordWidth -= piece;
        }
        for (size_t g = i; g < j; ++g) {
            cur.glyphs.push_back(stream[g]);
            cur.width += stream[g].advance;
        }
        curHasWord = true;
        i = j;
    }
    // Flush the last line. cur is empty here only when nothing is
    // pending: after a trailing '\n' (the flush already happened) or
    // when a hard split consumed the whole word down to its last
    // cluster boundary (words without cluster annotations always leave
    // glyphs in cur; a fully-consumed split is new with cluster
    // extension) - a phantom blank line must not appear in either case.
    if (!cur.glyphs.empty()) {
        flushLine(true);
    }

    out.lineCount = static_cast<int>(lines.size());
    if (lines.empty()) {
        return out;
    }

    // Block extents.
    for (const Line &line : lines) {
        out.textWidth = std::max(out.textWidth, line.width);
        out.textHeight += line.height;
    }

    const int padding = doc.box.background ? std::max(2, static_cast<int>(frameH / 60)) : 0;
    const int outerW = out.textWidth + 2 * padding;
    const int outerH = out.textHeight + 2 * padding;

    const int cx = static_cast<int>(std::llround(static_cast<double>(frameW) * doc.box.anchorX));
    const int cy = static_cast<int>(std::llround(static_cast<double>(frameH) * doc.box.anchorY));
    const int boxX = cx - outerW / 2;
    const int boxY = cy - outerH / 2;
    out.bgX = doc.box.background ? boxX : 0;
    out.bgY = doc.box.background ? boxY : 0;
    out.bgW = doc.box.background ? outerW : 0;
    out.bgH = doc.box.background ? outerH : 0;

    const int textX0 = boxX + padding;
    const int textY0 = boxY + padding;

    // Slices: coalesce consecutive same-run glyphs on a line into one
    // draw call; place each line per the document alignment.
    int lineTop = textY0;
    for (const Line &line : lines) {
        const int baseline = lineTop + line.ascent;
        int offset = 0;
        switch (doc.align) {
        case TextAlign::Center:
            offset = (out.textWidth - line.width) / 2;
            break;
        case TextAlign::Right:
            offset = out.textWidth - line.width;
            break;
        case TextAlign::Left:
            offset = 0;
            break;
        }
        int x = textX0 + offset;
        size_t g = 0;
        while (g < line.glyphs.size()) {
            const size_t runIdx = line.glyphs[g].shaped;
            const ShapedRun &run = shaped[runIdx];
            size_t k = g;
            while (k < line.glyphs.size() && line.glyphs[k].shaped == runIdx) {
                ++k;
            }
            const int cpStart = line.glyphs[g].cp;
            const int cpCount = static_cast<int>(k - g);
            LaidOutSlice slice;
            slice.runIndex = runIdx;
            slice.cpStart = cpStart;
            slice.cpCount = cpCount;
            slice.x = x;
            slice.baselineY = baseline;
            slice.byteStart = run.byteStarts[static_cast<size_t>(cpStart)];
            slice.byteLen = run.byteStarts[static_cast<size_t>(cpStart + cpCount)] -
                            run.byteStarts[static_cast<size_t>(cpStart)];
            out.slices.push_back(slice);
            for (size_t q = g; q < k; ++q) {
                x += line.glyphs[q].advance;
            }
            g = k;
        }
        lineTop += line.height;
    }
    return out;
}

void compositeOver(uint8_t *dst, const uint8_t *src, int width, int height) {
    if (!dst || !src || width <= 0 || height <= 0) {
        return;
    }
    const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    for (size_t p = 0; p < pixels; ++p) {
        uint8_t *d = dst + p * 4;
        const uint8_t *s = src + p * 4;
        const int sa = s[3];
        if (sa == 0) {
            continue;
        }
        if (sa == 255) {
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = s[3];
            continue;
        }
        const int da = d[3];
        // Rounded source-over alpha; the numerator is bounded by
        // 255*255 + 127, so the quotient never exceeds 255.
        const int outA = (sa * 255 + da * (255 - sa) + 127) / 255;
        if (outA <= 0) {
            continue;
        }
        const int denom = 255 * outA;
        const int half = denom / 2;
        for (int c = 0; c < 3; ++c) {
            const int num = s[c] * sa * 255 + d[c] * da * (255 - sa);
            d[c] = static_cast<uint8_t>((num + half) / denom);
        }
        d[3] = static_cast<uint8_t>(outA);
    }
}

} // namespace fc
