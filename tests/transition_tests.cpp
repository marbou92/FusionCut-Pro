// FusionCut Pro - transitions engine + model unit tests.
// Pure data + pure processing: no Qt, no FFmpeg. Every check is a
// deterministic reference value (integer or explicitly rounded double,
// far from rounding boundaries) derived by hand from the geometry
// definitions in transitions.{h,cpp} and timeline_model.h, so results
// are identical on every toolchain.

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "test_harness.h"
#include "timeline_model.h"
#include "transitions.h"

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
    bool operator==(const TestImg &o) const { return w == o.w && h == o.h && px == o.px; }
};

// The transitions catalog hash (spec-documented in transitions.cpp):
// identical formula, mirrored here so the checker wipe's per-cell
// thresholds are part of the tested contract.
uint32_t specHash(uint32_t x, uint32_t y, uint32_t s) {
    uint32_t h = x * 0x9E3779B1u + y * 0x85EBCA77u + s * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x2545F491u;
    h ^= h >> 13;
    return h;
}

// Runs one transition kind at progress p: A/B in, fresh out.
TestImg runKind(const std::string &kind, const TestImg &a, const TestImg &b, double p) {
    TestImg out(a.w, a.h, 0, 0, 0);
    applyTransition(a.px.data(), b.px.data(), out.px.data(), a.w, a.h, kind, p);
    return out;
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

// 16x16 per-channel gradient (r = x*16+y, g = 254 - x*16, b = 90 + y).
TestImg channels16() {
    TestImg img(16, 16, 0, 0, 0);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            img.set(x, y, 0, static_cast<uint8_t>(x * 16 + y));
            img.set(x, y, 1, static_cast<uint8_t>(254 - x * 16));
            img.set(x, y, 2, static_cast<uint8_t>(90 + y));
        }
    }
    return img;
}

// 8x8 per-pixel distinct pair for slide/push source-column verification.
TestImg slideA8() {
    TestImg img(8, 8, 0, 0, 0);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            img.set(x, y, 0, static_cast<uint8_t>(200 - x * 20));
            img.set(x, y, 1, static_cast<uint8_t>(20 + y * 20));
            img.set(x, y, 2, 50);
        }
    }
    return img;
}

TestImg slideB8() {
    TestImg img(8, 8, 0, 0, 0);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            img.set(x, y, 0, static_cast<uint8_t>(x * 30));
            img.set(x, y, 1, static_cast<uint8_t>(y * 30));
            img.set(x, y, 2, 7);
        }
    }
    return img;
}

void testCatalog() {
    const auto &cat = transitionCatalog();
    CHECK(cat.size() == 36);
    CHECK(cat.size() >= 30); // the catalog contract

    std::vector<std::string> ids;
    size_t dissolve = 0, wipe = 0, slide = 0, push = 0, zoom = 0;
    for (const TransitionDescriptor &d : cat) {
        CHECK(!d.id.empty());
        CHECK(!d.label.empty());
        CHECK(!d.category.empty());
        if (d.category == "Dissolve") {
            ++dissolve;
        } else if (d.category == "Wipe") {
            ++wipe;
        } else if (d.category == "Slide") {
            ++slide;
        } else if (d.category == "Push") {
            ++push;
        } else if (d.category == "Zoom") {
            ++zoom;
        } else {
            CHECK(false); // unknown category
        }
        ids.push_back(d.id);
    }
    CHECK(dissolve == 6);
    CHECK(wipe == 19);
    CHECK(slide == 4);
    CHECK(push == 4);
    CHECK(zoom == 3);

    for (size_t i = 0; i < ids.size(); ++i) {
        for (size_t j = i + 1; j < ids.size(); ++j) {
            CHECK(ids[i] != ids[j]);
        }
    }

    // Spot lookups.
    CHECK(findTransition("dissolve.cross") != nullptr);
    CHECK(findTransition("dissolve.cross")->label == "Cross Dissolve");
    CHECK(findTransition("wipe.clock-ccw") != nullptr);
    CHECK(findTransition("zoom.through") != nullptr);
    CHECK(findTransition("push.from-bottom") != nullptr);
    CHECK(findTransition("nope.nope") == nullptr);
    CHECK(findTransition("") == nullptr);
}

void testEndpoints() {
    // EVERY catalog kind is endpoint exact: p <= 0 returns A byte-for-byte,
    // p >= 1 returns B byte-for-byte (clamped progress included).
    const TestImg a = gradient16();
    const TestImg b(16, 16, 200, 40, 90);
    for (const TransitionDescriptor &d : transitionCatalog()) {
        CHECK(runKind(d.id, a, b, 0.0) == a);
        CHECK(runKind(d.id, a, b, 1.0) == b);
        CHECK(runKind(d.id, a, b, -0.4) == a);
        CHECK(runKind(d.id, a, b, 1.6) == b);
    }

    // A second, per-channel-distinct pair (catches channel swaps that a
    // gray A / gray B pair cannot).
    const TestImg c = channels16();
    const TestImg d2(16, 16, 10, 220, 130);
    for (const TransitionDescriptor &d : transitionCatalog()) {
        CHECK(runKind(d.id, c, d2, 0.0) == c);
        CHECK(runKind(d.id, c, d2, 1.0) == d2);
    }
}

void testDegenerate() {
    const TestImg a(4, 4, 10, 20, 30);
    const TestImg b(4, 4, 200, 210, 220);
    TestImg out(4, 4, 0, 0, 0);

    // Null buffers: no crash, no write.
    applyTransition(nullptr, b.px.data(), out.px.data(), 4, 4, "dissolve.cross", 0.5);
    applyTransition(a.px.data(), nullptr, out.px.data(), 4, 4, "dissolve.cross", 0.5);
    applyTransition(a.px.data(), b.px.data(), nullptr, 4, 4, "dissolve.cross", 0.5);

    // Degenerate sizes: no-op.
    applyTransition(a.px.data(), b.px.data(), out.px.data(), 0, 4, "dissolve.cross", 0.5);
    applyTransition(a.px.data(), b.px.data(), out.px.data(), 4, 0, "dissolve.cross", 0.5);
    applyTransition(a.px.data(), b.px.data(), out.px.data(), -4, 4, "dissolve.cross", 0.5);

    // Unknown kind: the outgoing frame passes through (hard cut).
    TestImg pass(4, 4, 0, 0, 0);
    applyTransition(a.px.data(), b.px.data(), pass.px.data(), 4, 4, "future.kind", 0.5);
    CHECK(pass == a);
    applyTransition(a.px.data(), b.px.data(), pass.px.data(), 4, 4, "", 0.5);
    CHECK(pass == a);
}

void testDissolves() {
    const TestImg a(8, 8, 100, 150, 200);
    const TestImg b(8, 8, 200, 40, 90);

    // Cross dissolve p=0.3: (100+30, 150-33, 200-33).
    {
        const TestImg out = runKind("dissolve.cross", a, b, 0.3);
        CHECK(out.at(2, 3, 0) == 130);
        CHECK(out.at(2, 3, 1) == 117);
        CHECK(out.at(2, 3, 2) == 167);
        CHECK(out.at(2, 3, 3) == 255);
    }
    // Cross dissolve p=0.5: (150, 95, 145).
    {
        const TestImg out = runKind("dissolve.cross", a, b, 0.5);
        CHECK(out.at(0, 0, 0) == 150);
        CHECK(out.at(0, 0, 1) == 95);
        CHECK(out.at(0, 0, 2) == 145);
    }
    // Dip to black: p=0.2 blends A toward black by 0.4 -> (60, 90, 120);
    // p=0.8 blends black toward B by 0.6 -> (120, 24, 54).
    {
        const TestImg out = runKind("dissolve.dip-black", a, b, 0.2);
        CHECK(out.at(1, 1, 0) == 60);
        CHECK(out.at(1, 1, 1) == 90);
        CHECK(out.at(1, 1, 2) == 120);
        const TestImg out2 = runKind("dissolve.dip-black", a, b, 0.8);
        CHECK(out2.at(1, 1, 0) == 120);
        CHECK(out2.at(1, 1, 1) == 24);
        CHECK(out2.at(1, 1, 2) == 54);
        // The exact midpoint of a dip is the pure dip color.
        const TestImg mid = runKind("dissolve.dip-black", a, b, 0.5);
        CHECK(mid.at(4, 4, 0) == 0);
        CHECK(mid.at(4, 4, 1) == 0);
        CHECK(mid.at(4, 4, 2) == 0);
    }
    // Dip to white: p=0.2 -> (162, 192, 222); p=0.8 -> (222, 126, 156).
    {
        const TestImg out = runKind("dissolve.dip-white", a, b, 0.2);
        CHECK(out.at(1, 1, 0) == 162);
        CHECK(out.at(1, 1, 1) == 192);
        CHECK(out.at(1, 1, 2) == 222);
        const TestImg out2 = runKind("dissolve.dip-white", a, b, 0.8);
        CHECK(out2.at(1, 1, 0) == 222);
        CHECK(out2.at(1, 1, 1) == 126);
        CHECK(out2.at(1, 1, 2) == 156);
        const TestImg mid = runKind("dissolve.dip-white", a, b, 0.5);
        CHECK(mid.at(4, 4, 0) == 255);
        CHECK(mid.at(4, 4, 1) == 255);
    }
    // Additive dissolve p=0.3: lerp (130, 117, 167) + 0.42 * screen
    // (78.431, 23.529, 70.588) -> (163, 127, 197).
    {
        const TestImg out = runKind("dissolve.additive", a, b, 0.3);
        CHECK(out.at(3, 3, 0) == 163);
        CHECK(out.at(3, 3, 1) == 127);
        CHECK(out.at(3, 3, 2) == 197);
        // The additive bump brightens the midpoint beyond the plain lerp.
        const TestImg plain = runKind("dissolve.cross", a, b, 0.5);
        const TestImg add = runKind("dissolve.additive", a, b, 0.5);
        CHECK(add.at(0, 0, 0) > plain.at(0, 0, 0));
        CHECK(add.at(0, 0, 1) > plain.at(0, 0, 1));
        CHECK(add.at(0, 0, 2) > plain.at(0, 0, 2));
    }
    // Film dissolve: deterministic, noisy (differs from the plain cross),
    // and every channel stays inside the A..B value range.
    {
        const TestImg o1 = runKind("dissolve.film", a, b, 0.3);
        const TestImg o2 = runKind("dissolve.film", a, b, 0.3);
        CHECK(o1 == o2); // deterministic (fixed hash, no rand)
        const TestImg plain = runKind("dissolve.cross", a, b, 0.3);
        CHECK(!(o1 == plain)); // the grain perturbs the mix
        bool sawDifference = false;
        for (int y = 0; y < 8 && !sawDifference; ++y) {
            for (int x = 0; x < 8; ++x) {
                for (int c = 0; c < 3; ++c) {
                    const uint8_t v = o1.at(x, y, c);
                    const uint8_t lo =
                        a.at(0, 0, c) < b.at(0, 0, c) ? a.at(0, 0, c) : b.at(0, 0, c);
                    const uint8_t hi =
                        a.at(0, 0, c) > b.at(0, 0, c) ? a.at(0, 0, c) : b.at(0, 0, c);
                    CHECK(v >= lo);
                    CHECK(v <= hi);
                    if (v != plain.at(x, y, c)) {
                        sawDifference = true;
                    }
                }
            }
        }
        CHECK(sawDifference);
    }
    // Blur dissolve: flat inputs blur to themselves, so a flat pair gives
    // the exact lerp; at p=0.05 the radius rounds to 0 (no blur at all -
    // byte-identical to the plain cross); at p=0.5 it really blurs.
    {
        const TestImg fa(8, 8, 80, 80, 80);
        const TestImg fb(8, 8, 160, 160, 160);
        const TestImg out = runKind("dissolve.blur", fa, fb, 0.3);
        CHECK(out.at(0, 0, 0) == 104);
        CHECK(out.at(7, 7, 0) == 104);
        CHECK(out.at(3, 4, 2) == 104);

        const TestImg g = gradient16();
        const TestImg near = runKind("dissolve.blur", g, b, 0.05);
        const TestImg cross = runKind("dissolve.cross", g, b, 0.05);
        CHECK(near == cross); // radius = lround(10*0.05*0.95) = 0

        const TestImg mid = runKind("dissolve.blur", g, b, 0.5);
        CHECK(!(mid == cross)); // radius 3 actually blurs
    }
}

void testWipes() {
    const TestImg a(8, 8, 100, 150, 200);
    const TestImg b(8, 8, 200, 40, 90);
    const bool isB = true;

    // Helper: full-frame column/row mapping check.
    auto columnsAre = [](const TestImg &out, const TestImg &a, const TestImg &b,
                         const std::vector<bool> &wantB) {
        for (int y = 0; y < out.h; ++y) {
            for (int x = 0; x < out.w; ++x) {
                for (int c = 0; c < 4; ++c) {
                    const uint8_t got = out.at(x, y, c);
                    const uint8_t expect =
                        wantB[static_cast<size_t>(x)] ? b.at(0, 0, c) : a.at(0, 0, c);
                    CHECK(got == expect);
                }
            }
        }
    };

    // Wipe Left: the edge travels LEFT, so B appears first at the RIGHT
    // edge. p=0.5 on 8 wide: x >= 4 shows B.
    {
        const TestImg out = runKind("wipe.left", a, b, 0.5);
        columnsAre(out, a, b, {false, false, false, false, isB, isB, isB, isB});
    }
    // Wipe Right: B first at the LEFT edge (x <= 3).
    {
        const TestImg out = runKind("wipe.right", a, b, 0.5);
        columnsAre(out, a, b, {isB, isB, isB, isB, false, false, false, false});
    }
    // Wipe Up: B first at the BOTTOM rows (y >= 4).
    {
        const TestImg out = runKind("wipe.up", a, b, 0.5);
        for (int x = 0; x < 8; ++x) {
            for (int y = 0; y < 8; ++y) {
                const bool wantB = y >= 4;
                for (int c = 0; c < 4; ++c) {
                    CHECK(out.at(x, y, c) == (wantB ? b.at(0, 0, c) : a.at(0, 0, c)));
                }
            }
        }
    }
    // Wipe Down: B first at the TOP rows (y <= 3).
    {
        const TestImg out = runKind("wipe.down", a, b, 0.5);
        for (int x = 0; x < 8; ++x) {
            for (int y = 0; y < 8; ++y) {
                const bool wantB = y <= 3;
                for (int c = 0; c < 4; ++c) {
                    CHECK(out.at(x, y, c) == (wantB ? b.at(0, 0, c) : a.at(0, 0, c)));
                }
            }
        }
    }

    // Corner wipes at p=0.5, 8x8: B where the center-to-corner distance
    // <= 8 (= p*(W+H)). Boundary rows are exact integer arithmetic.
    {
        const TestImg tl = runKind("wipe.corner-tl", a, b, 0.5);
        CHECK(tl.at(0, 0, 0) == b.at(0, 0, 0)); // d = 1
        CHECK(tl.at(3, 4, 0) == b.at(0, 0, 0)); // d = 8 (boundary, <=)
        CHECK(tl.at(4, 4, 0) == a.at(0, 0, 0)); // d = 9
        CHECK(tl.at(7, 7, 0) == a.at(0, 0, 0)); // d = 15

        const TestImg tr = runKind("wipe.corner-tr", a, b, 0.5); // B where y <= x
        CHECK(tr.at(7, 0, 0) == b.at(0, 0, 0));
        CHECK(tr.at(7, 7, 0) == b.at(0, 0, 0)); // d = 8 boundary
        CHECK(tr.at(0, 7, 0) == a.at(0, 0, 0));
        CHECK(tr.at(3, 4, 0) == a.at(0, 0, 0));
        CHECK(tr.at(4, 3, 0) == b.at(0, 0, 0));

        const TestImg bl = runKind("wipe.corner-bl", a, b, 0.5); // B where x <= y
        CHECK(bl.at(0, 7, 0) == b.at(0, 0, 0));
        CHECK(bl.at(7, 7, 0) == b.at(0, 0, 0));
        CHECK(bl.at(7, 0, 0) == a.at(0, 0, 0));
        CHECK(bl.at(3, 4, 0) == b.at(0, 0, 0));
        CHECK(bl.at(4, 3, 0) == a.at(0, 0, 0));

        const TestImg br = runKind("wipe.corner-br", a, b, 0.5); // B where x+y >= 7
        CHECK(br.at(7, 7, 0) == b.at(0, 0, 0));
        CHECK(br.at(0, 0, 0) == a.at(0, 0, 0));
        CHECK(br.at(3, 4, 0) == b.at(0, 0, 0)); // x+y = 7 boundary
        CHECK(br.at(4, 4, 0) == b.at(0, 0, 0));
        CHECK(br.at(3, 3, 0) == a.at(0, 0, 0)); // x+y = 6
    }

    // Iris box at p=0.5, 8x8: the B rectangle covers x,y in [2, 5].
    {
        const TestImg out = runKind("wipe.iris-box", a, b, 0.5);
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                const bool wantB = x >= 2 && x <= 5 && y >= 2 && y <= 5;
                for (int c = 0; c < 4; ++c) {
                    CHECK(out.at(x, y, c) == (wantB ? b.at(0, 0, c) : a.at(0, 0, c)));
                }
            }
        }
    }
    // Iris box out at p=0.5: the complement (A box shrinking at the center).
    {
        const TestImg out = runKind("wipe.iris-box-out", a, b, 0.5);
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                const bool wantA = x >= 2 && x <= 5 && y >= 2 && y <= 5;
                for (int c = 0; c < 4; ++c) {
                    CHECK(out.at(x, y, c) == (wantA ? a.at(0, 0, c) : b.at(0, 0, c)));
                }
            }
        }
    }
    // Iris circle at p=0.5: threshold radius = hypot(8,8)/4 ~ 2.828.
    {
        const TestImg out = runKind("wipe.circle", a, b, 0.5);
        CHECK(out.at(4, 4, 0) == b.at(0, 0, 0)); // dist 0.707
        CHECK(out.at(2, 2, 0) == b.at(0, 0, 0)); // dist 2.121
        CHECK(out.at(1, 1, 0) == a.at(0, 0, 0)); // dist 3.536
        CHECK(out.at(0, 0, 0) == a.at(0, 0, 0)); // dist 4.950
        CHECK(out.at(7, 7, 0) == a.at(0, 0, 0)); // dist 4.950
    }
    // Iris diamond at p=0.5: threshold (W+H)/4 = 4.
    {
        const TestImg out = runKind("wipe.diamond", a, b, 0.5);
        CHECK(out.at(4, 4, 0) == b.at(0, 0, 0)); // 1.0 < 4
        CHECK(out.at(2, 2, 0) == b.at(0, 0, 0)); // 3.0 < 4
        CHECK(out.at(5, 2, 0) == b.at(0, 0, 0)); // 3.0 < 4
        CHECK(out.at(1, 1, 0) == a.at(0, 0, 0)); // 5.0
        CHECK(out.at(3, 0, 0) == a.at(0, 0, 0)); // exactly 4.0 (strict <)
        CHECK(out.at(0, 3, 0) == a.at(0, 0, 0)); // exactly 4.0
        CHECK(out.at(5, 1, 0) == a.at(0, 0, 0)); // exactly 4.0
    }
    // Clock wipe at p=0.25: the hand points at 3 o'clock, so B covers the
    // clockwise quarter from 12 to 3 (upper-right).
    {
        const TestImg out = runKind("wipe.clock", a, b, 0.25);
        CHECK(out.at(7, 2, 0) == b.at(0, 0, 0)); // angle ~18 deg
        CHECK(out.at(4, 2, 0) == b.at(0, 0, 0)); // angle ~18 deg
        CHECK(out.at(7, 5, 0) == a.at(0, 0, 0)); // angle ~108 deg
        CHECK(out.at(4, 5, 0) == a.at(0, 0, 0)); // angle ~162 deg
        CHECK(out.at(0, 2, 0) == a.at(0, 0, 0)); // angle ~293 deg
        CHECK(out.at(0, 5, 0) == a.at(0, 0, 0)); // angle ~247 deg
        CHECK(out.at(3, 2, 0) == a.at(0, 0, 0)); // angle ~342 deg
    }
    // Clock wipe reverse at p=0.25: B covers the counter-clockwise quarter
    // from 12 to 9 (upper-left).
    {
        const TestImg out = runKind("wipe.clock-ccw", a, b, 0.25);
        CHECK(out.at(0, 2, 0) == b.at(0, 0, 0)); // swept angle ~67 deg
        CHECK(out.at(3, 2, 0) == b.at(0, 0, 0)); // swept angle ~18 deg
        CHECK(out.at(7, 2, 0) == a.at(0, 0, 0)); // swept angle ~342 deg
        CHECK(out.at(0, 5, 0) == a.at(0, 0, 0)); // swept angle ~113 deg
        CHECK(out.at(7, 5, 0) == a.at(0, 0, 0)); // swept angle ~252 deg
        CHECK(out.at(3, 5, 0) == a.at(0, 0, 0)); // swept angle ~162 deg
    }
}

void testBlindsAndChecker() {
    // Blinds need band height > 1, so use 16-frame images.
    const TestImg a(16, 16, 100, 150, 200);
    const TestImg b(16, 16, 200, 40, 90);

    {
        const TestImg out = runKind("wipe.blinds-h", a, b, 0.5);
        // bandH = ceil(16/8) = 2; u = (y % 2 + 0.5)/2 -> 0.25 (even) / 0.75.
        for (int y = 0; y < 16; ++y) {
            const bool wantB = (y % 2) == 0;
            for (int x = 0; x < 16; x += 5) {
                for (int c = 0; c < 4; ++c) {
                    CHECK(out.at(x, y, c) == (wantB ? b.at(0, 0, c) : a.at(0, 0, c)));
                }
            }
        }
    }
    {
        const TestImg out = runKind("wipe.blinds-v", a, b, 0.5);
        for (int x = 0; x < 16; ++x) {
            const bool wantB = (x % 2) == 0;
            for (int y = 0; y < 16; y += 5) {
                for (int c = 0; c < 4; ++c) {
                    CHECK(out.at(x, y, c) == (wantB ? b.at(0, 0, c) : a.at(0, 0, c)));
                }
            }
        }
    }
    // Checker on 16x16: 2x2 cells of 8px; the per-cell threshold comes from
    // the documented catalog hash. Every pixel is exactly A or B.
    {
        const TestImg out = runKind("wipe.checker", a, b, 0.5);
        int bCells = 0;
        for (int cy = 0; cy < 2; ++cy) {
            for (int cx = 0; cx < 2; ++cx) {
                const double t =
                    static_cast<double>(
                        specHash(static_cast<uint32_t>(cx), static_cast<uint32_t>(cy), 42u) % 255u +
                        1u) /
                    256.0;
                const bool wantB = 0.5 >= t;
                if (wantB) {
                    ++bCells;
                }
                for (int y = cy * 8; y < cy * 8 + 8; y += 3) {
                    for (int x = cx * 8; x < cx * 8 + 8; x += 3) {
                        for (int c = 0; c < 4; ++c) {
                            CHECK(out.at(x, y, c) == (wantB ? b.at(0, 0, c) : a.at(0, 0, c)));
                        }
                    }
                }
            }
        }
        CHECK(bCells >= 1); // the hash spreads thresholds across (0, 1)
        CHECK(bCells <= 3); // ... but never all-four / none at p = 0.5

        // Binary-ness across the whole frame.
        for (int y = 0; y < 16; ++y) {
            for (int x = 0; x < 16; ++x) {
                const bool isA = out.at(x, y, 0) == a.at(0, 0, 0) &&
                                 out.at(x, y, 1) == a.at(0, 0, 1) &&
                                 out.at(x, y, 2) == a.at(0, 0, 2);
                const bool isB = out.at(x, y, 0) == b.at(0, 0, 0) &&
                                 out.at(x, y, 1) == b.at(0, 0, 1) &&
                                 out.at(x, y, 2) == b.at(0, 0, 2);
                CHECK(isA != isB);
            }
        }
    }
    // Barn doors at p=0.5, 8x8: B where |x + 0.5 - 4| >= 2 -> x in {0,1,6,7}.
    {
        const TestImg a8(8, 8, 100, 150, 200);
        const TestImg b8(8, 8, 200, 40, 90);
        const TestImg out = runKind("wipe.barn-h", a8, b8, 0.5);
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                const bool wantB = x <= 1 || x >= 6;
                for (int c = 0; c < 4; ++c) {
                    CHECK(out.at(x, y, c) == (wantB ? b8.at(0, 0, c) : a8.at(0, 0, c)));
                }
            }
        }
        const TestImg outv = runKind("wipe.barn-v", a8, b8, 0.5);
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                const bool wantB = y <= 1 || y >= 6;
                for (int c = 0; c < 4; ++c) {
                    CHECK(outv.at(x, y, c) == (wantB ? b8.at(0, 0, c) : a8.at(0, 0, c)));
                }
            }
        }
    }
}

void testSlidesPushes() {
    const TestImg a = slideA8();
    const TestImg b = slideB8();

    // Slide from left at p=0.5 (8 wide): B occupies x <= 3 showing its
    // columns 4..7; A static on the right.
    {
        const TestImg out = runKind("slide.from-left", a, b, 0.5);
        for (int y = 0; y < 8; ++y) {
            CHECK(out.at(0, y, 0) == b.at(4, y, 0));
            CHECK(out.at(3, y, 1) == b.at(7, y, 1));
            CHECK(out.at(4, y, 0) == a.at(4, y, 0));
            CHECK(out.at(7, y, 2) == a.at(7, y, 2));
        }
    }
    // Slide from right at p=0.5: B occupies x >= 4 showing columns 0..3.
    {
        const TestImg out = runKind("slide.from-right", a, b, 0.5);
        for (int y = 0; y < 8; ++y) {
            CHECK(out.at(4, y, 0) == b.at(0, y, 0));
            CHECK(out.at(7, y, 1) == b.at(3, y, 1));
            CHECK(out.at(0, y, 2) == a.at(0, y, 2));
            CHECK(out.at(3, y, 0) == a.at(3, y, 0));
        }
    }
    // Slide from top at p=0.5: B rows <= 3 show source rows 4..7.
    {
        const TestImg out = runKind("slide.from-top", a, b, 0.5);
        for (int x = 0; x < 8; ++x) {
            CHECK(out.at(x, 0, 0) == b.at(x, 4, 0));
            CHECK(out.at(x, 3, 1) == b.at(x, 7, 1));
            CHECK(out.at(x, 4, 0) == a.at(x, 4, 0));
            CHECK(out.at(x, 7, 2) == a.at(x, 7, 2));
        }
    }
    // Slide from bottom at p=0.5: B rows >= 4 show source rows 0..3.
    {
        const TestImg out = runKind("slide.from-bottom", a, b, 0.5);
        for (int x = 0; x < 8; ++x) {
            CHECK(out.at(x, 4, 0) == b.at(x, 0, 0));
            CHECK(out.at(x, 7, 1) == b.at(x, 3, 1));
            CHECK(out.at(x, 0, 2) == a.at(x, 0, 2));
            CHECK(out.at(x, 3, 0) == a.at(x, 3, 0));
        }
    }
    // Push from left at p=0.5: B x <= 3 (cols 4..7); A pushed right, so
    // x >= 4 shows A's columns 0..3.
    {
        const TestImg out = runKind("push.from-left", a, b, 0.5);
        for (int y = 0; y < 8; ++y) {
            CHECK(out.at(0, y, 0) == b.at(4, y, 0));
            CHECK(out.at(3, y, 1) == b.at(7, y, 1));
            CHECK(out.at(4, y, 0) == a.at(0, y, 0));
            CHECK(out.at(7, y, 2) == a.at(3, y, 2));
        }
    }
    // Push from right at p=0.5: B x >= 4 (cols 0..3); A pushed left, so
    // x <= 3 shows A's columns 4..7.
    {
        const TestImg out = runKind("push.from-right", a, b, 0.5);
        for (int y = 0; y < 8; ++y) {
            CHECK(out.at(4, y, 0) == b.at(0, y, 0));
            CHECK(out.at(7, y, 1) == b.at(3, y, 1));
            CHECK(out.at(0, y, 0) == a.at(4, y, 0));
            CHECK(out.at(3, y, 2) == a.at(7, y, 2));
        }
    }
    // Push from top at p=0.5: B rows <= 3 (source rows 4..7); A rows >= 4
    // show source rows 0..3.
    {
        const TestImg out = runKind("push.from-top", a, b, 0.5);
        for (int x = 0; x < 8; ++x) {
            CHECK(out.at(x, 0, 0) == b.at(x, 4, 0));
            CHECK(out.at(x, 3, 1) == b.at(x, 7, 1));
            CHECK(out.at(x, 4, 0) == a.at(x, 0, 0));
            CHECK(out.at(x, 7, 2) == a.at(x, 3, 2));
        }
    }
    // Push from bottom at p=0.5: B rows >= 4 (source rows 0..3); A rows
    // <= 3 show source rows 4..7.
    {
        const TestImg out = runKind("push.from-bottom", a, b, 0.5);
        for (int x = 0; x < 8; ++x) {
            CHECK(out.at(x, 4, 0) == b.at(x, 0, 0));
            CHECK(out.at(x, 7, 1) == b.at(x, 3, 1));
            CHECK(out.at(x, 0, 0) == a.at(x, 4, 0));
            CHECK(out.at(x, 3, 2) == a.at(x, 7, 2));
        }
    }
}

void testZooms() {
    const TestImg a = gradient16();
    const TestImg b(16, 16, 200, 40, 90);
    const TestImg ch = channels16();

    // Zoom in at p=0.4: scale 2.2. Pixel (0,0) center (0.5, 0.5) maps to
    // source (4.59, 4.59) -> floor 4 -> A(4,4) = 68; blended 0.4 toward
    // B(0,0) = (200, 40, 90): (120.8, 56.8, 76.8) -> (121, 57, 77).
    {
        const TestImg out = runKind("zoom.in", a, b, 0.4);
        CHECK(out.at(0, 0, 0) == 121);
        CHECK(out.at(0, 0, 1) == 57);
        CHECK(out.at(0, 0, 2) == 77);
        // Pixel (10,10): center (10.5,10.5) -> (9.14, 9.14) -> A(9,9) = 153.
        CHECK(out.at(10, 10, 0) == 172);
        CHECK(out.at(10, 10, 1) == 108);
        CHECK(out.at(10, 10, 2) == 128);
    }
    // Zoom out at p=0.4: scale 1/2.2. Pixel (0,0) samples outside the
    // frame -> opaque black, blended 0.4 toward B: (80, 16, 36).
    // Pixel (5,5) maps to (2.5, 2.5) -> A(2,2) = 34: (100.4, 36.4, 56.4).
    {
        const TestImg out = runKind("zoom.out", a, b, 0.4);
        CHECK(out.at(0, 0, 0) == 80);
        CHECK(out.at(0, 0, 1) == 16);
        CHECK(out.at(0, 0, 2) == 36);
        CHECK(out.at(3, 3, 0) == 80); // (-1.9, -1.9): letterbox
        CHECK(out.at(5, 5, 0) == 100);
        CHECK(out.at(5, 5, 1) == 36);
        CHECK(out.at(5, 5, 2) == 56);
    }
    // Zoom through at p=0.4: A at scale 2.2, B at scale 2.8. Pixel (0,0):
    // A(4,4) = (68, 68, 68); B center (0.5,0.5) at 2.8 -> (5.32, 5.32) ->
    // B(5,5) = (85, 174, 95); blend 0.4 -> (74.8, 110.4, 78.8) ->
    // (75, 110, 79).
    {
        const TestImg out = runKind("zoom.through", a, ch, 0.4);
        CHECK(out.at(0, 0, 0) == 75);
        CHECK(out.at(0, 0, 1) == 110);
        CHECK(out.at(0, 0, 2) == 79);
    }
}

// ---- Timeline model -------------------------------------------------------

// V2 (0, video), V1 (1, video), A1 (2, audio) - mirrors the app layout.
TimelineModel makeModel() {
    TimelineModel model;
    model.setFps(24.0);
    model.addTrack("V2", false);
    model.addTrack("V1", false);
    model.addTrack("A1", true);
    return model;
}

void testModelAddAndValidate() {
    TimelineModel model = makeModel();
    const int64_t a = model.addClip(1, "a.mp4", "A", 0, 100, 0);
    const int64_t b = model.addClip(1, "b.mp4", "B", 0, 100, 100);
    CHECK(a > 0);
    CHECK(b > 0);

    CHECK(model.maxTransitionDuration(a, b) == 100);
    CHECK(model.transitions().empty());

    const int64_t t = model.addTransition(a, b, "dissolve.cross", 24);
    CHECK(t > 0);
    CHECK(model.transitions().size() == 1);
    CHECK(model.transitions()[0].id == t);
    CHECK(model.transitions()[0].trackIndex == 1);
    CHECK(model.transitions()[0].leftClipId == a);
    CHECK(model.transitions()[0].rightClipId == b);
    CHECK(model.transitions()[0].kind == "dissolve.cross");
    CHECK(model.transitions()[0].durationFrames == 24);

    // The boundary is taken now.
    CHECK(model.maxTransitionDuration(a, b) == 0);
    CHECK(model.addTransition(a, b, "wipe.left", 12) == 0);

    // Lookups.
    CHECK(model.transitionById(t) != nullptr);
    CHECK(model.transitionById(t)->kind == "dissolve.cross");
    CHECK(model.transitionBetween(a, b) != nullptr);
    CHECK(model.transitionBetween(a, b)->id == t);
    CHECK(model.transitionBetween(b, a) == nullptr); // order matters
    CHECK(model.transitionById(999) == nullptr);

    // Rejections.
    CHECK(model.addTransition(a, b, "nope.nope", 10) == 0);
    CHECK(model.addTransition(a, b, "", 10) == 0);
    CHECK(model.addTransition(a, b, "dissolve.cross", 0) == 0);
    CHECK(model.addTransition(a, b, "dissolve.cross", -5) == 0);
    CHECK(model.addTransition(a, b, "dissolve.cross", 101) == 0); // > left dur
    CHECK(model.addTransition(a, a, "dissolve.cross", 10) == 0);
    CHECK(model.addTransition(999, b, "dissolve.cross", 10) == 0);
    CHECK(model.addTransition(a, 998, "dissolve.cross", 10) == 0);

    // Gap pair cannot host.
    const int64_t c = model.addClip(1, "c.mp4", "C", 0, 50, 250); // [250, 300)
    CHECK(model.maxTransitionDuration(b, c) == 0);
    CHECK(model.addTransition(b, c, "wipe.left", 10) == 0);

    // Cross-track pair.
    const int64_t e = model.addClip(0, "e.mp4", "E", 0, 100, 100);
    CHECK(model.maxTransitionDuration(b, e) == 0);

    // Audio track pair.
    const int64_t f = model.addClip(2, "f.wav", "F", 0, 100, 0);
    const int64_t g = model.addClip(2, "g.wav", "G", 0, 100, 100);
    CHECK(model.maxTransitionDuration(f, g) == 0);
    CHECK(model.addTransition(f, g, "dissolve.cross", 24) == 0);

    // Whole-clip transition is legal on a fresh adjacent pair.
    const int64_t h = model.addClip(1, "h.mp4", "H", 0, 100, 300);         // [300, 400)
    const int64_t whole = model.addTransition(c, h, "dissolve.cross", 50); // == c dur
    CHECK(whole > 0);
    CHECK(model.transitionById(whole)->durationFrames == 50);

    // Duration edits.
    CHECK(model.setTransitionDuration(t, 48));
    CHECK(model.transitions()[0].durationFrames == 48);
    CHECK(model.setTransitionDuration(t, 100)); // whole clip
    CHECK(model.transitions()[0].durationFrames == 100);
    CHECK(!model.setTransitionDuration(t, 101));
    CHECK(!model.setTransitionDuration(t, 0));
    CHECK(model.transitions()[0].durationFrames == 100);
    CHECK(!model.setTransitionDuration(999, 10));

    // Removal.
    CHECK(model.removeTransition(t));
    CHECK(model.transitions().size() == 1); // only the whole-clip one remains
    CHECK(model.transitions()[0].id == whole);
    CHECK(!model.removeTransition(t));
    CHECK(model.removeTransition(whole));
    CHECK(model.transitions().empty());
    CHECK(model.maxTransitionDuration(a, b) == 100); // boundary free again
}

void testModelResolve() {
    TimelineModel model = makeModel();
    const int64_t a = model.addClip(1, "a.mp4", "A", 0, 100, 0);   // [0, 100)
    const int64_t b = model.addClip(1, "b.mp4", "B", 0, 100, 100); // [100, 200)
    const int64_t t = model.addTransition(a, b, "wipe.left", 24);
    CHECK(t > 0);

    TransitionSample s;
    CHECK(!model.resolveTransitionAt(75, 1, s));  // before the window
    CHECK(!model.resolveTransitionAt(100, 1, s)); // the cut itself: B only
    CHECK(!model.resolveTransitionAt(-1, 1, s));

    CHECK(model.resolveTransitionAt(76, 1, s));
    CHECK(s.transitionId == t);
    CHECK(s.trackIndex == 1);
    CHECK(s.kind == "wipe.left");
    CHECK(s.leftClipId == a);
    CHECK(s.rightClipId == b);
    CHECK(s.windowStartFrame == 76);
    CHECK(s.windowEndFrame == 100);
    CHECK(s.leftSourceFrame == 76); // sourceIn 0 + (76 - 0)
    CHECK(s.rightSourceFrame == 0); // held first frame = B's in-point
    CHECK(std::fabs(s.progress - 0.0) < 1e-12);

    CHECK(model.resolveTransitionAt(99, 1, s));
    CHECK(std::fabs(s.progress - 23.0 / 24.0) < 1e-12);
    CHECK(s.leftSourceFrame == 99);

    CHECK(model.resolveTransitionAt(88, 1, s));
    CHECK(std::fabs(s.progress - 12.0 / 24.0) < 1e-12);

    // Wrong track: no hit.
    CHECK(!model.resolveTransitionAt(88, 0, s));
    CHECK(!model.resolveTransitionAt(88, 2, s));

    // In-point offsets are honored: A trimmed in, B with a non-zero in.
    TimelineModel model2 = makeModel();
    const int64_t x = model2.addClip(1, "x.mp4", "X", 10, 110, 0);   // [0, 100)
    const int64_t y = model2.addClip(1, "y.mp4", "Y", 40, 140, 100); // [100, 200)
    const int64_t t2 = model2.addTransition(x, y, "dissolve.cross", 24);
    CHECK(t2 > 0);
    TransitionSample s2;
    CHECK(model2.resolveTransitionAt(76, 1, s2));
    CHECK(s2.leftSourceFrame == 10 + 76); // x in-point 10
    CHECK(s2.rightSourceFrame == 40);     // y in-point 40 (held)

    // Rate != 1: a half-speed clip maps timeline frames to half the source
    // advance (source span 50 at rate 0.5 -> 100 timeline frames).
    TimelineModel model3 = makeModel();
    const int64_t r = model3.addClip(1, "r.mp4", "R", 0, 50, 0, 0.5); // dur 100
    const int64_t q = model3.addClip(1, "q.mp4", "Q", 0, 100, 100);
    const int64_t t3 = model3.addTransition(r, q, "dissolve.cross", 24);
    CHECK(t3 > 0);
    TransitionSample s3;
    CHECK(model3.resolveTransitionAt(76, 1, s3));
    CHECK(s3.leftSourceFrame == 38); // lround(76 * 0.5)

    // Whole-clip transition: the window is the entire left clip.
    TimelineModel model4 = makeModel();
    const int64_t w = model4.addClip(1, "w.mp4", "W", 0, 100, 0);
    const int64_t v = model4.addClip(1, "v.mp4", "V", 0, 100, 100);
    const int64_t t4 = model4.addTransition(w, v, "dissolve.cross", 100);
    CHECK(t4 > 0);
    TransitionSample s4;
    CHECK(model4.resolveTransitionAt(0, 1, s4));
    CHECK(s4.windowStartFrame == 0);
    CHECK(std::fabs(s4.progress) < 1e-12);
    CHECK(model4.resolveTransitionAt(99, 1, s4));
    CHECK(std::fabs(s4.progress - 99.0 / 100.0) < 1e-12);
    CHECK(!model4.resolveTransitionAt(100, 1, s4));

    // No transitions at all.
    TimelineModel model5 = makeModel();
    CHECK(!model5.resolveTransitionAt(50, 1, s));
}

void testModelIntegrity() {
    // removeClip kills both side transitions.
    {
        TimelineModel model = makeModel();
        const int64_t a = model.addClip(1, "a.mp4", "A", 0, 100, 0);
        const int64_t b = model.addClip(1, "b.mp4", "B", 0, 100, 100);
        CHECK(model.addTransition(a, b, "dissolve.cross", 24) > 0);
        CHECK(model.removeClip(a));
        CHECK(model.transitions().empty());
    }
    // moveClipTo away breaks adjacency.
    {
        TimelineModel model = makeModel();
        const int64_t a = model.addClip(1, "a.mp4", "A", 0, 100, 0);
        const int64_t b = model.addClip(1, "b.mp4", "B", 0, 100, 100);
        CHECK(model.addTransition(a, b, "dissolve.cross", 24) > 0);
        CHECK(model.moveClipTo(b, 1, 150));
        CHECK(model.transitions().empty());
    }
    // Plain moveClip of the left clip breaks adjacency.
    {
        TimelineModel model = makeModel();
        const int64_t a = model.addClip(1, "a.mp4", "A", 0, 100, 0);
        const int64_t b = model.addClip(1, "b.mp4", "B", 0, 100, 100);
        CHECK(model.addTransition(a, b, "dissolve.cross", 24) > 0);
        CHECK(model.moveClip(a, 20));
        CHECK(model.transitions().empty());
    }
    // trimClipEnd (non-ripple) of the left clip breaks adjacency.
    {
        TimelineModel model = makeModel();
        const int64_t a = model.addClip(1, "a.mp4", "A", 0, 100, 0);
        const int64_t b = model.addClip(1, "b.mp4", "B", 0, 100, 100);
        CHECK(model.addTransition(a, b, "dissolve.cross", 24) > 0);
        CHECK(model.trimClipEnd(a, -60));
        CHECK(model.transitions().empty());
    }
    // trimClipStart keeps the clip END (boundary) - the transition
    // survives and the duration clamps when the clip shrinks below it.
    {
        TimelineModel model = makeModel();
        const int64_t a = model.addClip(1, "a.mp4", "A", 0, 100, 0);
        const int64_t b = model.addClip(1, "b.mp4", "B", 0, 100, 100);
        const int64_t t = model.addTransition(a, b, "dissolve.cross", 24);
        CHECK(t > 0);
        CHECK(model.trimClipStart(a, 10)); // [10, 100), dur 90
        CHECK(model.transitions().size() == 1);
        CHECK(model.transitions()[0].durationFrames == 24);
        CHECK(model.trimClipStart(a, 76)); // [86, 100), dur 14
        CHECK(model.transitions().size() == 1);
        CHECK(model.transitions()[0].durationFrames == 14); // clamped
        TransitionSample s;
        CHECK(model.resolveTransitionAt(95, 1, s));
        CHECK(s.windowStartFrame == 86);
        CHECK(s.leftSourceFrame == 86 + 9); // in-point 86, frame 95
    }
    // rollEdit moves the boundary with both clips: the transition survives
    // (clamped to the new left extent when the roll shrinks it).
    {
        TimelineModel model = makeModel();
        const int64_t a = model.addClip(1, "a.mp4", "A", 0, 100, 0);
        const int64_t b = model.addClip(1, "b.mp4", "B", 0, 100, 100);
        const int64_t t = model.addTransition(a, b, "dissolve.cross", 24);
        CHECK(t > 0);
        CHECK(model.rollEdit(a, b, 30)); // boundary -> 130
        CHECK(model.transitions().size() == 1);
        TransitionSample s;
        CHECK(model.resolveTransitionAt(129, 1, s));
        CHECK(s.windowStartFrame == 106);
        CHECK(s.windowEndFrame == 130);
        CHECK(s.leftSourceFrame == 129);
        CHECK(std::fabs(s.progress - 23.0 / 24.0) < 1e-12);
    }
    // Rolling far back shrinks the left clip below the transition
    // duration: the boundary survives, the duration clamps. The right
    // clip starts with a deep source head so the roll is legal.
    {
        TimelineModel model = makeModel();
        const int64_t a = model.addClip(1, "a.mp4", "A", 0, 100, 0); // [0, 100)
        const int64_t b = model.addClip(1, "b.mp4", "B", 80, 180, 100);
        const int64_t t = model.addTransition(a, b, "dissolve.cross", 24);
        CHECK(t > 0);
        CHECK(model.rollEdit(a, b, -80)); // boundary -> 20, left dur 20
        CHECK(model.transitions().size() == 1);
        CHECK(model.transitions()[0].durationFrames == 20); // clamped from 24
        TransitionSample s;
        CHECK(model.resolveTransitionAt(19, 1, s));
        CHECK(s.windowStartFrame == 0);
        CHECK(s.leftSourceFrame == 19);
    }
    // rippleTrimClipEnd shifts the neighbor with the new end: adjacency is
    // restored, the transition survives.
    {
        TimelineModel model = makeModel();
        const int64_t a = model.addClip(1, "a.mp4", "A", 0, 100, 0);
        const int64_t b = model.addClip(1, "b.mp4", "B", 0, 100, 100);
        const int64_t t = model.addTransition(a, b, "dissolve.cross", 24);
        CHECK(t > 0);
        CHECK(model.rippleTrimClipEnd(a, -30)); // a [0,70), b [70,170)
        CHECK(model.transitions().size() == 1);
        TransitionSample s;
        CHECK(model.resolveTransitionAt(69, 1, s));
        CHECK(s.windowStartFrame == 46);
        CHECK(s.windowEndFrame == 70);

        CHECK(model.rippleTrimClipEnd(a, -50)); // a [0,20), b [20,120)
        CHECK(model.transitions().size() == 1);
        CHECK(model.transitions()[0].durationFrames == 20); // clamped
    }
    // rippleDelete: transitions touching the deleted clip die.
    {
        TimelineModel model = makeModel();
        const int64_t a = model.addClip(1, "a.mp4", "A", 0, 100, 0);
        const int64_t b = model.addClip(1, "b.mp4", "B", 0, 100, 100);
        CHECK(model.addTransition(a, b, "dissolve.cross", 24) > 0);
        CHECK(model.rippleDelete(b));
        CHECK(model.transitions().empty());
    }
    // rippleDelete of an EARLIER clip: the later pair shifts together and
    // its transition survives untouched.
    {
        TimelineModel model = makeModel();
        const int64_t a = model.addClip(1, "a.mp4", "A", 0, 100, 0);
        const int64_t b = model.addClip(1, "b.mp4", "B", 0, 100, 100);
        const int64_t c = model.addClip(1, "c.mp4", "C", 0, 100, 200);
        CHECK(model.addTransition(a, b, "dissolve.cross", 24) > 0);
        const int64_t t2 = model.addTransition(b, c, "wipe.left", 24);
        CHECK(t2 > 0);
        CHECK(model.rippleDelete(a)); // b -> [0,100), c -> [100,200)
        CHECK(model.transitions().size() == 1);
        CHECK(model.transitions()[0].id == t2);
        TransitionSample s;
        CHECK(model.resolveTransitionAt(99, 1, s));
        CHECK(s.windowStartFrame == 76); // b->c window moved with the clips
        CHECK(s.windowEndFrame == 100);
    }
    // Middle-clip delete kills both its transitions.
    {
        TimelineModel model = makeModel();
        const int64_t a = model.addClip(1, "a.mp4", "A", 0, 100, 0);
        const int64_t b = model.addClip(1, "b.mp4", "B", 0, 100, 100);
        const int64_t c = model.addClip(1, "c.mp4", "C", 0, 100, 200);
        CHECK(model.addTransition(a, b, "dissolve.cross", 24) > 0);
        CHECK(model.addTransition(b, c, "wipe.left", 24) > 0);
        CHECK(model.rippleDelete(b));
        CHECK(model.transitions().empty());
    }
    // splitAt re-targets the left clip to the half that owns the cut.
    {
        TimelineModel model = makeModel();
        const int64_t a = model.addClip(1, "a.mp4", "A", 0, 100, 0);
        const int64_t b = model.addClip(1, "b.mp4", "B", 0, 100, 100);
        const int64_t t = model.addTransition(a, b, "dissolve.cross", 24);
        CHECK(t > 0);
        CHECK(model.splitAt(50, 1)); // a -> a1[0,50) + a2[50,100)
        CHECK(model.clips().size() == 3);
        CHECK(model.transitions().size() == 1);
        // The transition's left clip is now the SECOND half (new id).
        const int64_t a2 = model.clips()[1].id;
        CHECK(a2 != a);
        CHECK(model.transitions()[0].leftClipId == a2);
        CHECK(model.transitions()[0].rightClipId == b);
        TransitionSample s;
        CHECK(model.resolveTransitionAt(90, 1, s));
        CHECK(s.windowStartFrame == 76);
        CHECK(s.leftSourceFrame == 50 + 40); // a2 in-point 50, frame 90
    }
    // Splitting INSIDE the window: re-target + duration clamp.
    {
        TimelineModel model = makeModel();
        const int64_t a = model.addClip(1, "a.mp4", "A", 0, 100, 0);
        const int64_t b = model.addClip(1, "b.mp4", "B", 0, 100, 100);
        const int64_t t = model.addTransition(a, b, "dissolve.cross", 24);
        CHECK(t > 0);
        CHECK(model.splitAt(90, 1)); // a2[90,100) owns the cut, dur 10
        CHECK(model.transitions().size() == 1);
        CHECK(model.transitions()[0].durationFrames == 10); // clamped
        TransitionSample s;
        CHECK(model.resolveTransitionAt(95, 1, s));
        CHECK(s.windowStartFrame == 90);
        CHECK(std::fabs(s.progress - 0.5) < 1e-12);
    }
    // Splitting the RIGHT clip: the transition keeps pointing at the first
    // half (which retains the boundary).
    {
        TimelineModel model = makeModel();
        const int64_t a = model.addClip(1, "a.mp4", "A", 0, 100, 0);
        const int64_t b = model.addClip(1, "b.mp4", "B", 0, 100, 100);
        const int64_t t = model.addTransition(a, b, "dissolve.cross", 24);
        CHECK(t > 0);
        CHECK(model.splitAt(150, 1));
        CHECK(model.transitions().size() == 1);
        CHECK(model.transitions()[0].rightClipId == b); // b keeps id + start
        TransitionSample s;
        CHECK(model.resolveTransitionAt(90, 1, s));
        CHECK(s.windowStartFrame == 76);
    }
    // Two transitions back to back on chained clips coexist.
    {
        TimelineModel model = makeModel();
        const int64_t a = model.addClip(1, "a.mp4", "A", 0, 100, 0);
        const int64_t b = model.addClip(1, "b.mp4", "B", 0, 100, 100);
        const int64_t c = model.addClip(1, "c.mp4", "C", 0, 100, 200);
        CHECK(model.addTransition(a, b, "dissolve.cross", 24) > 0);
        CHECK(model.addTransition(b, c, "wipe.left", 24) > 0);
        CHECK(model.transitions().size() == 2);
        TransitionSample s;
        CHECK(model.resolveTransitionAt(90, 1, s)); // a->b window
        CHECK(s.kind == "dissolve.cross");
        CHECK(model.resolveTransitionAt(190, 1, s)); // b->c window
        CHECK(s.kind == "wipe.left");
        CHECK(s.windowStartFrame == 176);
        CHECK(!model.resolveTransitionAt(100, 1, s)); // plain B frame
    }
    // Transitions on different tracks are independent.
    {
        TimelineModel model = makeModel();
        const int64_t a = model.addClip(1, "a.mp4", "A", 0, 100, 0);
        const int64_t b = model.addClip(1, "b.mp4", "B", 0, 100, 100);
        const int64_t e = model.addClip(0, "e.mp4", "E", 0, 60, 0);
        const int64_t f = model.addClip(0, "f.mp4", "F", 0, 60, 60);
        CHECK(model.addTransition(a, b, "dissolve.cross", 24) > 0);
        const int64_t t2 = model.addTransition(e, f, "wipe.clock", 12);
        CHECK(t2 > 0);
        CHECK(model.transitions().size() == 2);
        TransitionSample s;
        CHECK(model.resolveTransitionAt(90, 1, s));
        CHECK(s.transitionId != t2);
        CHECK(model.resolveTransitionAt(55, 0, s));
        CHECK(s.transitionId == t2);
        CHECK(s.windowStartFrame == 48);
    }
}

void testClipStackAndTransitionCoexistence() {
    TimelineModel model = makeModel();
    const int64_t a = model.addClip(1, "a.mp4", "A", 0, 100, 0);
    const int64_t b = model.addClip(1, "b.mp4", "B", 0, 100, 100);
    const int64_t t = model.addTransition(a, b, "dissolve.cross", 24);
    CHECK(t > 0);

    // An effect stack on the left clip coexists with the transition...
    if (Clip *clip = model.clipById(a)) {
        EffectInstance fx = makeEffectInstance("color.brightness");
        fx.setParam("amount", 0.5);
        clip->effectStack.push_back(fx);
    }
    CHECK(model.clips()[0].effectStack.size() == 1);
    CHECK(model.transitions().size() == 1);

    // ...and splitting propagates BOTH: stack copies to both halves,
    // and the transition re-targets to the right half.
    CHECK(model.splitAt(50, 1));
    CHECK(model.clips().size() == 3);
    CHECK(model.clips()[0].effectStack.size() == 1);
    CHECK(model.clips()[1].effectStack.size() == 1);
    CHECK(model.transitions().size() == 1);
    CHECK(model.transitions()[0].leftClipId == model.clips()[1].id);
}

} // namespace

int main() {
    testCatalog();
    testEndpoints();
    testDegenerate();
    testDissolves();
    testWipes();
    testBlindsAndChecker();
    testSlidesPushes();
    testZooms();
    testModelAddAndValidate();
    testModelResolve();
    testModelIntegrity();
    testClipStackAndTransitionCoexistence();
    return testExitCode("transitions");
}
