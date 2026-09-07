#pragma once

#include <QImage>

#include <vector>

#include "text.h"

namespace fc {

// The M6 app-layer text rasterizer: bridges REAL font metrics (Qt) into
// the pure core layout engine, then paints the laid-out slices with
// QPainter. Glyph PIXELS are platform-dependent (every font engine
// differs); every NUMBER the core produces (wrap, line breaks,
// alignment, box placement) is not.
//
// shapeTextQt: one ShapedRun per document run - QFontMetrics integer
// advances/ascent/descent/leading plus the UTF-8 byte offsets the core
// slices by. Runs with empty text are skipped.
//
// renderTextLayer: a TRANSPARENT RGBA8888 image (width x height) with
// the document's background box (when enabled) and glyphs painted at
// the core layout positions. Safe to call from non-GUI threads (it
// paints on a QImage only - the export job does exactly that).
std::vector<ShapedRun> shapeTextQt(const TextDocument &doc);
QImage renderTextLayer(const TextDocument &doc, int width, int height);

// The pixel size actually used for a run (clamped into the core's
// legal range so QFont::setPixelSize never sees a degenerate value).
int textPixelSizeClamped(const TextStyle &style);

} // namespace fc
