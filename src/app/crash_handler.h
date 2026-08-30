// FusionCut Pro - process-wide crash diagnostics (Module 0 / runtime
// resilience). Replaces the run-console.bat diagnostic launcher with a
// built-in handler that captures the *real* failure (access violation,
// stack overflow, pure-virtual call, uncaught C++ exception, OOM,
// POSIX signal, CRT invalid parameter) and writes a structured report
// the user can hand back to the developer.
//
// Lives in src/app (NOT fc_core) because MessageBoxA is a user-facing
// GUI call - fc_core stays UI-free + C ABI per the architecture hedge.
// The Qt layer links this; fc_core tests do not, so the MSVC core-only
// CI leg is unaffected.
//
// Lifecycle: call fc::installCrashHandler(version, reportDir) ONCE, before
// QApplication construction in main(). Installing earlier than that catches
// the qwindows.dll platform-plugin init path (the historic 0xc0000005
// startup crash mode on machines without MSYS2's plugin path).

#pragma once

#include <string>

namespace fc {

// Install all available handlers for the host platform. Idempotent.
//
//   appVersion - human-readable version baked into the report header
//                (empty == "unknown")
//   reportDir - absolute directory for crash-*.log files. Empty means
//               "next to the executable" on Windows and "current working
//               directory" on POSIX. The directory must already exist
//               (the handler never creates directories - it cannot safely
//               do so from inside an exception context).
//
// On crash, the handler:
//   1. Writes FusionCutPro-crash-<YYYYMMDD-HHMMSS.mmm>.log into reportDir
//      with version, timestamp, exception kind/code/address, a best-effort
//      stack backtrace, and (Windows) the loaded-module list.
//   2. Mirrors a one-line summary to stderr so a console launch still
//      shows the failure.
//   3. On Windows, opens a modal MessageBox naming the log file path so
//      a double-click launch (no console) sees something actionable
//      instead of the bare "0xc0000005" dialog.
//   4. Re-raises the original exception / terminates the process.
//
// The handler is reentrancy-guarded; a crash inside the handler itself
// falls through to the default OS disposition without recursing.
void installCrashHandler(const std::string &appVersion = {}, const std::string &reportDir = {});

// Test hook: writes a synthetic report (kind="Manual") and returns the
// absolute path written. Used by the --crash-test CLI hook in main.cpp
// for end-to-end verification on a clean Windows machine. No-op on
// non-Windows hosts where the test hook is exercised via signal+log.
std::string writeManualCrashReport(const std::string &appVersion = {},
                                   const std::string &reportDir = {});

} // namespace fc
