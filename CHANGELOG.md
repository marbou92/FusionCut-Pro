# Changelog

All notable changes to FusionCut Pro are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project adheres to [Semantic Versioning](https://semver.org/).

## [0.4.8] - 2026-09-02

**v0.4.7's shim source died in the CI MinGW leg on a function
name that does not exist in the Windows API - and the compile
error was masking a second, semantic bug in the same function.
Both are fixed, and the offline verification battery that let the
phantom name through is rebuilt so it cannot happen again.**

### Fixed - CI compile failure: `SignalConditionVariable` is not a Windows function
- The MinGW leg aborted at `src/app/api_set_synch.c:311` with
  `implicit declaration of function 'SignalConditionVariable';
  did you mean 'WakeAllConditionVariable'?`. The Windows
  condition-variable wake surface is EXACTLY two functions:
  `WakeConditionVariable` (wakes ONE waiter) and
  `WakeAllConditionVariable` (wakes ALL waiters). There is no
  `SignalConditionVariable` anywhere in the Win32 API - the name
  was carried over from pthread terminology
  (`pthread_cond_signal`), the classic API-confusion trap.
  Cross-evidence that the rest of the surface is right: gcc's
  suggestion engine only proposes in-scope identifiers, and the
  neighboring call sites (`WakeConditionVariable`,
  `SleepConditionVariableSRW`, `AcquireSRWLockExclusive`,
  `ReleaseSRWLockExclusive`) produced no diagnostics in the same
  failed log - the real mingw-w64 headers declare all of them at
  `_WIN32_WINNT 0x0601`.
- **The branches were also swapped - a latent runtime bug the
  compile error happened to mask.** `fcp_wake(..., wake_all=TRUE)`
  (i.e. `WakeByAddressAll`) called `WakeConditionVariable`, which
  wakes exactly ONE waiter: with N threads blocked on the same
  address, a wake-one call leaves the other N-1 sleeping until
  their timeout (or forever with `INFINITE`). Correct mapping now:
  `WakeByAddressAll` -> `WakeAllConditionVariable` (broadcast),
  `WakeByAddressSingle` -> `WakeConditionVariable` (wake one),
  matching the documented api-set contract.

### Fixed - `Sleep` export warning localized
- The same CI log flagged `-Wattributes` at line 152:
  windows.h declares `Sleep` with `dllimport` (its real home is
  kernel32); the shim deliberately re-declares it with
  `dllexport` because THIS DLL is the api set, and gcc warns while
  merging the two declarations - the merged symbol is dllexport,
  which is exactly the state we want. The warning is now silenced
  by a `#pragma GCC diagnostic push` / `ignored "-Wattributes"` /
  `pop` pair wrapped around ONLY the four-export prototype block;
  no other diagnostic is affected.

### Fixed - the verification gap that let the phantom through
- v0.4.7's mock `windows.h` declared `SignalConditionVariable`
  because it was authored from the same wrong mental model as the
  source it was auditing - a self-confirming oracle: the mock
  agreed with the very bug it existed to catch, so the phantom
  compiled clean in the sandbox and only CI saw it. The mock is
  rebuilt (and now kept persistent at `winmock/windows.h` in the
  delivery workspace instead of ephemeral /tmp) under an explicit
  rule: it declares EXACTLY the real mingw-w64 function surface
  for the shim's include context at `_WIN32_WINNT 0x0601` (the
  full CV + SRW family, `Sleep`, `GetTickCount64`, `SetLastError`,
  `DisableThreadLibraryCalls`, with the real version guards - the
  Win8 futex family correctly EXCLUDED at 0x0601, which is what
  lets the shim own those export names) and NOTHING INVENTED.
- Regression proof, now a permanent battery item: re-injecting the
  v0.4.7 line into the fixed source reproduces the CI failure
  byte-for-byte offline (`implicit declaration of function
  'SignalConditionVariable'; did you mean 'WakeAllConditionVariable'?`),
  while the fixed source compiles clean. The mock demonstrably has
  the teeth that v0.4.7's mock lacked.

### Verified
- Mock cross-compile clean at `-O0` and `-O2`
  (`-std=c11 -Wall -Wextra -Werror -Wpedantic`); `nm -u` shows the
  undefined set is EXACTLY the 8 real kernel32 imports
  (`AcquireSRWLockExclusive`, `ReleaseSRWLockExclusive`,
  `SleepConditionVariableSRW`, `WakeConditionVariable`,
  `WakeAllConditionVariable`, `GetTickCount64`, `SetLastError`,
  `DisableThreadLibraryCalls`) and zero CRT/libgcc helper
  references - the `-nostdlib` CRT-free contract is intact.
- Whole-tree clang-format 22.1.8 gate: 0 violations; the `.c` shim
  (outside the gate's `*.cpp`/`*.h` glob) individually
  format-clean.
- Real CMake pipeline (Linux, `FC_BUILD_APP=OFF`): core 88 +
  timeline 64 = 152 checks pass. Workflow YAML valid; the staging
  script passes `bash -n`; the `PORTABLE.txt` printf executed in
  the sandbox (58 lines, zero apostrophes).
- Remaining oracles unchanged: the CI MinGW leg for the PE link
  (`-nostdlib`, `--entry=DllMain`, `PREFIX ""` - the export
  verification step is untouched, all four export names intact),
  then the user's Windows 7 machine for the real startup and
  futex semantics.

## [0.4.7] - 2026-09-01

**(superseded within one CI run - see [0.4.8])**

**v0.4.6 never shipped — the portable workflow itself died in CI
before it could zip anything. And the v0.4.6 fix design was wrong
for the user's actual test platform (Windows 7). This release fixes
both, and the compatibility shim is now a real, first-class build
component instead of a loose CI-only artifact definition.**

### Fixed — the v0.4.6 CI failure (`syntax error near unexpected token (`)
- One character class of bug: `PORTABLE.txt` prose was built with a
  bash `printf` whose arguments are single-quoted strings, and one
  line contained an **apostrophe escaped as `\'`** (`FFmpeg 8\'s`).
  Inside single quotes, `\'` is *not* an escape — the backslash is
  literal and the quote **terminates the string**. That flipped the
  quote parity of the whole remainder of the script, so the next
  `(` that landed *outside* quotes (`executable (FusionCutPro-crash-…`)
  aborted the parse: `line 138: syntax error near unexpected token ('`.
  The log's line number matches YAML line 192 minus the 54 script
  header lines exactly; reproduced byte-for-byte in the sandbox with
  `bash -n` against the v0.4.6 workflow (`exit 2`, same line 138), and
  the fixed script passes `bash -n` cleanly.
- Why the previous verification battery missed it: the YAML file is
  *valid YAML* (python `yaml.safe_load` passes) — the defect only
  exists at the bash layer. The battery now extracts the run block
  and runs `bash -n` on it; the `PORTABLE.txt` printf is additionally
  *executed* in the sandbox and its 58-line output inspected, plus a
  no-apostrophe scan over the generated file. A comment at the
  printf now documents the rule: no apostrophes inside the
  single-quoted arguments (write "the DLL that ships with FFmpeg 8",
  not "FFmpeg 8's DLL").
- The `ld.exe: warning: cannot find entry symbol DllMainCRTStartup`
  in the same log was benign (a DLL with entry RVA 0 is legal — the
  loader simply skips initialization) — but it is now gone too: the
  shim below provides a real `DllMain` and passes `--entry=DllMain`.

### Changed — real Win7-compatible api-set shim replaces the forwarder
- The user's test platform is **Windows 7** (v0.4.6 had assumed
  8.x from the module-size forensics). That matters: a *forwarder*
  stub exports `WaitOnAddress` as a forwarder into KernelBase — but
  KernelBase only implements the futex family since Windows 8. On
  Windows 7 the import would bind against our export table and then
  entry-point resolution would fail while loading `librav1e.dll`
  ("The procedure entry point WaitOnAddress could not be located") —
  trading the `0xc0000005` startup abort for an entry-point abort.
- NEW `src/app/api_set_synch.c`: the shim now carries **real
  implementations** of the full api set (`WaitOnAddress`,
  `WakeByAddressAll`, `WakeByAddressSingle`, `Sleep`) built
  exclusively from Windows-7-era kernel32 primitives (SRWLOCK and
  CONDITION_VARIABLE — both Vista+, both validly initialized by
  zeroed memory, so all shim state lives in `.bss` and `DllMain`
  does pointer writes only; `GetTickCount64` for timeouts). Futex
  semantics on top: a 512-bucket hash table keyed by the waited
  address, one wait node per active address (static pool of 4096),
  and — the part that makes it correct — the waiter holds the bucket
  SRWLock across both the value comparison *and* the
  `SleepConditionVariableSRW` call, so a concurrent `WakeByAddress*`
  (which must take the same lock) is impossible to miss: no lost
  wakes, spurious wakeups only, exactly the documented contract.
  Node recycling follows a strict bucket → free-list lock order;
  pool exhaustion fails loudly (`ERROR_NOT_ENOUGH_MEMORY`) instead
  of corrupting state.
- The shim keeps the zero-dependency rule of `fcp-loader-check`:
  linked with `-nostdlib` (kernel32 is its **only** import — no CRT,
  no MinGW runtime). Verified in the sandbox that the object emits
  no `memset`/`memcpy`/division-helper references at `-O0` and `-O2`
  (the code avoids aggregate zeroing and division precisely so
  `-nostdlib` can never break the link).
- Mechanism (unchanged from v0.4.6's analysis, now covering Win7
  too): the DLL search order tries the application directory before
  System32, so `api-ms-win-core-synch-l1-2-0.dll` next to
  `FusionCutPro.exe` wins the bind on Windows 7 and 8.x. On
  Windows 10/11 the ApiSetSchema resolves the name to KernelBase
  *before* the file search, so the bundled file is never touched —
  shipping it everywhere is inert and safe.

### Removed — `packaging/` directory
- `packaging/api-ms-win-core-synch-l1-2-0.def` is deleted. The shim
  is now a **first-class build component**, not loose CI packaging
  data: CMake target `fcp-apiset-synch` (in `src/app/CMakeLists.txt`)
  builds `api_set_synch.c` into `api-ms-win-core-synch-l1-2-0.dll`
  (exact api-set name via `OUTPUT_NAME` + empty `PREFIX`) and the
  regular `cmake --install` staging step places it next to
  `FusionCutPro.exe` — the same pipeline as the executables. The
  workflow's inline `gcc -shared -nostdlib … packaging/*.def` build
  is gone; what remains in CI is the export-table verification
  (end-of-line-anchored `objdump -p` grep for all four export names,
  blocking) so a silent export regression can never ship.
- Top-level `project()` gains the C language (`LANGUAGES C CXX`) —
  required to compile the shim's `.c` source; harmless on every CI
  leg (gcc/clang/MinGW/MSVC all detect a C compiler).

### Changed — system requirements floor
- Windows 7 SP1+ **restored** as the minimum (README updated): the
  bundled shim now provides the missing api set *functionally* on
  Windows 7, which the v0.4.6 forwarder design could not.

### Verified
- `bash -n` on the extracted staging script: old workflow reproduces
  `line 138: syntax error near unexpected token (' exactly; new
  workflow passes. `PORTABLE.txt` printf executed in the sandbox —
  58 lines, no apostrophes.
- Mock `_WIN32` cross-compile of `api_set_synch.c`
  (`-std=c11 -Wall -Wextra -Werror -Wpedantic`, recreated mock
  `windows.h` with the exact MinGW-w64 kernel32 surface the shim
  uses) clean; `nm` confirms all four api-set exports plus `DllMain`
  and only kernel32 functions as undefined symbols, at `-O0` and
  `-O2`.
- Whole-tree clang-format 22.1.8 gate exit 0 (47 sources including
  the new file); real CMake pipeline (Linux, `FC_BUILD_APP=OFF`):
  core 88 + timeline 64 = 152 tests pass; workflow YAML validated.
- The PE link itself (`-nostdlib`, `--entry=DllMain`, `PREFIX ""`)
  is verified by the CI MinGW leg (the sandbox has no Windows
  toolchain) — its `objdump` export check is the first gate; the
  user's Windows 7 machine is the final oracle.

## [0.4.6] - 2026-09-01

**Root cause found and fixed — the startup `0xc0000005` that has
broken every portable build on the user's machine since the first
MSYS2-FFmpeg package.**

### Root cause (final — confirmed by field data from the v0.4.5
debug-launch watch)
- `fcp-loader-check.exe` v2's watch caught the death that WER never
  reports: a first-chance `0xC0000005` **READ** at `ntdll.dll+0x4b4b4`
  (the Windows loader's import-snapping code), targeting an address
  **inside the just-mapped `api-ms-win-core-synch-l1-2-0.dll` image**
  (`0x7fef69f13f3` = module base `0x7fef69f0000` + `0x13f3`), and the
  load trail stopped at module 59 of the loader's depth-first walk:
  `FusionCutPro.exe → avcodec-62.dll → … → librav1e.dll →
  api-ms-win-core-synch-l1-2-0.dll`.
- `librav1e.dll` (the Rust-written AV1 encoder in MSYS2's FFmpeg 8
  dependency tree) hard-imports the `api-ms-win-core-synch-l1-2-0`
  API set (the `WaitOnAddress` futex family) by its literal name.
- On Windows 10/11 the loader resolves that name via the ApiSetSchema
  and redirects to KernelBase before the file search runs — those
  machines are unaffected (which is why CI, on windows-latest, always
  passed).
- On the user's machine (Windows 8.x — corroborated by the
  `gdi32 → lpk.dll → usp10.dll` load chain and the module size
  profile: shell32 14.2 MB / user32 1.0 MB / KernelBase 434 KB) the
  loader does **not** schema-resolve the name. It maps the System32
  *placeholder stub file*, then dereferences that stub's invalid
  export data while snapping librav1e's imports → READ fault on an
  unmapped page → `0xC0000005` inside ntdll, **before any user code
  runs** → the loader converts the fault into the process exit status
  → "The application was unable to start correctly (0xc0000005)"
  dialog, WER never engages, Event Viewer stays empty, no in-process
  handler (VEH, boot trace) can ever see it. This is why v0.4.1
  through v0.4.5 all failed identically: the import chain was always
  present; the crash was never a missing DLL and never a
  crash-handler-timing issue.

### Fixed — Windows 8.x api-set forwarder stub
- NEW `packaging/api-ms-win-core-synch-l1-2-0.def`: a
  forwarder-only DLL definition (no code, no imports, no CRT) that
  exports `Sleep`, `WaitOnAddress`, `WakeByAddressAll`,
  `WakeByAddressSingle` as **forwarders to their KernelBase
  implementations** (present since Windows 8).
- `portable-build.yml` builds it in the staging step with
  `gcc -shared -nostdlib -o dist/api-ms-win-core-synch-l1-2-0.dll
  packaging/api-ms-win-core-synch-l1-2-0.def` and **verifies the
  artifact with `objdump`** (the export table must contain
  `WaitOnAddress`), blocking the job if the .def→DLL pipeline ever
  breaks — the sandbox has no PE toolchain, so CI is the authoritative
  oracle for this artifact.
- Mechanism of the fix: the DLL search order tries the **application
  directory before System32**, so the stub placed next to
  `FusionCutPro.exe` wins the bind on Windows 8.x. The loader reads
  OUR valid export table, follows the forwarders into the
  already-loaded KernelBase, and startup proceeds. On Windows 10/11
  the schema resolves the name before the file search, so the stub is
  inert there — shipping it everywhere is safe.
- `PORTABLE.txt` now tells the user the bundled
  `api-ms-win-core-synch-l1-2-0.dll` is REQUIRED on Windows 8.x and
  to leave it in place.

### Improved — `fcp-loader-check.exe` v2.1
- AV-target module attribution: exception records now also map the
  access-violation **target** address to the module containing it and
  print it in the log line (`… (AV read of 0x… -> target inside
  api-ms-win-core-synch-l1-2-0.dll +0x13f3)`), in the loader-abort
  VERDICT ("the data being accessed lies in … — THAT module is the
  culprit; ntdll is only the code reading it"), and in the summary
  MessageBox. This is the analysis step that had to be done by hand
  to crack this case; the tool now does it automatically, so the NEXT
  loader-phase fault (if any) names its culprit directly in the log.
- Mock `_WIN32` cross-compile (re-created with the full debug-API
  surface) clean under `-Wall -Wextra -Werror -Wpedantic`; whole-tree
  clang-format gate exit 0.

### Changed
- System requirements floor: Windows 7 → **Windows 8.1** (README
  updated with the rationale — the `WaitOnAddress` implementations do
  not exist on Windows 7, so no forwarder can rescue it there;
  Windows 8.x works via the bundled stub, Windows 10/11 natively).

### Verified
- Mock `_WIN32` cross-compile of the v2.1 tool clean; CI-identical
  whole-tree format gate exit 0; real CMake pipeline (Linux,
  `FC_BUILD_APP=OFF`): core 88 + timeline 64 = 152 tests pass;
  workflow YAML validated.
- The forwarder DLL itself cannot be built or linked in the sandbox
  (no MinGW/PE toolchain): CI builds it, CI's `objdump` check proves
  its export table, and the user's machine is the final oracle. If a
  further schema-miss API set appears at a later startup stage, the
  v2.1 tool's log will name it directly (AV-target attribution), and
  the same forwarder technique applies.

## [0.4.5] - 2026-09-01

Diagnostic escalation. Field report from the v0.4.4 portable build:
`FusionCutPro.exe` still dies on double-click with the bare
"unable to start correctly (0xc0000005)" dialog; `fcp-loader-check.exe`
(v1) ran its import-tree probe and reported **all DLLs present** — but
the tool itself then crashed at process exit (WER BEX64 event,
execute-violation at a raw import-table RVA), and Event Viewer showed
**no Application-Error event for FusionCutPro.exe at all**.

### Root cause (what the field report proved)
- The v0.4.4 import probe was *correct*: every DLL in the portable
  folder maps. The startup crash is therefore **not** a missing or
  wrong-architecture DLL.
- The absence of an Event-1000 entry for FusionCutPro.exe means the
  failure never reaches WER's in-process reporting — it dies inside
  the loader-initialization phase (DllMain / TLS callbacks / static
  initializers), where the failure is converted into the process
  exit status and the "unable to start correctly" dialog is all the
  user ever sees. No in-process handler (VEH, boot trace) and no
  post-hoc WER log can observe that window.
- The v1 tool's own exit crash: it left ~250 DLLs mapped with
  `DONT_RESOLVE_DLL_REFERENCES` and never freed them; at process exit
  the loader teardown jumped through an unresolved import-table entry
  (a raw RVA, e.g. `0x9fade` → DEP execute violation → the WER BEX64
  event the user pasted). Diagnostic bug in v0.4.4, now fixed.

### Added — phase 2: debug-launch watch in `fcp-loader-check.exe`
- `src/app/loader_check.cpp` v2 is now a **two-phase** diagnostic:
  - **Phase 1** (unchanged in spirit): static import-tree probe with
    `DONT_RESOLVE_DLL_REFERENCES`; catches missing/wrong-arch DLLs by
    name. v2 change: every probe is `FreeLibrary`d immediately after
    its subtree walk, so the loader's module list is clean at process
    death and the tool no longer crashes after finishing (the exact
    v0.4.4 exit-crash root cause).
  - **Phase 2** (new — the decisive diagnostic): the tool spawns
    `FusionCutPro.exe` **suspended with `DEBUG_ONLY_THIS_PROCESS`**
    (the tool becomes the target's debugger), resumes it, and pumps
    debug events via the classic Windows debug API
    (`WaitForDebugEvent` / `ContinueDebugEvent` — kernel32 only, no
    dbghelp/psapi, no debugger install). Debug events are delivered
    to the debugger **before** Windows Error Reporting, including
    exceptions raised inside DllMain / TLS callbacks / static
    initializers — precisely the class where the bare
    "0xc0000005 unable to start" dialog appears with no Event-1000
    entry and no log.
- What the watch records and reports:
  - Every `LOAD_DLL` event (module name via the event's file handle +
    `GetFinalPathNameByHandleA`, base + size via remote PE-header
    read with `ReadProcessMemory`) — the load trail shows exactly how
    far initialization got.
  - Every exception event: code, address, first/second chance, access
    type (read/write/**execute**) + target address, thread RIP, and
    **fault-address → module attribution** (module + offset).
  - A second-chance (unhandled) exception is caught, named, and the
    target is terminated *before* WER can show any dialog.
  - If the process dies with an NTSTATUS but no exception reached WER
    (the exact "unable to start correctly" mode): any first-chance
    exception recorded just before death **is the fault site** (the
    loader's SEH swallowed it into the exit status); failing that, the
    last DLLs on the load trail are reported as prime suspects.
  - If the process initializes and runs 25 s without a fatal
    exception: reported healthy — the crash does not reproduce under
    a debugger, which points at environment injection (antivirus /
    shell-hook DLL in the Explorer launch path) rather than the app;
    the summary then suggests unblocking the zip / launching via cmd.
- First-chance C++ exceptions (`0xE06D7363`, thread-name exceptions
  `0x406D1388`) are passed through unhandled to the app's own EH —
  a Qt app legitimately raises benign exceptions during init probing;
  only second-chance events are treated as fatal.
- Still zero-dependency by design: `WIN32` subsystem, `-static`
  link, kernel32 + user32 only.

### Fixed
- `fcp-loader-check.exe` exit crash (v0.4.4): `DONT_RESOLVE` probe
  mappings are now `FreeLibrary`d right after each subtree walk
  (refcount-symmetric probing). The v1 tool reliably produced its log
  and summary, then crashed at process death with a WER BEX64 event
  that polluted Event Viewer with an `fcp-loader-check.exe` entry —
  confusingly similar to the app's own crash signature.

### Verified
- Mock `_WIN32` cross-compile on Linux
  (`g++ -std=c++17 -D_WIN32 -I winmock -Wall -Wextra -Werror
  -Wpedantic`): clean. The mock was extended with the full debug-API
  surface the new phase uses (`DEBUG_EVENT` + its union of
  `*_DEBUG_INFO` structs, `EXCEPTION_RECORD` (with
  `ExceptionInformation[15]`), aligned x64-style `CONTEXT` with
  `Rip`, `STARTUPINFOA`/`PROCESS_INFORMATION`, `CONTEXT_CONTROL`,
  `THREAD_GET_CONTEXT`, `DBG_CONTINUE`/`DBG_EXCEPTION_NOT_HANDLED`,
  event-code constants 1–9, `DEBUG_ONLY_THIS_PROCESS`,
  `CREATE_SUSPENDED`, `SEM_*`, and the kernel32 signatures for
  `CreateProcessA`, `ResumeThread`, `WaitForDebugEvent`,
  `ContinueDebugEvent`, `OpenThread`, `GetThreadContext`,
  `ReadProcessMemory`, `TerminateProcess`,
  `GetFinalPathNameByHandleA`, `GetTickCount64`, `SetErrorMode`,
  `FreeLibrary`). The mock compile caught one real source bug
  (unused parameter) and one format-truncation risk before CI.
- Format check: `clang-format -i src/app/loader_check.cpp` then the
  CI-identical whole-tree gate
  (`find src tests -type f \( -name '*.cpp' -o -name '*.h' \)
  -print0 | xargs -0 -n1 clang-format --dry-run --Werror`) exits 0 —
  **all** touched files formatted this time (the v0.4.4 miss).
- Real CMake pipeline (Linux, `FC_BUILD_APP=OFF`): core 88 + timeline
  64 = 152 tests pass. No regressions; the loader-check target is
  Windows-only.
- Windows-only behavior (debug-event flow, remote PE read,
  `GetFinalPathNameByHandleA` path handling) is CI-verified by the
  MinGW portable leg — same caveat as v0.4.2–v0.4.4: the sandbox has
  no Windows; the mock is a syntax-level gate, the CI leg is the
  authoritative oracle.

## [0.4.4] - 2026-08-30

Correction + diagnostic tool. v0.4.3's pre-main VEH install +
boot-trace log did NOT resolve the user's "0xc0000005 unable to start"
crash — the portable .exe still failed immediately on double-click with
the bare Windows loader dialog and **no log file** was produced.

### Root cause (revised)
- v0.4.3 assumed the "unable to start correctly (0xc0000005)" dialog
  was a Win32 exception a VEH could catch if installed early enough.
  This was wrong. That specific dialog is the **Windows loader's own
  failure dialog** (`BasepReportFatalError`), shown when
  `ntdll!LdrpInitializeProcess` returns `STATUS_ACCESS_VIOLATION`
  during process setup. The Windows loader sequence is:
  ```
  map exe image
    -> walk Import Directory, load each dependent DLL (recursive)
      -> run each DLL's DllMain(DLL_PROCESS_ATTACH)
        -> run .CRT$XIA  (CRT init)
          -> run .CRT$XCU  (user static initializers)   <- v0.4.3 VEH lives here
            -> call main() / WinMain
  ```
- A loader-phase `0xc0000005` aborts **before** the `.CRT$XCU` step.
  That means the `EarlyCrashHandlerInstaller` static initializer that
  v0.4.3 added **never runs** — the VEH is never installed, the
  boot-trace log file is never opened, no `AddVectoredExceptionHandler`
  / `SetUnhandledExceptionFilter` / `set_terminate` call ever executes.
- Even if the static initializer DID run, a loader-phase failure is
  not dispatched as a Win32 exception to user code — the loader hands
  the failure NTSTATUS straight to Windows Error Reporting, which shows
  the dialog. No user-mode exception filter can observe it. VEH is the
  wrong tool for this entire class of failure.

### Added — `fcp-loader-check.exe` (zero-dependency PE import walker)
- `src/app/loader_check.cpp` (NEW): a Windows-only diagnostic that
  walks the PE import tree of `FusionCutPro.exe` **from outside the
  process** and reports, by name, every DLL the loader cannot map.
  This is the diagnostic the bare "0xc0000005 unable to start" dialog
  does not show — it names the missing/wrong DLL so the actual fix
  (a one-line recipe edit to bundle that DLL) is immediate.
- Design constraints that make the tool survive exactly the scenario
  it diagnoses:
  * **WIN32 subsystem app** (no console flash on double-click).
  * **Fully static link** (`-static` in CMake): the tool has **zero DLL
    dependencies of its own** — it runs even when the portable folder
    is missing a DLL that breaks `FusionCutPro.exe`. Without this,
    `fcp-loader-check.exe` would itself 0xc0000005 at startup, making
    it useless as a diagnostic.
  * Uses **only `kernel32` + `user32`** functions — both are always
    present on every Windows since NT 3.1.
  * Walks imports via `LoadLibraryEx(..., DONT_RESOLVE_DLL_REFERENCES)`
    — maps each image **without running DllMain and without resolving
    its own imports**. A bad `DllMain` in a dependency therefore cannot
    crash the tool itself.
  * Recursively walks the transitive import tree (depth-first, with a
    visited set to break cycles, hard cap at depth 24).
- Output: a `MessageBox` summarizing the result (0 failures → "the
  0xc0000005 is a DllMain crash, use Event Viewer"; N failures → "open
  the log to see the failing DLL name(s)") plus a
  `loader-check-<timestamp>.log` next to the exe. Falls back to
  `%TEMP%` if the portable folder is read-only (e.g. extracted to
  `Program Files`).
- Each FAIL line names the DLL + the `GetLastError` code + the OS
  error string. Distinct error codes pin distinct root causes:
  * `ERROR_MOD_NOT_FOUND` (126) — the classic missing-DLL.
  * `ERROR_BAD_EXE_FORMAT` (193) — wrong-architecture DLL (e.g. a
    32-bit `Qt5Core.dll` in a 64-bit package).
  * other — corrupt PE, access denied, etc.
- `src/app/CMakeLists.txt`: new `fcp-loader-check` target. Windows-only
  (`if(WIN32)`), `WIN32` subsystem, links `user32` (kernel32 auto-linked),
  `target_link_options(... PRIVATE -static)` under MinGW. Install rule
  drops the exe next to `FusionCutPro.exe` in the portable root.
- `.github/workflows/portable-build.yml`: the existing ldd sweep
  (`for bin in dist/*.exe ...`) naturally picks up
  `fcp-loader-check.exe` — but because the tool is `-static`, ldd
  returns zero `/mingw64/` deps for it, which is correct, not a bug.
  Comment added to the sweep explaining this so a future maintainer
  doesn't "fix" the empty result by removing the static link.
  `PORTABLE.txt` rewritten with a clear triage ladder:
  (1) won't start → run `fcp-loader-check.exe` first;
  (2) starts but crashes at runtime → `crash-logs/` has the report.

### What `fcp-loader-check.exe` does NOT catch
- A DLL that **maps fine but crashes inside its own `DllMain`** when
  the real `FusionCutPro.exe` loads it. `DONT_RESOLVE_DLL_REFERENCES`
  skips `DllMain`, so the tool cannot observe a `DllMain` fault. For
  that class the documented fallback is **Event Viewer**:
  `eventvwr.msc → Windows Logs → Application → filter for
  FusionCutPro.exe → read the "Faulting module name:" line of the most
  recent Application Error event** (Event ID 1000). That line names the
  crashing DLL in 30 seconds with no tool install. The
  `fcp-loader-check.exe` MessageBox explicitly tells the user to do
  this when the import walk reports 0 failures.

### Fixed (v0.4.3 narrative correction)
- The `[0.4.3]` CHANGELOG entry claimed the static-init VEH install
  "closes the loader-phase crash gap". That was incorrect — see the
  revised root cause above. The static-init install is still useful
  for **runtime** crashes that happen after `.CRT$XCU` (it widens the
  window the VEH covers from "main() onward" to "static-init onward"),
  but it cannot and does not cover loader-phase failures. v0.4.3's
  code is left in place (it is correct for its actual scope); v0.4.4
  adds the complementary tool for the class v0.4.3 cannot reach.

### Verified
- `src/app/loader_check.cpp` standalone cross-compile on Linux with a
  minimal `_WIN32` mock-header set (re-created for this file only):
  `g++ -std=c++17 -D_WIN32 -I winmock -Wall -Wextra -Werror -Wpedantic
  -c loader_check.cpp` parses clean. The mock declares the PE structs
  (`IMAGE_DOS_HEADER`, `IMAGE_NT_HEADERS`, `IMAGE_IMPORT_DESCRIPTOR`,
  `IMAGE_DIRECTORY_ENTRY_IMPORT`), the macros
  (`IMAGE_DOS_SIGNATURE`, `IMAGE_NT_SIGNATURE`,
  `DONT_RESOLVE_DLL_REFERENCES`, `MAX_PATH`, `WIN32_LEAN_AND_MEAN`,
  `FORMAT_MESSAGE_FROM_SYSTEM`, `FORMAT_MESSAGE_IGNORE_INSERTS`,
  `MB_OK`, `MB_ICONERROR`, `MB_ICONINFORMATION`,
  `FILE_SHARE_READ`, `CREATE_ALWAYS`, `FILE_ATTRIBUTE_NORMAL`,
  `GENERIC_WRITE`, `INVALID_HANDLE_VALUE`) and the kernel32/user32
  function signatures (`LoadLibraryExA`, `LoadLibraryA`,
  `GetLastError`, `FormatMessageA`, `CreateFileA`, `WriteFileA`,
  `CloseHandle`, `GetModuleFileNameA`, `SetCurrentDirectoryA`,
  `GetLocalTime`, `GetTempPathA`, `GetConsoleWindow`, `MessageBoxA`)
  against documented MinGW-w64 typedefs.
- Format check: `src/app/crash_handler.cpp` (touched by v0.4.3 but
  never run through the formatter in that push) is now clang-format
  22.1.8-clean. v0.4.4's first CI run failed the blocking format job
  on exactly this file (4 continuation-line spots); this re-run
  formats it (5 insertions / 9 deletions, whitespace only - no
  behavior change) and the whole `src/`+`tests/` tree now passes
  `clang-format --dry-run --Werror`. The `src/app/loader_check.cpp`
  format pass from the first v0.4.4 push is unchanged.
- Real CMake pipeline (Linux, `FC_BUILD_APP=OFF`): core 88 + timeline
  64 = 152 tests pass via ctest. No regressions — v0.4.4 adds a
  Windows-only CMake target that the Linux leg does not build.
- Mock is a syntax-level check only — it does not validate the link
  step or the runtime behavior of `LoadLibraryExA`. The CI MinGW
  portable leg (`windows-portable` job) is the authoritative oracle
  for the Windows build, same as v0.4.2/v0.4.3.
- The tool's own portability belt-and-braces: `-static` link means the
  resulting `fcp-loader-check.exe` will run on any Windows x64 machine
  regardless of whether the MinGW runtime DLLs are present. If the
  portable zip's own `FusionCutPro.exe` cannot start because, say,
  `libstdc++-6.dll` is missing, `fcp-loader-check.exe` (which has no
  such dependency) still runs and names `libstdc++-6.dll` as the FAIL.

## [0.4.3] - 2026-08-30

Hotfix: portable-build runtime crash class. v0.4.2's MinGW
compile-time fixes landed green, but the resulting portable .exe
still crashed immediately on double-click with the Windows loader
error dialog "The application was unable to start correctly
(0xc0000005)" and **no crash log was produced**. This entry
closes that gap.

### Root cause
- The v0.4.1 crash handler installed the Windows VEH from inside
  `main()`. The Windows loader dialog phrasing "unable to start
  correctly (0xc0000005)" is specifically the loader-phase crash
  dialog shown by Windows Error Reporting during the
  import-resolution / DllMain / CRT-init rendezvous, BEFORE `main()`
  is reached. With VEH installed only from `main()`, any crash in
  the loader phase (e.g. a Qt5Core.dll / qwindows.dll DllMain
  fault, a missing API-set DLL on the target machine, or a static
  initializer in a transitive dependency) ran with **no in-process
  reporter registered** - hence no log.

### Fixed (pre-main crash handler installation)
- `src/app/crash_handler.cpp` now installs the VEH (Windows) and
  signal handlers (POSIX) from a file-scope static-storage
  initializer (`EarlyCrashHandlerInstaller` at the bottom of the
  file). Its constructor runs at .CRT$XCU (Windows) / .ctors
  (POSIX) static-init time, which is AFTER the basic
  CRT/kernel32/libstdc++ DLLs are mapped but BEFORE QApplication
  pulls in Qt5Core / Qt5Gui / Qt5Widgets / qwindows.dll. This
  covers the entire class of 0xc0000005 startup crashes that the
  v0.4.1 in-main() install point could not catch.
- `installCrashHandler()` is now split internally: the static
  initializer calls it with empty strings (installing handlers
  with version="unknown" and exe dir as report dir); `main()` then
  calls it again with the real `FC_VERSION_STRING`. The idempotent
  `g_installed` guard keeps the handlers installed exactly once;
  the version + report dir fields are updated on EVERY call so
  any crash report carries the real version, not the static-init
  placeholder.

### Added (boot-trace log file)
- `src/app/crash_handler.cpp` now opens
  `FusionCutPro-boot-<timestamp>.log` next to the executable at
  static-init time, and writes a milestone line at every startup
  checkpoint: stage0=VEH + handlers installed, stage1=install-
  CrashHandler called from main (version + dir set),
  stage2..stage7 = entered main, about-to-construct-QApplication,
  QApplication constructed, MainWindow constructed, window.show,
  app.exec entered.
- Even if VEH itself cannot run (e.g. the crash is inside a
  static-import DLL's DllMain before any user code runs), the
  partial boot trace shows how far startup got. This is the only
  diagnostic that survives a true loader-phase crash.
- The boot trace is embedded into every crash report body under a
  new `----- Boot Trace -----` section, so the developer sees the
  startup state in a single file.
- On clean exit (`fc::shutdownCrashHandler()` called from
  `main()` before `return app.exec()` result), the boot trace is
  deleted so it doesn't accumulate across runs. On crash it is
  preserved (the crash handler never calls shutdown) AND embedded
  into the crash report.
- New public API in `crash_handler.h`: `recordBootStage(stage,
  message)`, `shutdownCrashHandler()`, `bootTraceContent()`.
  `installCrashHandler()` keeps its existing signature; callers
  should still call it from `main()` with the real version.

### Hardened (VEH write path)
- The VEH crash-log writer now tries four candidate locations
  instead of two: the configured report dir, the directory next to
  the executable, the current working directory, and (Windows)
  `%TEMP%` via `GetTempPathA`. If the exe lives in a read-only
  Program Files install the log still lands in `%TEMP%` instead of
  being silently dropped.
- New `resolveReportDir()` helper centralizes the dir-resolution
  chain; the VEH, `terminateHandler`, `newHandler`,
  `invalidParameterHandler`, and `pureCallHandler` now all route
  through it. Same chain feeds the boot-trace open.

### Verified locally (sandbox)
- POSIX-branch compile of `crash_handler.cpp` clean with
  `g++ -std=c++17 -Wall -Wextra -Werror -Wpedantic` (no
  regression from v0.4.2).
- Standalone POSIX probe (`/tmp/fc_compile_probe.cpp`) exercising
  the new API end-to-end: static initializer's `installCrashHandler("",
  "")` runs, opens boot trace at exe dir, writes stage0;
  probe's `installCrashHandler("0.4.3-sandbox-probe", ".")`
  runs without re-installing handlers (idempotent guard holds),
  updates version+dir, writes stage1; `recordBootStage(2..7)`
  lines all land in the trace file; `writeManualCrashReport()`
  writes a report with the boot trace embedded under
  `----- Boot Trace -----`; `shutdownCrashHandler()` closes and
  deletes the trace file. Confirmed by `ls` showing the boot trace
  is gone post-shutdown and the manual report contains all
  expected sections.
- Real CMake (`FC_BUILD_MEDIA=OFF FC_BUILD_APP=OFF
  FC_BUILD_TESTS=ON`): core 88 + timeline 64 = 152 tests pass.
- Mock cross-compile of the Windows branch (`g++ -D_WIN32 -I
  winmock ...`) is intentionally not re-established this push:
  the marginal code added is 4 standard MinGW-w64 API calls
  (`GetTempPathA` x2 in `resolveReportDir` + VEH-fallback,
  `DeleteFileA` x1 in `closeBootTrace`, plus `MAX_PATH` already
  used in v0.4.2). The previous agent's v0.4.2 mock verification
  confirmed the rest of the Windows branch parses cleanly; these
  four are core MinGW-w64 declarations in `<windows.h>`.
  The CI portable leg remains the authoritative MinGW oracle
  (as documented in the v0.4.2 entry).

## [0.4.2] - 2026-08-30

Hotfix: portable-build (MinGW gcc 16.1) compile failure on the new
crash handler. The 5 main CI jobs (format, core x2, media x2, Qt)
were green for v0.4.1, but the MinGW portable leg failed compiling
`crash_handler.cpp` at two issues:

### Fixed (MinGW-w64 gcc 16.1 portable build)
- `crash_handler.cpp` now uses `std::_Exit` (C++11 standard in
  `<cstdlib>`) at every Windows-branch exit, not `std::_exit`. The
  POSIX `_exit` (lowercase, in `<unistd.h>`) is NOT in `std::` - it
  is a global function, and `std::_exit` does not exist on MinGW-w64
  libstdc++. glibc's libstdc++ surfaces `_exit` as a non-standard
  extension inside `std::` so the Linux sandbox compile passed; the
  MinGW compile aborts at `std::_exit(128 + sig)` in
  `posixSignalHandler`. Same class of cross-toolchain gap as the
  FFmpeg 4.4 swr_convert / `<cstddef>` regressions recorded in
  deliveries #7 and #14: glibc/Ubuntu-24.04 libstdc++ is permissive
  in ways MinGW-w64 is not - CI is the only oracle for the MinGW path.
  The POSIX branch (Linux) keeps using `_exit` (lowercase) for true
  async-signal-safety per POSIX; only the Windows branch was wrong.
- `NOMINMAX` / `WIN32_LEAN_AND_MEAN` now `#ifndef`-guarded. libstdc++
  `<bits/os_defines.h>` (pulled in transitively via `<string>` from
  `crash_handler.h`) already defines `NOMINMAX 1` before our explicit
  `#define NOMINMAX` (no value); MinGW-w64 gcc 16 warned "NOMINMAX
  redefined". The guard preserves the existing definition instead of
  replacing it.

### Verified locally (sandbox)
- Standalone Linux compile (`g++ -std=c++17 -Wall -Wextra -Werror`)
  of `crash_handler.cpp` still passes - no POSIX-branch regression.
- Synthetic `_WIN32` mock-header cross-compile of the Windows branch
  (full mock of `<windows.h>`, `<tlhelp32.h>`, `<stdlib.h>` typedefs
  against documented MinGW-w64 signatures) compiles clean past the
  previous CI abort point at line 468. All remaining `MessageBoxA`,
  `CaptureStackBackTrace`, `CreateToolhelp32Snapshot` /
  `Module32First` / `Module32Next`, `_set_invalid_parameter_handler`,
  `_set_purecall_handler`, `AddVectoredExceptionHandler` calls match
  the documented MinGW-w64 typedefs. The CI portable leg is the
  authoritative MinGW oracle; the mock cross-compile is a
  syntax-level catch only.
- clang-format 22.1.8 clean across all `src/` + `tests/`.
- core 88 + timeline 64 = 152 tests pass via real CMake
  (`FC_BUILD_MEDIA=OFF FC_BUILD_APP=OFF FC_BUILD_TESTS=ON`).

## [0.4.1] - 2026-08-24

Hotfix: two CI compile regressions introduced by the v0.4.0 editing-core
delta, plus a real feature - in-process crash diagnostics that replaces
the `run-console.bat` console launcher with structured crash-log output.

### Added
- **In-process crash handler** (`src/app/crash_handler.{h,cpp}`): on any
  unhandled Windows VEH exception (access violation, in-page error,
  stack overflow, illegal instruction, heap corruption, ...), C++
  uncaught exception (`std::set_terminate`), CRT invalid-parameter call,
  pure-virtual call, OOM (`std::set_new_handler`), or POSIX signal
  (SIGSEGV/SIGABRT/SIGFPE/SIGILL/SIGTERM), writes
  `crash-logs/FusionCutPro-crash-<timestamp>.log` next to the executable
  with version, timestamp, exception kind/code/address, a best-effort
  stack backtrace (raw addresses for offline symbolication), and on
  Windows a Toolhelp32 snapshot of every loaded module with its base
  address, size, and on-disk path. The handler then re-raises the
  original exception so the OS default disposition still runs - it is a
  reporter, not an exception swallower. On Windows a modal MessageBox
  names the log file path so a double-click launch sees something
  actionable instead of the bare `0xc0000005` dialog.
- `--crash-test` CLI hook in `main.cpp` writes a synthetic report and
  exits without starting the UI, for end-to-end verification on a clean
  Windows machine.
- Pre-created `crash-logs/` directory in the portable zip (the handler
  never creates directories itself - it runs in exception context and
  must not touch filesystem structure beyond opening a single file).
- `MessageBoxA` link dependency on Windows via `target_link_libraries(
  FusionCutPro PRIVATE user32)` (guarded by `if(WIN32)` so the Linux Qt
  compile path is unaffected).

### Fixed (CI regressions introduced by v0.4.0)
- `src/core/timeline_model.cpp` now `#include <cstddef>` for `ptrdiff_t`
  used as `std::vector::insert` iterator offset. Ubuntu 24.04's libstdc++
  pulled this in transitively through `<algorithm>`/`<cmath>`, but
  Ubuntu 22.04 (jammy container, FFmpeg 4.4 CI leg) did not, breaking
  the media-tests-ffmpeg44 compile at line 104.
- `fc::TimelineModel::trackAt(int)` gains a `const` overload
  (`const Track* trackAt(int) const;`). `TimelinePanel` renders against
  a `const fc::TimelineModel*` (a non-mutating view); the previously
  lone overload `Track* trackAt(int)` was non-const, so the Qt 5.15 CI
  leg rejected it as a `const` qualifier discard at
  `drawHeaderColumn`/`drawClips`. The non-const overload now delegates
  to the const one via `const_cast` (Scott-Meyers avoid-duplication),
  so the path is provably identical.

### Changed (packaging)
- `dist/run-console.bat` removed - the in-process crash handler writes
  a structured log AND shows a MessageBox naming the log path, which is
  strictly more diagnostic than the previous console wrapper. The
  `PORTABLE.txt` install note now documents the `crash-logs/` directory
  and the `--crash-test` smoke hook.

## [0.4.0] - 2026-08-23

Milestone 4, phase 1: the editing core. The timeline is no longer a static
placeholder - it owns a real, frame-accurate model with clips, split, trim,
move, and delete; the UI renders and edits against it.

### Added
- `fc::TimelineModel` (pure C++, in fc_core): tracks, clips with source
  in/out points, timeline position, playback rate; operations addClip /
  removeClip / splitAt / moveClip / trimClipStart / trimClipEnd / clipAt /
  durationFrames. 64-check unit suite (`fc_timeline_tests`) covering
  tracks, add validation, split semantics, move + trim invariants, clipAt
  gaps, and duration aggregation.
- TimelinePanel now renders real clips from the model as colored blocks
  with labels and selection highlight; click-to-select, scrub, Ctrl+wheel
  zoom; razor-mode split signal (mouse-based razor UI lands in M4b).
- Editing actions wired through MainWindow: double-clicking a project
  item places a 5-second clip on V1 at the playhead; **Clip → Split at
  Playhead (C)** splits the selected clip (or the V1 clip under the
  playhead); **Delete / Backspace** removes the selected clip; sequence
  duration tracks the model after every edit.
- Track header L/M/S cells now reflect model state (read-only this phase;
  toggling arrives in M4b).

### Fixed (packaging, post-v0.3.0 portable)
- Portable build no longer needs `windeployqt` or `qmake`: plugins are
  located via `find -name qwindows.dll`, mirrored into both `dist/<cat>/`
  and the canonical `dist/plugins/<cat>/` layout, with core Qt DLLs also
  copied next to the platform plugin (defensive against plugin-dep DLL
  search quirks); `dist/qt.conf` pins the install root so Qt does not
  chase the MSYS2 absolute path baked into the runtime.
- `dist/run-console.bat` launches the app in a console so a startup crash
  prints Qt's diagnostic (missing DLL / plugin) instead of the bare
  0xc0000005 dialog.

## [0.3.0] - 2026-08-23

Milestone 3: the dual-mode UI shell. The app now shows **real decoded
frames** - import media, scrub, play, and generate 360p proxies, all
through the M2 engine on a background thread.

### Added
- **Pro Mode workspace** (Module 2.1): dockable panels with saveable
  layout (QSettings) - Project panel (import/remove/metadata/context menu,
  thumbnails captured from the first decoded frame), tabbed Effects
  browser (Module 7 seed catalog, drag-enabled for M4), bottom Timeline |
  Audio Mixer tabs, right Effect Controls parameter tree (Motion +
  Lumetri seeds), and a Source | Program monitor splitter.
- **Quick Mode page** (Module 2.2): CapCut-style simplified layout with
  large preview, prominent Import, aspect selector (16:9 / 9:16 / 1:1 /
  4:3), and the main + quick-actions toolbars (placeholders wired to M4-M7).
  One menu toggle switches modes; both share the same playback engine.
- **Playback engine**: `DecodeWorker` on a background QThread owning one
  `VideoDecoder` (sequential reads while playing, keyframe seek when
  scrubbing), `PreviewCanvas` custom-painted monitor with letterboxing
  and adaptive aspect, `TransportBar` with `fc::Timecode` readout, frame
  stepping, scrub slider, and Space/Left/Right shortcuts; timeline
  playhead is click/drag-scrubbable with Ctrl+wheel zoom.
- **Proxy workflow in-app**: right-click a clip (or Clip menu) generates
  a 360p proxy with live progress in the status bar; playback
  automatically prefers the proxy once generated.
- **Timeline shell**: fully custom-painted - `fc::Timecode` ruler, three
  placeholder tracks (V2/V1/A1) with functional lock/mute/solo header
  cells and colorblind-friendly accents (Module 10.4).
- Portable packaging: iterative `ldd` DLL sweep replaces the manual
  three-DLL copy, so the zip now includes Qt + FFmpeg + MinGW runtime
  dependency chains automatically.
- High-DPI attributes enabled in `main.cpp` (Module 10.4 UI scaling).

### Changed
- App now links `fc_core` + `fc_media`; building the app requires
  `FC_BUILD_MEDIA=ON` (guarded with a clear CMake error).
- About dialog reports the linked FFmpeg runtime version.

## [0.2.0] - 2026-08-22

Milestone 2: the FFmpeg-backed media I/O layer. The engine can now inspect,
decode, and proxy real video files; the portable/Windows legs of CI build and
run it on MinGW too.

### Added
- `fc_media` library (FFmpeg via pkg-config, dual API paths for FFmpeg
  4.4 and 5.1+/7.x - `AVChannelLayout` guarded on `LIBAVUTIL_VERSION_INT`):
  - `MediaProbe` - file inspection into a dependency-free `MediaInfo` model
    (video/audio streams, codec names, pixel/sample formats, duration,
    frame rate preferring `r_frame_rate` so CFR rates stay exact).
  - `VideoDecoder` - sequential and seeking decode pump converting every
    frame to packed RGBA; strips mp4 `AV_PKT_FLAG_DISCARD` so trailing
    frames are never silently dropped; seek resumes at the first frame with
    pts >= target (half-frame tolerance).
  - `ProxyGenerator` - 360p edit-proxy transcode (H.264/x264 with CRF +
    preset, MPEG-4 Part 2 fallback when no H.264 encoder exists; audio
    re-encoded to 48 kHz stereo AAC via swresample + audio FIFO); never
    upscales, keeps aspect with even dimensions; monotonic progress
    callback with cancellation; removes partial output on failure.
  - RAII ownership wrappers for every FFmpeg object used plus a C++-safe
    `fcError()` (no compound literals - MSVC friendly).
- Test layer: shared header-only harness, runtime synthetic media generator
  (no binary test assets in the repo), and a 317-check integration suite
  covering probe, sequential decode (color-order assertions), seek,
  proxy geometry/audio/cancellation, and no-upscale behavior.
- CI: new `media-tests` job (Ubuntu, FFmpeg 7.x) and
  `media-tests-ffmpeg44` job (Ubuntu 22.04 container, FFmpeg 4.4 - the
  legacy API generation the Windows 7 build targets); Qt job now builds
  with the media layer enabled; portable workflow installs
  `mingw-w64-x86_64-ffmpeg` + `pkgconf` and runs media tests on MinGW.

## [0.1.0] - 2026-08-22

First public baseline: engineering foundation only (no editing features yet).

### Added
- CMake build system (C++17) driven by a single `VERSION` file, with
  `FC_BUILD_APP` / `FC_BUILD_TESTS` options.
- Core engine primitives (dependency-free, 88 unit checks in CI):
  - `fc::FrameRate` + `fc::Timecode` - rational frame rates (24/25/30/50/60 and
    NTSC 23.976/29.97/59.94), non-drop timecode parse/format/arithmetic.
  - `fc::LruCache` - LRU cache with hit/miss statistics (frame-cache policy core).
  - `fc::MemoryPool` - fixed-capacity, 64-byte-aligned block pool with ownership
    validation and double-release detection (deterministic frame allocation).
- Qt 5.15 desktop shell (`FusionCutPro.exe`): Module 2 menu structure, FusionCut
  dark theme (charcoal #1E1E1E / panels #252525 / accent #00A8FF), and the
  Pro Mode / Quick Mode workspace toggle placeholder.
- GitHub Actions CI: blocking format check (pinned clang-format), core unit
  tests on Ubuntu and Windows (MSVC), Qt app build on Ubuntu (Qt 5.15).
- GitHub Actions `Portable Build` workflow: on-demand Windows x64 portable zip
  (MSYS2 MinGW64 + windeployqt, tests run before packaging); tag pushes (`v*`)
  attach the zip to a GitHub Release.
- Project documentation: README, changelog, repository settings
  (description + topics), GPL-3.0 license.

### Fixed
- Qt build: replaced `QWidget::addAction(text, shortcut, receiver, functor)`
  calls (a Qt 6.3+ overload) with explicit action creation that compiles
  against Qt 5.15 - fixes the Ubuntu Qt CI job; shortcuts now live on the
  actions themselves. Bonus: Clip/Sequence/Effects menus gained their
  Module 6 default shortcuts (C, Ctrl+R, Ctrl+L, Ctrl+G, Ctrl+Shift+G,
  Ctrl+D, Enter, +/-).
- Format check: `.clang-format` now pins Qt-convention right pointer
  alignment (`PointerAlignment: Right`) matching the codebase, all sources
  were formatted with clang-format 22.1.8, and CI installs that exact
  pinned version - the check is now blocking and reproducible.

[0.4.8]: https://github.com/marbou92/FusionCut-Pro/releases/tag/v0.4.8
[0.4.6]: https://github.com/marbou92/FusionCut-Pro/releases/tag/v0.4.6
[0.4.5]: https://github.com/marbou92/FusionCut-Pro/releases/tag/v0.4.5
[0.4.4]: https://github.com/marbou92/FusionCut-Pro/releases/tag/v0.4.4
[0.4.3]: https://github.com/marbou92/FusionCut-Pro/releases/tag/v0.4.3
[0.4.2]: https://github.com/marbou92/FusionCut-Pro/releases/tag/v0.4.2
[0.4.1]: https://github.com/marbou92/FusionCut-Pro/releases/tag/v0.4.1
[0.4.0]: https://github.com/marbou92/FusionCut-Pro/releases/tag/v0.4.0
[0.3.0]: https://github.com/marbou92/FusionCut-Pro/releases/tag/v0.3.0
[0.2.0]: https://github.com/marbou92/FusionCut-Pro/releases/tag/v0.2.0
[0.1.0]: https://github.com/marbou92/FusionCut-Pro/releases/tag/v0.1.0
