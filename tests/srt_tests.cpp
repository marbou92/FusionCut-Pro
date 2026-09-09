// FusionCut Pro - SubRip caption unit tests.
// Pure parsing/writing: strict parser behaviors (tolerated quirks vs
// loud errors), the canonical writer's exact bytes, parse<->write
// round-trips, and the import markup stripper.

#include <string>
#include <vector>

#include "srt.h"
#include "test_harness.h"

using namespace fc;

// ---------------------------------------------------------------------------
// Parser: well-formed files and tolerated quirks.
// ---------------------------------------------------------------------------

static bool parses(const std::string &s, std::vector<SrtCue> &cues) {
    std::string err;
    const bool ok = parseSrt(s, cues, err);
    if (!ok) {
        std::printf("parse error: %s\n", err.c_str());
    }
    return ok;
}

static void testWellFormed() {
    std::vector<SrtCue> cues;
    // The canonical shape: index, timestamp, text, blank line.
    CHECK(parses("1\r\n"
                 "00:00:01,000 --> 00:00:04,000\r\n"
                 "Hello world\r\n"
                 "\r\n",
                 cues));
    CHECK(cues.size() == 1);
    CHECK(cues[0].startMs == 1000);
    CHECK(cues[0].endMs == 4000);
    CHECK(cues[0].text == "Hello world");

    // Multiple cues, LF endings, no trailing blank line.
    CHECK(parses("1\n00:00:00,000 --> 00:00:01,500\nFirst\n\n"
                 "2\n00:00:02,000 --> 00:00:03,000\nSecond line\nmore\n",
                 cues));
    CHECK(cues.size() == 2);
    CHECK(cues[1].text == "Second line\nmore");

    // No index lines at all.
    CHECK(parses("00:00:01,000 --> 00:00:02,000\nHi\n\n", cues));
    CHECK(cues.size() == 1);
    CHECK(cues[0].text == "Hi");

    // Mixed: some cues with indexes, some without.
    CHECK(parses("7\n00:00:01,000 --> 00:00:02,000\nA\n\n"
                 "00:00:03,000 --> 00:00:04,000\nB\n\n",
                 cues));
    CHECK(cues.size() == 2);

    // '.' as the millisecond separator, 1-digit hour.
    CHECK(parses("0:00:01.250 --> 00:00:02.500\nDot\n\n", cues));
    CHECK(cues[0].startMs == 1250);
    CHECK(cues[0].endMs == 2500);

    // UTF-8 BOM tolerated (the literal is split so the hex escape
    // cannot swallow the '1' that follows it).
    CHECK(parses("\xEF\xBB\xBF"
                 "1\r\n00:00:01,000 --> 00:00:02,000\nBOM\n\r\n",
                 cues));
    CHECK(cues.size() == 1);
    CHECK(cues[0].text == "BOM");

    // Lone-CR line endings.
    CHECK(parses("1\r00:00:01,000 --> 00:00:02,000\rCR\r\r", cues));
    CHECK(cues.size() == 1);
    CHECK(cues[0].text == "CR");

    // Extra blank lines between cues + trailing spaces on the timestamp
    // line + spaces-only text lines split blocks.
    CHECK(parses("\n\n1\n  00:00:01,000 --> 00:00:02,000  \nA\n\n\n\n"
                 "2\n00:00:03,000 --> 00:00:04,000\nB\n\n\n",
                 cues));
    CHECK(cues.size() == 2);

    // Empty text section: legal, round-trips.
    CHECK(parses("1\n00:00:01,000 --> 00:00:02,000\n\n", cues));
    CHECK(cues.size() == 1);
    CHECK(cues[0].text.empty());

    // Whitespace-only text line terminates the cue.
    CHECK(parses(
        "1\n00:00:01,000 --> 00:00:02,000\nA\n   \n2\n00:00:03,000 --> 00:00:04,000\nB\n\n", cues));
    CHECK(cues.size() == 2);

    // Empty input parses to zero cues, no error.
    CHECK(parses("", cues));
    CHECK(cues.empty());
    CHECK(parses("\n\n  \n", cues));
    CHECK(cues.empty());

    // Reversed times parse as-is (the importer decides what to do).
    CHECK(parses("1\n00:00:05,000 --> 00:00:02,000\nReversed\n\n", cues));
    CHECK(cues[0].startMs == 5000);
    CHECK(cues[0].endMs == 2000);

    // Cue index lines are never validated: out of order, duplicates.
    CHECK(parses("9\n00:00:01,000 --> 00:00:02,000\nA\n\n"
                 "9\n00:00:03,000 --> 00:00:04,000\nB\n\n",
                 cues));
    CHECK(cues.size() == 2);

    // Hours beyond 99 keep parsing (6 digits allowed).
    CHECK(parses("123:00:00,000 --> 123:00:01,000\nLong\n\n", cues));
    CHECK(cues[0].startMs == 123LL * 3600000LL);
}

// ---------------------------------------------------------------------------
// Parser: loud errors.
// ---------------------------------------------------------------------------

static void testErrors() {
    std::vector<SrtCue> cues;
    std::string err;

    CHECK(!parseSrt("1\n00:00:01,000 --> 00:00:02,000\n\n2\nnot a timestamp\n\n", cues, err));
    CHECK(err.find("line 5") != std::string::npos);

    // First line is neither digits nor a timestamp.
    CHECK(!parseSrt("hello\n00:00:01,000 --> 00:00:02,000\nA\n\n", cues, err));
    CHECK(err.find("line 1") != std::string::npos);

    // Missing '-->'.
    CHECK(!parseSrt("00:00:01,000 00:00:02,000\nA\n\n", cues, err));
    // Missing milliseconds separator.
    CHECK(!parseSrt("00:00:01 --> 00:00:02,000\nA\n\n", cues, err));
    // 1-digit minutes.
    CHECK(!parseSrt("00:0:01,000 --> 00:00:02,000\nA\n\n", cues, err));
    // 2-digit milliseconds.
    CHECK(!parseSrt("00:00:01,00 --> 00:00:02,000\nA\n\n", cues, err));
    // 4-digit milliseconds.
    CHECK(!parseSrt("00:00:01,0000 --> 00:00:02,000\nA\n\n", cues, err));
    // Junk after the end timestamp.
    CHECK(!parseSrt("00:00:01,000 --> 00:00:02,000 X\nA\n\n", cues, err));
    // A cue number with nothing after it.
    CHECK(!parseSrt("12\n", cues, err));
    CHECK(!parseSrt("12\n\n", cues, err));
    // A cue number followed by a blank line then a timestamp: the index
    // is separated from its timestamp - malformed.
    CHECK(!parseSrt("1\n\n00:00:01,000 --> 00:00:02,000\nA\n\n", cues, err));
    // Not-digits line where a timestamp belongs.
    CHECK(!parseSrt("abc\n", cues, err));

    // Every failure path clears the cues.
    parseSrt("1\n00:00:01,000 --> 00:00:02,000\nA\n\n2\nbad\n\n", cues, err);
    CHECK(!parseSrt("1\n00:00:01,000 --> 00:00:02,000\nA\n\n2\nbad\n\n", cues, err));
    CHECK(cues.empty());
}

// ---------------------------------------------------------------------------
// Writer: exact canonical bytes.
// ---------------------------------------------------------------------------

static void testWriter() {
    // Empty list -> empty file.
    CHECK(writeSrt({}) == "");

    std::vector<SrtCue> cues;
    SrtCue a;
    a.startMs = 1000;
    a.endMs = 4000;
    a.text = "Hello world";
    cues.push_back(a);
    SrtCue b;
    b.startMs = 10000;
    b.endMs = 10500;
    b.text = "Two\nlines";
    cues.push_back(b);

    const std::string out = writeSrt(cues);
    const std::string want = "1\r\n"
                             "00:00:01,000 --> 00:00:04,000\r\n"
                             "Hello world\r\n"
                             "\r\n"
                             "2\r\n"
                             "00:00:10,000 --> 00:00:10,500\r\n"
                             "Two\r\n"
                             "lines\r\n"
                             "\r\n";
    CHECK(out == want);

    // Milliseconds carry into seconds/minutes/hours (61500 = 1 m 1.5 s;
    // 7200000 = 2 h).
    SrtCue c;
    c.startMs = 61500;
    c.endMs = 7200000;
    c.text = "x";
    CHECK(writeSrt({c}) == "1\r\n00:01:01,500 --> 02:00:00,000\r\nx\r\n\r\n");

    // Negative times clamp to zero; 100+ hours widen the hour field.
    SrtCue d;
    d.startMs = -5;
    d.endMs = 360000000; // 100 hours
    d.text = "y";
    CHECK(writeSrt({d}) == "1\r\n00:00:00,000 --> 100:00:00,000\r\ny\r\n\r\n");

    // Empty text writes no text lines.
    SrtCue e;
    e.startMs = 0;
    e.endMs = 500;
    CHECK(writeSrt({e}) == "1\r\n00:00:00,000 --> 00:00:00,500\r\n\r\n");

    // Indexes are renumbered regardless of the input order.
    std::vector<SrtCue> swapped;
    SrtCue late;
    late.startMs = 5000;
    late.endMs = 6000;
    late.text = "late";
    swapped.push_back(late);
    SrtCue early;
    early.startMs = 0;
    early.endMs = 1000;
    early.text = "early";
    swapped.push_back(early);
    const std::string s = writeSrt(swapped);
    CHECK(s.find("1\r\n00:00:05,000") == 0); // order preserved as given
}

// ---------------------------------------------------------------------------
// Round-trips.
// ---------------------------------------------------------------------------

static void testRoundTrips() {
    // write -> parse -> equal cues.
    std::vector<SrtCue> cues;
    SrtCue a;
    a.startMs = 1234;
    a.endMs = 5678;
    a.text = "Multi\nline\ntext";
    cues.push_back(a);
    SrtCue b;
    b.startMs = 90000;
    b.endMs = 91000;
    b.text = "Emoji \xF0\x9F\x98\x80 and \xC3\xA9\xC3\xA8";
    cues.push_back(b);
    SrtCue c; // empty text
    c.startMs = 100;
    c.endMs = 200;
    cues.push_back(c);

    std::vector<SrtCue> back;
    CHECK(parses(writeSrt(cues), back));
    CHECK(back.size() == cues.size());
    CHECK(back == cues);

    // parse -> write -> byte-stable on the second pass.
    const std::string file = "1\n00:00:01,000 --> 00:00:02,000\nA\n\n"
                             "2\n00:00:03,500 --> 00:00:04,250\nB\nsecond\n\n";
    std::vector<SrtCue> first;
    CHECK(parses(file, first));
    const std::string once = writeSrt(first);
    std::vector<SrtCue> second;
    CHECK(parses(once, second));
    CHECK(writeSrt(second) == once);
}

// ---------------------------------------------------------------------------
// Markup stripping.
// ---------------------------------------------------------------------------

static void testStripMarkup() {
    CHECK(stripSrtMarkup("<i>Hello</i>") == "Hello");
    CHECK(stripSrtMarkup("<b>Bold</b> and <u>under</u>") == "Bold and under");
    CHECK(stripSrtMarkup("<font color=\"red\">Red</font>") == "Red");
    CHECK(stripSrtMarkup("<FONT SIZE=\"12\">Big</FONT>") == "Big");
    CHECK(stripSrtMarkup("<i><b>Nested</b></i>") == "Nested");
    CHECK(stripSrtMarkup("A <i>mix</i> of \xE2\x9C\x93 things") == "A mix of \xE2\x9C\x93 things");
    CHECK(stripSrtMarkup("</ i >") == "</ i >"); // spaces INSIDE the name: unknown tag
    CHECK(stripSrtMarkup("</i>") == "");         // bare closing tag
    CHECK(stripSrtMarkup("5 < 6 and 7 > 4") == "5 < 6 and 7 > 4"); // lone angle
    CHECK(stripSrtMarkup("<marquee>unknown tag</marquee>") == "<marquee>unknown tag</marquee>");
    CHECK(stripSrtMarkup("<font color=\"has>quote\">x</font>") == "x");
    CHECK(stripSrtMarkup("<font color=\"unterminated>x") == "<font color=\"unterminated>x");
    CHECK(stripSrtMarkup("no tags at all") == "no tags at all");
    CHECK(stripSrtMarkup("") == "");
    // A quoted '>' inside attributes is not the tag end.
    CHECK(stripSrtMarkup("<font face=\"a>b\">t</font>") == "t");
}

int main() {
    testWellFormed();
    testErrors();
    testWriter();
    testRoundTrips();
    testStripMarkup();
    return testExitCode("srt");
}
