#include "audio_mixer.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>

#include "audio_decoder.h"
#include "audio_math.h"

namespace fc {

namespace {

// One decoded, resampled chunk held for blending (interleaved at the
// mixer's own rate/channel count; the pts is in SOURCE seconds, which
// the decoder already converted to the mixer's rate timeline).
struct Chunk {
    double ptsSec = 0.0; // source-seconds of the FIRST sample frame
    int frames = 0;
    std::vector<float> samples; // frames * channels interleaved
};

double chunkEndSec(const Chunk &c, int rate) {
    return c.ptsSec + static_cast<double>(c.frames) / rate;
}

// Rolling decode state for one distinct source path.
struct Channel {
    std::unique_ptr<AudioDecoder> dec;
    std::deque<Chunk> chunks;
    double nextSrcSec = 0.0; // where the NEXT readSamples will land
    bool primed = false;     // nextSrcSec valid (a read or seek happened)
    bool failed = false;     // open/read hard failure - silent forever
    bool eof = false;        // decoder exhausted - silent past the end
};

} // namespace

struct AudioWindowMixer::Impl {
    int rate = 48000;
    int channels = 2;
    double masterGain = 1.0;
    std::map<std::string, Channel> sources; // rolling decode state by path
};

AudioWindowMixer::AudioWindowMixer(int sampleRate, int channels) : impl_(new Impl) {
    impl_->rate = std::clamp(sampleRate, 8000, 192000);
    impl_->channels = (channels == 1) ? 1 : 2;
}

AudioWindowMixer::~AudioWindowMixer() = default;

int AudioWindowMixer::sampleRate() const {
    return impl_->rate;
}

int AudioWindowMixer::channels() const {
    return impl_->channels;
}

void AudioWindowMixer::setMasterGain(double linear) {
    impl_->masterGain = linear;
}

void AudioWindowMixer::reset() {
    impl_->sources.clear();
}

bool AudioWindowMixer::pull(const std::vector<AudioSpan> &spans, int64_t startSample,
                            int sampleCount, float *dst, std::string &error) {
    if (sampleCount < 0 || (sampleCount > 0 && dst == nullptr)) {
        error = "invalid audio window";
        return false;
    }
    const int rate = impl_->rate;
    const int ch = impl_->channels;
    if (sampleCount > 0) {
        std::fill(dst, dst + size_t(sampleCount) * ch, 0.0f);
    }
    if (sampleCount == 0) {
        return true;
    }

    const double windowStartSec = static_cast<double>(startSample) / rate;
    const double windowEndSec = static_cast<double>(startSample + sampleCount) / rate;

    for (const AudioSpan &span : spans) {
        if (span.endSec <= span.startSec || span.path.empty()) {
            continue;
        }
        if (span.endSec <= windowStartSec || span.startSec >= windowEndSec) {
            continue; // not in this window
        }

        Channel &channel = impl_->sources[span.path];
        if (channel.failed) {
            if (error.empty()) {
                error = "audio source failed earlier: " + span.path;
            }
            continue;
        }

        // The source-second range this window needs from the span.
        const double srcFrom =
            span.srcStartSec + (std::max(windowStartSec, span.startSec) - span.startSec);
        const double srcTo =
            span.srcStartSec + (std::min(windowEndSec, span.endSec) - span.startSec);

        // Open on demand.
        if (!channel.dec) {
            channel.dec.reset(new AudioDecoder);
            std::string openError;
            if (!channel.dec->open(span.path, rate, ch, openError)) {
                channel.failed = true;
                if (error.empty()) {
                    error = "audio source will not open (" + openError + "): " + span.path;
                }
                continue;
            }
        }

        // Position the rolling decode: seek when the decoder's next
        // position is far behind the needed range (wasted forward
        // decode) or anywhere ahead of it (the window moved back).
        if (channel.primed) {
            if (channel.nextSrcSec < srcFrom - 0.25 || channel.nextSrcSec > srcTo + 0.001) {
                channel.chunks.clear();
                channel.eof = false;
                std::string seekError;
                if (!channel.dec->seekToSeconds(srcFrom, seekError)) {
                    channel.failed = true;
                    if (error.empty()) {
                        error = "audio seek failed (" + seekError + "): " + span.path;
                    }
                    continue;
                }
                channel.nextSrcSec = srcFrom;
            }
        } else {
            std::string seekError;
            if (!channel.dec->seekToSeconds(srcFrom, seekError)) {
                channel.failed = true;
                if (error.empty()) {
                    error = "audio seek failed (" + seekError + "): " + span.path;
                }
                continue;
            }
            channel.nextSrcSec = srcFrom;
            channel.primed = true;
        }

        // Read forward until the chunks cover srcTo (or the source ends).
        while (!channel.eof &&
               (channel.chunks.empty() || chunkEndSec(channel.chunks.back(), rate) < srcTo)) {
            DecodedAudio decoded;
            std::string readError;
            if (!channel.dec->readSamples(decoded, readError)) {
                if (!readError.empty()) {
                    channel.failed = true;
                    if (error.empty()) {
                        error = "audio decode failed (" + readError + "): " + span.path;
                    }
                    break;
                }
                channel.eof = true; // clean end of source
                break;
            }
            Chunk chunk;
            chunk.ptsSec = decoded.ptsSeconds;
            chunk.frames = decoded.frames;
            chunk.samples = std::move(decoded.samples);
            channel.nextSrcSec = chunkEndSec(chunk, rate);
            channel.chunks.push_back(std::move(chunk));
        }

        // Drop chunks that end before the needed range.
        while (!channel.chunks.empty() &&
               chunkEndSec(channel.chunks.front(), rate) < srcFrom - 1.0e-9) {
            channel.chunks.pop_front();
        }

        // Blend every overlapping chunk into the window.
        const double spanDur = span.endSec - span.startSec;
        const double fadeIn = span.fadeInSec > spanDur ? spanDur : span.fadeInSec;
        const double fadeOut = span.fadeOutSec > spanDur ? spanDur : span.fadeOutSec;
        double gainL = span.gain, gainR = span.gain;
        if (ch == 2) {
            double panL = 1.0, panR = 1.0;
            audioPanCoefficients(span.pan, panL, panR);
            gainL *= panL;
            gainR *= panR;
        } else {
            double panL = 1.0, panR = 1.0;
            audioPanCoefficients(span.pan, panL, panR);
            gainL *= 0.5 * (panL + panR); // mono output: pan folds to gain
        }

        // Chunk sample j sits at source-second c0 + j / rate, which maps
        // to timeline-second spanStart + (c0 + j / rate - srcStart), i.e.
        // dst sample kFloat + j with kFloat below. The mapping anchors on
        // the chunk's own pts (sub-sample error bounded, never drifts).
        const double kFloatBase = (span.startSec - span.srcStartSec) * static_cast<double>(rate) -
                                  static_cast<double>(startSample);
        for (const Chunk &chunk : channel.chunks) {
            if (chunkEndSec(chunk, rate) <= srcFrom - 1.0e-9) {
                continue;
            }
            if (chunk.ptsSec >= srcTo) {
                break;
            }
            const double kFloat = kFloatBase + chunk.ptsSec * static_cast<double>(rate);
            const int64_t k0 = static_cast<int64_t>(std::llround(kFloat));
            for (int j = 0; j < chunk.frames; ++j) {
                const int64_t k = k0 + j;
                if (k < 0) {
                    continue;
                }
                if (k >= sampleCount) {
                    break;
                }
                // Fades evaluate at the sample's own timeline position.
                const double tSec = static_cast<double>(startSample + k) / rate;
                const double local = tSec - span.startSec;
                double fade = 1.0;
                if (fadeIn > 0.0 || fadeOut > 0.0) {
                    fade = std::min(audioFadeMultiplier(local, fadeIn),
                                    audioFadeMultiplier(span.endSec - tSec, fadeOut));
                }
                const double gl = gainL * fade;
                const double gr = gainR * fade;
                const float *src = chunk.samples.data() + size_t(j) * ch;
                float *d = dst + size_t(k) * ch;
                if (ch == 2) {
                    d[0] += static_cast<float>(static_cast<double>(src[0]) * gl);
                    d[1] += static_cast<float>(static_cast<double>(src[1]) * gr);
                } else {
                    d[0] += static_cast<float>(static_cast<double>(src[0]) * gl);
                }
            }
        }
    }

    // Master gain + the hard clip, last.
    const double master = impl_->masterGain;
    for (size_t i = 0; i < size_t(sampleCount) * ch; ++i) {
        dst[i] = audioClipSample(static_cast<float>(static_cast<double>(dst[i]) * master));
    }
    return true;
}

} // namespace fc
