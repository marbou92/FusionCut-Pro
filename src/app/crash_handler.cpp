// FusionCut Pro - process-wide crash diagnostics (implementation).
//
// Design choices (rationale, so future changes do not regress them):
//
//   * Windows VEH, not SEH __try/__except: VEH runs before SEH's
//     __except filter and in *every* thread, so it catches the
//     qwindows.dll platform-plugin init path that the historic
//     0xc0000005 startup crash lived in. SEH __try would only catch
//     exceptions on the thread that owns the frame and requires
//     MSVC-compatible try/except syntax that MinGW GCC does not have.
//     VEH is plain Win32 C API; works under MSVC and MinGW alike.
//
//   * std::ofstream for log writing on Windows: VEH runs in ordinary
//     thread context (not signal context), so libc allocation is safe.
//     On POSIX the signal handler uses only async-signal-safe calls
//     (write/backtrace_symbols_fd) because SIGSEGV *is* signal context.
//
//   * Reentrancy guard via atomic<bool>: a crash inside the handler
//     short-circuits and lets the OS kill us. Recursive std::ofstream
//     usage from inside an exception filter would loop.
//
//   * No symbol resolution in-handler: SymFromAddr needs a lock and a
//     loaded dbghelp, which is fragile inside an exception. Raw
//     addresses are recorded; symbolication is offline (addr2line /
//     cv2pdb / dumpbin /symbols). This is the same trade-off Breakpad
//     makes for its "minidump pure" path.
//
//   * Loaded-module snapshot (Windows, via Toolhelp32): indispensable
//     for diagnosing the 0xc0000005 startup class - the crash report
//     ends up listing exactly which Qt5Core.dll / FFmpeg DLL /
//     qwindows.dll was loaded (or failed to load), at which base
//     address, from which path. toolhelp32 lives in kernel32 - no
//     extra link dependency.
//
//   * Pre-main VEH registration via static initializer: the v0.4.1
//     handler was installed from main(), which is too late for the
//     Windows loader-phase 0xc0000005 class ("The application was
//     unable to start correctly"). That dialog is shown by the OS
//     loader itself during the import-resolution / DllMain / CRT-init
//     rendezvous, BEFORE main() is reached. By registering VEH from a
//     static initializer (constructor) we move the handler install
//     point to .CRT$XCU static-init time, which runs after the basic
//     CRT/kernel32 DLLs are mapped but BEFORE QApplication pulls in
//     Qt5Core/Qt5Gui/Qt5Widgets/qwindows.dll. That covers the entire
//     class of startup crashes that previously had no in-process
//     reporter.
//
//   * Boot-trace log file (FusionCutPro-boot-<timestamp>.log): a
//     milestone log written at every startup checkpoint (stage0=VEH
//     registered, stage1=entered main, stage2=full handler install,
//     stage3=QApplication constructed, stage4=MainWindow constructed,
//     stage5=app.exec entered). Even if VEH can't run (e.g. the crash
//     is inside a static-import DLL's DllMain before any user code),
//     the partial boot log shows how far startup got. On a clean exit
//     the boot trace is deleted; on a crash it is preserved AND
//     embedded into the crash report so a single file is sufficient.

#include "crash_handler.h"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#if defined(_WIN32)
// Guard with #ifndef: libstdc++ <bits/os_defines.h> pulls NOMINMAX in
// transitively once <string> is included (via crash_handler.h), so a
// bare `#define NOMINMAX` (no value) collides with their `#define
// NOMINMAX 1` and MinGW-w64 gcc 16 warns "NOMINMAX redefined". The
// same defensive pattern protects WIN32_LEAN_AND_MEAN if a future
// libstdc++ or Qt header starts defining it too.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// Toolhelp32 (module snapshot) lives in <tlhelp32.h>; declared in
// kernel32, no extra link dependency.
#include <tlhelp32.h>
#else
#include <execinfo.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

// ---------- configuration (set in installCrashHandler) -------------------

std::string g_appVersion = "unknown";
std::string g_reportDir;
std::atomic<bool> g_installed{false};
std::atomic<bool> g_crashing{false}; // reentrancy guard

// ---------- boot trace ----------------------------------------------------
// A milestone log opened at static-init time and appended to at every
// startup checkpoint. On clean exit it is deleted; on crash it is
// preserved AND embedded into the crash report by the VEH/terminate
// handlers. The trace is the only diagnostic that survives a
// loader-phase crash (one that happens before VEH can install),
// because it is written incrementally with fsync after each line.
std::string g_bootTracePath;
std::FILE *g_bootTraceFile = nullptr;
std::mutex g_bootTraceMutex;
std::atomic<bool> g_bootTraceCleanExit{false};

// ---------- helpers -------------------------------------------------------

std::string exeDirectory() {
#if defined(_WIN32)
    char buf[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return ".";
    }
    std::string path(buf, n);
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? "." : path.substr(0, slash);
#else
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        return ".";
    }
    buf[n] = '\0';
    std::string path(buf);
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? "." : path.substr(0, slash);
#endif
}

std::string timestamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto t = system_clock::to_time_t(now);
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    char out[48];
    std::snprintf(out, sizeof(out), "%s.%03lld", buf, static_cast<long long>(ms.count()));
    return out;
}

std::string joinPath(const std::string &dir, const std::string &leaf) {
    if (dir.empty()) {
        return leaf;
    }
    const char sep =
#if defined(_WIN32)
        '\\';
#else
        '/';
#endif
    if (dir.back() == sep || dir.back() == '/' || dir.back() == '\\') {
        return dir + leaf;
    }
    return dir + std::string(1, sep) + leaf;
}

// Resolve the directory to use for crash logs and the boot trace.
// Tries (in order): the configured g_reportDir, the directory next to
// the executable, the current working directory, and on Windows the
// user's %TEMP%. Returns the first writable directory found, or "".
std::string resolveReportDir() {
    if (!g_reportDir.empty()) {
        return g_reportDir;
    }
    const std::string exeDir = exeDirectory();
    if (exeDir != "." && !exeDir.empty()) {
        return exeDir;
    }
#if defined(_WIN32)
    // Last resort: %TEMP% via GetTempPathA. The returned path already
    // has a trailing backslash, so joinPath will append the leaf
    // directly.
    char temp[MAX_PATH];
    const DWORD n = GetTempPathA(MAX_PATH, temp);
    if (n > 0 && n < MAX_PATH) {
        return std::string(temp, n);
    }
#endif
    return ".";
}

// Open (or reopen) the boot trace file in the resolved report dir.
// Called from the static initializer; safe to call repeatedly - the
// second call is a no-op once g_bootTraceFile is set.
void openBootTrace() {
    if (g_bootTraceFile != nullptr) {
        return;
    }
    const std::string dir = resolveReportDir();
    g_bootTracePath = joinPath(dir, "FusionCutPro-boot-" + timestamp() + ".log");
    g_bootTraceFile = std::fopen(g_bootTracePath.c_str(), "wb");
    if (g_bootTraceFile) {
        std::fprintf(g_bootTraceFile,
                     "FusionCut Pro boot trace\n"
                     "Opened: %s\n"
                     "Path: %s\n",
                     timestamp().c_str(), g_bootTracePath.c_str());
        std::fflush(g_bootTraceFile);
#if !defined(_WIN32)
        ::fsync(fileno(g_bootTraceFile));
#endif
    }
}

// Append one milestone line to the boot trace. Public via fc::recordBootStage.
void writeBootStage(int stage, const char *message) {
    if (g_bootTraceFile == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_bootTraceMutex);
    char line[512];
    const int n = std::snprintf(line, sizeof(line), "[%s] stage%d: %s\n",
                                timestamp().c_str(), stage,
                                message ? message : "(null)");
    if (n > 0) {
        std::fwrite(line, 1, static_cast<size_t>(n), g_bootTraceFile);
        std::fflush(g_bootTraceFile);
#if !defined(_WIN32)
        ::fsync(fileno(g_bootTraceFile));
#endif
    }
}

// Read the boot trace back into a string (for embedding in crash reports).
std::string readBootTrace() {
    if (g_bootTracePath.empty() || g_bootTraceFile == nullptr) {
        return "(boot trace not open)\n";
    }
    std::fflush(g_bootTraceFile);
    std::FILE *fp = std::fopen(g_bootTracePath.c_str(), "rb");
    if (!fp) {
        return "(cannot reopen boot trace: " + g_bootTracePath + ")\n";
    }
    std::string out;
    char buf[1024];
    while (true) {
        const size_t k = std::fread(buf, 1, sizeof(buf), fp);
        if (k == 0) {
            break;
        }
        out.append(buf, k);
    }
    std::fclose(fp);
    return out;
}

// Close + delete the boot trace file (only called on clean exit).
void closeBootTrace() {
    if (g_bootTraceFile != nullptr) {
        std::fclose(g_bootTraceFile);
        g_bootTraceFile = nullptr;
    }
    if (!g_bootTracePath.empty() && g_bootTraceCleanExit.load()) {
#if defined(_WIN32)
        DeleteFileA(g_bootTracePath.c_str());
#else
        ::unlink(g_bootTracePath.c_str());
#endif
    }
}

#if defined(_WIN32)
// Render a Windows exception code as the human-readable name. Source:
// winnt.h EXCEPTION_* and STATUS_* macros; narrowed to the subset we
// actually expect to see in a FusionCut Pro crash log.
const char *exceptionCodeName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DENORMAL_OPERAND:
        return "EXCEPTION_FLT_DENORMAL_OPERAND";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INEXACT_RESULT:
        return "EXCEPTION_FLT_INEXACT_RESULT";
    case EXCEPTION_FLT_INVALID_OPERATION:
        return "EXCEPTION_FLT_INVALID_OPERATION";
    case EXCEPTION_FLT_OVERFLOW:
        return "EXCEPTION_FLT_OVERFLOW";
    case EXCEPTION_FLT_STACK_CHECK:
        return "EXCEPTION_FLT_STACK_CHECK";
    case EXCEPTION_FLT_UNDERFLOW:
        return "EXCEPTION_FLT_UNDERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:
        return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW:
        return "EXCEPTION_INT_OVERFLOW";
    case EXCEPTION_PRIV_INSTRUCTION:
        return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
    case EXCEPTION_STACK_INVALID:
        return "EXCEPTION_STACK_INVALID";
    case EXCEPTION_STACK_OVERFLOW:
        return "EXCEPTION_STACK_OVERFLOW";
    case EXCEPTION_INVALID_DISPOSITION:
        return "EXCEPTION_INVALID_DISPOSITION";
    case EXCEPTION_INVALID_HANDLE:
        return "EXCEPTION_INVALID_HANDLE";
    case EXCEPTION_GUARD_PAGE:
        return "EXCEPTION_GUARD_PAGE";
    case STATUS_HEAP_CORRUPTION:
        return "STATUS_HEAP_CORRUPTION";
    case STATUS_STACK_BUFFER_OVERRUN:
        return "STATUS_STACK_BUFFER_OVERRUN";
    default: {
        // C++ EH exception code (0xE06D7363 'msc'/'EEms') and CLR/GC
        // codes land here as "Application/Unknown"; the hex value is
        // recorded in the log either way.
        if ((code & 0xF0000000) == 0xE0000000) {
            return "ApplicationException";
        }
        return "Unknown";
    }
    }
}

// Snapshot the loaded-module list via Toolhelp32. This must run in
// ordinary thread context (which VEH gives us), NOT in a POSIX signal
// handler. We deliberately avoid psapi's EnumProcessModules because
// that drags in a link dependency on psapi.lib on some toolchains;
// toolhelp32 is in kernel32 (always linked).
void appendModuleSnapshot(std::string &out) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        out += "  (CreateToolhelp32Snapshot failed)\n";
        return;
    }
    MODULEENTRY32 me{};
    me.dwSize = sizeof(me);
    char line[1024];
    if (Module32First(snap, &me)) {
        int idx = 0;
        do {
            if (me.szModule[0] == '\0' && me.szExePath[0] == '\0') {
                continue;
            }
            const int n = std::snprintf(
                line, sizeof(line), "  [%4d] base=0x%p size=%6lu mod=%s path=%s\n", idx++,
                static_cast<void *>(me.modBaseAddr), static_cast<unsigned long>(me.modBaseSize),
                me.szModule[0] ? me.szModule : "(unnamed)",
                me.szExePath[0] ? me.szExePath : "(unknown)");
            if (n > 0) {
                out.append(line, static_cast<size_t>(n));
            }
        } while (Module32Next(snap, &me));
    } else {
        out += "  (Module32First failed)\n";
    }
    CloseHandle(snap);
}
#endif // _WIN32

// Best-effort stack backtrace. Returns the frame count captured so
// callers can decide whether to fall through to symbolication.
unsigned appendBacktrace(std::string &out) {
#if defined(_WIN32)
    void *frames[64];
    const USHORT n = CaptureStackBackTrace(0, 64, frames, nullptr);
    char line[128];
    const int m = std::snprintf(
        line, sizeof(line),
        "Backtrace (CaptureStackBackTrace, %u frames; raw addresses for offline symbolication):\n",
        static_cast<unsigned>(n));
    if (m > 0) {
        out.append(line, static_cast<size_t>(m));
    }
    for (USHORT i = 0; i < n; ++i) {
        const int k = std::snprintf(line, sizeof(line), "  [%2u] 0x%p\n", static_cast<unsigned>(i),
                                    frames[i]);
        if (k > 0) {
            out.append(line, static_cast<size_t>(k));
        }
    }
    return n;
#else
    void *frames[64];
    const int n = backtrace(frames, 64);
    char line[128];
    const int m = std::snprintf(line, sizeof(line), "Backtrace (backtrace, %d frames):\n", n);
    if (m > 0) {
        out.append(line, static_cast<size_t>(m));
    }
    char **syms = backtrace_symbols(frames, n);
    if (syms) {
        for (int i = 0; i < n; ++i) {
            const int k =
                std::snprintf(line, sizeof(line), "  [%2d] %s\n", i, syms[i] ? syms[i] : "(null)");
            if (k > 0) {
                out.append(line, static_cast<size_t>(k));
            }
        }
        std::free(syms);
    }
    return static_cast<unsigned>(n > 0 ? n : 0);
#endif
}

// ---------- Windows VEH (covers all threads, including qwindows init) ----

#if defined(_WIN32)
LONG WINAPI vectoredExceptionHandler(PEXCEPTION_POINTERS ep) {
    // Filter: act only on truly fatal exceptions. VEH sees EVERY exception
    // raised in the process, including C++ throw (which the runtime will
    // match and unwind), DBG_PRINTEXCEPTION_C (OutputDebugString), and DLL
    // thread-attach notifications - logging those would spam the report
    // file. We restrict ourselves to the catastrophic set.
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    const bool cppEh = (code == 0xE06D7363); // C++ EH; let the runtime unwind it
    const bool dbgPrint = (code == DBG_PRINTEXCEPTION_C || code == DBG_PRINTEXCEPTION_WIDE_C);
    const bool isFatal =
        (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
         code == EXCEPTION_STACK_INVALID || code == EXCEPTION_STACK_OVERFLOW ||
         code == EXCEPTION_IN_PAGE_ERROR || code == EXCEPTION_ARRAY_BOUNDS_EXCEEDED ||
         code == EXCEPTION_DATATYPE_MISALIGNMENT || code == EXCEPTION_NONCONTINUABLE_EXCEPTION ||
         code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_FLT_DIVIDE_BY_ZERO ||
         code == EXCEPTION_FLT_INVALID_OPERATION || code == EXCEPTION_FLT_OVERFLOW ||
         code == EXCEPTION_FLT_UNDERFLOW || code == EXCEPTION_FLT_STACK_CHECK ||
         code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_INT_OVERFLOW ||
         code == STATUS_HEAP_CORRUPTION || code == STATUS_STACK_BUFFER_OVERRUN);
    if (cppEh || dbgPrint || !isFatal) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const bool firstCrash = g_crashing.exchange(true) == false;
    if (!firstCrash) {
        return EXCEPTION_CONTINUE_SEARCH; // recursive crash - bail out
    }

    // Build the report body. VEH runs in ordinary thread context, so
    // std::string heap allocation is safe here.
    std::string body;
    body.reserve(4096);
    char line[512];
    int n =
        std::snprintf(line, sizeof(line),
                      "FusionCut Pro crash report\n"
                      "Version: %s\n"
                      "Timestamp: %s\n"
                      "Kind: VectoredException\n"
                      "ExceptionCode: 0x%08lX (%s)\n"
                      "ExceptionAddress: 0x%p\n",
                      g_appVersion.c_str(), timestamp().c_str(), static_cast<unsigned long>(code),
                      exceptionCodeName(code), ep->ExceptionRecord->ExceptionAddress);
    if (n > 0) {
        body.append(line, static_cast<size_t>(n));
    }
    // ACCESS_VIOLATION / IN_PAGE_ERROR carry an array of sub-params: [0]
    // is the read/write/execute flag, [1] is the faulting VA.
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) {
        const ULONG_PTR *info = ep->ExceptionRecord->ExceptionInformation;
        const char *op = info[0] == 0   ? "Read"
                         : info[0] == 1 ? "Write"
                         : info[0] == 8 ? "DEP/Execute"
                                        : "Unknown";
        const int k =
            std::snprintf(line, sizeof(line), "AccessType: %s (%lu)\nFaultingAddress: 0x%p\n", op,
                          static_cast<unsigned long>(info[0]), reinterpret_cast<void *>(info[1]));
        if (k > 0) {
            body.append(line, static_cast<size_t>(k));
        }
    }
    body += "----- Stack -----\n";
    appendBacktrace(body);
    body += "----- Loaded Modules -----\n";
    appendModuleSnapshot(body);
    body += "----- Boot Trace -----\n";
    body += readBootTrace();

    // Path resolution + write. Try in order: configured report dir,
    // exe directory, cwd, %TEMP% (Windows). This is more robust than
    // the previous "exe dir, else cwd" chain - if the exe lives in a
    // read-only Program Files install we still get the log into %TEMP%.
    std::string writtenPath;
    const std::string leaf = "FusionCutPro-crash-" + timestamp() + ".log";
    const std::string dir = resolveReportDir();
    std::string path = joinPath(dir, leaf);
    FILE *fp = std::fopen(path.c_str(), "wb");
    if (!fp) {
        // Fallback 1: cwd-relative.
        std::fprintf(stderr,
                     "FusionCut Pro: cannot open crash log at %s; trying cwd\n",
                     path.c_str());
        path = leaf;
        fp = std::fopen(path.c_str(), "wb");
#if defined(_WIN32)
        if (!fp) {
            // Fallback 2: %TEMP%.
            char temp[MAX_PATH];
            const DWORD n = GetTempPathA(MAX_PATH, temp);
            if (n > 0 && n < MAX_PATH) {
                std::fprintf(stderr,
                             "FusionCut Pro: cannot open crash log at cwd; trying %%TEMP%%\n");
                path = joinPath(std::string(temp, n), leaf);
                fp = std::fopen(path.c_str(), "wb");
            }
        }
#endif
    }
    if (fp) {
        std::fwrite(body.c_str(), 1, body.size(), fp);
        std::fclose(fp);
        writtenPath = path;
        std::fprintf(stderr, "FusionCut Pro: crash log written to %s\n", path.c_str());
    } else {
        std::fprintf(stderr,
                     "FusionCut Pro: FAILED to write crash log at any candidate path.\n");
        writtenPath = "(write failed; see stderr)";
    }

    // User-facing dialog: only if a GUI desktop is available. Use
    // MB_SYSTEMMODAL so the dialog stays on top of any hung render.
    char msg[1024];
    std::snprintf(msg, sizeof(msg),
                  "FusionCut Pro has stopped unexpectedly.\r\n\r\n"
                  "Exception: %s (code 0x%08lX)\r\n"
                  "Address: 0x%p\r\n\r\n"
                  "A detailed crash log has been written to:\r\n%s\r\n\r\n"
                  "Please send that log file to the developer so the cause can be fixed.\r\n"
                  "The application will now close.",
                  exceptionCodeName(code), static_cast<unsigned long>(code),
                  ep->ExceptionRecord->ExceptionAddress, writtenPath.c_str());
    MessageBoxA(nullptr, msg, "FusionCut Pro - Crash Report",
                MB_OK | MB_ICONERROR | MB_SYSTEMMODAL | MB_SETFOREGROUND);

    // Re-raise the original exception so the OS default disposition
    // (terminate + WER) still runs. We are a *reporter*, not an
    // exception swallower.
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif // _WIN32

// ---------- POSIX signal handler (async-signal-safe subset) --------------

#if !defined(_WIN32)
int g_logFd = -1; // pre-opened in installCrashHandler for the signal path

void posixSignalHandler(int sig) {
    // Async-signal-safe only. No malloc, no std::ofstream, no stdio.
    const bool firstCrash = g_crashing.exchange(true) == false;
    if (!firstCrash) {
        _exit(128 + sig);
    }
    const char *name = "UnknownSignal";
    switch (sig) {
    case SIGSEGV:
        name = "SIGSEGV";
        break;
    case SIGABRT:
        name = "SIGABRT";
        break;
    case SIGFPE:
        name = "SIGFPE";
        break;
    case SIGILL:
        name = "SIGILL";
        break;
    case SIGTERM:
        name = "SIGTERM";
        break;
    default:
        break;
    }
    char line[256];
    int n = std::snprintf(line, sizeof(line),
                          "FusionCut Pro crash report\n"
                          "Version: %s\n"
                          "Kind: Signal\n"
                          "Signal: %s (%d)\n"
                          "----- Stack -----\n",
                          g_appVersion.c_str(), name, sig);
    if (n > 0) {
        if (write(g_logFd, line, static_cast<size_t>(n)) < 0) {
            // Swallow; nothing we can do.
        }
    }
    // backtrace_symbols_fd is async-signal-safe (allocates internally
    // via mmap; acceptable on Linux).
    void *frames[32];
    const int k = backtrace(frames, 32);
    backtrace_symbols_fd(frames, k, g_logFd);
    fsync(g_logFd);
    // Restore default disposition and re-raise so the parent shell
    // sees the real signal.
    std::signal(sig, SIG_DFL);
    std::raise(sig);
    _exit(128 + sig);
}
#else
void posixSignalHandler(int sig) {
    // Windows still installs SIGSEGV/SIGABRT handlers as a *secondary*
    // net for the rare case VEH does not catch (e.g. abort() called
    // from libc when /WARNINGS_AS_ERRORS in CRT). On Windows we can
    // safely call back into the VEH report path because we are not in
    // kernel signal context - the CRT has routed through here.
    const bool firstCrash = g_crashing.exchange(true) == false;
    if (!firstCrash) {
        // std::_Exit is the C++11 standard (in <cstdlib>, NOT
        // async-signal-safe but that's irrelevant on Windows: the CRT
        // has unwound here, not the kernel). std::_exit (lowercase,
        // POSIX) does NOT exist on MinGW's libstdc++ - only glibc's
        // libstdc++ surfaces it as an extension in std::; do NOT use.
        std::_Exit(128 + sig);
    }
    const char *name = sig == SIGSEGV   ? "SIGSEGV"
                       : sig == SIGABRT ? "SIGABRT"
                       : sig == SIGFPE  ? "SIGFPE"
                       : sig == SIGILL  ? "SIGILL"
                                        : "Signal";
    char msg[256];
    std::snprintf(msg, sizeof(msg), "FusionCut Pro crash (CRT signal path)\nSignal: %s (%d)\n",
                  name, sig);
    std::fputs(msg, stderr);
    std::fflush(stderr);
    std::_Exit(128 + sig);
}
#endif

// ---------- std::set_terminate / set_new_handler --------------------------

void terminateHandler() {
    const bool firstCrash = g_crashing.exchange(true) == false;
    if (!firstCrash) {
        std::_Exit(1);
    }
    const char *what = "no current exception";
    std::exception_ptr p = std::current_exception();
    try {
        if (p) {
            std::rethrow_exception(p);
        }
    } catch (const std::exception &e) {
        what = e.what();
    } catch (...) {
        what = "non-std C++ exception";
    }

    std::string body;
    body.reserve(1024);
    char line[512];
    const int n = std::snprintf(line, sizeof(line),
                                "FusionCut Pro crash report\n"
                                "Version: %s\n"
                                "Timestamp: %s\n"
                                "Kind: std::terminate (uncaught C++ exception)\n"
                                "Exception.what(): %s\n",
                                g_appVersion.c_str(), timestamp().c_str(), what ? what : "(null)");
    if (n > 0) {
        body.append(line, static_cast<size_t>(n));
    }
    body += "----- Stack -----\n";
    appendBacktrace(body);
#if defined(_WIN32)
    body += "----- Loaded Modules -----\n";
    appendModuleSnapshot(body);
#endif
    body += "----- Boot Trace -----\n";
    body += readBootTrace();

    const std::string dir = resolveReportDir();
    const std::string path = joinPath(dir, "FusionCutPro-crash-" + timestamp() + ".log");
    if (FILE *fp = std::fopen(path.c_str(), "wb")) {
        std::fwrite(body.c_str(), 1, body.size(), fp);
        std::fclose(fp);
        std::fprintf(stderr, "FusionCut Pro: terminate crash log -> %s\n", path.c_str());
#if defined(_WIN32)
        char msg[1024];
        std::snprintf(msg, sizeof(msg),
                      "FusionCut Pro has stopped due to an unhandled exception.\r\n\r\n"
                      "what(): %s\r\n\r\n"
                      "Crash log: %s\r\n\r\n"
                      "Please send that log file to the developer.",
                      what ? what : "(null)", path.c_str());
        MessageBoxA(nullptr, msg, "FusionCut Pro - Crash Report",
                    MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
#endif
    } else {
        std::fprintf(stderr, "FusionCut Pro: failed to write terminate crash log at %s\n",
                     path.c_str());
    }
    std::_Exit(1);
}

void newHandler() {
    // std::set_new_handler: invoked when operator new cannot satisfy a
    // request. By then the process may be unrecoverable; we log + exit.
    const bool firstCrash = g_crashing.exchange(true) == false;
    if (!firstCrash) {
        std::_Exit(2);
    }
    std::fprintf(stderr,
                 "FusionCut Pro: out of memory (std::bad_alloc). Writing minimal report.\n");
    const std::string dir = resolveReportDir();
    const std::string path = joinPath(dir, "FusionCutPro-crash-" + timestamp() + ".log");
    if (FILE *fp = std::fopen(path.c_str(), "wb")) {
        std::fprintf(fp,
                     "FusionCut Pro crash report\n"
                     "Version: %s\n"
                     "Timestamp: %s\n"
                     "Kind: std::bad_alloc (out of memory)\n",
                     g_appVersion.c_str(), timestamp().c_str());
        std::fclose(fp);
#if defined(_WIN32)
        char msg[512];
        std::snprintf(msg, sizeof(msg), "FusionCut Pro has run out of memory.\r\n\r\nCrash log: %s",
                      path.c_str());
        MessageBoxA(nullptr, msg, "FusionCut Pro - Out of Memory",
                    MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
#endif
    }
    std::_Exit(2);
}

#if defined(_WIN32)
// CRT "invalid parameter" hook: fires on misuses like passing nullptr to
// strlen, out-of-range fd to read, etc. Without this the CRT terminates
// the process silently with no diagnostic.
void __cdecl invalidParameterHandler(const wchar_t * /*expression*/, const wchar_t * /*function*/,
                                     const wchar_t * /*file*/, unsigned int /*line*/,
                                     uintptr_t /*kind*/) {
    const bool firstCrash = g_crashing.exchange(true) == false;
    if (!firstCrash) {
        std::_Exit(3);
    }
    const std::string dir = resolveReportDir();
    const std::string path = joinPath(dir, "FusionCutPro-crash-" + timestamp() + ".log");
    if (FILE *fp = std::fopen(path.c_str(), "wb")) {
        std::fprintf(fp,
                     "FusionCut Pro crash report\n"
                     "Version: %s\n"
                     "Timestamp: %s\n"
                     "Kind: CRT invalid parameter (libc misuse detected by runtime)\n",
                     g_appVersion.c_str(), timestamp().c_str());
        std::fclose(fp);
    }
    std::fprintf(stderr, "FusionCut Pro: CRT invalid parameter. Log: %s\n", path.c_str());
    char msg[512];
    std::snprintf(msg, sizeof(msg),
                  "FusionCut Pro detected an internal libc misuse (invalid parameter).\r\n\r\n"
                  "Crash log: %s",
                  path.c_str());
    MessageBoxA(nullptr, msg, "FusionCut Pro - Crash Report",
                MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
    std::_Exit(3);
}

void __cdecl pureCallHandler() {
    const bool firstCrash = g_crashing.exchange(true) == false;
    if (!firstCrash) {
        std::_Exit(4);
    }
    const std::string dir = resolveReportDir();
    const std::string path = joinPath(dir, "FusionCutPro-crash-" + timestamp() + ".log");
    if (FILE *fp = std::fopen(path.c_str(), "wb")) {
        std::fprintf(fp,
                     "FusionCut Pro crash report\n"
                     "Version: %s\n"
                     "Timestamp: %s\n"
                     "Kind: Pure virtual function call\n",
                     g_appVersion.c_str(), timestamp().c_str());
        std::fclose(fp);
    }
    std::fprintf(stderr, "FusionCut Pro: pure virtual function call. Log: %s\n", path.c_str());
    char msg[512];
    std::snprintf(msg, sizeof(msg),
                  "FusionCut Pro: a pure virtual function was called.\r\n\r\nCrash log: %s",
                  path.c_str());
    MessageBoxA(nullptr, msg, "FusionCut Pro - Crash Report",
                MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
    std::_Exit(4);
}
#endif // _WIN32

} // namespace

namespace fc {

void installCrashHandler(const std::string &appVersion, const std::string &reportDir) {
    // ALWAYS update version + report dir, even if handlers are already
    // installed by the static initializer. The static initializer calls
    // this with empty strings (so version stays "unknown" and dir stays
    // empty = exe directory), then main() calls this again with the real
    // FC_VERSION_STRING. The version string in every crash report must
    // reflect the real version, not the static-init placeholder.
    g_appVersion = appVersion.empty() ? std::string("unknown") : appVersion;
    g_reportDir = reportDir;

    const bool firstInstall = !g_installed.exchange(true);
    if (firstInstall) {
        // Open the boot trace FIRST. Every subsequent line in this
        // function and in main() flows through writeBootStage, which is
        // a no-op if g_bootTraceFile is null. So if the boot trace open
        // fails here, every later milestone is silently lost. We try
        // to reopen later from main() (via resolveReportDir()) too.
        openBootTrace();

#if defined(_WIN32)
        // VEH first: catches all SEH exceptions in all threads, including
        // the qwindows.dll platform-plugin init path. FirstHandler=1
        // means we run *before* any other VEH registered later by Qt or
        // the MinGW runtime. This is the single most important call in
        // this file for the 0xc0000005 startup class.
        AddVectoredExceptionHandler(1, vectoredExceptionHandler);
        _set_invalid_parameter_handler(invalidParameterHandler);
        _set_purecall_handler(pureCallHandler);
#else
        // POSIX: pre-open a log fd so the signal handler can write
        // without touching libc malloc. Opened once here; reused per
        // crash.
        const std::string dir = resolveReportDir();
        const std::string path =
            joinPath(dir, "FusionCutPro-crash-" + timestamp() + ".log");
        g_logFd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        // If the open failed, signal handlers fall back to stderr via
        // backtrace_symbols_fd (which writes to STDOUT/STDERR_FILENO).
#endif

        std::signal(SIGSEGV, posixSignalHandler);
        std::signal(SIGABRT, posixSignalHandler);
        std::signal(SIGFPE, posixSignalHandler);
        std::signal(SIGILL, posixSignalHandler);
        std::signal(SIGTERM, posixSignalHandler);

        std::set_terminate(terminateHandler);
        std::set_new_handler(newHandler);

        writeBootStage(0, "VEH + handlers installed (pre-main static initializer)");
    } else {
        // Already installed (by the static initializer). Try to (re)open
        // the boot trace in case the static-init attempt failed (e.g.
        // because exeDirectory() returned "." before argv[0] was set).
        if (g_bootTraceFile == nullptr) {
            openBootTrace();
        }
        writeBootStage(1, "installCrashHandler called from main (version + dir set)");
    }
}

void recordBootStage(int stage, const char *message) {
    writeBootStage(stage, message);
}

void shutdownCrashHandler() {
    // Mark clean exit so closeBootTrace() deletes the boot trace file
    // (a clean exit means the boot trace is no longer interesting - the
    // crash report is the only artifact the developer needs).
    g_bootTraceCleanExit.store(true);
    closeBootTrace();
}

std::string bootTraceContent() {
    return readBootTrace();
}

std::string writeManualCrashReport(const std::string &appVersion, const std::string &reportDir) {
    const std::string v = appVersion.empty() ? std::string("unknown") : appVersion;
    const std::string dir = reportDir.empty()
#if defined(_WIN32)
                                ? exeDirectory()
#else
                                ? std::string(".")
#endif
                                : reportDir;
    const std::string path = joinPath(dir, "FusionCutPro-crash-" + timestamp() + ".log");
    std::string body;
    body.reserve(512);
    char line[256];
    const int n = std::snprintf(line, sizeof(line),
                                "FusionCut Pro crash report\n"
                                "Version: %s\n"
                                "Timestamp: %s\n"
                                "Kind: Manual (smoke test)\n"
                                "Note: this is a synthetic report from --crash-test.\n",
                                v.c_str(), timestamp().c_str());
    if (n > 0) {
        body.append(line, static_cast<size_t>(n));
    }
    body += "----- Stack -----\n";
    appendBacktrace(body);
#if defined(_WIN32)
    body += "----- Loaded Modules -----\n";
    appendModuleSnapshot(body);
#endif
    body += "----- Boot Trace -----\n";
    body += readBootTrace();
    if (FILE *fp = std::fopen(path.c_str(), "wb")) {
        std::fwrite(body.c_str(), 1, body.size(), fp);
        std::fclose(fp);
        return path;
    }
    return {};
}

} // namespace fc

// ---------- pre-main static initializer ---------------------------------
// This file-scope static-storage object's constructor runs at .CRT$XCU
// (Windows) / .ctors (POSIX) static-init time, BEFORE main() is reached.
// That means the VEH is registered before QApplication pulls in Qt5Core
// / Qt5Gui / Qt5Widgets / qwindows.dll - covering the entire class of
// 0xc0000005 startup crashes that the v0.4.1 in-main() install could
// not catch. The boot trace is also opened here so a partial trace
// survives even a crash inside a static-import DLL's DllMain (a case
// VEH cannot catch because VEH itself is not yet wired up at that
// loader phase). This is intentionally simple - no platform-specific
// pragma section placement - which means GCC's ctor ordering puts us
// after the CRT and the standard library's own static ctors but before
// any TU that has a static object with a constructor depending on the
// handlers being in place.
namespace {
class EarlyCrashHandlerInstaller {
public:
    EarlyCrashHandlerInstaller() {
        // Empty version + empty dir -> "unknown" version, exe dir as
        // report dir. main() will call installCrashHandler again with
        // the real FC_VERSION_STRING; the idempotent guard keeps the
        // handlers installed, and the version/dir fields get updated.
        fc::installCrashHandler("", "");
    }
};
EarlyCrashHandlerInstaller g_earlyCrashHandlerInstaller;
} // namespace
