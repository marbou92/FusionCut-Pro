#pragma once

#include <cstddef>
#include <cstdint>

namespace fc {

// ---------------------------------------------------------------------------
// Emoji cluster segmentation - a pure Unicode policy, no fonts.
//
// The text engine draws emoji through the PLATFORM font stack: a cluster
// (a zero-width-joiner chain, a flag pair, a keycap, a skin-tone
// sequence, a tag-sequence flag) is handed to QPainter as ONE string so
// the platform shaper can ligate it (Segoe UI Emoji on Windows, Apple
// Color Emoji on macOS, whatever the desktop resolved for Linux), and it
// is measured with the same string metrics so layout reserves exactly
// what the renderer paints. The font families the user picks in the Text
// panel (or the platform fallback when the family has no emoji) decide
// WHICH emoji glyphs appear; this module only decides where CLUSTER
// boundaries are, so sequences never split across lines, draw calls, or
// typewriter reveals.
//
// Over-clustering is harmless (a cluster is still drawn as its exact
// codepoint string and measured as that exact string); UNDER-clustering
// splits a ligature into pieces. The policy below therefore errs on the
// side of clustering.
//
// Pure fc_core: no Qt, no FFmpeg, no state, no allocations. The unit
// tests pin every rule with hand-built codepoint arrays.
// ---------------------------------------------------------------------------

// Variation selectors and the joiner: never rendered as glyphs by the
// cluster policy (the platform decides their ink); they steer cluster
// resolution. U+FE0F forces emoji presentation, U+FE0E text presentation.
constexpr uint32_t kEmojiVS16 = 0xFE0Fu;               // emoji presentation
constexpr uint32_t kEmojiVS15 = 0xFE0Eu;               // text presentation
constexpr uint32_t kZeroWidthJoiner = 0x200Du;         // joins emoji into ligatures
constexpr uint32_t kCombiningKeycap = 0x20E3u;         // keycap sequence terminator
constexpr uint32_t kEmojiSkinToneFirst = 0x1F3FBu;     // light skin tone
constexpr uint32_t kEmojiSkinToneLast = 0x1F3FFu;      // dark skin tone
constexpr uint32_t kRegionalIndicatorFirst = 0x1F1E6u; // 'A'
constexpr uint32_t kRegionalIndicatorLast = 0x1F1FFu;  // 'Z'
constexpr uint32_t kEmojiTagFirst = 0xE0020u;          // tag sequence characters
constexpr uint32_t kEmojiTagLast = 0xE007Eu;
constexpr uint32_t kEmojiTagTerminator = 0xE007Fu; // ends a tag sequence

// Unicode "default emoji presentation" policy for single codepoints:
// true when the codepoint's default presentation is a colorful emoji
// (no U+FE0F needed). The SMP emoji range is taken wholesale
// (over-inclusive on purpose - a lone codepoint there is measured and
// drawn as one unit either way); the BMP set is the stable
// Emoji_Presentation list (codepoints like U+2764 HEAVY BLACK HEART or
// U+2600 SUN are TEXT-default and need U+FE0F to join a cluster - that
// is Unicode's own rule, not a heuristic).
bool isDefaultEmojiPresentation(uint32_t cp);

// Returns the length in CODEPOINTS of the emoji cluster starting at
// cps[pos]:
//   >= 2  a multi-codepoint sequence (joiner chain, flag pair, keycap,
//         skin tone, variation selector, tag sequence) - the layout
//         engine treats [pos, pos+len) as one atomic unit;
//   1     a single emoji-presentation codepoint (measured as one unit;
//         no continuation codepoints to protect);
//   0     plain text starts at pos (no cluster).
// Never reads past cps[count]. Joiner/selector codepoints never start a
// cluster. A joiner is only consumed when the codepoint AFTER it can
// itself start a cluster; otherwise the cluster ends before the joiner.
int emojiClusterLength(const uint32_t *cps, size_t count, size_t pos);

} // namespace fc
