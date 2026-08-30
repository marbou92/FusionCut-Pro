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
// Lifecycle: a static-storage initializer in crash_handler.cpp calls
// installCrashHandler("", "") at .CRT$XCU static-init time, BEFORE
// main() runs. This is intentional - it makes the Windows VEH active
// before QApplication pulls in Qt5Core / qwindows.dll, which is the
// exact class of 0xc0000005 startup crash the v0.4.1 in-main() install
// could not catch. main() then calls installCrashHandler again with
// the real FC_VERSION_STRING; the idempotent guard keeps the handlers
// installed but updates the version + report dir fields. Callers
// should still call installCrashHandler(VERSION) from main() so the
// version string in any crash report is the real one, not "unknown".

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
//
// Idempotency: the version + report dir fields are updated on EVERY
// call, but the handlers themselves are installed exactly once (the
// static initializer's call installs them; main()'s call only updates
// version + dir). This split is what lets the VEH be active before
// main() while still letting main() supply the real version string.
void installCrashHandler(const std::string &appVersion = {}, const std::string &reportDir = {});

// Append a milestone line to the boot-trace log file
// (FusionCutPro-boot-<timestamp>.log, next to the executable). Called
// by main() at every major startup checkpoint (entered main, about to
// construct QApplication, QApplication constructed, MainWindow
// constructed, app.exec entered). The boot trace is the only
// diagnostic that survives a loader-phase crash (one that happens
// before the VEH is wired up); it is embedded into the crash report
// so the developer sees how far startup got. No-op if the trace file
// could not be opened.
//
//   stage   - small integer milestone id (caller-chosen; 0 and 1 are
//             reserved for the static initializer and the late
//             installCrashHandler call respectively)
//   message - human-readable description, e.g. "QApplication constructed"
void recordBootStage(int stage, const char *message);

// Mark the process as exiting cleanly and close + delete the boot
// trace file. Called from main() immediately before returning 0 from
// a successful app.exec(). If this is NOT called, the boot trace file
// is left on disk for the developer to inspect (intentional - on a
// crash we never call this, so the trace survives and is embedded
// into the crash report).
void shutdownCrashHandler();

// Read the current contents of the boot-trace log file into a string.
// Used by tests + by the crash handler itself when embedding the
// trace into a crash report. Returns "(boot trace not open)" if the
// file could not be opened at static-init time.
std::string bootTraceContent();

// Test hook: writes a synthetic report (kind="Manual") and returns the
// absolute path written. Used by the --crash-test CLI hook in main.cpp
// for end-to-end verification on a clean Windows machine. No-op on
// non-Windows hosts where the test hook is exercised via signal+log.
std::string writeManualCrashReport(const std::string &appVersion = {},
                                   const std::string &reportDir = {});

} // namespace fc
