#include "text_renderer.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include <algorithm>

namespace fc {

int textPixelSizeClamped(const TextStyle &style) {
    return std::min(std::max(style.size, kTextMinSize), kTextMaxSize);
}

namespace {

QColor toQColor(uint32_t rgba) {
    return QColor(textRed(rgba), textGreen(rgba), textBlue(rgba), textAlpha(rgba));
}

// The QFont for one styled run. family "" = the application default.
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
    out.append(QChar(static_cast<ushort>(0xDC00u + (v & 0x3FFu))));
    return out;
}

} // namespace

std::vector<ShapedRun> shapeTextQt(const TextDocument &doc, const EmojiFont *emoji) {
    std::vector<ShapedRun> shaped;
    shaped.reserve(doc.runs.size());
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
        s.emojiGlyphs.assign(n, 0);
        const QFont font = fontForStyle(run.style);
        const QFontMetrics metrics(font);
        const int sizePx = textPixelSizeClamped(run.style);
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
            EmojiFont::Resolved resolved;
            if (emoji != nullptr && emoji->loaded() &&
                emoji->resolveCluster(s.codepoints.data(), n, c, &resolved)) {
                EmojiFont::Bitmap bm;
                if (emoji->bitmapFor(resolved.glyph, &bm)) {
                    // Cluster: bitmap advance on the head, 0 on the
                    // continuation codepoints; emoji extents fold into
                    // the run metrics so mixed lines grow tall enough.
                    s.advances[c] = emojiScaleStrike(bm.metrics.advance, sizePx, bm.ppem);
                    for (int k = 1; k < resolved.codepoints; ++k) {
                        s.advances[c + size_t(k)] = 0;
                        s.clusterStarts[c + size_t(k)] = 0;
                    }
                    s.emojiGlyphs[c] = resolved.glyph;
                    ascent =
                        std::max(ascent, emojiScaleStrike(bm.metrics.bearingY, sizePx, bm.ppem));
                    descent =
                        std::max(descent, emojiScaleStrike(bm.metrics.height - bm.metrics.bearingY,
                                                           sizePx, bm.ppem));
                    c += size_t(resolved.codepoints);
                    continue;
                }
            }
            // Plain text codepoint (also the fallback when the resolved
            // glyph has no bitmap): advance from the text font.
            s.advances[c] = metrics.horizontalAdvance(charAsQString(cp));
            ++c;
        }
        s.ascent = ascent;
        s.descent = descent;
        s.lineGap = metrics.leading();
        shaped.push_back(std::move(s));
    }
    return shaped;
}

QImage renderTextLayer(const TextDocument &doc, int width, int height, EmojiPainter *emoji) {
    QImage image(std::max(width, 1), std::max(height, 1), QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    if (width <= 0 || height <= 0 || doc.runs.empty()) {
        return image;
    }

    const std::vector<ShapedRun> shaped = shapeTextQt(doc, emoji ? emoji->font() : nullptr);
    const TextLayout layout = layoutText(doc, shaped, width, height);
    if (layout.lineCount == 0) {
        return image;
    }

    QPainter painter(&image);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (doc.box.background && layout.bgW > 0 && layout.bgH > 0) {
        painter.fillRect(QRect(layout.bgX, layout.bgY, layout.bgW, layout.bgH),
                         toQColor(doc.box.backgroundRgba));
    }

    // One font per document run (built once, reused by its slices).
    std::vector<QFont> fonts;
    std::vector<QColor> colors;
    fonts.reserve(doc.runs.size());
    colors.reserve(doc.runs.size());
    for (const TextRun &run : doc.runs) {
        fonts.push_back(fontForStyle(run.style));
        colors.push_back(toQColor(run.style.colorRgba));
    }

    for (const LaidOutSlice &slice : layout.slices) {
        const ShapedRun &run = shaped[slice.runIndex];
        if (run.runIndex >= doc.runs.size() || slice.cpCount <= 0 || slice.byteLen <= 0) {
            continue;
        }
        const std::string &text = doc.runs[run.runIndex].text;
        const int sizePx = textPixelSizeClamped(doc.runs[run.runIndex].style);
        painter.setFont(fonts[run.runIndex]);
        painter.setPen(QPen(colors[run.runIndex], 1));

        // Walk the slice codepoint by codepoint: contiguous plain-text
        // codepoints coalesce into one drawText call at their start
        // position; emoji cluster heads draw the bitmap instead. The
        // pen advance comes from ShapedRun::advances (cluster heads
        // carry the whole cluster's scaled advance).
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
        for (int c = 0; c < slice.cpCount; ++c) {
            const int cp = slice.cpStart + c;
            const uint16_t emojiGlyph =
                (emoji != nullptr && !run.emojiGlyphs.empty()) ? run.emojiGlyphs[size_t(cp)] : 0;
            if (emojiGlyph != 0) {
                flushSegment(c);
                const QImage bm = emoji->bitmap(emojiGlyph, sizePx);
                EmojiFont::Bitmap info;
                if (!bm.isNull() && emoji->font()->bitmapFor(emojiGlyph, &info)) {
                    const int bx = emojiScaleStrike(info.metrics.bearingX, sizePx, info.ppem);
                    const int by = emojiScaleStrike(info.metrics.bearingY, sizePx, info.ppem);
                    painter.drawImage(QPoint(pen + bx, slice.baselineY - by), bm);
                }
            } else if (segStart < 0) {
                segStart = c;
                segPen = pen;
            }
            pen += run.advances[size_t(cp)];
        }
        flushSegment(slice.cpCount);
    }
    return image;
}

// ---- EmojiPainter ----

bool EmojiPainter::load(const QString &path) {
    cache_.clear();
    font_ = EmojiFont();
    bytes_.clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray raw = file.readAll();
    if (raw.isEmpty()) {
        return false;
    }
    bytes_.assign(raw.constData(), raw.constData() + raw.size());
    if (!font_.load(bytes_.data(), bytes_.size())) {
        bytes_.clear();
        font_ = EmojiFont();
        return false;
    }
    return true;
}

QImage EmojiPainter::bitmap(uint16_t glyph, int sizePx) {
    if (!font_.loaded()) {
        return QImage();
    }
    const auto key = std::make_pair(glyph, sizePx);
    const auto it = cache_.find(key);
    if (it != cache_.end()) {
        return it->second;
    }
    EmojiFont::Bitmap bm;
    if (!font_.bitmapFor(glyph, &bm)) {
        return QImage();
    }
    // Decoded PNGs are ~136x128x4 = 68 KB each; the cap bounds the
    // cache at roughly 16 MB before a wholesale clear (simple and
    // ample for a text clip's distinct emoji + sizes).
    if (cache_.size() >= 256) {
        cache_.clear();
    }
    QImage img;
    if (!img.loadFromData(bm.png, int(bm.pngLen), "PNG")) {
        return QImage();
    }
    const int w = emojiScaleStrike(bm.metrics.width, sizePx, bm.ppem);
    const int h = emojiScaleStrike(bm.metrics.height, sizePx, bm.ppem);
    if (w > 0 && h > 0 && (img.width() != w || img.height() != h)) {
        img = img.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    cache_[key] = img;
    return img;
}

QString emojiFontFilePath() {
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + "/NotoColorEmoji.ttf",
        appDir + "/resources/fonts/NotoColorEmoji.ttf",
        appDir + "/../resources/fonts/NotoColorEmoji.ttf",
        appDir + "/../../resources/fonts/NotoColorEmoji.ttf",
        appDir + "/../../../resources/fonts/NotoColorEmoji.ttf",
    };
    for (const QString &c : candidates) {
        if (QFileInfo::exists(c)) {
            return c;
        }
    }
    return candidates.front(); // missing file: load() fails, text-only rendering
}

} // namespace fc
