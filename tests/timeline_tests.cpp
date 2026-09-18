// FusionCut Pro - timeline model unit tests (the editing core).
// Pure data + operations; no Qt, no FFmpeg - runs anywhere ctest runs.

#include <cmath>
#include <limits>
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
    CHECK(v1a > 0);
    CHECK(v1b > 0);

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

// ---- clip playback rate (setClipRate) ----
static void testSetClipRate() {
    TimelineModel model;
    model.addTrack("V1", false);
    model.addTrack("A1", true);
    const int textTrack = model.insertTrack(2, "T1", false, true);
    CHECK(textTrack == 2);

    // Success at 2.0: the timeline duration halves, the source extent and
    // the anchored start stay put, and the edit dirties the model.
    const int64_t a = model.addClip(0, "v.mp4", "A", 100, 200, 40); // tl 40..140
    CHECK(a > 0);
    const uint64_t rev0 = model.revision();
    CHECK(model.setClipRate(a, 2.0));
    CHECK(model.revision() == rev0 + 1);
    CHECK(model.clipById(a)->durationFrames() == 50); // llround(100/2.0)
    CHECK(model.clipById(a)->timelineStart == 40);
    CHECK(model.clipById(a)->sourceInFrames == 100);
    CHECK(model.clipById(a)->sourceOutFrames == 200);
    CHECK(model.clipById(a)->timelineEnd() == 90);

    // Success at 0.5: the extent doubles (no follower - room to grow).
    CHECK(model.setClipRate(a, 0.5));
    CHECK(model.clipById(a)->durationFrames() == 200);
    CHECK(model.clipById(a)->timelineStart == 40);
    CHECK(model.clipById(a)->sourceInFrames == 100);
    CHECK(model.clipById(a)->sourceOutFrames == 200);

    // llround pin on a 3-frame source extent: 2.0 -> 2 (1.5 rounds away
    // from zero), 1.5 -> 2 (exact), 0.75 -> 4 (exact).
    const int64_t b = model.addClip(0, "v.mp4", "B", 500, 503, 1000); // tl 1000..1003
    CHECK(b > 0);
    CHECK(model.clipById(b)->durationFrames() == 3);
    CHECK(model.setClipRate(b, 2.0));
    CHECK(model.clipById(b)->durationFrames() == 2);
    CHECK(model.clipById(b)->timelineStart == 1000);
    CHECK(model.setClipRate(b, 1.5));
    CHECK(model.clipById(b)->durationFrames() == 2);
    CHECK(model.setClipRate(b, 0.75));
    CHECK(model.clipById(b)->durationFrames() == 4);

    // Failure pairs: every rejection leaves the rate AND the revision
    // exactly as they were.
    const uint64_t revFail = model.revision();
    CHECK(!model.setClipRate(9999, 2.0)); // unknown id
    CHECK(!model.setClipRate(b, 0.0));    // rate 0
    CHECK(!model.setClipRate(b, -2.0));   // negative rate
    CHECK(!model.setClipRate(b, std::numeric_limits<double>::quiet_NaN()));
    CHECK(!model.setClipRate(b, std::numeric_limits<double>::infinity()));
    CHECK(model.clipById(b)->rate == 0.75);
    CHECK(model.revision() == revFail);

    // Text clips have generated content - no speed.
    TextDocument doc;
    const int64_t t = model.addTextClip(textTrack, doc, 0, 30);
    CHECK(t > 0);
    const uint64_t revText = model.revision();
    CHECK(!model.setClipRate(t, 2.0));
    CHECK(model.clipById(t)->rate == 1.0);
    CHECK(model.revision() == revText);

    // Zombie guard: a 1-frame extent collapses to zero timeline frames at
    // a huge (but finite) rate - rejected, same as the trims.
    const int64_t z = model.addClip(0, "v.mp4", "Z", 900, 901, 2000); // tl 2000..2001
    CHECK(z > 0);
    const uint64_t revZ = model.revision();
    CHECK(!model.setClipRate(z, 1.0e9)); // llround(1e-9) = 0
    CHECK(model.clipById(z)->rate == 1.0);
    CHECK(model.clipById(z)->durationFrames() == 1);
    CHECK(model.revision() == revZ);

    // Overlap rejection: slowing a clip into a follower is rejected and
    // changes nothing; speeding up away from it opens a gap.
    TimelineModel m2;
    m2.addTrack("V1", false);
    const int64_t l = m2.addClip(0, "v.mp4", "L", 0, 100, 0);   // 0..100
    const int64_t f = m2.addClip(0, "v.mp4", "F", 0, 100, 100); // 100..200
    CHECK(l > 0 && f > 0);
    const uint64_t rev2 = m2.revision();
    CHECK(!m2.setClipRate(l, 0.5)); // would grow 0..100 into 0..200
    CHECK(m2.clipById(l)->rate == 1.0);
    CHECK(m2.clipById(l)->durationFrames() == 100);
    CHECK(m2.revision() == rev2);
    CHECK(m2.setClipRate(l, 2.0));
    CHECK(m2.clipById(l)->durationFrames() == 50);
    CHECK(m2.clipById(l)->timelineEnd() == 50);
    CHECK(m2.clipById(f)->timelineStart == 100); // follower untouched
    CHECK(m2.clipAt(75, 0) == nullptr);          // the gap [50, 100)

    // Head/tail nuance: growth is forward-only, so a clip can reach the
    // tail of a clip that PRECEDES it in the clips vector. That is an
    // overlap like any other - rejected - while landing exactly on the
    // previous clip's head is legal (touching is not overlapping).
    TimelineModel m3;
    m3.addTrack("V1", false);
    const int64_t p = m3.addClip(0, "v.mp4", "P", 0, 100, 200); // added first, tl 200..300
    const int64_t q = m3.addClip(0, "v.mp4", "Q", 0, 200, 0);   // tl 0..200, touching P
    CHECK(p > 0 && q > 0);
    CHECK(m3.clips()[0].id == p);    // the previous clip comes first in clips
    CHECK(m3.setClipRate(q, 1.002)); // llround(200/1.002) = 200: end stays on P's head
    CHECK(m3.clipById(q)->durationFrames() == 200);
    CHECK(m3.clipById(q)->timelineEnd() == 200);
    CHECK(!m3.setClipRate(q, 0.5)); // grows into P's tail
    CHECK(m3.clipById(q)->rate == 1.002);
    // ...and an ADJACENT previous clip at the head must never cause a
    // false rejection either (only the follower side can be hit).
    TimelineModel m4;
    m4.addTrack("V1", false);
    m4.addClip(0, "v.mp4", "H", 0, 100, 0);                     // 0..100
    const int64_t g = m4.addClip(0, "v.mp4", "G", 0, 100, 100); // 100..200
    CHECK(g > 0);
    CHECK(m4.setClipRate(g, 0.5)); // grows to 100..300, H adjacent before it
    CHECK(m4.clipById(g)->durationFrames() == 200);
    CHECK(m4.clipById(g)->timelineStart == 100);

    // Transitions: a rate change that keeps the boundary adjacent keeps
    // the transition (duration still fits -> kept); a speed-up that
    // breaks adjacency prunes it, same policy as the trims.
    TimelineModel m5;
    m5.addTrack("V1", false);
    const int64_t c1 = m5.addClip(0, "v.mp4", "C1", 0, 100, 0);   // 0..100
    const int64_t c2 = m5.addClip(0, "v.mp4", "C2", 0, 100, 100); // 100..200
    CHECK(c1 > 0 && c2 > 0);
    CHECK(m5.addTransition(c1, c2, "dissolve.cross", 40) > 0);
    CHECK(m5.setClipRate(c1, 1.004)); // llround(100/1.004) = 100: end stays put
    CHECK(m5.clipById(c1)->durationFrames() == 100);
    CHECK(m5.transitions().size() == 1);
    CHECK(m5.transitions()[0].durationFrames == 40); // fits -> kept
    CHECK(m5.setClipRate(c2, 2.0));                  // right side: start anchored, boundary intact
    CHECK(m5.transitions().size() == 1);
    CHECK(m5.transitions()[0].durationFrames == 40);
    CHECK(m5.setClipRate(c1, 2.0)); // C1 -> 0..50: adjacency to C2 breaks
    CHECK(m5.transitions().empty());

    // Interplay: splitting a rate-2 clip keeps the rate on BOTH halves
    // with rate-correct source halves; one timeline frame of trim-start
    // consumes rate source frames.
    TimelineModel m6;
    m6.addTrack("V1", false);
    const int64_t s = m6.addClip(0, "v.mp4", "S", 0, 200, 0, 2.0); // 0..100, rate 2
    CHECK(s > 0);
    CHECK(m6.clipById(s)->durationFrames() == 100);
    CHECK(m6.splitAt(50, 0));
    CHECK(m6.clips().size() == 2);
    const Clip &sl = m6.clips()[0];
    const Clip &sr = m6.clips()[1];
    CHECK(sl.rate == 2.0 && sr.rate == 2.0);
    CHECK(sl.sourceInFrames == 0 && sl.sourceOutFrames == 100);
    CHECK(sr.sourceInFrames == 100 && sr.sourceOutFrames == 200);
    CHECK(sl.durationFrames() == 50 && sr.durationFrames() == 50);
    CHECK(sl.timelineStart == 0 && sr.timelineStart == 50);
    CHECK(m6.trimClipStart(sr.id, 1));
    CHECK(m6.clipById(sr.id)->sourceInFrames == 102); // 1 * rate 2
    CHECK(m6.clipById(sr.id)->timelineStart == 51);
    CHECK(m6.clipById(sr.id)->durationFrames() == 49); // llround(98/2)

    // The snapshot round-trip carries the rate exactly.
    const TimelineModel snap = m6.snapshot();
    CHECK(m6.setClipRate(sr.id, 1.0));
    CHECK(m6.clipById(sr.id)->rate == 1.0);
    m6.restoreSnapshot(snap);
    CHECK(m6.clipById(sr.id)->rate == 2.0);
    CHECK(m6.clipById(sr.id)->sourceInFrames == 102);
    CHECK(m6.clipById(sr.id)->durationFrames() == 49);

    // Audio fades are TIMELINE frames: they ride a rate change unchanged.
    TimelineModel m7;
    m7.addTrack("A1", true);
    const int64_t w = m7.addClip(0, "a.wav", "W", 0, 200, 0); // 0..200
    CHECK(w > 0);
    CHECK(m7.setClipAudioFades(w, 10, 20));
    CHECK(m7.setClipRate(w, 2.0));
    CHECK(m7.clipById(w)->durationFrames() == 100);
    CHECK(m7.clipById(w)->fadeInFrames == 10);
    CHECK(m7.clipById(w)->fadeOutFrames == 20);
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

// Regression armor for the shipped fixes: the setTrackState no-op
// revision suppression, and the 0-timeline-frame "zombie" guards
// (a rate > 1 can collapse a positive source extent to zero frames).
static void testGuardsAndRevisions() {
    TimelineModel model;
    model.addTrack("V1", false);
    model.addClip(0, "v.mp4", "A", 0, 100, 0);

    // Writing the SAME track state must not dirty the project (the
    // core no-op suppression the app's mixer sync relies on).
    const uint64_t before = model.revision();
    CHECK(model.setTrackState(0, false, false, false)); // already reads this
    CHECK(model.revision() == before);
    CHECK(model.setTrackState(0, true, false, false)); // real change bumps
    CHECK(model.revision() == before + 1);
    CHECK(model.setTrackState(0, true, false, false)); // writing it again
    CHECK(model.revision() == before + 1);

    // ---- addClip rejections (every lane is non-overlapping; no
    // 0-timeline-frame zombies).----
    CHECK(model.addClip(0, "v.mp4", "overlap", 0, 50, 50) == 0);     // 50..100 hits A
    CHECK(model.addClip(0, "v.mp4", "zombie", 0, 1, 200, 3.0) == 0); // llround(1/3)=0
    // rate 7: a 10-source-frame clip lasts 1 timeline frame, and ONE
    // timeline frame of trim moves the source by 7 - the surviving
    // extent 3 rounds llround(3/7) to a 0-frame zombie.
    const int64_t r7 = model.addClip(0, "v.mp4", "R7", 0, 10, 150, 7.0); // 150..151
    CHECK(r7 > 0);
    CHECK(model.clipById(r7)->durationFrames() == 1);

    // ---- trimClipEnd into a zombie.----
    CHECK(!model.trimClipEnd(r7, -1)); // extent 10-7=3 -> llround(3/7)=0
    CHECK(model.clipById(r7)->sourceOutFrames == 10);
    CHECK(model.clipById(r7)->durationFrames() == 1);

    // ---- trimClipStart into a zombie.----
    const int64_t s = model.addClip(0, "v.mp4", "S", 200, 210, 250, 7.0); // 250..251
    CHECK(s > 0);
    CHECK(!model.trimClipStart(s, 1)); // newIn 207: extent 3 -> llround(3/7)=0
    CHECK(model.clipById(s)->sourceInFrames == 200);
    CHECK(model.clipById(s)->durationFrames() == 1);

    // ---- rollEdit into a zombie on the RIGHT side: the incoming clip's
    // surviving source extent rounds to zero timeline frames.----
    TimelineModel m2;
    m2.addTrack("V1", false);
    const int64_t e = m2.addClip(0, "v.mp4", "E", 0, 12, 0, 3.0);  // 0..4
    const int64_t f = m2.addClip(0, "v.mp4", "F", 12, 16, 4, 3.0); // 4..5
    CHECK(e > 0 && f > 0);
    CHECK(!m2.rollEdit(e, f, 1)); // right keeps extent 1 -> llround(1/3)=0
    CHECK(m2.clipById(f)->sourceInFrames == 12);
    CHECK(m2.clipById(f)->durationFrames() == 1);
    // A delta of 0 is rejected outright (nothing to roll).
    CHECK(!m2.rollEdit(e, f, 0));
    CHECK(m2.clipById(f)->sourceInFrames == 12);
}

// ---- undo/redo snapshots ----
static void testSnapshotRestore() {
    TimelineModel model;
    model.addTrack("V1", false);
    model.addTrack("A1", true);
    const int64_t a = model.addClip(0, "v.mp4", "A", 0, 48, 0);
    CHECK(a > 0);
    const uint64_t revAtSnapshot = model.revision();

    // The snapshot is a plain deep copy: mutating afterwards leaves it
    // frozen at the old state.
    const TimelineModel snap = model.snapshot();
    const int64_t b = model.addClip(0, "v.mp4", "B", 48, 96, 48);
    CHECK(b > 0);
    CHECK(model.clips().size() == 2);
    CHECK(snap.clips().size() == 1);
    CHECK(model.revision() > revAtSnapshot);

    // Restore brings the exact state back...
    model.restoreSnapshot(snap);
    CHECK(model.clips().size() == 1);
    CHECK(model.clipById(a) != nullptr);
    CHECK(model.clipById(b) == nullptr);
    CHECK(model.fps() == snap.fps());
    CHECK(model.tracks().size() == snap.tracks().size());
    CHECK(model.trackAt(0)->locked == snap.trackAt(0)->locked);
    CHECK(model.durationFrames() == snap.durationFrames());
    CHECK(model.masterGainDb() == snap.masterGainDb());

    // ...and the revision stays MONOTONE: views that already saw the
    // newer revisions must still see the restore as a change (a plain
    // assignment would run the counter backwards past what they
    // observed, and the restore would look like a no-op).
    CHECK(model.revision() > revAtSnapshot);

    // Restoring twice in a row keeps the monotone guarantee.
    const uint64_t afterFirst = model.revision();
    model.restoreSnapshot(snap);
    CHECK(model.revision() > afterFirst);

    // Mutations after a restore keep working.
    const int64_t c = model.addClip(0, "v.mp4", "C", 48, 96, 48);
    CHECK(c > 0);
    CHECK(model.clips().size() == 2);

    // Audio state rides the snapshot: track strips + the master fader.
    TimelineModel m2;
    m2.addTrack("A1", true);
    const int64_t t = m2.addClip(0, "a.wav", "T", 0, 24, 0);
    CHECK(t > 0);
    CHECK(m2.setTrackAudio(0, -3.0, 0.5));
    m2.setMasterGainDb(-6.0);
    const TimelineModel audioSnap = m2.snapshot();
    CHECK(m2.setTrackAudio(0, 0.0, 0.0));
    m2.setMasterGainDb(3.0);
    m2.restoreSnapshot(audioSnap);
    CHECK(m2.trackAt(0)->gainDb == -3.0);
    CHECK(m2.trackAt(0)->pan == 0.5);
    CHECK(m2.masterGainDb() == -6.0);
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
    testSetClipRate();
    testActiveVideoClipAt();
    testGuardsAndRevisions();
    testSnapshotRestore();
    return testExitCode("timeline");
}
