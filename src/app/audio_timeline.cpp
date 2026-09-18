#include "audio_timeline.h"

#include <algorithm>
#include <cmath>

#include "audio_math.h"

namespace fc {

std::vector<AudioSpan> flattenAudioSpans(const TimelineModel &model, double fps,
                                         const AudioDecodeResolver &resolver) {
    std::vector<AudioSpan> spans;
    if (fps <= 0.0 || !resolver) {
        return spans;
    }

    // Solo logic across the AUDIO lanes only: any soloed audio track
    // silences the non-soloed ones.
    bool anySolo = false;
    for (const Track &track : model.tracks()) {
        if (track.isAudio && track.solo) {
            anySolo = true;
            break;
        }
    }

    for (const Track &track : model.tracks()) {
        if (!track.isAudio) {
            continue; // video/text clips contribute audio through their
                      // own audio-track siblings, not directly
        }
        if (track.muted || (anySolo && !track.solo)) {
            continue; // muted, or outvoted by a solo
        }
        const double gain = audioDbToLinear(track.gainDb);
        if (gain <= 0.0) {
            continue; // fader at the floor: nothing to contribute
        }
        const double pan = std::clamp(track.pan, -1.0, 1.0);

        for (const Clip &clip : model.clips()) {
            if (clip.trackIndex != track.index || clip.isText) {
                continue;
            }
            if (clip.sourcePath.empty()) {
                continue;
            }
            const std::string decodePath = resolver(clip.sourcePath);
            if (decodePath.empty()) {
                continue;
            }
            const int64_t dur = clip.durationFrames();
            if (dur <= 0) {
                continue;
            }
            AudioSpan span;
            span.path = decodePath;
            // The SAME source mapping the video decode path uses
            // (fetchFrame: source seconds = timeline frames at the
            // clip's speed, offset by the source in-point) so an audio
            // clip sits under its video sibling exactly. The span
            // carries the clip's playback RATE: srcStartSec stays the
            // source in-point in seconds while the mixer consumes
            // source seconds at rate x timeline time, so the audio
            // tracks its video sibling at any speed (startSec/endSec
            // above are timeline seconds and already rescale through
            // the clip's rate-shortened duration).
            span.srcStartSec = static_cast<double>(clip.sourceInFrames) / fps;
            span.startSec = static_cast<double>(clip.timelineStart) / fps;
            span.endSec = static_cast<double>(clip.timelineStart + dur) / fps;
            if (span.endSec <= span.startSec) {
                continue;
            }
            // Fades clamp to the clip's own extent.
            const double spanDur = span.endSec - span.startSec;
            const double fadeIn = static_cast<double>(clip.fadeInFrames) / fps;
            const double fadeOut = static_cast<double>(clip.fadeOutFrames) / fps;
            span.fadeInSec = std::min(fadeIn, spanDur);
            span.fadeOutSec = std::min(fadeOut, spanDur);
            span.gain = gain;
            span.pan = pan;
            // The clip's playback rate (the mixer maps timeline time to
            // source time through it; default 1.0 = the old behavior).
            span.rate = clip.rate;
            spans.push_back(std::move(span));
        }
    }
    return spans;
}

void AudioTimelineSnapshot::publish(std::vector<AudioSpan> spans, double masterGainLinear) {
    std::lock_guard<std::mutex> lock(mutex_);
    spans_ = std::make_shared<const std::vector<AudioSpan>>(std::move(spans));
    masterGain_ = masterGainLinear;
}

void AudioTimelineSnapshot::take(std::vector<AudioSpan> &spans, double &masterGainLinear) const {
    std::shared_ptr<const std::vector<AudioSpan>> current;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current = spans_;
        masterGainLinear = masterGain_;
    }
    if (current) {
        spans = *current;
    } else {
        spans.clear();
    }
}

bool AudioTimelineSnapshot::hasAudio() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return spans_ && !spans_->empty();
}

void AudioTimelineSnapshot::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    spans_.reset();
    masterGain_ = 1.0;
}

} // namespace fc
