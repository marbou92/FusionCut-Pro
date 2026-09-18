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

        // The source-second range this window needs from the span. At a
        // playback rate != 1 the clip consumes source seconds `speed`
        // times faster than timeline seconds, so the offset into the
        // source scales by `speed` (at 1.0 the multiplication is by
        // exactly 1.0 - bit-identical to the unscaled arithmetic).
        const double speed = (std::isfinite(span.rate) && span.rate > 0.0) ? span.rate : 1.0;
        const double srcFrom =
            span.srcStartSec + (std::max(windowStartSec, span.startSec) - span.startSec) * speed;
        const double srcTo =
            span.srcStartSec + (std::min(windowEndSec, span.endSec) - span.startSec) * speed;

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

        // Position the rolling decode. The decision reads the HELD
        // chunks, not the raw decoder position: decoded chunks
        // legitimately overhang the pull window (AAC 21 ms / MP3 26 ms /
        // FLAC ~85 ms chunks vs ~10 ms WASAPI pulls), so the old
        // position-based rule fired a clear+demuxer-seek+flush+re-decode
        // cycle on EVERY audio callback. Coverage rule: seek only when
        // the held audio does not reach the needed range - it starts
        // after srcFrom (the window moved back into undecoded audio) or
        // ends well before srcFrom (a jump too far forward to decode
        // through).
        if (channel.primed) {
            bool needSeek = false;
            if (channel.chunks.empty()) {
                // Nothing held (all dropped or eof): fall back to the
                // decoder position - far behind wastes a forward
                // re-decode, ahead of the window means the position
                // moved back.
                needSeek =
                    channel.nextSrcSec < srcFrom - 0.25 || channel.nextSrcSec > srcTo + 0.001;
            } else {
                const double heldFrom = channel.chunks.front().ptsSec;
                const double heldTo = chunkEndSec(channel.chunks.back(), rate);
                needSeek = heldFrom > srcFrom + 1.0e-4 || heldTo < srcFrom - 0.25;
            }
            if (needSeek) {
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
        // to timeline-second spanStart + (c0 + j / rate - srcStart) at
        // speed 1 - i.e. dst sample kFloat + j with kFloat below. The
        // mapping anchors on the chunk's own pts (sub-sample error
        // bounded, never drifts). At a playback rate != 1 the span
        // consumes source seconds `speed` times faster, so the source
        // offset divides by `speed` on the way into timeline time:
        // fi(j) = kFloat + j / speed, linearly interpolated between the
        // two nearest output samples. Division by exactly 1.0 is exact,
        // so the speed == 1.0 branch below keeps the pinned
        // integer-stride scatter bit-identical.
        const double kFloatBase =
            (span.startSec - span.srcStartSec / speed) * static_cast<double>(rate) -
            static_cast<double>(startSample);
        for (const Chunk &chunk : channel.chunks) {
            if (chunkEndSec(chunk, rate) <= srcFrom - 1.0e-9) {
                continue;
            }
            if (chunk.ptsSec >= srcTo) {
                break;
            }
            // A BACKWARD seek can land on a packet that starts before the
            // needed range: skip the samples sitting before the span's
            // in-point (srcFrom) instead of blending them. They are source
            // audio the clip does not own (the previous clip's tail or
            // raw lead-in) - leaking them bakes a click into every clip
            // head that lacks a fadeIn, in exports and the WASAPI preview
            // alike, whenever the clip head falls strictly inside a pull
            // window.
            int j0 = 0;
            if (chunk.ptsSec < srcFrom - 1.0e-9) {
                const double raw = (srcFrom - chunk.ptsSec) * rate;
                j0 = static_cast<int>(std::ceil(raw - 1.0e-9));
                if (j0 < 0) {
                    j0 = 0;
                }
                if (j0 >= chunk.frames) {
                    continue;
                }
            }
            const double kFloat = kFloatBase + chunk.ptsSec * static_cast<double>(rate) / speed;
            if (speed == 1.0) {
                const int64_t k0 = static_cast<int64_t>(std::llround(kFloat));
                for (int j = j0; j < chunk.frames; ++j) {
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
            } else {
                // Speed-shifted blend: per-sample float index, no
                // accumulation (fi is recomputed from j so error stays
                // sub-sample and never drifts), fraction of a sample
                // linearly shared with the next output slot. fi is
                // strictly increasing in j, so the window-exit break is
                // safe.
                const double invSpeed = 1.0 / speed;
                for (int j = j0; j < chunk.frames; ++j) {
                    const double fi = kFloat + static_cast<double>(j) * invSpeed;
                    const double kf = std::floor(fi);
                    const int64_t k = static_cast<int64_t>(kf);
                    if (k >= sampleCount) {
                        break;
                    }
                    const double frac = fi - kf;
                    // Fades evaluate at the sample's own (fractional)
                    // timeline position.
                    const double tSec = (static_cast<double>(startSample) + fi) / rate;
                    const double local = tSec - span.startSec;
                    double fade = 1.0;
                    if (fadeIn > 0.0 || fadeOut > 0.0) {
                        fade = std::min(audioFadeMultiplier(local, fadeIn),
                                        audioFadeMultiplier(span.endSec - tSec, fadeOut));
                    }
                    const double gl = gainL * fade;
                    const double gr = gainR * fade;
                    const float *src = chunk.samples.data() + size_t(j) * ch;
                    if (k >= 0) {
                        float *d = dst + size_t(k) * ch;
                        if (ch == 2) {
                            d[0] +=
                                static_cast<float>(static_cast<double>(src[0]) * gl * (1.0 - frac));
                            d[1] +=
                                static_cast<float>(static_cast<double>(src[1]) * gr * (1.0 - frac));
                        } else {
                            d[0] +=
                                static_cast<float>(static_cast<double>(src[0]) * gl * (1.0 - frac));
                        }
                    }
                    if (frac > 0.0 && k + 1 >= 0 && k + 1 < sampleCount) {
                        float *d = dst + size_t(k + 1) * ch;
                        if (ch == 2) {
                            d[0] += static_cast<float>(static_cast<double>(src[0]) * gl * frac);
                            d[1] += static_cast<float>(static_cast<double>(src[1]) * gr * frac);
                        } else {
                            d[0] += static_cast<float>(static_cast<double>(src[0]) * gl * frac);
                        }
                    }
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
