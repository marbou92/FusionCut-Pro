#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace fc {

// ---------------------------------------------------------------------------
// export pipeline (the effects/transitions render side).
//
// Renders a fixed-rate RGBA frame sequence through the FFmpeg encode stack
// (H.264 with an MPEG-4 Part 2 fallback, yuv420p, CRF rate control) into
// an .mp4. The renderer itself is a CALLBACK: the caller (the app, the
// test suite) resolves timeline frame N into a composited, effect- and
// transition-processed RGBA8888 frame - exactly the same pipeline the
// program monitor runs - and the exporter owns only the encode/mux side.
// That keeps fc_media free of any timeline knowledge.
//
// Audio: the export renders the video program only for now. Timeline
// audio mixing (multi-track summing, crossfades) ships later; the
// export contract here is "what you scrub is what you render",
// frame-accurate, effects and transitions included.
// ---------------------------------------------------------------------------

struct ExportConfig {
    int width = 1280; // output frame size (evened down internally)
    int height = 720;
    double fps = 24.0;       // output frame rate (> 0)
    int64_t totalFrames = 0; // sequence length in output frames
    int crf = 23;            // x264 constant-rate factor (18..28 typical)
    std::string preset = "medium";
};

// Called for every output frame 0..totalFrames-1. Fill `rgba` (width *
// height * 4 bytes, RGBA8888 memory order) with the composited program
// frame. Return false to CANCEL the export (a source that cannot be
// decoded, or the user pressing Cancel).
using ExportFrameProvider = std::function<bool(int64_t frameIndex, uint8_t *rgba)>;

// Called after each encoded frame with the completed fraction [0..1].
// Return false to cancel.
using ExportProgress = std::function<bool(double fraction)>;

class Exporter {
public:
    // Runs the whole export synchronously (call from a worker thread).
    // Returns false on error (`error` filled) or cancellation (`error`
    // stays empty); partial output files are removed in both cases.
    static bool run(const std::string &dstPath, const ExportConfig &config,
                    const ExportFrameProvider &provider, const ExportProgress &progress,
                    std::string &error);
};

} // namespace fc
