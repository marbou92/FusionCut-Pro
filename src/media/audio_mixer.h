#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fc {

// ---------------------------------------------------------------------------
// Multi-track audio mixing over arbitrary sample windows.
//
// The timeline model is FLATTENED into AudioSpans by the app layer (see
// audio_timeline.h): one span per audio clip, with the clip's source
// in-point, its timeline placement, its fades, and the track's gain/pan
// baked in (mute/solo/master resolved at flatten time - the mixer stays
// timeline-free, exactly like the frame exporter takes a callback).
//
// The mixer decodes each DISTINCT source path through its own
// AudioDecoder (converted to the mixer's rate and channel count),
// keeps a small rolling queue of decoded chunks, and blends the
// chunk samples that overlap the requested window into the output
// buffer. Sequential monotonic windows (export, playback) decode
// strictly forward; a window that jumps (scrub, restart) makes the
// channel seek. Sample mapping is exact per chunk (a chunk's first
// sample lands at its own pts; integer stride within), so alignment
// never accumulates drift.
//
// Failure philosophy: a window is ALWAYS produced - a missing or
// broken source contributes silence and names itself in `error`
// rather than killing the export or the preview (the video path has
// the same tolerance). pull() returns false only when the arguments
// themselves are invalid.
// ---------------------------------------------------------------------------

// One clip's audio contribution over the timeline, in seconds.
struct AudioSpan {
    std::string path;         // DECODE path (proxy-resolved by the flattener)
    double srcStartSec = 0.0; // the clip's source in-point
    double startSec = 0.0;    // timeline start of the clip
    double endSec = 0.0;      // timeline end (exclusive)
    double fadeInSec = 0.0;   // linear ramp at the head (click-free cuts)
    double fadeOutSec = 0.0;  // linear ramp at the tail
    double gain = 1.0;        // linear, the track's fader baked in
    double pan = 0.0;         // [-1..1], the track's pan baked in
};

class AudioWindowMixer {
public:
    // channels must be 1 or 2; the rate is whatever the consumer needs
    // (48 kHz for export, the audio device's rate for preview).
    AudioWindowMixer(int sampleRate, int channels);
    ~AudioWindowMixer();

    AudioWindowMixer(const AudioWindowMixer &) = delete;
    AudioWindowMixer &operator=(const AudioWindowMixer &) = delete;

    int sampleRate() const;
    int channels() const;

    // Post-sum master gain (linear). Default 1.0.
    void setMasterGain(double linear);

    // Mixes `sampleCount` frames at absolute timeline position
    // `startSample` (in the mixer's own rate) into `dst` (interleaved,
    // sampleCount * channels floats, zeroed first; the hard clip runs
    // last so overflow can never leave the mixer). Returns false only
    // on invalid arguments; source problems land in `error` (first
    // one wins, the window still renders).
    bool pull(const std::vector<AudioSpan> &spans, int64_t startSample, int sampleCount, float *dst,
              std::string &error);

    // Drops every decoder and rolling buffer (position jumps, transport
    // stops). The next pull re-opens and seeks on demand.
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fc
