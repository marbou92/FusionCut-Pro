// FusionCut Pro - baked-in Windows loader + startup diagnostic.
//
// REPLACES the standalone fcp-loader-check.exe it used to ship with: the
// same two-phase diagnostic now ships inside FusionCutPro.exe and runs
// as `FusionCutPro.exe --diag`. The exe launches a second copy of
// ITSELF (`--diag-child`) under the classic kernel32 debug API and
// watches it start - no separate binary, no extra install file, no
// -static toolchain target.
//
//   PHASE 1 - static import-tree probe (target NOT executed):
//     Maps FusionCutPro.exe + every DLL in its transitive import tree
//     with LoadLibraryEx(DONT_RESOLVE_DLL_REFERENCES) and walks the PE
//     import directories manually. Catches missing DLLs
//     (ERROR_MOD_NOT_FOUND) and wrong-architecture DLLs
//     (ERROR_BAD_EXE_FORMAT) by name, without running any DllMain.
//     Each probe mapping is FreeLibrary'd immediately after its subtree
//     walk so the loader teardown stays clean at exit (the historical
//     fcp-loader-check bug, kept fixed).
//
//   PHASE 2 - debug-launch watch (the decisive diagnostic):
//     Spawns a fresh FusionCutPro.exe (--diag-child) with
//     DEBUG_ONLY_THIS_PROCESS (this process becomes its debugger) and
//     pumps debug events. Debug events are delivered to the debugger
//     BEFORE Windows Error Reporting - including exceptions raised
//     inside DllMain / TLS callbacks / static initializers during
//     loader initialization, which is exactly the crash class where
//     the bare "unable to start correctly (0xc0000005)" dialog appears
//     with NO Event-1000 entry and NO in-process log.
//
//     The watch records:
//       * every LOAD_DLL event (module name, base, size) - the load
//         trail shows exactly how far initialization got;
//       * every exception event (code, address, first/second chance,
//         AV read/write/execute type + target address, thread RIP);
//       * a second-chance (unhandled) exception is caught, blamed on
//         the module containing the faulting address, and the child is
//         terminated before WER can show any dialog;
//       * if the child dies with an NTSTATUS but no exception was
//         dispatched to WER (the pure loader-abort path - the exact
//         "unable to start correctly (0xc0000005)" mode), any
//         first-chance exception recorded just before death names the
//         fault site; failing that, the last DLLs on the load trail are
//         reported as prime suspects;
//       * if the child initializes and runs 25 s without a fatal
//         exception it is reported healthy - a double-click crash that
//         does not reproduce under a debugger points at environment
//         injection (antivirus / shell hook DLLs), not the app.
//
//   Everything is kernel32 + user32 only (both already linked for the
//   crash handler); no dbghelp, no psapi, no debugger install required.
//
// Self-debugging is legal: a Windows process may debug ANOTHER instance
// of the same image (the child is a separate process with its own
// address space; only the image file is shared). The child recognizes
// --diag-child in main.cpp and starts the NORMAL app path, so the flag
// can never recurse into another supervisor.

#pragma once

#include <string>

namespace fc {

// Run the two-phase diagnostic. Returns 0 when the install is healthy,
// 1 when any import-tree failure or fatal/abort outcome was recorded.
// On non-Windows hosts prints a note and returns 1 (the mode exists to
// diagnose the Windows loader; there is nothing to watch elsewhere).
int runDiagSupervisor(const std::string &appVersion);

} // namespace fc
