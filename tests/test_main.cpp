// FusionCut Pro - core engine unit tests.
// Plain asserts on purpose: zero dependencies, runs in milliseconds on any
// CI runner (including 1GB-class hardware the product targets).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "lru_cache.h"
#include "memory_pool.h"
#include "timecode.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

static void testFrameRate() {
    using namespace fc;

    CHECK(FrameRate::Fps24.isValid());
    CHECK(FrameRate::Fps2997NDF.isValid());
    CHECK(FrameRate().isValid()); // default 24/1

    CHECK(FrameRate::Fps24.toDouble() == 24.0);
    CHECK(std::fabs(FrameRate::Fps2997NDF.toDouble() - 29.9700299700) < 1e-9);
    CHECK(FrameRate::Fps2997DF.dropFrame);
    CHECK(!FrameRate::Fps2997NDF.dropFrame);
}

static void testTimecode() {
    using namespace fc;

    // Formatting
    CHECK(Timecode::fromFrames(0, FrameRate::Fps24).toString() == "00:00:00:00");
    CHECK(Timecode::fromFrames(90000, FrameRate::Fps25).toString() == "01:00:00:00");
    CHECK(Timecode::fromFrames(24 * 3600 - 1, FrameRate::Fps24).toString() == "00:59:59:23");
    CHECK(Timecode::fromFrames(30, FrameRate::Fps2997NDF).toString() == "00:00:01:00");
    CHECK(Timecode::fromFrames(-5, FrameRate::Fps24).toString() == "-00:00:00:05");

    // Components
    const Timecode t = Timecode::fromFrames(90000 + 25 * 61 + 5, FrameRate::Fps25);
    CHECK(t.hours() == 1);
    CHECK(t.minutes() == 1);
    CHECK(t.seconds() == 1);
    CHECK(t.frames() == 5);

    // Seconds conversion (30000/1001: one displayed second = 30 frames)
    const Timecode ntsc = Timecode::fromSeconds(1.0, FrameRate::Fps2997NDF);
    CHECK(ntsc.totalFrames() == 30);
    CHECK(std::fabs(ntsc.totalSeconds() - 1.001) < 1e-9);

    // Parsing
    Timecode p;
    CHECK(Timecode::parse("01:02:03:04", FrameRate::Fps24, p));
    CHECK(p.totalFrames() == (((1 * 60 + 2) * 60 + 3) * 24 + 4));
    CHECK(p.toString() == "01:02:03:04");

    CHECK(Timecode::parse("10:20:29", FrameRate::Fps30, p)); // MM:SS:FF (ff: 0-29)
    CHECK(p.totalFrames() == (620 * 30 + 29));

    CHECK(Timecode::parse("+00:00:00:07", FrameRate::Fps24, p));
    CHECK(p.totalFrames() == 7);
    CHECK(Timecode::parse("-00:00:00:05", FrameRate::Fps24, p));
    CHECK(p.totalFrames() == -5);
    CHECK(Timecode::parse("00;00;00;09", FrameRate::Fps24, p)); // ';' tolerated
    CHECK(p.totalFrames() == 9);

    // Round trip
    const Timecode rt = Timecode::fromFrames(123456, FrameRate::Fps30);
    Timecode rt2;
    CHECK(Timecode::parse(rt.toString(), FrameRate::Fps30, rt2));
    CHECK(rt == rt2);

    // Rejections
    CHECK(!Timecode::parse("", FrameRate::Fps24, p));
    CHECK(!Timecode::parse("garbage", FrameRate::Fps24, p));
    CHECK(!Timecode::parse("00:00:60:00", FrameRate::Fps24, p)); // ss out of range
    CHECK(!Timecode::parse("00:61:00:00", FrameRate::Fps24, p)); // mm out of range
    CHECK(!Timecode::parse("00:00:00:24", FrameRate::Fps24, p)); // ff == fps
    CHECK(!Timecode::parse("1:2:3:4:5", FrameRate::Fps24, p));   // too many fields
    CHECK(!Timecode::parse("01:02:03:", FrameRate::Fps24, p));   // trailing separator
    CHECK(!Timecode::parse("00:00:00:00:0x", FrameRate::Fps24, p));

    // Arithmetic (same-rate operands)
    const Timecode a = Timecode::fromFrames(50, FrameRate::Fps24);
    const Timecode b = Timecode::fromFrames(20, FrameRate::Fps24);
    CHECK((a - b).totalFrames() == 30);
    CHECK((b - a).totalFrames() == -30);
    CHECK((a + b).totalFrames() == 70);
}

static void testLruCache() {
    using namespace fc;

    LruCache<int, int> cache(2);
    cache.put(1, 10);
    cache.put(2, 20); // order (MRU first): [2, 1]
    CHECK(cache.size() == 2);

    CHECK(*cache.get(1) == 10);     // hit 1, promotes 1: [1, 2]
    cache.put(3, 30);               // full, evicts LRU (2): [3, 1]
    CHECK(cache.get(2) == nullptr); // miss 1
    CHECK(*cache.get(3) == 30);     // hit 2: [3, 1]
    CHECK(*cache.get(1) == 10);     // hit 3: [1, 3]
    cache.put(4, 40);               // full, evicts LRU (3): [4, 1]
    CHECK(cache.get(3) == nullptr); // miss 2
    CHECK(*cache.get(4) == 40);     // hit 4
    CHECK(*cache.get(1) == 10);     // hit 5

    CHECK(cache.hits() == 5);
    CHECK(cache.misses() == 2);

    // Update in place does not evict
    cache.put(4, 44);
    CHECK(*cache.get(4) == 44);

    // Erase
    CHECK(cache.erase(4));
    CHECK(!cache.erase(4));
    CHECK(cache.get(4) == nullptr);

    // Zero-capacity stores nothing
    LruCache<int, int> zero(0);
    zero.put(1, 10);
    CHECK(zero.size() == 0);
    CHECK(zero.get(1) == nullptr);

    // Clear resets stats
    cache.clear();
    CHECK(cache.size() == 0);
    CHECK(cache.hits() == 0);
    CHECK(cache.misses() == 0);
}

static void testMemoryPool() {
    using namespace fc;

    MemoryPool pool(1024, 4);
    CHECK(pool.blockSize() == 1024);
    CHECK(pool.capacity() == 4);
    CHECK(pool.available() == 4);
    CHECK(pool.inUse() == 0);
    CHECK(pool.totalBytes() >= 4 * 1024);

    void *a = pool.acquire();
    void *b = pool.acquire();
    void *c = pool.acquire();
    void *d = pool.acquire();
    CHECK(a != nullptr);
    CHECK(b != nullptr);
    CHECK(c != nullptr);
    CHECK(d != nullptr);
    CHECK(a != b && a != c && a != d && b != c && b != d && c != d);
    CHECK(pool.inUse() == 4);
    CHECK(pool.available() == 0);
    CHECK(pool.acquire() == nullptr); // exhausted, no growth

    // Alignment contract
    CHECK((reinterpret_cast<std::uintptr_t>(a) % 64) == 0);
    CHECK((reinterpret_cast<std::uintptr_t>(d) % 64) == 0);

    // Ownership
    CHECK(pool.owns(a));
    CHECK(!pool.owns(nullptr));
    int onStack = 0;
    CHECK(!pool.owns(&onStack));
    CHECK(!pool.owns(static_cast<unsigned char *>(a) + 1)); // mid-block

    // Release / reuse
    CHECK(pool.release(b));
    CHECK(pool.available() == 1);
    CHECK(!pool.release(b)); // double release rejected
    CHECK(pool.available() == 1);
    void *e = pool.acquire();
    CHECK(e == b); // freed block is recycled
    CHECK(pool.available() == 0);

    // Inert pool
    MemoryPool empty(0, 0);
    CHECK(empty.acquire() == nullptr);
    CHECK(empty.capacity() == 0);
}

int main() {
    testFrameRate();
    testTimecode();
    testLruCache();
    testMemoryPool();

    if (g_failures == 0) {
        std::printf("ALL PASSED: %d checks\n", g_checks);
        return 0;
    }
    std::printf("FAILED: %d of %d checks\n", g_failures, g_checks);
    return 1;
}
