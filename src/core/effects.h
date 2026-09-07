#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fc {

// ---------------------------------------------------------------------------
// M5 effects engine (Module 7 catalog core).
//
// Pure data + pure processing: no Qt, no FFmpeg - unit tested in
// fc_effect_tests. The engine processes RGBA8888 byte buffers (4 bytes
// per pixel in R, G, B, A memory order - exactly QImage::Format_RGBA8888
// layout), in place, on the CPU. Every effect is deterministic: the film
// grain uses a fixed integer hash, not rand(), so identical inputs +
// parameters always produce identical outputs across platforms.
//
// Model:
//   EffectParamDescriptor - one knob of an effect (range + default).
//   EffectDescriptor      - the effect kind: id, label, category, params.
//   EffectInstance       - one applied effect: id + parameter values
//                           + (M5 Phase 3) per-parameter keyframe tracks.
//   Clip::effectStack    - the ordered list of instances on a clip
//                           (applied top to bottom).
//
// The catalog is a static table; ids are stable strings ("color.brightness").
// Processing dispatches on the id in applyEffectStack - one switch keeps
// everything testable and lets the UI enumerate descriptors without
// linking any processing code.
// ---------------------------------------------------------------------------

// Time value meaning "no keyframe context - use the static values" (the
// pre-Phase-3 semantics of applyEffectStack).
constexpr int64_t kNoKeyframeTime = INT64_MIN;

// M5 Phase 3: one keyframed parameter sample. `frame` is CLIP-RELATIVE
// (0 = the clip's first timeline frame); `value` is the parameter value
// at that frame (clamped to the descriptor range when stored).
struct EffectKeyframe {
    int64_t frame = 0;
    double value = 0.0;

    bool operator==(const EffectKeyframe &other) const {
        return frame == other.frame && value == other.value;
    }
    bool operator!=(const EffectKeyframe &other) const { return !(*this == other); }
};

// One parameter's keyframe track (sorted by frame; a track with zero
// points is pruned away by the mutators).
struct EffectKeyframeTrack {
    std::string key;                    // descriptor param key ("amount")
    std::vector<EffectKeyframe> points; // ascending by frame
};

enum class EffectParamType {
    Number, // double in [minValue, maxValue]
    Boolean // 0 or 1
};

struct EffectParamDescriptor {
    std::string key;   // "amount"
    std::string label; // "Amount"
    EffectParamType type = EffectParamType::Number;
    double minValue = 0.0;
    double maxValue = 1.0;
    double defaultValue = 0.0;
};

struct EffectDescriptor {
    std::string id;       // "color.brightness"
    std::string label;    // "Brightness"
    std::string category; // "Color", "Tone", "Filter", "Blur & Sharpen", "Stylize"
    std::vector<EffectParamDescriptor> params;
};

// One applied effect on a clip: the effect id plus one value per
// descriptor param (booleans stored as 0.0 / 1.0). Instances are value
// types - copied freely with the Clip they belong to (splitting a clip
// copies its stack to both halves; the model then re-bases the keyframe
// tracks of the right half).
//
// Keyframes (M5 Phase 3): every Number param can carry a keyframe
// track. The STATIC value in `values` stays authoritative whenever the
// track is empty; with a non-empty track the effective value at a clip
// frame is resolved by paramAt(): clamp to the first/last point outside
// the [first, last] frame range, linear interpolation inside it.
struct EffectInstance {
    std::string effectId;
    bool enabled = true;
    std::vector<double> values;
    std::vector<EffectKeyframeTrack> keyframes; // at most one track per key

    // The catalog entry for effectId, or nullptr when the id is unknown
    // (an instance from a newer catalog survives round-trips but is
    // skipped by the processor).
    const EffectDescriptor *descriptor() const;

    // Parameter value by key (descriptor order is the storage order).
    // Unknown key or wrong-size values -> fallback. Static value: no
    // keyframe resolution (use paramAt for a time-aware read).
    double param(const std::string &key, double fallback = 0.0) const;

    // Time-aware value: the keyframe track for `key` resolved at
    // `clipFrame` (clamp outside [first, last], lerp inside); the static
    // param() value when the track is empty. `clipFrame == kNoKeyframeTime`
    // (or a Boolean param) reads the static value.
    double paramAt(const std::string &key, int64_t clipFrame) const;

    // Sets and CLAMPS to the descriptor range; resizes values when the
    // instance predates a param (fills defaults). No-op for unknown keys.
    void setParam(const std::string &key, double value);

    // Convenience: param as bool (>= 0.5).
    bool paramBool(const std::string &key) const;

    // ---- Keyframe editing (no-ops for unknown keys / Boolean params) ----

    // Sets (or overwrites) a keyframe at `frame`, value CLAMPED to the
    // descriptor range. Keeps the track sorted ascending by frame.
    void setKeyframe(const std::string &key, int64_t frame, double value);

    // Removes the point exactly at `frame`; drops the track when its last
    // point goes. Returns true when a point was removed.
    bool removeKeyframe(const std::string &key, int64_t frame);

    // The point exactly at `frame`, or nullptr.
    const EffectKeyframe *keyframeAt(const std::string &key, int64_t frame) const;

    // The track for `key` (sorted points), or nullptr when the param has
    // no keyframes.
    const std::vector<EffectKeyframe> *keyframeTrack(const std::string &key) const;

    // Drops the whole track for `key` (back to the static value).
    void clearKeyframes(const std::string &key);

    // True when ANY param carries keyframes (drives UI affordances).
    bool hasKeyframes() const;

    // Re-bases every track by -offset and DROPS points that fall before
    // frame 0 (used when a clip is split: the right half inherits the
    // stack, its keyframes shift with its new in-point). Points at
    // exactly 0 survive.
    void rebaseKeyframes(int64_t offset);
};

// The M5 catalog: 52 effects across 7 categories (25 shipped in Phase 1,
// 27 added in Phase 3).
const std::vector<EffectDescriptor> &effectCatalog();

// Catalog lookup by id; nullptr when unknown.
const EffectDescriptor *findEffect(const std::string &id);

// A default-parameter instance of the catalog effect, or an empty
// instance (unknown id: no effectId, never processes) for unknown ids.
EffectInstance makeEffectInstance(const std::string &id);

// Applies the stack top-to-bottom to an RGBA8888 buffer in place.
// Disabled instances and unknown ids are skipped; degenerate sizes
// (width or height <= 0, null buffer) are no-ops. The alpha channel is
// preserved by every effect.
//
// `clipFrame` (M5 Phase 3) is the frame's CLIP-RELATIVE position (0 = the
// clip's first timeline frame) used to resolve keyframed parameters; pass
// kNoKeyframeTime (the default) to apply the static parameter values,
// which is exactly the pre-Phase-3 behavior.
void applyEffectStack(uint8_t *rgba, int width, int height,
                      const std::vector<EffectInstance> &stack,
                      int64_t clipFrame = kNoKeyframeTime);

} // namespace fc
