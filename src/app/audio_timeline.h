#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "audio_mixer.h"
#include "timeline_model.h"

namespace fc {

// ---------------------------------------------------------------------------
// The app-side bridge from the timeline MODEL to the media-layer mixer:
// the audio-track clips flattened into AudioSpans (seconds), with the
// tracks' strip state baked in.
//
// The preview player snapshots this list (a shared_ptr under a small
// mutex - the GUI thread rebuilds it, the audio thread reads it), and
// the export job flattens ONCE into a frozen copy (the model is frozen
// for the export's whole lifetime - same contract the video path has).
//
// Pure data plumbing: no Qt widgets, no FFmpeg - safe to link into the
// runtime smoke harness.
// ---------------------------------------------------------------------------

// Maps a source path to its DECODE path (the proxy when one exists,
// else the source itself). Returns an empty string to drop the clip's
// audio (the resolver decides; an empty result skips the span).
using AudioDecodeResolver = std::function<std::string(const std::string &)>;

// Flattens every audio-track clip into mixer spans:
//   * track gain (dB -> linear) and pan bake into each span;
//   * a MUTED track contributes nothing; when any audio track is
//     SOLOED, only the soloed ones contribute (per-track solo logic,
//     the audio lanes only);
//   * clip fades convert from timeline frames to seconds and clamp to
//     the clip's own length.
// `resolver` maps source paths; `fps` is the project rate. Skips
// clips with no source, empty resolved paths, or degenerate extents.
std::vector<AudioSpan> flattenAudioSpans(const TimelineModel &model, double fps,
                                         const AudioDecodeResolver &resolver);

// Thread-safe handoff of the flattened audio timeline: the GUI thread
// publishes (rebuild), the audio render thread takes the current
// snapshot without blocking the publisher.
class AudioTimelineSnapshot {
public:
    // Replaces the snapshot (GUI thread).
    void publish(std::vector<AudioSpan> spans, double masterGainLinear);

    // Copies out the current snapshot + master gain (any thread; a
    // short mutex hold, no mixing happens under it).
    void take(std::vector<AudioSpan> &spans, double &masterGainLinear) const;

    // True when the current snapshot has at least one span (cheap
    // pre-check before spinning up an audio device).
    bool hasAudio() const;

    void clear();

private:
    mutable std::mutex mutex_;
    std::shared_ptr<const std::vector<AudioSpan>> spans_;
    double masterGain_ = 1.0;
};

} // namespace fc
