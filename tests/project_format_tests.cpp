// FusionCut Pro - project persistence unit tests (6th ctest
// suite). The JSON codec and the project round-trip are pure functions:
// every check is deterministic. Round-trips use exactly-representable
// doubles (halves, integers) so field-by-field equality is exact.

#include <cmath>
#include <string>
#include <vector>

#include "effects.h"
#include "project_format.h"
#include "test_harness.h"
#include "timeline_model.h"

using namespace fc;

namespace {

// Builds a model exercising every serialized feature: 3 tracks (one
// locked audio), 3 clips (stacks incl. keyframes, a disabled instance,
// an unknown forward-compat id), 1 transition.
TimelineModel richModel() {
    TimelineModel m;
    m.setFps(30.0);
    m.addTrack("V2", false);
    m.addTrack("V1", false);
    m.addTrack("A1", true);
    m.setTrackState(2, true, true, false);

    const int64_t a = m.addClip(1, "C:/media/a.mp4", "a", 0, 120, 0);
    const int64_t b = m.addClip(1, "C:/media/b.mp4", "b", 0, 90, 120);
    const int64_t c = m.addClip(0, "C:/media/c.mp4", "c", 10, 70, 210, 0.5);

    Clip *ca = m.clipById(a);
    {
        EffectInstance bright = makeEffectInstance("color.brightness");
        bright.setParam("amount", 0.25);
        bright.setKeyframe("amount", 0, 0.0);
        bright.setKeyframe("amount", 60, 0.5);
        bright.setKeyframe("amount", 119, 1.0);
        ca->effectStack.push_back(bright);

        EffectInstance corrector = makeEffectInstance("color.corrector");
        corrector.setParam("exposure", -0.5);
        corrector.setParam("temperature", 0.75);
        ca->effectStack.push_back(corrector);

        EffectInstance off = makeEffectInstance("filter.grain");
        off.enabled = false;
        off.setParam("amount", 0.125);
        ca->effectStack.push_back(off);

        // Forward compatibility: an id this catalog does not know.
        EffectInstance future;
        future.effectId = "color.quantumWarp";
        future.enabled = true;
        future.values = {1.0, 2.0, 3.5};
        ca->effectStack.push_back(future);
    }
    (void)b;
    (void)c;

    m.addTransition(a, b, "dissolve.cross", 12);
    return m;
}

bool sameClip(const Clip &a, const Clip &b) {
    if (!(a.id == b.id && a.sourcePath == b.sourcePath && a.label == b.label &&
          a.sourceInFrames == b.sourceInFrames && a.sourceOutFrames == b.sourceOutFrames &&
          a.timelineStart == b.timelineStart && a.rate == b.rate && a.trackIndex == b.trackIndex &&
          a.effectStack.size() == b.effectStack.size())) {
        return false;
    }
    for (size_t i = 0; i < a.effectStack.size(); ++i) {
        const EffectInstance &fa = a.effectStack[i];
        const EffectInstance &fb = b.effectStack[i];
        if (fa.effectId != fb.effectId || fa.enabled != fb.enabled ||
            fa.values.size() != fb.values.size()) {
            return false;
        }
        for (size_t v = 0; v < fa.values.size(); ++v) {
            if (fa.values[v] != fb.values[v]) {
                return false;
            }
        }
        if (fa.keyframes.size() != fb.keyframes.size()) {
            return false;
        }
        for (size_t t = 0; t < fa.keyframes.size(); ++t) {
            if (fa.keyframes[t].key != fb.keyframes[t].key ||
                fa.keyframes[t].points != fb.keyframes[t].points) {
                return false;
            }
        }
    }
    return true;
}

void testRoundTrip() {
    TimelineModel m = richModel();
    const std::string text = serializeProject(m);

    // Determinism: same model -> byte-identical output.
    CHECK(serializeProject(m) == text);

    TimelineModel loaded;
    std::string error;
    CHECK(parseProject(text, loaded, error));
    CHECK(error.empty());

    CHECK(loaded.fps() == 30.0);
    CHECK(loaded.trackCount() == 3);
    CHECK(loaded.tracks()[0].name == "V2");
    CHECK(loaded.tracks()[1].name == "V1");
    CHECK(loaded.tracks()[2].name == "A1");
    CHECK(loaded.tracks()[2].isAudio);
    CHECK(loaded.tracks()[2].locked);
    CHECK(loaded.tracks()[2].muted);

    CHECK(loaded.clips().size() == m.clips().size());
    for (size_t i = 0; i < m.clips().size(); ++i) {
        CHECK(sameClip(m.clips()[i], loaded.clips()[i]));
    }

    // Ids are preserved exactly.
    CHECK(loaded.clips()[0].id == m.clips()[0].id);
    CHECK(loaded.clips()[1].id == m.clips()[1].id);
    CHECK(loaded.clips()[2].id == m.clips()[2].id);

    // The unknown effect survived with its positional values.
    const Clip *a = loaded.clipById(m.clips()[0].id);
    CHECK(a->effectStack.size() == 4);
    CHECK(a->effectStack[3].effectId == "color.quantumWarp");
    CHECK(a->effectStack[3].values.size() == 3);
    CHECK(a->effectStack[3].values[0] == 1.0);
    CHECK(a->effectStack[3].values[1] == 2.0);
    CHECK(a->effectStack[3].values[2] == 3.5);
    CHECK(a->effectStack[2].enabled == false);
    CHECK(a->effectStack[2].values[0] == 0.125);

    // Keyframe track survived exactly.
    const EffectInstance &bright = a->effectStack[0];
    const std::vector<EffectKeyframe> *track = bright.keyframeTrack("amount");
    CHECK(track != nullptr);
    if (track) {
        CHECK(track->size() == 3);
        CHECK((*track)[0].frame == 0 && (*track)[0].value == 0.0);
        CHECK((*track)[1].frame == 60 && (*track)[1].value == 0.5);
        CHECK((*track)[2].frame == 119 && (*track)[2].value == 1.0);
    }

    // The transition survived with the same ids and linkage.
    CHECK(loaded.transitions().size() == 1);
    const Transition *t = loaded.transitionById(m.transitions()[0].id);
    CHECK(t != nullptr);
    if (t) {
        CHECK(t->kind == "dissolve.cross");
        CHECK(t->durationFrames == 12);
        CHECK(t->leftClipId == m.transitions()[0].leftClipId);
        CHECK(t->rightClipId == m.transitions()[0].rightClipId);
        CHECK(t->trackIndex == m.transitions()[0].trackIndex);
    }

    // Reload of the serialized project is a fixed point: serializing the
    // reload reproduces the same bytes (ids and field order are stable).
    const std::string text2 = serializeProject(loaded);
    TimelineModel loaded2;
    std::string error2;
    CHECK(parseProject(text2, loaded2, error2));
    CHECK(serializeProject(loaded2) == text2);

    // The reloaded model mints ids above every loaded one.
    const int64_t fresh = loaded.addClip(1, "x", "x", 0, 10, 400);
    CHECK(fresh > loaded.clips()[2].id);
    CHECK(fresh > loaded.transitions()[0].id);
}

void testParserStrictness() {
    TimelineModel m;
    std::string error;

    // Empty / garbage / truncated.
    CHECK(!parseProject("", m, error));
    CHECK(!parseProject("null", m, error));
    CHECK(!parseProject("{", m, error));
    CHECK(!parseProject("{\"format\":1,}", m, error));
    CHECK(!parseProject("{\"format\":1 \"fps\":24}", m, error));
    CHECK(!parseProject(serializeProject(richModel()).substr(0, 30), m, error));
    // Trailing whitespace after a complete value is legal JSON.
    error.clear();
    CHECK(parseProject(serializeProject(richModel()) + "\n", m, error));

    // Wrong format version.
    CHECK(!parseProject("{\"format\":2,\"fps\":24,\"tracks\":[]}", m, error));

    // Missing / wrongly typed fields.
    CHECK(!parseProject("{\"format\":1,\"tracks\":[]}", m, error)); // no fps
    CHECK(!parseProject("{\"format\":1,\"fps\":\"24\",\"tracks\":[]}", m, error));
    CHECK(!parseProject("{\"format\":1,\"fps\":0,\"tracks\":[]}", m, error));  // fps <= 0
    CHECK(!parseProject("{\"format\":1,\"fps\":24}", m, error));               // no tracks
    CHECK(!parseProject("{\"format\":1,\"fps\":24,\"tracks\":[]}", m, error)); // empty tracks
    CHECK(!parseProject(
        "{\"format\":1,\"fps\":24,\"tracks\":[{\"name\":\"V\",\"audio\":false,"
        "\"locked\":false,\"muted\":false,\"solo\":false}],\"clips\":[{\"id\":1,"
        "\"track\":5,\"source\":\"a\",\"label\":\"a\",\"in\":0,\"out\":10,\"start\":0,"
        "\"rate\":1}]}",
        m, error)); // dangling track
    CHECK(!parseProject(
        "{\"format\":1,\"fps\":24,\"tracks\":[{\"name\":\"V\",\"audio\":false,"
        "\"locked\":false,\"muted\":false,\"solo\":false}],\"clips\":[{\"id\":1,"
        "\"track\":0,\"source\":\"a\",\"label\":\"a\",\"in\":10,\"out\":10,\"start\":0,"
        "\"rate\":1}]}",
        m, error)); // out <= in
    CHECK(!parseProject(
        "{\"format\":1,\"fps\":24,\"tracks\":[{\"name\":\"V\",\"audio\":false,"
        "\"locked\":false,\"muted\":false,\"solo\":false}],\"clips\":[{\"id\":0,"
        "\"track\":0,\"source\":\"a\",\"label\":\"a\",\"in\":0,\"out\":10,\"start\":0,"
        "\"rate\":1}]}",
        m, error)); // id <= 0
    CHECK(!parseProject(
        "{\"format\":1,\"fps\":24,\"tracks\":[{\"name\":\"V\",\"audio\":false,"
        "\"locked\":false,\"muted\":false,\"solo\":false}],\"clips\":[{\"id\":1,"
        "\"track\":0,\"source\":\"a\",\"label\":\"a\",\"in\":0,\"out\":10,\"start\":0,"
        "\"rate\":1},{\"id\":1,\"track\":0,\"source\":\"a\",\"label\":\"a\",\"in\":0,"
        "\"out\":10,\"start\":40,\"rate\":1}]}",
        m, error)); // duplicate id

    // A valid minimal project loads.
    error.clear();
    CHECK(parseProject(
        "{\"format\":1,\"fps\":24,\"tracks\":[{\"name\":\"V\",\"audio\":false,"
        "\"locked\":false,\"muted\":false,\"solo\":false}],\"clips\":[],\"transitions\":[]}",
        m, error));
    CHECK(m.trackCount() == 1);
    CHECK(m.clips().empty());
    CHECK(m.transitions().empty());
    CHECK(m.fps() == 24.0);

    // A failed parse leaves the model untouched.
    TimelineModel keep = richModel();
    const std::string before = serializeProject(keep);
    CHECK(!parseProject("{\"format\":1", keep, error));
    CHECK(serializeProject(keep) == before);
}

void testJsonStringsAndNumbers() {
    TimelineModel m;
    m.setFps(24.0);
    m.addTrack("V1", false);
    // Paths with quotes, backslashes, newlines, and a CJK label.
    m.addClip(0, "C:\\videos \"take 2\".mp4", "\xE6\xB5\x8B\xE8\xAF\x95", 0, 12, 0);
    const std::string text = serializeProject(m);
    CHECK(text.find("\\\"take 2\\\"") != std::string::npos);
    CHECK(text.find("C:\\\\videos") != std::string::npos);

    TimelineModel loaded;
    std::string error;
    CHECK(parseProject(text, loaded, error));
    CHECK(loaded.clips()[0].sourcePath == "C:\\videos \"take 2\".mp4");
    CHECK(loaded.clips()[0].label == "\xE6\xB5\x8B\xE8\xAF\x95");

    // \\uXXXX escapes decode (incl. a surrogate pair).
    CHECK(parseProject(
        "{\"format\":1,\"fps\":24,\"tracks\":[{\"name\":\"\\u0056\\u0031\",\"audio\":false,"
        "\"locked\":false,\"muted\":false,\"solo\":false}],\"clips\":[],\"transitions\":[]}",
        loaded, error));
    CHECK(loaded.tracks()[0].name == "V1");
    CHECK(parseProject(
        "{\"format\":1,\"fps\":24,\"tracks\":[{\"name\":\"\xE6\xB5\x8B\xE8\xAF\x95\","
        "\"audio\":false,\"locked\":false,\"muted\":false,\"solo\":false}],\"clips\":[],"
        "\"transitions\":[]}",
        loaded, error));
    CHECK(loaded.tracks()[0].name == "\xE6\xB5\x8B\xE8\xAF\x95");

    // Fractional fps round-trips (12.5 is exactly representable).
    m.setFps(12.5);
    CHECK(parseProject(serializeProject(m), loaded, error));
    CHECK(loaded.fps() == 12.5);

    // Exponent form parses.
    CHECK(parseProject(
        "{\"format\":1,\"fps\":2.4e1,\"tracks\":[{\"name\":\"V\",\"audio\":false,"
        "\"locked\":false,\"muted\":false,\"solo\":false}],\"clips\":[],\"transitions\":[]}",
        loaded, error));
    CHECK(loaded.fps() == 24.0);
}

// ---------------------------------------------------------------------------
// Text animations ride inside the text document: additive on write
// (default = no key), strict on read.
// ---------------------------------------------------------------------------

void testTextAnimationRoundTrip() {
    TimelineModel m;
    m.setFps(24.0);
    m.insertTrack(0, "T1", false, true);
    TextDocument doc;
    TextRun run;
    run.text = "Hello";
    doc.runs.push_back(run);
    doc.animation.inKind = TextAnimKind::Fade;
    doc.animation.inFrames = 12;
    doc.animation.outKind = TextAnimKind::Slide;
    doc.animation.outFrames = 8;
    doc.animation.dir = TextAnimDir::Up;
    const int64_t id = m.addTextClip(0, doc, 0, 96);
    CHECK(id > 0);

    const std::string text = serializeProject(m);
    // The animation object is present with exactly these fields.
    CHECK(text.find("\"animation\":{\"in\":\"fade\",\"inFrames\":12,\"out\":\"slide\","
                    "\"outFrames\":8,\"dir\":\"up\"}") != std::string::npos);

    TimelineModel loaded;
    std::string error;
    CHECK(parseProject(text, loaded, error));
    const Clip *clip = loaded.clipById(id);
    CHECK(clip != nullptr);
    CHECK(clip->text.animation.inKind == TextAnimKind::Fade);
    CHECK(clip->text.animation.inFrames == 12);
    CHECK(clip->text.animation.outKind == TextAnimKind::Slide);
    CHECK(clip->text.animation.outFrames == 8);
    CHECK(clip->text.animation.dir == TextAnimDir::Up);
    // Byte-identical re-serialization (the involution invariant).
    CHECK(serializeProject(loaded) == text);

    // A DEFAULT animation writes no key at all (old files stay stable).
    TimelineModel plain;
    plain.setFps(24.0);
    plain.insertTrack(0, "T1", false, true);
    TextDocument d2;
    TextRun r2;
    r2.text = "Hi";
    d2.runs.push_back(r2);
    plain.addTextClip(0, d2, 0, 48);
    const std::string t2 = serializeProject(plain);
    CHECK(t2.find("animation") == std::string::npos);
    TimelineModel l2;
    CHECK(parseProject(t2, l2, error));
    CHECK(l2.clips()[0].text.animation == TextAnimation());

    // A pre-animation document JSON (no "animation" key) loads with the
    // default animation.
    const std::string old = R"({"format":1,"fps":24,"tracks":[)"
                            R"({"name":"T1","audio":false,"text":true,"locked":false,)"
                            R"("muted":false,"solo":false}],"clips":[{"id":1,"track":0,)"
                            R"("label":"Hi","start":0,"duration":48,)"
                            R"("text":{"align":"center","anchorX":0.5,"anchorY":0.5,"wrap":0.8,)"
                            R"("background":false,"bgColor":"000000B4","runs":[)"
                            R"({"text":"Hi","family":"","size":64,"bold":false,"italic":false,)"
                            R"("underline":false,"color":"FFFFFFFF"}]},"effects":[]}],)"
                            R"("transitions":[]})";
    TimelineModel l3;
    CHECK(parseProject(old, l3, error));
    CHECK(l3.clips()[0].text.animation == TextAnimation());
}

void testTextAnimationRejections() {
    TimelineModel model;
    std::string error;
    const std::string head = R"({"format":1,"fps":24,"tracks":[)"
                             R"({"name":"T1","audio":false,"text":true,"locked":false,)"
                             R"("muted":false,"solo":false}],"clips":[{"id":1,"track":0,)"
                             R"("label":"Hi","start":0,"duration":48,)"
                             R"("text":{"align":"center","anchorX":0.5,"anchorY":0.5,"wrap":0.8,)"
                             R"("background":false,"bgColor":"000000B4",)";
    const std::string tail = R"(,"runs":[{"text":"Hi","family":"","size":64,"bold":false,)"
                             R"("italic":false,"underline":false,"color":"FFFFFFFF"}]},)"
                             R"("effects":[]}],"transitions":[]})";
    // A valid animation for splicing.
    const std::string good = R"("animation":{"in":"fade","inFrames":12,"out":"none",)"
                             R"("outFrames":0,"dir":"left"})";

    // Baseline: the valid object parses.
    CHECK(parseProject(head + good + tail, model, error));

    // Unknown kind.
    CHECK(!parseProject(head +
                            R"("animation":{"in":"spin","inFrames":12,"out":"none",)"
                            R"("outFrames":0,"dir":"left"})" +
                            tail,
                        model, error));
    // Unknown direction.
    CHECK(!parseProject(head +
                            R"("animation":{"in":"fade","inFrames":12,"out":"none",)"
                            R"("outFrames":0,"dir":"sideways"})" +
                            tail,
                        model, error));
    // Negative frames.
    CHECK(!parseProject(head +
                            R"("animation":{"in":"fade","inFrames":-1,"out":"none",)"
                            R"("outFrames":0,"dir":"left"})" +
                            tail,
                        model, error));
    // Non-integral frames.
    CHECK(!parseProject(head +
                            R"("animation":{"in":"fade","inFrames":1.5,"out":"none",)"
                            R"("outFrames":0,"dir":"left"})" +
                            tail,
                        model, error));
    // Missing field (strict when the object exists).
    CHECK(!parseProject(head +
                            R"("animation":{"in":"fade","inFrames":12,"out":"none",)"
                            R"("dir":"left"})" +
                            tail,
                        model, error));
    // Not an object.
    CHECK(!parseProject(head + R"("animation":"fade")" + tail, model, error));
    // Frames beyond the sanity cap.
    CHECK(!parseProject(head +
                            R"("animation":{"in":"fade","inFrames":2000000000,)"
                            R"("out":"none","outFrames":0,"dir":"left"})" +
                            tail,
                        model, error));
}

} // namespace

int main() {
    testRoundTrip();
    testParserStrictness();
    testJsonStringsAndNumbers();
    testTextAnimationRoundTrip();
    testTextAnimationRejections();
    return testExitCode("project");
}
