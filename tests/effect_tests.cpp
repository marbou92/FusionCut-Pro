// FusionCut Pro - effects engine unit tests.
// Pure data + pure processing: no Qt, no FFmpeg. Every check is a
// deterministic reference value (integer or explicitly rounded double,
// far from rounding boundaries) so results are identical on every
// toolchain.

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "effects.h"
#include "test_harness.h"
#include "timeline_model.h"

using namespace fc;

namespace {

struct TestImg {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> px;

    TestImg() = default;
    TestImg(int w_, int h_, uint8_t r, uint8_t g, uint8_t b)
        : w(w_), h(h_), px(static_cast<size_t>(w_) * h_ * 4) {
        for (size_t i = 0; i < px.size(); i += 4) {
            px[i] = r;
            px[i + 1] = g;
            px[i + 2] = b;
            px[i + 3] = 255;
        }
    }

    uint8_t at(int x, int y, int c) const { return px[(static_cast<size_t>(y) * w + x) * 4 + c]; }
    void set(int x, int y, int c, uint8_t v) { px[(static_cast<size_t>(y) * w + x) * 4 + c] = v; }
};

// Runs one catalog effect with the given parameters on img.
void run(const std::string &id, TestImg &img,
         const std::vector<std::pair<std::string, double>> &params) {
    EffectInstance fx = makeEffectInstance(id);
    for (const auto &kv : params) {
        fx.setParam(kv.first, kv.second);
    }
    applyEffectStack(img.px.data(), img.w, img.h, {fx});
}

void runStack(TestImg &img, const std::vector<EffectInstance> &stack) {
    applyEffectStack(img.px.data(), img.w, img.h, stack);
}

// 16x16 luma gradient (r = g = b = x*16 + y, alpha 255).
TestImg gradient16() {
    TestImg img(16, 16, 0, 0, 0);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const uint8_t v = static_cast<uint8_t>(x * 16 + y);
            img.set(x, y, 0, v);
            img.set(x, y, 1, v);
            img.set(x, y, 2, v);
        }
    }
    return img;
}

bool unchanged(const TestImg &a, const TestImg &b) {
    return a.w == b.w && a.h == b.h && a.px == b.px;
}

void testCatalog() {
    const auto &cat = effectCatalog();
    CHECK(cat.size() == 52);

    // Unique ids, non-empty metadata, valid param ranges/defaults.
    for (const EffectDescriptor &d : cat) {
        CHECK(!d.id.empty());
        CHECK(!d.label.empty());
        CHECK(!d.category.empty());
        for (const EffectParamDescriptor &p : d.params) {
            CHECK(p.minValue <= p.maxValue);
            CHECK(p.defaultValue >= p.minValue);
            CHECK(p.defaultValue <= p.maxValue);
            CHECK(!p.key.empty());
        }
    }
    std::vector<std::string> ids;
    for (const EffectDescriptor &d : cat) {
        ids.push_back(d.id);
    }
    for (size_t i = 0; i < ids.size(); ++i) {
        for (size_t j = i + 1; j < ids.size(); ++j) {
            CHECK(ids[i] != ids[j]);
        }
    }

    // Spot descriptors.
    CHECK(findEffect("color.brightness") != nullptr);
    CHECK(findEffect("color.brightness")->params.size() == 1);
    CHECK(findEffect("color.brightness")->params[0].key == "amount");
    CHECK(findEffect("tone.levels")->params.size() == 4);
    CHECK(findEffect("filter.grain")->params.size() == 3);
    // The grain monochrome param is the one Boolean knob.
    const EffectDescriptor *grain = findEffect("filter.grain");
    bool hasBool = false;
    for (const EffectParamDescriptor &p : grain->params) {
        if (p.type == EffectParamType::Boolean) {
            hasBool = true;
        }
    }
    CHECK(hasBool);
    CHECK(findEffect("nope.nope") == nullptr);

    // Categories present (14 Color, plus the Distort
    // and Generate categories).
    size_t colorCount = 0;
    size_t distortCount = 0;
    size_t generateCount = 0;
    for (const EffectDescriptor &d : cat) {
        if (d.category == "Color") {
            ++colorCount;
        }
        if (d.category == "Distort") {
            ++distortCount;
        }
        if (d.category == "Generate") {
            ++generateCount;
        }
    }
    CHECK(colorCount == 14);
    CHECK(distortCount == 4);
    CHECK(generateCount == 4);

    // Spot descriptors (the added catalog effects).
    CHECK(findEffect("color.corrector") != nullptr);
    CHECK(findEffect("color.corrector")->params.size() == 9);
    CHECK(findEffect("stylize.glitch") != nullptr);
    CHECK(findEffect("stylize.glitch")->params.size() == 4);
}

void testInstance() {
    EffectInstance fx = makeEffectInstance("color.brightness");
    CHECK(fx.effectId == "color.brightness");
    CHECK(fx.enabled);
    CHECK(fx.values.size() == 1);
    CHECK(fx.values[0] == 0.0);
    CHECK(fx.descriptor() == findEffect("color.brightness"));

    EffectInstance levels = makeEffectInstance("tone.levels");
    CHECK(levels.values.size() == 4);
    CHECK(levels.values[0] == 0.0);   // inBlack
    CHECK(levels.values[1] == 255.0); // inWhite
    CHECK(levels.values[2] == 0.0);   // outBlack
    CHECK(levels.values[3] == 255.0); // outWhite

    // Unknown id -> empty instance, never processed, no descriptor.
    EffectInstance bad = makeEffectInstance("does.not.exist");
    CHECK(bad.effectId.empty());
    CHECK(bad.values.empty());
    CHECK(bad.descriptor() == nullptr);

    // setParam clamps to the descriptor range.
    fx.setParam("amount", 5.0);
    CHECK(fx.values[0] == 1.0);
    fx.setParam("amount", -5.0);
    CHECK(fx.values[0] == -1.0);
    fx.setParam("amount", 0.25);
    CHECK(fx.values[0] == 0.25);
    // Unknown key: no crash, no change.
    fx.setParam("zzz", 1.0);
    CHECK(fx.values[0] == 0.25);

    // Legacy instance (short values vector): param() falls back to
    // defaults and setParam resizes + fills.
    EffectInstance legacy;
    legacy.effectId = "tone.levels";
    CHECK(legacy.param("inWhite") == 255.0); // default despite size 0
    legacy.setParam("inWhite", 200.0);
    CHECK(legacy.values.size() == 4);
    CHECK(legacy.values[0] == 0.0);
    CHECK(legacy.values[1] == 200.0);
    CHECK(legacy.values[3] == 255.0);

    // paramBool threshold.
    EffectInstance g = makeEffectInstance("filter.grain");
    CHECK(g.paramBool("monochrome"));
    g.setParam("monochrome", 0.0);
    CHECK(!g.paramBool("monochrome"));
}

void testIdentityAtNeutral() {
    const char *neutral[] = {
        "color.brightness",  "color.contrast", "color.saturation", "color.vibrance", "color.hue",
        "color.temperature", "color.tint",     "color.exposure",   "color.gamma",    "tone.levels",
    };
    for (const char *id : neutral) {
        TestImg a = gradient16();
        TestImg b = gradient16();
        run(id, b, {});
        CHECK(unchanged(a, b));
    }

    // Neutral-parameter grain (amount 0) and zero-shift chromatic are
    // identities too.
    TestImg a = gradient16();
    TestImg b = gradient16();
    run("filter.grain", b, {{"amount", 0.0}});
    CHECK(unchanged(a, b));
    b = gradient16();
    run("filter.chromatic", b, {{"shift", 0.0}});
    CHECK(unchanged(a, b));

    // Alpha is never touched by any neutral effect (already covered by
    // the byte-exact compares above).
}

void testColorEffects() {
    // Brightness.
    {
        TestImg img(1, 3, 0, 0, 0);
        img.set(0, 1, 0, 128);
        img.set(0, 1, 1, 128);
        img.set(0, 1, 2, 128);
        img.set(0, 2, 0, 255);
        img.set(0, 2, 1, 255);
        img.set(0, 2, 2, 255);
        run("color.brightness", img, {{"amount", 1.0}});
        CHECK(img.at(0, 0, 0) == 255);
        CHECK(img.at(0, 1, 0) == 255);
        CHECK(img.at(0, 2, 0) == 255);
        CHECK(img.at(0, 1, 3) == 255); // alpha preserved
    }
    {
        TestImg img(1, 1, 255, 255, 255);
        run("color.brightness", img, {{"amount", -1.0}});
        CHECK(img.at(0, 0, 0) == 0);
    }

    // Contrast -1 flattens everything to mid gray.
    {
        TestImg img(1, 3, 0, 0, 0);
        img.set(0, 1, 0, 100);
        img.set(0, 1, 1, 100);
        img.set(0, 1, 2, 100);
        img.set(0, 2, 0, 255);
        img.set(0, 2, 1, 255);
        img.set(0, 2, 2, 255);
        run("color.contrast", img, {{"amount", -1.0}});
        CHECK(img.at(0, 0, 0) == 128);
        CHECK(img.at(0, 1, 0) == 128);
        CHECK(img.at(0, 2, 0) == 128);
    }

    // Saturation 0: Rec.601 luma (integer weights).
    {
        TestImg img(1, 3, 0, 0, 0);
        img.set(0, 0, 0, 255); // red
        img.set(0, 1, 1, 255); // green
        img.set(0, 2, 2, 255); // blue
        run("color.saturation", img, {{"amount", 0.0}});
        CHECK(img.at(0, 0, 0) == 76);
        CHECK(img.at(0, 1, 0) == 150);
        CHECK(img.at(0, 2, 0) == 29);
        CHECK(img.at(0, 0, 1) == 76); // gray now
    }

    // Vibrance 1: fully saturated primaries are untouched.
    {
        TestImg img(1, 1, 255, 0, 0);
        run("color.vibrance", img, {{"amount", 1.0}});
        CHECK(img.at(0, 0, 0) == 255);
        CHECK(img.at(0, 0, 1) == 0);
        CHECK(img.at(0, 0, 2) == 0);
    }

    // Hue 180: red rotates into the teal quadrant (SVG hueRotate
    // matrix reference: red -> ~(0, 109, 109)).
    {
        TestImg img(1, 1, 255, 0, 0);
        run("color.hue", img, {{"angle", 180.0}});
        CHECK(img.at(0, 0, 0) < 60);
        CHECK(img.at(0, 0, 1) >= 80 && img.at(0, 0, 1) <= 180);
        CHECK(img.at(0, 0, 2) >= 80 && img.at(0, 0, 2) <= 180);
        CHECK(img.at(0, 0, 1) > img.at(0, 0, 0));
    }

    // Temperature: +1 warms (r up, b down) by 40.
    {
        TestImg img(1, 1, 128, 128, 128);
        run("color.temperature", img, {{"amount", 1.0}});
        CHECK(img.at(0, 0, 0) == 168);
        CHECK(img.at(0, 0, 1) == 128);
        CHECK(img.at(0, 0, 2) == 88);
    }
    {
        TestImg img(1, 1, 128, 128, 128);
        run("color.temperature", img, {{"amount", -1.0}});
        CHECK(img.at(0, 0, 0) == 88);
        CHECK(img.at(0, 0, 2) == 168);
    }

    // Tint: +1 magenta (g down 40, r/b up 20).
    {
        TestImg img(1, 1, 128, 128, 128);
        run("color.tint", img, {{"amount", 1.0}});
        CHECK(img.at(0, 0, 0) == 148);
        CHECK(img.at(0, 0, 1) == 88);
        CHECK(img.at(0, 0, 2) == 148);
    }

    // Exposure: +1 stop doubles, -1 halves.
    {
        TestImg img(1, 2, 64, 64, 64);
        img.set(0, 1, 0, 200);
        img.set(0, 1, 1, 200);
        img.set(0, 1, 2, 200);
        run("color.exposure", img, {{"stops", 1.0}});
        CHECK(img.at(0, 0, 0) == 128);
        CHECK(img.at(0, 1, 0) == 255); // 400 clamps
    }
    {
        TestImg img(1, 1, 128, 128, 128);
        run("color.exposure", img, {{"stops", -1.0}});
        CHECK(img.at(0, 0, 0) == 64);
    }

    // Gamma 2.2 lifts midtones (reference value far from a rounding
    // boundary); gamma 0.5 darkens them.
    {
        TestImg img(1, 1, 128, 128, 128);
        run("color.gamma", img, {{"gamma", 2.2}});
        CHECK(std::abs(int(img.at(0, 0, 0)) - 186) <= 1);
    }
    {
        TestImg img(1, 1, 128, 128, 128);
        run("color.gamma", img, {{"gamma", 0.5}});
        CHECK(std::abs(int(img.at(0, 0, 0)) - 64) <= 1);
    }
}

void testToneEffects() {
    // Levels: inWhite 128 doubles the scale.
    {
        TestImg img(1, 4, 64, 64, 64);
        img.set(0, 1, 0, 128);
        img.set(0, 1, 1, 128);
        img.set(0, 1, 2, 128);
        img.set(0, 2, 0, 200);
        img.set(0, 2, 1, 200);
        img.set(0, 2, 2, 200);
        img.set(0, 3, 0, 0);
        img.set(0, 3, 1, 0);
        img.set(0, 3, 2, 0);
        run("tone.levels", img, {{"inWhite", 128.0}});
        CHECK(img.at(0, 0, 0) == 128);
        CHECK(img.at(0, 1, 0) == 255);
        CHECK(img.at(0, 2, 0) == 255);
        CHECK(img.at(0, 3, 0) == 0);
    }
    // Levels output remap 0..255 -> 64..192.
    {
        TestImg img(1, 2, 0, 0, 0);
        img.set(0, 1, 0, 255);
        img.set(0, 1, 1, 255);
        img.set(0, 1, 2, 255);
        run("tone.levels", img, {{"outBlack", 64.0}, {"outWhite", 192.0}});
        CHECK(img.at(0, 0, 0) == 64);
        CHECK(img.at(0, 1, 0) == 192);
    }

    // Posterize 2 levels: below half -> 0, above -> 255.
    {
        TestImg img(1, 1, 0, 100, 200);
        run("tone.posterize", img, {{"levels", 2.0}});
        CHECK(img.at(0, 0, 0) == 0);
        CHECK(img.at(0, 0, 1) == 0);
        CHECK(img.at(0, 0, 2) == 255);
    }

    // Threshold.
    {
        TestImg img(1, 2, 100, 100, 100);
        img.set(0, 1, 0, 200);
        img.set(0, 1, 1, 200);
        img.set(0, 1, 2, 200);
        run("tone.threshold", img, {{"cutoff", 0.5}});
        CHECK(img.at(0, 0, 0) == 0);
        CHECK(img.at(0, 1, 0) == 255);
        CHECK(img.at(0, 1, 1) == 255); // grayscale binary
        CHECK(img.at(0, 1, 3) == 255);
    }
    {
        TestImg img(1, 1, 100, 100, 100);
        run("tone.threshold", img, {{"cutoff", 0.2}});
        CHECK(img.at(0, 0, 0) == 255);
    }

    // Solarize: above cutoff inverts, below passes.
    {
        TestImg img(1, 1, 100, 200, 128);
        run("tone.solarize", img, {{"threshold", 0.5}});
        CHECK(img.at(0, 0, 0) == 100);
        CHECK(img.at(0, 0, 1) == 55);
        CHECK(img.at(0, 0, 2) == 128); // cut=128: 128 is not > 128, passes
    }

    // Invert: full and partial.
    {
        TestImg img(1, 1, 100, 100, 100);
        img.set(0, 0, 3, 200);
        run("tone.invert", img, {});
        CHECK(img.at(0, 0, 0) == 155);
        CHECK(img.at(0, 0, 3) == 200); // alpha preserved
    }
    {
        TestImg img(1, 1, 100, 100, 100);
        run("tone.invert", img, {{"amount", 0.5}});
        CHECK(img.at(0, 0, 0) == 128);
    }
}

void testFilterEffects() {
    // Grayscale.
    {
        TestImg img(1, 1, 255, 0, 0);
        run("filter.grayscale", img, {});
        CHECK(img.at(0, 0, 0) == 76);
    }
    {
        TestImg img(1, 1, 255, 0, 0);
        run("filter.grayscale", img, {{"amount", 0.5}});
        CHECK(img.at(0, 0, 0) == 166); // mix(255, 76, 0.5)
    }

    // Sepia matrix reference (100 gray -> 135/120/94).
    {
        TestImg img(1, 1, 100, 100, 100);
        run("filter.sepia", img, {});
        CHECK(img.at(0, 0, 0) == 135);
        CHECK(img.at(0, 0, 1) == 120);
        CHECK(img.at(0, 0, 2) == 94);
    }

    // Vignette: center untouched, corner darkened by amount at t=1.
    {
        TestImg img(64, 64, 255, 255, 255);
        run("filter.vignette", img, {{"amount", 0.5}});
        CHECK(img.at(32, 32, 0) == 255); // center: inside radius
        CHECK(img.at(0, 0, 0) == 128);   // corner: 255 * 0.5
        CHECK(img.at(63, 63, 0) > 128);  // near-corner: dimmed, t < 1
        CHECK(img.at(63, 63, 0) < 255);
    }
    {
        TestImg img(64, 64, 255, 255, 255);
        run("filter.vignette", img, {{"amount", 0.0}});
        CHECK(img.at(0, 0, 0) == 255);
    }

    // Grain: deterministic; alpha untouched; monochrome keeps channels
    // equal on gray input; color mode decorrelates channels somewhere.
    {
        TestImg a(16, 16, 100, 100, 100);
        TestImg b(16, 16, 100, 100, 100);
        run("filter.grain", a, {{"amount", 1.0}, {"seed", 0.5}});
        run("filter.grain", b, {{"amount", 1.0}, {"seed", 0.5}});
        CHECK(a.px == b.px); // deterministic
        CHECK(a.at(0, 0, 3) == 255);
        bool differs = false;
        for (int y = 0; y < 16 && !differs; ++y) {
            for (int x = 0; x < 16; ++x) {
                if (a.at(x, y, 0) != 100 || a.at(x, y, 1) != 100) {
                    differs = true;
                    break;
                }
            }
        }
        CHECK(differs); // amount 1 must actually move pixels
        // Monochrome: all channels moved identically (gray stays gray).
        for (int y = 0; y < 16; ++y) {
            for (int x = 0; x < 16; ++x) {
                CHECK(a.at(x, y, 0) == a.at(x, y, 1));
                CHECK(a.at(x, y, 1) == a.at(x, y, 2));
            }
        }
        // Different seed -> different noise.
        TestImg c(16, 16, 100, 100, 100);
        run("filter.grain", c, {{"amount", 1.0}, {"seed", 0.7}});
        CHECK(c.px != a.px);
        // Color mode: channels decorrelate somewhere on the 16x16 grid.
        TestImg d(16, 16, 100, 100, 100);
        run("filter.grain", d, {{"amount", 1.0}, {"seed", 0.5}, {"monochrome", 0.0}});
        bool decorrelated = false;
        for (int y = 0; y < 16 && !decorrelated; ++y) {
            for (int x = 0; x < 16; ++x) {
                if (d.at(x, y, 0) != d.at(x, y, 1) || d.at(x, y, 1) != d.at(x, y, 2)) {
                    decorrelated = true;
                    break;
                }
            }
        }
        CHECK(decorrelated);
    }

    // Pixelate: 8x8, block 4, one white pixel in the first block.
    {
        TestImg img(8, 8, 0, 0, 0);
        img.set(0, 0, 0, 255);
        run("filter.pixelate", img, {{"block", 4.0}});
        // Block average: 255/16 = 15.94 -> 16, uniform inside the block.
        CHECK(img.at(0, 0, 0) == 16);
        CHECK(img.at(3, 3, 0) == 16);
        CHECK(img.at(0, 1, 0) == 16);
        // Neighbor block stays black.
        CHECK(img.at(4, 0, 0) == 0);
        CHECK(img.at(0, 4, 0) == 0);
    }

    // Chromatic aberration: shift 2 moves red right / blue left, green
    // stays, edges clamp.
    {
        TestImg img(8, 1, 0, 0, 0);
        for (int x = 0; x < 8; ++x) {
            img.set(x, 0, 0, static_cast<uint8_t>(x * 10));
            img.set(x, 0, 1, 7);
            img.set(x, 0, 2, static_cast<uint8_t>(200 - x * 10));
        }
        run("filter.chromatic", img, {{"shift", 2.0}});
        CHECK(img.at(5, 0, 0) == 70);  // r[5] = orig r[7]
        CHECK(img.at(0, 0, 0) == 20);  // r[0] = orig r[2]
        CHECK(img.at(5, 0, 2) == 170); // b[5] = orig b[3]
        CHECK(img.at(0, 0, 2) == 200); // b[0] = orig b[0] (clamped)
        CHECK(img.at(5, 0, 1) == 7);   // green untouched
    }
}

void testBlurSharpen() {
    // Box blur 1 on a 3x1 impulse row.
    {
        TestImg img(3, 1, 0, 0, 0);
        img.set(1, 0, 0, 255);
        img.set(1, 0, 1, 255);
        img.set(1, 0, 2, 255);
        run("blur.box", img, {{"radius", 1.0}});
        CHECK(img.at(0, 0, 0) == 85);
        CHECK(img.at(1, 0, 0) == 85);
        CHECK(img.at(2, 0, 0) == 85);
    }
    // Box blur: uniform image is an exact identity.
    {
        TestImg a(16, 16, 137, 24, 99);
        TestImg b(16, 16, 137, 24, 99);
        run("blur.box", b, {{"radius", 8.0}});
        CHECK(unchanged(a, b));
    }
    // Gaussian: uniform identity + 3x1 impulse reference.
    {
        TestImg a(16, 16, 137, 24, 99);
        TestImg b(16, 16, 137, 24, 99);
        run("blur.gaussian", b, {{"radius", 5.0}});
        CHECK(unchanged(a, b));
    }
    {
        TestImg img(3, 1, 0, 0, 0);
        img.set(1, 0, 0, 255);
        img.set(1, 0, 1, 255);
        img.set(1, 0, 2, 255);
        run("blur.gaussian", img, {{"radius", 1.0}});
        CHECK(img.at(1, 0, 0) == 201); // 255 * k(0)
        CHECK(img.at(0, 0, 0) == 27);  // 255 * k(1)
        CHECK(img.at(2, 0, 0) == 27);
    }
    // Sharpen: uniform identity; dark side darkens, bright side
    // brightens (unsharp mask against the 3x3 box).
    {
        TestImg a(16, 16, 90, 90, 90);
        TestImg b(16, 16, 90, 90, 90);
        run("stylize.sharpen", b, {{"amount", 1.0}});
        CHECK(unchanged(a, b));
    }
    {
        TestImg img(3, 1, 100, 100, 100);
        img.set(1, 0, 0, 150);
        img.set(1, 0, 1, 150);
        img.set(1, 0, 2, 150);
        run("stylize.sharpen", img, {{"amount", 1.0}});
        CHECK(img.at(0, 0, 0) == 83);  // 100 + (100 - 117)
        CHECK(img.at(1, 0, 0) == 183); // 150 + (150 - 117)
        CHECK(img.at(2, 0, 0) == 83);
    }
}

void testStylizeEffects() {
    // Find edges: uniform -> black, alpha preserved.
    {
        TestImg img(8, 8, 90, 90, 90);
        run("stylize.edges", img, {});
        CHECK(img.at(4, 4, 0) == 0);
        CHECK(img.at(4, 4, 3) == 255);
    }
    // Find edges amount 0 -> identity.
    {
        TestImg a(8, 8, 90, 90, 90);
        TestImg b(8, 8, 90, 90, 90);
        run("stylize.edges", b, {{"amount", 0.0}});
        CHECK(unchanged(a, b));
    }
    // Single-row step: endpoints hit full magnitude, center stays flat.
    {
        TestImg img(3, 1, 0, 0, 0);
        img.set(1, 0, 0, 255);
        img.set(1, 0, 1, 255);
        img.set(1, 0, 2, 255);
        run("stylize.edges", img, {});
        CHECK(img.at(0, 0, 0) == 255);
        CHECK(img.at(1, 0, 0) == 0);
        CHECK(img.at(2, 0, 0) == 255);
    }
    // Emboss: uniform -> flat 128 gray.
    {
        TestImg img(8, 8, 90, 90, 90);
        run("stylize.emboss", img, {});
        CHECK(img.at(4, 4, 0) == 128);
        CHECK(img.at(4, 4, 1) == 128);
        CHECK(img.at(4, 4, 2) == 128);
    }
    // Emboss on a rising gray luma ramp 0, 128, 255.
    {
        TestImg img(3, 1, 0, 0, 0);
        img.set(1, 0, 0, 128);
        img.set(1, 0, 1, 128);
        img.set(1, 0, 2, 128);
        img.set(2, 0, 0, 255);
        img.set(2, 0, 1, 255);
        img.set(2, 0, 2, 255);
        run("stylize.emboss", img, {});
        CHECK(img.at(0, 0, 0) == 128); // no left/up neighbor
        CHECK(img.at(1, 0, 0) == 255); // 128 + (128 - 0)
        CHECK(img.at(2, 0, 0) == 255); // 128 + (255 - 128)
    }
}

void testStackSemantics() {
    // Order matters: brighten-then-invert vs invert-then-brighten.
    {
        EffectInstance bright = makeEffectInstance("color.brightness");
        bright.setParam("amount", 1.0);
        EffectInstance inv = makeEffectInstance("tone.invert");

        TestImg a(1, 1, 0, 0, 0);
        runStack(a, {bright, inv}); // 0 -> 255 -> 0
        CHECK(a.at(0, 0, 0) == 0);

        TestImg b(1, 1, 0, 0, 0);
        runStack(b, {inv, bright}); // 0 -> 255 -> 255 (clamp)
        CHECK(b.at(0, 0, 0) == 255);
    }
    // Disabled instances are skipped.
    {
        EffectInstance off = makeEffectInstance("color.brightness");
        off.setParam("amount", 1.0);
        off.enabled = false;
        EffectInstance inv = makeEffectInstance("tone.invert");
        TestImg img(1, 1, 0, 0, 0);
        runStack(img, {off, inv});
        CHECK(img.at(0, 0, 0) == 255);
    }
    // Unknown ids are skipped without crashing.
    {
        EffectInstance ghost;
        ghost.effectId = "future.effect";
        EffectInstance inv = makeEffectInstance("tone.invert");
        TestImg img(1, 1, 0, 0, 0);
        runStack(img, {ghost, inv});
        CHECK(img.at(0, 0, 0) == 255);
    }
    // Empty stack: identity.
    {
        TestImg a(4, 4, 10, 20, 30);
        TestImg b(4, 4, 10, 20, 30);
        runStack(b, {});
        CHECK(unchanged(a, b));
    }
    // Degenerate buffers: no crash.
    {
        std::vector<EffectInstance> stack = {makeEffectInstance("tone.invert")};
        applyEffectStack(nullptr, 4, 4, stack);
        TestImg empty(0, 0, 0, 0, 0);
        runStack(empty, stack);
    }
}

// ---- the 27 new effects ------------------------------------

void testCatalogAdditionsIdentityAtNeutral() {
    // Neutral defaults are exact identities.
    const char *neutral[] = {
        "color.corrector", "color.channelgain", "tone.highlights",
        "tone.shadows",    "tone.gain",         "distort.tile",
    };
    for (const char *id : neutral) {
        TestImg a = gradient16();
        TestImg b = gradient16();
        run(id, b, {});
        CHECK(unchanged(a, b));
    }
    // Zero-strength variants of the effects with expressive defaults.
    struct ZeroCase {
        const char *id;
        std::vector<std::pair<std::string, double>> params;
    };
    const ZeroCase zeros[] = {
        {"color.fade", {{"amount", 0.0}}},        {"blur.radial", {{"amount", 0.0}}},
        {"distort.wave", {{"amplitude", 0.0}}},   {"distort.ripple", {{"amplitude", 0.0}}},
        {"distort.fisheye", {{"amount", 0.0}}},   {"generate.bars", {{"amount", 0.0}}},
        {"generate.gradient", {{"amount", 0.0}}}, {"generate.grid", {{"amount", 0.0}}},
        {"generate.noise", {{"amount", 0.0}}},    {"stylize.thermal", {{"amount", 0.0}}},
        {"stylize.glitch", {{"amount", 0.0}}},
    };
    for (const ZeroCase &z : zeros) {
        TestImg a = gradient16();
        TestImg b = gradient16();
        run(z.id, b, z.params);
        CHECK(unchanged(a, b));
    }
    // Zero-amount variants of the blending effects.
    TestImg a = gradient16();
    TestImg b = gradient16();
    run("color.colorize", b, {{"amount", 0.0}});
    CHECK(unchanged(a, b));
    b = gradient16();
    run("color.duotone", b, {{"amount", 0.0}});
    CHECK(unchanged(a, b));
}

void testCatalogAdditionsColor() {
    // Corrector: exposure, contrast, temperature, shadows - each alone.
    {
        TestImg img(8, 8, 100, 100, 100);
        run("color.corrector", img, {{"exposure", 1.0}});
        CHECK(img.at(3, 3, 0) == 200 && img.at(3, 3, 1) == 200 && img.at(3, 3, 2) == 200);
    }
    {
        TestImg img(8, 8, 100, 100, 100);
        run("color.corrector", img, {{"contrast", -0.5}});
        CHECK(img.at(3, 3, 0) == 114 && img.at(3, 3, 1) == 114 && img.at(3, 3, 2) == 114);
    }
    {
        TestImg img(8, 8, 100, 100, 100);
        run("color.corrector", img, {{"temperature", 1.0}});
        CHECK(img.at(3, 3, 0) == 140);
        CHECK(img.at(3, 3, 1) == 100);
        CHECK(img.at(3, 3, 2) == 60);
    }
    {
        TestImg img(8, 8, 50, 50, 50);
        run("color.corrector", img, {{"shadows", 0.5}});
        // w = (128-50)/128 = 0.609375; d = lround(0.5*90*w) = 27.
        CHECK(img.at(3, 3, 0) == 77 && img.at(3, 3, 1) == 77 && img.at(3, 3, 2) == 77);
    }

    // Channel gain: 100/150/200 with 0.5/1.0/0.5.
    {
        TestImg img(8, 8, 100, 150, 200);
        run("color.channelgain", img, {{"rGain", 0.5}, {"gGain", 1.0}, {"bGain", 0.5}});
        CHECK(img.at(3, 3, 0) == 50);
        CHECK(img.at(3, 3, 1) == 150);
        CHECK(img.at(3, 3, 2) == 100);
    }

    // Colorize: luma 124 toward pure red (hue 0).
    {
        TestImg img(8, 8, 200, 100, 50);
        run("color.colorize", img, {{"hue", 0.0}, {"amount", 1.0}});
        CHECK(img.at(3, 3, 0) == 124);
        CHECK(img.at(3, 3, 1) == 0);
        CHECK(img.at(3, 3, 2) == 0);
    }
    {
        TestImg img(8, 8, 200, 100, 50);
        run("color.colorize", img, {{"hue", 240.0}, {"amount", 1.0}});
        CHECK(img.at(3, 3, 0) == 0);
        CHECK(img.at(3, 3, 1) == 0);
        CHECK(img.at(3, 3, 2) == 124);
    }

    // Duotone: black maps to the shadow hue, white to the highlight hue.
    {
        TestImg img = gradient16(); // (0,0) is 0, (15,15) is 255
        run("color.duotone", img, {{"shadowHue", 240.0}, {"highlightHue", 60.0}, {"amount", 1.0}});
        CHECK(img.at(0, 0, 0) == 0 && img.at(0, 0, 1) == 0 && img.at(0, 0, 2) == 255);
        CHECK(img.at(15, 15, 0) == 255 && img.at(15, 15, 1) == 255 && img.at(15, 15, 2) == 0);
    }

    // Faded film: luma 124, k = 0.88, lift 12.
    {
        TestImg img(8, 8, 200, 100, 50);
        run("color.fade", img, {{"amount", 0.4}});
        CHECK(img.at(3, 3, 0) == 203);
        CHECK(img.at(3, 3, 1) == 115);
        CHECK(img.at(3, 3, 2) == 71);
    }
}

void testCatalogAdditionsTone() {
    // Highlights: solid 160, amount 0.5 -> +11 (w = 32/127).
    {
        TestImg img(8, 8, 160, 160, 160);
        run("tone.highlights", img, {{"amount", 0.5}});
        CHECK(img.at(3, 3, 0) == 171);
    }
    {
        TestImg img(8, 8, 50, 50, 50);
        run("tone.highlights", img, {{"amount", 0.5}}); // below 128: untouched
        CHECK(img.at(3, 3, 0) == 50);
    }
    // Shadows: solid 64, amount 0.4 -> +18 (w = 0.5).
    {
        TestImg img(8, 8, 64, 64, 64);
        run("tone.shadows", img, {{"amount", 0.4}});
        CHECK(img.at(3, 3, 0) == 82);
    }
    // Gain: 100 * 1.5.
    {
        TestImg img(8, 8, 100, 100, 100);
        run("tone.gain", img, {{"gain", 1.5}});
        CHECK(img.at(3, 3, 0) == 150);
    }
    // Bayer dither, levels 2 (step 255): 128 dithers to 0 or 255 by the
    // 4x4 matrix threshold.
    {
        TestImg img(16, 16, 128, 128, 128);
        run("tone.bayer", img, {{"levels", 2.0}});
        // (0,0): matrix 0 -> thresh -127.5 -> 0.5/255 -> 0.
        CHECK(img.at(0, 0, 0) == 0);
        // (1,0): matrix 12 -> thresh +63.75 -> 191.75/255 -> 1 -> 255.
        CHECK(img.at(1, 0, 0) == 255);
        CHECK(img.at(2, 0, 0) == 0); // matrix 3 -> 48.31/255 -> 0
    }
}

void testCatalogAdditionsFilter() {
    // Mirror (vertical axis, default axis 0.5): on the 16-wide gradient,
    // x >= 8 mirrors from 2*8-1-x.
    {
        TestImg img = gradient16();
        run("filter.mirror", img, {{"axis", 0.5}, {"vertical", 1.0}});
        CHECK(img.at(15, 3, 0) == 3); // from (0,3)
        CHECK(img.at(9, 3, 0) == 99); // from (6,3)
        CHECK(img.at(3, 4, 0) == 52); // left half untouched
    }
    // Flip (horizontal): (x,y) <- (15-x, y).
    {
        TestImg img = gradient16();
        run("filter.flip", img, {{"vertical", 0.0}});
        CHECK(img.at(0, 0, 0) == 240);
        CHECK(img.at(15, 0, 0) == 0);
        CHECK(img.at(0, 0, 2) == 240);
    }
    // Glow: uniform 128, blurred = 128, screen = 192, mix 0.5 -> 160.
    {
        TestImg img(16, 16, 128, 128, 128);
        run("filter.glow", img, {{"radius", 2.0}, {"intensity", 0.5}});
        CHECK(img.at(7, 7, 0) == 160);
    }
    // Scanlines: rows y % period == 0 darken by amount.
    {
        TestImg img(16, 16, 200, 200, 200);
        run("filter.scanlines", img, {{"amount", 0.5}, {"period", 3.0}});
        CHECK(img.at(5, 0, 0) == 100);
        CHECK(img.at(5, 1, 0) == 200);
        CHECK(img.at(5, 3, 0) == 100);
    }
    // Halftone: solid white, cell 8 - dot radius 4 around cell centers.
    {
        TestImg img(16, 16, 255, 255, 255);
        run("filter.halftone", img, {{"cell", 8.0}, {"amount", 1.0}});
        CHECK(img.at(0, 0, 0) == 0);   // corner: dist 4.95 > 4
        CHECK(img.at(4, 4, 0) == 255); // near center: 0.707 <= 4
        CHECK(img.at(15, 15, 0) == 0); // corner of the (8,8) cell
    }
}

void testCatalogAdditionsBlurDistortGenerate() {
    // Motion blur H, radius 2 on the gradient (v = x*16 + y).
    {
        TestImg img = gradient16();
        run("blur.motionH", img, {{"radius", 2.0}});
        CHECK(img.at(5, 5, 0) == 85); // interior: exact average
        CHECK(img.at(0, 0, 0) == 10); // {0,0,0,16,32} -> 9.6 -> 10
        CHECK(img.at(2, 0, 0) == 32); // {0,16,32,48,64} -> 32
    }
    // Motion blur V, radius 2: the x=0 column holds 0,1,2,3,...
    {
        TestImg img = gradient16();
        run("blur.motionV", img, {{"radius", 2.0}});
        CHECK(img.at(5, 5, 0) == 85);
        CHECK(img.at(0, 0, 0) == 1);  // {0,0,0,1,2} -> 0.6 -> 1
        CHECK(img.at(2, 0, 0) == 33); // {32,32,32,33,34} -> 32.8 -> 33
    }
    // Radial blur: center pixel is an exact identity for any amount.
    {
        TestImg img = gradient16();
        run("blur.radial", img, {{"amount", 1.0}});
        CHECK(img.at(8, 8, 0) == 136);
    }
    // Wave: y=8 has sin(pi/2)=1 -> x shifts by exactly amplitude 4.
    {
        TestImg img = gradient16();
        run("distort.wave", img, {{"amplitude", 4.0}, {"wavelength", 32.0}});
        CHECK(img.at(4, 8, 0) == 136); // from (8,8)
        CHECK(img.at(4, 0, 0) == 64);  // y=0: sin(0)=0 -> identity
    }
    // Tile: 16x16 with 8x8 blocks wraps.
    {
        TestImg img = gradient16();
        run("distort.tile", img, {{"blockW", 8.0}, {"blockH", 8.0}});
        CHECK(img.at(9, 9, 0) == 17);    // from (1,1)
        CHECK(img.at(15, 15, 0) == 119); // from (7,7)
    }
    // Bars: 7 columns across 16 pixels (col = x*7/16).
    {
        TestImg img = gradient16();
        run("generate.bars", img, {{"amount", 1.0}});
        CHECK(img.at(0, 0, 0) == 255 && img.at(0, 0, 1) == 255 && img.at(0, 0, 2) == 255);
        CHECK(img.at(3, 3, 0) == 255 && img.at(3, 3, 1) == 255 && img.at(3, 3, 2) == 0);
        CHECK(img.at(5, 3, 0) == 0 && img.at(5, 3, 1) == 255 && img.at(5, 3, 2) == 255);
        CHECK(img.at(15, 15, 0) == 0 && img.at(15, 15, 1) == 0 && img.at(15, 15, 2) == 255);
    }
    // Gradient ramp: x*255/15.
    {
        TestImg img(16, 16, 90, 90, 90);
        run("generate.gradient", img, {{"amount", 1.0}});
        CHECK(img.at(0, 5, 0) == 0);
        CHECK(img.at(8, 5, 0) == 136);
        CHECK(img.at(15, 5, 0) == 255);
    }
    // Grid: spacing 8, thickness 1.
    {
        TestImg img = gradient16();
        run("generate.grid", img, {{"spacing", 8.0}, {"thickness", 1.0}, {"amount", 1.0}});
        CHECK(img.at(0, 0, 0) == 0);
        CHECK(img.at(8, 4, 0) == 0);
        CHECK(img.at(4, 4, 0) == 68);
    }
    // Noise: same seed -> identical, different seed -> different.
    {
        TestImg a = gradient16();
        TestImg b = gradient16();
        TestImg c = gradient16();
        run("generate.noise", a, {{"amount", 1.0}, {"seed", 0.3}});
        run("generate.noise", b, {{"amount", 1.0}, {"seed", 0.3}});
        run("generate.noise", c, {{"amount", 1.0}, {"seed", 0.7}});
        CHECK(a.px == b.px);
        CHECK(a.px != c.px);
    }
}

void testCatalogAdditionsStylize() {
    // Thermal palette: exact stop math on 0 / 128 / 255 luma.
    {
        TestImg img(4, 4, 0, 0, 0);
        run("stylize.thermal", img, {{"amount", 1.0}});
        CHECK(img.at(1, 1, 0) == 0 && img.at(1, 1, 1) == 0 && img.at(1, 1, 2) == 0);
    }
    {
        TestImg img(4, 4, 128, 128, 128);
        run("stylize.thermal", img, {{"amount", 1.0}});
        CHECK(img.at(1, 1, 0) == 160);
        CHECK(img.at(1, 1, 1) == 16);
        CHECK(img.at(1, 1, 2) == 80);
    }
    {
        TestImg img(4, 4, 255, 255, 255);
        run("stylize.thermal", img, {{"amount", 1.0}});
        CHECK(img.at(1, 1, 0) == 255);
        CHECK(img.at(1, 1, 1) == 255);
        CHECK(img.at(1, 1, 2) == 224);
    }
    // Glitch: deterministic per seed; some block row must shift.
    {
        TestImg a = gradient16();
        TestImg b = gradient16();
        run("stylize.glitch", a,
            {{"amount", 1.0}, {"seed", 0.5}, {"blockH", 8.0}, {"maxShift", 24.0}});
        run("stylize.glitch", b,
            {{"amount", 1.0}, {"seed", 0.5}, {"blockH", 8.0}, {"maxShift", 24.0}});
        CHECK(a.px == b.px);
        bool row0Shifted = false;
        bool row1Shifted = false;
        for (int x = 0; x < 16; ++x) {
            if (a.at(x, 0, 0) != b.at(x, 0, 0) || a.at(x, 0, 0) != gradient16().at(x, 0, 0)) {
                row0Shifted = true;
            }
            if (a.at(x, 8, 0) != gradient16().at(x, 8, 0)) {
                row1Shifted = true;
            }
        }
        CHECK(row0Shifted || row1Shifted);
    }
}

// ---- keyframes ----------------------------------------------

void testKeyframes() {
    EffectInstance fx = makeEffectInstance("color.brightness");
    CHECK(!fx.hasKeyframes());

    fx.setKeyframe("amount", 0, 0.0);
    fx.setKeyframe("amount", 60, 0.5);
    fx.setKeyframe("amount", 120, 1.0);
    CHECK(fx.hasKeyframes());

    const std::vector<EffectKeyframe> *track = fx.keyframeTrack("amount");
    CHECK(track != nullptr);
    CHECK(track->size() == 3);
    CHECK((*track)[0].frame == 0 && (*track)[0].value == 0.0);
    CHECK((*track)[1].frame == 60 && (*track)[1].value == 0.5);
    CHECK((*track)[2].frame == 120 && (*track)[2].value == 1.0);

    // Resolution: clamped ends, linear interior.
    CHECK(fx.paramAt("amount", 0) == 0.0);
    CHECK(fx.paramAt("amount", 60) == 0.5);
    CHECK(fx.paramAt("amount", 120) == 1.0);
    CHECK(fx.paramAt("amount", 30) == 0.25);
    CHECK(fx.paramAt("amount", 90) == 0.75);
    CHECK(fx.paramAt("amount", -5) == 0.0);
    CHECK(fx.paramAt("amount", 500) == 1.0);

    // The static value is untouched by keyframing (and paramAt without a
    // time context reads it).
    CHECK(fx.param("amount") == 0.0);
    CHECK(fx.paramAt("amount", kNoKeyframeTime) == 0.0);

    // Out-of-range values clamp to the descriptor range on store.
    fx.setKeyframe("amount", 10, 5.0);
    CHECK(fx.keyframeAt("amount", 10)->value == 1.0);

    // Overwrite: same frame, new value; the track never grows duplicates.
    fx.setKeyframe("amount", 10, 0.2);
    CHECK(fx.keyframeTrack("amount")->size() == 4);
    fx.setKeyframe("amount", 10, 0.4);
    CHECK(fx.keyframeTrack("amount")->size() == 4);
    CHECK(fx.keyframeAt("amount", 10)->value == 0.4);

    // Unsorted inserts stay sorted.
    EffectInstance fx2 = makeEffectInstance("color.brightness");
    fx2.setKeyframe("amount", 60, 0.5);
    fx2.setKeyframe("amount", 0, 0.0);
    fx2.setKeyframe("amount", 120, 1.0);
    CHECK(fx2.keyframeTrack("amount")->size() == 3);
    CHECK((*fx2.keyframeTrack("amount"))[0].frame == 0);
    CHECK((*fx2.keyframeTrack("amount"))[2].frame == 120);

    // Removal.
    CHECK(fx.removeKeyframe("amount", 10));
    CHECK(!fx.removeKeyframe("amount", 10));
    CHECK(fx.keyframeTrack("amount")->size() == 3);
    // Removing every point drops the track entirely.
    CHECK(fx.removeKeyframe("amount", 0));
    CHECK(fx.removeKeyframe("amount", 60));
    CHECK(fx.removeKeyframe("amount", 120));
    CHECK(fx.keyframeTrack("amount") == nullptr);
    CHECK(!fx.hasKeyframes());

    // Boolean params and unknown keys never keyframe.
    EffectInstance grain = makeEffectInstance("filter.grain");
    grain.setKeyframe("monochrome", 0, 1.0);
    grain.setKeyframe("nope", 0, 1.0);
    CHECK(!grain.hasKeyframes());

    // clearKeyframes.
    EffectInstance fx3 = makeEffectInstance("color.brightness");
    fx3.setKeyframe("amount", 0, 0.0);
    fx3.setKeyframe("amount", 60, 1.0);
    fx3.clearKeyframes("amount");
    CHECK(!fx3.hasKeyframes());
    CHECK(fx3.paramAt("amount", 30) == fx3.param("amount"));

    // Time-aware application: brightness 0 -> 1 across frames 0..8.
    {
        EffectInstance b = makeEffectInstance("color.brightness");
        b.setKeyframe("amount", 0, 0.0);
        b.setKeyframe("amount", 8, 1.0);
        TestImg img(8, 8, 100, 100, 100);
        runStack(img, {b}); // no time context -> static 0 -> unchanged
        CHECK(img.at(3, 3, 0) == 100);

        TestImg zero(8, 8, 100, 100, 100);
        applyEffectStack(zero.px.data(), zero.w, zero.h, {b}, 0);
        CHECK(zero.at(3, 3, 0) == 100); // amount 0

        TestImg half(8, 8, 100, 100, 100);
        applyEffectStack(half.px.data(), half.w, half.h, {b}, 4);
        CHECK(half.at(3, 3, 0) == 228); // amount 0.5 -> +128

        TestImg one(8, 8, 100, 100, 100);
        applyEffectStack(one.px.data(), one.w, one.h, {b}, 8);
        CHECK(one.at(3, 3, 0) == 255); // amount 1 -> +255 -> clamped
        CHECK(one.at(3, 3, 2) == 255);
    }

    // Rebase: shift by 40 drops the two early points.
    EffectInstance fx4 = makeEffectInstance("color.brightness");
    fx4.setKeyframe("amount", 0, 0.0);
    fx4.setKeyframe("amount", 30, 0.25);
    fx4.setKeyframe("amount", 60, 0.5);
    fx4.setKeyframe("amount", 90, 0.75);
    fx4.rebaseKeyframes(40);
    const std::vector<EffectKeyframe> *t4 = fx4.keyframeTrack("amount");
    CHECK(t4 != nullptr);
    if (t4) {
        CHECK(t4->size() == 2);
        CHECK((*t4)[0].frame == 20 && (*t4)[0].value == 0.5);
        CHECK((*t4)[1].frame == 50 && (*t4)[1].value == 0.75);
    }

    // Split integration: the right half's keyframes shift with its
    // in-point; the left half keeps its full track.
    {
        TimelineModel model;
        model.setFps(24.0);
        model.addTrack("V1", false);
        const int64_t id = model.addClip(0, "a", "a", 0, 120, 0);
        if (Clip *clip = model.clipById(id)) {
            EffectInstance b = makeEffectInstance("color.brightness");
            b.setKeyframe("amount", 0, 0.0);
            b.setKeyframe("amount", 60, 0.5);
            clip->effectStack.push_back(b);
        }
        CHECK(model.splitAt(40, 0));
        const std::vector<EffectKeyframe> *left =
            model.clips()[0].effectStack[0].keyframeTrack("amount");
        const std::vector<EffectKeyframe> *right =
            model.clips()[1].effectStack[0].keyframeTrack("amount");
        CHECK(left != nullptr);
        CHECK(right != nullptr);
        if (left && right) {
            CHECK(left->size() == 2);  // non-destructive: left keeps both
            CHECK(right->size() == 1); // 60 - 40 = 20
            CHECK((*right)[0].frame == 20 && (*right)[0].value == 0.5);
        }
    }
}

void testClipStackIntegration() {
    TimelineModel model;
    model.addTrack("V1", false);
    model.setFps(24.0);
    const int64_t id = model.addClip(0, "video.mp4", "Clip A", 0, 240, 0);
    CHECK(model.clips()[0].effectStack.empty()); // default: no effects

    // Attach a stack through the mutable accessor (the UI path).
    if (Clip *clip = model.clipById(id)) {
        EffectInstance fx = makeEffectInstance("color.brightness");
        fx.setParam("amount", 0.5);
        clip->effectStack.push_back(fx);
    }
    CHECK(model.clips()[0].effectStack.size() == 1);
    CHECK(model.clips()[0].effectStack[0].param("amount") == 0.5);

    // Splitting copies the stack to BOTH halves.
    CHECK(model.splitAt(120, 0));
    CHECK(model.clips().size() == 2);
    CHECK(model.clips()[0].effectStack.size() == 1);
    CHECK(model.clips()[1].effectStack.size() == 1);
    CHECK(model.clips()[1].effectStack[0].param("amount") == 0.5);
}

} // namespace

int main() {
    testCatalog();
    testInstance();
    testIdentityAtNeutral();
    testColorEffects();
    testToneEffects();
    testFilterEffects();
    testBlurSharpen();
    testStylizeEffects();
    testStackSemantics();
    testClipStackIntegration();
    testCatalogAdditionsIdentityAtNeutral();
    testCatalogAdditionsColor();
    testCatalogAdditionsTone();
    testCatalogAdditionsFilter();
    testCatalogAdditionsBlurDistortGenerate();
    testCatalogAdditionsStylize();
    testKeyframes();
    return testExitCode("effects");
}
