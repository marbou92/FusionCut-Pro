// FusionCut Pro - timeline model unit tests (the editing core).
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

static void testMoveClipTo() {
    TimelineModel model;
    model.addTrack("V2", false);
    model.addTrack("V1", false);
    model.addTrack("A1", true);

    const int64_t v1a = model.addClip(1, "v.mp4", "A", 0, 100, 0);   // V1 0..100
    const int64_t v1b = model.addClip(1, "v.mp4", "B", 0, 100, 150); // V1 150..250

    // Cross-track move to empty V2.
    CHECK(model.moveClipTo(v1a, 0, 0));
    CHECK(model.clips()[0].trackIndex == 0);
    CHECK(model.moveClipTo(v1a, 1, 0)); // and back

    // Overlap rejection on the same track.
    CHECK(!model.moveClipTo(v1a, 1, 100));      // 100..200 overlaps B at 150
    CHECK(model.clips()[0].timelineStart == 0); // unchanged on failure

    // Snug fit between the two clips is legal.
    CHECK(model.moveClipTo(v1a, 1, 50)); // 50..150 ends exactly at B's start
    CHECK(model.clips()[0].timelineStart == 50);

    // Kind mismatch: video clip cannot move to the audio track.
    CHECK(!model.moveClipTo(v1a, 2, 0));
    // Invalid track / negative start / bad id.
    CHECK(!model.moveClipTo(v1a, 99, 0));
    CHECK(!model.moveClipTo(v1a, 0, -1));
    CHECK(!model.moveClipTo(12345, 0, 0));

    // An audio clip cannot move to a video track either.
    const int64_t a1 = model.addClip(2, "a.mp3", "Audio", 0, 50, 0);
    CHECK(!model.moveClipTo(a1, 1, 300));
}

static void testFindDropPosition() {
    TimelineModel model;
    model.addTrack("V2", false);
    model.addTrack("V1", false);
    model.addTrack("A1", true);
    const int64_t a = model.addClip(1, "v.mp4", "A", 0, 100, 0);  // 0..100
    const int64_t b = model.addClip(1, "v.mp4", "B", 0, 50, 150); // 150..200
    // V1 occupied (moving clip excluded): gaps [100,150) and [200,inf).

    // Desired lands in a free gap: exact match.
    CHECK(model.findDropPosition(1, a, 120, 25) == 120);
    // Desired overlaps B: snaps to the far edge of the gap before B.
    CHECK(model.findDropPosition(1, a, 160, 25) == 125); // clamp into [100,125]
    // Moving B: A stays at 0..100, so the nearest hostable spot to the
    // desired position 10 is the trailing gap start (A blocks 0..100).
    CHECK(model.findDropPosition(1, b, 10, 40) == 100);
    // Clip too big for any interior gap: snaps into the trailing gap.
    CHECK(model.findDropPosition(1, a, 40, 200) == 200);
    // The moving clip itself is excluded: dropping onto its own spot works.
    CHECK(model.findDropPosition(1, a, 0, 100) == 0);
    // Wrong-kind target: -1.
    CHECK(model.findDropPosition(2, a, 0, 50) == -1);
    // Bad duration / negative desired: -1.
    CHECK(model.findDropPosition(1, a, 0, 0) == -1);
    CHECK(model.findDropPosition(1, a, -5, 50) == -1);
    // Unknown clip id: -1.
    CHECK(model.findDropPosition(1, 987, 0, 50) == -1);

    // Magnetic snap prefers the gap whose edge is nearest: C excluded,
    // D occupies 110..210; desired 105 with duration 50 fits [0,110) and
    // snaps to end exactly at D's start (60).
    TimelineModel tight;
    tight.addTrack("V1", false);
    const int64_t c1 = tight.addClip(0, "v.mp4", "C", 0, 100, 0); // 0..100
    tight.addClip(0, "v.mp4", "D", 0, 100, 110);                  // 110..210
    CHECK(tight.findDropPosition(0, c1, 105, 50) == 60);
}

static void testRippleDelete() {
    TimelineModel model;
    model.addTrack("V1", false);
    const int64_t a = model.addClip(0, "v.mp4", "A", 0, 40, 0);  // 0..40
    const int64_t b = model.addClip(0, "v.mp4", "B", 0, 40, 50); // 50..90
    const int64_t c = model.addClip(0, "v.mp4", "C", 0, 40, 90); // 90..130

    // Ripple-delete B: C shifts left by 40 (90 -> 50); A is before B and
    // stays put.
    CHECK(model.rippleDelete(b));
    CHECK(model.clips().size() == 2);
    CHECK(model.clipById(a)->timelineStart == 0);
    CHECK(model.clipById(c)->timelineStart == 50);

    // Ripple-delete A: C shifts left again (50 -> 10).
    CHECK(model.rippleDelete(a));
    CHECK(model.clips().size() == 1);
    CHECK(model.clipById(c)->timelineStart == 10);
    CHECK(!model.rippleDelete(9999));
}

static void testRippleTrim() {
    TimelineModel model;
    model.addTrack("V1", false);
    const int64_t a = model.addClip(0, "v.mp4", "A", 0, 100, 0); // 0..100
    model.addClip(0, "v.mp4", "B", 0, 100, 100);                 // 100..200

    // Shrink A by 20: B pulls left to 80.
    CHECK(model.rippleTrimClipEnd(a, -20));
    CHECK(model.clipById(a)->sourceOutFrames == 80);
    CHECK(model.clipById(2)->timelineStart == 80);

    // Extend A by 30: B pushed right to 110.
    CHECK(model.rippleTrimClipEnd(a, 30));
    CHECK(model.clipById(a)->sourceOutFrames == 110);
    CHECK(model.clipById(2)->timelineStart == 110);

    // Collapse to zero is rejected, nothing moves.
    CHECK(!model.rippleTrimClipEnd(a, -200));
    CHECK(model.clipById(2)->timelineStart == 110);
    // Zero delta rejected (no-op is not a successful edit).
    CHECK(!model.rippleTrimClipEnd(a, 0));
    // Only LATER clips shift: a clip on another track stays put.
    model.addTrack("A1", true);
    model.addClip(1, "a.mp3", "Audio", 0, 50, 10);
    CHECK(model.rippleTrimClipEnd(a, -10));
    CHECK(model.clipById(3)->timelineStart == 10); // audio untouched
}

static void testRollEdit() {
    TimelineModel model;
    model.addTrack("V1", false);
    const int64_t a = model.addClip(0, "v.mp4", "A", 0, 100, 0);    // tl 0..100
    const int64_t b = model.addClip(0, "v.mp4", "B", 20, 120, 100); // tl 100..200

    // Roll right by 10: boundary 100 -> 110.
    CHECK(model.rollEdit(a, b, 10));
    CHECK(model.clipById(a)->sourceOutFrames == 110);
    CHECK(model.clipById(b)->sourceInFrames == 30);
    CHECK(model.clipById(b)->timelineStart == 110);
    CHECK(model.durationFrames() == 200); // total length unchanged

    // Roll left by 30: boundary 110 -> 80 (B in lands exactly at 0).
    CHECK(model.rollEdit(a, b, -30));
    CHECK(model.clipById(a)->sourceOutFrames == 80);
    CHECK(model.clipById(b)->sourceInFrames == 0);
    CHECK(model.clipById(b)->timelineStart == 80);

    // Guards from this state (boundary 80, B in 0, B out 120, A in 0):
    CHECK(!model.rollEdit(a, b, -1));                // B in-point would go negative
    CHECK(!model.rollEdit(a, b, -80));               // A would collapse (out 80-80 <= in 0)
    CHECK(!model.rollEdit(a, b, 120));               // B would collapse (in 0+120 == out 120)
    CHECK(model.clipById(a)->sourceOutFrames == 80); // unchanged on failure

    // Non-adjacent clips rejected.
    const int64_t c = model.addClip(0, "v.mp4", "C", 0, 50, 300);
    CHECK(!model.rollEdit(a, c, 10));
    CHECK(!model.rollEdit(b, c, 10));
    // Zero delta + unknown ids.
    CHECK(!model.rollEdit(a, b, 0));
    CHECK(!model.rollEdit(9, b, 10));
}

static void testActiveVideoClipAt() {
    TimelineModel model;
    model.addTrack("V2", false); // index 0 = topmost
    model.addTrack("V1", false); // index 1
    model.addTrack("A1", true);  // index 2

    model.addClip(1, "v1.mp4", "Lower", 0, 100, 0); // V1 0..100
    model.addClip(0, "v2.mp4", "Upper", 0, 50, 20); // V2 20..70

    CHECK(model.activeVideoClipAt(10) != nullptr);
    CHECK(std::string(model.activeVideoClipAt(10)->label) == "Lower"); // only V1
    CHECK(model.activeVideoClipAt(40) != nullptr);
    CHECK(std::string(model.activeVideoClipAt(40)->label) == "Upper"); // V2 wins
    CHECK(model.activeVideoClipAt(90) != nullptr);
    CHECK(std::string(model.activeVideoClipAt(90)->label) == "Lower");
    CHECK(model.activeVideoClipAt(150) == nullptr); // past everything
    CHECK(model.activeVideoClipAt(85) != nullptr);  // V1 only region
}

int main() {
    testTracks();
    testAddClip();
    testSplit();
    testMoveAndTrim();
    testClipAtAndDuration();
    testRemoveClip();
    testMoveClipTo();
    testFindDropPosition();
    testRippleDelete();
    testRippleTrim();
    testRollEdit();
    testActiveVideoClipAt();
    return testExitCode("timeline");
}
