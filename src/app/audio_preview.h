#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

#include <QString>

namespace fc {

// ---------------------------------------------------------------------------
// AudioPreview: the Windows audio OUTPUT device driver for timeline
// playback (WASAPI shared mode, event-driven). The rest of the app
// stays audio-API-free: this class only knows how to pull interleaved
// FLOAT frames from a callback onto the device.
//
// Usage contract (two-phase - the mixer must be built at the DEVICE's
// rate, so the device is negotiated first):
//   AudioPreview preview;
//   int rate = 0, channels = 0;
//   if (preview.probe(rate, channels)) {
//       ... build an AudioWindowMixer(rate, channels) ...
//       preview.begin([mixer](float *dst, int frames) { ...mix... });
//   }
//   preview.stop();
//
// The callback runs on the PREVIEW THREAD: it must not touch Qt
// widgets; it may lock small mutexes and read immutable data (the
// audio timeline snapshot is designed exactly for that).
//
// playedSeconds() reports the device's consumed position (submitted
// minus currently-buffered) so the transport's playhead can follow the
// REAL audio clock instead of a drifting wall timer.
//
// Non-Windows platforms (the CI legs, the offscreen smoke harness):
// probe() returns false and everything else is a no-op - preview audio
// simply is not available there, and nothing else changes.
// ---------------------------------------------------------------------------
class AudioPreview {
public:
    // dst carries `frames` interleaved sample-frames at the negotiated
    // rate/channels; fill it (silence on failure - never throw).
    using PullFn = std::function<void(float *dst, int frames)>;

    AudioPreview();
    ~AudioPreview();

    AudioPreview(const AudioPreview &) = delete;
    AudioPreview &operator=(const AudioPreview &) = delete;

    // Opens the default render endpoint and negotiates a FLOAT format
    // (1 or 2 channels, any rate). On success fills the negotiated
    // geometry and keeps the device OPEN for begin(). Returns false
    // when audio output is unavailable (no device, exclusive-mode
    // lock, non-float mix format the player cannot feed, or a
    // non-Windows platform).
    bool probe(int &sampleRate, int &channels);

    // Starts the render thread (idempotent: a running preview stops
    // first). The pull callback drives the mixed timeline audio onto
    // the device. Returns false (device closed) after a failed probe.
    bool begin(const PullFn &pull);

    // Stops the stream and joins the render thread. Safe when not
    // running. The device handle is RELEASED (probe again for a new
    // run) so other apps can take the endpoint.
    void stop();

    bool running() const { return running_.load(); }

    // The device's consumed position in SECONDS (frames the device has
    // actually played, not what was submitted): the transport follows
    // this while playing. 0.0 when not running.
    double playedSeconds() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> running_{false};
};

} // namespace fc
