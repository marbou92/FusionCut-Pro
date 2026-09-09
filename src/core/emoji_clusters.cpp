#include "emoji_clusters.h"

#include <algorithm>

namespace fc {

namespace {

// BMP codepoints whose Unicode default presentation is emoji (the
// stable Emoji_Presentation set; everything outside the SMP range is
// text-default and needs U+FE0F). Sorted by start; binary-searched.
struct Range {
    uint32_t first, last;
};

constexpr Range kBmpEmojiPresentation[] = {
    {0x231Au, 0x231Bu}, // watch, hourglass
    {0x23E9u, 0x23F3u}, // media controls, hourglasses
    {0x23F8u, 0x23FAu}, // pause, record
    {0x25AAu, 0x25ABu}, // small squares
    {0x25B6u, 0x25B6u}, // play
    {0x25C0u, 0x25C0u}, // reverse
    {0x25FBu, 0x25FEu}, // medium squares
    {0x2614u, 0x2615u}, // umbrella, hot beverages
    {0x2648u, 0x2653u}, // zodiac
    {0x267Fu, 0x267Fu}, // wheelchair
    {0x2693u, 0x2693u}, // anchor
    {0x26A1u, 0x26A1u}, // high voltage
    {0x26AAu, 0x26ABu}, // circles
    {0x26BDu, 0x26BEu}, // soccer, baseball
    {0x26C4u, 0x26C5u}, // snowman, sun behind cloud
    {0x26CEu, 0x26CEu}, // ophiuchus
    {0x26D4u, 0x26D4u}, // no entry
    {0x26EAu, 0x26EAu}, // church
    {0x26F2u, 0x26F3u}, // fountain, flag in hole
    {0x26F5u, 0x26F5u}, // sailboat
    {0x26FAu, 0x26FAu}, // tent
    {0x26FDu, 0x26FDu}, // fuel pump
    {0x2705u, 0x2705u}, // check mark
    {0x270Au, 0x270Bu}, // fists, hand
    {0x2728u, 0x2728u}, // sparkles
    {0x274Cu, 0x274Cu}, // cross mark
    {0x274Eu, 0x274Eu}, // negative cross mark
    {0x2753u, 0x2755u}, // question, exclamation
    {0x2757u, 0x2757u}, // heavy exclamation
    {0x2795u, 0x2797u}, // plus, minus, divide
    {0x27B0u, 0x27B0u}, // curly loop
    {0x27BFu, 0x27BFu}, // double curly loop
    {0x2B1Bu, 0x2B1Cu}, // big squares
    {0x2B50u, 0x2B50u}, // star
    {0x2B55u, 0x2B55u}, // circle
};

bool inSortedRanges(uint32_t cp, const Range *ranges, size_t n) {
    size_t lo = 0;
    size_t hi = n;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (cp < ranges[mid].first) {
            hi = mid;
        } else if (cp > ranges[mid].last) {
            lo = mid + 1;
        } else {
            return true;
        }
    }
    return false;
}

bool isRegionalIndicator(uint32_t cp) {
    return cp >= kRegionalIndicatorFirst && cp <= kRegionalIndicatorLast;
}

bool isSkinTone(uint32_t cp) {
    return cp >= kEmojiSkinToneFirst && cp <= kEmojiSkinToneLast;
}

bool isTagChar(uint32_t cp) {
    return cp >= kEmojiTagFirst && cp <= kEmojiTagLast;
}

bool isJoinerOrSelector(uint32_t cp) {
    return cp == kZeroWidthJoiner || cp == kEmojiVS16 || cp == kEmojiVS15 ||
           cp == kCombiningKeycap || isSkinTone(cp);
}

// Can a cluster START at this codepoint, given the codepoint that
// FOLLOWS it? Digits, letters, and text-default symbols only start a
// cluster when the follower forces it (a variation selector or the
// keycap combiner); regional indicators, tag-sequence bases, and
// default-emoji-presentation codepoints start one on their own.
bool isClusterStart(uint32_t cp, uint32_t next) {
    if (isJoinerOrSelector(cp)) {
        return false; // joiners/selectors/modifiers never lead
    }
    if (isDefaultEmojiPresentation(cp)) {
        return true;
    }
    if (isRegionalIndicator(cp)) {
        return true;
    }
    if (cp == 0x1F3F4u && isTagChar(next)) {
        return true; // tag-sequence flag base
    }
    if (next == kEmojiVS16 || next == kEmojiVS15 || next == kCombiningKeycap) {
        return true; // forced presentation / keycap
    }
    return false;
}

} // namespace

bool isDefaultEmojiPresentation(uint32_t cp) {
    // The SMP emoji blocks, wholesale (over-inclusive on purpose - see
    // the header).
    if (cp >= 0x1F000u && cp <= 0x1FAFFu) {
        return true;
    }
    return inSortedRanges(cp, kBmpEmojiPresentation,
                          sizeof(kBmpEmojiPresentation) / sizeof(kBmpEmojiPresentation[0]));
}

int emojiClusterLength(const uint32_t *cps, size_t count, size_t pos) {
    if (cps == nullptr || pos >= count) {
        return 0;
    }
    const uint32_t base = cps[pos];
    const uint32_t next = pos + 1 < count ? cps[pos + 1] : 0;
    if (!isClusterStart(base, next)) {
        return 0;
    }

    // Walk forward over the cluster. `adjacent` marks positions that
    // still directly extend the ORIGINAL base - the flag-pair and
    // tag-sequence rules apply only there (a third regional indicator
    // after a pair is a NEW cluster, not an extension).
    size_t end = pos + 1;
    bool adjacent = true;
    while (end < count) {
        const uint32_t cp = cps[end];
        if (cp == kEmojiVS16 || cp == kEmojiVS15 || cp == kCombiningKeycap || isSkinTone(cp)) {
            ++end; // attach to the cluster in any order
            continue;
        }
        if (cp == kZeroWidthJoiner) {
            // The joiner joins only when a cluster start follows it;
            // otherwise the cluster ends BEFORE the dangling joiner.
            if (end + 1 < count) {
                const uint32_t after = cps[end + 1];
                const uint32_t afterNext = end + 2 < count ? cps[end + 2] : 0;
                if (isClusterStart(after, afterNext)) {
                    end += 2; // joiner + the next base
                    adjacent = false;
                    continue;
                }
            }
            break;
        }
        if (adjacent && isRegionalIndicator(base) && isRegionalIndicator(cp)) {
            ++end; // the flag's second indicator
            adjacent = false;
            continue;
        }
        if (adjacent && base == 0x1F3F4u && isTagChar(cp)) {
            // Tag sequence: consume tag characters through the
            // terminator (E007F, CANCEL TAG - outside the regular tag
            // character range). A missing terminator still swallows the
            // tag run - the tags render as nothing either way, and the
            // layout must not break them out as plain text.
            while (end < count && (isTagChar(cps[end]) || cps[end] == kEmojiTagTerminator)) {
                ++end;
                if (cps[end - 1] == kEmojiTagTerminator) {
                    break;
                }
            }
            adjacent = false;
            continue;
        }
        break;
    }
    return static_cast<int>(end - pos);
}

} // namespace fc
