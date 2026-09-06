#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fc {

// ---------------------------------------------------------------------------
// M5 Phase 1 effects engine (Module 7 catalog core).
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
//   EffectInstance       - one applied effect: id + parameter values.
//   Clip::effectStack    - the ordered list of instances on a clip
//                           (applied top to bottom).
//
// The catalog is a static table; ids are stable strings ("color.brightness").
// Processing dispatches on the id in applyEffectStack - one switch keeps
// everything testable and lets the UI enumerate descriptors without
// linking any processing code.
// ---------------------------------------------------------------------------

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
// copies its stack to both halves).
struct EffectInstance {
    std::string effectId;
    bool enabled = true;
    std::vector<double> values;

    // The catalog entry for effectId, or nullptr when the id is unknown
    // (an instance from a newer catalog survives round-trips but is
    // skipped by the processor).
    const EffectDescriptor *descriptor() const;

    // Parameter value by key (descriptor order is the storage order).
    // Unknown key or wrong-size values -> fallback.
    double param(const std::string &key, double fallback = 0.0) const;

    // Sets and CLAMPS to the descriptor range; resizes values when the
    // instance predates a param (fills defaults). No-op for unknown keys.
    void setParam(const std::string &key, double value);

    // Convenience: param as bool (>= 0.5).
    bool paramBool(const std::string &key) const;
};

// The M5 Phase 1 catalog (25 effects across 5 categories).
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
void applyEffectStack(uint8_t *rgba, int width, int height,
                      const std::vector<EffectInstance> &stack);

} // namespace fc
