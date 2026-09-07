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

} // namespace

int main() {
    testRoundTrip();
    testParserStrictness();
    testJsonStringsAndNumbers();
    return testExitCode("project");
}
