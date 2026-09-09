#include "text_renderer.h"

#include <QFile>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPen>
#include <QTransform>

#include <algorithm>
#include <cmath>

#include "emoji_clusters.h"

namespace fc {

int textPixelSizeClamped(const TextStyle &style) {
    return std::min(std::max(style.size, kTextMinSize), kTextMaxSize);
}

namespace {

QColor toQColor(uint32_t rgba) {
    return QColor(textRed(rgba), textGreen(rgba), textBlue(rgba), textAlpha(rgba));
}

// The QFont for one styled run. family "" = the application default;
// any family the user picks is used verbatim and Qt's font fallback
// handles codepoints it does not cover.
QFont fontForStyle(const TextStyle &style) {
    QFont font;
    if (!style.family.empty()) {
        font.setFamily(QString::fromStdString(style.family));
    }
    font.setPixelSize(textPixelSizeClamped(style));
    font.setBold(style.bold);
    font.setItalic(style.italic);
    font.setUnderline(style.underline);
    return font;
}

// One codepoint as a QString (surrogate pair for astral planes).
QString charAsQString(uint32_t cp) {
    if (cp <= 0xFFFFu) {
        return QString(QChar(static_cast<ushort>(cp)));
    }
    QString out;
    const uint32_t v = cp - 0x10000u;
    out.append(QChar(static_cast<ushort>(0xD800u + (v >> 10))));
    out.append(QChar(static_cast<ushort>(0xDC00u + (v & 0x3FF))));
    return out;
}

// A codepoint range (one cluster) as one QString.
QString cpsAsQString(const uint32_t *cps, size_t count) {
    QString out;
    out.reserve(int(count));
    for (size_t i = 0; i < count; ++i) {
        out += charAsQString(cps[i]);
    }
    return out;
}

// Folds a string's tight ink rect into the run metrics so bitmap emoji
// (which routinely paint taller than the text font's line box) do not
// overlap the previous line. Qt's tightBoundingRect measures through
// the same shaped, fallback-aware path as drawText.
void foldInkExtents(const QFontMetrics &metrics, const QString &s, int &ascent, int &descent) {
    const QRect r = metrics.tightBoundingRect(s);
    if (r.height() <= 0) {
        return; // no ink (spaces, null rect)
    }
    if (r.top() < 0) {
        ascent = std::max(ascent, -r.top());
    }
    if (r.bottom() >= 0) {
        descent = std::max(descent, r.bottom() + 1);
    }
}

// Folds one emoji bitmap's extents (strike pixels) into the run
// metrics, scaled to the text pixel size. `imgH` is the bitmap height
// (the CBDT record carries it; sbix extents come from the decoded
// image). Top bearing UP from the baseline; bottom reach below it.
void foldEmojiExtents(const EmojiFont::Bitmap &bm, int imgH, int size, int &ascent, int &descent) {
    const int top = bm.format == 0 ? bm.metrics.bearingY : bm.originY + imgH;
    const int bottom = bm.format == 0 ? bm.metrics.height - bm.metrics.bearingY : -bm.originY;
    ascent = std::max(ascent, emojiScaleStrike(top, size, bm.ppem));
    descent = std::max(descent, emojiScaleStrike(bottom, size, bm.ppem));
}

// The wipe rectangle: the visible span of the block at fraction `wipe`
// along `dir` (the edge the reveal starts at - see text.h).
QRect wipeRect(const TextLayout &layout, TextAnimDir dir, double wipe) {
    const int w = std::max(1, layout.blockW);
    const int h = std::max(1, layout.blockH);
    const int wx = static_cast<int>(std::llround(wipe * static_cast<double>(w)));
    const int wy = static_cast<int>(std::llround(wipe * static_cast<double>(h)));
    switch (dir) {
    case TextAnimDir::Left:
        return QRect(layout.blockX, layout.blockY, std::max(0, wx), h);
    case TextAnimDir::Right:
        return QRect(layout.blockX + w - std::max(0, wx), layout.blockY, std::max(0, wx), h);
    case TextAnimDir::Up: // reveal starts at the bottom edge, grows up
        return QRect(layout.blockX, layout.blockY + h - std::max(0, wy), w, std::max(0, wy));
    case TextAnimDir::Down:
        return QRect(layout.blockX, layout.blockY, w, std::max(0, wy));
    }
    return QRect(layout.blockX, layout.blockY, w, h);
}

// Erases (alpha = 0) every pixel OUTSIDE `keep` on an RGBA8888 image.
void eraseOutsideRect(QImage &img, const QRect &keep) {
    const QRect image(0, 0, img.width(), img.height());
    const QRect inside = keep.intersected(image);
    for (int y = image.top(); y <= image.bottom(); ++y) {
        uint8_t *line = img.scanLine(y);
        const bool rowInside = y >= inside.top() && y <= inside.bottom();
        for (int x = image.left(); x <= image.right(); ++x) {
            if (!rowInside || x < inside.left() || x > inside.right()) {
                line[x * 4 + 3] = 0;
            }
        }
    }
}

// Multiplies every pixel's alpha by `factor` (rounded).
void multiplyAlpha(QImage &img, double factor) {
    const int num = static_cast<int>(std::llround(factor * 256.0));
    if (num >= 256) {
        return;
    }
    const int k = std::max(0, num);
    for (int y = 0; y < img.height(); ++y) {
        uint8_t *line = img.scanLine(y);
        for (int x = 0; x < img.width(); ++x) {
            line[x * 4 + 3] = static_cast<uint8_t>((line[x * 4 + 3] * k + 128) / 256);
        }
    }
}

// Is this codepoint an invisible cluster joiner/selector (never any
// ink of its own)?
bool isJoinerOrSelector(uint32_t cp) {
    return cp == kEmojiVS16 || cp == kEmojiVS15 || cp == kZeroWidthJoiner || cp == kCombiningKeycap;
}

// Marks the cluster [pos, pos + len) with emoji-font bitmaps and
// advances; returns true when at least one bitmap will paint (the
// caller then skips the platform-text path for this cluster).
//
//  * A resolved cluster (GSUB ligature or emoji-presentation single,
//    per the font's own rules - resolveCluster) paints ONE bitmap at
//    its head with the scaled strike advance; the font may consume
//    fewer codepoints than the Unicode policy clustered (covered),
//    and any LEFTOVER members fall through to per-codepoint bitmaps.
//  * An unresolved cluster tries each member codepoint's own bitmap
//    (joiners/selectors paint nothing, no width); this is what an
//    sbix font without GSUB ligatures gets - a family sequence shows
//    its member emoji side by side, never half a flag.
bool markBitmapCluster(const EmojiFont *ef, EmojiPainter *painter, ShapedRun &s, size_t pos,
                       int len, int size, int &ascent, int &descent) {
    const size_t n = s.codepoints.size();
    EmojiFont::Resolved resolved;
    size_t covered = 0;
    bool any = false;
    if (ef->resolveCluster(s.codepoints.data(), n, pos, &resolved) && resolved.codepoints >= 1) {
        EmojiFont::Bitmap bm;
        if (ef->bitmapFor(resolved.glyph, &bm)) {
            const QImage *img = bm.format != 0 ? painter->image(resolved.glyph) : nullptr;
            const int imgH = bm.format != 0 ? (img ? img->height() : 0) : bm.metrics.height;
            s.emojiGlyphs[pos] = resolved.glyph;
            s.advances[pos] = emojiScaleStrike(bm.metrics.advance, size, bm.ppem);
            foldEmojiExtents(bm, imgH, size, ascent, descent);
            covered = std::min<size_t>(size_t(resolved.codepoints), size_t(len));
            any = true;
        }
    }
    // Invisible followers up to `covered`, per-codepoint bitmaps after.
    for (int k = 0; k < len; ++k) {
        const size_t idx = pos + size_t(k);
        if (k > 0) {
            s.clusterStarts[idx] = 0; // continuation of the cluster
        }
        if (size_t(k) < covered) {
            if (idx != pos) {
                s.advances[idx] = 0; // the head's advance spans them
            }
            continue;
        }
        const uint32_t cp = s.codepoints[idx];
        if (isJoinerOrSelector(cp)) {
            s.advances[idx] = 0; // no ink, no width
            continue;
        }
        const uint16_t gid = ef->codepointGlyph(cp);
        EmojiFont::Bitmap bm;
        if (gid != 0 && ef->bitmapFor(gid, &bm)) {
            const QImage *img = bm.format != 0 ? painter->image(gid) : nullptr;
            const int imgH = bm.format != 0 ? (img ? img->height() : 0) : bm.metrics.height;
            s.emojiGlyphs[idx] = gid;
            s.advances[idx] = emojiScaleStrike(bm.metrics.advance, size, bm.ppem);
            foldEmojiExtents(bm, imgH, size, ascent, descent);
            any = true;
        } else {
            s.advances[idx] = 0; // no bitmap: no ink from the emoji font
        }
    }
    return any;
}

} // namespace

std::vector<ShapedRun> shapeTextQt(const TextDocument &doc, EmojiPainter *emoji) {
    std::vector<ShapedRun> shaped;
    shaped.reserve(doc.runs.size());
    const EmojiFont *ef = (emoji && emoji->loaded()) ? emoji->font() : nullptr;
    for (size_t i = 0; i < doc.runs.size(); ++i) {
        const TextRun &run = doc.runs[i];
        if (run.text.empty()) {
            continue;
        }
        ShapedRun s;
        s.runIndex = i;
        utf8DecodeDetailed(run.text, s.codepoints, s.byteStarts);
        const size_t n = s.codepoints.size();
        s.advances.assign(n, 0);
        s.clusterStarts.assign(n, 1);
        if (ef) {
            s.emojiGlyphs.assign(n, 0);
        }
        const QFont font = fontForStyle(run.style);
        const QFontMetrics metrics(font);
        const int size = textPixelSizeClamped(run.style);
        int ascent = metrics.ascent();
        int descent = metrics.descent();
        size_t c = 0;
        while (c < n) {
            const uint32_t cp = s.codepoints[c];
            if (cp == 0x0Au) {
                s.advances[c] = 0;
                ++c;
                continue;
            }
            const int cluster = emojiClusterLength(s.codepoints.data(), n, c);
            if (cluster >= 2 || (cluster == 1 && ef != nullptr)) {
                const int len = cluster >= 2 ? cluster : 1;
                if (ef != nullptr &&
                    markBitmapCluster(ef, emoji, s, c, len, size, ascent, descent)) {
                    c += size_t(len);
                    continue;
                }
                // Platform path: the cluster (or single emoji) is one
                // shaped string through the platform font stack - the
                // head carries its advance, the layout stays atomic,
                // and the renderer draws it as one drawText call.
                const QString str = cpsAsQString(s.codepoints.data() + c, size_t(len));
                s.advances[c] = metrics.horizontalAdvance(str);
                for (int k = 1; k < len; ++k) {
                    s.advances[c + size_t(k)] = 0;
                    s.clusterStarts[c + size_t(k)] = 0;
                }
                foldInkExtents(metrics, str, ascent, descent);
                c += size_t(len);
                continue;
            }
            // Plain text: per-codepoint advance from the resolved font
            // stack (the pre-emoji behavior).
            const QString str = charAsQString(cp);
            s.advances[c] = metrics.horizontalAdvance(str);
            if (cluster == 1) {
                foldInkExtents(metrics, str, ascent, descent);
            }
            ++c;
        }
        s.ascent = ascent;
        s.descent = descent;
        s.lineGap = metrics.leading();
        shaped.push_back(std::move(s));
    }
    return shaped;
}

QImage renderTextLayer(const TextDocument &doc, int width, int height, int64_t clipFrame,
                       int64_t clipDuration, EmojiPainter *emoji) {
    QImage image(std::max(width, 1), std::max(height, 1), QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    if (width <= 0 || height <= 0 || doc.runs.empty()) {
        return image;
    }

    // Animation state: clipFrame < 0 (or a static document) renders the
    // settled state. The typewriter reveal truncates the DOCUMENT
    // before layout, so wrapping, the background box, and the block
    // rect all match the visible text exactly.
    TextAnimState anim;
    const bool animated = clipFrame >= 0 && clipDuration > 0 && hasTextAnimation(doc.animation);
    TextDocument work = doc;
    if (animated) {
        const int64_t total = countTextCodepoints(doc);
        anim = textAnimationAt(doc.animation, clipFrame, clipDuration, total);
        if (anim.revealCodepoints >= 0 && anim.revealCodepoints < total) {
            truncateTextDocument(doc, anim.revealCodepoints, work);
        }
    }

    const std::vector<ShapedRun> shaped = shapeTextQt(work, emoji);
    const TextLayout layout = layoutText(work, shaped, width, height);
    if (layout.lineCount == 0) {
        return image;
    }

    QPainter painter(&image);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    if (work.box.background && layout.bgW > 0 && layout.bgH > 0) {
        painter.fillRect(QRect(layout.bgX, layout.bgY, layout.bgW, layout.bgH),
                         toQColor(work.box.backgroundRgba));
    }

    // One font per document run (built once, reused by its slices).
    std::vector<QFont> fonts;
    std::vector<QColor> colors;
    std::vector<int> sizes;
    fonts.reserve(work.runs.size());
    colors.reserve(work.runs.size());
    sizes.reserve(work.runs.size());
    for (const TextRun &run : work.runs) {
        fonts.push_back(fontForStyle(run.style));
        colors.push_back(toQColor(run.style.colorRgba));
        sizes.push_back(textPixelSizeClamped(run.style));
    }

    // Paints one emoji bitmap at (penX, baselineY): the strike metrics
    // scaled to the run's pixel size, image TOP-LEFT at the scaled
    // bearing (baseline - top bearing, pen + left bearing).
    auto drawBitmap = [&](uint16_t glyph, int penX, int baselineY, int size) {
        if (!emoji || !emoji->loaded()) {
            return;
        }
        EmojiFont::Bitmap bm;
        if (!emoji->font()->bitmapFor(glyph, &bm)) {
            return;
        }
        const QImage *img = emoji->scaledImage(glyph, size);
        if (img == nullptr || img->isNull()) {
            return; // no decodable image (unknown format, plugin gone)
        }
        const int imgH = bm.format == 0 ? bm.metrics.height : img->height();
        const int top = bm.format == 0 ? bm.metrics.bearingY : bm.originY + imgH;
        const int dx = emojiScaleStrike(bm.metrics.bearingX, size, bm.ppem);
        const int dy = -emojiScaleStrike(top, size, bm.ppem);
        painter.drawImage(QPointF(penX + dx, baselineY + dy), *img);
    };

    for (const LaidOutSlice &slice : layout.slices) {
        if (slice.runIndex >= shaped.size() || slice.cpCount <= 0 || slice.byteLen <= 0) {
            continue;
        }
        const ShapedRun &run = shaped[slice.runIndex];
        if (run.runIndex >= work.runs.size()) {
            continue;
        }
        const std::string &text = work.runs[run.runIndex].text;
        const int size = sizes[run.runIndex];
        const int n = static_cast<int>(run.codepoints.size());
        const bool hasTable = !run.clusterStarts.empty();
        const bool hasEmoji = !run.emojiGlyphs.empty();
        painter.setFont(fonts[run.runIndex]);
        painter.setPen(QPen(colors[run.runIndex], 1));

        // Walk the slice: contiguous plain-text codepoints coalesce
        // into one drawText call at their start position; a codepoint
        // marked in emojiGlyphs paints its bitmap at the pen; a
        // continuation codepoint (clusterStarts 0) paints nothing
        // (its head already covered it); a multi-codepoint cluster
        // with NO bitmaps anywhere draws as ONE string so the
        // platform shaper can ligate the sequence.
        int pen = slice.x;
        int segStart = -1; // slice-relative codepoint index of the open text segment
        int segPen = 0;
        auto flushSegment = [&](int segEnd) {
            if (segStart < 0 || segEnd <= segStart) {
                return;
            }
            const int cp0 = slice.cpStart + segStart;
            const int cp1 = slice.cpStart + segEnd;
            const int byte0 = run.byteStarts[size_t(cp0)];
            const int byte1 = run.byteStarts[size_t(cp1)];
            const QString piece = QString::fromUtf8(text.data() + byte0, byte1 - byte0);
            painter.drawText(QPointF(segPen, slice.baselineY), piece);
            segStart = -1;
        };
        int c = 0;
        while (c < slice.cpCount) {
            const int cp = slice.cpStart + c;
            const uint16_t eg = hasEmoji ? run.emojiGlyphs[size_t(cp)] : 0;
            if (eg != 0) {
                flushSegment(c);
                drawBitmap(eg, pen, slice.baselineY, size);
                pen += run.advances[size_t(cp)];
                ++c;
                continue;
            }
            const bool continuation = hasTable && run.clusterStarts[size_t(cp)] == 0;
            if (continuation) {
                // covered by the head's bitmap or platform string
                pen += run.advances[size_t(cp)];
                ++c;
                continue;
            }
            // A head (or standalone) codepoint with no bitmap. A whole
            // cluster with no bitmaps draws as one string; otherwise
            // this codepoint joins the running text segment.
            int e = cp + 1;
            if (hasTable) {
                while (e < n && run.clusterStarts[size_t(e)] == 0) {
                    ++e;
                }
            }
            if (e > cp + 1) {
                bool anyEg = false;
                for (int k = cp; k < e; ++k) {
                    anyEg = anyEg || (hasEmoji && run.emojiGlyphs[size_t(k)] != 0);
                }
                if (!anyEg) {
                    flushSegment(c);
                    const int byte0 = run.byteStarts[size_t(cp)];
                    const int byte1 = run.byteStarts[size_t(e)];
                    const QString piece = QString::fromUtf8(text.data() + byte0, byte1 - byte0);
                    painter.drawText(QPointF(pen, slice.baselineY), piece);
                    for (int k = cp; k < e; ++k) {
                        pen += run.advances[size_t(k)];
                    }
                    c += e - cp;
                    continue;
                }
            }
            if (segStart < 0) {
                segStart = c;
                segPen = pen;
            }
            pen += run.advances[size_t(cp)];
            ++c;
        }
        flushSegment(slice.cpCount);
    }
    painter.end();

    if (animated) {
        // Wipe: erase everything outside the revealed span (the whole
        // layer - background and text together).
        if (anim.wipe < 1.0) {
            eraseOutsideRect(image, wipeRect(layout, doc.animation.dir, anim.wipe));
        }
        // Scale about the block center, then slide by the animation
        // offset (fractions of the frame size). One resampling pass.
        const double dx = std::llround(anim.offsetX * static_cast<double>(width));
        const double dy = std::llround(anim.offsetY * static_cast<double>(height));
        if (std::abs(anim.scale - 1.0) > 1.0e-12 || dx != 0.0 || dy != 0.0) {
            QImage out(image.width(), image.height(), QImage::Format_RGBA8888);
            out.fill(Qt::transparent);
            QPainter p(&out);
            p.setRenderHint(QPainter::SmoothPixmapTransform, true);
            const double cx = static_cast<double>(layout.blockX + layout.blockW / 2);
            const double cy = static_cast<double>(layout.blockY + layout.blockH / 2);
            QTransform t;
            t.translate(dx, dy);
            t.translate(cx, cy);
            t.scale(anim.scale, anim.scale);
            t.translate(-cx, -cy);
            p.setTransform(t);
            p.drawImage(0, 0, image);
            p.end();
            image = out;
        }
        // Global alpha last (a cheaper pass; also dims the wiped edge
        // correctly since wipe runs before it).
        if (anim.alpha < 1.0) {
            multiplyAlpha(image, anim.alpha);
        }
    }
    return image;
}

// ---------------------------------------------------------------------------
// EmojiPainter
// ---------------------------------------------------------------------------

bool EmojiPainter::load(const QString &path) {
    bytes_.clear();
    raw_.clear();
    scaled_.clear();
    path_ = QString();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray blob = file.readAll();
    if (blob.isEmpty()) {
        return false;
    }
    bytes_.assign(blob.constData(), blob.constData() + blob.size());
    if (!font_.load(bytes_.data(), bytes_.size())) {
        bytes_.clear();
        return false;
    }
    path_ = path;
    return true;
}

const QImage *EmojiPainter::image(uint16_t glyph) {
    const auto it = raw_.find(glyph);
    if (it != raw_.end()) {
        return it->second.isNull() ? nullptr : &it->second;
    }
    EmojiFont::Bitmap bm;
    if (!font_.bitmapFor(glyph, &bm)) {
        raw_[glyph] = QImage(); // remember "no bitmap" too
        return nullptr;
    }
    QImage img = QImage::fromData(bm.data, int(bm.dataLen));
    if (!img.isNull() && bm.mirror) {
        img = img.mirrored(true, false); // sbix 'flip'
    }
    // Bound the cache: a full Apple Color Emoji face has ~3000 glyphs
    // of ~100 KB images; a document never shows that many at once.
    // Crude but bounded: past the cap, drop everything (re-decodes on
    // demand; rendering stays correct, just colder).
    if (raw_.size() >= 256) {
        raw_.clear();
    }
    raw_[glyph] = img;
    return img.isNull() ? nullptr : &raw_[glyph];
}

const QImage *EmojiPainter::scaledImage(uint16_t glyph, int sizePx) {
    const auto key = std::make_pair(glyph, sizePx);
    const auto it = scaled_.find(key);
    if (it != scaled_.end()) {
        return it->second.isNull() ? nullptr : &it->second;
    }
    EmojiFont::Bitmap bm;
    if (!font_.bitmapFor(glyph, &bm)) {
        scaled_[key] = QImage();
        return nullptr;
    }
    const QImage *raw = image(glyph);
    if (raw == nullptr) {
        scaled_[key] = QImage();
        return nullptr;
    }
    const int w = bm.format == 0 ? bm.metrics.width : raw->width();
    const int h = bm.format == 0 ? bm.metrics.height : raw->height();
    const int sw = std::max(1, emojiScaleStrike(w, sizePx, bm.ppem));
    const int sh = std::max(1, emojiScaleStrike(h, sizePx, bm.ppem));
    QImage scaled;
    if (raw->width() == sw && raw->height() == sh) {
        scaled = *raw;
    } else {
        scaled = raw->scaled(sw, sh, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    if (scaled_.size() >= 256) {
        scaled_.clear();
    }
    scaled_[key] = scaled;
    return scaled.isNull() ? nullptr : &scaled_[key];
}

} // namespace fc
