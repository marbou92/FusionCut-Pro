#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fc {

// One placed media segment on the timeline. Source frames [sourceIn,
// sourceOut) play at timeline position [timelineStart, timelineStart +
// duration) where duration = (sourceOut - sourceIn) / rate.
struct Clip {
    int64_t id = 0;
    std::string sourcePath;
    std::string label;
    int64_t sourceInFrames = 0;  // in-point in the source (inclusive)
    int64_t sourceOutFrames = 0; // out-point in the source (exclusive)
    int64_t timelineStart = 0;   // position on the timeline (frames)
    double rate = 1.0;           // playback rate (1.0 = normal)
    int trackIndex = 0;

    int64_t durationFrames() const;
    int64_t timelineEnd() const { return timelineStart + durationFrames(); }
};

struct Track {
    int index = 0;
    std::string name;
    bool isAudio = false;
    bool locked = false;
    bool muted = false;
    bool solo = false;
};

// Frame-accurate timeline model (Module 6.2 / Module 4 editing core).
// Pure data + operations, no Qt, no FFmpeg - unit tested in fc_timeline_tests.
// M4a owned placement, split, trim, and move; M4b adds cross-track moves
// with overlap rejection, magnetic drop resolution, ripple delete/trim,
// rolling boundary edits, and topmost-clip lookup for the program monitor.
// Multi-source compositing beyond topmost-wins and audio mixing arrive in
// later M4/M5 phases.
class TimelineModel {
public:
    TimelineModel() = default;

    void setFps(double fps);
    double fps() const { return fps_; }

    int addTrack(const std::string &name, bool isAudio);
    int trackCount() const { return static_cast<int>(tracks_.size()); }
    const std::vector<Track> &tracks() const { return tracks_; }
    // Mutable lookup (model is non-const; e.g. setTrackState path).
    Track *trackAt(int index);
    // Const lookup - used by const views (TimelinePanel renders against a
    // const TimelineModel*). Required for const-correctness: callers on a
    // const model cannot bind to the non-const overload above.
    const Track *trackAt(int index) const;
    bool setTrackState(int index, bool locked, bool muted, bool solo);

    const std::vector<Clip> &clips() const { return clips_; }

    // Adds a clip; returns its id (>0) or 0 on invalid arguments.
    int64_t addClip(int trackIndex, const std::string &sourcePath, const std::string &label,
                    int64_t sourceInFrames, int64_t sourceOutFrames, int64_t timelineStart,
                    double rate = 1.0);

    bool removeClip(int64_t id);

    // Splits every clip on `trackIndex` whose range contains `frame` (not at
    // a boundary) into two clips. Returns true if any clip was split.
    bool splitAt(int64_t frame, int trackIndex);

    bool moveClip(int64_t id, int64_t newTimelineStart);

    // M4b: move a clip to another track and/or position. Rejects invalid
    // track, negative start, kind mismatch (a clip on a video track cannot
    // move to an audio track and vice versa - clip kind is the kind of its
    // current track), and any overlap with another clip on the target
    // track. Use findDropPosition() first to get a guaranteed-valid start.
    bool moveClipTo(int64_t id, int newTrackIndex, int64_t newTimelineStart);

    // M4b: resolve the nearest valid drop start for a clip of the given
    // duration on the given track (the moving clip itself is excluded by
    // id so an in-place drop is always legal). Returns the start closest
    // to desiredStart that fits without overlapping anything; -1 when no
    // gap on the track can hold the clip or the target track is of the
    // wrong kind for the clip. "Magnetic": the desired position wins if
    // it is free, otherwise the result snaps to the nearest gap edge.
    int64_t findDropPosition(int trackIndex, int64_t clipId, int64_t desiredStart,
                             int64_t durationFrames) const;

    // Trim the start by `delta` timeline frames (positive = lose content
    // from the head, negative = extend). Source in-point and timeline start
    // move together. Honors sourceIn >= 0 and timelineStart >= 0.
    bool trimClipStart(int64_t id, int64_t deltaFrames);

    // Trim the end by `delta` timeline frames (positive = extend, negative =
    // shrink). Honors sourceOut > sourceIn.
    bool trimClipEnd(int64_t id, int64_t deltaFrames);

    // M4b ripple delete: remove the clip and shift every later clip on the
    // same track left by exactly its duration, closing the gap it leaves.
    bool rippleDelete(int64_t id);

    // M4b ripple trim end: trimClipEnd + shift every clip that started at
    // or after the trimmed clip's ORIGINAL end by the same delta (positive
    // delta pushes the following clips right, negative pulls them left).
    // The gap after the trimmed edge closes/opens; later gaps are kept.
    bool rippleTrimClipEnd(int64_t id, int64_t deltaFrames);

    // M4b rolling edit: move the shared boundary of two adjacent clips on
    // one track by `delta` timeline frames. The left clip's out-point and
    // the right clip's in-point + timeline start all shift by delta; the
    // total sequence length is unchanged. Rejects non-adjacent clips and
    // any delta that would collapse either clip to zero length or push the
    // right clip's source in-point negative.
    bool rollEdit(int64_t leftId, int64_t rightId, int64_t deltaFrames);

    const Clip *clipAt(int64_t frame, int trackIndex) const;
    Clip *clipById(int64_t id);
    // Const lookup for const views (findDropPosition resolves the moving
    // clip's kind from a const model).
    const Clip *clipById(int64_t id) const;

    // M4b: the clip the program monitor should show at a timeline frame -
    // the clip on the visually TOPMOST video lane that covers the frame
    // (video tracks are drawn bottom-up; track 0 is the topmost lane, so
    // the scan runs from index 0 downward). Returns nullptr in gaps.
    const Clip *activeVideoClipAt(int64_t frame) const;

    int64_t durationFrames() const;
    double durationSeconds() const;

private:
    double fps_ = 24.0;
    std::vector<Track> tracks_;
    std::vector<Clip> clips_;
    int64_t nextId_ = 1;
};

} // namespace fc
