#include "timeline_model.h"

#include <algorithm>
#include <cmath>
#include <cstddef> // ptrdiff_t (std::vector::insert iterator offset; jammy's older libstdc++ does not pull this in transitively)

namespace fc {

int64_t Clip::durationFrames() const {
    if (rate <= 0.0 || sourceOutFrames <= sourceInFrames) {
        return 0;
    }
    return static_cast<int64_t>(
        std::llround(static_cast<double>(sourceOutFrames - sourceInFrames) / rate));
}

void TimelineModel::setFps(double fps) {
    if (fps > 1.0) {
        fps_ = fps;
    }
}

int TimelineModel::addTrack(const std::string &name, bool isAudio) {
    Track track;
    track.index = static_cast<int>(tracks_.size());
    track.name = name;
    track.isAudio = isAudio;
    tracks_.push_back(track);
    return track.index;
}

const Track *TimelineModel::trackAt(int index) const {
    return (index >= 0 && index < static_cast<int>(tracks_.size()))
               ? &tracks_[static_cast<size_t>(index)]
               : nullptr;
}

Track *TimelineModel::trackAt(int index) {
    // Delegate to the const overload (Scott-Meyers avoid-duplication):
    // call the const version, then cast away const on the result. Safe
    // because the non-const overload is only callable on a non-const
    // `this`, so the underlying object is genuinely mutable here.
    return const_cast<Track *>(static_cast<const TimelineModel &>(*this).trackAt(index));
}

bool TimelineModel::setTrackState(int index, bool locked, bool muted, bool solo) {
    if (Track *track = trackAt(index)) {
        track->locked = locked;
        track->muted = muted;
        track->solo = solo;
        return true;
    }
    return false;
}

int64_t TimelineModel::addClip(int trackIndex, const std::string &sourcePath,
                               const std::string &label, int64_t sourceInFrames,
                               int64_t sourceOutFrames, int64_t timelineStart, double rate) {
    if (trackIndex < 0 || trackIndex >= trackCount()) {
        return 0;
    }
    if (sourceOutFrames <= sourceInFrames) {
        return 0;
    }
    if (timelineStart < 0) {
        return 0;
    }
    if (rate <= 0.0) {
        return 0;
    }

    Clip clip;
    clip.id = nextId_++;
    clip.sourcePath = sourcePath;
    clip.label = label;
    clip.sourceInFrames = sourceInFrames;
    clip.sourceOutFrames = sourceOutFrames;
    clip.timelineStart = timelineStart;
    clip.rate = rate;
    clip.trackIndex = trackIndex;
    clips_.push_back(clip);
    return clip.id;
}

bool TimelineModel::removeClip(int64_t id) {
    for (auto it = clips_.begin(); it != clips_.end(); ++it) {
        if (it->id == id) {
            clips_.erase(it);
            return true;
        }
    }
    return false;
}

bool TimelineModel::splitAt(int64_t frame, int trackIndex) {
    bool split = false;
    for (size_t i = 0; i < clips_.size(); ++i) {
        Clip &clip = clips_[i];
        if (clip.trackIndex != trackIndex) {
            continue;
        }
        const int64_t start = clip.timelineStart;
        const int64_t end = clip.timelineEnd();
        if (frame > start && frame < end) {
            const int64_t sourceDelta =
                static_cast<int64_t>(std::llround(static_cast<double>(frame - start) * clip.rate));
            Clip right = clip;
            right.id = nextId_++;
            right.label = clip.label + " (2)";
            right.sourceInFrames = clip.sourceInFrames + sourceDelta;
            right.timelineStart = frame;
            clip.sourceOutFrames = right.sourceInFrames; // left half ends here
            clips_.insert(clips_.begin() + static_cast<ptrdiff_t>(i + 1), right);
            split = true;
            break; // one split per call; caller can repeat
        }
    }
    return split;
}

bool TimelineModel::moveClip(int64_t id, int64_t newTimelineStart) {
    if (newTimelineStart < 0) {
        return false;
    }
    if (Clip *clip = clipById(id)) {
        clip->timelineStart = newTimelineStart;
        return true;
    }
    return false;
}

bool TimelineModel::trimClipStart(int64_t id, int64_t deltaFrames) {
    Clip *clip = clipById(id);
    if (!clip) {
        return false;
    }
    const int64_t sourceDelta =
        static_cast<int64_t>(std::llround(static_cast<double>(deltaFrames) * clip->rate));
    const int64_t newSourceIn = clip->sourceInFrames + sourceDelta;
    const int64_t newStart = clip->timelineStart + deltaFrames;
    if (newSourceIn < 0 || newSourceIn >= clip->sourceOutFrames) {
        return false;
    }
    if (newStart < 0) {
        return false;
    }
    clip->sourceInFrames = newSourceIn;
    clip->timelineStart = newStart;
    return true;
}

bool TimelineModel::trimClipEnd(int64_t id, int64_t deltaFrames) {
    Clip *clip = clipById(id);
    if (!clip) {
        return false;
    }
    const int64_t sourceDelta =
        static_cast<int64_t>(std::llround(static_cast<double>(deltaFrames) * clip->rate));
    const int64_t newSourceOut = clip->sourceOutFrames + sourceDelta;
    if (newSourceOut <= clip->sourceInFrames) {
        return false;
    }
    clip->sourceOutFrames = newSourceOut;
    return true;
}

const Clip *TimelineModel::clipAt(int64_t frame, int trackIndex) const {
    for (const Clip &clip : clips_) {
        if (clip.trackIndex != trackIndex) {
            continue;
        }
        if (frame >= clip.timelineStart && frame < clip.timelineEnd()) {
            return &clip;
        }
    }
    return nullptr;
}

Clip *TimelineModel::clipById(int64_t id) {
    for (Clip &clip : clips_) {
        if (clip.id == id) {
            return &clip;
        }
    }
    return nullptr;
}

int64_t TimelineModel::durationFrames() const {
    int64_t max = 0;
    for (const Clip &clip : clips_) {
        const int64_t end = clip.timelineEnd();
        if (end > max) {
            max = end;
        }
    }
    return max;
}

double TimelineModel::durationSeconds() const {
    return static_cast<double>(durationFrames()) / fps_;
}

} // namespace fc
