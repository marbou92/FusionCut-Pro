#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fc {

// ---------------------------------------------------------------------------
// SubRip (.srt) captions: a strict parser, a canonical writer, and a
// small markup stripper for importing.
//
// Pure fc_core: no Qt, no FFmpeg, no state. The parser is strict -
// a malformed file fails with a message naming the 1-based line number
// and what went wrong (the same philosophy as the project-file parser:
// captions are user data, and silent misreads are worse than a loud
// error). What it TOLERATES, because real .srt files do it constantly:
//   * a UTF-8 BOM,
//   * CRLF, lone CR, or LF line endings,
//   * trailing whitespace on the timestamp line,
//   * a missing cue-index line (or a mixture of present and absent),
//   * '.' instead of ',' as the millisecond separator,
//   * 1-digit hours,
//   * any number of blank lines between cues,
//   * cues whose end precedes their start (parsed as-is; the importer
//     skips them),
//   * an empty text section (round-trips).
// Cue INDEX numbers are ignored on read (never validated) and renumbered
// sequentially on write.
//
// The writer is canonical: 2-digit-or-more hours, ',' milliseconds,
// CRLF line endings, one blank line after every cue (including the
// last). parse(write(cues)) == cues for every parseable cue list, and
// write(parse(file)) is byte-stable on a second pass.
//
// Caption TEXT is kept verbatim (markup tags included). The importer
// strips the common presentation tags via stripSrtMarkup before turning
// a cue into a text clip.
// ---------------------------------------------------------------------------

// One caption cue. Times are milliseconds from the file's start.
struct SrtCue {
    int64_t startMs = 0;
    int64_t endMs = 0;
    std::string text; // UTF-8; '\n' separates the cue's lines (verbatim)

    bool operator==(const SrtCue &other) const {
        return startMs == other.startMs && endMs == other.endMs && text == other.text;
    }
    bool operator!=(const SrtCue &other) const { return !(*this == other); }
};

// Parses the whole file. On failure returns false, clears `cues`, and
// fills `error` with a human-readable message ("line N: ..."). An empty
// (or whitespace-only) file parses successfully to zero cues.
bool parseSrt(const std::string &utf8, std::vector<SrtCue> &cues, std::string &error);

// Writes the canonical form. Negative millisecond values clamp to 0.
std::string writeSrt(const std::vector<SrtCue> &cues);

// Removes the common SRT presentation tags: <i>, <b>, <u>, <font ...>
// and their closing forms (case-insensitive, with or without a space in
// "</ font>"). Everything else - including unknown tags and lone '<' -
// stays verbatim (round-trip safety: only unambiguous presentation
// markup is stripped, and the parser above never invents tags).
std::string stripSrtMarkup(const std::string &text);

} // namespace fc
