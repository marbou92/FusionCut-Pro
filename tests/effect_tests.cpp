// FusionCut Pro - M5 Phase 1 effects engine unit tests.
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
    CHECK(cat.size() == 25);

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

    // Categories present.
    size_t colorCount = 0;
    for (const EffectDescriptor &d : cat) {
        if (d.category == "Color") {
            ++colorCount;
        }
    }
    CHECK(colorCount == 9);
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
    return testExitCode("effects");
}
