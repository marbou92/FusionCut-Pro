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
    return insertTrack(trackCount(), name, isAudio, /*isText=*/false);
}

int TimelineModel::insertTrack(int position, const std::string &name, bool isAudio, bool isText) {
    if (isAudio && isText) {
        return -1; // a lane is exactly one of video / audio / text
    }
    position = std::min(std::max(position, 0), trackCount());
    Track track;
    track.index = position;
    track.name = name;
    track.isAudio = isAudio;
    track.isText = isText;
    tracks_.insert(tracks_.begin() + static_cast<ptrdiff_t>(position), track);
    for (size_t i = 0; i < tracks_.size(); ++i) {
        tracks_[i].index = static_cast<int>(i);
    }
    // Renumber everything at or below the insertion point: every clip and
    // transition on a track with index >= position shifts up by one. The
    // shift is uniform per track, so adjacency and same-track invariants
    // (transitions) move with their clips.
    for (Clip &clip : clips_) {
        if (clip.trackIndex >= position) {
            ++clip.trackIndex;
        }
    }
    for (Transition &t : transitions_) {
        if (t.trackIndex >= position) {
            ++t.trackIndex;
        }
    }
    return position;
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
    const Track *track = trackAt(trackIndex);
    if (track && track->isText) {
        return 0; // text lanes host text clips only (addTextClip)
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

int64_t TimelineModel::addTextClip(int trackIndex, const TextDocument &doc, int64_t timelineStart,
                                   int64_t durationFrames) {
    const Track *track = trackAt(trackIndex);
    if (!track || !track->isText || track->isAudio) {
        return 0;
    }
    if (durationFrames < 1 || timelineStart < 0) {
        return 0;
    }
    const int64_t newEnd = timelineStart + durationFrames;
    for (const Clip &other : clips_) {
        if (other.trackIndex != trackIndex) {
            continue;
        }
        if (timelineStart < other.timelineEnd() && newEnd > other.timelineStart) {
            return 0; // would overlap
        }
    }
    Clip clip;
    clip.id = nextId_++;
    clip.isText = true;
    clip.text = doc;
    normalizeTextDocument(clip.text);
    clip.label = textPreviewLabel(clip.text);
    clip.sourceInFrames = 0;
    clip.sourceOutFrames = durationFrames; // == duration at rate 1.0
    clip.timelineStart = timelineStart;
    clip.rate = 1.0;
    clip.trackIndex = trackIndex;
    clips_.push_back(clip);
    return clip.id;
}

std::vector<const Clip *> TimelineModel::textClipsAt(int64_t frame) const {
    std::vector<const Clip *> out;
    for (int t = trackCount() - 1; t >= 0; --t) {
        // Bottom-most text lanes paint first: lane order in the panel is
        // index order (lower index drawn higher), so higher text lanes
        // composite LAST = on top (the same topmost-wins convention the
        // video stack uses, inverted for alpha painting).
        const Track *track = trackAt(t);
        if (!track || !track->isText) {
            continue;
        }
        if (const Clip *clip = clipAt(frame, t)) {
            out.push_back(clip);
        }
    }
    return out;
}

void TimelineModel::replaceAll(double fps, std::vector<Track> tracks, std::vector<Clip> clips,
                               std::vector<Transition> transitions) {
    // Indices are re-derived from position (tracks arrive in file order);
    // clip trackIndex references were validated against this count by the
    // parser before the move.
    for (size_t i = 0; i < tracks.size(); ++i) {
        tracks[i].index = static_cast<int>(i);
    }
    fps_ = fps;
    tracks_ = std::move(tracks);
    clips_ = std::move(clips);
    transitions_ = std::move(transitions);
    nextId_ = 1;
    for (const Clip &clip : clips_) {
        nextId_ = std::max(nextId_, clip.id + 1);
    }
    for (const Transition &t : transitions_) {
        nextId_ = std::max(nextId_, t.id + 1);
    }
    pruneTransitions(); // belt and braces: the file's invariants hold
}

bool TimelineModel::removeClip(int64_t id) {
    for (auto it = clips_.begin(); it != clips_.end(); ++it) {
        if (it->id == id) {
            clips_.erase(it);
            pruneTransitions();
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
            const int64_t splitClipId = clip.id; // clip dangles after insert below
            Clip right = clip;
            right.id = nextId_++;
            right.label = clip.label + " (2)";
            if (clip.isText) {
                // Text clips have no source: the left half keeps [0,
                // splitOffset), the right half RESTARTS its synthetic
                // extent at 0 (sourceIn is always 0 on text clips).
                right.sourceInFrames = 0;
                right.sourceOutFrames = clip.sourceOutFrames - sourceDelta;
                clip.sourceOutFrames = sourceDelta;
            } else {
                right.sourceInFrames = clip.sourceInFrames + sourceDelta;
                clip.sourceOutFrames = right.sourceInFrames; // left half ends here
            }
            right.timelineStart = frame;
            // M5 Phase 3: the right half inherits the stack, so its
            // keyframe tracks shift with its new in-point. Keyframe frames
            // are CLIP-RELATIVE TIMELINE frames, so the offset is the
            // timeline split position, not the rate-adjusted source delta.
            // Points before the split drop; the left half keeps its full
            // tracks - unreachable points simply wait past its (shorter)
            // extent, and would come back if it is extended again
            // (non-destructive).
            for (EffectInstance &fx : right.effectStack) {
                fx.rebaseKeyframes(frame - start);
            }
            clips_.insert(clips_.begin() + static_cast<ptrdiff_t>(i + 1), right);
            // M5 Phase 2: a transition on this clip's END boundary now
            // belongs to the RIGHT half (the half that owns the cut).
            for (Transition &t : transitions_) {
                if (t.leftClipId == splitClipId) {
                    t.leftClipId = right.id;
                }
            }
            split = true;
            break; // one split per call; caller can repeat
        }
    }
    if (split) {
        pruneTransitions(); // clamps durations that no longer fit
    }
    return split;
}

bool TimelineModel::moveClip(int64_t id, int64_t newTimelineStart) {
    if (newTimelineStart < 0) {
        return false;
    }
    if (Clip *clip = clipById(id)) {
        clip->timelineStart = newTimelineStart;
        pruneTransitions();
        return true;
    }
    return false;
}

bool TimelineModel::moveClipTo(int64_t id, int newTrackIndex, int64_t newTimelineStart) {
    Clip *clip = clipById(id);
    if (!clip) {
        return false;
    }
    const Track *target = trackAt(newTrackIndex);
    const Track *origin = trackAt(clip->trackIndex);
    if (!target || !origin) {
        return false;
    }
    // Clip kind = the kind of its track: video / audio / text must match
    // exactly (M6: text clips never leave text lanes).
    if (target->isAudio != origin->isAudio || target->isText != origin->isText) {
        return false;
    }
    if (newTimelineStart < 0) {
        return false;
    }
    const int64_t dur = clip->durationFrames();
    if (dur <= 0) {
        return false;
    }
    const int64_t newEnd = newTimelineStart + dur;
    for (const Clip &other : clips_) {
        if (other.id == id || other.trackIndex != newTrackIndex) {
            continue;
        }
        const int64_t oStart = other.timelineStart;
        const int64_t oEnd = other.timelineEnd();
        if (newTimelineStart < oEnd && newEnd > oStart) {
            return false; // would overlap
        }
    }
    clip->trackIndex = newTrackIndex;
    clip->timelineStart = newTimelineStart;
    pruneTransitions();
    return true;
}

int64_t TimelineModel::findDropPosition(int trackIndex, int64_t clipId, int64_t desiredStart,
                                        int64_t durationFrames) const {
    const Clip *moving = clipById(clipId);
    if (!moving) {
        if (clipId != 0) {
            return -1;
        }
        // clipId 0 = a NEW clip being placed (no model identity yet); the
        // CALLER guarantees the track-kind match (M6: text drops resolve
        // this way before addTextClip mints an id).
    }
    const Track *target = trackAt(trackIndex);
    if (!target) {
        return -1;
    }
    if (moving) {
        const Track *origin = trackAt(moving->trackIndex);
        if (!origin || target->isAudio != origin->isAudio || target->isText != origin->isText) {
            return -1;
        }
    }
    if (durationFrames <= 0 || desiredStart < 0) {
        return -1;
    }

    // Collect the occupied intervals on the target track (the moving clip
    // excluded), sorted by start.
    struct Span {
        int64_t start;
        int64_t end;
    };
    std::vector<Span> spans;
    spans.reserve(clips_.size());
    for (const Clip &other : clips_) {
        if (other.id == clipId || other.trackIndex != trackIndex) {
            continue;
        }
        spans.push_back({other.timelineStart, other.timelineEnd()});
    }
    std::sort(spans.begin(), spans.end(),
              [](const Span &a, const Span &b) { return a.start < b.start; });

    // Walk the gaps between/before/after the spans. A gap [g0, g1) can host
    // the drop at candidate start = clamp(desiredStart, g0, g1 - duration).
    // Keep the candidate closest to desiredStart (ties -> earlier).
    int64_t best = -1;
    int64_t bestDist = -1;
    int64_t gapStart = 0;
    for (const Span &s : spans) {
        const int64_t gapEnd = s.start; // exclusive
        if (gapEnd > gapStart && gapEnd - gapStart >= durationFrames) {
            int64_t cand = desiredStart;
            if (cand < gapStart) {
                cand = gapStart;
            }
            if (cand > gapEnd - durationFrames) {
                cand = gapEnd - durationFrames;
            }
            const int64_t dist = cand > desiredStart ? cand - desiredStart : desiredStart - cand;
            if (bestDist < 0 || dist < bestDist) {
                bestDist = dist;
                best = cand;
            }
        }
        if (s.end > gapStart) {
            gapStart = s.end;
        }
    }
    // Trailing gap [gapStart, infinity): the closest hostable point to
    // desiredStart is max(desiredStart, gapStart) itself.
    {
        const int64_t cand = desiredStart > gapStart ? desiredStart : gapStart;
        const int64_t dist = cand > desiredStart ? cand - desiredStart : desiredStart - cand;
        if (bestDist < 0 || dist < bestDist) {
            bestDist = dist;
            best = cand;
        }
    }
    return best;
}

bool TimelineModel::trimClipStart(int64_t id, int64_t deltaFrames) {
    Clip *clip = clipById(id);
    if (!clip) {
        return false;
    }
    if (clip->isText) {
        // Text clips have no source to consume: trimming the head moves
        // the start AND shrinks/grows the synthetic extent (sourceOut
        // moves against the fixed sourceIn = 0), so the head can extend
        // freely - there is no "before the first frame" for generated
        // text. Keep >= 1 frame and timelineStart >= 0.
        const int64_t newSourceOut = clip->sourceOutFrames - deltaFrames;
        const int64_t newStart = clip->timelineStart + deltaFrames;
        if (newSourceOut <= 0 || newStart < 0) {
            return false;
        }
        clip->sourceOutFrames = newSourceOut;
        clip->timelineStart = newStart;
        pruneTransitions();
        return true;
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
    // The clip's END (start + duration) is unchanged, so an outgoing
    // transition survives with its boundary intact; the shrunken duration
    // may clamp it.
    pruneTransitions();
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
    pruneTransitions();
    return true;
}

bool TimelineModel::rippleDelete(int64_t id) {
    const Clip *clip = clipById(id);
    if (!clip) {
        return false;
    }
    const int track = clip->trackIndex;
    const int64_t start = clip->timelineStart;
    const int64_t dur = clip->durationFrames();
    if (!removeClip(id)) {
        return false;
    }
    for (Clip &other : clips_) {
        if (other.trackIndex == track && other.timelineStart >= start + dur) {
            other.timelineStart -= dur;
        }
    }
    // Transitions of later pairs survive the shift (both sides move
    // together); pruning again is idempotent.
    pruneTransitions();
    return true;
}

bool TimelineModel::rippleTrimClipEnd(int64_t id, int64_t deltaFrames) {
    Clip *clip = clipById(id);
    if (!clip || deltaFrames == 0) {
        return false;
    }
    const int track = clip->trackIndex;
    const int64_t origEnd = clip->timelineEnd();
    {
        // Trim WITHOUT pruning: the neighbor shift below re-attaches the
        // right clip to the moved end, so a boundary transition must
        // survive the intermediate state.
        const int64_t sourceDelta =
            static_cast<int64_t>(std::llround(static_cast<double>(deltaFrames) * clip->rate));
        const int64_t newSourceOut = clip->sourceOutFrames + sourceDelta;
        if (newSourceOut <= clip->sourceInFrames) {
            return false;
        }
        clip->sourceOutFrames = newSourceOut;
    }
    // Shift the following clips by exactly the applied delta so they stay
    // attached to the new edge. Non-negative starts are guaranteed: a
    // following clip started at >= origEnd = start + oldDur, and the trim
    // can shrink at most oldDur - 1 frames, so the shifted start stays
    // >= start + 1 > 0 (see the header contract).
    for (Clip &other : clips_) {
        if (other.id != id && other.trackIndex == track && other.timelineStart >= origEnd) {
            other.timelineStart += deltaFrames;
        }
    }
    pruneTransitions(); // clamps the duration to the new left extent
    return true;
}

bool TimelineModel::rollEdit(int64_t leftId, int64_t rightId, int64_t deltaFrames) {
    Clip *left = clipById(leftId);
    Clip *right = clipById(rightId);
    if (!left || !right || left == right || deltaFrames == 0) {
        return false;
    }
    if (left->trackIndex != right->trackIndex) {
        return false;
    }
    if (right->timelineStart != left->timelineEnd()) {
        return false; // must be adjacent
    }
    if (left->isText != right->isText) {
        return false; // kind mismatch (cannot happen via the track rules)
    }
    if (left->isText) {
        // Text pair (rate 1, sourceIn 0 on both): rolling the boundary
        // grows one synthetic extent and shrinks the other. The right
        // clip's head has no source to consume, so its SOURCE OUT moves
        // against the boundary instead of its source in-point.
        if (left->sourceOutFrames + deltaFrames <= 0) {
            return false; // left must keep >= 1 frame
        }
        if (right->sourceOutFrames - deltaFrames <= 0) {
            return false; // right must keep >= 1 frame
        }
        if (right->timelineStart + deltaFrames <= left->timelineStart) {
            return false; // boundary would pass the left clip's start
        }
        left->sourceOutFrames += deltaFrames;
        right->sourceOutFrames -= deltaFrames;
        right->timelineStart += deltaFrames;
        pruneTransitions();
        return true;
    }
    const int64_t leftSrcDelta =
        static_cast<int64_t>(std::llround(static_cast<double>(deltaFrames) * left->rate));
    const int64_t rightSrcDelta =
        static_cast<int64_t>(std::llround(static_cast<double>(deltaFrames) * right->rate));
    // Left must keep >= 1 frame after the move.
    if (left->sourceOutFrames + leftSrcDelta <= left->sourceInFrames) {
        return false;
    }
    // Right must keep >= 1 frame and a non-negative in-point.
    if (right->sourceInFrames + rightSrcDelta < 0) {
        return false;
    }
    if (right->sourceOutFrames <= right->sourceInFrames + rightSrcDelta) {
        return false;
    }
    if (right->timelineStart + deltaFrames <= left->timelineStart) {
        return false; // boundary would pass the left clip's start
    }
    left->sourceOutFrames += leftSrcDelta;
    right->sourceInFrames += rightSrcDelta;
    right->timelineStart += deltaFrames;
    // The boundary moved with BOTH clips (still adjacent); a transition on
    // it survives, its duration clamped to the new left extent.
    pruneTransitions();
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

const Clip *TimelineModel::clipById(int64_t id) const {
    // Delegate to the mutable lookup would drop const on `this`; instead
    // mirror it (the loop body is trivial and const-correct here).
    for (const Clip &clip : clips_) {
        if (clip.id == id) {
            return &clip;
        }
    }
    return nullptr;
}

const Clip *TimelineModel::activeVideoClipAt(int64_t frame) const {
    // Track 0 is the visually topmost video lane (the panel draws rows in
    // index order, V2 above V1); the first hit wins. Text lanes (M6) are
    // NOT video: they composite on top instead of winning the lookup.
    for (int t = 0; t < trackCount(); ++t) {
        const Track *track = trackAt(t);
        if (!track || track->isAudio || track->isText) {
            continue;
        }
        if (const Clip *clip = clipAt(frame, t)) {
            return clip;
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

// ---------------------------------------------------------------------------
// M5 Phase 2: cut transitions.
// ---------------------------------------------------------------------------

int64_t TimelineModel::maxTransitionDuration(int64_t leftClipId, int64_t rightClipId) const {
    const Clip *left = clipById(leftClipId);
    const Clip *right = clipById(rightClipId);
    if (!left || !right || left == right) {
        return 0;
    }
    if (left->trackIndex != right->trackIndex) {
        return 0;
    }
    const Track *track = trackAt(left->trackIndex);
    if (!track || track->isAudio || track->isText) {
        // M6 Phase 1: cut transitions stay on VIDEO lanes - text clips
        // have no decoded stream to hold for the incoming side (their
        // animated compositing arrives with the M6 animation phase).
        return 0;
    }
    if (right->timelineStart != left->timelineEnd()) {
        return 0; // not adjacent
    }
    if (transitionBetween(leftClipId, rightClipId) != nullptr) {
        return 0; // boundary already carries a transition
    }
    return left->durationFrames();
}

int64_t TimelineModel::addTransition(int64_t leftClipId, int64_t rightClipId,
                                     const std::string &kind, int64_t durationFrames) {
    if (kind.empty() || findTransition(kind) == nullptr) {
        return 0;
    }
    const int64_t cap = maxTransitionDuration(leftClipId, rightClipId);
    if (cap <= 0 || durationFrames < 1 || durationFrames > cap) {
        return 0;
    }
    Transition t;
    t.id = nextId_++;
    t.trackIndex = clipById(leftClipId)->trackIndex;
    t.leftClipId = leftClipId;
    t.rightClipId = rightClipId;
    t.kind = kind;
    t.durationFrames = durationFrames;
    transitions_.push_back(t);
    return t.id;
}

bool TimelineModel::removeTransition(int64_t id) {
    for (auto it = transitions_.begin(); it != transitions_.end(); ++it) {
        if (it->id == id) {
            transitions_.erase(it);
            return true;
        }
    }
    return false;
}

bool TimelineModel::setTransitionDuration(int64_t id, int64_t durationFrames) {
    const Transition *t = transitionById(id);
    if (!t) {
        return false;
    }
    const Clip *left = clipById(t->leftClipId);
    if (!left) {
        return false;
    }
    if (durationFrames < 1 || durationFrames > left->durationFrames()) {
        return false;
    }
    // transitionById returned a pointer into the vector; re-find mutably.
    for (Transition &tr : transitions_) {
        if (tr.id == id) {
            tr.durationFrames = durationFrames;
            return true;
        }
    }
    return false;
}

const Transition *TimelineModel::transitionById(int64_t id) const {
    for (const Transition &t : transitions_) {
        if (t.id == id) {
            return &t;
        }
    }
    return nullptr;
}

const Transition *TimelineModel::transitionBetween(int64_t leftClipId, int64_t rightClipId) const {
    for (const Transition &t : transitions_) {
        if (t.leftClipId == leftClipId && t.rightClipId == rightClipId) {
            return &t;
        }
    }
    return nullptr;
}

bool TimelineModel::resolveTransitionAt(int64_t frame, int trackIndex,
                                        TransitionSample &out) const {
    for (const Transition &t : transitions_) {
        if (t.trackIndex != trackIndex || t.durationFrames < 1) {
            continue;
        }
        const Clip *left = clipById(t.leftClipId);
        const Clip *right = clipById(t.rightClipId);
        if (!left || !right || left->trackIndex != trackIndex) {
            continue;
        }
        // Defensive: the prune invariant guarantees duration <= left
        // duration, but resolve must never hand out an out-of-clip window.
        int64_t duration = t.durationFrames;
        if (duration > left->durationFrames()) {
            duration = left->durationFrames();
        }
        if (duration < 1) {
            continue;
        }
        const int64_t boundary = left->timelineEnd();
        const int64_t windowStart = boundary - duration;
        if (frame < windowStart || frame >= boundary) {
            continue;
        }
        out.transitionId = t.id;
        out.trackIndex = trackIndex;
        out.kind = t.kind;
        out.leftClipId = t.leftClipId;
        out.rightClipId = t.rightClipId;
        out.windowStartFrame = windowStart;
        out.windowEndFrame = boundary;
        out.progress = static_cast<double>(frame - windowStart) / static_cast<double>(duration);
        out.leftSourceFrame = left->sourceInFrames +
                              static_cast<int64_t>(std::llround(
                                  static_cast<double>(frame - left->timelineStart) * left->rate));
        out.rightSourceFrame = right->sourceInFrames; // held first frame
        return true;
    }
    return false;
}

void TimelineModel::pruneTransitions() {
    for (auto it = transitions_.begin(); it != transitions_.end();) {
        const Clip *left = clipById(it->leftClipId);
        const Clip *right = clipById(it->rightClipId);
        bool valid = left && right && left != right && left->trackIndex == right->trackIndex &&
                     right->timelineStart == left->timelineEnd();
        if (valid) {
            const int64_t leftDur = left->durationFrames();
            if (it->durationFrames > leftDur) {
                it->durationFrames = leftDur; // clamp: window stays inside
            }
            if (it->durationFrames < 1) {
                valid = false;
            }
        }
        if (!valid) {
            it = transitions_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace fc
