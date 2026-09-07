#include "text_renderer.h"

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

std::vector<ShapedRun> shapeTextQt(const TextDocument &doc) {
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
        s.advances.reserve(s.codepoints.size());
        const QFont font = fontForStyle(run.style);
        const QFontMetrics metrics(font);
        for (uint32_t cp : s.codepoints) {
            if (cp == 0x0Au) {
                s.advances.push_back(0);
                continue;
            }
            s.advances.push_back(metrics.horizontalAdvance(charAsQString(cp)));
        }
        s.ascent = metrics.ascent();
        s.descent = metrics.descent();
        s.lineGap = metrics.leading();
        shaped.push_back(std::move(s));
    }
    return shaped;
}

QImage renderTextLayer(const TextDocument &doc, int width, int height) {
    QImage image(std::max(width, 1), std::max(height, 1), QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    if (width <= 0 || height <= 0 || doc.runs.empty()) {
        return image;
    }

    const std::vector<ShapedRun> shaped = shapeTextQt(doc);
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
        const QString piece = QString::fromUtf8(text.data() + slice.byteStart, slice.byteLen);
        painter.setFont(fonts[run.runIndex]);
        painter.setPen(QPen(colors[run.runIndex], 1));
        painter.drawText(QPointF(slice.x, slice.baselineY), piece);
    }
    return image;
}

} // namespace fc
