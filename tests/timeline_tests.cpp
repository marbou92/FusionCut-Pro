// FusionCut Pro - timeline model unit tests (Module 4 editing core).
// Pure data + operations; no Qt, no FFmpeg - runs anywhere ctest runs.

#include <cmath>
#include <string>

#include "test_harness.h"
#include "timeline_model.h"

using namespace fc;

static void testTracks() {
    TimelineModel model;
    CHECK(model.trackCount() == 0);

    const int v2 = model.addTrack("V2", false);
    const int v1 = model.addTrack("V1", false);
    const int a1 = model.addTrack("A1", true);
    CHECK(v2 == 0);
    CHECK(v1 == 1);
    CHECK(a1 == 2);
    CHECK(model.trackCount() == 3);

    CHECK(model.trackAt(2) != nullptr);
    CHECK(model.trackAt(2)->isAudio);
    CHECK(model.trackAt(1)->name == "V1");

    CHECK(model.setTrackState(0, true, false, false));
    CHECK(model.trackAt(0)->locked);
    CHECK(!model.setTrackState(99, true, true, true)); // out of range
}

static void testAddClip() {
    TimelineModel model;
    model.addTrack("V1", false);
    model.setFps(24.0);

    const int64_t id = model.addClip(0, "video.mp4", "Clip A", 0, 240, 0); // 10s at 24fps
    CHECK(id > 0);
    CHECK(model.clips().size() == 1);
    CHECK(model.clips()[0].label == "Clip A");
    CHECK(model.clips()[0].durationFrames() == 240);
    CHECK(model.clips()[0].timelineEnd() == 240);
    CHECK(std::fabs(model.durationSeconds() - 10.0) < 1e-9);

    // Invalid: out <= in
    CHECK(model.addClip(0, "x", "bad", 100, 100, 0) == 0);
    // Invalid: negative start
    CHECK(model.addClip(0, "x", "bad", 0, 10, -1) == 0);
    // Invalid: bad track
    CHECK(model.addClip(99, "x", "bad", 0, 10, 0) == 0);
    // Invalid: zero/negative rate
    CHECK(model.addClip(0, "x", "bad", 0, 10, 0, 0.0) == 0);
}

static void testSplit() {
    TimelineModel model;
    model.addTrack("V1", false);
    model.setFps(25.0);

    model.addClip(0, "v.mp4", "A", 0, 100, 0); // 4s at 25fps, frames 0..100
    CHECK(model.clips().size() == 1);

    // Split at frame 50: two clips [0,50) and [50,100).
    CHECK(model.splitAt(50, 0));
    CHECK(model.clips().size() == 2);
    const Clip &left = model.clips()[0];
    const Clip &right = model.clips()[1];
    CHECK(left.timelineStart == 0);
    CHECK(left.durationFrames() == 50);
    CHECK(left.sourceOutFrames == 50);
    CHECK(right.timelineStart == 50);
    CHECK(right.sourceInFrames == 50);
    CHECK(right.sourceOutFrames == 100);
    CHECK(right.label == "A (2)");

    // Split at a boundary: no-op.
    CHECK(!model.splitAt(0, 0));
    CHECK(!model.splitAt(50, 0));
    CHECK(!model.splitAt(100, 0));
    CHECK(model.clips().size() == 2);

    // Split on the wrong track: no-op.
    model.addTrack("A1", true);
    CHECK(!model.splitAt(25, 1));
}

static void testMoveAndTrim() {
    TimelineModel model;
    model.addTrack("V1", false);
    const int64_t id = model.addClip(0, "v.mp4", "A", 100, 200, 0); // 100 frames
    CHECK(id > 0);

    CHECK(model.moveClip(id, 240));
    CHECK(model.clips()[0].timelineStart == 240);
    CHECK(!model.moveClip(id, -1)); // negative rejected

    CHECK(model.trimClipEnd(id, 50)); // extend end by 50 timeline frames
    CHECK(model.clips()[0].sourceOutFrames == 250);
    CHECK(!model.trimClipEnd(id, -300));            // would collapse to <= in
    CHECK(model.clips()[0].sourceOutFrames == 250); // unchanged on failure

    CHECK(model.trimClipStart(id, 10));            // lose 10 from head
    CHECK(model.clips()[0].sourceInFrames == 110); // rate 1.0
    CHECK(model.clips()[0].timelineStart == 250);
    CHECK(!model.trimClipStart(id, -1000000)); // sourceIn would go negative
}

static void testClipAtAndDuration() {
    TimelineModel model;
    model.addTrack("V1", false);
    model.addTrack("A1", true);
    model.addClip(0, "v.mp4", "A", 0, 100, 0);     // frames 0..100
    model.addClip(0, "v.mp4", "B", 0, 50, 100);    // frames 100..150
    model.addClip(1, "a.mp3", "Audio", 0, 200, 0); // audio frames 0..200

    CHECK(model.clipAt(0, 0) != nullptr);
    CHECK(model.clipAt(0, 0)->label == "A");
    CHECK(model.clipAt(99, 0)->label == "A");
    CHECK(model.clipAt(100, 0)->label == "B");
    CHECK(model.clipAt(149, 0)->label == "B");
    CHECK(model.clipAt(150, 0) == nullptr); // gap
    CHECK(model.clipAt(75, 1) != nullptr);  // audio track
    CHECK(model.clipAt(75, 1)->label == "Audio");

    CHECK(model.durationFrames() == 200); // audio is longest
    CHECK(model.clipById(999) == nullptr);
    CHECK(model.clipById(2) != nullptr);
}

static void testRemoveClip() {
    TimelineModel model;
    model.addTrack("V1", false);
    const int64_t a = model.addClip(0, "v.mp4", "A", 0, 10, 0);
    const int64_t b = model.addClip(0, "v.mp4", "B", 0, 10, 10);
    CHECK(model.clips().size() == 2);
    CHECK(model.removeClip(a));
    CHECK(model.clips().size() == 1);
    CHECK(model.clips()[0].id == b);
    CHECK(!model.removeClip(9999));
}

int main() {
    testTracks();
    testAddClip();
    testSplit();
    testMoveAndTrim();
    testClipAtAndDuration();
    testRemoveClip();
    return testExitCode("timeline");
}
