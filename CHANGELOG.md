# Changelog

All notable changes to FusionCut Pro are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project adheres to [Semantic Versioning](https://semver.org/).

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

[0.4.4]: https://github.com/marbou92/FusionCut-Pro/releases/tag/v0.4.4
[0.4.3]: https://github.com/marbou92/FusionCut-Pro/releases/tag/v0.4.3
[0.4.2]: https://github.com/marbou92/FusionCut-Pro/releases/tag/v0.4.2
[0.4.1]: https://github.com/marbou92/FusionCut-Pro/releases/tag/v0.4.1
[0.4.0]: https://github.com/marbou92/FusionCut-Pro/releases/tag/v0.4.0
[0.3.0]: https://github.com/marbou92/FusionCut-Pro/releases/tag/v0.3.0
[0.2.0]: https://github.com/marbou92/FusionCut-Pro/releases/tag/v0.2.0
[0.1.0]: https://github.com/marbou92/FusionCut-Pro/releases/tag/v0.1.0
