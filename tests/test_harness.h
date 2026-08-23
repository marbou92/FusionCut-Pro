#pragma once

// Tiny shared test harness for all FusionCut test binaries. Header-only,
// C++17 inline globals, zero dependencies - mirrors the CHECK macro the
// v0.1 core tests were built on.

#include <cstdio>

inline int g_failures = 0;
inline int g_checks = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

// Prints the summary and returns the process exit code.
inline int testExitCode(const char *suiteName) {
    if (g_failures == 0) {
        std::printf("ALL PASSED (%s): %d checks\n", suiteName, g_checks);
        return 0;
    }
    std::printf("FAILED (%s): %d of %d checks\n", suiteName, g_failures, g_checks);
    return 1;
}
