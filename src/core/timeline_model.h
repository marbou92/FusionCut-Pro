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
// Multi-track compositing, ripple/rolling, and audio mixing arrive in later
// M4 phases; this layer owns placement, split, trim, and move semantics.
class TimelineModel {
public:
    TimelineModel() = default;

    void setFps(double fps);
    double fps() const { return fps_; }

    int addTrack(const std::string &name, bool isAudio);
    int trackCount() const { return static_cast<int>(tracks_.size()); }
    const std::vector<Track> &tracks() const { return tracks_; }
    Track *trackAt(int index);
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

    // Trim the start by `delta` timeline frames (positive = lose content
    // from the head, negative = extend). Source in-point and timeline start
    // move together. Honors sourceIn >= 0 and timelineStart >= 0.
    bool trimClipStart(int64_t id, int64_t deltaFrames);

    // Trim the end by `delta` timeline frames (positive = extend, negative =
    // shrink). Honors sourceOut > sourceIn.
    bool trimClipEnd(int64_t id, int64_t deltaFrames);

    const Clip *clipAt(int64_t frame, int trackIndex) const;
    Clip *clipById(int64_t id);

    int64_t durationFrames() const;
    double durationSeconds() const;

private:
    double fps_ = 24.0;
    std::vector<Track> tracks_;
    std::vector<Clip> clips_;
    int64_t nextId_ = 1;
};

} // namespace fc
