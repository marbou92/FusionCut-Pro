#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fc {

// ---------------------------------------------------------------------------
// M5 Phase 2 transitions engine.
//
// Pure data + pure processing: no Qt, no FFmpeg - unit tested in
// fc_transition_tests. The engine composites two RGBA8888 frames (4 bytes
// per pixel in R, G, B, A memory order - exactly QImage::Format_RGBA8888
// layout) into a third, distinct output buffer, on the CPU, deterministically.
//
// Semantics (the cross-clip compositor contract):
//   `a`       the OUTGOING frame (the left clip at its LIVE source position)
//   `b`       the INCOMING frame (the right clip's HELD first frame, see the
//             timeline model's transition-window documentation)
//   `out`     the composite; must NOT alias a or b
//   `progress` blend position, 0 = the cut starts, 1 = the cut is complete
//
// Every transition is ENDPOINT EXACT: progress <= 0 produces a byte-identical
// copy of a, progress >= 1 a byte-identical copy of b. All math is integer or
// explicitly-rounded double; the film dissolve and checker wipe use the same
// fixed integer-hash family as the film grain (seeded, no rand()), so
// identical inputs always produce identical outputs across platforms.
//
// Geometry conventions (pixel centers, not corners): a pixel is "through" a
// wipe edge when its CENTER crosses it; x + 0.5 and y + 0.5 are used
// throughout so p = 0 and p = 1 are exactly the pure frames. Sampling
// transitions (slide/push/zoom) use nearest-neighbor reads with edge
// clamping (zoom.out letterboxes out-of-bounds reads as black).
//
// The catalog is a static table; ids are stable strings ("dissolve.cross").
// Processing dispatches on the id in applyTransition. Unknown ids copy `a`
// (forward compatible: a saved project from a newer catalog degrades to a
// hard cut instead of crashing).
//
// Model:
//   TransitionDescriptor - one catalog entry (id, label, category).
//   Transition           - one placed transition on a clip boundary
//                          (timeline_model.h; kind references an id here).
// ---------------------------------------------------------------------------

struct TransitionDescriptor {
    std::string id;       // "dissolve.cross"
    std::string label;    // "Cross Dissolve"
    std::string category; // "Dissolve", "Wipe", "Slide", "Push", "Zoom"
};

// The M5 Phase 2 catalog: 36 transitions across 5 categories.
const std::vector<TransitionDescriptor> &transitionCatalog();

// Catalog lookup by id; nullptr when unknown.
const TransitionDescriptor *findTransition(const std::string &id);

// Composites the transition at `progress` (clamped to [0, 1]) from `a` into
// `out`. All three buffers hold width * height * 4 bytes in RGBA8888 order
// and must be pairwise distinct. Degenerate sizes (width or height <= 0,
// null a/b/out) and unknown kinds are a no-op (a valid `a`/`out` pair with
// an unknown kind copies a). The alpha channel is preserved from the frame
// the pixel is taken from (blends interpolate alpha too; zoom-out
// letterbox pixels are opaque black).
void applyTransition(const uint8_t *a, const uint8_t *b, uint8_t *out, int width, int height,
                     const std::string &kind, double progress);

} // namespace fc
