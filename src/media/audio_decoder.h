#pragma once

#include <string>
#include <vector>

#include "ffmpeg_wrappers.h"
#include "media_info.h"

namespace fc {

// One decoded audio chunk: INTERLEAVED float samples (the decoder's
// configured target channel count per sample frame), `frames`
// sample-frames, and the presentation time of the FIRST frame.
struct DecodedAudio {
    std::vector<float> samples; // frames * channels floats
    int frames = 0;
    double ptsSeconds = 0.0;
};

// Sequential/seeking decode pump for one audio stream, converted to
// interleaved FLOAT at a caller-chosen rate and channel count (the
// window mixer picks 48 kHz stereo for export, the preview player
// whatever the audio device wants). Mirrors VideoDecoder's semantics:
// open -> read forward, seekToSeconds to jump (BACKWARD to the nearest
// packet at or before the target, flush, then skip frames that precede
// the target). Owns all FFmpeg resources; reopen() closes any
// previously opened file.
class AudioDecoder {
public:
    AudioDecoder() = default;
    ~AudioDecoder() = default;

    AudioDecoder(const AudioDecoder &) = delete;
    AudioDecoder &operator=(const AudioDecoder &) = delete;

    // Opens `path` and prepares the best audio stream, resampling every
    // decoded frame to interleaved float at (sampleRate, channels).
    // channels must be 1 or 2. Returns false (error filled) when the
    // file has no audio or the codec cannot be opened.
    bool open(const std::string &path, int sampleRate, int channels, std::string &error);

    bool isOpen() const { return format_ != nullptr; }
    void close();

    // The negotiated OUTPUT geometry (the constructor arguments, clamped
    // to legal values by open()).
    int sampleRate() const { return rate_; }
    int channels() const { return channels_; }

    // Seeks backwards to the nearest demuxer packet at or before
    // `seconds` and flushes the decoder. Subsequent readSamples() calls
    // decode forward from that packet but skip chunks whose frames all
    // precede `seconds` (half a frame's tolerance), so callers resume
    // at the requested position. Audio streams have no keyframes -
    // every packet decodes independently - so a BACKWARD seek lands on
    // a packet boundary and the skip covers the sub-packet remainder.
    bool seekToSeconds(double seconds, std::string &error);

    // Decodes the next chunk (one resampled frame's worth of samples).
    // Returns false at end of stream (error is left empty in that case)
    // or on failure. Empty chunks (the resampler's warm-up delay) are
    // skipped internally; a true return always carries >= 1 frame.
    bool readSamples(DecodedAudio &out, std::string &error);

private:
    // (Re)creates the resampler when the source geometry changes.
    bool ensureResampler(std::string &error);

    int rate_ = 48000;
    int channels_ = 2;
    FormatContextPtr format_;
    CodecContextPtr codec_;
    SwrContextPtr resampler_;
    PacketPtr packet_;
    FramePtr frame_;
    int audioStreamIndex_ = -1;
    AVRational streamTimebase_{0, 1};
    bool endOfFile_ = false;
    bool draining_ = false;
    double seekTarget_ = -1.0; // >= 0 while skipping to the seek target
    // Source geometry the current resampler was built for (0 = unset).
    int resamplerSrcChannels_ = 0;
    int resamplerSrcRate_ = 0;
    int resamplerSrcFormat_ = -1;
};

} // namespace fc
