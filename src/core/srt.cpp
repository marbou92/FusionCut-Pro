#include "srt.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace fc {

namespace {

// Splits on '\n' AND lone '\r' (CRLF, CR-only, and LF-only files all
// normalize to clean LF lines; a '\r' immediately before a '\n' is
// one separator, not two). Lines keep their other bytes verbatim.
// `lineNumbers` receives the 1-based line number of each split line
// (for error messages).
void splitLines(const std::string &utf8, std::vector<std::string> &lines,
                std::vector<size_t> &lineNumbers) {
    lines.clear();
    lineNumbers.clear();
    size_t start = 0;
    size_t number = 1;
    for (size_t i = 0; i <= utf8.size(); ++i) {
        if (i == utf8.size() || utf8[i] == '\n' || utf8[i] == '\r') {
            lines.emplace_back(utf8, start, i - start);
            lineNumbers.push_back(number);
            ++number;
            if (i < utf8.size() && utf8[i] == '\r' && i + 1 < utf8.size() && utf8[i + 1] == '\n') {
                ++i; // CRLF is one separator
            }
            start = i + 1;
        }
    }
}

// Consumes 1..maxDigits ASCII digits at s[pos..). Returns false when
// fewer than minDigits digits are available. Does not skip whitespace.
bool readDigits(const std::string &s, size_t &pos, int minDigits, int maxDigits, int64_t *out) {
    size_t p = pos;
    int64_t value = 0;
    int digits = 0;
    while (p < s.size() && digits < maxDigits && s[p] >= '0' && s[p] <= '9') {
        value = value * 10 + (s[p] - '0');
        ++p;
        ++digits;
    }
    if (digits < minDigits) {
        return false;
    }
    pos = p;
    *out = value;
    return true;
}

bool skipSpaces(const std::string &s, size_t &pos) {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) {
        ++pos;
    }
    return true;
}

// Parses "H+:MM:SS(.|,)mmm" at pos. Hours: 1..6 digits; minutes and
// seconds exactly 2; milliseconds exactly 3, ',' or '.' separator.
bool parseTimestamp(const std::string &s, size_t &pos, int64_t *ms, std::string &error) {
    int64_t h = 0, m = 0, sec = 0, msec = 0;
    if (!readDigits(s, pos, 1, 6, &h)) {
        error = "expected hours";
        return false;
    }
    if (pos >= s.size() || s[pos] != ':') {
        error = "expected ':' after hours";
        return false;
    }
    ++pos;
    if (!readDigits(s, pos, 2, 2, &m)) {
        error = "expected 2-digit minutes";
        return false;
    }
    if (pos >= s.size() || s[pos] != ':') {
        error = "expected ':' after minutes";
        return false;
    }
    ++pos;
    if (!readDigits(s, pos, 2, 2, &sec)) {
        error = "expected 2-digit seconds";
        return false;
    }
    if (pos >= s.size() || (s[pos] != ',' && s[pos] != '.')) {
        error = "expected ',' (or '.') before milliseconds";
        return false;
    }
    ++pos;
    if (!readDigits(s, pos, 3, 3, &msec)) {
        error = "expected 3-digit milliseconds";
        return false;
    }
    *ms = ((h * 60 + m) * 60 + sec) * 1000 + msec;
    return true;
}

// True when the line looks like it could START a timestamp (digit then
// more digits/dots/colons). Used to decide "index line vs timestamp
// line vs garbage".
bool looksLikeTimestampStart(const std::string &line) {
    if (line.empty() || !std::isdigit(static_cast<unsigned char>(line[0]))) {
        return false;
    }
    // An index line is ALL digits; a timestamp line is not.
    bool allDigits = true;
    for (char c : line) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            allDigits = false;
            break;
        }
    }
    return !allDigits;
}

bool isAllDigits(const std::string &line) {
    if (line.empty()) {
        return false;
    }
    for (char c : line) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

// Appends "H:MM:SS,mmm" (hours at least 2 digits, wider when needed).
void appendTimestamp(std::string &out, int64_t ms) {
    if (ms < 0) {
        ms = 0;
    }
    const int64_t msec = ms % 1000;
    const int64_t totalSec = ms / 1000;
    const int64_t sec = totalSec % 60;
    const int64_t totalMin = totalSec / 60;
    const int64_t min = totalMin % 60;
    const int64_t h = totalMin / 60;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld,%03lld", static_cast<long long>(h),
                  static_cast<long long>(min), static_cast<long long>(sec),
                  static_cast<long long>(msec));
    out += buf;
}

// Tag-name match at text[i] (i points just past '<' or past "</").
// Returns the length of the matched tag name, or 0.
size_t matchTagName(const std::string &text, size_t i, const char *name) {
    size_t n = 0;
    while (name[n] != '\0') {
        ++n;
    }
    if (i + n > text.size()) {
        return 0;
    }
    for (size_t k = 0; k < n; ++k) {
        if (std::tolower(static_cast<unsigned char>(text[i + k])) !=
            std::tolower(static_cast<unsigned char>(name[k]))) {
            return 0;
        }
    }
    return n;
}

} // namespace

bool parseSrt(const std::string &utf8, std::vector<SrtCue> &cues, std::string &error) {
    cues.clear();
    error.clear();

    // Strip a UTF-8 BOM.
    std::string body = utf8;
    if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEFu &&
        static_cast<unsigned char>(body[1]) == 0xBBu &&
        static_cast<unsigned char>(body[2]) == 0xBFu) {
        body.erase(0, 3);
    }

    std::vector<std::string> lines;
    std::vector<size_t> numbers;
    splitLines(body, lines, numbers);

    const auto lineError = [&numbers](size_t idx, const std::string &what) {
        return "line " + std::to_string(numbers[idx]) + ": " + what;
    };

    size_t i = 0;
    while (i < lines.size()) {
        // Skip blank lines between cues.
        while (i < lines.size() && lines[i].find_first_not_of(" \t") == std::string::npos) {
            ++i;
        }
        if (i >= lines.size()) {
            break;
        }

        // Optional index line.
        if (isAllDigits(lines[i]) && i + 1 < lines.size() &&
            !lines[i + 1].empty() /* a timestamp line always follows */) {
            // Only treat it as an index when the NEXT line is not itself
            // an index (guards "1\n2\n..." sequences from eating cues).
            if (!(isAllDigits(lines[i + 1]))) {
                ++i;
            }
        } else if (isAllDigits(lines[i])) {
            // A run of digits with nothing after it: malformed block.
            error = lineError(i, "cue number without a timestamp line");
            cues.clear();
            return false;
        }

        if (i >= lines.size()) {
            error = lineError(i, "unexpected end of file");
            cues.clear();
            return false;
        }

        // Timestamp line.
        const std::string &ts = lines[i];
        size_t pos = 0;
        skipSpaces(ts, pos);
        SrtCue cue;
        std::string what;
        if (!parseTimestamp(ts, pos, &cue.startMs, what)) {
            error = lineError(i, "bad start timestamp - " + what);
            cues.clear();
            return false;
        }
        skipSpaces(ts, pos);
        if (pos + 3 > ts.size() || ts.compare(pos, 3, "-->") != 0) {
            error = lineError(i, "missing '-->'");
            cues.clear();
            return false;
        }
        pos += 3;
        skipSpaces(ts, pos);
        if (!parseTimestamp(ts, pos, &cue.endMs, what)) {
            error = lineError(i, "bad end timestamp - " + what);
            cues.clear();
            return false;
        }
        skipSpaces(ts, pos);
        if (pos != ts.size()) {
            error = lineError(i, "unexpected text after the timestamps");
            cues.clear();
            return false;
        }
        ++i;

        // Text lines until a blank line or EOF.
        while (i < lines.size() && lines[i].find_first_not_of(" \t") != std::string::npos) {
            if (!cue.text.empty()) {
                cue.text += '\n';
            }
            cue.text += lines[i];
            ++i;
        }
        cues.push_back(std::move(cue));
    }
    return true;
}

std::string writeSrt(const std::vector<SrtCue> &cues) {
    std::string out;
    for (size_t k = 0; k < cues.size(); ++k) {
        const SrtCue &cue = cues[k];
        out += std::to_string(k + 1);
        out += "\r\n";
        appendTimestamp(out, cue.startMs);
        out += " --> ";
        appendTimestamp(out, cue.endMs);
        out += "\r\n";
        // The cue's text: split on '\n' into CRLF lines (empty text
        // emits no lines at all).
        if (!cue.text.empty()) {
            size_t start = 0;
            while (true) {
                const size_t nl = cue.text.find('\n', start);
                out.append(cue.text, start,
                           nl == std::string::npos ? std::string::npos : nl - start);
                out += "\r\n";
                if (nl == std::string::npos) {
                    break;
                }
                start = nl + 1;
            }
        }
        out += "\r\n"; // the blank line after the cue
    }
    return out;
}

std::string stripSrtMarkup(const std::string &text) {
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] != '<') {
            out += text[i];
            ++i;
            continue;
        }
        // A tag: <name ...> or </name>, case-insensitive, where name is
        // i, b, u, or font. Anything else stays verbatim.
        size_t j = i + 1;
        bool closing = false;
        if (j < text.size() && text[j] == '/') {
            closing = true;
            ++j;
            // Tolerate a space after the slash ("</ i>").
            if (j < text.size() && text[j] == ' ') {
                ++j;
            }
        }
        size_t matched = 0;
        matched = matchTagName(text, j, "i");
        if (matched == 0) {
            matched = matchTagName(text, j, "b");
        }
        if (matched == 0) {
            matched = matchTagName(text, j, "u");
        }
        if (matched == 0) {
            matched = matchTagName(text, j, "font");
        }
        if (matched > 0) {
            size_t k = j + matched;
            if (closing) {
                if (k < text.size() && text[k] == '>') {
                    i = k + 1; // </name>
                    continue;
                }
            } else {
                // Skip attribute bytes up to the '>' (quotes may contain
                // '>' - handle simple quoted spans).
                while (k < text.size() && text[k] != '>') {
                    if (text[k] == '"' || text[k] == '\'') {
                        const char quote = text[k];
                        ++k;
                        while (k < text.size() && text[k] != quote) {
                            ++k;
                        }
                    }
                    if (k < text.size()) {
                        ++k;
                    }
                }
                if (k < text.size() && text[k] == '>') {
                    i = k + 1; // <name ...>
                    continue;
                }
            }
        }
        // Not a recognized tag: keep the '<' and move on.
        out += text[i];
        ++i;
    }
    return out;
}

} // namespace fc
