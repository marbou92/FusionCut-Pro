#include "project_format.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "effects.h"
#include "transitions.h"

namespace fc {

namespace {

// ---------------------------------------------------------------------------
// Minimal JSON DOM + recursive-descent parser (strict subset).
// ---------------------------------------------------------------------------

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolValue = false;
    double numberValue = 0.0;
    std::string stringValue;
    std::vector<JsonValue> arrayValue;
    // Ordered members (preserved for deterministic re-serialization).
    std::vector<std::pair<std::string, JsonValue>> objectValue;

    const JsonValue *find(const std::string &key) const {
        if (type != Type::Object) {
            return nullptr;
        }
        for (const auto &kv : objectValue) {
            if (kv.first == key) {
                return &kv.second;
            }
        }
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string &text) : s_(text) {}

    bool parse(JsonValue &out, std::string &error) {
        skipWs();
        if (!parseValue(out, error, 0)) {
            return false;
        }
        skipWs();
        if (pos_ != s_.size()) {
            error = "trailing characters after the JSON value";
            return false;
        }
        return true;
    }

private:
    const std::string &s_;
    size_t pos_ = 0;

    void skipWs() {
        while (pos_ < s_.size() &&
               (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) {
            ++pos_;
        }
    }

    bool fail(std::string &error, const char *message) {
        if (error.empty()) {
            error = std::string(message) + " (offset " + std::to_string(pos_) + ")";
        }
        return false;
    }

    bool parseValue(JsonValue &out, std::string &error, int depth) {
        if (depth > 64) {
            return fail(error, "nesting too deep");
        }
        skipWs();
        if (pos_ >= s_.size()) {
            return fail(error, "unexpected end of input");
        }
        const char c = s_[pos_];
        if (c == '{') {
            return parseObject(out, error, depth);
        }
        if (c == '[') {
            return parseArray(out, error, depth);
        }
        if (c == '"') {
            out.type = JsonValue::Type::String;
            return parseString(out.stringValue, error);
        }
        if (c == 't' || c == 'f') {
            out.type = JsonValue::Type::Bool;
            return parseBool(out.boolValue, error);
        }
        if (c == 'n') {
            if (s_.compare(pos_, 4, "null") == 0) {
                pos_ += 4;
                out.type = JsonValue::Type::Null;
                return true;
            }
            return fail(error, "bad literal");
        }
        return parseNumber(out, error);
    }

    bool parseObject(JsonValue &out, std::string &error, int depth) {
        out.type = JsonValue::Type::Object;
        ++pos_; // '{'
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            skipWs();
            if (pos_ >= s_.size() || s_[pos_] != '"') {
                return fail(error, "expected object key string");
            }
            std::string key;
            if (!parseString(key, error)) {
                return false;
            }
            skipWs();
            if (pos_ >= s_.size() || s_[pos_] != ':') {
                return fail(error, "expected ':' after object key");
            }
            ++pos_;
            JsonValue value;
            if (!parseValue(value, error, depth + 1)) {
                return false;
            }
            // Duplicate keys: last one wins (we never write them).
            bool replaced = false;
            for (auto &kv : out.objectValue) {
                if (kv.first == key) {
                    kv.second = std::move(value);
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                out.objectValue.emplace_back(std::move(key), std::move(value));
            }
            skipWs();
            if (pos_ >= s_.size()) {
                return fail(error, "unterminated object");
            }
            if (s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (s_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return fail(error, "expected ',' or '}' in object");
        }
    }

    bool parseArray(JsonValue &out, std::string &error, int depth) {
        out.type = JsonValue::Type::Array;
        ++pos_; // '['
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            JsonValue value;
            if (!parseValue(value, error, depth + 1)) {
                return false;
            }
            out.arrayValue.push_back(std::move(value));
            skipWs();
            if (pos_ >= s_.size()) {
                return fail(error, "unterminated array");
            }
            if (s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (s_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return fail(error, "expected ',' or ']' in array");
        }
    }

    bool parseString(std::string &out, std::string &error) {
        ++pos_; // opening '"'
        out.clear();
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return fail(error, "raw control character in string");
            }
            if (c == '\\') {
                ++pos_;
                if (pos_ >= s_.size()) {
                    return fail(error, "unterminated escape");
                }
                const char e = s_[pos_];
                switch (e) {
                case '"':
                    out += '"';
                    break;
                case '\\':
                    out += '\\';
                    break;
                case '/':
                    out += '/';
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!parseHex4(cp, error)) {
                        return false;
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 < s_.size() &&
                        s_[pos_ + 1] == '\\' && s_[pos_ + 2] == 'u') {
                        pos_ += 2; // consume '\' 'u'
                        uint32_t lo = 0;
                        if (!parseHex4(lo, error)) {
                            return false;
                        }
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default:
                    return fail(error, "invalid escape character");
                }
                ++pos_;
                continue;
            }
            out += c;
            ++pos_;
        }
        return fail(error, "unterminated string");
    }

    bool parseHex4(uint32_t &out, std::string &error) {
        if (pos_ + 4 >= s_.size() + 1 || s_.size() - pos_ < 5) {
            return fail(error, "truncated \\u escape");
        }
        uint32_t v = 0;
        for (int i = 1; i <= 4; ++i) {
            const char c = s_[pos_ + i];
            v <<= 4;
            if (c >= '0' && c <= '9') {
                v |= static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                v |= static_cast<uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                v |= static_cast<uint32_t>(c - 'A' + 10);
            } else {
                return fail(error, "invalid hex digit in \\u escape");
            }
        }
        pos_ += 4;
        out = v;
        return true;
    }

    static void appendUtf8(std::string &out, uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool parseBool(bool &out, std::string &error) {
        if (s_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            out = true;
            return true;
        }
        if (s_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            out = false;
            return true;
        }
        return fail(error, "bad literal");
    }

    bool parseNumber(JsonValue &out, std::string &error) {
        const size_t start = pos_;
        if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) {
            ++pos_;
        }
        bool digits = false;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') {
            ++pos_;
            digits = true;
        }
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') {
                ++pos_;
                digits = true;
            }
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) {
                ++pos_;
            }
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') {
                ++pos_;
            }
        }
        if (!digits) {
            return fail(error, "invalid number");
        }
        const std::string token = s_.substr(start, pos_ - start);
        errno = 0;
        char *end = nullptr;
        const double v = std::strtod(token.c_str(), &end);
        if (end != token.c_str() + token.size() || errno == ERANGE) {
            return fail(error, "unparsable number");
        }
        out.type = JsonValue::Type::Number;
        out.numberValue = v;
        return true;
    }
};

// ---------------------------------------------------------------------------
// Writer helpers (deterministic: fixed field order, no whitespace).
// ---------------------------------------------------------------------------

void appendEscaped(std::string &out, const std::string &in) {
    out += '"';
    for (const char c : in) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<int>(c));
                out += buf;
            } else {
                out += c;
            }
            break;
        }
    }
    out += '"';
}

// Integral values print exactly; others use %.10g (shortest sensible,
// round-trips every value this format ever stores). Locale-independent:
// the C locale is never changed by the app or its dependencies.
void appendDouble(std::string &out, double v) {
    if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
        out += buf;
        return;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.10g", v);
    out += buf;
}

void appendInt(std::string &out, int64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
    out += buf;
}

void appendBool(std::string &out, bool v) {
    out += v ? "true" : "false";
}

// ---- Reading helpers (typed access with schema checking). ----

bool getNumber(const JsonValue *node, double &out, std::string &error, const char *field) {
    if (!node || node->type != JsonValue::Type::Number) {
        error = std::string("field '") + field + "' missing or not a number";
        return false;
    }
    out = node->numberValue;
    return true;
}

bool getIntegral(const JsonValue *node, int64_t &out, std::string &error, const char *field) {
    double v = 0.0;
    if (!getNumber(node, v, error, field)) {
        return false;
    }
    if (std::floor(v) != v || std::fabs(v) > 9.0e15) {
        error = std::string("field '") + field + "' must be an integer";
        return false;
    }
    out = static_cast<int64_t>(v);
    return true;
}

bool getBool(const JsonValue *node, bool &out, std::string &error, const char *field) {
    if (!node || node->type != JsonValue::Type::Bool) {
        error = std::string("field '") + field + "' missing or not a boolean";
        return false;
    }
    out = node->boolValue;
    return true;
}

bool getString(const JsonValue *node, std::string &out, std::string &error, const char *field) {
    if (!node || node->type != JsonValue::Type::String) {
        error = std::string("field '") + field + "' missing or not a string";
        return false;
    }
    out = node->stringValue;
    return true;
}

bool getArray(const JsonValue *node, const std::vector<JsonValue> *&out, std::string &error,
              const char *field) {
    if (!node || node->type != JsonValue::Type::Array) {
        error = std::string("field '") + field + "' missing or not an array";
        return false;
    }
    out = &node->arrayValue;
    return true;
}

bool getObject(const JsonValue *node, const std::vector<std::pair<std::string, JsonValue>> *&out,
               std::string &error, const char *field) {
    if (!node || node->type != JsonValue::Type::Object) {
        error = std::string("field '") + field + "' missing or not an object";
        return false;
    }
    out = &node->objectValue;
    return true;
}

// Rebuilds one EffectInstance from its JSON object. Known effects load
// "params" by key into descriptor order (missing keys fall back to
// defaults); unknown effects keep a positional "values" copy so they
// round-trip untouched. Keyframes load generically (no descriptor
// needed - values arrive pre-clamped from a writer that had one).
bool parseEffectInstance(const JsonValue &node, EffectInstance &out, std::string &error) {
    if (node.type != JsonValue::Type::Object) {
        error = "effect entry is not an object";
        return false;
    }
    if (!getString(node.find("id"), out.effectId, error, "id")) {
        return false;
    }
    if (!getBool(node.find("enabled"), out.enabled, error, "enabled")) {
        return false;
    }
    const EffectDescriptor *d = findEffect(out.effectId);

    const JsonValue *params = node.find("params");
    if (params && params->type == JsonValue::Type::Object && d) {
        out.values.assign(d->params.size(), 0.0);
        for (size_t i = 0; i < d->params.size(); ++i) {
            out.values[i] = d->params[i].defaultValue;
        }
        for (const auto &kv : params->objectValue) {
            bool matched = false;
            for (size_t i = 0; i < d->params.size(); ++i) {
                if (d->params[i].key == kv.first) {
                    if (kv.second.type != JsonValue::Type::Number) {
                        error = "effect parameter '" + kv.first + "' is not a number";
                        return false;
                    }
                    out.values[i] = std::min(std::max(kv.second.numberValue, d->params[i].minValue),
                                             d->params[i].maxValue);
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                // A param this catalog no longer knows: dropped (the
                // instance still processes with the params it has).
                continue;
            }
        }
    } else if (params && params->type == JsonValue::Type::Array && !d) {
        // Unknown effect: positional values kept verbatim.
        out.values.clear();
        for (const JsonValue &v : params->arrayValue) {
            if (v.type != JsonValue::Type::Number) {
                error = "unknown-effect values array holds a non-number";
                return false;
            }
            out.values.push_back(v.numberValue);
        }
    } else if (d) {
        // No params stored: defaults.
        out.values.clear();
        for (const EffectParamDescriptor &p : d->params) {
            out.values.push_back(p.defaultValue);
        }
    }

    const JsonValue *kf = node.find("keyframes");
    if (kf && kf->type == JsonValue::Type::Object) {
        for (const auto &kv : kf->objectValue) {
            const std::vector<JsonValue> *points = nullptr;
            if (!getArray(&kv.second, points, error, "keyframe points")) {
                return false;
            }
            EffectKeyframeTrack track;
            track.key = kv.first;
            for (const JsonValue &point : *points) {
                const std::vector<JsonValue> *pair = nullptr;
                if (!getArray(&point, pair, error, "keyframe pair")) {
                    return false;
                }
                if (pair->size() != 2) {
                    error = "keyframe pair must be [frame, value]";
                    return false;
                }
                int64_t frame = 0;
                double value = 0.0;
                if (!getIntegral(&(*pair)[0], frame, error, "keyframe frame") ||
                    !getNumber(&(*pair)[1], value, error, "keyframe value")) {
                    return false;
                }
                if (frame < 0) {
                    error = "keyframe frame must be >= 0";
                    return false;
                }
                track.points.push_back({frame, value});
            }
            std::sort(
                track.points.begin(), track.points.end(),
                [](const EffectKeyframe &a, const EffectKeyframe &b) { return a.frame < b.frame; });
            // Drop duplicate frames (first wins - we never write them).
            track.points.erase(std::unique(track.points.begin(), track.points.end(),
                                           [](const EffectKeyframe &a, const EffectKeyframe &b) {
                                               return a.frame == b.frame;
                                           }),
                               track.points.end());
            if (!track.points.empty()) {
                out.keyframes.push_back(std::move(track));
            }
        }
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// serializeProject
// ---------------------------------------------------------------------------

std::string serializeProject(const TimelineModel &model) {
    std::string out;
    out.reserve(1024);
    out += "{\"format\":";
    appendInt(out, kProjectFormatVersion);
    out += ",\"fps\":";
    appendDouble(out, model.fps());

    out += ",\"tracks\":[";
    bool first = true;
    for (const Track &track : model.tracks()) {
        if (!first) {
            out += ',';
        }
        first = false;
        out += "{\"name\":";
        appendEscaped(out, track.name);
        out += ",\"audio\":";
        appendBool(out, track.isAudio);
        out += ",\"locked\":";
        appendBool(out, track.locked);
        out += ",\"muted\":";
        appendBool(out, track.muted);
        out += ",\"solo\":";
        appendBool(out, track.solo);
        out += '}';
    }
    out += ']';

    out += ",\"clips\":[";
    first = true;
    for (const Clip &clip : model.clips()) {
        if (!first) {
            out += ',';
        }
        first = false;
        out += "{\"id\":";
        appendInt(out, clip.id);
        out += ",\"track\":";
        appendInt(out, clip.trackIndex);
        out += ",\"source\":";
        appendEscaped(out, clip.sourcePath);
        out += ",\"label\":";
        appendEscaped(out, clip.label);
        out += ",\"in\":";
        appendInt(out, clip.sourceInFrames);
        out += ",\"out\":";
        appendInt(out, clip.sourceOutFrames);
        out += ",\"start\":";
        appendInt(out, clip.timelineStart);
        out += ",\"rate\":";
        appendDouble(out, clip.rate);

        out += ",\"effects\":[";
        bool firstFx = true;
        for (const EffectInstance &fx : clip.effectStack) {
            if (!firstFx) {
                out += ',';
            }
            firstFx = false;
            out += "{\"id\":";
            appendEscaped(out, fx.effectId);
            out += ",\"enabled\":";
            appendBool(out, fx.enabled);
            const EffectDescriptor *d = fx.descriptor();
            if (d) {
                // Known effect: params by key (catalog order changes are
                // harmless), stored in descriptor order.
                out += ",\"params\":{";
                for (size_t i = 0; i < d->params.size(); ++i) {
                    if (i > 0) {
                        out += ',';
                    }
                    appendEscaped(out, d->params[i].key);
                    out += ':';
                    const double v =
                        i < fx.values.size() ? fx.values[i] : d->params[i].defaultValue;
                    appendDouble(out, v);
                }
                out += '}';
            } else {
                // Unknown effect: positional values, verbatim.
                out += ",\"params\":[";
                for (size_t i = 0; i < fx.values.size(); ++i) {
                    if (i > 0) {
                        out += ',';
                    }
                    appendDouble(out, fx.values[i]);
                }
                out += ']';
            }
            if (!fx.keyframes.empty()) {
                out += ",\"keyframes\":{";
                bool firstTrack = true;
                for (const EffectKeyframeTrack &track : fx.keyframes) {
                    if (track.points.empty()) {
                        continue; // empty tracks never persist
                    }
                    if (!firstTrack) {
                        out += ',';
                    }
                    firstTrack = false;
                    appendEscaped(out, track.key);
                    out += ":[";
                    for (size_t i = 0; i < track.points.size(); ++i) {
                        if (i > 0) {
                            out += ',';
                        }
                        out += '[';
                        appendInt(out, track.points[i].frame);
                        out += ',';
                        appendDouble(out, track.points[i].value);
                        out += ']';
                    }
                    out += ']';
                }
                out += '}';
            }
            out += '}';
        }
        out += ']';
        out += '}';
    }
    out += ']';

    out += ",\"transitions\":[";
    first = true;
    for (const Transition &t : model.transitions()) {
        if (!first) {
            out += ',';
        }
        first = false;
        out += "{\"id\":";
        appendInt(out, t.id);
        out += ",\"track\":";
        appendInt(out, t.trackIndex);
        out += ",\"left\":";
        appendInt(out, t.leftClipId);
        out += ",\"right\":";
        appendInt(out, t.rightClipId);
        out += ",\"kind\":";
        appendEscaped(out, t.kind);
        out += ",\"duration\":";
        appendInt(out, t.durationFrames);
        out += '}';
    }
    out += ']';

    out += '}';
    return out;
}

// ---------------------------------------------------------------------------
// parseProject
// ---------------------------------------------------------------------------

bool parseProject(const std::string &text, TimelineModel &model, std::string &error) {
    JsonValue root;
    JsonParser parser(text);
    if (!parser.parse(root, error)) {
        return false;
    }
    if (root.type != JsonValue::Type::Object) {
        error = "project root is not a JSON object";
        return false;
    }

    int64_t format = 0;
    if (!getIntegral(root.find("format"), format, error, "format")) {
        return false;
    }
    if (format != kProjectFormatVersion) {
        error = "unsupported project format version " + std::to_string(format) +
                " (this build reads format " + std::to_string(kProjectFormatVersion) + ")";
        return false;
    }

    double fps = 0.0;
    if (!getNumber(root.find("fps"), fps, error, "fps") || fps <= 0.0 || fps > 1000.0) {
        if (error.empty()) {
            error = "fps out of range";
        }
        return false;
    }

    // ---- tracks ----
    const std::vector<JsonValue> *trackNodes = nullptr;
    if (!getArray(root.find("tracks"), trackNodes, error, "tracks")) {
        return false;
    }
    std::vector<Track> tracks;
    tracks.reserve(trackNodes->size());
    for (const JsonValue &node : *trackNodes) {
        if (node.type != JsonValue::Type::Object) {
            error = "track entry is not an object";
            return false;
        }
        Track track;
        if (!getString(node.find("name"), track.name, error, "track.name") ||
            !getBool(node.find("audio"), track.isAudio, error, "track.audio") ||
            !getBool(node.find("locked"), track.locked, error, "track.locked") ||
            !getBool(node.find("muted"), track.muted, error, "track.muted") ||
            !getBool(node.find("solo"), track.solo, error, "track.solo")) {
            return false;
        }
        track.index = static_cast<int>(tracks.size());
        tracks.push_back(track);
    }
    if (tracks.empty()) {
        error = "project has no tracks";
        return false;
    }

    // ---- clips ----
    const std::vector<JsonValue> *clipNodes = nullptr;
    if (!getArray(root.find("clips"), clipNodes, error, "clips")) {
        return false;
    }
    std::vector<Clip> clips;
    clips.reserve(clipNodes->size());
    for (const JsonValue &node : *clipNodes) {
        if (node.type != JsonValue::Type::Object) {
            error = "clip entry is not an object";
            return false;
        }
        Clip clip;
        int64_t clipTrack = 0;
        if (!getIntegral(node.find("id"), clip.id, error, "clip.id") ||
            !getIntegral(node.find("track"), clipTrack, error, "clip.track") ||
            !getString(node.find("source"), clip.sourcePath, error, "clip.source") ||
            !getString(node.find("label"), clip.label, error, "clip.label") ||
            !getIntegral(node.find("in"), clip.sourceInFrames, error, "clip.in") ||
            !getIntegral(node.find("out"), clip.sourceOutFrames, error, "clip.out") ||
            !getIntegral(node.find("start"), clip.timelineStart, error, "clip.start") ||
            !getNumber(node.find("rate"), clip.rate, error, "clip.rate")) {
            return false;
        }
        clip.trackIndex = clipTrack;
        if (clip.id <= 0) {
            error = "clip id must be positive";
            return false;
        }
        if (clip.trackIndex < 0 || clip.trackIndex >= static_cast<int>(tracks.size())) {
            error = "clip references a track that does not exist";
            return false;
        }
        if (clip.sourceInFrames < 0 || clip.sourceOutFrames <= clip.sourceInFrames) {
            error = "clip has an invalid source range";
            return false;
        }
        if (clip.timelineStart < 0) {
            error = "clip start must be >= 0";
            return false;
        }
        if (clip.rate <= 0.0 || clip.rate > 100.0) {
            error = "clip rate out of range";
            return false;
        }
        for (const Clip &other : clips) {
            if (other.id == clip.id) {
                error = "duplicate clip id";
                return false;
            }
        }

        const std::vector<JsonValue> *effectNodes = nullptr;
        if (!getArray(node.find("effects"), effectNodes, error, "clip.effects")) {
            return false;
        }
        for (const JsonValue &fxNode : *effectNodes) {
            EffectInstance fx;
            if (!parseEffectInstance(fxNode, fx, error)) {
                return false;
            }
            clip.effectStack.push_back(std::move(fx));
        }
        clips.push_back(std::move(clip));
    }

    // ---- transitions ----
    const std::vector<JsonValue> *transitionNodes = nullptr;
    if (!getArray(root.find("transitions"), transitionNodes, error, "transitions")) {
        return false;
    }
    std::vector<Transition> transitions;
    transitions.reserve(transitionNodes->size());
    for (const JsonValue &node : *transitionNodes) {
        if (node.type != JsonValue::Type::Object) {
            error = "transition entry is not an object";
            return false;
        }
        Transition t;
        int64_t tTrack = 0;
        if (!getIntegral(node.find("id"), t.id, error, "transition.id") ||
            !getIntegral(node.find("track"), tTrack, error, "transition.track") ||
            !getIntegral(node.find("left"), t.leftClipId, error, "transition.left") ||
            !getIntegral(node.find("right"), t.rightClipId, error, "transition.right") ||
            !getString(node.find("kind"), t.kind, error, "transition.kind") ||
            !getIntegral(node.find("duration"), t.durationFrames, error, "transition.duration")) {
            return false;
        }
        t.trackIndex = tTrack;
        if (t.id <= 0) {
            error = "transition id must be positive";
            return false;
        }
        if (t.durationFrames < 1) {
            error = "transition duration must be >= 1 frame";
            return false;
        }
        const Clip *left = nullptr;
        const Clip *right = nullptr;
        for (const Clip &clip : clips) {
            if (clip.id == t.leftClipId) {
                left = &clip;
            }
            if (clip.id == t.rightClipId) {
                right = &clip;
            }
        }
        if (!left || !right) {
            error = "transition references a clip that does not exist";
            return false;
        }
        if (left->trackIndex != t.trackIndex || right->trackIndex != t.trackIndex ||
            left->timelineEnd() != right->timelineStart) {
            error = "transition pair is not adjacent on its track";
            return false;
        }
        if (static_cast<size_t>(t.trackIndex) >= tracks.size() ||
            tracks[static_cast<size_t>(t.trackIndex)].isAudio) {
            error = "transition lives on a non-video track";
            return false;
        }
        if (t.durationFrames > left->durationFrames()) {
            error = "transition duration exceeds the outgoing clip";
            return false;
        }
        for (const Transition &other : transitions) {
            if (other.id == t.id) {
                error = "duplicate transition id";
                return false;
            }
            if (other.leftClipId == t.leftClipId && other.rightClipId == t.rightClipId) {
                error = "two transitions on one cut";
                return false;
            }
        }
        transitions.push_back(t);
    }

    // Everything validated: swap the whole model in one step.
    model.replaceAll(fps, std::move(tracks), std::move(clips), std::move(transitions));
    return true;
}

} // namespace fc
