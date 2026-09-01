// FusionCut Pro - zero-dependency Windows loader diagnostic.
//
// WHY THIS EXISTS
//   The Windows loader dialog "The application was unable to start
//   correctly (0xc0000005)" is shown by ntdll!LdrpInitializeProcess
//   when it returns STATUS_ACCESS_VIOLATION during process setup -
//   i.e. BEFORE any user code in the target .exe runs. The Windows
//   loader sequence is:
//
//     map exe image
//       -> walk Import Directory, load each dependent DLL (recursive)
//         -> run each DLL's DllMain(DLL_PROCESS_ATTACH)
//           -> run .CRT$XIA  (CRT init)
//             -> run .CRT$XCU  (user static initializers)
//               -> call main() / WinMain
//
//   A loader-phase 0xc0000005 aborts BEFORE .CRT$XCU. That means:
//     * the in-process VEH installed by the crash handler's static
//       initializer is NEVER installed (that initializer never runs)
//     * the boot-trace log file is NEVER opened
//     * no static initializer runs, no SetUnhandledExceptionFilter,
//       no AddVectoredExceptionHandler - nothing in the process
//       observes the failure
//
//   So the dialog pops with zero diagnostic, exactly as the user sees.
//   The only way to name the missing/wrong DLL is to inspect the
//   target .exe's import tree from OUTSIDE the process - which is
//   exactly what this tool does.
//
// WHAT IT CATCHES
//   - Missing DLL (most common): LoadLibraryEx returns NULL with
//     GetLastError = ERROR_MOD_NOT_FOUND (126).
//   - Wrong-architecture DLL (e.g. 32-bit Qt5Core.dll in a 64-bit
//     package): GetLastError = ERROR_BAD_EXE_FORMAT (193).
//   - Corrupt PE: various errors.
//   Each FAIL line names the DLL + the OS error string, so the cause
//   is unambiguous.
//
// WHAT IT DOES NOT CATCH
//   A DLL that MAPS fine but whose DllMain crashes when the real
//   FusionCutPro.exe loads it. This tool uses
//   DONT_RESOLVE_DLL_REFERENCES, which maps the image WITHOUT running
//   DllMain - so a DllMain crash cannot bring the tool down (and
//   cannot be observed). For that class, use Event Viewer:
//     eventvwr.msc -> Windows Logs -> Application -> filter
//     FusionCutPro.exe -> the "Faulting module name:" line of the
//     most recent Application Error event names the crashing DLL.
//
// HOW IT RUNS
//   - WIN32 subsystem app (no console flash on double-click).
//   - Statically linked (-static): zero DLL dependencies of its own,
//     so it runs even when the portable folder is missing a DLL that
//     breaks FusionCutPro.exe.
//   - Uses ONLY kernel32 + user32 functions (both always present on
//     every Windows since NT 3.1).
//   - Writes loader-check-<timestamp>.log next to itself (falls back
//     to %TEMP% if the portable folder is read-only), and pops a
//     MessageBox summarizing the result so a double-click launch
//     sees something actionable.
//   - Recursively walks the transitive import tree (depth-first, with
//     a visited set to break cycles, hard cap at depth 24).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>

namespace {

constexpr int kMaxVisited = 512;
constexpr int kMaxDepth = 24;

struct Visited {
    char name[256];
};

Visited g_visited[kMaxVisited];
int g_visitedCount = 0;
int g_failCount = 0;
HANDLE g_logFile = INVALID_HANDLE_VALUE;
// 1024 (not MAX_PATH=260) because the log path = parent dir + ~35-char
// timestamped filename; a 260-char GetTempPathA result + suffix
// exceeds MAX_PATH. 1024 is well above any realistic Windows path.
char g_logPath[1024] = {0};

bool alreadyVisited(const char *name) {
    for (int i = 0; i < g_visitedCount; ++i) {
        // lstrcmpiA is the Win32 case-insensitive compare (kernel32,
        // always present) - avoids the _stricmp portability gap (MSVC
        // extension; glibc has strcasecmp instead, MinGW-w64 has
        // _stricmp in <string.h> but sticking to the kernel32 API
        // keeps the tool's "only kernel32 + user32" constraint clean).
        if (lstrcmpiA(g_visited[i].name, name) == 0)
            return true;
    }
    return false;
}

void markVisited(const char *name) {
    if (g_visitedCount < kMaxVisited) {
        strncpy(g_visited[g_visitedCount].name, name, 255);
        g_visited[g_visitedCount].name[255] = 0;
        ++g_visitedCount;
    }
}

void logLine(const char *line) {
    // Log file only - WIN32 subsystem app has no stdout. The log is
    // the persistent artifact the user opens after the MessageBox
    // dismisses.
    if (g_logFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_logFile, line, (DWORD)strlen(line), &written, NULL);
        WriteFile(g_logFile, "\r\n", 2, &written, NULL);
    }
}

void logFail(const char *dllName, DWORD err, const char *via) {
    char buf[512] = {0};
    // FormatMessageA with FROM_SYSTEM turns the numeric error into the
    // human OS string ("The specified module could not be found.",
    // "%1 is not a valid Win32 application.", etc.) - each pinpoints a
    // distinct root cause.
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err, 0, buf,
                   sizeof(buf), NULL);
    // Trim trailing whitespace/newlines from the OS string.
    for (char *p = buf + strlen(buf) - 1; p >= buf && (*p == '\n' || *p == '\r' || *p == ' ');
         --p) {
        *p = 0;
    }
    char line[1100];
    snprintf(line, sizeof(line), "  FAIL  %-32s  err=%lu  %s  (via %s)", dllName,
             (unsigned long)err, buf, via);
    logLine(line);
    ++g_failCount;
}

void logOk(const char *dllName, const char *via) {
    char line[600];
    snprintf(line, sizeof(line), "  OK    %-32s  (via %s)", dllName, via);
    logLine(line);
}

// Open the log next to this exe; if that fails (read-only portable
// folder, e.g. extracted to Program Files), fall back to %TEMP%.
// GetTempPathA always succeeds on Win2K+ and returns a path that
// ends in a backslash, so no separator is appended in the fallback.
HANDLE openLog(const char *selfDir) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(g_logPath, sizeof(g_logPath), "%s\\loader-check-%04d%02d%02d-%02d%02d%02d.log",
             selfDir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    HANDLE h = CreateFileA(g_logPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE)
        return h;
    char tmp[MAX_PATH];
    if (GetTempPathA(MAX_PATH, tmp) == 0)
        return INVALID_HANDLE_VALUE;
    snprintf(g_logPath, sizeof(g_logPath), "%sloader-check-%04d%02d%02d-%02d%02d%02d.log", tmp,
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return CreateFileA(g_logPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
}

// Recursively walk the import tree of a mapped image. Each imported
// DLL name is LoadLibraryEx'd with DONT_RESOLVE_DLL_REFERENCES, which
// maps the image WITHOUT running DllMain or resolving its own imports
// - so a bad DllMain in a dependency cannot crash this tool. If the
// map fails, the DLL is missing/wrong-arch/corrupt and is logged.
void walkImports(HMODULE h, const char *contextName, int depth) {
    if (depth > kMaxDepth)
        return;

    auto dos = reinterpret_cast<IMAGE_DOS_HEADER *>(h);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS *>(reinterpret_cast<BYTE *>(h) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return;

    auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.Size == 0)
        return; // pure-resource DLL, no imports - fine

    auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(reinterpret_cast<BYTE *>(h) +
                                                           dir.VirtualAddress);
    for (; imp->Name != 0; ++imp) {
        const char *name = reinterpret_cast<const char *>(reinterpret_cast<BYTE *>(h) + imp->Name);
        if (alreadyVisited(name))
            continue;
        markVisited(name);

        // DONT_RESOLVE_DLL_REFERENCES: map image, no DllMain, no
        // transitive load. Catches missing/wrong-arch/corrupt; misses
        // DllMain crashes (documented at top of file).
        HMODULE dep = LoadLibraryExA(name, NULL, DONT_RESOLVE_DLL_REFERENCES);
        if (!dep) {
            logFail(name, GetLastError(), contextName);
            continue;
        }
        logOk(name, contextName);
        walkImports(dep, name, depth + 1);
        // Intentionally do NOT FreeLibrary(dep): keep it mapped so
        // the visited set prevents re-walking if a later import pulls
        // the same DLL (cycle break). OS cleans up at process exit.
    }
}

} // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR lpCmdLine, int) {
    // Derive the portable folder from this exe's own path - both
    // FusionCutPro.exe and fcp-loader-check.exe live there.
    char selfPath[MAX_PATH];
    GetModuleFileNameA(NULL, selfPath, MAX_PATH);
    char selfDir[MAX_PATH];
    strncpy(selfDir, selfPath, MAX_PATH);
    selfDir[MAX_PATH - 1] = 0;
    char *last = strrchr(selfDir, '\\');
    if (last) {
        *last = 0;
    } else {
        // No backslash - fall back to cwd.
        GetCurrentDirectoryA(MAX_PATH, selfDir);
    }

    // Default target is FusionCutPro.exe in the same folder; allow
    // override via command line for power users (e.g. to check a
    // different build). lpCmdLine is a raw C string, no argv splitting.
    char targetName[MAX_PATH] = "FusionCutPro.exe";
    if (lpCmdLine && lpCmdLine[0]) {
        // Strip a single pair of surrounding quotes if present.
        const char *s = lpCmdLine;
        while (*s == ' ' || *s == '\t')
            ++s;
        if (*s == '"') {
            ++s;
            const char *e = strchr(s, '"');
            size_t n = e ? (size_t)(e - s) : strlen(s);
            if (n >= MAX_PATH)
                n = MAX_PATH - 1;
            memcpy(targetName, s, n);
            targetName[n] = 0;
        } else {
            strncpy(targetName, s, MAX_PATH - 1);
            targetName[MAX_PATH - 1] = 0;
            // Trim trailing whitespace.
            size_t n = strlen(targetName);
            while (n > 0 && (targetName[n - 1] == ' ' || targetName[n - 1] == '\t')) {
                targetName[--n] = 0;
            }
        }
    }

    char targetPath[1024];
    snprintf(targetPath, sizeof(targetPath), "%s\\%s", selfDir, targetName);

    g_logFile = openLog(selfDir);

    logLine("==============================================================");
    logLine("FusionCut Pro loader-check");
    {
        char hdr[1100];
        snprintf(hdr, sizeof(hdr), "Target: %s", targetPath);
        logLine(hdr);
    }
    logLine("==============================================================");
    logLine("");
    logLine("Walking the PE import tree. Each FAIL line below names a DLL");
    logLine("the Windows loader could not map. The first FAIL is the most");
    logLine("likely root cause of the '0xc0000005 unable to start' dialog.");
    logLine("");
    logLine("If all lines are OK below, the 0xc0000005 is NOT a");
    logLine("missing/wrong DLL - it is a DllMain crash. In that case use");
    logLine("Event Viewer (eventvwr.msc) -> Windows Logs -> Application ->");
    logLine("filter for FusionCutPro.exe; the 'Faulting module name:' line");
    logLine("of the most recent Application Error event names the DLL.");
    logLine("");

    // Set cwd to the portable folder so LoadLibraryExA's default
    // search order (application dir first) matches the real
    // FusionCutPro.exe loader behavior.
    SetCurrentDirectoryA(selfDir);

    HMODULE h = LoadLibraryExA(targetPath, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!h) {
        // The target itself can't be mapped - e.g. the file is gone,
        // or it's a 32-bit exe on 64-bit Windows, or corrupt.
        logFail(targetPath, GetLastError(), "(self)");
    } else {
        markVisited(targetName);
        walkImports(h, targetName, 0);
    }

    if (g_logFile != INVALID_HANDLE_VALUE) {
        CloseHandle(g_logFile);
        g_logFile = INVALID_HANDLE_VALUE;
    }

    char summary[2048];
    if (g_failCount == 0) {
        snprintf(summary, sizeof(summary),
                 "All imports resolved (0 failures).\r\n\r\n"
                 "The 0xc0000005 is NOT a missing/wrong DLL.\r\n"
                 "Likely cause: a DLL's DllMain crashes at startup.\r\n\r\n"
                 "Open Event Viewer (eventvwr.msc) ->\r\n"
                 "  Windows Logs -> Application ->\r\n"
                 "  filter for FusionCutPro.exe.\r\n"
                 "The 'Faulting module name:' line names the DLL.\r\n\r\n"
                 "Full log:\r\n%s",
                 g_logPath);
    } else {
        snprintf(summary, sizeof(summary),
                 "%d DLL(s) failed to load!\r\n\r\n"
                 "Open the log file to see the failing DLL name(s).\r\n"
                 "The first FAIL line is the most likely root cause of\r\n"
                 "the '0xc0000005 unable to start' dialog.\r\n\r\n"
                 "Log:\r\n%s",
                 g_failCount, g_logPath);
    }
    MessageBoxA(NULL, summary, "FusionCut Pro Loader Check",
                MB_OK | (g_failCount ? MB_ICONERROR : MB_ICONINFORMATION));
    return g_failCount > 0 ? 1 : 0;
}
