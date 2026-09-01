// FusionCut Pro - zero-dependency Windows loader + startup diagnostic.
//
// v2 (v0.4.5). Two phases, one run:
//
//   PHASE 1 - static import-tree probe (target NOT executed):
//     Maps FusionCutPro.exe + every DLL in its transitive import tree
//     with LoadLibraryEx(DONT_RESOLVE_DLL_REFERENCES) and walks the PE
//     import directories manually. Catches missing DLLs
//     (ERROR_MOD_NOT_FOUND) and wrong-architecture DLLs
//     (ERROR_BAD_EXE_FORMAT) by name, without running any DllMain.
//
//     v2 FIX: each probe mapping is FreeLibrary'd immediately after its
//     subtree walk. v0.4.4 left ~250 DONT_RESOLVE-mapped images in the
//     process at exit; the loader teardown then jumped through an
//     unresolved import-table entry (raw RVA like 0x9fade -> DEP
//     execute violation -> WER BEX64 event) - the tool itself crashed
//     right AFTER finishing its job. Refcount-symmetric probing keeps
//     the module list clean at process death.
//
//   PHASE 2 - debug-launch watch (the decisive diagnostic):
//     Spawns FusionCutPro.exe SUSPENDED with DEBUG_ONLY_THIS_PROCESS
//     (this tool becomes its debugger), resumes it, and pumps debug
//     events. Debug events are delivered to the debugger BEFORE
//     Windows Error Reporting - including exceptions raised inside
//     DllMain / TLS callbacks / static initializers during loader
//     initialization, which is exactly the crash class where the bare
//     "unable to start correctly (0xc0000005)" dialog appears with NO
//     Event-1000 entry and NO in-process log.
//
//     The watch records:
//       * every LOAD_DLL event (module name, base, size) - the load
//         trail shows exactly how far initialization got;
//       * every exception event (code, address, first/second chance,
//         AV read/write/execute type + target address, thread RIP);
//       * a second-chance (unhandled) exception is caught, blamed on
//         the module containing the faulting address, and the target
//         is terminated before WER can show any dialog;
//       * if the process dies with an NTSTATUS but no exception was
//         dispatched to WER (the pure loader-abort path - the exact
//         "unable to start correctly (0xc0000005)" mode), any
//         first-chance exception recorded just before death names the
//         fault site; failing that, the last DLLs on the load trail
//         are reported as prime suspects;
//       * if the process initializes and runs 25 s without a fatal
//         exception, it is reported healthy - the double-click crash
//         then does not reproduce under a debugger, which points at
//         environment injection (antivirus / shell hook DLLs) rather
//         than the app itself.
//
//   Everything is kernel32 + user32 only; the binary is -static
//   linked (zero MinGW-runtime DLL dependencies of its own), so it
//   runs even when the portable folder is missing a DLL that breaks
//   FusionCutPro.exe. Uses the classic Windows debug API
//   (CreateProcess + WaitForDebugEvent + ContinueDebugEvent) - no
//   dbghelp, no psapi, no debugger install required.

#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr int kMaxVisited = 512;
constexpr int kMaxDepth = 24;
constexpr int kMaxMods = 400;
constexpr int kMaxExcs = 128;

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

// ----- phase 2 records -------------------------------------------------

struct ModInfo {
    char name[1024]; // full path (\\?\ prefix stripped)
    unsigned long long base;
    unsigned long long size; // SizeOfImage via remote PE header read
};

ModInfo g_mods[kMaxMods];
int g_modCount = 0;

struct ExcInfo {
    DWORD code;
    unsigned long long addr; // ExceptionRecord.ExceptionAddress
    DWORD firstChance;
    unsigned long long avType; // 0 read / 1 write / 8 execute (AV only)
    unsigned long long avTarget;
    unsigned long long rip; // thread RIP at the event (0 = unavailable)
    char blame[1024];       // module containing the fault (or note)
    unsigned long long blameOff;
    // v2.1: module containing the AV TARGET address - for loader-phase
    // faults this names the DLL whose (invalid) data the loader was
    // reading, which is the actual culprit (the instruction blame lands
    // in ntdll, which is only the messenger). Empty if the target lies
    // outside every recorded module.
    char avTargetBlame[1024];
    unsigned long long avTargetOff;
};

ExcInfo g_excs[kMaxExcs];
int g_excCount = 0;

HANDLE g_dbgProcess = NULL;

// ----- shared log ------------------------------------------------------

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

// ----- phase 1: import-tree probe --------------------------------------

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
        // DllMain crashes (phase 2 of this tool covers those).
        HMODULE dep = LoadLibraryExA(name, NULL, DONT_RESOLVE_DLL_REFERENCES);
        if (!dep) {
            logFail(name, GetLastError(), contextName);
            continue;
        }
        logOk(name, contextName);
        walkImports(dep, name, depth + 1);
        // v2: NEVER leave a DONT_RESOLVE-mapped image in the loader's
        // module list. v0.4.4 kept them all mapped (as a re-walk
        // optimization) and the process then crashed at exit: the
        // loader teardown jumped through an unresolved import-table
        // entry (raw RVA -> DEP execute violation, WER BEX64 event).
        // The visited set above already prevents re-walking; freeing
        // here keeps the exit path clean. For modules that were
        // already loaded normally (kernel32 etc.) this is a plain
        // refcount decrement - symmetric and safe.
        FreeLibrary(dep);
    }
}

// ----- phase 2: debug-launch watch --------------------------------------

// The \\?\ extended-length prefix Win32 path normalization adds; strip
// it so the log shows a normal user-facing path.
void stripExtendedPrefix(char *path) {
    if (strncmp(path, "\\\\?\\", 4) == 0) {
        memmove(path, path + 4, strlen(path + 4) + 1);
    }
}

// Record one module of the debuggee: name via the debug event's file
// handle, size via reading the remote PE header (SizeOfImage).
void recordModule(unsigned long long base, HANDLE file) {
    if (g_modCount >= kMaxMods)
        return;
    ModInfo &m = g_mods[g_modCount];
    m.base = base;
    m.size = 0;
    m.name[0] = 0;
    if (file && file != INVALID_HANDLE_VALUE) {
        GetFinalPathNameByHandleA(file, m.name, sizeof(m.name) - 1, 0);
        stripExtendedPrefix(m.name);
    }
    if (g_dbgProcess) {
        IMAGE_DOS_HEADER dos;
        SIZE_T got = 0;
        if (ReadProcessMemory(g_dbgProcess, (LPCVOID)base, &dos, sizeof(dos), &got) &&
            got == sizeof(dos) && dos.e_magic == IMAGE_DOS_SIGNATURE) {
            IMAGE_NT_HEADERS nt;
            got = 0;
            if (ReadProcessMemory(g_dbgProcess, (LPCVOID)(base + dos.e_lfanew), &nt, sizeof(nt),
                                  &got) &&
                got >= sizeof(nt) && nt.Signature == IMAGE_NT_SIGNATURE) {
                m.size = nt.OptionalHeader.SizeOfImage;
            }
        }
    }
    ++g_modCount;
}

// Map a faulting address to the debuggee module containing it.
// Returns NULL if no module matches (address outside every recorded
// module - typical for a jump through a garbage pointer).
const char *blameFor(unsigned long long addr, unsigned long long *offOut) {
    for (int i = 0; i < g_modCount; ++i) {
        if (g_mods[i].size != 0 && addr >= g_mods[i].base &&
            addr < g_mods[i].base + g_mods[i].size) {
            if (offOut)
                *offOut = addr - g_mods[i].base;
            return g_mods[i].name;
        }
    }
    return NULL;
}

const char *baseNameOf(const char *path) {
    if (!path)
        return "";
    const char *s = strrchr(path, '\\');
    return s ? s + 1 : path;
}

void recordException(const DEBUG_EVENT &ev) {
    if (g_excCount >= kMaxExcs)
        return;
    ExcInfo &e = g_excs[g_excCount];
    const EXCEPTION_RECORD &r = ev.u.Exception.ExceptionRecord;
    e.code = r.ExceptionCode;
    e.addr = (unsigned long long)r.ExceptionAddress;
    e.firstChance = ev.u.Exception.dwFirstChance;
    e.avType = (r.NumberParameters >= 1) ? r.ExceptionInformation[0] : 0;
    e.avTarget = (r.NumberParameters >= 2) ? r.ExceptionInformation[1] : 0;
    e.rip = 0;
    e.blame[0] = 0;
    e.blameOff = 0;
    e.avTargetBlame[0] = 0;
    e.avTargetOff = 0;

    // Thread RIP as a backup attribution source (ExceptionAddress is
    // usually the faulting instruction itself, but for jumps through
    // garbage pointers RIP can land in a different module).
    HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, ev.dwThreadId);
    if (th) {
        CONTEXT ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(th, &ctx))
            e.rip = ctx.Rip;
        CloseHandle(th);
    }

    unsigned long long off = 0;
    const char *b = blameFor(e.addr, &off);
    if (!b && e.rip)
        b = blameFor(e.rip, &off);
    if (b) {
        strncpy(e.blame, b, sizeof(e.blame) - 1);
        e.blameOff = off;
    } else {
        snprintf(e.blame, sizeof(e.blame), "(no module - raw address 0x%016llx)", e.addr);
        e.blameOff = e.addr;
    }

    // Attribute the AV TARGET address too (v2.1). For a loader-phase
    // fault the instruction address blames ntdll (the loader code),
    // but the DATA being read belongs to the module named here - that
    // module is the real culprit.
    if (r.ExceptionCode == 0xC0000005 && r.NumberParameters >= 2) {
        unsigned long long toff = 0;
        const char *tb = blameFor(e.avTarget, &toff);
        if (tb) {
            strncpy(e.avTargetBlame, tb, sizeof(e.avTargetBlame) - 1);
            e.avTargetOff = toff;
        }
    }
    ++g_excCount;
}

struct WatchResult {
    bool spawned; // CreateProcess succeeded
    int outcome;  // 0 fatal exception, 1 died with NTSTATUS, 2 clean exit,
                  // 3 healthy (25 s, no fatal), 4 spawn failed
    DWORD exitCode;
    int fatalIdx; // index into g_excs of the second-chance exception (-1)
    DWORD spawnErr;
};

void runWatch(const char *targetPath, const char *workDir, WatchResult &res) {
    res.spawned = false;
    res.outcome = 4;
    res.exitCode = 0;
    res.fatalIdx = -1;
    res.spawnErr = 0;

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    char cmd[1100];
    snprintf(cmd, sizeof(cmd), "\"%s\"", targetPath);

    if (!CreateProcessA(targetPath, cmd, NULL, NULL, FALSE,
                        DEBUG_ONLY_THIS_PROCESS | CREATE_SUSPENDED, NULL, workDir, &si, &pi)) {
        res.spawnErr = GetLastError();
        return;
    }
    res.spawned = true;
    g_dbgProcess = pi.hProcess;
    ResumeThread(pi.hThread);

    ULONGLONG t0 = GetTickCount64();
    bool sawInitialBreakpoint = false;
    bool exited = false;
    bool weKilled = false;
    bool killAfter = false;

    for (;;) {
        DEBUG_EVENT ev;
        memset(&ev, 0, sizeof(ev));
        if (!WaitForDebugEvent(&ev, 2000)) {
            // Quiet period: no debug event pending.
            ULONGLONG elapsed = GetTickCount64() - t0;
            if (sawInitialBreakpoint && elapsed > 25000) {
                // Startup completed long ago and nothing fatal happened:
                // declare healthy and stop the debuggee before it shows a
                // window the user might mistake for the app working.
                TerminateProcess(pi.hProcess, 0);
                weKilled = true;
            } else if (elapsed > 60000) {
                // Hard cap: initial breakpoint never even arrived (extreme
                // loader hang). Kill and fall through to reporting.
                TerminateProcess(pi.hProcess, 0);
                weKilled = true;
            }
            if (weKilled) {
                // Loop once more so the EXIT_PROCESS_DEBUG_EVENT drains.
                if (exited)
                    break;
            }
            continue;
        }

        DWORD cont = DBG_CONTINUE;
        switch (ev.dwDebugEventCode) {
        case EXCEPTION_DEBUG_EVENT: {
            const DWORD code = ev.u.Exception.ExceptionRecord.ExceptionCode;
            const bool secondChance = (ev.u.Exception.dwFirstChance == 0);
            if (code == 0x80000003 && !sawInitialBreakpoint) {
                // The loader's initial breakpoint - everything is fine.
                sawInitialBreakpoint = true;
                cont = DBG_CONTINUE;
            } else if (code == 0x406D1388) {
                // SetThreadName() noise - benign, swallow.
                cont = DBG_CONTINUE;
            } else {
                recordException(ev);
                if (secondChance) {
                    // Unhandled by the app AND by WER-suppression: this is
                    // the crash. Swallow it (DBG_CONTINUE) + kill the
                    // debuggee so no WER dialog pops on the user's screen.
                    res.fatalIdx = g_excCount - 1;
                    res.outcome = 0;
                    killAfter = true;
                    cont = DBG_CONTINUE;
                } else {
                    // First chance: hand it to the app's own handlers (a
                    // Qt app legitimately raises C++ exceptions during
                    // init probing). If nobody handles it, the
                    // second-chance event above follows.
                    cont = DBG_EXCEPTION_NOT_HANDLED;
                }
            }
            break;
        }
        case CREATE_PROCESS_DEBUG_EVENT:
            recordModule((unsigned long long)(uintptr_t)ev.u.CreateProcessInfo.lpBaseOfImage,
                         ev.u.CreateProcessInfo.hFile);
            break;
        case LOAD_DLL_DEBUG_EVENT:
            recordModule((unsigned long long)(uintptr_t)ev.u.LoadDll.lpBaseOfDll,
                         ev.u.LoadDll.hFile);
            break;
        case EXIT_PROCESS_DEBUG_EVENT:
            res.exitCode = ev.u.ExitProcess.dwExitCode;
            exited = true;
            if (!weKilled && res.outcome != 0) {
                // NTSTATUS-failure range (0xC0000005 etc.) = the classic
                // "unable to start correctly" loader-abort death.
                res.outcome = (res.exitCode >= 0xC0000000) ? 1 : 2;
            }
            break;
        default:
            // CREATE/EXIT_THREAD, UNLOAD_DLL, OUTPUT_DEBUG_STRING, RIP:
            // not needed for the diagnosis; just continue.
            break;
        }

        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
        if (killAfter) {
            TerminateProcess(pi.hProcess, 1);
            killAfter = false;
            weKilled = true;
        }
        if (exited)
            break;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    g_dbgProcess = NULL;
}

void logWatch(const WatchResult &res) {
    logLine("");
    logLine("----- PHASE 2: debug-launch watch -----");
    if (!res.spawned) {
        char line[1200];
        snprintf(line, sizeof(line),
                 "CreateProcess failed for target: err=%lu - the exe could not even be",
                 (unsigned long)res.spawnErr);
        logLine(line);
        logLine("spawned (missing file / corrupt PE / blocked by policy).");
        return;
    }
    logLine("");

    char line[1200];
    snprintf(line, sizeof(line), "Module load trail (%d modules):", g_modCount);
    logLine(line);
    for (int i = 0; i < g_modCount; ++i) {
        snprintf(line, sizeof(line), "  [%3d] base=0x%016llx  size=0x%08llx  %s", i + 1,
                 g_mods[i].base, g_mods[i].size, g_mods[i].name);
        logLine(line);
    }
    logLine("");
    snprintf(line, sizeof(line), "Exception events (%d):", g_excCount);
    logLine(line);
    for (int i = 0; i < g_excCount; ++i) {
        const ExcInfo &e = g_excs[i];
        char targetNote[1100] = "";
        if (e.avTargetBlame[0]) {
            snprintf(targetNote, sizeof(targetNote), " -> target inside %s +0x%llx",
                     baseNameOf(e.avTargetBlame), e.avTargetOff);
        }
        snprintf(line, sizeof(line),
                 "  %s  code=0x%08lx  addr=0x%016llx  in %s +0x%llx  (AV %s 0x%016llx%s)",
                 e.firstChance ? "1st-chance" : "2ND-CHANCE", (unsigned long)e.code, e.addr,
                 baseNameOf(e.blame), e.blameOff,
                 e.avType == 0
                     ? "read of"
                     : (e.avType == 1 ? "write to" : (e.avType == 8 ? "EXECUTE of" : "?")),
                 e.avTarget, targetNote);
        logLine(line);
    }
    logLine("");
    switch (res.outcome) {
    case 0:
        if (res.fatalIdx >= 0) {
            const ExcInfo &e = g_excs[res.fatalIdx];
            snprintf(line, sizeof(line),
                     "VERDICT: unhandled exception 0x%08lx (second-chance) at 0x%016llx in",
                     (unsigned long)e.code, e.addr);
            logLine(line);
            snprintf(line, sizeof(line), "%s +0x%llx. That module is the crashing component.",
                     e.blame, e.blameOff);
            logLine(line);
        }
        break;
    case 1:
        snprintf(line, sizeof(line),
                 "VERDICT: process died during LOADER INIT with status 0x%08lx.",
                 (unsigned long)res.exitCode);
        logLine(line);
        logLine("No exception was dispatched to WER (that is why Event Viewer shows");
        logLine("nothing for FusionCutPro.exe). Details:");
        if (g_excCount > 0) {
            // A first-chance exception recorded just before death IS the
            // fault site - the loader's SEH swallowed it into the exit
            // status instead of letting WER report it.
            const ExcInfo &e = g_excs[g_excCount - 1];
            snprintf(line, sizeof(line),
                     "  first-chance exception 0x%08lx at 0x%016llx in %s +0x%llx",
                     (unsigned long)e.code, e.addr, e.blame, e.blameOff);
            logLine(line);
            logLine("  (recorded immediately before death - this is the fault site).");
            if (e.avTargetBlame[0]) {
                snprintf(line, sizeof(line),
                         "  the data being accessed lies in %s +0x%llx - THAT module",
                         e.avTargetBlame, e.avTargetOff);
                logLine(line);
                logLine("  is the culprit (ntdll is only the code reading it).");
            }
        } else {
            logLine("  no exception was raised at all - the loader aborted on its own.");
            logLine("  prime suspects = the last DLLs on the load trail above");
            logLine("  (the crash happened while one of them was loading/initializing).");
        }
        break;
    case 2:
        snprintf(line, sizeof(line), "VERDICT: process exited cleanly (code %lu).",
                 (unsigned long)res.exitCode);
        logLine(line);
        break;
    case 3:
        logLine("VERDICT: startup completed; process ran 25 s under the debug watch");
        logLine("without a fatal exception. The double-click crash does NOT reproduce");
        logLine("under a debugger - pointing at environment injection (antivirus /");
        logLine("shell hook DLL) rather than at FusionCutPro itself.");
        break;
    default:
        break;
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
    logLine("FusionCut Pro loader-check v2 (two-phase)");
    {
        char hdr[1100];
        snprintf(hdr, sizeof(hdr), "Target: %s", targetPath);
        logLine(hdr);
    }
    logLine("==============================================================");
    logLine("");

    // Reduce hard-error dialogs while we own the debuggee.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    // Set cwd to the portable folder so LoadLibraryExA's default
    // search order (application dir first) matches the real
    // FusionCutPro.exe loader behavior.
    SetCurrentDirectoryA(selfDir);

    logLine("PHASE 1: static import-tree probe (no target execution).");
    logLine("Each FAIL line names a DLL the loader cannot map:");
    logLine("");

    // ----- phase 1 -----
    HMODULE h = LoadLibraryExA(targetPath, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!h) {
        // The target itself can't be mapped - e.g. the file is gone,
        // or it's a 32-bit exe on 64-bit Windows, or corrupt.
        logFail(targetPath, GetLastError(), "(self)");
    } else {
        markVisited(targetName);
        walkImports(h, targetName, 0);
        FreeLibrary(h); // refcount-symmetric probe (see walkImports note)
    }

    // ----- phase 2 -----
    WatchResult res;
    runWatch(targetPath, selfDir, res);
    logWatch(res);

    if (g_logFile != INVALID_HANDLE_VALUE) {
        CloseHandle(g_logFile);
        g_logFile = INVALID_HANDLE_VALUE;
    }

    // ----- combined summary -----
    // 3072: worst case is blame basename + log path + fixed text; the
    // full module PATHS stay in the log file, the MessageBox uses
    // base names only.
    char summary[3072];
    int bad = (g_failCount > 0) || (res.outcome == 0) || (res.outcome == 1);
    if (res.outcome == 0 && res.fatalIdx >= 0) {
        const ExcInfo &e = g_excs[res.fatalIdx];
        snprintf(summary, sizeof(summary),
                 "CAUGHT THE STARTUP CRASH:\r\n\r\n"
                 "Unhandled exception 0x%08lx (2nd chance)\r\n"
                 "at 0x%016llx in module:\r\n  %s +0x%llx\r\n\r\n"
                 "%d DLLs loaded at the time.\r\n"
                 "Last loaded: %s\r\n\r\n"
                 "Full details in the log:\r\n%s",
                 (unsigned long)e.code, e.addr, baseNameOf(e.blame), e.blameOff, g_modCount,
                 g_modCount > 0 ? baseNameOf(g_mods[g_modCount - 1].name) : "(none)", g_logPath);
    } else if (res.outcome == 1) {
        char exc[600] = "";
        if (g_excCount > 0) {
            const ExcInfo &e = g_excs[g_excCount - 1];
            char tgt[200] = "";
            if (e.avTargetBlame[0]) {
                snprintf(tgt, sizeof(tgt),
                         "Data accessed lies in:\r\n  %s +0x%llx\r\n"
                         "(that module is the culprit;\r\n"
                         " ntdll is only the reader.)\r\n",
                         baseNameOf(e.avTargetBlame), e.avTargetOff);
            }
            snprintf(exc, sizeof(exc),
                     "A first-chance exception was recorded just before\r\n"
                     "death - that IS the fault site:\r\n"
                     "  0x%08lx at 0x%016llx in %s +0x%llx\r\n"
                     "%s\r\n",
                     (unsigned long)e.code, e.addr, baseNameOf(e.blame), e.blameOff, tgt);
        }
        snprintf(summary, sizeof(summary),
                 "DIED DURING LOADER INIT\r\n"
                 "(the 'unable to start correctly' class):\r\n\r\n"
                 "Process exited with status 0x%08lx before any\r\n"
                 "exception reached WER - which is exactly why\r\n"
                 "Event Viewer shows nothing for the app.\r\n\r\n"
                 "%s"
                 "%d DLLs mapped; last loaded: %s\r\n\r\n"
                 "Full load trail in the log:\r\n%s",
                 (unsigned long)res.exitCode, exc, g_modCount,
                 g_modCount > 0 ? baseNameOf(g_mods[g_modCount - 1].name) : "(none)", g_logPath);
    } else if (res.outcome == 3) {
        snprintf(summary, sizeof(summary),
                 "STARTUP COMPLETED UNDER THE DEBUG WATCH.\r\n\r\n"
                 "FusionCutPro.exe initialized and ran 25 seconds\r\n"
                 "without a fatal exception (its window appeared).\r\n\r\n"
                 "The double-click crash does NOT reproduce under a\r\n"
                 "debugger - that points at an injected shell/AV DLL\r\n"
                 "in the Explorer launch path, not at the app.\r\n\r\n"
                 "Try: right-click the zip -> Properties -> Unblock\r\n"
                 "(BEFORE extracting), or launch from cmd.exe.\r\n\r\n"
                 "Full details in the log:\r\n%s",
                 g_logPath);
    } else if (res.outcome == 2) {
        snprintf(summary, sizeof(summary),
                 "Process exited cleanly (code %lu).\r\nPhase 1: %d import failure(s).\r\n\r\n"
                 "Full details in the log:\r\n%s",
                 (unsigned long)res.exitCode, g_failCount, g_logPath);
    } else if (res.outcome == 4) {
        snprintf(summary, sizeof(summary),
                 "Could not spawn the target (err=%lu).\r\nPhase 1: %d import failure(s).\r\n\r\n"
                 "Full details in the log:\r\n%s",
                 (unsigned long)res.spawnErr, g_failCount, g_logPath);
    } else {
        snprintf(summary, sizeof(summary),
                 "Phase 1: %d import failure(s).\r\nPhase 2: no verdict.\r\n\r\n"
                 "Full details in the log:\r\n%s",
                 g_failCount, g_logPath);
    }

    MessageBoxA(NULL, summary, "FusionCut Pro Loader Check",
                MB_OK | (bad ? MB_ICONERROR : MB_ICONINFORMATION));
    return bad ? 1 : 0;
}
