#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

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
//   ... every play/pause cycle:
//   preview.begin(pull);   // play  - reuses the open device (instant)
//   preview.stop();        // pause - parks the stream, device STAYS open
//   preview.release();     // final teardown (joins the thread)
//
// INSTANT transport (round 4): the render thread keeps the negotiated
// device open between runs. begin()/stop() only move it between the
// parked and rendering states - no device reopen, no thread respawn,
// no COM re-init on the GUI thread. stop() waits a BOUNDED time for
// the parked handshake so the caller knows when the pull callback can
// no longer run; begin() waits the same way so its bool reflects a
// stream that actually started. Only failure (device death) or
// release() tears the device down.
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
    // (1 or 2 channels, any rate). Fast path: a device that is already
    // open (a previous probe) returns the cached geometry immediately -
    // the GUI thread never waits for a reopen on a play after pause.
    // Otherwise the endpoint is opened and negotiated now. On success
    // fills the negotiated geometry and keeps the device OPEN for
    // begin(). Returns false when audio output is unavailable (no
    // device, exclusive-mode lock, non-float mix format the player
    // cannot feed, or a non-Windows platform).
    bool probe(int &sampleRate, int &channels);

    // Starts the render thread's stream (idempotent with stop(): a
    // running preview parks first). The pull callback drives the mixed
    // timeline audio onto the device. Returns false (with the device
    // still open for a retry) after a failed probe or when the stream
    // could not start (device lost while parked).
    bool begin(const PullFn &pull);

    // Parks the stream and joins NOTHING: the device stays open so the
    // next begin() is instant. Returns true once the parked handshake
    // completed (the pull callback can no longer run - the caller's
    // mixer may be freed); a false return means the render thread is
    // wedged inside the driver and the caller must keep anything the
    // callback references alive (a leaked mixer beats a dangling one).
    bool stop();

    // Full teardown: stops the stream if parked, joins the render
    // thread and releases the device (other apps can take the
    // endpoint). Called by the destructor; also the recovery path when
    // the device died mid-run.
    void release();

    bool running() const { return running_.load(); }

    // True while the render thread is alive AND healthy: a device that
    // died mid-run (endpoint removed, format change, driver failure)
    // tears its stream down but - unlike release() - never clears
    // running_, so the transport must check healthy() before trusting
    // the audio clock (playedSeconds() freezes at the death point).
    bool healthy() const;

    // The device's consumed position in SECONDS (frames the device has
    // actually played, not what was submitted): the transport follows
    // this while playing. 0.0 when not running.
    double playedSeconds() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> running_{false};
    // Set by the render thread's FAILURE exits (never by the clean
    // stop path); reset by probe() so the next run starts optimistic.
    std::atomic<bool> failed_{false};
};

} // namespace fc
